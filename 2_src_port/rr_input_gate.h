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
void rr_gate_pump(void);
#ifdef __cplusplus
}
#endif
#endif