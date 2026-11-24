/* Low-level input gate.
 *
 * Why not a WndProc subclass: Unity reads the pointer through raw input and the
 * keyboard through GetAsyncKeyState / GetKeyboardState. Neither goes through the
 * window message queue in a way a subclass can filter, so swallowing WM_KEYDOWN
 * left the character walking, while swallowing WM_INPUT broke every in-game
 * button (shop, map, location select). WH_MOUSE_LL and WH_KEYBOARD_LL sit ahead
 * of both paths, so they are the only place where the block actually holds.
 *
 * What is blocked while the cheat panel is open:
 *   - mouse movement (so the view does not spin under the panel)
 *   - the locomotion / stance / weapon keys
 * What always passes:
 *   - every mouse button and the wheel, so the game's own UI stays clickable
 *   - ESC, TAB, ENTER, arrows, F-keys, so game menus keep working
 *
 * The hooks live on their own thread with a real message pump. A low-level hook
 * is removed by Windows if its callback stalls, and the render thread cannot
 * guarantee that during a discovery pass. This thread does nothing but pump.
 *
 * Note on the anti-cheat: rr_int.cpp deliberately owns no thread because a new
 * thread inside the protected process is fatal. This module therefore installs
 * its hooks ONLY when rr_gate_install is called explicitly, and the caller in
 * rr_int.cpp does that from the first presented frame - long after attach, on a
 * process that has already survived its own startup checks. If the game kills
 * the process on CreateThread here, set RR_GATE_NO_THREAD below to 1 and the
 * module falls back to installing on the calling thread; the panel then must be
 * pumped every frame, which rr_gate_pump already does.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include "rr_input_gate.h"

#define RR_GATE_NO_THREAD 0

static HHOOK   g_mouse_hook;
static HHOOK   g_key_hook;
static HWND    g_gate_hwnd;
static volatile LONG g_open;
static volatile LONG g_installed;
static HANDLE  g_thread;
static DWORD   g_thread_id;
static volatile LONG g_quit;

/* Injected by our own SendInput: never block ourselves. LLKHF_INJECTED alone is
 * not enough (other tools inject too), so the aim path tags its events with a
 * private dwExtraInfo value. */
#define RR_GATE_MAGIC ((ULONG_PTR)0x52524741u)   /* 'RRGA' */

static int gate_key(DWORD vk) {
    switch (vk) {
    case 'W': case 'A': case 'S': case 'D':
    case 'Q': case 'E': case 'R': case 'F': case 'C': case 'Z': case 'X':
    case 'V': case 'G': case 'B': case 'T': case 'Y':
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': case '0':
    case VK_SPACE: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_LBUTTON: case VK_RBUTTON:
        return 1;
    default:
        return 0;
    }
}

static int gate_foreground(void) {
    /* Only gate the game window. If the user alt-tabbed, the panel is not
     * visible and nothing should be swallowed. */
    if (!g_gate_hwnd) return 0;
    return GetForegroundWindow() == g_gate_hwnd;
}

static LRESULT CALLBACK mouse_ll(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && InterlockedCompareExchange(&g_open, 0, 0) &&
        gate_foreground()) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)l;
        if (m->dwExtraInfo != RR_GATE_MAGIC && w == WM_MOUSEMOVE)
            return 1;   /* block view rotation only; buttons pass */
    }
    return CallNextHookEx(NULL, code, w, l);
}

static LRESULT CALLBACK key_ll(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && InterlockedCompareExchange(&g_open, 0, 0) &&
        gate_foreground()) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)l;
        if (k->dwExtraInfo != RR_GATE_MAGIC && gate_key(k->vkCode))
            return 1;
    }
    return CallNextHookEx(NULL, code, w, l);
}

static int install_hooks(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_ll, inst, 0);
    g_key_hook   = SetWindowsHookExW(WH_KEYBOARD_LL, key_ll, inst, 0);
    return g_mouse_hook && g_key_hook;
}

static void remove_hooks(void) {
    if (g_mouse_hook) { UnhookWindowsHookEx(g_mouse_hook); g_mouse_hook = NULL; }
    if (g_key_hook)   { UnhookWindowsHookEx(g_key_hook);   g_key_hook   = NULL; }
}

static unsigned WINAPI gate_thread(void* arg) {
    MSG msg;
    (void)arg;
    /* Force the thread to have a message queue before the hooks go in. */
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    if (!install_hooks()) {
        remove_hooks();
        InterlockedExchange(&g_installed, 0);
        return 0;
    }
    InterlockedExchange(&g_installed, 1);
    while (!InterlockedCompareExchange(&g_quit, 0, 0)) {
        const DWORD r = MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    InterlockedExchange(&g_quit, 1);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    remove_hooks();
    InterlockedExchange(&g_installed, 0);
    return 0;
}

int rr_gate_install(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return 0;
    g_gate_hwnd = hwnd;
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 1;

#if RR_GATE_NO_THREAD
    if (!install_hooks()) {
        remove_hooks();
        InterlockedExchange(&g_installed, 0);
        return 0;
    }
    return 1;
#else
    g_thread = (HANDLE)_beginthreadex(NULL, 0, gate_thread, NULL, 0,
                                      (unsigned*)&g_thread_id);
    if (!g_thread) {
        InterlockedExchange(&g_installed, 0);
        return 0;
    }
    return 1;
#endif
}

void rr_gate_pump(void) {
#if RR_GATE_NO_THREAD
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#else
    (void)0;
#endif
}

void rr_gate_remove(void) {
    InterlockedExchange(&g_open, 0);
#if RR_GATE_NO_THREAD
    remove_hooks();
    InterlockedExchange(&g_installed, 0);
#else
    if (g_thread) {
        InterlockedExchange(&g_quit, 1);
        if (g_thread_id) PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
        g_thread_id = 0;
    }
    InterlockedExchange(&g_installed, 0);
#endif
    g_gate_hwnd = NULL;
}

void rr_gate_set_open(int open) {
    InterlockedExchange(&g_open, open ? 1 : 0);
}

int rr_gate_is_open(void) {
    return InterlockedCompareExchange(&g_open, 0, 0) != 0;
}