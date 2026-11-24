/* rr_il.c - Export lookup, managed allocation and managed invocation.
 *
 * Transport design, in order:
 *
 *  1. Resolve il2cpp_domain_get / il2cpp_thread_attach / il2cpp_thread_detach /
 *     il2cpp_runtime_invoke (or il2cpp_object_new) out of the target module's
 *     export table. They are real exports: the image publishes 241 il2cpp_*.
 *  2. Allocate one RWX block in the target: generated stub at +0, argument
 *     pointer array at +0x200, exception slot +0x300, result +0x308, attached
 *     thread +0x310, argument values +0x320.
 *  3. Execute it. On an unprotected target that is CreateRemoteThread. When the
 *     title filters remote allocation and thread creation (kernel anti-cheat),
 *     the block comes from the read/write driver and the execution step comes
 *     from the APC channel driver (rr_exec.h): a user-mode APC queued from
 *     kernel mode needs no process handle, so there is nothing to filter.
 *  4. Read the result and exception slots, free the block.
 *
 * Both paths use the same stub and the same slots. The stub claims the block
 * with an atomic exchange before it touches the runtime, so the same block can
 * be delivered to several threads and still run the call exactly once.
 *
 * The block is freed on every Win32 path, the wait is bounded, and a call that
 * does not finish inside the budget is reported rather than waited on.
 */
#include "rr_il.h"
#include "rr_mem.h"
#include "rr_exec.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#define IL_BLOCK      0x400
#define IL_OFF_PARAMS 0x200
#define IL_OFF_EXC    0x300
#define IL_OFF_RET    0x308
#define IL_OFF_THR    0x310
#define IL_OFF_CLAIM  0x318
#define IL_OFF_ARG    0x320
#define IL_OFF_DONE   0x340
#define IL_WAIT_MS    5000u

/* Nonzero claim and completion markers. Any value works; these spell out what
 * the slot means when someone dumps the block. */
#define IL_CLAIM_MAGIC 0x314D49414C43314Cull /* 'LC1LAIM1' */
#define IL_DONE_MAGIC  0x31454E4F44494C49ull /* 'ILIDONE1' */

/* ---- PE export table ----------------------------------------------------- */

static int pe_export_dir(uint64_t base, uint32_t* rva_out, uint32_t* size_out) {
    uint8_t dos[0x40], nt[0x108];
    int32_t e_lfanew = 0;
    uint16_t magic = 0;
    uint32_t er = 0, es = 0;

    if (!base) return 0;
    if (!rr_read(base, dos, sizeof dos)) return 0;
    memcpy(&e_lfanew, dos + 0x3C, 4);
    if (e_lfanew <= 0 || e_lfanew > 0x1000) return 0;
    if (!rr_read(base + (uint64_t)e_lfanew, nt, sizeof nt)) return 0;
    if (memcmp(nt, "PE\0\0", 4) != 0) return 0;
    memcpy(&magic, nt + 0x18, 2);
    /* Data directories start at the optional header (PE sig + 4 + 20), so the
     * first entry sits at +0x88 on PE32+ and +0x78 on PE32. */
    if (magic == 0x20B) {           /* PE32+ */
        memcpy(&er, nt + 0x88, 4);
        memcpy(&es, nt + 0x8C, 4);
    } else if (magic == 0x10B) {    /* PE32 */
        memcpy(&er, nt + 0x78, 4);
        memcpy(&es, nt + 0x7C, 4);
    } else {
        return 0;
    }
    if (!er || es < 0x28) return 0;
    *rva_out = er;
    *size_out = es;
    return 1;
}

typedef struct {
    uint32_t nfun, nnam, aof, aon, aoo;
} IL_EXPDIR;

/* Failure report for the header path: the raw values the driver handed back,
 * which is the difference between "read refused" and "read wrong". */
