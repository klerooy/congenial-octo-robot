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
/* Called every frame from the render thread: pumps the hook thread's queue so
 * the low-level hooks stay alive (Windows removes a hook that stalls). */
void rr_gate_pump(void);
#ifdef __cplusplus
}
#endif
#endif