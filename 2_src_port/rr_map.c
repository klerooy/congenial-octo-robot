/* rr_map.c - import-table trampoline loader for a protected target.
 *
 * See rr_map.h for the method and the reason each step exists. The only
 * primitives used here are memory reads and writes through the transport: no
 * process handle is opened, no remote allocation API is called, no thread is
 * created and no APC is queued.
 */
#include "rr_map.h"
#include "rr_mem.h"
#include "rr_exec.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAP_BLOCK   0x2000u
#define OFF_CODE    0x000u
#define OFF_SAVE    0x200u   /* rcx, rdx, r8, r9                              */
#define OFF_RAN     0x240u   /* completion marker                              */
#define OFF_RES     0x248u   /* HMODULE returned by LoadLibraryW               */
#define OFF_PATH    0x400u   /* UTF-16 payload path                            */
#define PATH_MAX_W  0x300u
#define RAN_MAGIC   0x4E41525F4D52495FULL /* "_IRM_RAN" */

static int rd(uint32_t pid, uint64_t a, void* b, size_t n) {
    return rr_read_pid(pid, a, b, n);
}
static int wr(uint32_t pid, uint64_t a, const void* b, size_t n) {
    return rr_write_pid(pid, a, b, n);
}

/* The transport refuses writes into read-only pages (the kernel copy probes for
 * write access). Two ways to get past that:
 *
 *   1. the channel driver attaches to the target and calls ZwProtectVirtualMemory
 *      from kernel mode - this is the only path that works on the game, because
 *      the anti-cheat strips PROCESS_VM_OPERATION from every user-mode handle;
 *   2. a user-mode VirtualProtectEx on a target nobody is guarding.
 *
 * The first is tried always, so the game path needs no special case. */
/* Set by the last failed wr_page so the caller can name the blocked step in one
 * line instead of making the next session bisect it. */
static char g_pagewrite_why[224] = "";

static int wr_page(uint32_t pid, uint64_t a, const void* b, size_t n) {
    uint64_t page = a & ~0xFFFULL;
    uint32_t oldp = 0;
    long st = 0;
    unsigned stage = 0;
    HANDLE h;
    DWORD old = 0;
    int ok, prot_ok;
    const char* why = "the page write was refused";

    g_pagewrite_why[0] = 0;
    if (wr(pid, a, b, n)) return 1;

    prot_ok = rr_exec_protect(pid, page, 0x1000, 0x04u /* PAGE_READWRITE */,
                              &st, &stage, &oldp);
    if (prot_ok && st >= 0) {
        ok = wr(pid, a, b, n);
        {
            uint32_t back = 0;
            long st2 = 0;
            unsigned stage2 = 0;
            rr_exec_protect(pid, page, 0x1000, oldp ? oldp : 0x02u,
                            &st2, &stage2, &back);
        }
        if (ok) return 1;
        why = "the page went writable but the write still failed";
    } else if (!prot_ok) {
        /* The driver never answered the protect op: either the request op is not
         * in this image (rr_exec.sys predates rr_exec4.sys) or the channel is
         * down. Either way the read-only import page cannot be opened by hand. */
        why = "the loaded driver has no OP_PROTECT (0x02), so a read-only page "
              "stays read-only";
    } else {
        snprintf(g_pagewrite_why, sizeof g_pagewrite_why,
                 "the driver refused the protection change (status=0x%08lX stage=%u)",
                 (unsigned long)st, stage);
        why = g_pagewrite_why;
    }

    h = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        strcpy(g_pagewrite_why, why);
        return 0;
    }
    if (!VirtualProtectEx(h, (LPVOID)(ULONG_PTR)page, 0x1000,
                          PAGE_EXECUTE_READWRITE, &old)) {
        CloseHandle(h);
        strcpy(g_pagewrite_why, why);
        return 0;
    }
    ok = wr(pid, a, b, n);
    VirtualProtectEx(h, (LPVOID)(ULONG_PTR)page, 0x1000, old, &old);
    CloseHandle(h);
    if (!ok) strcpy(g_pagewrite_why, why);
    return ok;
}

/* ---- target image path --------------------------------------------------- */

int rr_target_image_path(uint32_t pid, char* out, size_t cap) {
    uint64_t peb = rr_peb();
    uint64_t params, buf;
    uint16_t len = 0;
    wchar_t wpath[MAX_PATH * 2];
    if (!out || cap < 2) return 0;
    out[0] = 0;
    if (!peb) return 0;
    if (!rd(pid, peb + 0x20, &params, 8) || params < 0x10000) return 0;
    /* RTL_USER_PROCESS_PARAMETERS.ImagePathName is a counted UNICODE_STRING at
     * +0x60; the buffer pointer is a plain user address. */
    if (!rd(pid, params + 0x60, &len, 2)) return 0;
    if (!rd(pid, params + 0x68, &buf, 8)) return 0;
    if (!len || len > sizeof(wpath) - 2 || buf < 0x10000) return 0;
    if (!rd(pid, buf, wpath, len)) return 0;
    wpath[len / 2] = 0;
    if (!WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, (int)cap, NULL, NULL)) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* ---- export resolution inside the target -------------------------------- */