static void pe_diag(uint64_t base, FILE* out) {
    uint8_t dos[0x30] = {0}, nt[0x20] = {0};
    uint32_t lf = 0;
    uint16_t magic = 0;
    int got_dos, got_nt = 0;

    if (!out) return;
    got_dos = rr_read(base, dos, sizeof dos);
    fprintf(out, "   diag base=0x%llX dos_read=%d mz=%02X%02X\n",
            (unsigned long long)base, got_dos, dos[0], dos[1]);
    memcpy(&lf, dos + 0x3C, 4);
    fprintf(out, "   diag e_lfanew=0x%X\n", lf);
    if (lf > 0 && lf < 0x1000) {
        got_nt = rr_read(base + lf, nt, sizeof nt);
        memcpy(&magic, nt + 0x18, 2);
        fprintf(out, "   diag nt_read=%d sig=%02X%02X magic=0x%X\n",
                got_nt, nt[0], nt[1], magic);
    }
}

static int il_export_dir(uint64_t base, IL_EXPDIR* d) {
    uint32_t rva = 0, size = 0;
    uint8_t hdr[0x28];
    if (!pe_export_dir(base, &rva, &size)) return 0;
    if (!rr_read(base + rva, hdr, sizeof hdr)) return 0;
    memcpy(&d->nfun, hdr + 0x14, 4);
    memcpy(&d->nnam, hdr + 0x18, 4);
    memcpy(&d->aof,  hdr + 0x1C, 4);
    memcpy(&d->aon,  hdr + 0x20, 4);
    memcpy(&d->aoo,  hdr + 0x24, 4);
    return d->nnam > 0 && d->nnam <= 0x10000 && d->aof && d->aon && d->aoo;
}

static int il_cstr(uint64_t addr, char* out, int cap) {
    int i = 0;
    if (!out || cap <= 1) return 0;
    for (; i < cap - 1; ++i) {
        char ch = 0;
        if (!rr_read(addr + (uint64_t)i, &ch, 1)) break;
        out[i] = ch;
        if (!ch) return i;
    }
    out[i] = 0;
    return i;
}

static uint64_t il_module(void) {
    uint64_t base = 0;
    uint32_t size = 0;
    if (!rr_module("GameAssembly.dll", &base, &size)) return 0;
    return base;
}

int rr_il_export(const char* name, uint64_t* addr_out) {
    uint64_t base = il_module();
    IL_EXPDIR d;
    uint8_t row[4];
    char nm[160];

    if (!base || !name || !addr_out) return 0;
    if (!il_export_dir(base, &d)) return 0;

    for (uint32_t i = 0; i < d.nnam; ++i) {
        if (!rr_read(base + d.aon + (uint64_t)i * 4, row, sizeof row)) return 0;
        uint32_t nr = 0;
        memcpy(&nr, row, 4);
        if (!nr) continue;
        if (il_cstr(base + nr, nm, sizeof nm) <= 0) continue;
        if (strcmp(nm, name) != 0) continue;

        uint16_t ord = 0;
        if (!rr_read(base + d.aoo + (uint64_t)i * 2, &ord, 2)) return 0;
        uint32_t frva = 0;
        if (!rr_read(base + d.aof + (uint64_t)ord * 4, &frva, 4)) return 0;
        if (!frva) return 0;
        *addr_out = base + frva;
        return 1;
    }
    return 0;
}

/* Case-insensitive substring, local to this file. */
static int il_has_ci(const char* hay, const char* needle) {
    size_t n;
    if (!hay || !needle) return 0;
    n = strlen(needle);
    if (!n) return 1;
    for (; *hay; ++hay)
        if (_strnicmp(hay, needle, n) == 0) return 1;
    return 0;
}

int rr_il_exports(const char* filter, FILE* out) {
    uint64_t base = il_module();
    IL_EXPDIR d;
    uint8_t row[4];
    char nm[160];
    int shown = 0;

    if (!out) return 0;
    if (!base) { fprintf(out, "EXPORTS: GameAssembly.dll not loaded\n"); return 0; }
    if (!il_export_dir(base, &d)) {
        fprintf(out, "EXPORTS: export directory unreadable\n");
        pe_diag(base, out);
        return 0;
    }
    fprintf(out, "EXPORTS module=0x%llX functions=%u names=%u\n",
            (unsigned long long)base, d.nfun, d.nnam);

    for (uint32_t i = 0; i < d.nnam; ++i) {
        uint32_t nr = 0;
        uint16_t ord = 0;
        uint32_t frva = 0;
        if (!rr_read(base + d.aon + (uint64_t)i * 4, row, 4)) break;
        memcpy(&nr, row, 4);
        if (!nr || il_cstr(base + nr, nm, sizeof nm) <= 0) continue;
        if (filter && *filter && !il_has_ci(nm, filter)) continue;
        if (!rr_read(base + d.aoo + (uint64_t)i * 2, &ord, 2)) break;
        if (!rr_read(base + d.aof + (uint64_t)ord * 4, &frva, 4)) break;
        fprintf(out, "   GameAssembly+0x%06X  %s\n", frva, nm);
        ++shown;
    }
    fprintf(out, "   (%d shown)\n", shown);
    return shown;
}

