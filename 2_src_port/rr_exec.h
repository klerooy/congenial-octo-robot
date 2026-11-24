/* rr_exec.h - client for the APC channel driver (3_src_driver/rr_exec.c).
 *
 * The driver is a kernel-side execution step: it queues a user-mode APC into a
 * thread of the target, which the Win32 API cannot do once a kernel anti-cheat
 * has filtered PROCESS_VM_OPERATION and PROCESS_CREATE_THREAD. The channel is a
 * small file the driver polls; nothing here needs a device handle or elevation.
 */
#ifndef RR_EXEC_H
#define RR_EXEC_H

#include <stdint.h>

/* 1 when a worker answers a ping within the budget, 0 otherwise. A mapped image
 * takes the channel at load, so a live answer means the APC path is usable. */
int rr_exec_ping(int timeout_ms);

/* Same request, plus the channel generation the answering image wrote when it
 * took the channel. A newer image bumps the counter and the older worker leaves
 * its loop, so the number names which image is answering: needed to tell an
 * image that carries OP_PROTECT from one that was loaded before the op existed. */
int rr_exec_ping_gen(int timeout_ms, uint32_t* generation);

/* Queue a user-mode APC at `routine` with `context` in thread `tid` of `pid`.
 * Returns 1 when the request reached the driver, with *status holding the
 * driver's NTSTATUS and *stage naming the step it stopped at. */
int rr_exec_apc(uint32_t pid, uint32_t tid, uint64_t routine, uint64_t context,
                long* status, unsigned* stage);

/* Set the protection of a range in `pid`. The write transport probes the
 * destination for write access, so a read-only import slot needs this before it
 * can be patched. *old_protect receives the previous value so the caller can put
 * the page back exactly as it was. */
int rr_exec_protect(uint32_t pid, uint64_t addr, uint64_t size, uint32_t protect,
                    long* status, unsigned* stage, uint32_t* old_protect);

/* Thread ids of a process, via the toolhelp snapshot: no handle into the target
 * is opened, so handle filtering cannot hide the list. Returns the count. */
int rr_thread_ids(uint32_t pid, uint32_t* out, int cap);

#endif /* RR_EXEC_H */
