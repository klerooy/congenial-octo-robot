/* rr_probe.c - read-only diagnostic console without an elevation manifest.
 *
 * The overlay ships with requireAdministrator, which means every live check
 * starts with a UAC prompt. The driver is already mapped by the launcher, and
 * its device only needs a key, so a plain medium-integrity console can read the
 * target the same way the overlay does. This tool exists to answer questions
 * that do not involve executing anything in the game:
 *
 *   rr_probe module            - GameAssembly base/size and PE header truth
 *   rr_probe exports [substr]   - export table walk
 *   rr_probe probe              - the il2cpp API set the transport needs
 *   rr_probe peek <hex> [len]   - raw bytes at an address
 *   rr_probe regions [n]        - committed regions the scanner would walk
 *   rr_probe handle             - can this process open the game for execution
 *
 * Build: build_probe.bat. No manifest, no UI, exit code 0 on success.
 */
#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr_mem.h"
#include "rr_il.h"
#include "rr_exec.h"
#include "rr_map.h"
#include "rr_mmap.h"

#define PROBE_DUMP_MAX 0x400

static uint64_t parse_hex(const char* s) {
    char* end = NULL;
    unsigned long long v;
    if (!s) return 0;
    v = _strtoui64(s, &end, 16);
    return (end && end != s) ? (uint64_t)v : 0;
}

static void dump(uint64_t addr, unsigned len) {
    uint8_t buf[PROBE_DUMP_MAX];
    if (len < 16) len = 16;
    if (len > sizeof buf) len = sizeof buf;
    if (!rr_read(addr, buf, len)) {
        printf("read failed at 0x%llX (%u bytes)\n", (unsigned long long)addr, len);
        return;
    }
    for (unsigned i = 0; i < len; i += 16) {
        unsigned n = len - i < 16 ? len - i : 16;
        printf("%016llX  ", (unsigned long long)(addr + i));
        for (unsigned j = 0; j < n; ++j) printf("%02X ", buf[i + j]);
        for (unsigned j = n; j < 16; ++j) printf("   ");
        printf(" ");
        for (unsigned j = 0; j < n; ++j) {
            unsigned char c = buf[i + j];
            putchar(c >= 0x20 && c < 0x7F ? c : '.');
        }
        putchar('\n');
    }
}

/* Same walk rr_il.c performs, printed step by step so a failure names its stage. */
static void module_report(void) {
    uint64_t base = 0;
    uint32_t size = 0;
    uint8_t dos[0x40] = {0}, nt[0x100] = {0};
    uint32_t lfanew = 0;
    uint16_t magic = 0, nsec = 0;
    int got_dos, got_nt;

    if (!rr_module("GameAssembly.dll", &base, &size)) {
        printf("module: GameAssembly.dll not found in the loader list\n");
        return;
    }
    printf("module: base=0x%llX size=0x%X\n", (unsigned long long)base, size);

    got_dos = rr_read(base, dos, sizeof dos);
    printf("header: dos_read=%d mz=%02X%02X\n", got_dos, dos[0], dos[1]);
    memcpy(&lfanew, dos + 0x3C, 4);
    printf("header: e_lfanew=0x%X\n", lfanew);
    if (!lfanew || lfanew > 0x1000) return;

    got_nt = rr_read(base + lfanew, nt, sizeof nt);
    memcpy(&magic, nt + 0x18, 2);
    memcpy(&nsec, nt + 0x06, 2);
    printf("header: nt_read=%d sig=%02X%02X sections=%u magic=0x%X\n",
           got_nt, nt[0], nt[1], nsec, magic);
    if (magic == 0x20B)
        printf("header: export_rva=0x%X export_size=0x%X\n",
               *(uint32_t*)(nt + 0x88), *(uint32_t*)(nt + 0x8C));
    else if (magic == 0x10B)
        printf("header: export_rva=0x%X export_size=0x%X\n",
               *(uint32_t*)(nt + 0x78), *(uint32_t*)(nt + 0x7C));
}

static void region_report(int want) {
    uint64_t a = 0x10000ULL;
    RR_REGION r;
    int shown = 0, total = 0;
    while (a < 0x7FFFFFFFFFFFULL) {
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (!r.size) break;
        ++total;
        if (shown < want && r.state == 0x1000) {
            printf("  base=0x%llX size=0x%llX state=0x%X protect=0x%X type=0x%X\n",
                   (unsigned long long)r.base, (unsigned long long)r.size,
                   r.state, r.protect, r.type);
            ++shown;
        }
        if (r.base + r.size <= a) break;
        a = r.base + r.size;
    }
    printf("regions: %d walked, %d shown\n", total, shown);
}