int rr_il_probe(FILE* out) {
    static const char* const need[] = {
        "il2cpp_domain_get", "il2cpp_thread_attach", "il2cpp_thread_detach",
        "il2cpp_object_new", "il2cpp_runtime_invoke", "il2cpp_class_from_name",
        "il2cpp_domain_get_assemblies", "il2cpp_assembly_get_image",
        "il2cpp_array_new", "il2cpp_string_new"
    };
    int have = 0, needn = (int)(sizeof need / sizeof need[0]);
    if (!out) return 0;
    fprintf(out, "PROBE il2cpp transport module=0x%llX\n", (unsigned long long)il_module());
    for (int i = 0; i < needn; ++i) {
        uint64_t a = 0;
        int ok = rr_il_export(need[i], &a);
        fprintf(out, "   %-32s %s%s0x%llX\n", need[i], ok ? "yes" : "no ",
                ok ? "  " : "  ", (unsigned long long)a);
        have += ok;
    }
    fprintf(out, "   (%d/%d)\n", have, needn);
    return have == needn;
}

/* ---- remote stub --------------------------------------------------------- */

static HANDLE il_proc(void) {
    static HANDLE cached = NULL;
    if (cached) return cached;
    if (!g_rr_pid) return NULL;
    cached = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                         FALSE, g_rr_pid);
    return cached;
}

typedef struct { uint8_t* p; size_t n; } IL_EMIT;

static void em_bytes(IL_EMIT* e, const void* src, size_t n) {
    memcpy(e->p + e->n, src, n);
    e->n += n;
}
static void em_imm(IL_EMIT* e, uint8_t a, uint8_t b, uint64_t v) {
    uint8_t op[2] = { a, b };
    em_bytes(e, op, 2);
    em_bytes(e, &v, 8);
}
static void em_store_rax(IL_EMIT* e, uint64_t slot) {
    uint8_t op[2] = { 0x48, 0xA3 };
    em_bytes(e, op, 2);
    em_bytes(e, &slot, 8);
}
static void em_load_rax(IL_EMIT* e, uint64_t slot) {
    uint8_t op[2] = { 0x48, 0xA1 };
    em_bytes(e, op, 2);
    em_bytes(e, &slot, 8);
}
static void em_mov_rcx_from_rax(IL_EMIT* e) {
    uint8_t op[3] = { 0x48, 0x89, 0xC1 };
    em_bytes(e, op, 3);
}
/* sub rsp,0x28 / call rax / add rsp,0x28 - the shadow space plus the
 * alignment the Win64 calling convention expects at every call site. */
static void em_call_rax(IL_EMIT* e) {
    uint8_t op[10] = { 0x48, 0x83, 0xEC, 0x28, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28 };
    em_bytes(e, op, sizeof op);
}

/* op 0 = allocate (klass in target), op 1 = invoke.
 *
 * `base` is the runtime address the stub will live at, needed for the two
 * RIP-relative displacements (the claim slot) and the skip jump.
 *
 *   mov  rax, IL_CLAIM_MAGIC      ; one owner per block, even with N APCs
 *   lock xchg [claim], rax
 *   test rax, rax
 *   jnz  .ret
 *   ... domain_get / attach / call / store ret / detach ...
 *   mov  rax, IL_DONE_MAGIC
 *   mov  [done], rax
 * .ret:
 *   ret
 */