/* Every address below is read out of the target's own loaded image, so the
 * resolution does not depend on the local file matching the loaded one. */
static int tgt_export(uint32_t pid, uint64_t base, const char* name,
                      uint64_t* out, int depth) {
    uint8_t dos[0x40], nt[0x110], dir[0x28];
    uint32_t lfanew = 0, er = 0, es = 0;
    uint16_t magic = 0;
    uint32_t nfun = 0, nnam = 0, aof = 0, aon = 0, aoo = 0;
    char nm[96];

    if (depth > 4 || !base) return 0;
    if (!rd(pid, base, dos, sizeof dos)) return 0;
    memcpy(&lfanew, dos + 0x3C, 4);
    if (lfanew < 0x40 || lfanew > 0x1000) return 0;
    if (!rd(pid, base + lfanew, nt, sizeof nt)) return 0;
    if (memcmp(nt, "PE\0\0", 4) != 0) return 0;
    memcpy(&magic, nt + 0x18, 2);
    if (magic == 0x20B) {
        memcpy(&er, nt + 0x88, 4);
        memcpy(&es, nt + 0x8C, 4);
    } else if (magic == 0x10B) {
        memcpy(&er, nt + 0x78, 4);
        memcpy(&es, nt + 0x7C, 4);
    } else {
        return 0;
    }
    if (!er || es < 0x28) return 0;
    if (!rd(pid, base + er, dir, sizeof dir)) return 0;
    memcpy(&nfun, dir + 0x14, 4);
    memcpy(&nnam, dir + 0x18, 4);
    memcpy(&aof,  dir + 0x1C, 4);
    memcpy(&aon,  dir + 0x20, 4);
    memcpy(&aoo,  dir + 0x24, 4);
    if (!nnam || nnam > 0x10000 || !aof || !aon || !aoo) return 0;

    for (uint32_t i = 0; i < nnam; ++i) {
        uint32_t nrva = 0;
        uint16_t ord = 0;
        uint32_t frva = 0;
        if (!rd(pid, base + aon + (uint64_t)i * 4, &nrva, 4) || !nrva) continue;
        memset(nm, 0, sizeof nm);
        if (!rd(pid, base + nrva, nm, sizeof nm - 1)) continue;
        if (strcmp(nm, name) != 0) continue;
        if (!rd(pid, base + aoo + (uint64_t)i * 2, &ord, 2)) return 0;
        if (ord >= nfun) return 0;
        if (!rd(pid, base + aof + (uint64_t)ord * 4, &frva, 4) || !frva) return 0;
        if (frva >= er && frva < er + es) {
            /* Forwarder: "DLL.Function", resolved against the other module.
             * The module part has no extension while the PEB list does, so the
             * qualified name is built in a second buffer - writing ".dll" over
             * the dot would land on top of the function name. */
            char fwd[96];
            char fmod[104];
            char* dot;
            uint64_t fbase = 0;
            uint32_t fsize = 0;
            memset(fwd, 0, sizeof fwd);
            if (!rd(pid, base + frva, fwd, sizeof fwd - 1)) return 0;
            dot = strchr(fwd, '.');
            if (!dot) return 0;
            *dot = 0;
            {
                size_t fn = strlen(fwd);
                if (fn > 4 && !_stricmp(fwd + fn - 4, ".dll")) snprintf(fmod, sizeof fmod, "%s", fwd);
                else snprintf(fmod, sizeof fmod, "%s.dll", fwd);
            }
            if (!rr_module(fmod, &fbase, &fsize) || !fbase) return 0;
            return tgt_export(pid, fbase, dot + 1, out, depth + 1);
        }
        *out = base + frva;
        return 1;
    }
    return 0;
}

static int target_export(uint32_t pid, const char* module, const char* name,
                         uint64_t* out) {
    uint64_t base = 0;
    uint32_t size = 0;
    *out = 0;
    if (g_rr_pid != pid) rr_force_pid(pid);
    if (!rr_module(module, &base, &size) || !base) return 0;
    return tgt_export(pid, base, name, out, 0);
}

int rr_target_export(uint32_t pid, const char* module, const char* name, uint64_t* out) {
    if (!out) return 0;
    *out = 0;
    if (!module || !*module || !name || !*name) return 0;
    return target_export(pid, module, name, out);
}

/* The same walk, keyed by ordinal: some import tables reference a function by
 * number, and the payload's Direct2D entry is one of them. */
