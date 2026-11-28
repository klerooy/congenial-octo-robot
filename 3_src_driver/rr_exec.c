/* rr_exec.c v2 — Callout + Shared Memory channel driver.
 *
 * Architectural rewrite to eliminate all four EAC detection vectors:
 *   1. System thread stack trace -> callout thread with IRETQ mechanism
 *   2. BigPool PE+import heuristic -> no PE header, imports resolved at map time
 *   3. File channel polling -> shared memory section (no ZwCreateFile)
 *   4. APC NormalRoutine unbacked -> optional; prefer in-process command queue
 *
 * Built without WDK headers (same as v1). All kernel declarations are the
 * documented x64 shape. Import library generated from ntoskrnl export table.
 *
 * Channel: named section object, mapped into both usermode and kernel.
 *   Layout identical to v1 for client compatibility:
 *     0x00 REQ 0x30 { magic, seq, op, pid, tid, pad, routine, context }
 *     0x40 REP 0x18 { magic, seq, status, stage, extra }
 *     0x80 ULONG generation
 *
 * The section name is passed through a bootstrap handshake page so it never
 * appears as a string in the driver image. Usermode creates the section before
 * mapping; the driver receives the name via the initial mapped view.
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long               ntstatus;

#define NT_SUCCESS(s)                  ((s) >= 0)
#define STATUS_SUCCESS                 ((ntstatus)0x00000000)
#define STATUS_UNSUCCESSFUL            ((ntstatus)0xC0000001)
#define STATUS_INVALID_PARAMETER       ((ntstatus)0xC000000D)
#define STATUS_INSUFFICIENT_RESOURCES  ((ntstatus)0xC000009A)
#define STATUS_NOT_FOUND               ((ntstatus)0xC0000225)
#define STATUS_ACCESS_DENIED           ((ntstatus)0xC0000022)

#define POOL_NX             0x200u
#define APC_BYTES           0x80u
#define REAL_STACK_SIZE     0x4000u
#define SECTION_VIEW_SIZE   0x1000u

unsigned __int64 __rdtsc(void);
#pragma intrinsic(__rdtsc)

static u32 g_pool_tag;
static u32 make_pool_tag(void) {
    unsigned __int64 t = __rdtsc();
    u32 tag = (u32)(t ^ (t >> 32)) ^ 0x9E3779B9u;
    return tag ? tag : 1u;
}

#define WIRE_KEY            0x5A5A5A6Au
#define REQ_MAGIC_ENC       (0x31515252u ^ WIRE_KEY)
#define REP_MAGIC_ENC       (0x31505252u ^ WIRE_KEY)
static volatile u32 g_wire_key = WIRE_KEY;
static u32 req_magic(void) { return REQ_MAGIC_ENC ^ g_wire_key; }
static u32 rep_magic(void) { return REP_MAGIC_ENC ^ g_wire_key; }

#define OFF_REQ             0x00u
#define OFF_REP             0x40u
#define OFF_GEN             0x80u

#define OP_PING             0x00u
#define OP_APC              0x01u
#define OP_PROTECT          0x02u
#define OP_STOP             0xFEu

#define PROT_PAGE_MASK      0x6Eu

typedef struct { u16 Length; u16 MaximumLength; unsigned short* Buffer; } USTR;
typedef struct {
    u32 Length; u32 Pad0; void* RootDirectory; USTR* ObjectName;
    u32 Attributes; u32 Pad1; void* SecurityDescriptor; void* SecurityQualityOfService;
} OBJ;
typedef struct { ntstatus Status; u32 Pad; u64 Information; } IOSB;

typedef struct {
    u32 magic, seq, op, pid, tid, pad;
    u64 routine, context;
} REQ;

typedef struct {
    u32 magic, seq;
    ntstatus status;
    u32 stage;
    u64 extra;
} REP;

__declspec(dllimport) ntstatus PsLookupProcessByProcessId(void* pid, void** out);
__declspec(dllimport) ntstatus PsLookupThreadByThreadId(void* tid, void** out);
__declspec(dllimport) void*    PsGetThreadProcess(void* thread);
__declspec(dllimport) char*    PsGetProcessImageFileName(void* process);
__declspec(dllimport) void     ObfDereferenceObject(void* obj);
__declspec(dllimport) void*    ExAllocatePoolWithTag(u32 pool, u64 size, u32 tag);
__declspec(dllimport) void     ExFreePoolWithTag(void* p, u32 tag);
__declspec(dllimport) void     KeInitializeApc(void* apc, void* thread, u8 state,
                                               void* kernelRoutine, void* rundownRoutine,
                                               void* normalRoutine, u8 mode, void* ctx);
__declspec(dllimport) u8       KeInsertQueueApc(void* apc, void* a1, void* a2, u8 boost);
__declspec(dllimport) ntstatus PsCreateSystemThread(void** handle, u32 access, void* objAttr,
                                                    void* process, void* clientId,
                                                    void* startRoutine, void* startContext);
__declspec(dllimport) ntstatus PsTerminateSystemThread(ntstatus code);
__declspec(dllimport) ntstatus KeDelayExecutionThread(u8 waitMode, u8 alertable, long long* interval);
__declspec(dllimport) void     RtlInitUnicodeString(USTR* dst, const unsigned short* src);
__declspec(dllimport) void     KeStackAttachProcess(void* process, void* apcState);
__declspec(dllimport) void     KeUnstackDetachProcess(void* apcState);
__declspec(dllimport) ntstatus ZwProtectVirtualMemory(void* handle, void** base, u64* size,
                                                     u32 protect, u32* old);
__declspec(dllimport) ntstatus ZwClose(void* handle);
__declspec(dllimport) ntstatus MmMapViewOfSection(void* section, void* process,
                                                   void** base, u64 zeroBits, u64 commitSize,
                                                   void* offset, u64* viewSize, u32 inherit,
                                                   u32 allocType, u32 protect);
__declspec(dllimport) ntstatus ZwOpenSection(void** handle, u32 access, OBJ* oa);

static void*    g_section_handle;
static void*    g_mapped_view;
static u32      g_generation;
static volatile long g_stop;
void*           g_real_stack;
void*           g_iret_gadget;      /* Set by mapper before DriverEntry */
void*           g_ret_gadget;       /* Set by mapper before DriverEntry */
u64             g_kernel_stack_base;/* Captured at callout_bootstrap entry */

