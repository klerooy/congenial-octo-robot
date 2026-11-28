/* rr_exec.c - client for the APC channel driver (3_src_driver/rr_exec.c).
 *
 * Transport: a session-local section the client creates and the driver opens.
 * No file, no device handle, no elevation, no handle into the target.
 *
 *   0x00  REQ  0x28  { magic, seq, op, pid, tid, pad, routine, context }
 *   0x40  REP  0x18  { magic, seq, status, stage, extra }
 *   0x80  u32  generation - every mapped image takes the channel by bumping it
 *
 * A request carries a sequence number and the reply echoes it, so a late reply
 * to an earlier request is never mistaken for the answer to this one.
 */
#include "rr_exec.h"

#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

#define RR_SECTION_BYTES 0x1000u
#define RR_OFF_REQ 0x00u
#define RR_OFF_REP 0x40u
#define RR_OFF_GEN 0x80u
/* Decoded at runtime so the ASCII markers do not sit in the DLL. The values
 * are the same as the driver's, so the wire format is unchanged. */
#define RR_WIRE_KEY 0x5A5A5A6Au
#define RR_REQ_MAGIC_ENC (0x31515252u ^ RR_WIRE_KEY)
#define RR_REP_MAGIC_ENC (0x31505252u ^ RR_WIRE_KEY)
static volatile uint32_t g_wire_key = RR_WIRE_KEY;
static uint32_t rr_req_magic(void) { return RR_REQ_MAGIC_ENC ^ g_wire_key; }
static uint32_t rr_rep_magic(void) { return RR_REP_MAGIC_ENC ^ g_wire_key; }
#define RR_OP_PING 0x00u
#define RR_OP_APC 0x01u
#define RR_OP_PROTECT 0x02u

/* Same suffix as the driver, XOR 0xA7. The "Local\" prefix keeps the section
 * in this session; the driver probes session paths 0-15 for it. */
static const unsigned char g_name_enc[] = {
    0xF0, 0x94, 0x95, 0xE9, 0xD3, 0xC1, 0xDE, 0xF7,
    0xC8, 0xD5, 0xD3, 0xF8, 0x90, 0xE6, 0x94, 0xE1
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, seq, op, pid, tid, pad;
    uint64_t routine, context;
} RR_REQ_WIRE;

typedef struct {
    uint32_t magic, seq;
    int32_t status;
    uint32_t stage;
    uint64_t extra;
} RR_REP_WIRE;
#pragma pack(pop)

static HANDLE g_map = NULL;
static volatile unsigned char* g_view = NULL;
static uint32_t g_seq;
static CRITICAL_SECTION g_lock;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK lock_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void lock(void) {
    InitOnceExecuteOnce(&g_once, lock_init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

static void unlock(void) {
    LeaveCriticalSection(&g_lock);
}

static int ch_name(char* out, int cap) {
    static const char prefix[] = "Local\\";
    int n = 0;
    for (int i = 0; prefix[i]; i++) {
        if (n + 1 >= cap) return 0;
        out[n++] = prefix[i];
    }
    for (unsigned i = 0; i < sizeof g_name_enc; i++) {
        if (n + 1 >= cap) return 0;
        out[n++] = (char)(g_name_enc[i] ^ 0xA7);
    }
    out[n] = 0;
    return 1;
}

static int ch_open(void) {
    char name[64];
    if (g_view) return 1;
    if (!ch_name(name, (int)sizeof name)) return 0;
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                               0, RR_SECTION_BYTES, name);
    if (!g_map) return 0;
    g_view = (volatile unsigned char*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS,
                                                    0, 0, 0);
    if (!g_view) {
        CloseHandle(g_map);
        g_map = NULL;
        return 0;
    }
    if (!g_seq) g_seq = GetTickCount() & 0x7FFFFFFFu; /* survive a client restart */
    return 1;
}

static void send_request(uint32_t op, uint32_t pid, uint32_t tid, uint64_t routine,
                         uint64_t context, uint32_t* seq_out) {
    RR_REQ_WIRE* q = (RR_REQ_WIRE*)(g_view + RR_OFF_REQ);
    memset((void*)q, 0, sizeof *q);
    q->seq = ++g_seq;
    q->op = op;
    q->pid = pid;
    q->tid = tid;
    q->routine = routine;
    q->context = context;
    MemoryBarrier();
    q->magic = rr_req_magic();
    *seq_out = q->seq;
}

static int wait_reply(uint32_t seq, int timeout_ms, RR_REP_WIRE* out) {
    RR_REP_WIRE* r = (RR_REP_WIRE*)(g_view + RR_OFF_REP);
    DWORD t0 = GetTickCount();
    for (;;) {
        if (r->magic == rr_rep_magic() && r->seq == seq) {
            if (out) *out = *r;
            return 1;
        }
        if ((int)(GetTickCount() - t0) >= timeout_ms) return 0;
        Sleep(1);
    }
}

int rr_exec_ping_gen(int timeout_ms, uint32_t* generation) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    int ok;
    if (generation) *generation = 0;
    lock();
    if (!ch_open()) { unlock(); return 0; }
    send_request(RR_OP_PING, 0, 0, 0, 0, &seq);
    ok = wait_reply(seq, timeout_ms, &r) && r.stage == 0x50;
    if (ok && generation) *generation = (uint32_t)r.extra;
    unlock();
    return ok;
}

int rr_exec_ping(int timeout_ms) {
    return rr_exec_ping_gen(timeout_ms, NULL);
}

int rr_exec_apc(uint32_t pid, uint32_t tid, uint64_t routine, uint64_t context,
                long* status, unsigned* stage) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    int ok;
    if (status) *status = 0;
    if (stage) *stage = 0;
    lock();
    if (!ch_open()) { unlock(); return 0; }
    send_request(RR_OP_APC, pid, tid, routine, context, &seq);
    ok = wait_reply(seq, 3000, &r);
    if (ok) {
        if (status) *status = (long)r.status;
        if (stage) *stage = r.stage;
    }
    unlock();
    return ok;
}

int rr_exec_protect(uint32_t pid, uint64_t addr, uint64_t size, uint32_t protect,
                    long* status, unsigned* stage, uint32_t* old_protect) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    int ok;
    if (status) *status = 0;
    if (stage) *stage = 0;
    if (old_protect) *old_protect = 0;
    lock();
    if (!ch_open()) { unlock(); return 0; }
    /* The wire reuses tid for the new protection: an APC needs it, a protect
     * request cannot be confused with one because the op field separates them. */
    send_request(RR_OP_PROTECT, pid, protect, addr, size, &seq);
    ok = wait_reply(seq, 3000, &r);
    if (ok) {
        if (status) *status = (long)r.status;
        if (stage) *stage = r.stage;
        if (old_protect) *old_protect = (uint32_t)r.extra;
    }
    unlock();
    return ok;
}

int rr_thread_ids(uint32_t pid, uint32_t* out, int cap) {
    THREADENTRY32 te;
    HANDLE snap;
    int n = 0;

    if (!out || cap <= 0) return 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    memset(&te, 0, sizeof te);
    te.dwSize = (DWORD)sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID && n < cap)
                out[n++] = te.th32ThreadID;
            te.dwSize = (DWORD)sizeof te;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}