static int tgt_export_ord(uint32_t pid, uint64_t base, uint32_t want, uint64_t* out,
                          int depth) {
    uint8_t dos[0x40], nt[0x110], dir[0x28];
    uint32_t lfanew = 0, er = 0, es = 0, nfun = 0, base_ord = 0, aof = 0, frva = 0;
    uint16_t magic = 0;
    uint32_t idx;

    if (depth > 4 || !base || !want) return 0;
    if (!rd(pid, base, dos, sizeof dos)) return 0;
    memcpy(&lfanew, dos + 0x3C, 4);
    if (lfanew < 0x40 || lfanew > 0x1000) return 0;
    if (!rd(pid, base + lfanew, nt, sizeof nt)) return 0;
    if (memcmp(nt, "PE\0\0", 4) != 0) return 0;
    memcpy(&magic, nt + 0x18, 2);
    if (magic == 0x20B) {
        memcpy(&er, nt + 0x88, 4);
        memcpy(&es, nt + 0x8C, 4);
    } else if (magic == 0x10B) {
        memcpy(&er, nt + 0x78, 4);
        memcpy(&es, nt + 0x7C, 4);
    } else {
        return 0;
    }
    if (!er || es < 0x28) return 0;
    if (!rd(pid, base + er, dir, sizeof dir)) return 0;
    memcpy(&base_ord, dir + 0x10, 4);
    memcpy(&nfun, dir + 0x14, 4);
    memcpy(&aof,  dir + 0x1C, 4);
    if (!nfun || !aof || want < base_ord) return 0;
    idx = want - base_ord;
    if (idx >= nfun) return 0;
    if (!rd(pid, base + aof + (uint64_t)idx * 4, &frva, 4) || !frva) return 0;
    if (frva >= er && frva < er + es) {
        char fwd[96];
        char fmod[104];
        char* dot;
        uint64_t fbase = 0;
        uint32_t fsize = 0;
        memset(fwd, 0, sizeof fwd);
        if (!rd(pid, base + frva, fwd, sizeof fwd - 1)) return 0;
        dot = strchr(fwd, '.');
        if (!dot) return 0;
        *dot = 0;
        {
            size_t fn = strlen(fwd);
            if (fn > 4 && !_stricmp(fwd + fn - 4, ".dll")) snprintf(fmod, sizeof fmod, "%s", fwd);
            else snprintf(fmod, sizeof fmod, "%s.dll", fwd);
        }
        if (!rr_module(fmod, &fbase, &fsize) || !fbase) return 0;
        {
            uint64_t fb = 0;
            if (!rr_target_export(pid, fmod, dot + 1, &fb) || !fb) return 0;
            *out = fb;
            return 1;
        }
    }
    *out = base + frva;
    return 1;
}

int rr_target_export_ordinal(uint32_t pid, const char* module, uint32_t ord, uint64_t* out) {
    uint64_t base = 0;
    uint32_t size = 0;
    if (!out) return 0;
    *out = 0;
    if (!module || !*module || !ord) return 0;
    if (g_rr_pid != pid) rr_force_pid(pid);
    if (!rr_module(module, &base, &size) || !base) return 0;
    return tgt_export_ord(pid, base, ord, out, 0);
}

/* ---- local PE file: import slot RVA ------------------------------------- */

typedef struct { uint32_t va, vsz, raw, rsz; } SEC;

static uint32_t file_rva2off(const SEC* secs, int n, uint32_t rva) {
    for (int i = 0; i < n; ++i) {
        uint32_t span = secs[i].vsz > secs[i].rsz ? secs[i].vsz : secs[i].rsz;
        if (rva >= secs[i].va && rva < secs[i].va + span)
            return secs[i].raw + (rva - secs[i].va);
    }
    return 0;
}

static uint8_t* read_file_all(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    long n;
    uint8_t* buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0 || n > 0x10000000) { fclose(f); return NULL; }
    rewind(f);
    buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