static u32 g_name_salt;
static u32 salted_name_hash(const char* s) {
    u32 h = 2166136261u ^ g_name_salt;
    if (!s) return 0;
    while (*s) { h ^= (u8)*s++; h *= 16777619u; }
    return h;
}
static u32 g_target_hash;

static int image_is_target(void* process) {
    return salted_name_hash(PsGetProcessImageFileName(process)) == g_target_hash;
}

static void zero(void* p, u32 n) { u8* b = (u8*)p; while (n--) *b++ = 0; }

static void sleep_ms(long ms) {
    long long interval = -10000LL * (long long)ms;
    KeDelayExecutionThread(0, 0, &interval);
}

static int resolve_gadgets(void) {
    if (!g_iret_gadget || !g_ret_gadget) return 0;
    return 1;
}

static int channel_open_from_section(void* section_name_utf16, u32 name_len) {
    USTR name; OBJ oa; ntstatus st; u64 view_size = SECTION_VIEW_SIZE;
    if (g_mapped_view) return 1;
    name.Buffer = (unsigned short*)section_name_utf16;
    name.Length = (u16)(name_len * 2);
    name.MaximumLength = name.Length;
    zero(&oa, sizeof oa);
    oa.Length = sizeof(OBJ); oa.ObjectName = &name; oa.Attributes = 0x40;
    st = ZwOpenSection(&g_section_handle, 0xF001F, &oa);
    if (!NT_SUCCESS(st)) return 0;
    st = MmMapViewOfSection(g_section_handle, (void*)-1, &g_mapped_view,
                            0, 0, 0, &view_size, 2, 0, 0x04);
    if (!NT_SUCCESS(st)) {
        ZwClose(g_section_handle); g_section_handle = 0; return 0;
    }
    return 1;
}

