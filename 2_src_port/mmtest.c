/* mmtest.c - a payload that does nothing but prove how far it got.
 *
 * Used to separate the two causes after a manual map inside the game: the
 * mapper producing an image that cannot survive, or the payload's own worker
 * thread dying on its first CRT call. Every marker goes out through WriteFile on
 * a handle opened in DllMain, so a marker that is missing means the code above
 * it did not run - not that a buffer was lost.
 */
#include <windows.h>
#include <stdio.h>

extern IMAGE_DOS_HEADER __ImageBase;

static HANDLE g_log;

static void mark(const char* what) {
    char line[160];
    DWORD n = 0, wrote = 0;
    int len;
    if (g_log == INVALID_HANDLE_VALUE || !g_log) return;
    len = snprintf(line, sizeof line, "mmtest: %s pid=%lu tick=%lu\r\n", what,
                   (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    if (len <= 0) return;
    WriteFile(g_log, line, (DWORD)len, &wrote, NULL);
    FlushFileBuffers(g_log);
    (void)n;
}

/* A thread the loader did not create never gets its CRT per-thread data, because
 * this image is not in the loader list and no DLL_THREAD_ATTACH is delivered to
 * it. The entry point is the CRT's own thread-attach path, so calling it with
 * that reason does exactly what the loader would have done. */
static void crt_thread_attach(HINSTANCE mod) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)mod + dos->e_lfanew);
    BOOL (WINAPI* entry)(HINSTANCE, DWORD, LPVOID) =
        (BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID))
        ((char*)mod + nt->OptionalHeader.AddressOfEntryPoint);
    entry(mod, DLL_THREAD_ATTACH, NULL);
}

static DWORD WINAPI worker(LPVOID unused) {
    FILE* f;
    (void)unused;
    crt_thread_attach((HINSTANCE)&__ImageBase);
    mark("thread-start");
    f = fopen("C:\\Users\\Public\\mmtest_crt.log", "a");
    if (f) {
        fprintf(f, "mmtest: fopen on the new thread worked pid=%lu\r\n",
                (unsigned long)GetCurrentProcessId());
        fclose(f);
        mark("fopen-ok");
    } else {
        mark("fopen-null");
    }
    mark("thread-end");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE mod, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_log = CreateFileA("C:\\Users\\Public\\mmtest.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        mark("attach");
        DisableThreadLibraryCalls(mod);
        {
            HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (t) {
                mark("thread-created");
                CloseHandle(t);
            } else {
                mark("thread-create-failed");
            }
        }
        mark("attach-done");
    }
    return TRUE;
}
