/* rr_int_glue.c - the two host services the aim worker expects.
 *
 * rr_aim.c is shared with the external overlay, so it calls into the host for
 * line of sight and for the tracked raw-mouse delta. Inside the game there is no
 * capture worker and no raw-input ring: the payload owns neither. Both entries
 * therefore answer the honest value instead of pretending.
 *
 * LOS: UNKNOWN is the documented "no data" answer, and rr_aim.c treats a
 * non-CLEAR result as not visible, so an enabled LOS filter blocks rather than
 * fires through a wall. The internal menu keeps aa_los_on at 0.
 *
 * Raw mouse: the external host accumulates WM_INPUT deltas for recoil
 * compensation. Inside the process the game consumes that input itself, so the
 * delta reads zero and recoil correction runs on the camera-derived path only.
 */
#include "rr_los.h"
#include "rr_recoil.h"

int rr_los_start(void) { return 0; }

void rr_los_stop(void) {}

int rr_los_check(RR_V3 start, RR_V3 end) {
    (void)start;
    (void)end;
    return RR_LOS_UNKNOWN;
}

int rr_raw_mouse_delta(int* dx, int* dy) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;
    return 0;
}