static int il_build_stub(uint8_t* buf, uint64_t base, uint64_t api_domain_get,
                         uint64_t api_thread_attach, uint64_t api_thread_detach, int op,
                         uint64_t api_op, uint64_t target, uint64_t obj, uint64_t params,
                         uint64_t exc, uint64_t ret_slot, uint64_t thr_slot,
                         uint64_t claim_slot, uint64_t done_slot) {
    IL_EMIT e = { buf, 0 };
    size_t disp_at, jmp_at, ret_at;

    em_imm(&e, 0x48, 0xB8, IL_CLAIM_MAGIC);       /* mov rax, claim magic */
    { uint8_t op3[3] = { 0xF0, 0x48, 0x87 }; em_bytes(&e, op3, 3); }
    { uint8_t modrm = 0x05; em_bytes(&e, &modrm, 1); }  /* lock xchg [rip+d], rax */
    disp_at = e.n;
    { int32_t d0 = 0; em_bytes(&e, &d0, 4); }
    { uint8_t t[3] = { 0x48, 0x85, 0xC0 }; em_bytes(&e, t, 3); }  /* test rax, rax */
    jmp_at = e.n;
    { uint8_t j[2] = { 0x0F, 0x85 }; em_bytes(&e, j, 2); }        /* jnz .ret */
    { int32_t z = 0; em_bytes(&e, &z, 4); }

    if (op != 3) {
        em_imm(&e, 0x48, 0xB8, api_domain_get);   /* mov rax, domain_get */
        em_call_rax(&e);
        em_mov_rcx_from_rax(&e);                  /* rcx = domain       */
        em_imm(&e, 0x48, 0xB8, api_thread_attach);
        em_call_rax(&e);                          /* rax = thread       */
        em_store_rax(&e, thr_slot);

        if (op == 0) {
            em_imm(&e, 0x48, 0xB9, target);       /* rcx = klass        */
            em_imm(&e, 0x48, 0xB8, api_op);
            em_call_rax(&e);
        } else if (op == 1) {
            em_imm(&e, 0x48, 0xB9, target);       /* rcx = method       */
            em_imm(&e, 0x48, 0xBA, obj);          /* rdx = this         */
            em_imm(&e, 0x49, 0xB8, params);       /* r8  = void** args  */
            em_imm(&e, 0x49, 0xB9, exc);          /* r9  = exc out      */
            em_imm(&e, 0x48, 0xB8, api_op);
            em_call_rax(&e);
        } else {                                  /* op 2: no-argument call */
            em_imm(&e, 0x48, 0xB8, api_op);
            em_call_rax(&e);
        }
        em_store_rax(&e, ret_slot);

        em_load_rax(&e, thr_slot);
        em_mov_rcx_from_rax(&e);
        em_imm(&e, 0x48, 0xB8, api_thread_detach);
        em_call_rax(&e);
    }

    em_imm(&e, 0x48, 0xB8, IL_DONE_MAGIC);    /* completion marker */
    em_store_rax(&e, done_slot);

    ret_at = e.n;
    { uint8_t ret = 0xC3; em_bytes(&e, &ret, 1); }

    /* Both displacements are relative to the instruction that follows them, and
     * the stub runs at `base`, not in this stack buffer. */
    { int32_t d = (int32_t)((int64_t)claim_slot - (int64_t)(base + (uint64_t)(disp_at + 4)));
      memcpy(buf + disp_at, &d, 4); }
    { int32_t r = (int32_t)((int64_t)ret_at - (int64_t)(jmp_at + 6));
      memcpy(buf + jmp_at + 2, &r, 4); }
    return (int)e.n;
}

/* Wait for the stub's completion marker. Reads go through the same driver that
 * owns the block, so no target handle is involved. */
static int il_wait_done(uint64_t blk, uint64_t* ret_out, uint64_t* exc_out) {
    DWORD t0 = GetTickCount();
    for (;;) {
        uint64_t done = 0;
        if (rr_read(blk + IL_OFF_DONE, &done, 8) && done == IL_DONE_MAGIC) {
            if (!rr_read(blk + IL_OFF_RET, ret_out, 8)) return 0;
            if (!rr_read(blk + IL_OFF_EXC, exc_out, 8)) return 0;
            return 1;
        }
        if (GetTickCount() - t0 >= IL_WAIT_MS) return 0;
        Sleep(2);
    }
}

