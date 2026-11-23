/* acphal.dll: the console's audio coprocessor (ACP) and its memory (the APU heap).
 *
 * Unreal titles create the APU heap at startup and stop with a fatal error if that fails
 * (a UE5 title: "Failed to create APU heap: 80004001" with a stub). Here the heap is ordinary
 * memory and the coprocessor (SHAPE: XMA decoding, sample-rate conversion, mixing) isn't
 * emulated: IAcpHal acknowledges commands and ticks audio frames (below), and XMA is decoded
 * on the CPU by shims/xaudio2_9 instead. Signatures and the SHAPE_CONTEXTS layout follow TomBox's notes, read
 * from a UE5 title's call sites (facts only):
 *   HRESULT ApuCreateHeap(UINT32 size, UINT32 flags)
 *   HRESULT ApuAlloc(void **block, APU_ADDRESS *address, UINT32 size, UINT32 alignment, UINT32 flags)
 *       APU_ADDRESS is 32 bits: the block's offset in the heap
 *   HRESULT ApuFree(void *block)
 *   HRESULT ApuHeapGetState(UINT32 *state, UINT32 flags): state[0] bytes free, state[1] in use
 *   HRESULT AcpHalCreate(IAcpHal **hal)
 *   HRESULT AcpHalAllocateShapeContexts(SHAPE_CONTEXTS *contexts)
 *       six UINT32 counts (SRC, EQ/compressor, filter/volume, DMA, XMA, PCM) at +0x00..+0x14,
 *       then the six arrays' addresses at +0x18..+0x40; an XMA context is 64 bytes */

#include <windows.h>

#include "../common/rebox_log.h"

static SRWLOCK lock = SRWLOCK_INIT;
static BYTE *heap;
static SIZE_T heap_size, heap_used, outside;