/* Returns the RVA of the import slot for dll_want!func in the file's IAT. */
static int file_import_slot(const char* path, const char* dll_want, const char* func,
                            uint32_t* slot_rva, FILE* log) {
    size_t size = 0;
    uint8_t* d = read_file_all(path, &size);
    uint32_t lfanew, imp_rva = 0, imp_size = 0;
    uint16_t optsz, nsec;
    uint16_t magic;
    SEC secs[96];
    int ns = 0, found = 0;

    *slot_rva = 0;
    if (log) fprintf(log, "  iat: %s!%s in %s\n", dll_want, func, path);
    if (!d) {
        if (log) fprintf(log, "  iat: cannot read %s\n", path);
        return 0;
    }
    if (size < 0x200) { free(d); return 0; }
    memcpy(&lfanew, d + 0x3C, 4);
    if (lfanew < 0x40 || lfanew + 0x120 > size) { free(d); return 0; }
    memcpy(&magic, d + lfanew + 0x18, 2);
    memcpy(&nsec, d + lfanew + 6, 2);
    memcpy(&optsz, d + lfanew + 20, 2);
    if (magic != 0x20B || nsec > 96) { free(d); return 0; }
    memcpy(&imp_rva, d + lfanew + 24 + 112 + 8, 4);
    memcpy(&imp_size, d + lfanew + 24 + 112 + 12, 4);
    if (!imp_rva) { free(d); return 0; }

    {
        uint32_t so = lfanew + 24 + optsz;
        for (uint32_t i = 0; i < nsec; ++i) {
            uint32_t s = so + i * 40;
            if (s + 40 > size) break;
            memcpy(&secs[i].vsz, d + s + 8, 4);
            memcpy(&secs[i].va,  d + s + 12, 4);
            memcpy(&secs[i].rsz, d + s + 16, 4);
            memcpy(&secs[i].raw, d + s + 20, 4);
            ++ns;
        }
    }

    {
        uint32_t off = file_rva2off(secs, ns, imp_rva);
        if (!off) { free(d); return 0; }
        for (int k = 0; k < 512; ++k, off += 20) {
            uint32_t name_rva, iat, ilt;
            if (off + 20 > size) break;
            memcpy(&ilt, d + off + 0, 4);
            memcpy(&name_rva, d + off + 12, 4);
            memcpy(&iat, d + off + 16, 4);
            if (!name_rva || !iat) break;
            {
                uint32_t noff = file_rva2off(secs, ns, name_rva);
                const char* dn = noff ? (const char*)(d + noff) : "";
                if (_stricmp(dn, dll_want) != 0) continue;
            }
            {
                uint32_t thunk_rva = ilt ? ilt : iat;
                uint32_t toff = file_rva2off(secs, ns, thunk_rva);
                if (!toff) break;
                for (uint32_t j = 0; j < 4096 && toff + 8 <= size; ++j, toff += 8) {
                    uint64_t v;
                    memcpy(&v, d + toff, 8);
                    if (!v) break;   /* end of this module's thunk list */
                    if (!(v >> 63)) {
                        uint32_t nro = file_rva2off(secs, ns, (uint32_t)(v & 0x7FFFFFFF));
                        if (nro && nro + 2 < size && strcmp((const char*)(d + nro + 2), func) == 0) {
                            *slot_rva = iat + j * 8;
                            found = 1;
                            break;
                        }
                    }
                }
            }
            if (found) break;
        }
    }
    free(d);
    if (!found && log)
        fprintf(log, "  iat: %s!%s not found in %s\n", dll_want, func, path);
    return found;
}

/* ---- trampoline ---------------------------------------------------------- */

typedef struct {
    uint8_t* p;
    size_t   n;
    size_t   disp[16];
    int      ndisp;
} EMIT;

static void em(EMIT* e, const void* src, size_t n) {
    memcpy(e->p + e->n, src, n);
    e->n += n;
}
static void em_u8(EMIT* e, uint8_t v) { em(e, &v, 1); }
static void em_imm64(EMIT* e, uint64_t v) { em(e, &v, 8); }
static void em_disp(EMIT* e) {
    e->disp[e->ndisp++] = e->n;
    { int32_t z = 0; em(e, &z, 4); }
}

/* mov [rip+d], reg / mov reg, [rip+d] for rax and the four argument
 * registers. REX.R is set for r8/r9, which live in the extended file. */
static void mov_store(EMIT* e, int reg) {
    em_u8(e, (uint8_t)(0x48 | (reg >= 8 ? 0x04 : 0)));
    em_u8(e, 0x89);
    em_u8(e, (uint8_t)(0x05 | ((reg & 7) << 3)));
    em_disp(e);
}
static void mov_load(EMIT* e, int reg) {
    em_u8(e, (uint8_t)(0x48 | (reg >= 8 ? 0x04 : 0)));
    em_u8(e, 0x8B);
    em_u8(e, (uint8_t)(0x05 | ((reg & 7) << 3)));
    em_disp(e);
}
static void patch_disps(EMIT* e, uint64_t code_base, const size_t* target_abs) {
    for (int i = 0; i < e->ndisp; ++i) {
        size_t at = e->disp[i];
        int32_t d = (int32_t)((int64_t)target_abs[i] -
                              (int64_t)(code_base + at + 4));
        memcpy(e->p + at, &d, 4);
    }
}

/* Emits the trampoline. code_base is the address the buffer will live at;
 * the only absolute values are LoadLibraryW, the path and the original
 * function. The slot offsets are RIP-relative so the page can move. */
