/* Game-input gate. Only player locomotion and fire keys are swallowed while the
 * overlay menu is open; mouse messages and WM_INPUT always pass through, so the
 * game's own UI (shop, inventory, map) stays clickable. Unity reads pointer
 * input through raw input on some paths - blocking it broke every in-game
 * button, which is why the mouse is never filtered here. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rr_input_gate.h"

static HWND    g_gate_hwnd;
static WNDPROC g_gate_orig;
static volatile LONG g_gate_open;
static int     g_gate_installed;

/* Movement, stance, fire and interaction. Deliberately excludes ESC, TAB,
 * arrows, ENTER and the mouse: those drive the game's menus. */
static int gate_key(WPARAM vk) {
    switch (vk) {
    case 'W': case 'A': case 'S': case 'D':
    case 'Q': case 'E': case 'R': case 'F': case 'C': case 'Z': case 'X':
    case 'V': case 'G': case 'H': case 'B': case 'T': case 'Y':
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9': case '0':
    case VK_SPACE: case VK_SHIFT: case VK_CONTROL: case VK_LSHIFT:
    case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
        return 1;
    default:
        return 0;
    }
}

static LRESULT CALLBACK gate_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (InterlockedCompareExchange(&g_gate_open, 0, 0)) {
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN ||
             m == WM_KEYUP   || m == WM_SYSKEYUP) && gate_key(w))
            return 0;
        /* Characters produced by those keys must not reach a focused field. */
        if (m == WM_CHAR || m == WM_SYSCHAR || m == WM_DEADCHAR)
            return 0;
    }
    return CallWindowProcW(g_gate_orig, h, m, w, l);
}

int rr_gate_install(HWND hwnd) {
    WNDPROC prev;
    if (g_gate_installed) return 1;
    if (!hwnd || !IsWindow(hwnd)) return 0;
    SetLastError(0);
    prev = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)gate_proc);
    if (!prev && GetLastError()) return 0;
    g_gate_orig = prev;
    g_gate_hwnd = hwnd;
    g_gate_installed = 1;
    return 1;
}

void rr_gate_remove(void) {
    if (!g_gate_installed) return;
    InterlockedExchange(&g_gate_open, 0);
    if (g_gate_hwnd && IsWindow(g_gate_hwnd) && g_gate_orig)
        SetWindowLongPtrW(g_gate_hwnd, GWLP_WNDPROC, (LONG_PTR)g_gate_orig);
    g_gate_orig = NULL; g_gate_hwnd = NULL; g_gate_installed = 0;
}

void rr_gate_set_open(int open) {
    const int was = (int)InterlockedExchange(&g_gate_open, open ? 1 : 0);
    if (was && !open && g_gate_hwnd && IsWindow(g_gate_hwnd)) {
        /* Release movement keys the game may still consider held. Posted, not
         * sent: the render thread must not block in the game's pump. Mouse-up
         * is NOT posted - the player may be holding a real click on a shop
         * button at the moment the panel closes. */
        const WPARAM keys[] = { 'W','A','S','D','Q','E','R','F','C','Z','X',
                                VK_SPACE, VK_SHIFT, VK_CONTROL };
        for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
            PostMessageW(g_gate_hwnd, WM_KEYUP, keys[i], 0xC0000001u);
    }
}

int rr_gate_is_open(void) {
    return InterlockedCompareExchange(&g_gate_open, 0, 0) != 0;
}