/* Driver path: queue the stub as a user-mode APC. The claim guard makes a
 * multi-thread delivery safe, so the first thread whose APC is accepted is
 * enough; each further one is only there in case the previous thread is parked
 * in a wait it will not leave soon. */
static int il_run_apc(uint64_t blk, uint64_t* ret_out, uint64_t* exc_out, FILE* out) {
    uint32_t tids[128];
    int n = rr_thread_ids(g_rr_pid, tids, (int)(sizeof tids / sizeof tids[0]));
    int queued = 0;

    if (!n) {
        if (out) fprintf(out, "IL: no thread ids for pid %lu\n", (unsigned long)g_rr_pid);
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        long st = 0;
        unsigned stage = 0;
        if (!rr_exec_apc(g_rr_pid, tids[i], blk, 0, &st, &stage)) {
            if (out) fprintf(out, "IL: APC channel driver is not answering "
                                  "(map 3_src_driver/rr_exec.sys)\n");
            return 0;
        }
        if (st < 0) {
            if (out) fprintf(out, "IL: APC queue tid=%lu status=0x%08lX stage=%u\n",
                             (unsigned long)tids[i], (unsigned long)st, stage);
            continue;
        }
        ++queued;
    }
    if (!queued) {
        if (out) fprintf(out, "IL: no thread accepted the APC\n");
        return 0;
    }
    if (!il_wait_done(blk, ret_out, exc_out)) {
        if (out) fprintf(out, "IL: APC did not complete in %ums - block 0x%llX left "
                              "allocated\n", IL_WAIT_MS, (unsigned long long)blk);
        return 0;
    }
    return 1;
}

/* User-mode path: queue the same stub as a plain user APC. This needs only
 * THREAD_SET_CONTEXT on each thread handle, which is a different syscall from the
 * ones a protected target usually filters, so it is tried when no channel driver
 * is mapped. The APC fires the next time the thread returns to user mode from an
 * alertable wait; that is why every thread gets one. */