static int build_trampoline(uint8_t* buf, size_t cap, uint64_t code_base,
                            uint64_t loadlib, uint64_t path_addr, uint64_t orig,
                            uint64_t save_off, uint64_t ran_off, uint64_t res_off,
                            int* out_len) {
    EMIT e = { buf, 0, {0}, 0 };
    size_t tgt[16];
    int nt = 0;

    /* save rcx, rdx, r8, r9 */
    mov_store(&e, 1); tgt[nt++] = save_off + 0;
    mov_store(&e, 2); tgt[nt++] = save_off + 8;
    mov_store(&e, 8); tgt[nt++] = save_off + 16;
    mov_store(&e, 9); tgt[nt++] = save_off + 24;

    /* mov rcx, path ; mov rax, LoadLibraryW ; sub rsp,0x28 ; call rax */
    em_u8(&e, 0x48); em_u8(&e, 0xB9); em_imm64(&e, path_addr);
    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, loadlib);
    { uint8_t s[4] = { 0x48, 0x83, 0xEC, 0x28 }; em(&e, s, 4); }
    { uint8_t s[2] = { 0xFF, 0xD0 }; em(&e, s, 2); }
    { uint8_t s[4] = { 0x48, 0x83, 0xC4, 0x28 }; em(&e, s, 4); }

    /* mov [res], rax ; mov rax, magic ; mov [ran], rax */
    em_u8(&e, 0x48); em_u8(&e, 0x89); em_u8(&e, 0x05); em_disp(&e);
    tgt[nt++] = res_off;
    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, RAN_MAGIC);
    em_u8(&e, 0x48); em_u8(&e, 0x89); em_u8(&e, 0x05); em_disp(&e);
    tgt[nt++] = ran_off;

    /* restore the argument registers */
    mov_load(&e, 1); tgt[nt++] = save_off + 0;
    mov_load(&e, 2); tgt[nt++] = save_off + 8;
    mov_load(&e, 8); tgt[nt++] = save_off + 16;
    mov_load(&e, 9); tgt[nt++] = save_off + 24;

    /* jmp original */
    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, orig);
    { uint8_t s[2] = { 0xFF, 0xE0 }; em(&e, s, 2); }

    if (e.ndisp != nt) return 0;
    patch_disps(&e, code_base, tgt);
    if (e.n > cap) return 0;
    *out_len = (int)e.n;
    return 1;
}

/* ---- loader -------------------------------------------------------------- */

int rr_file_import_slot(const char* path, const char* dll, const char* func,
                        uint32_t* slot_rva, FILE* log) {
    return file_import_slot(path, dll, func, slot_rva, log);
}

static const char* base_name(const char* path) {
    const char* a = strrchr(path, '\\');
    const char* b = strrchr(path, '/');
    const char* s = a > b ? a : b;
    return s ? s + 1 : path;
}

