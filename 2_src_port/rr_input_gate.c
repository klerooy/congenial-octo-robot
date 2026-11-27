/* Input gate - in-process window filter for the open panel.
 *
 * Two hard constraints in this payload, both documented in rr_int.cpp:
 *   1. A new thread inside the protected process is fatal. This file owns no
 *      thread; everything runs from the render thread or from the game's own
 *      window thread.
 *   2. The image is hand-mapped and has no loader entry, so a global
 *      SetWindowsHookEx callback has no owning module and other processes'
 *      hook walks fault on it. No global hook is installed here.
 *
 * SetWindowLongPtr(GWLP_WNDPROC) is safe under both rules: the pointer is
 * called only inside this process, on the thread that owns the game window.
 * The subclass exists only while the panel is open, so normal play leaves the
 * game's own procedure untouched. While it is installed the filter drops
 * mouse, raw-input and keyboard presses before the game's procedure sees
 * them, so the camera does not turn and the character does not act under the
 * menu. The panel reads GetAsyncKeyState/GetCursorPos directly and is
 * unaffected by the filter.
 *
 * A key or button held when the panel opens was already delivered, so the
 * game still believes it is down. On the open edge the gate posts one release
 * for every held key/button and lets exactly those releases through; every
 * other up-message is swallowed because its down never reached the game. This
 * keeps the character from walking away under the panel without leaking menu
 * clicks into the game.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rr_input_gate.h"

static HWND g_hwnd;
static volatile LONG g_open;
static int g_installed;

/* The swapchain window and, when it is a child, its top-level owner. */
#define GATE_MAX_WINDOWS 4
static HWND    g_sub_hwnd[GATE_MAX_WINDOWS];
static WNDPROC g_sub_orig[GATE_MAX_WINDOWS];

/* Keys/buttons released on the open edge. A release message may pass only
 * while its bit is set; taking the bit clears it, so a duplicate posted
 * release or the real release that follows is swallowed. 256 bits, one per
 * virtual-key code. */
static volatile LONG g_release[8];

/* Set once the held keys for this open have been released; keeps the per-frame
 * install from posting them again. Render thread only. */
static int g_armed;

/* Every key the game could bind: movement, camera, weapons, chat and the menu
 * keys the panel itself uses. Releasing these while the panel is up keeps the
 * character and the view still. */
static const WPARAM g_keys[] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '0','1','2','3','4','5','6','7','8','9',
    VK_SPACE, VK_RETURN, VK_TAB, VK_ESCAPE, VK_BACK,
    VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
    VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
    VK_MENU, VK_LMENU, VK_RMENU,
    VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
    VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_INSERT, VK_DELETE,
    VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
    VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12
};

static void gate_rel_set(int vk) {
    if (vk < 0 || vk > 255) return;
    InterlockedOr(&g_release[vk >> 5], (LONG)(1UL << (vk & 31)));
}

static int gate_rel_take(int vk) {
    volatile LONG* word;
    LONG bit, old;
    if (vk < 0 || vk > 255) return 0;
    word = &g_release[vk >> 5];
    bit = (LONG)(1UL << (vk & 31));
    for (;;) {
        old = InterlockedCompareExchange(word, 0, 0);
        if (!(old & bit)) return 0;
        if (InterlockedCompareExchange(word, old & ~bit, old) == old) return 1;
    }
}

static void gate_rel_clear(void) {
    int i;
    for (i = 0; i < 8; ++i) InterlockedExchange(&g_release[i], 0);
}

/* Virtual key carried by a release message, or -1 when the message is not a
 * release. Only these messages are ever let through while the panel is up. */
static int gate_release_vk(UINT msg, WPARAM wp) {
    switch (msg) {
    case WM_KEYUP: case WM_SYSKEYUP:
        return (int)wp;
    case WM_LBUTTONUP: case WM_NCLBUTTONUP: return VK_LBUTTON;
    case WM_RBUTTONUP: case WM_NCRBUTTONUP: return VK_RBUTTON;
    case WM_MBUTTONUP: case WM_NCMBUTTONUP: return VK_MBUTTON;
    case WM_XBUTTONUP: case WM_NCXBUTTONUP:
        return (HIWORD(wp) == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
    default:
        return -1;
    }
}

/* Messages that must not reach the game while the panel is up. Releases are
 * not listed here: they are handled separately so the held-key release can
 * pass while menu clicks cannot. */
static int gate_blocks(UINT msg) {
    switch (msg) {
    case WM_INPUT:
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
    case WM_LBUTTONDOWN:  case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:  case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:   case WM_MOUSEHWHEEL:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
    case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
    case WM_KEYDOWN:      case WM_SYSKEYDOWN:
    case WM_CHAR:         case WM_SYSCHAR:
    case WM_DEADCHAR:     case WM_SYSDEADCHAR:
    case WM_UNICHAR:
    case WM_HOTKEY:
    case WM_APPCOMMAND:
        return 1;
    default:
        return 0;
    }
}

static WNDPROC gate_orig_for(HWND hwnd) {
    int i;
    for (i = 0; i < GATE_MAX_WINDOWS; ++i)
        if (g_sub_hwnd[i] == hwnd) return g_sub_orig[i];
    return NULL;
}

static void gate_forget(HWND hwnd) {
    int i;
    for (i = 0; i < GATE_MAX_WINDOWS; ++i) {
        if (g_sub_hwnd[i] != hwnd) continue;
        g_sub_hwnd[i] = NULL;
        g_sub_orig[i] = NULL;
    }
}

static LRESULT CALLBACK gate_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC orig = gate_orig_for(hwnd);
    if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == WM_NCDESTROY) {
        LRESULT r = CallWindowProcW(orig, hwnd, msg, wp, lp);
        gate_forget(hwnd);
        return r;
    }

    if (InterlockedCompareExchange(&g_open, 0, 0)) {
        if (msg == WM_SETCURSOR) return TRUE;
        {
            const int vk = gate_release_vk(msg, wp);
            if (vk >= 0) {
                if (gate_rel_take(vk)) return CallWindowProcW(orig, hwnd, msg, wp, lp);
                return 0;
            }
        }
        if (gate_blocks(msg)) {
            /* A WM_INPUT message owns a raw buffer; DefWindowProc is what frees
             * it. Calling it also keeps the raw queue from filling up while the
             * game never reads the packet. */
            if (msg == WM_INPUT) return DefWindowProcW(hwnd, msg, wp, lp);
            return 0;
        }
    }
    return CallWindowProcW(orig, hwnd, msg, wp, lp);
}