static int il_run_umapc(uint64_t blk, uint64_t* ret_out, uint64_t* exc_out, FILE* out) {
    uint32_t tids[64];
    int n = rr_thread_ids(g_rr_pid, tids, (int)(sizeof tids / sizeof tids[0]));
    int queued = 0, denied = 0;
    DWORD first_err = 0;

    if (!n) {
        if (out) fprintf(out, "IL: no thread ids for pid %lu\n", (unsigned long)g_rr_pid);
        return 0;
    }
    for (int i = 0; i < n; ++i) {
        HANDLE th = OpenThread(THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
        if (!th) {
            if (!denied++) first_err = GetLastError();
            continue;
        }
        if (!QueueUserAPC((PAPCFUNC)blk, th, 0)) {
            DWORD err = GetLastError();
            if (!denied++) first_err = err;
        } else {
            ++queued;
        }
        CloseHandle(th);
    }
    if (out)
        fprintf(out, "IL: user APC queued on %d of %d threads (%d refused, first error %lu)\n",
                queued, n, denied, (unsigned long)first_err);
    if (!queued) return 0;
    if (!il_wait_done(blk, ret_out, exc_out)) {
        if (out) fprintf(out, "IL: user APC did not complete in %ums - block 0x%llX left "
                              "allocated\n", IL_WAIT_MS, (unsigned long long)blk);
        return 0;
    }
    return 1;
}

/* One block, one execution, bounded wait, block freed on every Win32 exit. */
static int il_run(int op, uint64_t api_op, uint64_t target, uint64_t obj,
                  const uint64_t* args, int nargs, uint64_t* ret_out, uint64_t* exc_out,
                  FILE* out) {
    uint64_t api_dg = 0, api_ta = 0, api_td = 0;
    uint8_t code[0x200];
    uint64_t blk = 0;
    HANDLE proc = il_proc(), th = NULL;
    DWORD waited = 0, exit_code = 0, werr = 0;
    uint64_t ret = 0, exc = 0;
    int len, ok = 0, leaked = 0, via_driver = 0;

    if (!rr_il_export("il2cpp_domain_get", &api_dg) ||
        !rr_il_export("il2cpp_thread_attach", &api_ta) ||
        !rr_il_export("il2cpp_thread_detach", &api_td)) {
        if (out) fprintf(out, "IL: il2cpp runtime exports missing\n");
        return 0;
    }

    if (proc) {
        blk = (uint64_t)VirtualAllocEx(proc, NULL, IL_BLOCK, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
        if (!blk) werr = GetLastError();
    }
    if (!blk) {
        /* No Win32 allocation possible: take the page from the driver that
         * already has read/write access to the target, and execute through the
         * APC channel. The page belongs to the target's address space and is
         * released when the target exits. */
        if (!rr_drv_alloc_remote(g_rr_pid, IL_BLOCK, 0x40 /* PAGE_EXECUTE_READWRITE */,
                                 &blk) || !blk) {
            if (out) {
                fprintf(out, "IL: no executable block in the target ");
                if (!proc) fprintf(out, "(OpenProcess failed 0x%08lX) ",
                                   (unsigned long)GetLastError());
                else fprintf(out, "(VirtualAllocEx failed %lu) ", (unsigned long)werr);
                fprintf(out, "and the driver could not provide one\n");
            }
            return 0;
        }
        via_driver = 1;
    }

    len = il_build_stub(code, blk, api_dg, api_ta, api_td, op, api_op, target, obj,
                        blk + IL_OFF_PARAMS, blk + IL_OFF_EXC, blk + IL_OFF_RET,
                        blk + IL_OFF_THR, blk + IL_OFF_CLAIM, blk + IL_OFF_DONE);
    if (len <= 0 || len > (int)IL_OFF_PARAMS) goto done;
    if (!rr_write(blk, code, (size_t)len)) {
        if (out) fprintf(out, "IL: stub write failed\n");
        goto done;
    }

    if (op == 1) {
        uint64_t vals[4], ptrs[4];
        if (nargs < 0) nargs = 0;
        if (nargs > 4) nargs = 4;
        memset(vals, 0, sizeof vals);
        memset(ptrs, 0, sizeof ptrs);
        for (int i = 0; i < nargs; ++i) {
            vals[i] = args[i];
            ptrs[i] = blk + IL_OFF_ARG + (uint64_t)i * 8;
        }
        if (!rr_write(blk + IL_OFF_ARG, vals, sizeof vals) ||
            !rr_write(blk + IL_OFF_PARAMS, ptrs, sizeof ptrs)) {
            if (out) fprintf(out, "IL: argument write failed\n");
            goto done;
        }
    }
    /* No slot may hold data from an earlier call: the claim slot decides who
     * executes, the done slot decides when the result is readable. */
    { uint64_t zero = 0;
      if (!rr_write(blk + IL_OFF_EXC, &zero, 8) || !rr_write(blk + IL_OFF_RET, &zero, 8) ||
          !rr_write(blk + IL_OFF_THR, &zero, 8) || !rr_write(blk + IL_OFF_CLAIM, &zero, 8) ||
          !rr_write(blk + IL_OFF_DONE, &zero, 8)) {
          if (out) fprintf(out, "IL: slot reset failed\n");
          goto done;
      } }

    if (via_driver) {
        /* Channel driver when one is mapped, plain user APC otherwise. The probe
         * is cached: it costs a round trip and the answer cannot change inside
         * one process. */
        static int chan = -1;
        if (chan < 0) chan = rr_exec_ping(200) ? 1 : 0;
        ok = chan ? il_run_apc(blk, &ret, &exc, out)
                  : il_run_umapc(blk, &ret, &exc, out);
        goto done;
    }

    th = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)blk, NULL, 0, NULL);
    if (!th) {
        DWORD err = GetLastError();
        if (out) {
            fprintf(out, "IL: CreateRemoteThread failed (%lu)", (unsigned long)err);
            if (err == ERROR_ACCESS_DENIED)
                fprintf(out, " - thread creation inside the target is denied; map "
                             "3_src_driver/rr_exec.sys for the APC path");
            fprintf(out, "\n");
        }
        goto done;
    }
    waited = WaitForSingleObject(th, IL_WAIT_MS);
    GetExitCodeThread(th, &exit_code);
    CloseHandle(th);
    th = NULL;
    if (waited != WAIT_OBJECT_0) {
        /* The thread is still executing the stub, so its block stays allocated:
         * releasing it here would free the code under a live instruction
         * pointer. One leaked page per stuck call is the correct trade. */
        leaked = 1;
        if (out) fprintf(out, "IL: thread did not finish in %ums (exit=%lu) "
                              "- block 0x%llX left allocated\n",
                         IL_WAIT_MS, (unsigned long)exit_code,
                         (unsigned long long)blk);
        goto done;
    }
    if (!rr_read(blk + IL_OFF_RET, &ret, 8) || !rr_read(blk + IL_OFF_EXC, &exc, 8)) {
        if (out) fprintf(out, "IL: result read failed\n");
        goto done;
    }
    ok = 1;

done:
    if (th) CloseHandle(th);
    /* Driver-owned pages are not ours to release: there is no free ioctl and the
     * target reclaims them at exit. */
    if (blk && !via_driver && !leaked) VirtualFreeEx(proc, (LPVOID)blk, 0, MEM_RELEASE);
    if (ok) {
        if (ret_out) *ret_out = ret;
        if (exc_out) *exc_out = exc;
    }
    return ok;
}
int rr_il_alloc(uint64_t klass, uint64_t* obj_out, FILE* out) {
    uint64_t api = 0, obj = 0, exc = 0;
    if (!klass) {
        if (out) fprintf(out, "IL: no class address\n");
        return 0;
    }
    if (!rr_il_export("il2cpp_object_new", &api)) {
        if (out) fprintf(out, "IL: il2cpp_object_new missing\n");
        return 0;
    }
    if (!il_run(0, api, klass, 0, NULL, 0, &obj, &exc, out)) return 0;
    if (exc) {
        if (out) fprintf(out, "IL: allocation threw 0x%llX\n", (unsigned long long)exc);
        return 0;
    }
    if (obj_out) *obj_out = obj;
    return obj != 0;
}

