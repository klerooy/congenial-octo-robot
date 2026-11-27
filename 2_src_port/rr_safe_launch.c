/* rr_safe_launch.c - driverless launcher for the internal payload.
 *
 * Starts ROB RUN.exe suspended, waits until the target loader has published
 * kernel32 in its PEB, maps rr_int.dll with rr_safemap.c (no LoadLibrary in the
 * target, no driver, no APC), then resumes the game. The payload is inside the
 * process before the game's entry point runs, so no anti-cheat-protected handle
 * is needed and no WmPort device is opened at any point.
 *
 * The mapper still creates one short-lived thread through ntdll to run the
 * payload entry point; it is created before the game thread starts. There is no
 * CreateRemoteThread call, no module-list entry and no file placed in the game
 * folder.
 *
 * Log: C:\Users\Public\rr_safe.log
 *
 * Usage:
 *   RRSafe.exe                     find the Steam install and launch it
 *   RRSafe.exe --game <exe>        launch a specific ROB RUN.exe
 *   RRSafe.exe --dll <dll>         payload path (default: rr_int.dll beside this exe)
 *   RRSafe.exe --attach            map into an already-running game
 *   RRSafe.exe --find              print the discovered game path and exit
 *   RRSafe.exe --suspend           create the game suspended (loader may not be ready)
 *   RRSafe.exe --no-suspend        start the game running, then map (default)
 *   RRSafe.exe --delay <ms>        wait after loader readiness before mapping
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "rr_safemap.h"

#define LOG_PATH   "C:\\Users\\Public\\rr_safe.log"
#define GAME_EXE   L"ROB RUN.exe"
#define GAME_REL   L"steamapps\\common\\Rob Run\\ROB RUN.exe"

static FILE* g_log;
static CRITICAL_SECTION g_cs;
static int g_cs_ready;

static void slog(const wchar_t* fmt, ...) {
    va_list ap;
    if (g_cs_ready) EnterCriticalSection(&g_cs);
    if (g_log) {
        va_start(ap, fmt);
        vfwprintf(g_log, fmt, ap);
        va_end(ap);
        fputwc(L'\n', g_log);
        fflush(g_log);
    }
    va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    wprintf(L"\n");
    if (g_cs_ready) LeaveCriticalSection(&g_cs);
}

static int file_exists(const wchar_t* path) {
    DWORD a;
    if (!path || !*path) return 0;
    a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const wchar_t* path) {
    DWORD a;
    if (!path || !*path) return 0;
    a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void parent_dir(wchar_t* path) {
    wchar_t* slash;
    if (!path) return;
    slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
}

static int exe_dir(wchar_t* out, size_t cap) {
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (!n || n >= cap) return 0;
    parent_dir(out);
    return out[0] != 0;
}

/* Read the next quoted token from a VDF line: the text between the first and
 * second quote after `from`. Returns a pointer past the closing quote, or NULL. */
static const wchar_t* vdf_value(const wchar_t* from, wchar_t* out, size_t cap) {
    const wchar_t* a;
    const wchar_t* b;
    size_t n;
    if (!from || !out || cap < 2) return NULL;
    a = wcschr(from, L'"');
    if (!a) return NULL;
    ++a;
    b = wcschr(a, L'"');
    if (!b) return NULL;
    n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    wmemcpy(out, a, n);
    out[n] = 0;
    return b + 1;
}

static int check_root(const wchar_t* root, wchar_t* out, size_t cap) {
    wchar_t candidate[MAX_PATH * 2];
    if (!root || !*root) return 0;
    swprintf_s(candidate, _countof(candidate), L"%s\\%s", root, GAME_REL);
    if (file_exists(candidate)) {
        wcsncpy_s(out, cap, candidate, _TRUNCATE);
        return 1;
    }
    return 0;
}

