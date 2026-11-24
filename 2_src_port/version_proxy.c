/* version.dll proxy - loads rr_int.dll into the game process at start.
 *
 * UnityPlayer.dll imports VERSION.dll, and the loader resolves it from the
 * application directory first. Dropping this stub next to ROB RUN.exe means
 * the payload is inside the process before the engine creates its swapchain,
 * with no driver, no handle and no remote thread.
 *
 * Every export is forwarded to the real %SystemRoot%\System32\version.dll,
 * loaded by absolute path so the proxy cannot resolve to itself.
 */

/* winver.h declares the VERSION.dll entry points with __declspec(dllimport);
 * the definitions below would collide with that linkage, so the declarations
 * are skipped and the exports are defined here instead. */
#define VER_H
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static HMODULE g_real;
static CRITICAL_SECTION g_cs;
static int g_cs_ready;

static void plog(const char* fmt, ...) {
    FILE* f;
    va_list ap;
    if (g_cs_ready) EnterCriticalSection(&g_cs);
    va_start(ap, fmt);
    f = fopen("C:\\Users\\Public\\rr_proxy.log", "a");
    if (f) {
        vfprintf(f, fmt, ap);
        fputc('\n', f);
        fclose(f);
    }
    va_end(ap);
    if (g_cs_ready) LeaveCriticalSection(&g_cs);
}

static HMODULE real_module(void) {
    WCHAR sys[MAX_PATH];
    WCHAR path[MAX_PATH];
    UINT n;

    if (g_real) return g_real;
    n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n + 20 >= MAX_PATH) return NULL;
    swprintf_s(path, MAX_PATH, L"%s\\version.dll", sys);
    g_real = LoadLibraryExW(path, NULL, 0);
    plog("proxy: real module %p (%ls)", (void*)g_real, path);
    return g_real;
}

static FARPROC api(const char* name) {
    HMODULE m = real_module();
    return m ? GetProcAddress(m, name) : NULL;
}

static DWORD WINAPI load_payload(LPVOID param) {
    WCHAR dir[MAX_PATH];
    WCHAR path[MAX_PATH];
    WCHAR exe[MAX_PATH];
    const WCHAR* base;
    DWORD n;
    HMODULE payload;

    Sleep(1200);
    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) { plog("proxy: exe path unavailable"); return 1; }
    base = exe;
    for (DWORD i = 0; i < n; ++i) if (exe[i] == L'\\') base = exe + i + 1;
    /* Unity ships CrashHandler/UnityCrashHandler64 next to the game; both load
     * VERSION.dll from this folder, and neither should host the payload. */
    if (_wcsicmp(base, L"ROB RUN.exe") != 0) {
        plog("proxy: skip pid=%lu exe=%ls", (unsigned long)GetCurrentProcessId(), base);
        return 1;
    }
    n = GetModuleFileNameW((HMODULE)param, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { plog("proxy: module path unavailable"); return 1; }
    while (n > 0 && dir[n - 1] != L'\\') --n;
    dir[n] = 0;
    swprintf_s(path, MAX_PATH, L"%srr_int.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        plog("proxy: payload absent at %ls", path);
        return 1;
    }
    payload = LoadLibraryW(path);
    plog("proxy: payload %p err=%lu path=%ls", (void*)payload,
         payload ? 0UL : GetLastError(), path);
    return 0;
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    HANDLE th;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(mod);
    InitializeCriticalSection(&g_cs);
    g_cs_ready = 1;
    plog("proxy: attach pid=%lu base=%p", (unsigned long)GetCurrentProcessId(), (void*)mod);
    th = CreateThread(NULL, 0, load_payload, (LPVOID)mod, 0, NULL);
    if (th) CloseHandle(th);
    else plog("proxy: loader thread failed (%lu)", GetLastError());
    return TRUE;
}

