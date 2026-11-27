/* rr_los.h - Lifecycle and three-state line-of-sight query interface. */
#ifndef RR_LOS_H
#define RR_LOS_H
#include "rr_game.h"
#include "rr_los_geom.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Reader-owned background worker; returns UNKNOWN while snapshots are stale. */
int rr_los_start(void);
void rr_los_stop(void);
/* One capture iteration for hosts that cannot create a worker thread (the
 * in-process payload calls this from its Present hook at ~40 ms). */
void rr_los_pump(void);
/* 1 when this host can answer LOS queries at all. Hosts that compile the
 * capture worker return 1 and treat UNKNOWN as not visible. */
int rr_los_available(void);
int rr_los_check(RR_V3 start, RR_V3 end);
/* Static geometry only. Used for a target seated inside a vehicle: the
 * occupant's own hull is a runtime collider and must not block the ray. */
int rr_los_check_static(RR_V3 start, RR_V3 end);
#ifdef __cplusplus
}
#endif
#endif