/* Walk <root>\steamapps\libraryfolders.vdf for additional library paths. */
static int check_libraries(const wchar_t* root, wchar_t* out, size_t cap) {
    wchar_t vdf[MAX_PATH * 2];
    FILE* f;
    wchar_t line[2048];
    int found = 0;

    if (!root || !*root) return 0;
    swprintf_s(vdf, _countof(vdf), L"%s\\steamapps\\libraryfolders.vdf", root);
    f = _wfopen(vdf, L"r");
    if (!f) {
        slog(L"safe: no library file at %s", vdf);
        return 0;
    }

    while (!found && fgetws(line, _countof(line), f)) {
        const wchar_t* p = wcsstr(line, L"\"path\"");
        wchar_t path[MAX_PATH * 2];
        if (!p) continue;
        p += 6;
        if (!vdf_value(p, path, _countof(path))) continue;
        /* VDF writes backslashes doubled; collapse them before building a path. */
        {
            wchar_t* s = path;
            wchar_t* d = path;
            while (*s) {
                if (s[0] == L'\\' && s[1] == L'\\') { *d++ = L'\\'; s += 2; }
                else *d++ = *s++;
            }
            *d = 0;
        }
        slog(L"safe: library %s", path);
        if (check_root(path, out, cap)) found = 1;
    }
    fclose(f);
    return found;
}

static int find_game(wchar_t* out, size_t cap) {
    static const wchar_t* roots[] = {
        L"C:\\Program Files (x86)\\Steam",
        L"C:\\Program Files\\Steam",
        L"C:\\Steam",
        L"D:\\Steam",
        L"E:\\Steam"
    };
    wchar_t env[MAX_PATH * 2];
    DWORD n;
    HKEY hk;
    wchar_t steam[MAX_PATH * 2];

    n = GetEnvironmentVariableW(L"RR_GAME", env, _countof(env));
    if (n && n < _countof(env) && file_exists(env)) {
        wcsncpy_s(out, cap, env, _TRUNCATE);
        return 1;
    }

    for (size_t i = 0; i < _countof(roots); ++i)
        if (check_root(roots[i], out, cap) || check_libraries(roots[i], out, cap))
            return 1;

    /* SteamPath from the per-user key, then the machine-wide install path. */
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &hk) ==
        ERROR_SUCCESS) {
        DWORD sz = sizeof(steam), ty = 0;
        if (RegQueryValueExW(hk, L"SteamPath", NULL, &ty, (LPBYTE)steam, &sz) == ERROR_SUCCESS) {
            RegCloseKey(hk);
            if (check_root(steam, out, cap) || check_libraries(steam, out, cap)) return 1;
        } else {
            RegCloseKey(hk);
        }
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", 0,
                      KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD sz = sizeof(steam), ty = 0;
        if (RegQueryValueExW(hk, L"InstallPath", NULL, &ty, (LPBYTE)steam, &sz) ==
            ERROR_SUCCESS) {
            RegCloseKey(hk);
            if (check_root(steam, out, cap) || check_libraries(steam, out, cap)) return 1;
        } else {
            RegCloseKey(hk);
        }
    }
    return 0;
}

static DWORD find_game_pid(void) {
    HANDLE snap;
    PROCESSENTRY32W e;
    DWORD pid = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&e, 0, sizeof(e));
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, GAME_EXE) == 0) {
                pid = e.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return pid;
}

/* Wait until the target loader list names kernel32. The mapper needs that list
 * to bind imports; before it exists there is nothing safe to write. */
static int wait_loader(HANDLE proc, DWORD timeout_ms, int* exited) {
    ULONGLONG start = GetTickCount64();
    if (exited) *exited = 0;
    while (GetTickCount64() - start < timeout_ms) {
        uint64_t peb;
        uint64_t k32 = 0;
        if (WaitForSingleObject(proc, 0) == WAIT_OBJECT_0) {
            if (exited) *exited = 1;
            return 0;
        }
        peb = rr_safemap_peb(proc);
        if (peb && rr_safemap_module(proc, peb, "kernel32.dll", &k32) && k32)
            return 1;
        Sleep(5);
    }
    return 0;
}

