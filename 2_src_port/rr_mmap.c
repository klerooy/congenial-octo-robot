/* rr_mmap.c - see rr_mmap.h for why this exists.
 *
 * Order of work, and the reason for each step:
 *   1. one RWX allocation in the target through the read/write driver, sized by
 *      the image's own SizeOfImage;
 *   2. headers and section raw data copied verbatim, so the image sits at the
 *      RVA layout its PE headers describe;
 *   3. base relocations applied for the new base - the file is linked at 0x180000000
 *      and the allocation lands wherever the target's address space has room;
 *   4. imports bound against the modules already loaded in the target, resolved
 *      by walking their export directories (no LoadLibrary, no GetProcAddress
 *      inside the target);
 *   5. entry point queued as a user-mode APC: rcx=base, rdx=DLL_PROCESS_ATTACH,
 *      r8=0, and the return value published to a slot the caller polls.
 */
#include "rr_mmap.h"
#include "rr_map.h"
#include "rr_mem.h"
#include "rr_exec.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- manual map (no loader entry) --------------------------------------- */

static int target_load_module(uint32_t pid, const char* module, uint64_t* out, FILE* log);

typedef struct {
    uint8_t* file;
    size_t   size;
    const uint8_t* sec;      /* first section header in `file`                */
    uint16_t nsec;
    uint16_t opt_magic;
    uint64_t image_base;
    uint32_t size_image;
    uint32_t size_headers;
    uint32_t entry;
    uint32_t reloc_rva, reloc_size;
    uint32_t import_rva, import_size;
    uint32_t tls_rva, tls_size;
    uint32_t exc_rva, exc_size;
} IMG;

static int rd(uint32_t pid, uint64_t a, void* b, size_t n) {
    return rr_read_pid(pid, a, b, n);
}
static int wr(uint32_t pid, uint64_t a, const void* b, size_t n) {
    return rr_write_pid(pid, a, b, n);
}

/* The transport caps one write at 0x10000 bytes; a section is larger, so the
 * copy walks the buffer in chunks that the driver accepts. */
static int wr_chunk(uint32_t pid, uint64_t a, const uint8_t* b, size_t n) {
    while (n) {
        size_t step = n > 0x8000u ? 0x8000u : n;
        if (!wr(pid, a, b, step)) return 0;
        a += step;
        b += step;
        n -= step;
    }
    return 1;
}

/* The transport caps one request at 0x10000 bytes in both directions, so the
 * template copy walks the buffer the same way the section copy does. */
static int rd_chunk(uint32_t pid, uint64_t a, uint8_t* b, size_t n) {
    while (n) {
        size_t step = n > 0x8000u ? 0x8000u : n;
        if (!rd(pid, a, b, step)) return 0;
        a += step;
        b += step;
        n -= step;
    }
    return 1;
}