static void channel_close(void) {
    g_mapped_view = 0;
    if (g_section_handle) { ZwClose(g_section_handle); g_section_handle = 0; }
}

static void* channel_ptr(u32 offset) {
    if (!g_mapped_view) return 0;
    return (u8*)g_mapped_view + offset;
}

static void apc_kernel(void* apc, void** normal, void** ctx, void** a1, void** a2) {
    (void)normal; (void)ctx; (void)a1; (void)a2;
    ExFreePoolWithTag(apc, g_pool_tag);
}
static void apc_rundown(void* apc) { ExFreePoolWithTag(apc, g_pool_tag); }

static ntstatus queue_user_apc(REQ* q, REP* r) {
    void* process = 0; void* thread = 0; ntstatus st; void* apc;
    if (!q->pid || !q->tid || !q->routine) return STATUS_INVALID_PARAMETER;
    if (q->routine > 0x7FFFFFFEFFFFull) return STATUS_INVALID_PARAMETER;
    st = PsLookupProcessByProcessId((void*)(u64)q->pid, &process);
    if (!NT_SUCCESS(st)) { r->stage = 1; return st; }
    if (!image_is_target(process)) {
        ObfDereferenceObject(process); r->stage = 2; return STATUS_ACCESS_DENIED;
    }
    st = PsLookupThreadByThreadId((void*)(u64)q->tid, &thread);
    if (!NT_SUCCESS(st)) { ObfDereferenceObject(process); r->stage = 3; return st; }
    if (PsGetThreadProcess(thread) != process) {
        st = STATUS_NOT_FOUND; r->stage = 4; goto done;
    }
    apc = ExAllocatePoolWithTag(POOL_NX, APC_BYTES, g_pool_tag);
    if (!apc) { st = STATUS_INSUFFICIENT_RESOURCES; r->stage = 5; goto done; }
    zero(apc, APC_BYTES);
    KeInitializeApc(apc, thread, 0, apc_kernel, apc_rundown,
                    (void*)q->routine, 1, (void*)q->context);
    if (!KeInsertQueueApc(apc, 0, 0, 0)) {
        ExFreePoolWithTag(apc, g_pool_tag);
        st = STATUS_UNSUCCESSFUL; r->stage = 6; goto done;
    }
    r->stage = 7; st = STATUS_SUCCESS;
done:
    ObfDereferenceObject(thread); ObfDereferenceObject(process);
    return st;
}

static ntstatus protect_range(REQ* q, REP* r) {
    void* process = 0; ntstatus st;
    u64 base, size; u32 protect = q->tid, old = 0;
    u8 apc_state[0x80];
    if (!q->pid || !q->routine) return STATUS_INVALID_PARAMETER;
    if (q->routine > 0x7FFFFFFEFFFFull) return STATUS_INVALID_PARAMETER;
    if (!protect || (protect & ~PROT_PAGE_MASK)) return STATUS_INVALID_PARAMETER;
    st = PsLookupProcessByProcessId((void*)(u64)q->pid, &process);
    if (!NT_SUCCESS(st)) { r->stage = 1; return st; }
    if (!image_is_target(process)) {
        ObfDereferenceObject(process); r->stage = 2; return STATUS_ACCESS_DENIED;
    }
    zero(apc_state, sizeof apc_state);
    KeStackAttachProcess(process, apc_state);
    base = q->routine; size = q->context ? q->context : 0x1000u;
    st = ZwProtectVirtualMemory((void*)-1, (void**)&base, &size, protect, &old);
    KeUnstackDetachProcess(apc_state);
    ObfDereferenceObject(process);
    r->extra = (u64)old; r->stage = 3;
    return st;
}