int rr_il_invoke(uint64_t method, uint64_t obj, const uint64_t* args, int nargs,
                 uint64_t* ret_out, uint64_t* exc_out, FILE* out) {
    uint64_t api = 0, ret = 0, exc = 0;
    if (!method) {
        if (out) fprintf(out, "IL: no method address\n");
        return 0;
    }
    if (!rr_il_export("il2cpp_runtime_invoke", &api)) {
        if (out) fprintf(out, "IL: il2cpp_runtime_invoke missing\n");
        return 0;
    }
    if (!il_run(1, api, method, obj, args, nargs, &ret, &exc, out)) return 0;
    if (ret_out) *ret_out = ret;
    if (exc_out) *exc_out = exc;
    return 1;
}

int rr_il_apc_smoke(FILE* out) {
    /* op 3: the stub is the claim guard plus the completion marker. If the
     * marker appears, the execution step reached the target and came back. */
    if (!il_run(3, 0, 0, 0, NULL, 0, NULL, NULL, out)) {
        if (out) fprintf(out, "IL: APC smoke test failed\n");
        return 0;
    }
    if (out) fprintf(out, "IL: APC smoke test ok - stub ran in pid %lu\n",
                     (unsigned long)g_rr_pid);
    return 1;
}

int rr_il_domain_probe(FILE* out) {
    uint64_t api = 0, dom = 0;
    if (!rr_il_export("il2cpp_domain_get", &api)) {
        if (out) fprintf(out, "IL: il2cpp_domain_get missing\n");
        return 0;
    }
    if (!il_run(2, api, 0, 0, NULL, 0, &dom, NULL, out)) {
        if (out) fprintf(out, "IL: il2cpp_domain_get did not complete\n");
        return 0;
    }
    if (out) fprintf(out, "IL: il2cpp_domain_get -> 0x%llX\n",
                     (unsigned long long)dom);
    return dom != 0;
}

int rr_il_object_class(uint64_t obj, char* name, int cap) {
    uint64_t klass;
    char nm[160];
    if (!obj || !name || cap <= 1) return 0;
    klass = rr_u64(obj);
    if (!klass || klass < 0x10000) return 0;
    if (il_cstr(rr_u64(klass + 0x10), nm, sizeof nm) <= 0) return 0;
    if (!nm[0]) return 0;
    snprintf(name, (size_t)cap, "%s", nm);
    return 1;
}
