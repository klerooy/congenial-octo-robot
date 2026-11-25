/* Input gate - polling only. No thread, no hook.
 *
 * Two hard constraints in this payload, both documented in rr_int.cpp:
 *   1. A new thread inside the protected process is fatal. _beginthreadex here
 *      killed the game on the first presented frame.
 *   2. The image is hand-mapped and has no loader entry, so a hook callback
 *      address has no owning module. Windows walks the hook chain from other
 *      threads' contexts and faults on such an address.
 *
 * So there is no SetWindowsHookEx and no worker thread. Instead, while the
 * panel is open, the render thread neutralises held locomotion keys once per
 * frame by posting their key-up. The game sees the keys released; the player
 * stops. Mouse buttons, raw input and every UI key are never touched, so the
 * shop, the map and the location select keep working.
 *
 * This does not block a key that is pressed and released inside one frame, and
 * it does not stop the view from turning with the mouse. Both are acceptable:
 * the panel is operated with the mouse anyway, and the character no longer
 * walks around under it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rr_input_gate.h"

static HWND g_hwnd;
static volatile LONG g_open;
static int g_installed;

/* Locomotion / stance / weapon keys. Deliberately excludes ESC, TAB, ENTER,
 * arrows, F-keys and every mouse button: those drive the game's own menus. */
static const WPARAM g_keys[] = {
    'W','A','S','D','Q','E','R','F','C','Z','X','V','G','B','T','Y',
    '1','2','3','4','5','6','7','8','9','0',
    VK_SPACE, VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
    VK_CONTROL, VK_LCONTROL, VK_RCONTROL
};

int rr_gate_install(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return 0;
    g_hwnd = hwnd;
    g_installed = 1;
    return 1;
}

void rr_gate_remove(void) {
    InterlockedExchange(&g_open, 0);
    g_hwnd = NULL;
    g_installed = 0;
}

void rr_gate_set_open(int open) {
    InterlockedExchange(&g_open, open ? 1 : 0);
}

int rr_gate_is_open(void) {
    return InterlockedCompareExchange(&g_open, 0, 0) != 0;
}

/* Called every presented frame from the render thread. Releases any locomotion
 * key the game still considers held, but only while the panel is open and only
 * when the game window is foreground. Posted, never sent: the render thread
 * must not block inside the game's message pump. */
void rr_gate_pump(void) {
    if (!g_installed || !g_hwnd || !IsWindow(g_hwnd)) return;
    if (!InterlockedCompareExchange(&g_open, 0, 0)) return;
    if (GetForegroundWindow() != g_hwnd) return;

    for (unsigned i = 0; i < sizeof(g_keys)/sizeof(g_keys[0]); ++i) {
        const WPARAM vk = g_keys[i];
        /* Only release what is actually down, so we do not spam the queue with
         * thirty messages a frame for keys nobody pressed. */
        if (GetAsyncKeyState((int)vk) & 0x8000)
            PostMessageW(g_hwnd, WM_KEYUP, vk, 0xC0000001u);
    }
}