/* ---- forwards ------------------------------------------------------------ */

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
    typedef DWORD(WINAPI* F)(LPCSTR, LPDWORD);
    F f = (F)api("GetFileVersionInfoSizeA");
    return f ? f(a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
    typedef DWORD(WINAPI* F)(LPCWSTR, LPDWORD);
    F f = (F)api("GetFileVersionInfoSizeW");
    return f ? f(a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExA(DWORD fl, LPCSTR a, LPDWORD b) {
    typedef DWORD(WINAPI* F)(DWORD, LPCSTR, LPDWORD);
    F f = (F)api("GetFileVersionInfoSizeExA");
    return f ? f(fl, a, b) : 0;
}

__declspec(dllexport) DWORD WINAPI GetFileVersionInfoSizeExW(DWORD fl, LPCWSTR a, LPDWORD b) {
    typedef DWORD(WINAPI* F)(DWORD, LPCWSTR, LPDWORD);
    F f = (F)api("GetFileVersionInfoSizeExW");
    return f ? f(fl, a, b) : 0;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    typedef BOOL(WINAPI* F)(LPCSTR, DWORD, DWORD, LPVOID);
    F f = (F)api("GetFileVersionInfoA");
    return f ? f(a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    typedef BOOL(WINAPI* F)(LPCWSTR, DWORD, DWORD, LPVOID);
    F f = (F)api("GetFileVersionInfoW");
    return f ? f(a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExA(DWORD fl, LPCSTR a, DWORD b, DWORD c, LPVOID d) {
    typedef BOOL(WINAPI* F)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    F f = (F)api("GetFileVersionInfoExA");
    return f ? f(fl, a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoExW(DWORD fl, LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
    typedef BOOL(WINAPI* F)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    F f = (F)api("GetFileVersionInfoExW");
    return f ? f(fl, a, b, c, d) : FALSE;
}

__declspec(dllexport) BOOL WINAPI GetFileVersionInfoByHandle(DWORD fl, HANDLE h, LPVOID out) {
    typedef BOOL(WINAPI* F)(DWORD, HANDLE, LPVOID);
    F f = (F)api("GetFileVersionInfoByHandle");
    return f ? f(fl, h, out) : FALSE;
}

__declspec(dllexport) DWORD WINAPI VerFindFileA(DWORD fl, LPSTR a, LPSTR b, LPSTR c,
                                                LPSTR cur, PUINT curlen, LPSTR dst, PUINT dstlen) {
    typedef DWORD(WINAPI* F)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, PUINT, LPSTR, PUINT);
    F f = (F)api("VerFindFileA");
    return f ? f(fl, a, b, c, cur, curlen, dst, dstlen) : (DWORD)0;
}

__declspec(dllexport) DWORD WINAPI VerFindFileW(DWORD fl, LPWSTR a, LPWSTR b, LPWSTR c,
                                                LPWSTR cur, PUINT curlen, LPWSTR dst, PUINT dstlen) {
    typedef DWORD(WINAPI* F)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    F f = (F)api("VerFindFileW");
    return f ? f(fl, a, b, c, cur, curlen, dst, dstlen) : (DWORD)0;
}

__declspec(dllexport) DWORD WINAPI VerInstallFileA(DWORD fl, LPSTR src, LPSTR dst, LPSTR sd,
                                                   LPSTR dd, LPSTR tmp, PUINT tmplen) {
    typedef DWORD(WINAPI* F)(DWORD, LPSTR, LPSTR, LPSTR, LPSTR, LPSTR, PUINT);
    F f = (F)api("VerInstallFileA");
    return f ? f(fl, src, dst, sd, dd, tmp, tmplen) : (DWORD)0;
}

__declspec(dllexport) DWORD WINAPI VerInstallFileW(DWORD fl, LPWSTR src, LPWSTR dst, LPWSTR sd,
                                                   LPWSTR dd, LPWSTR tmp, PUINT tmplen) {
    typedef DWORD(WINAPI* F)(DWORD, LPWSTR, LPWSTR, LPWSTR, LPWSTR, LPWSTR, PUINT);
    F f = (F)api("VerInstallFileW");
    return f ? f(fl, src, dst, sd, dd, tmp, tmplen) : (DWORD)0;
}

__declspec(dllexport) DWORD WINAPI VerLanguageNameA(DWORD lang, LPSTR buf, DWORD cch) {
    typedef DWORD(WINAPI* F)(DWORD, LPSTR, DWORD);
    F f = (F)api("VerLanguageNameA");
    return f ? f(lang, buf, cch) : (DWORD)0;
}

__declspec(dllexport) DWORD WINAPI VerLanguageNameW(DWORD lang, LPWSTR buf, DWORD cch) {
    typedef DWORD(WINAPI* F)(DWORD, LPWSTR, DWORD);
    F f = (F)api("VerLanguageNameW");
    return f ? f(lang, buf, cch) : (DWORD)0;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID* buf, PUINT len) {
    typedef BOOL(WINAPI* F)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    F f = (F)api("VerQueryValueA");
    return f ? f(block, sub, buf, len) : FALSE;
}

__declspec(dllexport) BOOL WINAPI VerQueryValueW(LPCVOID block, LPCWSTR sub, LPVOID* buf, PUINT len) {
    typedef BOOL(WINAPI* F)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    F f = (F)api("VerQueryValueW");
    return f ? f(block, sub, buf, len) : FALSE;
}