static void gate_subclass(HWND hwnd) {
    WNDPROC old;
    int i, slot = -1;
    if (!hwnd || !IsWindow(hwnd)) return;
    for (i = 0; i < GATE_MAX_WINDOWS; ++i) {
        if (g_sub_hwnd[i] == hwnd) return;
        if (!g_sub_hwnd[i] || !IsWindow(g_sub_hwnd[i])) slot = i;
    }
    if (slot < 0) return;
    old = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)gate_wndproc);
    if (!old || old == gate_wndproc) return;
    g_sub_hwnd[slot] = hwnd;
    g_sub_orig[slot] = old;
}

/* Put the game's original procedure back. Called on close and on unload, so
 * the subclass is not present during ordinary play. If something wrapped our
 * procedure in the meantime the slot is kept: gate_wndproc may still be in
 * that chain and must know what to forward to. */
static void gate_restore(void) {
    int i;
    for (i = 0; i < GATE_MAX_WINDOWS; ++i) {
        HWND h = g_sub_hwnd[i];
        if (!h) continue;
        if (!IsWindow(h)) {
            g_sub_hwnd[i] = NULL;
            g_sub_orig[i] = NULL;
            continue;
        }
        if ((WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) != gate_wndproc)
            continue;
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)g_sub_orig[i]);
        g_sub_hwnd[i] = NULL;
        g_sub_orig[i] = NULL;
    }
}

/* Post one release for every key/button that is physically down right now and
 * let the filter pass it. Returns 0 when there is no window to post to yet, so
 * the install pass can retry once the swapchain reports its window. */
static int gate_release_held(void) {
    unsigned i;
    HWND kb, ms;
    if (!g_hwnd || !IsWindow(g_hwnd)) return 0;
    kb = GetAncestor(g_hwnd, GA_ROOT);
    if (!kb) kb = g_hwnd;
    ms = g_hwnd;

    for (i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); ++i) {
        const int vk = (int)g_keys[i];
        if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
        gate_rel_set(vk);
        PostMessageW(kb, WM_KEYUP, (WPARAM)vk, 0xC0000001u);
    }
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        gate_rel_set(VK_LBUTTON);
        PostMessageW(ms, WM_LBUTTONUP, 0, 0);
    }
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
        gate_rel_set(VK_RBUTTON);
        PostMessageW(ms, WM_RBUTTONUP, 0, 0);
    }
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
        gate_rel_set(VK_MBUTTON);
        PostMessageW(ms, WM_MBUTTONUP, 0, 0);
    }
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) {
        gate_rel_set(VK_XBUTTON1);
        PostMessageW(ms, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), 0);
    }
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) {
        gate_rel_set(VK_XBUTTON2);
        PostMessageW(ms, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), 0);
    }
    return 1;
}

int rr_gate_install(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return 0;
    g_hwnd = hwnd;
    g_installed = 1;
    if (InterlockedCompareExchange(&g_open, 0, 0)) {
        HWND root;
        gate_subclass(hwnd);
        root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd) gate_subclass(root);
        if (!g_armed && gate_release_held()) g_armed = 1;
    }
    return 1;
}

void rr_gate_remove(void) {
    InterlockedExchange(&g_open, 0);
    g_armed = 0;
    gate_rel_clear();
    gate_restore();
    g_hwnd = NULL;
    g_installed = 0;
}

void rr_gate_set_open(int open) {
    if (open) {
        if (InterlockedCompareExchange(&g_open, 0, 0)) return;   /* already up */
        if (g_hwnd && IsWindow(g_hwnd)) {
            HWND root;
            gate_subclass(g_hwnd);
            root = GetAncestor(g_hwnd, GA_ROOT);
            if (root && root != g_hwnd) gate_subclass(root);
        }
        InterlockedExchange(&g_open, 1);
        if (!g_armed && gate_release_held()) g_armed = 1;
    } else {
        if (!InterlockedCompareExchange(&g_open, 0, 0)) return;  /* already down */
        InterlockedExchange(&g_open, 0);
        g_armed = 0;
        gate_rel_clear();
        gate_restore();
    }
}

int rr_gate_is_open(void) {
    return InterlockedCompareExchange(&g_open, 0, 0) != 0;
}

HWND rr_gate_hwnd(void) { return g_hwnd; }

int rr_gate_foreground(void) {
    if (!g_installed || !g_hwnd || !IsWindow(g_hwnd)) return 0;
    HWND root = GetAncestor(g_hwnd, GA_ROOT);
    if (!root) root = g_hwnd;
    return GetForegroundWindow() == root;
}