static void handle_report(void) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, g_rr_pid);
    if (!h) {
        printf("handle: OpenProcess failed (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    printf("handle: OpenProcess ok\n");
    {
        void* p = VirtualAllocEx(h, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
        printf("handle: VirtualAllocEx %s (%lu)\n",
               p ? "ok" : "failed", p ? 0ul : (unsigned long)GetLastError());
        if (p) VirtualFreeEx(h, p, 0, MEM_RELEASE);
    }
    CloseHandle(h);
}

/* The transport as EAC allows it: memory from the driver, thread from Win32.
 * Stub is `xor eax,eax; ret`, so a completed thread proves both halves. */
static void remote_report(void) {
    static const uint8_t stub[] = { 0x48, 0x31, 0xC0, 0xC3 };
    uint64_t blk = 0;
    HANDLE h, th;
    DWORD waited = 0, code = 0;

    if (!rr_drv_alloc_remote(g_rr_pid, 0x1000, 0x40 /* PAGE_EXECUTE_READWRITE */, &blk)) {
        printf("remote: driver allocation failed\n");
        return;
    }
    printf("remote: block=0x%llX (driver)\n", (unsigned long long)blk);
    if (!rr_write(blk, stub, sizeof stub)) {
        printf("remote: stub write failed\n");
        return;
    }
    {
        uint8_t back[sizeof stub] = {0};
        if (!rr_read(blk, back, sizeof back) || memcmp(back, stub, sizeof stub) != 0) {
            printf("remote: stub readback mismatch\n");
            return;
        }
        printf("remote: stub verified in target\n");
    }
    h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION,
                    FALSE, g_rr_pid);
    if (!h) {
        printf("remote: OpenProcess for thread failed (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    th = CreateRemoteThread(h, NULL, 0, (LPTHREAD_START_ROUTINE)blk, NULL, 0, NULL);
    if (!th) {
        printf("remote: CreateRemoteThread failed (%lu)\n", (unsigned long)GetLastError());
        CloseHandle(h);
        return;
    }
    waited = WaitForSingleObject(th, 5000);
    GetExitCodeThread(th, &code);
    printf("remote: thread %s exit=%lu\n",
           waited == WAIT_OBJECT_0 ? "completed" : "hung", (unsigned long)code);
    CloseHandle(th);
    CloseHandle(h);
}

/* Scan committed regions for an ASCII string. This is the read-only version of
 * the resolver's first stage: it says whether a type name is present at all, and
 * where, which is what separates "class not loaded" from "outside the window". */
static int find_string(const char* needle, unsigned long long budget_mb, int max_hits) {
    size_t n = strlen(needle);
    uint64_t a = 0x10000ULL;
    static uint8_t buf[0x10000 + 0x100];
    unsigned long long scanned = 0, limit = budget_mb * 1024ull * 1024ull;
    int hits = 0;

    if (n < 4 || n > 200) {
        printf("find: 4..200 byte needle\n");
        return 0;
    }
    while (a < 0x7FFFFFFFFFFFULL && hits < max_hits && scanned < limit) {
        RR_REGION r;
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (!r.size) break;
        if (r.state == 0x1000 && (r.protect & 0xEE) && r.size >= 0x1000) {
            uint64_t end = r.base + r.size;
            for (uint64_t p = r.base; p + n <= end && hits < max_hits; p += 0x10000) {
                size_t want = (size_t)((end - p) < 0x10000 ? (end - p) : 0x10000);
                if (!rr_read(p, buf, want)) break;
                scanned += want;
                for (size_t i = 0; i + n <= want; ++i) {
                    if (buf[i] != (uint8_t)needle[0]) continue;
                    if (memcmp(buf + i, needle, n) != 0) continue;
                    printf("  hit 0x%llX  %s\n", (unsigned long long)(p + i), needle);
                    if (++hits >= max_hits) break;
                }
                if (scanned >= limit) break;
            }
        }
        if (r.base + r.size <= a) break;
        a = r.base + r.size;
    }
    printf("find: %d hits, %llu MB scanned, %s\n", hits, scanned >> 20,
           hits ? "present" : "not found in budget");
    return hits;
}

/* Where does an address sit in the resolver's slice plan? Prints the region,
 * its ordinal among scannable regions, and the cumulative byte offset, which is
 * what decides whether a fixed slice count can reach it. */
static void where_report(uint64_t addr) {
    uint64_t a = 0x10000ULL, cum = 0;
    RR_REGION r;
    int idx = 0;
    while (a < 0x7FFFFFFFFFFFULL) {
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (!r.size) break;
        int scan = (r.state == 0x1000) && (r.protect & 0xEE) && r.size >= 0x1000;
        if (scan) {
            if (addr >= r.base && addr < r.base + r.size) {
                printf("where: 0x%llX in region #%d base=0x%llX size=0x%llX "
                       "protect=0x%X type=0x%X\n",
                       (unsigned long long)addr, idx, (unsigned long long)r.base,
                       (unsigned long long)r.size, r.protect, r.type);
                printf("where: cumulative offset=0x%llX slices=%llu of 2048 at 32MB\n",
                       (unsigned long long)cum, cum >> 25);
                return;
            }
            ++idx;
            cum += r.size;
        }
        if (r.base + r.size <= a) break;
        a = r.base + r.size;
    }
    printf("where: 0x%llX not inside any scannable region (%d scanned before)\n",
           (unsigned long long)addr, idx);
}

/* Find qwords equal to a value: the inverse direction, used to locate class
 * objects that reference a name string. */
static int find_qword(uint64_t value, int max_hits) {
    uint64_t a = 0x10000ULL;
    static uint8_t buf[0x10000 + 0x100];
    int hits = 0;
    while (a < 0x7FFFFFFFFFFFULL && hits < max_hits) {
        RR_REGION r;
        if (!rr_query_region(a, &r)) { a += 0x10000; continue; }
        if (!r.size) break;
        if (r.state == 0x1000 && (r.protect & 0xEE) && r.size >= 0x1000) {
            uint64_t end = r.base + r.size;
            for (uint64_t p = r.base; p + 8 <= end && hits < max_hits; p += 0x10000) {
                size_t want = (size_t)((end - p) < 0x10000 ? (end - p) : 0x10000);
                if (!rr_read(p, buf, want)) break;
                for (size_t i = 0; i + 8 <= want; i += 8) {
                    uint64_t v;
                    memcpy(&v, buf + i, 8);
                    if (v != value) continue;
                    printf("  ref 0x%llX\n", (unsigned long long)(p + i));
                    if (++hits >= max_hits) break;
                }
            }
        }
        if (r.base + r.size <= a) break;
        a = r.base + r.size;
    }
    printf("xref: %d hits for 0x%llX\n", hits, (unsigned long long)value);
    return hits;
}

/* Which thread primitives the target allows. Reads a context and writes it back
 * unchanged: if that pair succeeds a redirect is possible with no kernel driver
 * at all, which is the cheapest execution step there is. */
static void ctx_probe(void) {
    uint32_t tids[8];
    int n = rr_thread_ids(g_rr_pid, tids, 8);
    int ok_open = 0, ok_get = 0, ok_set = 0;

    if (!n) {
        printf("ctxprobe: no threads\n");
        return;
    }
    for (int i = 0; i < n; ++i) {
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tids[i]);
        CONTEXT c;
        DWORD prev;
        if (!th) {
            printf("ctxprobe: OpenThread %lu failed (%lu)\n", (unsigned long)tids[i],
                   (unsigned long)GetLastError());
            continue;
        }
        ++ok_open;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_FULL;
        prev = SuspendThread(th);
        if (prev != (DWORD)-1 && GetThreadContext(th, &c)) {
            ++ok_get;
            printf("ctxprobe: tid=%lu rip=0x%llX rsp=0x%llX\n", (unsigned long)tids[i],
                   (unsigned long long)c.Rip, (unsigned long long)c.Rsp);
            if (SetThreadContext(th, &c)) ++ok_set;
            else printf("ctxprobe: SetThreadContext failed (%lu)\n",
                        (unsigned long)GetLastError());
        } else {
            printf("ctxprobe: GetThreadContext failed (%lu) prev=%ld\n",
                   (unsigned long)GetLastError(), (long)prev);
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    printf("ctxprobe: open=%d get=%d set=%d of %d\n", ok_open, ok_get, ok_set, n);
}

/* Kernel-side checks. NtQuerySystemInformation(SystemModuleInformation) gives the
 * loaded-module bases, and the same read request the overlay uses works with an
 * explicit pid, so pid 4 answers whether the transport reaches kernel memory at
 * all. No patch is attempted here. */
typedef struct {
    HANDLE section;
    void*  mapped_base;
    void*  image_base;
    uint32_t image_size;
    uint32_t flags;
    uint16_t load_order, init_order, load_count, name_off;
    uint8_t  path[256];
} RR_KMOD;

typedef long (NTAPI* PFN_NTQSI)(ULONG, void*, ULONG, ULONG*);

static int kmod_list(RR_KMOD* out, int cap) {
    PFN_NTQSI q = (PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                            "NtQuerySystemInformation");
    typedef long (NTAPI* PFN_ADJ)(ULONG, BOOLEAN, BOOLEAN, PBOOLEAN);
    PFN_ADJ adj = (PFN_ADJ)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                          "RtlAdjustPrivilege");
    uint32_t need = 0, got = 0;
    uint8_t* buf;
    int n = 0;
    if (!q) return 0;
    /* Kernel module bases are blanked for callers without SeDebugPrivilege. */
    if (adj) { BOOLEAN was = 0; adj(20 /* SeDebugPrivilege */, TRUE, FALSE, &was); }
    if (q(11, NULL, 0, &need) != 0xC0000004L || !need) return 0;
    need += 0x10000;
    buf = (uint8_t*)malloc(need);
    if (!buf) return 0;
    if (q(11, buf, need, &got) < 0) { free(buf); return 0; }
    {
        uint32_t count = *(uint32_t*)buf;
        RR_KMOD* items = (RR_KMOD*)(buf + 8);
        if (count > (uint32_t)cap) count = (uint32_t)cap;
        for (uint32_t i = 0; i < count; ++i) out[i] = items[i];
        n = (int)count;
    }
    free(buf);
    return n;
}

static int hex2bin(const char* s, uint8_t* out, int cap) {
    int n = 0;
    while (*s && n < cap) {
        while (*s == ' ' || *s == ',' || *s == 'x' || *s == 'X') ++s;
        if (!s[0] || !s[1]) break;
        unsigned v = 0;
        int ok = 1;
        for (int i = 0; i < 2; ++i) {
            char c = s[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else { ok = 0; break; }
        }
        if (!ok) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void kwrite_report(uint64_t addr, const char* hex) {
    uint8_t buf[4096];
    int n = hex ? hex2bin(hex, buf, (int)sizeof buf) : 0;
    if (!n) { printf("kwrite: need <addr> <hexbytes>\n"); return; }
    if (!rr_write_pid(4, addr, buf, (size_t)n)) {
        printf("kwrite %llX: refused\n", (unsigned long long)addr);
        return;
    }
    printf("kwrite %llX %d bytes ok\n", (unsigned long long)addr, n);
}

static void kdump_file(uint64_t addr, unsigned len, const char* path) {
    uint8_t* buf;
    FILE* f;
    if (!path || !len || len > 0x100000u) {
        printf("kdump: need <addr> <len> <file>\n");
        return;
    }
    buf = (uint8_t*)malloc(len);
    if (!buf) return;
    if (!rr_read_pid(4, addr, buf, len)) {
        printf("kdump %llX: refused\n", (unsigned long long)addr);
        free(buf);
        return;
    }
    f = fopen(path, "wb");
    if (!f) { printf("kdump: cannot write %s\n", path); free(buf); return; }
    fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    printf("kdump %llX %u bytes -> %s\n", (unsigned long long)addr, len, path);
}

static void kmraw_report(void) {
    PFN_NTQSI q = (PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                            "NtQuerySystemInformation");
    uint32_t need = 0, got = 0;
    uint8_t* buf;
    if (!q) return;
    if (q(11, NULL, 0, &need) != 0xC0000004L || !need) return;
    need += 0x10000;
    buf = (uint8_t*)malloc(need);
    if (!buf) return;
    if (q(11, buf, need, &got) < 0) { free(buf); return; }
    printf("kmraw: count=%u got=%u\n", *(uint32_t*)buf, got);
    for (int e = 0; e < 2; ++e) {
        uint8_t* p = buf + 8 + (size_t)e * 0x128;
        printf("entry %d:\n", e);
        for (int r = 0; r < 3; ++r) {
            printf("  +%02X  ", r * 16);
            for (int c = 0; c < 16; ++c) printf("%02X ", p[r * 16 + c]);
            printf("\n");
        }
        printf("  name: %s\n", (char*)p + 0x28 + *(uint16_t*)(p + 0x26));
    }
    free(buf);
}

static void kmod_report(const char* filter) {
    RR_KMOD mods[512];
    int n = kmod_list(mods, 512);
    printf("kmods: %d loaded\n", n);
    for (int i = 0; i < n; ++i) {
        const char* name = (const char*)mods[i].path + mods[i].name_off;
        if (filter && *filter && !strstr(name, filter)) continue;
        printf("  %p  %8X  %s\n", mods[i].image_base, mods[i].image_size, name);
    }
}

static void kread_report(uint64_t addr, unsigned len) {
    uint8_t* buf;
    if (!len) len = 32;
    if (len > 4096) len = 4096;
    buf = (uint8_t*)malloc(len);
    if (!buf) return;
    if (!rr_read_pid(4, addr, buf, len)) {
        printf("kread %llX: refused (pid 4 read failed)\n", (unsigned long long)addr);
        free(buf);
        return;
    }
    printf("kread %llX ok:\n", (unsigned long long)addr);
    for (unsigned i = 0; i < len; i += 16) {
        printf("  %llX  ", (unsigned long long)(addr + i));
        for (unsigned j = 0; j < 16 && i + j < len; ++j) printf("%02X ", buf[i + j]);
        printf("\n");
    }
    free(buf);
}

/* The op table is compiled into the answering image, not stored anywhere the
 * client can read, so the only honest way to know whether OP_PROTECT is there is
 * to send one. The request names this process, which is not the game image, so
 * the driver stops at the image gate (stage 2) before touching a range; on a
 * driver that predates the op the same request falls into the unknown-op branch
 * (stage 0x52) instead. Returns 1 when the op exists, 0 when it does not, -1 when
 * nothing answered. */
static int protect_op_present(int* stage_out) {
    long st = 0;
    unsigned stage = 0;
    uint32_t old = 0;
    if (stage_out) *stage_out = 0;
    if (!rr_exec_protect(GetCurrentProcessId(), 0x1000, 0x1000, 0x04u,
                         &st, &stage, &old))
        return -1;
    if (stage_out) *stage_out = (int)stage;
    return stage != 0x52;
}

static int channel_report(void) {
    uint32_t gen = 0;
    int stage = 0;
    int have;

    if (!rr_exec_ping_gen(600, &gen)) {
        printf("channel: no worker - map 3_src_driver/rr_exec.sys (BUILD.md, "
               "\"Map the APC driver\")\n");
        return 3;
    }
    printf("channel: worker answering - APC execution path ready, generation %u\n", gen);

    have = protect_op_present(&stage);
    if (have < 0) {
        printf("channel: OP_PROTECT probe got no reply\n");
        return 5;
    }
    if (!have) {
        printf("channel: OP_PROTECT missing - the answering image predates op 0x02 "
               "(stage=0x%X)\n", stage);
        printf("channel: every protection change will be refused; load "
               "3_src_driver/rr_exec.sys (dseload) to take the channel with an "
               "image that has the op\n");
        return 5;
    }
    printf("channel: OP_PROTECT present (probe stopped at stage %d, image gate)\n", stage);
    return 0;
}

/* Driver install path. With driver signature enforcement off the normal loader
 * accepts these images, so no mapper is needed: create a demand-start kernel
 * service pointing at the .sys file and start it. */
static int svc_load(const char* path) {
    char name[MAX_PATH];
    wchar_t wname[MAX_PATH], wpath[MAX_PATH];
    const char* base = strrchr(path, '\\');
    size_t i;
    SC_HANDLE scm, svc;
    int ok = 0;

    base = base ? base + 1 : path;
    for (i = 0; base[i] && base[i] != '.' && i < sizeof(name) - 1; ++i) name[i] = base[i];
    name[i] = 0;
    if (!i || !*path) { printf("load: need a .sys path\n"); return 0; }
    MultiByteToWideChar(CP_ACP, 0, name, -1, wname, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { printf("load: OpenSCManager err=%lu\n", GetLastError()); return 0; }

    svc = CreateServiceW(scm, wname, wname, SERVICE_START | SERVICE_QUERY_STATUS,
                         SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                         wpath, NULL, NULL, NULL, NULL, NULL);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        /* A previous attempt left the key behind; reuse it if it points here. */
        svc = OpenServiceW(scm, wname, SERVICE_START | SERVICE_QUERY_STATUS);
        printf("load: service exists, reopening\n");
    }
    if (!svc) {
        DWORD err = GetLastError();
        printf("load: CreateService err=%lu (0x%lX)\n", err, err);
        CloseServiceHandle(scm);
        return 0;
    }
    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        printf("load: StartService err=%lu (0x%lX)\n", err, err);
        DeleteService(svc);
    } else {
        SERVICE_STATUS ss;
        printf("load: started\n");
        if (QueryServiceStatus(svc, &ss))
            printf("load: state=%lu\n", ss.dwCurrentState);
        ok = 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

/* ci.dll's CiPolicy section holds the code-integrity options dword at +4; the
 * value is 6 on a stock boot. Patching it to 0 makes the loader accept an
 * unsigned image until the next boot. The RVA comes from the installed file so
 * nothing is hard-coded to one build. */
static int ci_policy_base(uint64_t* out) {
    RR_KMOD mods[512];
    int n = kmod_list(mods, 512);
    uint64_t base = 0;
    FILE* f;
    uint8_t hdr[0x1000];
    uint32_t pe, nsec, optsz, sec_rva = 0;
    int found = 0;

    for (int i = 0; i < n; ++i) {
        const char* nm = (const char*)mods[i].path + mods[i].name_off;
        if (!_stricmp(nm, "ci.dll")) { base = (uint64_t)mods[i].image_base; break; }
    }
    if (!base) { printf("dse: ci.dll not in the module list (run elevated)\n"); return 0; }

    f = fopen("C:\\Windows\\System32\\ci.dll", "rb");
    if (!f) { printf("dse: cannot read ci.dll\n"); return 0; }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return 0; }
    pe = *(uint32_t*)(hdr + 0x3C);
    nsec = *(uint16_t*)(hdr + pe + 6);
    optsz = *(uint16_t*)(hdr + pe + 20);
    {
        uint8_t* s = hdr + pe + 24 + optsz;
        for (uint32_t i = 0; i < nsec && s + 40 < hdr + sizeof hdr; ++i, s += 40) {
            if (!memcmp(s, "CiPolicy", 8)) { sec_rva = *(uint32_t*)(s + 12); found = 1; break; }
        }
    }
    fclose(f);
    if (!found) { printf("dse: no CiPolicy section\n"); return 0; }

    *out = base + sec_rva;
    return 1;
}

static int ci_options_va(uint64_t* out, uint32_t* old_value) {
    if (!ci_policy_base(out)) return 0;
    *out += 4;
    if (!rr_read_pid(4, *out, old_value, 4)) {
        printf("dse: cannot read kernel memory at %llX (transport unavailable)\n",
               (unsigned long long)*out);
        return 0;
    }
    return 1;
}

static int dse_set(uint32_t value) {
    uint64_t va = 0;
    uint32_t old = 0;
    if (!ci_options_va(&va, &old)) return 0;
    printf("dse: g_CiOptions at %llX = 0x%X\n", (unsigned long long)va, old);
    if (old == value) { printf("dse: already 0x%X\n", value); return 1; }
    if (!rr_write_pid(4, va, &value, 4)) { printf("dse: write refused\n"); return 0; }
    if (!rr_read_pid(4, va, &old, 4) || old != value) {
        printf("dse: write did not stick (now 0x%X)\n", old);
        return 0;
    }
    printf("dse: now 0x%X\n", old);
    return 1;
}

static void ci_report(void) {
    PFN_NTQSI q = (PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                            "NtQuerySystemInformation");
    struct { uint32_t len; uint32_t options; } ci = { 8, 0 };
    struct { uint32_t len; uint8_t secure; } sb = { 5, 0xFF };
    uint32_t got = 0;
    uint64_t va = 0;
    uint32_t opts = 0;
    if (!q) return;
    /* SystemCodeIntegrityInformation == 103, SystemSecureBootInformation == 145. */
    printf("ci: NtQSI(103) status=%08lX options=0x%X\n",
           (unsigned long)q(103, &ci, sizeof ci, &got), ci.options);
    printf("ci: NtQSI(145) status=%08lX secureboot=%u\n",
           (unsigned long)q(145, &sb, sizeof sb, &got), (unsigned)sb.secure);
    if (rr_drv_open() && ci_options_va(&va, &opts))
        printf("ci: g_CiOptions@%llX = 0x%X\n", (unsigned long long)va, opts);
    else
        printf("ci: kernel read unavailable (no WmPort)\n");
}

static void ci_boot_report(void) {
    PFN_NTQSI q = (PFN_NTQSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                            "NtQuerySystemInformation");
    uint8_t buf[64];
    uint32_t got = 0;
    LONG st;
    if (!q) return;
    /* SystemBootEnvironmentInformation == 90. */
    memset(buf, 0, sizeof buf);
    st = q(90, buf, sizeof buf, &got);
    printf("boot: NtQSI(90) status=%08lX got=%u\n", (unsigned long)st, got);
    for (unsigned i = 0; i < got && i < sizeof buf; i += 8) {
        printf("  +%02X  %02X %02X %02X %02X %02X %02X %02X %02X\n", i,
               buf[i], buf[i+1], buf[i+2], buf[i+3],
               buf[i+4], buf[i+5], buf[i+6], buf[i+7]);
    }
}

static void ci_set_report(uint32_t value) {
    typedef LONG(NTAPI * PFN_NTSETSI)(ULONG, PVOID, ULONG);
    PFN_NTSETSI f = (PFN_NTSETSI)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                "NtSetSystemInformation");
    struct { uint32_t len; uint32_t options; } ci = { 8, 0 };
    LONG st;
    ci.options = value;
    if (!f) { printf("ciset: no NtSetSystemInformation\n"); return; }
    st = f(103, &ci, sizeof ci);
    printf("ciset: options=0x%X -> status=%08lX\n", value, (unsigned long)st);
    ci_report();
}

static int ci_fix(void) {
    uint64_t pol = 0;
    uint32_t opts = 6, zero = 0, one = 1;
    if (!rr_drv_open()) { printf("cifix: no transport\n"); return 0; }
    if (!ci_policy_base(&pol)) return 0;
    if (!rr_write_pid(4, pol + 4, &opts, 4)) { printf("cifix: options write refused\n"); return 0; }
    if (!rr_write_pid(4, pol + 0x10, &zero, 4)) { printf("cifix: policy write refused\n"); return 0; }
    if (!rr_write_pid(4, pol + 8, &one, 4)) { printf("cifix: enable write refused\n"); return 0; }
    printf("cifix: CiPolicy@%llX options=6 policy=0 enabled=1\n", (unsigned long long)pol);
    ci_report();
    return 1;
}

static int dse_load(const char* path) {
    uint64_t va = 0;
    uint32_t old = 0;
    int ok;
    if (!ci_options_va(&va, &old)) return 0;
    printf("dse: g_CiOptions at %llX = 0x%X\n", (unsigned long long)va, old);
    {
        uint32_t zero = 0;
        if (!rr_write_pid(4, va, &zero, 4)) { printf("dse: patch refused\n"); return 0; }
    }
    ok = svc_load(path);
    rr_write_pid(4, va, &old, 4);
    {
        uint32_t back = 0;
        if (rr_read_pid(4, va, &back, 4))
            printf("dse: restored 0x%X (was 0x%X)\n", back, old);
    }
    return ok;
}

int main(int argc, char** argv) {
    const char* cmd = argc > 1 ? argv[1] : "probe";

    /* These three do not need the game: they install a driver or patch the
     * code-integrity options. */
    if (!_stricmp(cmd, "load")) {
        return svc_load(argc > 2 ? argv[2] : "") ? 0 : 6;
    }
    if (!_stricmp(cmd, "dse")) {
        const char* v = argc > 2 ? argv[2] : NULL;
        uint32_t val;
        if (!v) { printf("dse: need a value (0 disables enforcement, 6 is stock)\n"); return 4; }
        if (!rr_drv_open()) { printf("driver: \\\\.\\WmPort unavailable\n"); return 3; }
        val = (uint32_t)strtoul(v, NULL, 0);
        return dse_set(val) ? 0 : 6;
    }
    if (!_stricmp(cmd, "dseload")) {
        if (!rr_drv_open()) { printf("driver: \\\\.\\WmPort unavailable\n"); return 3; }
        return dse_load(argc > 2 ? argv[2] : "") ? 0 : 6;
    }

    if (!_stricmp(cmd, "chan")) {
        /* Needs only the channel file, not the game. Exit 0 only when the
         * answering image also carries OP_PROTECT, so a bring-up script can
         * gate on the state instead of reading the text. */
        return channel_report();
    }

    /* Kernel-space reads and writes address pid 4 explicitly, so they work
     * whether or not the game is up. Everything else needs the target pid. */
    {
        int kernel_only = !_stricmp(cmd, "kmods") || !_stricmp(cmd, "kmraw") ||
                          !_stricmp(cmd, "kread") || !_stricmp(cmd, "kwrite") ||
                          !_stricmp(cmd, "kdump") || !_stricmp(cmd, "ci") ||
                          !_stricmp(cmd, "ciset") || !_stricmp(cmd, "boot") ||
                          !_stricmp(cmd, "cifix");
        /* pid=<n> retargets every non-kernel command, so the loader can be
         * exercised on a harmless process before it touches the game. */
        for (int i = 2; i < argc; ++i)
            if (!_strnicmp(argv[i], "pid=", 4))
                rr_force_pid((uint32_t)strtoul(argv[i] + 4, NULL, 0));
        if (!kernel_only && !rr_attach("ROB RUN.exe")) {
            printf("attach: ROB RUN.exe not running\n");
            return 2;
        }
        if (!_stricmp(cmd, "ci") || !_stricmp(cmd, "ciset") || !_stricmp(cmd, "boot") ||
            !_stricmp(cmd, "cifix")) {
            /* ci_report opens the transport itself when it exists. */
        } else if (!rr_drv_open()) {
            printf("driver: \\\\.\\WmPort unavailable (not mapped, or key rejected)\n");
            return 3;
        }
        if (kernel_only) printf("driver: open, kernel-only (pid 4)\n");
        else              printf("transport: %s, pid=%u\n", rr_transport(), (unsigned)g_rr_pid);
    }

    if (!_stricmp(cmd, "module")) {
        module_report();
    } else if (!_stricmp(cmd, "exports")) {
        rr_il_exports(argc > 2 ? argv[2] : "", stdout);
    } else if (!_stricmp(cmd, "kwrite")) {
        kwrite_report(parse_hex(argc > 2 ? argv[2] : NULL), argc > 3 ? argv[3] : NULL);
    } else if (!_stricmp(cmd, "kdump")) {
        kdump_file(parse_hex(argc > 2 ? argv[2] : NULL),
                   argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0,
                   argc > 4 ? argv[4] : NULL);
    } else if (!_stricmp(cmd, "kmraw")) {
        kmraw_report();
    } else if (!_stricmp(cmd, "kmods")) {
        kmod_report(argc > 2 ? argv[2] : NULL);
    } else if (!_stricmp(cmd, "ci")) {
        ci_report();
    } else if (!_stricmp(cmd, "ciset")) {
        ci_set_report(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
    } else if (!_stricmp(cmd, "cifix")) {
        return ci_fix() ? 0 : 6;
    } else if (!_stricmp(cmd, "boot")) {
        ci_boot_report();
    } else if (!_stricmp(cmd, "kread")) {
        kread_report(parse_hex(argc > 2 ? argv[2] : NULL),
                     argc > 3 ? (unsigned)atoi(argv[3]) : 32);
    } else if (!_stricmp(cmd, "ctxprobe")) {
        ctx_probe();
    } else if (!_stricmp(cmd, "apc")) {
        return rr_il_apc_smoke(stdout) ? 0 : 5;
    } else if (!_stricmp(cmd, "iatslot")) {
        uint32_t rva = 0;
        const char* file = argc > 2 ? argv[2] : NULL;
        const char* dll = argc > 3 ? argv[3] : "user32.dll";
        const char* fn = argc > 4 ? argv[4] : "PeekMessageW";
        const char* mod = argc > 5 ? argv[5] : NULL;
        if (!file || !rr_file_import_slot(file, dll, fn, &rva, stdout)) {
            printf("iatslot: not found\n");
            return 6;
        }
        printf("iatslot: %s!%s rva=0x%X\n", dll, fn, rva);
        if (mod) {
            uint64_t base = 0;
            uint32_t size = 0;
            RR_REGION r;
            if (rr_module(mod, &base, &size) && base) {
                uint64_t a = base + rva;
                printf("iatslot: %s base=0x%llX addr=0x%llX\n", mod,
                       (unsigned long long)base, (unsigned long long)a);
                if (rr_query_region(a, &r))
                    printf("iatslot: protect=0x%X state=0x%X type=0x%X\n",
                           r.protect, r.state, r.type);
            } else {
                printf("iatslot: %s not loaded in the target\n", mod);
            }
        }
    } else if (!_stricmp(cmd, "protect")) {
        uint64_t a = parse_hex(argc > 2 ? argv[2] : NULL);
        uint64_t sz = argc > 3 ? parse_hex(argv[3]) : 0x1000;
        uint32_t prot = argc > 4 ? (uint32_t)parse_hex(argv[4]) : 0x04u;
        long st = 0;
        unsigned stage = 0;
        uint32_t old = 0;
        if (!a) { printf("protect: address required\n"); return 6; }
        if (!rr_exec_protect((uint32_t)g_rr_pid, a, sz, prot, &st, &stage, &old)) {
            printf("protect: no answer from the channel driver\n");
            return 6;
        }
        printf("protect: addr=0x%llX size=0x%llX new=0x%X status=%08lX stage=%u old=0x%X\n",
               (unsigned long long)a, (unsigned long long)sz, prot,
               (unsigned long)st, stage, old);
        return st >= 0 ? 0 : 5;
    } else if (!_stricmp(cmd, "mapdll")) {
        RR_INT_LOAD load;
        const char* dll = argc > 2 ? argv[2] : NULL;
        const char* imp = argc > 3 ? argv[3] : NULL;
        const char* mod = argc > 4 ? argv[4] : NULL;
        if (!dll) {
            printf("mapdll: need a DLL path\n");
            return 4;
        }
        printf("mapdll: %s -> pid %u\n", dll, (unsigned)g_rr_pid);
        if (rr_int_load(g_rr_pid, dll, mod, imp, &load, stdout)) {
            printf("mapdll: loaded, base=0x%llX in %ums\n",
                   (unsigned long long)load.image_base, load.waited_ms);
            return 0;
        }
        printf("mapdll: failed (see lines above)\n");
        return 6;
    } else if (!_stricmp(cmd, "tgt")) {
        uint64_t a = 0;
        const char* m = argc > 2 ? argv[2] : NULL;
        const char* n = argc > 3 ? argv[3] : NULL;
        if (!m || !n) { printf("tgt: need a module and a function name\n"); return 4; }
        {
            uint64_t b = 0;
            uint32_t s = 0;
            if (rr_module(m, &b, &s)) printf("tgt: module %s base=0x%llX size=0x%X\n",
                                             m, (unsigned long long)b, s);
            else printf("tgt: module %s not in the target's list\n", m);
        }
        if (rr_target_export(g_rr_pid, m, n, &a) && a) {
            printf("tgt: %s!%s = 0x%llX\n", m, n, (unsigned long long)a);
            return 0;
        }
        printf("tgt: %s!%s not resolved\n", m, n);
        return 6;
    } else if (!_stricmp(cmd, "mmap")) {
        RR_MMAP mm;
        const char* dll = argc > 2 ? argv[2] : NULL;
        if (!dll) {
            printf("mmap: need a DLL path\n");
            return 4;
        }
        printf("mmap: %s -> pid %u (manual, no loader entry)\n", dll, (unsigned)g_rr_pid);
        if (rr_manual_map(g_rr_pid, dll, &mm, stdout)) {
            printf("mmap: mapped at 0x%llX size=0x%X relocs=%u imports=%u dllmain=%llu in %ums\n",
                   (unsigned long long)mm.base, mm.size, mm.relocs, mm.imports,
                   (unsigned long long)mm.dll_main, mm.waited_ms);
            return 0;
        }
        printf("mmap: failed (see lines above)\n");
        return 6;
    } else if (!_stricmp(cmd, "apcdll")) {
        return rr_il_domain_probe(stdout) ? 0 : 5;
    } else if (!_stricmp(cmd, "probe")) {
        rr_il_probe(stdout);
    } else if (!_stricmp(cmd, "peek")) {
        dump(parse_hex(argc > 2 ? argv[2] : NULL), argc > 3 ? (unsigned)atoi(argv[3]) : 64);
    } else if (!_stricmp(cmd, "regions")) {
        region_report(argc > 2 ? atoi(argv[2]) : 12);
    } else if (!_stricmp(cmd, "region")) {
        RR_REGION r;
        uint64_t a = parse_hex(argc > 2 ? argv[2] : NULL);
        if (rr_query_region(a, &r))
            printf("0x%llX base=0x%llX size=0x%llX state=0x%X protect=0x%X type=0x%X\n",
                   (unsigned long long)a, (unsigned long long)r.base,
                   (unsigned long long)r.size, r.state, r.protect, r.type);
        else
            printf("region: query failed\n");
    } else if (!_stricmp(cmd, "handle")) {
        handle_report();
    } else if (!_stricmp(cmd, "remote")) {
        remote_report();
    } else if (!_stricmp(cmd, "find")) {
        find_string(argc > 2 ? argv[2] : "",
                    argc > 3 ? _strtoui64(argv[3], NULL, 10) : 512ull,
                    argc > 4 ? atoi(argv[4]) : 6);
    } else if (!_stricmp(cmd, "xref")) {
        find_qword(parse_hex(argc > 2 ? argv[2] : NULL), argc > 3 ? atoi(argv[3]) : 12);
    } else if (!_stricmp(cmd, "where")) {
        where_report(parse_hex(argc > 2 ? argv[2] : NULL));
    } else {
        printf("usage: rr_probe load <sys>|dse <value>|dseload <sys>|chan|"
               "module|exports [substr]|probe|apc|apcdll|mapdll <dll> [import] [module]|"
               "mmap <dll>|"
               "iatslot <file> [dll] [func] [target-module]|"
               "peek <hex> [len]|"
               "regions [n]|region <hex>|handle|remote|find <ascii> [mb] [hits]|xref <hex> [hits]|"
               "where <hex>|kmods [substr]|kmraw|kread <hex> [len]|kwrite <hex> <bytes>|"
               "kdump <hex> <len> <file>\n");
        return 4;
    }
    return 0;
}
