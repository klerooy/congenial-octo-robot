/* rrtool_exe.c - RRTool.exe, the double-click entry for the packaged build.
 *
 * It owns no logic of its own: the chain lives in RRTool.ps1 and stays readable
 * there. This program only does the two things a .bat cannot do quietly:
 *
 *   1. ask for admin before anything runs, so the console never sits on a
 *      "not elevated" refusal the user has to answer with a second launch
 *   2. start RRTool.ps1 -Auto, which leaves what is already up alone, launches
 *      the game when it is not, waits for a scene, maps once and proves frames
 *
 * Built with /MT so the friend build needs no runtime installed.
 *
 * Exit code is RRTool.ps1's own: 0 ok, 1 not elevated, 2 layout, 3 service,
 * 4 byovd, 5 channel, 6 game, 7 map, 8 machine preflight.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_CMD 16384

static int is_elevated(void) {
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD got = 0;
    int elevated = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return 0;
    memset(&el, 0, sizeof el);
    if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &got)) {
        elevated = el.TokenIsElevated ? 1 : 0;
    }
    CloseHandle(tok);
    return elevated;
}

static int own_dir(wchar_t* out, size_t cap) {
    DWORD n = (DWORD)cap;
    wchar_t* slash;

    if (!GetModuleFileNameW(NULL, out, n)) return 0;
    slash = wcsrchr(out, L'\\');
    if (!slash) return 0;
    *slash = L'\0';
    return 1;
}

/* -Auto unless the caller already picked a mode. -NoPause is consumed here. */
static int wants_auto(int argc, wchar_t** argv, int* no_pause) {
    int mode = 0;
    int i;

    *no_pause = 0;
    for (i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"-NoPause")) {
            *no_pause = 1;
            continue;
        }
        if (!_wcsicmp(argv[i], L"-Auto") || !_wcsicmp(argv[i], L"-MapOnly") ||
            !_wcsicmp(argv[i], L"-NoGame") || !_wcsicmp(argv[i], L"-Install") ||
            !_wcsicmp(argv[i], L"-Uninstall") || !_wcsicmp(argv[i], L"-Check")) {
            mode = 1;
        }
    }
    return mode ? 0 : 1;
}

static void pause_if_console(void) {
    HANDLE in;
    DWORD mode = 0;
    wchar_t line[8];

    in = GetStdHandle(STD_INPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) return;
    wprintf(L"\n  press Enter to close\n");
    fflush(stdout);
    fgetws(line, 8, stdin);
}

int wmain(int argc, wchar_t** argv) {
    wchar_t dir[MAX_PATH];
    wchar_t script[MAX_PATH + 32];
    wchar_t powershell[MAX_PATH];
    wchar_t cmd[MAX_CMD];
    wchar_t tail[MAX_CMD];
    wchar_t self[MAX_PATH];
    int no_pause = 0;
    int auto_mode;
    size_t used;
    int i;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    UINT sys;

    if (!own_dir(dir, MAX_PATH)) {
        wprintf(L"RRTool: cannot resolve the folder this program is in\n");
        return 1;
    }
    _snwprintf_s(script, MAX_PATH + 32, _TRUNCATE, L"%s\\RRTool.ps1", dir);
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"RRTool: RRTool.ps1 is missing next to this program (%s)\n", script);
        pause_if_console();
        return 2;
    }

    sys = GetSystemDirectoryW(powershell, MAX_PATH - 36);
    if (!sys || sys >= MAX_PATH - 36) {
        wprintf(L"RRTool: cannot resolve the system folder\n");
        return 2;
    }
    wcscat_s(powershell, MAX_PATH, L"\\WindowsPowerShell\\v1.0\\powershell.exe");

    auto_mode = wants_auto(argc, argv, &no_pause);

    if (!is_elevated()) {
        if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return 1;
        wprintf(L"RRTool: asking for administrator once (the kernel services need it)\n");
        fflush(stdout);
        tail[0] = L'\0';
        used = 0;
        for (i = 1; i < argc && used < MAX_CMD - 64; ++i) {
            int n = _snwprintf_s(tail + used, MAX_CMD - used, _TRUNCATE, L" %s", argv[i]);
            if (n <= 0) break;
            used += (size_t)n;
        }
        if (auto_mode) wcscat_s(tail, MAX_CMD, L" -Auto");
        if ((INT_PTR)ShellExecuteW(NULL, L"runas", self, tail, dir, SW_SHOWNORMAL) <= 32) {
            wprintf(L"RRTool: the elevation prompt was refused, nothing was started\n");
            pause_if_console();
            return 1;
        }
        return 0;
    }

    _snwprintf_s(cmd, MAX_CMD, _TRUNCATE,
                 L"\"%s\" -NoProfile -ExecutionPolicy Bypass -File \"%s\"%s",
                 powershell, script, auto_mode ? L" -Auto" : L"");
    used = wcslen(cmd);
    for (i = 1; i < argc && used < MAX_CMD - 260; ++i) {
        if (!_wcsicmp(argv[i], L"-NoPause")) continue;
        if (auto_mode && !_wcsicmp(argv[i], L"-Auto")) continue;   /* already added */
        _snwprintf_s(cmd + used, MAX_CMD - used, _TRUNCATE, L" \"%s\"", argv[i]);
        used = wcslen(cmd);
    }

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(powershell, cmd, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi)) {
        wprintf(L"RRTool: cannot start PowerShell (error %lu)\n", GetLastError());
        pause_if_console();
        return 2;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (code == 0) {
        wprintf(L"\nRRTool: done.\n");
    } else {
        wprintf(L"\nRRTool: stopped on exit code %lu (see the lines above)\n",
                (unsigned long)code);
    }
    if (!no_pause) pause_if_console();
    return (int)code;
}