static uint8_t* read_all(const char* path, size_t* size) {
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

static int img_parse(IMG* im, const char* path, FILE* log) {
    uint32_t lfanew = 0;
    const uint8_t* nt;
    const uint8_t* opt;

    memset(im, 0, sizeof *im);
    im->file = read_all(path, &im->size);
    if (!im->file) {
        if (log) fprintf(log, "  mmap: cannot read %s\n", path);
        return 0;
    }
    if (im->size < 0x200) { if (log) fprintf(log, "  mmap: file too small\n"); return 0; }
    memcpy(&lfanew, im->file + 0x3C, 4);
    if (lfanew < 0x40 || lfanew + 0x108 > im->size) {
        if (log) fprintf(log, "  mmap: bad e_lfanew\n");
        return 0;
    }
    nt = im->file + lfanew;
    if (memcmp(nt, "PE\0\0", 4) != 0) { if (log) fprintf(log, "  mmap: no PE signature\n"); return 0; }
    memcpy(&im->nsec, nt + 6, 2);
    memcpy(&im->opt_magic, nt + 24, 2);
    opt = nt + 24;
    if (im->opt_magic != 0x20B) {
        if (log) fprintf(log, "  mmap: not PE32+ (magic 0x%X)\n", im->opt_magic);
        return 0;
    }
    memcpy(&im->entry,        opt + 16, 4);
    memcpy(&im->image_base,   opt + 24, 8);
    memcpy(&im->size_image,   opt + 56, 4);
    memcpy(&im->size_headers, opt + 60, 4);
    /* DataDirectory: [1] import, [3] exception, [5] base reloc, [9] TLS. */
    memcpy(&im->import_rva,  opt + 112 + 1 * 8, 4);
    memcpy(&im->import_size, opt + 112 + 1 * 8 + 4, 4);
    memcpy(&im->exc_rva,     opt + 112 + 3 * 8, 4);
    memcpy(&im->exc_size,    opt + 112 + 3 * 8 + 4, 4);
    memcpy(&im->reloc_rva,   opt + 112 + 5 * 8, 4);
    memcpy(&im->reloc_size,  opt + 112 + 5 * 8 + 4, 4);
    memcpy(&im->tls_rva,     opt + 112 + 9 * 8, 4);
    memcpy(&im->tls_size,    opt + 112 + 9 * 8 + 4, 4);
    im->sec = opt + 240;   /* SizeOfOptionalHeader is 240 for PE32+ here */
    if (!im->size_image || !im->size_headers || im->size_headers > im->size_image) {
        if (log) fprintf(log, "  mmap: bad image sizes\n");
        return 0;
    }
    if ((size_t)(im->sec - im->file) + (size_t)im->nsec * 40 > im->size) {
        if (log) fprintf(log, "  mmap: section table outside the file\n");
        return 0;
    }
    if (log) fprintf(log, "  mmap: image=%u headers=%u entry=%X base=0x%llX sections=%u\n",
                     im->size_image, im->size_headers, im->entry,
                     (unsigned long long)im->image_base, (unsigned)im->nsec);
    return 1;
}

static void section(const IMG* im, int i, uint32_t* vsize, uint32_t* va,
                    uint32_t* rawsize, uint32_t* raw) {
    const uint8_t* s = im->sec + (size_t)i * 40;
    memcpy(vsize, s + 8, 4);
    memcpy(va, s + 12, 4);
    memcpy(rawsize, s + 16, 4);
    memcpy(raw, s + 20, 4);
}

static uint32_t rva2off(const IMG* im, uint32_t rva) {
    for (int i = 0; i < im->nsec; ++i) {
        uint32_t vsize, va, rawsize, raw;
        uint32_t span;
        section(im, i, &vsize, &va, &rawsize, &raw);
        span = vsize > rawsize ? vsize : rawsize;
        if (rva >= va && rva < va + span) {
            uint32_t off = rva - va;
            if (off >= rawsize) return 0;
            return raw + off;
        }
    }
    if (rva < im->size_headers && rva < im->size) return rva;
    return 0;
}

static int img_copy(uint32_t pid, uint64_t base, const IMG* im, FILE* log) {
    if (!wr_chunk(pid, base, im->file, im->size_headers)) {
        if (log) fprintf(log, "  mmap: header write refused at 0x%llX\n",
                         (unsigned long long)base);
        return 0;
    }
    for (int i = 0; i < im->nsec; ++i) {
        uint32_t vsize, va, rawsize, raw;
        section(im, i, &vsize, &va, &rawsize, &raw);
        if (!rawsize) continue;
        if ((size_t)raw + rawsize > im->size) {
            if (log) fprintf(log, "  mmap: section %d raw range outside the file\n", i);
            return 0;
        }
        if (!wr_chunk(pid, base + va, im->file + raw, rawsize)) {
            if (log) fprintf(log, "  mmap: section %d write refused at 0x%llX\n", i,
                             (unsigned long long)(base + va));
            return 0;
        }
    }
    return 1;
}

static int img_relocs(uint32_t pid, uint64_t base, const IMG* im, uint32_t* count, FILE* log) {
    int64_t delta = (int64_t)(base - im->image_base);
    uint32_t off, end;
    uint32_t applied = 0;

    *count = 0;
    if (!im->reloc_rva || !im->reloc_size) {
        if (log) fprintf(log, "  mmap: image has no relocations (fixed base)\n");
        return delta == 0;
    }
    off = rva2off(im, im->reloc_rva);
    if (!off) { if (log) fprintf(log, "  mmap: reloc directory unmapped\n"); return 0; }
    end = off + im->reloc_size;
    if (end > im->size) end = (uint32_t)im->size;

    while (off + 8 <= end) {
        uint32_t page = 0, block = 0;
        memcpy(&page, im->file + off, 4);
        memcpy(&block, im->file + off + 4, 4);
        if (!block || off + block > end) break;
        for (uint32_t p = off + 8; p + 2 <= off + block; p += 2) {
            uint16_t e = 0;
            uint32_t type, rva;
            memcpy(&e, im->file + p, 2);
            type = e >> 12;
            rva = page + (e & 0xFFF);
            if (type == 0) continue;
            if (type == 10) {
                uint64_t v = 0;
                if (!rd(pid, base + rva, &v, 8)) {
                    if (log) fprintf(log, "  mmap: reloc read failed at %X\n", rva);
                    return 0;
                }
                v = (uint64_t)((int64_t)v + delta);
                if (!wr(pid, base + rva, &v, 8)) {
                    if (log) fprintf(log, "  mmap: reloc write failed at %X\n", rva);
                    return 0;
                }
                ++applied;
            } else if (type == 3) {
                uint32_t v = 0;
                if (!rd(pid, base + rva, &v, 4)) return 0;
                v = (uint32_t)((int64_t)v + delta);
                if (!wr(pid, base + rva, &v, 4)) return 0;
                ++applied;
            } else if (type != 0) {
                if (log) fprintf(log, "  mmap: unsupported reloc type %u at %X\n", type, rva);
                return 0;
            }
        }
        off += block;
    }
    *count = applied;
    if (log) fprintf(log, "  mmap: %u relocations, delta=%lld\n", applied, (long long)delta);
    return 1;
}

static int img_imports(uint32_t pid, uint64_t base, const IMG* im, uint32_t* count, FILE* log) {
    uint32_t off = rva2off(im, im->import_rva);
    uint32_t bound = 0;

    *count = 0;
    if (!im->import_rva) { if (log) fprintf(log, "  mmap: no import directory\n"); return 1; }
    if (!off) { if (log) fprintf(log, "  mmap: import directory unmapped\n"); return 0; }

    for (;;) {
        uint32_t name_rva = 0, first = 0, oft = 0;
        char module[128];
        uint64_t mbase = 0;
        uint32_t msize = 0;
        uint32_t noff;
        uint32_t i;

        if (off + 20 > im->size) break;
        memcpy(&oft, im->file + off + 0, 4);
        memcpy(&name_rva, im->file + off + 12, 4);
        memcpy(&first, im->file + off + 16, 4);
        if (!name_rva && !first) break;
        noff = rva2off(im, name_rva);
        if (!noff) { if (log) fprintf(log, "  mmap: import name unmapped\n"); return 0; }
        memset(module, 0, sizeof module);
        strncpy(module, (const char*)im->file + noff, sizeof module - 1);
        if (!rr_module(module, &mbase, &msize) || !mbase) {
            /* A module the game never pulled in on its own: ask the target's own
             * loader for it. Signed images pass the anti-cheat's module check, so
             * the Direct2D stack loads; an unsigned name would not. */
            if (!target_load_module(pid, module, &mbase, log) || !mbase) {
                if (log) fprintf(log, "  mmap: %s is not loaded in the target\n", module);
                return 0;
            }
            msize = 0;
        }
        {
            uint32_t thunk_rva = oft ? oft : first;
            for (i = 0; ; ++i) {
                uint64_t thunk = 0;
                uint32_t slot = first + i * 8;
                if (!rd(pid, base + thunk_rva + (uint64_t)i * 8, &thunk, 8)) {
                    if (log) fprintf(log, "  mmap: thunk read failed in %s\n", module);
                    return 0;
                }
                if (!thunk) break;
                if (thunk & 0x8000000000000000ULL) {
                    uint64_t addr = 0;
                    if (!rr_target_export_ordinal(pid, module, (uint32_t)(thunk & 0xFFFF), &addr) ||
                        !addr) {
                        if (log) fprintf(log, "  mmap: %s ordinal %llu not resolved\n", module,
                                         (unsigned long long)(thunk & 0xFFFF));
                        return 0;
                    }
                    if (!wr(pid, base + slot, &addr, 8)) {
                        if (log) fprintf(log, "  mmap: ordinal import slot write failed\n");
                        return 0;
                    }
                    ++bound;
                    continue;
                }
                {
                    uint32_t hoff = rva2off(im, (uint32_t)thunk);
                    uint64_t addr = 0;
                    if (!hoff || hoff + 2 >= im->size) {
                        if (log) fprintf(log, "  mmap: import name unmapped in %s\n", module);
                        return 0;
                    }
                    if (!rr_target_export(pid, module, (const char*)im->file + hoff + 2, &addr) || !addr) {
                        if (log) fprintf(log, "  mmap: %s!%s not exported\n",
                                         module, (const char*)im->file + hoff + 2);
                        return 0;
                    }
                    if (!wr(pid, base + slot, &addr, 8)) {
                        if (log) fprintf(log, "  mmap: import slot write failed\n");
                        return 0;
                    }
                    ++bound;
                }
            }
        }
        off += 20;
    }
    *count = bound;
    if (log) fprintf(log, "  mmap: %u imports bound\n", bound);
    return 1;
}
/* An import can name a module the target has not touched yet - d2d1.dll is the
 * one that matters here, because the game draws with d3d11 and never asks for
 * Direct2D. Those modules are Microsoft-signed, so the loader inside the
 * protected process still accepts them: the path is written into the target and
 * LoadLibraryW is called from an APC. */
static int target_load_module(uint32_t pid, const char* module, uint64_t* out, FILE* log) {
    char path[MAX_PATH];
    wchar_t wpath[MAX_PATH];
    uint64_t datab = 0, loadlib = 0, hmod = 0;
    uint64_t arg;
    UINT syslen;
    int n;

    *out = 0;
    syslen = GetSystemDirectoryA(path, MAX_PATH);
    if (!syslen || syslen + 1 + strlen(module) + 1 > MAX_PATH) {
        if (log) fprintf(log, "  mmap: cannot build a path for %s\n", module);
        return 0;
    }
    snprintf(path + syslen, MAX_PATH - syslen, "\\%s", module);
    n = MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    if (!n) return 0;
    if (!rr_drv_alloc_remote(pid, 0x400, 0x04 /* PAGE_READWRITE */, &datab) || !datab) {
        if (log) fprintf(log, "  mmap: no page for the %s path\n", module);
        return 0;
    }
    if (!wr(pid, datab, wpath, (size_t)n * sizeof(wchar_t))) {
        if (log) fprintf(log, "  mmap: %s path write failed\n", module);
        return 0;
    }
    if (!rr_target_export(pid, "kernel32.dll", "LoadLibraryW", &loadlib) || !loadlib) {
        if (log) fprintf(log, "  mmap: kernel32!LoadLibraryW unresolved\n");
        return 0;
    }
    arg = datab;
    if (!rr_target_call(pid, NULL, NULL, loadlib, datab, 0, 0, &hmod, NULL, log)) return 0;
    if (!hmod) {
        if (log) fprintf(log, "  mmap: LoadLibraryW(%s) returned 0 in the target\n", module);
        return 0;
    }
    if (log) fprintf(log, "  mmap: loaded %s in the target at 0x%llX\n",
                     module, (unsigned long long)hmod);
    *out = hmod;
    return 1;
}


/* A manually mapped image is not in the loader's module list, so the unwinder
 * has no entry for it and an exception raised inside the image cannot be
 * dispatched: the CRT's own SEH - the stdio lock, a stack probe, a guard page -
 * takes the whole process down instead of unwinding one frame. Registering the
 * image's .pdata fixes that.
 *
 * RUNTIME_FUNCTION holds RVAs, and RtlAddFunctionTable resolves them against the
 * BaseAddress it is handed, so the base passed here must be the address the
 * image actually sits at - not the one it was linked for. Passing the link base
 * registers the table over unmapped VA space and looks successful: the entries
 * land at link_base+rva while the code runs at mapped_base+rva, so every
 * __try/__except in the payload is a comment. Measured: with the table at the
 * mapped base RtlLookupFunctionEntry(mapped_base+rva) answers, with the link base
 * it returns NULL. That NULL is what let one unreadable region during the sweep
 * kill the whole game process with no log line. */
static int img_register_unwind(uint32_t pid, uint64_t base, const IMG* im,
                               uint32_t* count, FILE* log) {
    uint64_t fn = 0, ret = 0;
    uint32_t n;

    if (count) *count = 0;
    if (!im->exc_rva || im->exc_size < 12) return 1;
    n = im->exc_size / 12;
    if (!rr_target_export(pid, "kernel32.dll", "RtlAddFunctionTable", &fn) || !fn) {
        if (log) fprintf(log, "  mmap: RtlAddFunctionTable unresolved\n");
        return 0;
    }
    if (!rr_target_call(pid, NULL, NULL, fn, base + im->exc_rva, n, base,
                        &ret, NULL, log))
        return 0;
    if (!ret) {
        if (log) fprintf(log, "  mmap: RtlAddFunctionTable refused %u entries\n", n);
        return 0;
    }
    if (count) *count = n;
    if (log) fprintf(log, "  mmap: %u unwind entries registered at base 0x%llX\n", n,
                     (unsigned long long)base);
    return 1;
}

/* ---- static TLS for a loader-invisible image ------------------------------
 *
 * `__declspec(thread)` variables live in a block the loader allocates per
 * thread and hangs off TEB.ThreadLocalStoragePointer at the index the image's
 * TLS directory names. A hand-mapped image has no index, so the CRT reads slot
 * 0 - another module's block - and its per-thread data lands on top of whatever
 * that module kept there. The CRT notices the wreckage later and calls abort(),
 * which is a __fastfail: no handler, no stack, just a dead process.
 *
 * The index has to be set before the entry point runs, and every thread that may
 * execute the image needs a block, so this is done here, in the mapper, exactly
 * where the loader would have done it. Threads created later get their block
 * from the payload's own thread attach. */
typedef struct {
    uint64_t start, end, index, callbacks;
    uint32_t zero_fill, chars;
} TLS_DIR;

typedef struct {
    LONG     exit_status;
    uint64_t teb;
    uint64_t client_id[2];
    uint64_t affinity;
    LONG     priority;
    LONG     base_priority;
} THREAD_BASIC;

extern LONG NtQueryInformationThread(HANDLE, ULONG, PVOID, ULONG, PULONG);

#define MAX_TLS_THREADS 256

/* The payload carries no TLS directory on purpose: `__declspec(thread)` data
 * needs a real index, and the TEB slot array is not a safe place to claim one.
 * Entries past the loader's bitmap hold allocator residue that differs thread to
 * thread, so a slot that reads empty on the main thread can hold another block
 * on the next, and writing through it lands in unmapped memory - which is what
 * produced the wild write at image offset 0x40090. The payload keeps its
 * per-thread state in fiber local storage instead (rr_td in rr_game.c), because
 * FlsAlloc hands out a slot from the same table the loader manages and its
 * cleanup callback runs for every thread, including threads this image created.
 *
 * If an image ever arrives here with a TLS directory, mapping it would corrupt
 * the target silently somewhere later, so refuse while the reason still logs. */
static int img_tls_install(uint32_t pid, uint64_t base, const IMG* im, int* slot_out,
                           FILE* log) {
    (void)pid;
    (void)base;
    if (slot_out) *slot_out = 0;
    if (!im->tls_rva || !im->tls_size) return 1;
    if (log)
        fprintf(log, "  mmap: image has a TLS directory (rva 0x%X); the payload must "
                     "carry no TLS and use FLS, refusing to map\n", im->tls_rva);
    return 0;
}
int rr_manual_map(uint32_t pid, const char* dll_path, RR_MMAP* out, FILE* log) {
    IMG im;
    uint64_t base = 0;
    uint32_t relocs = 0, imports = 0, waited = 0;
    uint64_t ret = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!img_parse(&im, dll_path, log)) return 0;
    if (!rr_drv_alloc_remote(pid, im.size_image, 0x40 /* PAGE_EXECUTE_READWRITE */, &base) || !base) {
        if (log) fprintf(log, "  mmap: allocation of 0x%X bytes refused\n", im.size_image);
        return 0;
    }
    if (log) fprintf(log, "  mmap: image allocated at 0x%llX\n", (unsigned long long)base);
    if (!img_copy(pid, base, &im, log)) return 0;
    if (!img_relocs(pid, base, &im, &relocs, log)) return 0;
    if (!img_imports(pid, base, &im, &imports, log)) return 0;
    if (!img_tls_install(pid, base, &im, NULL, log)) return 0;
    if (!img_register_unwind(pid, base, &im, NULL, log)) return 0;
    {
        uint64_t args[3];
        args[0] = base;                       /* rcx: image base                */
        args[1] = 1;                          /* rdx: DLL_PROCESS_ATTACH        */
        args[2] = 0;                          /* r8:  reserved                  */
        if (!rr_target_call(pid, NULL, NULL, base + im.entry, args[0], args[1], args[2],
                            &ret, &waited, log))
            return 0;
    }

    if (out) {
        out->base = base;
        out->size = im.size_image;
        out->entry = im.entry;
        out->dll_main = ret;
        out->relocs = relocs;
        out->imports = imports;
        out->waited_ms = waited;
    }
    if (log) fprintf(log, "  mmap: DllMain returned %llu after %ums, base=0x%llX\n",
                     (unsigned long long)ret, waited, (unsigned long long)base);
    free(im.file);
    return 1;
}