static void dir_of(const char* path, char* out, size_t cap) {
    const char* a = strrchr(path, '\\');
    size_t n = a ? (size_t)(a - path) + 1 : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

int rr_int_load(uint32_t pid, const char* dll_path, const char* trigger_module,
                const char* trigger_import, RR_INT_LOAD* out, FILE* log) {
    char image[MAX_PATH * 2], dir[MAX_PATH * 2], trigger_file[MAX_PATH * 2];
    
    const char* module = trigger_module && *trigger_module ? trigger_module
                                                          : RR_MAP_DEFAULT_TRIGGER;
    const char* import = trigger_import && *trigger_import ? trigger_import
                                                            : RR_MAP_DEFAULT_IMPORT;
    uint64_t blk = 0, loadlib = 0, orig = 0, tbase = 0;
    uint32_t tsize = 0, slot_rva = 0, waited = 0;
    wchar_t pathw[PATH_MAX_W];
    uint8_t code[0x200];
    int len = 0, ok = 0;
    DWORD t0;
    uint64_t ran = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!dll_path || !*dll_path) return 0;
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        if (log) fprintf(log, "  map: payload missing: %s\n", dll_path);
        return 0;
    }
    if (g_rr_pid != pid) rr_force_pid(pid);

    if (!rr_target_image_path(pid, image, sizeof image)) {
        if (log) fprintf(log, "  map: cannot read the target image path from the PEB\n");
        return 0;
    }
    dir_of(image, dir, sizeof dir);
    if (log) fprintf(log, "  map: target image %s\n", image);

    if (strstr(module, "\\")) {
        snprintf(trigger_file, sizeof trigger_file, "%s", module);
        module = base_name(module);
    } else {
        snprintf(trigger_file, sizeof trigger_file, "%s%s", dir, module);
    }
    if (!file_import_slot(trigger_file, "user32.dll", import, &slot_rva, log)) {
        if (log) fprintf(log, "  map: aborting, no trigger slot\n");
        return 0;
    }
    if (!target_export(pid, "kernel32.dll", "LoadLibraryW", &loadlib) || !loadlib) {
        if (log) fprintf(log, "  map: LoadLibraryW not resolvable in the target\n");
        return 0;
    }
    if (!rr_module(module, &tbase, &tsize) || !tbase) {
        if (log) fprintf(log, "  map: %s is not loaded in the target\n", module);
        return 0;
    }
    if (!rd(pid, tbase + slot_rva, &orig, 8) || orig < 0x10000) {
        if (log) fprintf(log, "  map: trigger slot unreadable or empty (0x%llX)\n",
                         (unsigned long long)orig);
        return 0;
    }
    {
        /* The slot must already point into user32: anything else means the file
         * on disk and the loaded image disagree about the import layout. */
        uint64_t ub = 0;
        uint32_t us = 0;
        if (rr_module("user32.dll", &ub, &us) && ub && (orig < ub || orig >= ub + us)) {
            if (log) fprintf(log, "  map: slot value 0x%llX is outside user32 "
                                  "(0x%llX+0x%X); file/loaded image mismatch\n",
                             (unsigned long long)orig, (unsigned long long)ub, us);
            return 0;
        }
    }

    if (!rr_drv_alloc_remote(pid, MAP_BLOCK, 0x40 /* PAGE_EXECUTE_READWRITE */, &blk) || !blk) {
        if (log) fprintf(log, "  map: no executable page in the target (driver path required)\n");
        return 0;
    }
    if (log) fprintf(log, "  map: %s base=0x%llX slot=%s!%s@0x%llX -> 0x%llX\n",
                     module, (unsigned long long)tbase, module, import,
                     (unsigned long long)(tbase + slot_rva),
                     (unsigned long long)orig);
    if (log) fprintf(log, "  map: trampoline page 0x%llX, LoadLibraryW=0x%llX\n",
                     (unsigned long long)blk, (unsigned long long)loadlib);

    if (!build_trampoline(code, sizeof code, blk + OFF_CODE, loadlib, blk + OFF_PATH,
                          orig, blk + OFF_SAVE, blk + OFF_RAN, blk + OFF_RES, &len)) {
        if (log) fprintf(log, "  map: trampoline build failed\n");
        return 0;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, dll_path, -1, pathw, PATH_MAX_W)) {
        if (log) fprintf(log, "  map: payload path too long\n");
        return 0;
    }
    
    {
        uint64_t zeros[6] = { 0, 0, 0, 0, 0, 0 };
        if (!wr(pid, blk + OFF_CODE, code, (size_t)len) ||
            !wr(pid, blk + OFF_SAVE, zeros, 32) ||
            !wr(pid, blk + OFF_RAN, zeros, 16) ||
            !wr(pid, blk + OFF_PATH, pathw, (wcslen(pathw) + 1) * sizeof(wchar_t))) {
            if (log) fprintf(log, "  map: trampoline write failed\n");
            return 0;
        }
    }
    {
        uint8_t back[0x100];
        if (!rd(pid, blk + OFF_CODE, back, (size_t)len) ||
            memcmp(back, code, (size_t)len) != 0) {
            if (log) fprintf(log, "  map: trampoline readback mismatch\n");
            return 0;
        }
    }

    /* The patch: one pointer, in a page this process never writes. */
    {
        uint64_t hook = blk + OFF_CODE;
        if (!wr_page(pid, tbase + slot_rva, &hook, 8)) {
            if (log) fprintf(log, "  map: import patch refused (%s)\n", g_pagewrite_why);
            return 0;
        }
        {
            uint64_t check = 0;
            if (!rd(pid, tbase + slot_rva, &check, 8) || check != hook) {
                if (log) fprintf(log, "  map: import patch did not stick\n");
                return 0;
            }
        }
        if (log) fprintf(log, "  map: %s!%s patched; waiting for a call\n", module, import);
    }

    t0 = GetTickCount();
    while (GetTickCount() - t0 < 8000) {
        if (rd(pid, blk + OFF_RAN, &ran, 8) && ran == RAN_MAGIC) break;
        Sleep(4);
    }
    waited = GetTickCount() - t0;

    /* Restore before anything else, including the failure path: a slot left
     * pointing at a page nobody owns is how the game dies later. */
    wr_page(pid, tbase + slot_rva, &orig, 8);

    if (ran != RAN_MAGIC) {
        if (log) fprintf(log, "  map: %s!%s was not called in %ums; slot restored, "
                              "nothing loaded\n", module, import, waited);
        return 0;
    }
    {
        uint64_t res = 0;
        rd(pid, blk + OFF_RES, &res, 8);
        if (!res) {
            if (log) fprintf(log, "  map: trampoline ran but LoadLibraryW returned 0 "
                                  "(GetLastError is in the game thread)\n");
            return 0;
        }
        ok = 1;
        if (out) {
            out->image_base = res;
            out->stub_addr = blk + OFF_CODE;
            out->iat_slot = tbase + slot_rva;
            out->original_fn = orig;
            out->waited_ms = waited;
        }
        if (log) fprintf(log, "  map: loaded in %ums, HMODULE=0x%llX\n",
                         waited, (unsigned long long)res);
    }
    {
        uint64_t base = 0;
        uint32_t size = 0;
        if (rr_module(base_name(dll_path), &base, &size) && base)
            if (log) fprintf(log, "  map: %s visible in the loader list at 0x%llX "
                                  "size=0x%X\n", base_name(dll_path),
                             (unsigned long long)base, size);
        else if (log)
            fprintf(log, "  map: %s not in the loader list (returned handle still "
                         "non-zero)\n", base_name(dll_path));
    }
    return ok;
}

