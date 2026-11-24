/* rr_mmap.h - manual map of an image the loader will not accept.
 *
 * rr_map.h loads a payload by calling LoadLibraryW inside the target. That is
 * enough when nothing inspects module loads; Easy Anti-Cheat does: inside the
 * protected game an unsigned DLL never gets an HMODULE (LoadLibraryW answers
 * NULL while a signed DLL from the same folder loads in 31 ms). The image still
 * has to run, so this loader does the work the NT loader would do - copy the
 * sections, apply the relocations, bind the imports against the target's own
 * modules - and then calls the entry point from an APC.
 *
 * Nothing here touches the loader list: the image has no LDR_DATA_TABLE_ENTRY,
 * which is exactly what a module-enumeration check looks for.
 */
#ifndef RR_MMAP_H
#define RR_MMAP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RR_MMAP {
    uint64_t base;        /* image base in the target                      */
    uint32_t size;        /* SizeOfImage                                   */
    uint32_t entry;       /* RVA the APC called                            */
    uint64_t dll_main;    /* return value of the entry point               */
    uint32_t relocs;      /* fixups applied                                */
    uint32_t imports;     /* import slots bound                            */
    uint32_t waited_ms;   /* time from APC to completion marker            */
} RR_MMAP;

/* Manual-map dll_path into pid. Returns 1 on success. The image stays in the
 * target for the lifetime of the process; there is no unload path because
 * there is no loader entry to remove. */
int rr_manual_map(uint32_t pid, const char* dll_path, RR_MMAP* out, FILE* log);

#ifdef __cplusplus
}
#endif

#endif /* RR_MMAP_H */
