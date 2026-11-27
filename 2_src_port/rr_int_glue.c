/* rr_int_glue.c - the host service the aim worker expects that the in-process
 * payload cannot share with the external overlay.
 *
 * Line of sight is now real: rr_los.c and its capture modules are linked into
 * rr_int.dll and pumped from Present (see rr_los_pump), so the aim can reject
 * targets behind collision geometry.
 *
 * Raw mouse: the external host accumulates WM_INPUT deltas for recoil
 * compensation. Inside the process the game consumes that input itself, so the
 * delta reads zero and recoil correction runs on the camera-derived path only.
 */
#include "rr_recoil.h"

int rr_raw_mouse_delta(int* dx, int* dy) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return 0;
}