/* Emits the same trampoline as build_trampoline, with the call target and its
 * three arguments supplied by the caller instead of a LoadLibraryW path. Used to
 * run one function inside a protected process: an anti-cheat that drops queued
 * APCs still cannot stop a call the game makes to its own import. */
static int build_call_trampoline(uint8_t* buf, size_t cap, uint64_t code_base,
                                 uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t orig, uint64_t save_off, uint64_t ran_off,
                                 uint64_t res_off, uint64_t once_off, int* out_len) {
    EMIT e = { buf, 0, {0}, 0 };
    size_t tgt[16];
    int nt = 0;
    size_t once_disp;

    /* Fire-once guard, and it is not an optimisation. The import slot stays
     * patched until the caller restores it, and a game calls PeekMessageW
     * thousands of times a second, so the stub runs again within the same
     * millisecond. Running an image's entry point twice is fatal: the CRT
     * checks its startup state and calls __scrt_fastfail when it finds the
     * initialiser nested, which is an int 29h nobody can catch. The guard makes
     * every fire after the first a plain pass-through to the real function.
     * `lock xchg` over a slot the page was allocated with at zero. */
    { uint8_t s[7] = { 0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00 }; em(&e, s, 7); }
    em_u8(&e, 0xF0); em_u8(&e, 0x48); em_u8(&e, 0x87); em_u8(&e, 0x05); em_disp(&e);
    tgt[nt++] = once_off;
    { uint8_t s[3] = { 0x48, 0x85, 0xC0 }; em(&e, s, 3); }        /* test rax, rax */
    { uint8_t s[2] = { 0x0F, 0x85 }; em(&e, s, 2); }              /* jnz pass-through */
    once_disp = e.n;
    { uint8_t z[4] = { 0, 0, 0, 0 }; em(&e, z, 4); }

    mov_store(&e, 1); tgt[nt++] = save_off + 0;
    mov_store(&e, 2); tgt[nt++] = save_off + 8;
    mov_store(&e, 8); tgt[nt++] = save_off + 16;
    mov_store(&e, 9); tgt[nt++] = save_off + 24;

    em_u8(&e, 0x48); em_u8(&e, 0xB9); em_imm64(&e, a1);          /* mov rcx, a1 */
    em_u8(&e, 0x48); em_u8(&e, 0xBA); em_imm64(&e, a2);          /* mov rdx, a2 */
    em_u8(&e, 0x49); em_u8(&e, 0xB8); em_imm64(&e, a3);          /* mov r8,  a3 */
    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, fn);          /* mov rax, fn */
    { uint8_t s[4] = { 0x48, 0x83, 0xEC, 0x28 }; em(&e, s, 4); }
    { uint8_t s[2] = { 0xFF, 0xD0 }; em(&e, s, 2); }
    { uint8_t s[4] = { 0x48, 0x83, 0xC4, 0x28 }; em(&e, s, 4); }

    em_u8(&e, 0x48); em_u8(&e, 0x89); em_u8(&e, 0x05); em_disp(&e);
    tgt[nt++] = res_off;
    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, RAN_MAGIC);
    em_u8(&e, 0x48); em_u8(&e, 0x89); em_u8(&e, 0x05); em_disp(&e);
    tgt[nt++] = ran_off;

    mov_load(&e, 1); tgt[nt++] = save_off + 0;
    mov_load(&e, 2); tgt[nt++] = save_off + 8;
    mov_load(&e, 8); tgt[nt++] = save_off + 16;
    mov_load(&e, 9); tgt[nt++] = save_off + 24;

    {   /* pass-through label: the return address is already the caller's, and */
        /* the argument registers were never touched on this path.            */
        int32_t rel = (int32_t)((int64_t)e.n - (int64_t)(once_disp + 4));
        memcpy(buf + once_disp, &rel, 4);
    }

    em_u8(&e, 0x48); em_u8(&e, 0xB8); em_imm64(&e, orig);
    { uint8_t s[2] = { 0xFF, 0xE0 }; em(&e, s, 2); }

    if (e.ndisp != nt) return 0;
    patch_disps(&e, code_base, tgt);
    if (e.n > cap) return 0;
    *out_len = (int)e.n;
    return 1;
}

