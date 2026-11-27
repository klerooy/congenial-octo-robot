#ifndef RR_INPUT_GATE_H
#define RR_INPUT_GATE_H
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
int  rr_gate_install(HWND hwnd);
void rr_gate_remove(void);
void rr_gate_set_open(int open);
int  rr_gate_is_open(void);
/* The game window installed by the render thread, or NULL before the first
 * present. rr_gate_foreground() is true only while that window owns input. */
HWND rr_gate_hwnd(void);
int  rr_gate_foreground(void);
#ifdef __cplusplus
}
#endif
#endif