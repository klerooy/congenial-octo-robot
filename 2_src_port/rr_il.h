/* rr_il.h - Managed invocation from outside the target process.
 *
 * There are three pieces, and none of them touch the game's memory layout:
 *
 *   export   - resolve an il2cpp API entry point in the loaded GameAssembly
 *              image (the module exports 241 il2cpp_* symbols);
 *   alloc    - il2cpp_object_new() on a thread that the runtime knows about;
 *   invoke   - il2cpp_runtime_invoke() with up to four pointer-sized arguments.
 *
 * Both execution paths run a generated stub inside the target. On an
 * unprotected process that is a remote thread created with CreateRemoteThread;
 * when the title filters remote allocation and thread creation, the block comes
 * from the read/write driver and the execution step is a user-mode APC queued
 * by the kernel driver (3_src_driver/rr_exec.c). The stub attaches the thread to
 * the il2cpp domain first (BOEHM GC allocation from an unregistered thread is
 * not safe), performs the one call, stores the result, detaches, and returns.
 * Everything is bounded: one block, a 5 s wait, and the block is freed on every
 * Win32 path. The stub claims the block before it runs, so the same block can be
 * offered to several threads and still execute exactly once.
 *
 * This is transport only. Which method to call, and what object graph the
 * message needs, comes from --methoda and the harvested field map.
 */
#ifndef RR_IL_H
#define RR_IL_H
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve one export in the loaded GameAssembly.dll. 1 on success. */
int rr_il_export(const char* name, uint64_t* addr_out);

/* Print exports matching a case-insensitive substring with their RVAs.
 * Returns the number printed. */
int rr_il_exports(const char* filter, FILE* out);

/* Report whether the API set the transport needs is present. */
int rr_il_probe(FILE* out);

/* il2cpp_object_new on an attached remote thread. Returns 1 and the object
 * address, or 0 with a reason written to out (when out is non-NULL). */
int rr_il_alloc(uint64_t klass, uint64_t* obj_out, FILE* out);

/* il2cpp_runtime_invoke(method, obj, args, &exc). obj may be 0 for a static
 * method. args are pointer-sized values; a managed int is passed by pointer to
 * a 4-byte slot, so callers that need value types pass the address of a slot
 * rather than the integer. Returns 1 when the call completed; ret_out receives
 * the return value and exc_out the exception object (0 when none was thrown). */
int rr_il_invoke(uint64_t method, uint64_t obj, const uint64_t* args, int nargs,
                 uint64_t* ret_out, uint64_t* exc_out, FILE* out);

/* Class name of an object address, for diagnostics (0 when unreadable). */
int rr_il_object_class(uint64_t obj, char* name, int cap);

/* Execution-path checks. Both drive the same block the transport uses, so they
 * answer the only question that matters before a real call is attempted: does
 * the chosen execution step reach the target's address space and come back.
 *
 *   rr_il_apc_smoke   - block + a stub that touches nothing, writes its
 *                       completion marker, returns. Proves the APC fired.
 *   rr_il_domain_probe- block + il2cpp_domain_get() only. Proves the managed
 *                       runtime is callable from that execution context.
 *
 * Return 1 on success. Both use the driver path when Win32 is denied. */
int rr_il_apc_smoke(FILE* out);
int rr_il_domain_probe(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* RR_IL_H */
