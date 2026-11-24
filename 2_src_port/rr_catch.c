/* rr_catch.c - catch the fastfail that kills the game after the payload maps.
 *
 * A __fastfail (int 29h) is not dispatched to any in-process handler: no SEH, no
 * vectored handler, no unhandled-exception filter. The only place it can be
 * observed is a debugger. This attaches as the debugger, waits for the fatal
 * exception, and on it dumps the registers plus every stack word that points
 * into the mapped image. Those words are return addresses, so subtracting the
 * image base turns each one into an RVA that resolves against the link map.
 *
 *   rr_catch.exe <pid> [out.txt]
 *
 * Attach before mapping. The process is left to die exactly as it would have.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

static FILE* g_out;
static HANDLE g_proc;
static uint64_t g_base, g_size;
static int g_reported;

static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fflush(g_out);
}

static int read_mem(uint64_t addr, void* buf, size_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_proc, (LPCVOID)(uintptr_t)addr, buf, n, &got))
        return 0;
    return (int)got;
}

/* The image was mapped by hand, so it is not in the module list and nothing
 * announces its base. Walk back page by page from an address known to be inside
 * it until the PE header that contains that address turns up. */
static int find_image(uint64_t inside) {
    uint64_t page = inside & ~(uint64_t)0xFFF;
    int i;
    for (i = 0; i < 0x2000; ++i, page -= 0x1000) {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS nt;
        if (!read_mem(page, &dos, sizeof dos)) continue;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            dos.e_lfanew > 0x1000)
            continue;
        if (!read_mem(page + dos.e_lfanew, &nt, sizeof nt)) continue;
        if (nt.Signature != IMAGE_NT_SIGNATURE) continue;
        if (inside < page || inside >= page + nt.OptionalHeader.SizeOfImage) continue;
        g_base = page;
        g_size = nt.OptionalHeader.SizeOfImage;
        return 1;
    }
    return 0;
}

static void dump_stack(uint64_t rsp) {
    uint64_t words[600];
    int got, n, i;
    out("stack from 0x%llX:\n", (unsigned long long)rsp);
    got = read_mem(rsp, words, sizeof words);
    n = got / 8;
    for (i = 0; i < n; ++i) {
        uint64_t v = words[i];
        if (v >= g_base && v < g_base + g_size)
            out("  [%3d] 0x%llX  rva=0x%llX\n", i, (unsigned long long)v,
                (unsigned long long)(v - g_base));
    }
}

static void report_exception(EXCEPTION_RECORD* r, DWORD tid) {
    uintptr_t at = (uintptr_t)r->ExceptionAddress;
    DWORD i;

    out("--- exception code=0x%08lX tid=%lu at 0x%llX", (unsigned long)r->ExceptionCode,
        (unsigned long)tid, (unsigned long long)at);
    if (at >= g_base && at < g_base + g_size)
        out(" (rva=0x%llX)", (unsigned long long)(at - g_base));
    out("\n  params=%llu", (unsigned long long)r->NumberParameters);
    for (i = 0; i < r->NumberParameters && i < 4; ++i)
        out(" 0x%llX", (unsigned long long)r->ExceptionInformation[i]);
    out("\n");

    {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        CONTEXT ctx;
        if (!th) {
            out("  OpenThread failed: %lu\n", GetLastError());
            return;
        }
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_ALL;
        if (GetThreadContext(th, &ctx)) {
            out("  rip=0x%llX rsp=0x%llX rbp=0x%llX\n", (unsigned long long)ctx.Rip,
                (unsigned long long)ctx.Rsp, (unsigned long long)ctx.Rbp);
            dump_stack(ctx.Rsp);
        } else {
            out("  GetThreadContext failed: %lu\n", GetLastError());
        }
        CloseHandle(th);
    }
}

int main(int argc, char** argv) {
    DEBUG_EVENT ev;
    int looping = 1, first_break = 0;
    uint32_t pid;

    if (argc < 2) {
        printf("usage: rr_catch.exe <pid> [out.txt]\n");
        return 2;
    }
    pid = (uint32_t)strtoul(argv[1], NULL, 0);
    g_out = (argc > 2) ? fopen(argv[2], "w") : stdout;
    if (!g_out) g_out = stdout;

    if (!DebugActiveProcess(pid)) {
        out("DebugActiveProcess failed: %lu\n", GetLastError());
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    out("== rr_catch attached to %u ==\n", pid);

    while (looping) {
        DWORD status = DBG_CONTINUE;
        if (!WaitForDebugEvent(&ev, 30000)) {
            out("WaitForDebugEvent: no event in 30s (attached, still running)\n");
            continue;
        }
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hProcess) g_proc = ev.u.CreateProcessInfo.hProcess;
            if (!g_proc) g_proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ev.dwProcessId);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD* r = &ev.u.Exception.ExceptionRecord;
            uintptr_t at = (uintptr_t)r->ExceptionAddress;
            int inside = 0;

            if (!first_break) {
                first_break = 1;                 /* the initial debug break */
                break;
            }
            if (r->ExceptionCode == 0x80000003L || r->ExceptionCode == 0x80000004L)
                break;                            /* breakpoint / single step */
            if (r->ExceptionCode == 0xE06D7363L)
                break;                            /* first-chance C++ throw */
            if (g_size && at >= g_base && at < g_base + g_size)
                inside = 1;
            if (!inside && (r->ExceptionCode == 0xC0000409L ||
                            r->ExceptionCode == 0xC0000005L)) {
                /* A fatal-looking fault that is not ours yet: the image base is
                 * unknown, so try to locate one that contains it. */
                if (!g_size) inside = find_image(at);
                if (g_size && at >= g_base && at < g_base + g_size) inside = 1;
            }
            if (inside) {
                g_reported = 1;
                out("image base=0x%llX size=0x%llX\n", (unsigned long long)g_base,
                    (unsigned long long)g_size);
                report_exception(r, ev.dwThreadId);
                status = DBG_EXCEPTION_NOT_HANDLED;   /* let it die as it would */
            } else if (r->ExceptionCode >= 0xC0000000L) {
                out("fatal outside image: code=0x%08lX at 0x%llX\n",
                    (unsigned long)r->ExceptionCode, (unsigned long long)at);
                if (!g_size && find_image(at))
                    out("  (nearest image base=0x%llX size=0x%llX)\n",
                        (unsigned long long)g_base, (unsigned long long)g_size);
                status = DBG_EXCEPTION_NOT_HANDLED;
            }
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            out("exit-process code=0x%lX\n", (unsigned long)ev.u.ExitProcess.dwExitCode);
            looping = 0;
            break;
        default:
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, status);
    }
    if (g_out != stdout) fclose(g_out);
    return g_reported ? 0 : 1;
}