HRESULT WINAPI ApuCreateHeap(UINT32 size, UINT32 flags)
{
    HRESULT hr = S_OK;

    AcquireSRWLockExclusive(&lock);
    if (!heap)
    {
        heap_size = size > 0x10000 ? size : 0x10000;
        if (!(heap = VirtualAlloc(NULL, heap_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
            hr = E_OUTOFMEMORY;
        rebox_log("acphal: APU heap of %u bytes (flags %#x) %s", size, flags, heap ? "created" : "failed");
    }
    ReleaseSRWLockExclusive(&lock);
    return hr;
}

HRESULT WINAPI ApuAlloc(void **block, UINT32 *address, UINT32 size, UINT32 alignment, UINT32 flags)
{
    SIZE_T align = alignment > 16 ? alignment : 16, start;
    void *p = NULL;

    if (!block)
        return E_INVALIDARG;
    if (!heap)
        ApuCreateHeap(64 << 20, 0);
    AcquireSRWLockExclusive(&lock);
    start = (heap_used + align - 1) & ~(align - 1);
    if (heap && start + size <= heap_size)
    {
        p = heap + start;
        heap_used = start + size;
        if (address)
            *address = (UINT32)start;
    }
    ReleaseSRWLockExclusive(&lock);
    if (!p)
    {
        /* Heap full: memory of its own, with an address past the heap's. */
        static LONG warned;
        if (!(p = VirtualAlloc(NULL, size ? size : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
            return E_OUTOFMEMORY;
        if (!InterlockedExchange(&warned, 1))
            rebox_log("acphal: APU heap full (%zu bytes); further blocks get memory of their own", heap_size);
        AcquireSRWLockExclusive(&lock);
        if (address)
            *address = (UINT32)(heap_size + outside);
        outside += size;
        ReleaseSRWLockExclusive(&lock);
    }
    *block = p;
    return S_OK;
}

/* Blocks come from a bump allocator: freeing one returns nothing to the heap. */
HRESULT WINAPI ApuFree(void *block)
{
    return S_OK;
}

HRESULT WINAPI ApuHeapGetState(UINT32 *state, UINT32 flags)
{
    if (!state)
        return E_INVALIDARG;
    AcquireSRWLockShared(&lock);
    state[0] = (UINT32)(heap_size - heap_used);
    state[1] = (UINT32)heap_used;
    ReleaseSRWLockShared(&lock);
    return S_OK;
}

/* IAcpHal: the coprocessor's command and message channel. The layout and the behaviour follow
 * xcl (ivall/xcl, GPL-2.0-only, crates/xcl/src/hle/acphal.rs): the vtable is
 *   0 HRESULT Connect(UINT32 commands, UINT32 messages)   1 HRESULT Disconnect()
 *   2 HRESULT SubmitCommand(UINT32 type, UINT64 id, UINT32 frame, const void *data, UINT32 size)
 *   3 BOOL PopMessage(ACP_MESSAGE *message)               4 UINT32 GetNumMessages()
 *   5 void Release()
 * and a message is { UINT32 type; UINT32 reserved; UINT64 value; UINT64 id; }: 2 an audio frame,
 * 0x10 a command done (its id), 0x80 disconnected. Nothing runs here, but the channel answers
 * as hardware with nothing to do: each command is done at once, and an audio frame comes every
 * 128 samples at 48 kHz. Titles wait for both (a GDK title submits a command at startup and polls
 * PopMessage until it is done: with no answer it spun there forever, at 34% loaded). */
enum { ACP_AUDIO_FRAME = 2, ACP_COMMAND_DONE = 0x10, ACP_DISCONNECTED = 0x80 };
#define ACP_QUEUE 1024
#define HAL_SLOTS 32

struct acp_message { UINT32 type, reserved; UINT64 value, id; };

struct hal
{
    void *const *vtbl;
    SRWLOCK lock;
    BOOL connected;
    struct acp_message queue[ACP_QUEUE];
    UINT head, count;
    LONGLONG next_frame, frame_ticks; /* QueryPerformanceCounter ticks */
};

static void hal_log(UINT slot, UINT64 a, UINT64 b, UINT64 c)
{
    static LONG seen[HAL_SLOTS];
    /* Saturating: a title polling a slot millions of times a second would wrap a counter. */
    if (seen[slot] < 2 && InterlockedIncrement(&seen[slot]) <= 2)
        rebox_log("acphal: IAcpHal slot %u (%#llx, %#llx, %#llx)", slot, (unsigned long long)a,
                     (unsigned long long)b, (unsigned long long)c);
}

static LONGLONG now_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void hal_push(struct hal *h, UINT32 type, UINT64 id)
{
    struct acp_message *m;
    if (h->count == ACP_QUEUE)
        return; /* nobody is reading: drop it */
    m = &h->queue[(h->head + h->count++) % ACP_QUEUE];
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->id = id;
}

static BOOL frame_due(struct hal *h, LONGLONG now)
{
    return h->connected && now >= h->next_frame;
}

static HRESULT WINAPI hal_connect(struct hal *h, UINT32 commands, UINT32 messages)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    AcquireSRWLockExclusive(&h->lock);
    h->connected = TRUE;
    h->frame_ticks = f.QuadPart * 128 / 48000;
    h->next_frame = now_ticks() + h->frame_ticks;
    ReleaseSRWLockExclusive(&h->lock);
    rebox_log("acphal: coprocessor connected (%u commands, %u messages): commands are done at once, "
                 "nothing runs", commands, messages);
    return S_OK;
}

static HRESULT WINAPI hal_disconnect(struct hal *h)
{
    AcquireSRWLockExclusive(&h->lock);
    h->connected = FALSE;
    hal_push(h, ACP_DISCONNECTED, 0);
    ReleaseSRWLockExclusive(&h->lock);
    return S_OK;
}

static HRESULT WINAPI hal_submit(struct hal *h, UINT32 type, UINT64 id, UINT32 frame, const void *data, UINT32 size)
{
    static UINT32 seen[64];
    static LONG seen_count;
    BOOL connected, known = FALSE;

    for (LONG i = 0; i < seen_count && i < 64; i++)
        known |= seen[i] == type;
    if (!known && seen_count < 64)
    {
        seen[seen_count++] = type;
        rebox_log("acphal: coprocessor command %#x first seen (id %#llx, frame %#x, %u bytes)", type,
                     (unsigned long long)id, frame, size);
    }
    AcquireSRWLockExclusive(&h->lock);
    if ((connected = h->connected) && id)
        hal_push(h, ACP_COMMAND_DONE, id); /* id 0: nobody waits for it */
    ReleaseSRWLockExclusive(&h->lock);
    return connected ? S_OK : E_UNEXPECTED;
}

/* The next message: queued ones first, else an audio frame when one is due (more than 8 frames
 * late are not made up for). */
static BOOL WINAPI hal_pop(struct hal *h, struct acp_message *out)
{
    BOOL got = FALSE;
    LONGLONG now;

    if (!out)
        return FALSE;
    now = now_ticks();
    AcquireSRWLockExclusive(&h->lock);
    if (h->count)
    {
        *out = h->queue[h->head];
        h->head = (h->head + 1) % ACP_QUEUE;
        h->count--;
        got = TRUE;
    }
    else if (frame_due(h, now))
    {
        LONGLONG next = h->next_frame + h->frame_ticks, floor = now - 8 * h->frame_ticks;
        h->next_frame = next > floor ? next : floor;
        memset(out, 0, sizeof(*out));
        out->type = ACP_AUDIO_FRAME;
        got = TRUE;
    }
    ReleaseSRWLockExclusive(&h->lock);
    return got;
}

static UINT32 WINAPI hal_count(struct hal *h)
{
    UINT32 n;
    LONGLONG now = now_ticks();
    AcquireSRWLockShared(&h->lock);
    n = h->count + (frame_due(h, now) ? 1 : 0);
    ReleaseSRWLockShared(&h->lock);
    return n;
}

static void WINAPI hal_release(struct hal *h)
{
    HeapFree(GetProcessHeap(), 0, h);
}

/* Slots past Release aren't known: they log and succeed. */
#define HAL(n) static HRESULT WINAPI hal_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { hal_log(n, a, b, c); return S_OK; }
HAL(6) HAL(7) HAL(8) HAL(9) HAL(10) HAL(11) HAL(12) HAL(13) HAL(14) HAL(15)
HAL(16) HAL(17) HAL(18) HAL(19) HAL(20) HAL(21) HAL(22) HAL(23) HAL(24) HAL(25) HAL(26) HAL(27) HAL(28) HAL(29)
HAL(30) HAL(31)

HRESULT WINAPI AcpHalCreate(void **out)
{
    static void *const methods[HAL_SLOTS] = {
        hal_connect, hal_disconnect, hal_submit, hal_pop, hal_count, hal_release, hal_6, hal_7, hal_8, hal_9,
        hal_10, hal_11, hal_12, hal_13, hal_14, hal_15, hal_16, hal_17, hal_18, hal_19, hal_20, hal_21, hal_22,
        hal_23, hal_24, hal_25, hal_26, hal_27, hal_28, hal_29, hal_30, hal_31,
    };
    struct hal *h;

    if (!out)
        return E_INVALIDARG;
    if (!(h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h))))
        return E_OUTOFMEMORY;
    h->vtbl = methods;
    InitializeSRWLock(&h->lock);
    *out = h;
    rebox_log("acphal: AcpHalCreate: a coprocessor channel that acknowledges commands (no SHAPE)");
    return S_OK;
}

HRESULT WINAPI AcpHalAllocateShapeContexts(BYTE *contexts)
{
    static const char *const names[6] = { "SRC", "EQ/compressor", "filter/volume", "DMA", "XMA", "PCM" };
    UINT32 *counts = (UINT32 *)contexts;

    if (!contexts)
        return E_INVALIDARG;
    for (int i = 0; i < 6; i++)
    {
        /* 64 bytes is an XMA context; the others' sizes aren't known, so they get 512 each. */
        SIZE_T each = i == 4 ? 64 : 512;
        void *array = NULL;
        UINT32 address = 0;

        if (counts[i] && FAILED(ApuAlloc(&array, &address, (UINT32)(counts[i] * each), 256, 0)))
            return E_OUTOFMEMORY;
        if (array)
            memset(array, 0, counts[i] * each);
        /* Only kinds asked for: FMOD's struct has five arrays (xcl), the sixth count reserved. */
        if (counts[i])
            memcpy(contexts + 0x18 + 8 * i, &array, sizeof(array));
        if (counts[i])
            rebox_log("acphal: %u %s contexts at %p", counts[i], names[i], array);
    }
    return S_OK;
}

/* FMOD (a GDK title) also imports these (xcl has the same meanings). */
HRESULT WINAPI AcpHalReleaseShapeContexts(void)
{
    return S_OK; /* the arrays stay in the heap, which never gives memory back */
}

/* An APU address for the title's own memory: its heap offset if it's in the heap, else a new
 * address past the heap's (as ApuAlloc gives a block that didn't fit). */
HRESULT WINAPI ApuMapVirtualAddress(UINT32 *address, void *va, UINT32 size)
{
    if (!address)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    if (heap && (BYTE *)va >= heap && (BYTE *)va < heap + heap_size)
        *address = (UINT32)((BYTE *)va - heap);
    else
    {
        *address = (UINT32)(heap_size + outside);
        outside += (size + 0xfff) & ~0xfff;
    }
    ReleaseSRWLockExclusive(&lock);
    return S_OK;
}

/* All of the process's memory is the coprocessor's here. */
BOOL WINAPI ApuIsVirtualAddressValid(const void *va, UINT32 size)
{
    return TRUE;
}