/* worker_loop runs on real stack via callout_bootstrap.
 * All NT API calls here MUST go through callout_invoke() from callout.asm.
 * Direct calls leave unsigned return addresses on whatever stack is active.
 * For brevity this shows the logic; production version wraps every NT call
 * in callout_invoke(). The sleep_ms below is the one exception that must
 * also be wrapped. */
void worker_loop(void) {
    REQ q; REP r; u32 last = 0;
    while (!g_stop) {
        u32 gen;
        void* req_ptr = channel_ptr(OFF_REQ);
        void* rep_ptr = channel_ptr(OFF_REP);
        void* gen_ptr = channel_ptr(OFF_GEN);
        if (!req_ptr || !rep_ptr || !gen_ptr) { /* callout_invoke(sleep_ms,50) */ continue; }
        zero(&q, sizeof q);
        { volatile u8* src = (volatile u8*)req_ptr; u8* dst = (u8*)&q;
          for (u32 i = 0; i < sizeof q; ++i) dst[i] = src[i]; }
        gen = *(volatile u32*)gen_ptr;
        if (gen && gen > g_generation) break;
        if (q.magic != req_magic() || !q.seq || q.seq == last) {
            /* callout_invoke(sleep_ms,15) */ continue;
        }
        last = q.seq;
        zero(&r, sizeof r);
        r.magic = rep_magic(); r.seq = q.seq; r.status = STATUS_SUCCESS;
        if (q.op == OP_PING) { r.stage = 0x50; r.extra = (u64)g_generation; }
        else if (q.op == OP_APC) { r.status = queue_user_apc(&q, &r); }
        else if (q.op == OP_PROTECT) { r.status = protect_range(&q, &r); }
        else if (q.op == OP_STOP) {
            r.stage = 0x51;
            { volatile u8* dst = (volatile u8*)rep_ptr; u8* src = (u8*)&r;
              for (u32 i = 0; i < sizeof r; ++i) dst[i] = src[i]; }
            break;
        } else { r.status = STATUS_INVALID_PARAMETER; r.stage = 0x52; }
        { volatile u8* dst = (volatile u8*)rep_ptr; u8* src = (u8*)&r;
          for (u32 i = 0; i < sizeof r; ++i) dst[i] = src[i]; }
        /* callout_invoke(sleep_ms,15) */
    }
    channel_close();
    /* callout_invoke(PsTerminateSystemThread, STATUS_SUCCESS) */
}

ntstatus DriverEntry(void* driverObject, void* registryPath) {
    void* thread = 0; ntstatus st;
    (void)driverObject; (void)registryPath;
    g_pool_tag = make_pool_tag();
    g_name_salt = (u32)(__rdtsc() ^ (__rdtsc() >> 32));
    g_real_stack = ExAllocatePoolWithTag(POOL_NX, REAL_STACK_SIZE, g_pool_tag);
    if (!g_real_stack) return STATUS_INSUFFICIENT_RESOURCES;
    zero(g_real_stack, REAL_STACK_SIZE);
    if (!resolve_gadgets()) {
        ExFreePoolWithTag(g_real_stack, g_pool_tag);
        return STATUS_UNSUCCESSFUL;
    }
    /* Start address = callout_bootstrap (in signed-module-backed stub or
     * pre-resolved gadget). Mapper must set this to the correct entry.
     * Here we use g_iret_gadget as placeholder — mapper patches it. */
    st = PsCreateSystemThread(&thread, 0x1FFFFF, 0, 0, 0,
                              g_iret_gadget, 0);
    if (!NT_SUCCESS(st)) {
        ExFreePoolWithTag(g_real_stack, g_pool_tag); return st;
    }
    if (thread) ZwClose(thread);
    return STATUS_SUCCESS;
}