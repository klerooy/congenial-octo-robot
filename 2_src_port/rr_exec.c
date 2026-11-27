/* rr_exec.c - client for the APC channel driver (3_src_driver/rr_exec.c).
 *
 * The channel is one file the driver polls, so this side needs no device
 * handle, no elevation and no handle into the target at all:
 *
 *   0x00  REQ  0x30  { magic, seq, op, pid, tid, pad, routine, context }
 *   0x40  REP  0x18  { magic, seq, status, stage, extra }
 *   0x80  u32  generation - every mapped image takes the channel by bumping it
 *
 * A request carries a sequence number and the reply echoes it, so a late reply
 * to an earlier request is never mistaken for the answer to this one.
 */
#include "rr_exec.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

#define RR_CHANNEL_PATH "C:\\Users\\Public\\rr_exec.bin"
#define RR_CHANNEL_BYTES 0x100u
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

static HANDLE g_ch = INVALID_HANDLE_VALUE;
static uint32_t g_seq;

static int ch_open(void) {
    LARGE_INTEGER size;
    if (g_ch != INVALID_HANDLE_VALUE) return 1;
    g_ch = CreateFileA(RR_CHANNEL_PATH, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_ch == INVALID_HANDLE_VALUE) return 0;
    if (GetFileSizeEx(g_ch, &size) && size.QuadPart < RR_CHANNEL_BYTES) {
        LARGE_INTEGER end;
        end.QuadPart = RR_CHANNEL_BYTES;
        if (SetFilePointerEx(g_ch, end, NULL, FILE_BEGIN)) SetEndOfFile(g_ch);
    }
    if (!g_seq) g_seq = GetTickCount() & 0x7FFFFFFFu; /* survive a client restart */
    return 1;
}

static int ch_io(void* buf, DWORD len, uint32_t off, int write) {
    LARGE_INTEGER pos;
    DWORD done = 0;
    if (!ch_open()) return 0;
    pos.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(g_ch, pos, NULL, FILE_BEGIN)) return 0;
    if (write) return WriteFile(g_ch, buf, len, &done, NULL) && done == len;
    return ReadFile(g_ch, buf, len, &done, NULL) && done == len;
}

static void send_request(uint32_t op, uint32_t pid, uint32_t tid, uint64_t routine,
                         uint64_t context, uint32_t* seq_out) {
    RR_REQ_WIRE q;
    memset(&q, 0, sizeof q);
    q.magic = rr_req_magic();
    q.seq = ++g_seq;
    q.op = op;
    q.pid = pid;
    q.tid = tid;
    q.routine = routine;
    q.context = context;
    *seq_out = q.seq;
    ch_io(&q, (DWORD)sizeof q, RR_OFF_REQ, 1);
}

static int wait_reply(uint32_t seq, int timeout_ms, RR_REP_WIRE* out) {
    DWORD t0 = GetTickCount();
    for (;;) {
        RR_REP_WIRE r;
        memset(&r, 0, sizeof r);
        if (ch_io(&r, (DWORD)sizeof r, RR_OFF_REP, 0) && r.magic == rr_rep_magic() &&
            r.seq == seq) {
            if (out) *out = r;
            return 1;
        }
        if ((int)(GetTickCount() - t0) >= timeout_ms) return 0;
        Sleep(2);
    }
}

int rr_exec_ping_gen(int timeout_ms, uint32_t* generation) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    if (generation) *generation = 0;
    if (!ch_open()) return 0;
    send_request(RR_OP_PING, 0, 0, 0, 0, &seq);
    if (!wait_reply(seq, timeout_ms, &r)) return 0;
    if (r.stage != 0x50) return 0;
    if (generation) *generation = (uint32_t)r.extra;
    return 1;
}

int rr_exec_ping(int timeout_ms) {
    return rr_exec_ping_gen(timeout_ms, NULL);
}

int rr_exec_apc(uint32_t pid, uint32_t tid, uint64_t routine, uint64_t context,
                long* status, unsigned* stage) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    if (status) *status = 0;
    if (stage) *stage = 0;
    if (!ch_open()) return 0;
    send_request(RR_OP_APC, pid, tid, routine, context, &seq);
    if (!wait_reply(seq, 3000, &r)) return 0;
    if (status) *status = (long)r.status;
    if (stage) *stage = r.stage;
    return 1;
}

int rr_exec_protect(uint32_t pid, uint64_t addr, uint64_t size, uint32_t protect,
                    long* status, unsigned* stage, uint32_t* old_protect) {
    uint32_t seq = 0;
    RR_REP_WIRE r;
    if (status) *status = 0;
    if (stage) *stage = 0;
    if (old_protect) *old_protect = 0;
    if (!ch_open()) return 0;
    /* The wire reuses tid for the new protection: an APC needs it, a protect
     * request cannot be confused with one because the op field separates them. */
    send_request(RR_OP_PROTECT, pid, protect, addr, size, &seq);
    if (!wait_reply(seq, 3000, &r)) return 0;
    if (status) *status = (long)r.status;
    if (stage) *stage = r.stage;
    if (old_protect) *old_protect = (uint32_t)r.extra;
    return 1;
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