static int wide_to_ansi(const wchar_t* in, char* out, size_t cap) {
    int n;
    if (!in || !out || cap == 0) return 0;
    n = WideCharToMultiByte(CP_ACP, 0, in, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

static int map_payload(DWORD pid, const wchar_t* dll, unsigned delay_ms) {
    HANDLE proc;
    RR_SAFEMAP_RESULT res;
    char err[512];
    char ansi[MAX_PATH * 2];
    int ready, exited = 0;

    proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                       FALSE, pid);
    if (!proc) {
        slog(L"safe: OpenProcess(%lu) for readiness failed (Win32 %lu)",
             (unsigned long)pid, (unsigned long)GetLastError());
        return 0;
    }
    ready = wait_loader(proc, 15000, &exited);
    CloseHandle(proc);
    if (!ready) {
        slog(L"safe: pid %lu %s before its loader was ready", (unsigned long)pid,
             exited ? L"exited" : L"timed out");
        return 0;
    }
    if (delay_ms) {
        slog(L"safe: loader ready, waiting %u ms before mapping", delay_ms);
        Sleep(delay_ms);
    }
    if (!wide_to_ansi(dll, ansi, sizeof(ansi))) {
        slog(L"safe: cannot convert the payload path");
        return 0;
    }

    memset(&res, 0, sizeof(res));
    err[0] = 0;
    if (!rr_safemap_dll(pid, ansi, &res, err, sizeof(err))) {
        slog(L"safe: mapping failed: %hs", err[0] ? err : "unknown error");
        return 0;
    }
    slog(L"safe: mapped pid=%lu base=%016llX size=%X entry-thread=%lu",
         (unsigned long)pid, (unsigned long long)res.base, res.size,
         (unsigned long)res.thread_id);
    return 1;
}

static int attach_mode(const wchar_t* dll, unsigned delay_ms) {
    DWORD pid = find_game_pid();
    if (!pid) {
        slog(L"safe: %s is not running", GAME_EXE);
        return 2;
    }
    slog(L"safe: attaching to running pid %lu", (unsigned long)pid);
    return map_payload(pid, dll, delay_ms) ? 0 : 4;
}

static int launch_mode(const wchar_t* game, const wchar_t* dll, unsigned delay_ms,
                       int suspend) {
    wchar_t dir[MAX_PATH * 2];
    wchar_t cmd[MAX_PATH * 2 + 8];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD flags = suspend ? CREATE_SUSPENDED : 0;
    int ok;

    wcsncpy_s(dir, _countof(dir), game, _TRUNCATE);
    parent_dir(dir);
    swprintf_s(cmd, _countof(cmd), L"\"%s\"", game);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(game, cmd, NULL, NULL, FALSE, flags, NULL,
                        dir_exists(dir) ? dir : NULL, &si, &pi)) {
        slog(L"safe: CreateProcessW failed (Win32 %lu)", (unsigned long)GetLastError());
        return 3;
    }
    slog(L"safe: started pid %lu (%s)", (unsigned long)pi.dwProcessId,
         suspend ? L"suspended" : L"running");

    ok = map_payload(pi.dwProcessId, dll, delay_ms);

    if (suspend) {
        DWORD r = ResumeThread(pi.hThread);
        slog(L"safe: ResumeThread -> %lu", (unsigned long)r);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    return ok ? 0 : 4;
}

static void usage(void) {
    wprintf(L"RRSafe - driverless ROB RUN payload loader\n"
            L"  --game <exe>     ROB RUN.exe (default: find the Steam install)\n"
            L"  --dll <dll>      payload (default: rr_int.dll beside RRSafe.exe)\n"
            L"  --attach         map into a game that is already running\n"
            L"  --find           print the discovered game path and exit\n"
            L"  --suspend        create the game suspended (loader may not be ready)\n"
            L"  --no-suspend     start the game running, then map (default)\n"
            L"  --delay <ms>     wait after the loader is ready (default 0)\n"
            L"Log: C:\\Users\\Public\\rr_safe.log\n");
}

int main(void) {
    wchar_t game[MAX_PATH * 2] = L"";
    wchar_t dll[MAX_PATH * 2] = L"";
    wchar_t self[MAX_PATH * 2];
    unsigned delay_ms = 0;
    int attach = 0;
    int suspend = 0;
    int find_only = 0;
    int rc;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (!argv) return 2;
    InitializeCriticalSection(&g_cs);
    g_cs_ready = 1;
    g_log = _wfopen(L"C:\\Users\\Public\\rr_safe.log", L"a");

    if (!exe_dir(self, _countof(self))) self[0] = 0;
    swprintf_s(dll, _countof(dll), L"%s\\rr_int.dll", self);

    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--help") || !_wcsicmp(argv[i], L"-h")) {
            usage();
            if (g_log) fclose(g_log);
            LocalFree(argv);
            return 0;
        } else if (!_wcsicmp(argv[i], L"--game") && i + 1 < argc) {
            wcsncpy_s(game, _countof(game), argv[++i], _TRUNCATE);
        } else if (!_wcsicmp(argv[i], L"--dll") && i + 1 < argc) {
            wcsncpy_s(dll, _countof(dll), argv[++i], _TRUNCATE);
        } else if (!_wcsicmp(argv[i], L"--delay") && i + 1 < argc) {
            delay_ms = (unsigned)_wtoi(argv[++i]);
        } else if (!_wcsicmp(argv[i], L"--attach")) {
            attach = 1;
        } else if (!_wcsicmp(argv[i], L"--find")) {
            find_only = 1;
        } else if (!_wcsicmp(argv[i], L"--no-suspend")) {
            suspend = 0;
        } else if (!_wcsicmp(argv[i], L"--suspend")) {
            suspend = 1;
        } else {
            slog(L"safe: unknown argument %s", argv[i]);
            usage();
            if (g_log) fclose(g_log);
            LocalFree(argv);
            return 2;
        }
    }

    if (find_only) {
        if (!game[0] && !find_game(game, _countof(game))) {
            wprintf(L"not found\n");
            slog(L"safe: ROB RUN.exe not found; pass --game <path> or set RR_GAME");
            if (g_log) fclose(g_log);
            LocalFree(argv);
            return 2;
        }
        wprintf(L"%s\n", game);
        slog(L"safe: found %s", game);
        if (g_log) fclose(g_log);
        LocalFree(argv);
        return file_exists(game) ? 0 : 2;
    }

    slog(L"==== RRSafe %s ====", attach ? L"attach" : L"launch");
    slog(L"safe: dll=%s", dll);
    if (!file_exists(dll)) {
        slog(L"safe: payload not found at %s", dll);
        if (g_log) fclose(g_log);
        LocalFree(argv);
        return 2;
    }

    if (attach) {
        rc = attach_mode(dll, delay_ms);
    } else {
        DWORD running = find_game_pid();
        if (running) {
            slog(L"safe: %s is already running (pid %lu); attaching",
                 GAME_EXE, (unsigned long)running);
            rc = map_payload(running, dll, delay_ms) ? 0 : 4;
        } else {
            if (!game[0] && !find_game(game, _countof(game))) {
                slog(L"safe: ROB RUN.exe not found; pass --game <path> or set RR_GAME");
                if (g_log) fclose(g_log);
                LocalFree(argv);
                return 2;
            }
            if (!file_exists(game)) {
                slog(L"safe: game not found at %s", game);
                if (g_log) fclose(g_log);
                LocalFree(argv);
                return 2;
            }
            slog(L"safe: game=%s", game);
            rc = launch_mode(game, dll, delay_ms, suspend);
        }
    }

    if (g_log) fclose(g_log);
    LocalFree(argv);
    return rc;
}