int rr_target_call(uint32_t pid, const char* trigger_module, const char* trigger_import,
                   uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t* result, uint32_t* waited, FILE* log) {
    char image[MAX_PATH * 2], dir[MAX_PATH * 2], trigger_file[MAX_PATH * 2];
    const char* module = trigger_module && *trigger_module ? trigger_module
                                                          : RR_MAP_DEFAULT_TRIGGER;
    const char* import = trigger_import && *trigger_import ? trigger_import
                                                            : RR_MAP_DEFAULT_IMPORT;
    uint64_t blk = 0, orig = 0, tbase = 0;
    uint32_t tsize = 0, slot_rva = 0, waited_ms = 0;
    uint8_t code[0x200];
    int len = 0;
    DWORD t0;
    uint64_t ran = 0;

    if (result) *result = 0;
    if (waited) *waited = 0;
    if (!fn) return 0;
    if (g_rr_pid != pid) rr_force_pid(pid);

    if (!rr_target_image_path(pid, image, sizeof image)) {
        if (log) fprintf(log, "  call: cannot read the target image path from the PEB\n");
        return 0;
    }
    dir_of(image, dir, sizeof dir);

    if (strstr(module, "\\")) {
        snprintf(trigger_file, sizeof trigger_file, "%s", module);
        module = base_name(module);
    } else {
        snprintf(trigger_file, sizeof trigger_file, "%s%s", dir, module);
    }
    if (!file_import_slot(trigger_file, "user32.dll", import, &slot_rva, log)) {
        if (log) fprintf(log, "  call: no trigger slot in %s\n", trigger_file);
        return 0;
    }
    if (!rr_module(module, &tbase, &tsize) || !tbase) {
        if (log) fprintf(log, "  call: %s is not loaded in the target\n", module);
        return 0;
    }
    if (!rd(pid, tbase + slot_rva, &orig, 8) || orig < 0x10000) {
        if (log) fprintf(log, "  call: trigger slot unreadable (0x%llX)\n",
                         (unsigned long long)orig);
        return 0;
    }
    if (!rr_drv_alloc_remote(pid, MAP_BLOCK, 0x40 /* PAGE_EXECUTE_READWRITE */, &blk) || !blk) {
        if (log) fprintf(log, "  call: no executable page in the target\n");
        return 0;
    }
    if (!build_call_trampoline(code, sizeof code, blk + OFF_CODE, fn, a1, a2, a3, orig,
                               blk + OFF_SAVE, blk + OFF_RAN, blk + OFF_RES,
                               blk + OFF_RES + 8, &len)) {
        if (log) fprintf(log, "  call: trampoline build failed\n");
        return 0;
    }
    {
        uint64_t zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        if (!wr(pid, blk + OFF_CODE, code, (size_t)len) ||
            !wr(pid, blk + OFF_SAVE, zeros, 32) ||
            !wr(pid, blk + OFF_RAN, zeros, 32)) {
            if (log) fprintf(log, "  call: trampoline write failed\n");
            return 0;
        }
    }
    {
        uint64_t hook = blk + OFF_CODE;
        if (!wr_page(pid, tbase + slot_rva, &hook, 8)) {
            if (log) fprintf(log, "  call: import patch refused (%s)\n", g_pagewrite_why);
            return 0;
        }
        {
            uint64_t check = 0;
            if (!rd(pid, tbase + slot_rva, &check, 8) || check != hook) {
                if (log) fprintf(log, "  call: import patch did not stick\n");
                return 0;
            }
        }
    }

    t0 = GetTickCount();
    while (GetTickCount() - t0 < 8000) {
        if (rd(pid, blk + OFF_RAN, &ran, 8) && ran == RAN_MAGIC) break;
        Sleep(4);
    }
    waited_ms = GetTickCount() - t0;
    wr_page(pid, tbase + slot_rva, &orig, 8);
    if (waited) *waited = waited_ms;

    if (ran != RAN_MAGIC) {
        if (log) fprintf(log, "  call: %s!%s was not called in %ums; slot restored\n",
                         module, import, waited_ms);
        return 0;
    }
    if (result) rd(pid, blk + OFF_RES, result, 8);
    if (log) fprintf(log, "  call: fn=0x%llX returned after %ums, rax=0x%llX\n",
                     (unsigned long long)fn, waited_ms,
                     (unsigned long long)(result ? *result : 0));
    return 1;
}

