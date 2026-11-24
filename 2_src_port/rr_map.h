/* rr_map.h - internal-module loader for a protected target.
 *
 * Loads an x64 DLL inside the target process without VirtualAllocEx, without
 * CreateRemoteThread and without a user APC. Those three steps are the ones a
 * kernel anti-cheat filters; a write to the target's import table is not.
 *
 * Method, in order:
 *   1. Read the target's image path out of its PEB, so the trigger module can
 *      be parsed from the same file the target loaded.
 *   2. Allocate one RWX page in the target through the read/write driver.
 *   3. Write a trampoline there: save the argument registers, call
 *      LoadLibraryW(path), publish the HMODULE, restore the registers and jump
 *      to the original function.
 *   4. Overwrite one import slot in the trigger module with the trampoline.
 *      The next call to that import runs the trampoline on a live game thread.
 *   5. Poll for the completion marker, restore the import slot, confirm the
 *      module is present in the target's loader list.
 *
 * The trampoline never restores the import slot itself: the slot lives in a
 * read-only page, and a user-mode write there would fault. The loader restores
 * it from kernel memory as soon as the marker appears.
 */
#ifndef RR_MAP_H
#define RR_MAP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RR_MAP_DEFAULT_TRIGGER "UnityPlayer.dll"
#define RR_MAP_DEFAULT_IMPORT  "PeekMessageW"

typedef struct RR_INT_LOAD {
    uint64_t image_base;     /* module base in the target after the load      */
    uint64_t stub_addr;      /* trampoline page                                */
    uint64_t iat_slot;       /* patched import slot                            */
    uint64_t original_fn;    /* value the slot held before the patch           */
    uint32_t waited_ms;      /* time from patch to completion marker           */
} RR_INT_LOAD;

/* Load dll_path (a path on this machine, readable by the target) into pid.
 * trigger_module is a module name in the target, e.g. "UnityPlayer.dll";
 * trigger_import is one of its imports, e.g. "PeekMessageW".
 * Both may be NULL for the defaults above. Returns 1 on success. */
int rr_int_load(uint32_t pid, const char* dll_path, const char* trigger_module,
                const char* trigger_import, RR_INT_LOAD* out, FILE* log);

/* Target image path from its PEB (UTF-8). Returns 1 on success. */
int rr_target_image_path(uint32_t pid, char* out, size_t cap);

/* Import-slot RVA of dll!func inside a local PE file. Used by the loader and by
 * rr_probe's diagnostics. Returns 1 and stores the RVA on success. */
int rr_file_import_slot(const char* path, const char* dll, const char* func,
                        uint32_t* slot_rva, FILE* log);

/* Address of module!name inside the target, resolved by walking the target's own
 * export directory. The manual mapper binds imports with this, so the address is
 * the one actually mapped in the game rather than the one in this process. */
int rr_target_export(uint32_t pid, const char* module, const char* name, uint64_t* out);

/* The same resolution keyed by ordinal, for import tables that reference a
 * function by number rather than by name. */
int rr_target_export_ordinal(uint32_t pid, const char* module, uint32_t ord, uint64_t* out);

/* Run fn(a1,a2,a3) once inside the target, on a game thread, by borrowing a
 * trigger import (default UnityPlayer.dll!PeekMessageW). This is the execution
 * primitive that survives a process whose queued APCs are dropped: the call
 * happens because the game itself makes it, and the trampoline restores the
 * argument registers and jumps to the original function afterwards. The import
 * slot is put back as soon as the marker appears. Returns 1 when the call ran;
 * the return value lands in *result. */
int rr_target_call(uint32_t pid, const char* trigger_module, const char* trigger_import,
                   uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3,
                   uint64_t* result, uint32_t* waited, FILE* log);

#ifdef __cplusplus
}
#endif

#endif /* RR_MAP_H */
