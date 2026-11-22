/* acphal.dll: the console's audio coprocessor (ACP) and its memory (the APU heap).
 *
 * Unreal titles create the APU heap at startup and stop with a fatal error if that fails
 * (a UE5 title: "Failed to create APU heap: 80004001" with a stub). Here the heap is ordinary
 * memory and the coprocessor (SHAPE: XMA decoding, sample-rate conversion, mixing) isn't
 * emulated: IAcpHal is an inert object that logs its calls, and XMA is decoded on the CPU by
 * shims/xaudio2_9 instead. Signatures and the SHAPE_CONTEXTS layout follow TomBox's notes, read
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

/* IAcpHal: every method logs its first calls and succeeds; nothing is ever processed. */
#define HAL_SLOTS 32
static void *hal_vtbl[HAL_SLOTS];
static void *const hal_object = hal_vtbl;

static HRESULT hal_method(UINT slot, UINT64 a, UINT64 b, UINT64 c)
{
    static LONG seen[HAL_SLOTS];
    if (InterlockedIncrement(&seen[slot]) <= 2)
        rebox_log("acphal: IAcpHal slot %u (%#llx, %#llx, %#llx): no coprocessor, S_OK", slot,
                     (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
    return S_OK;
}

#define HAL(n) static HRESULT WINAPI hal_##n(void *self, UINT64 a, UINT64 b, UINT64 c) { return hal_method(n, a, b, c); }
HAL(0) HAL(1) HAL(2) HAL(3) HAL(4) HAL(5) HAL(6) HAL(7) HAL(8) HAL(9) HAL(10) HAL(11) HAL(12) HAL(13) HAL(14) HAL(15)
HAL(16) HAL(17) HAL(18) HAL(19) HAL(20) HAL(21) HAL(22) HAL(23) HAL(24) HAL(25) HAL(26) HAL(27) HAL(28) HAL(29)
HAL(30) HAL(31)

HRESULT WINAPI AcpHalCreate(void **hal)
{
    static void *const methods[HAL_SLOTS] = {
        hal_0, hal_1, hal_2, hal_3, hal_4, hal_5, hal_6, hal_7, hal_8, hal_9, hal_10, hal_11, hal_12, hal_13, hal_14,
        hal_15, hal_16, hal_17, hal_18, hal_19, hal_20, hal_21, hal_22, hal_23, hal_24, hal_25, hal_26, hal_27,
        hal_28, hal_29, hal_30, hal_31,
    };

    if (!hal)
        return E_INVALIDARG;
    memcpy(hal_vtbl, methods, sizeof(methods));
    *hal = (void *)&hal_object;
    rebox_log("acphal: AcpHalCreate: an inert IAcpHal (no SHAPE coprocessor)");
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
        memcpy(contexts + 0x18 + 8 * i, &array, sizeof(array));
        if (counts[i])
            rebox_log("acphal: %u %s contexts at %p", counts[i], names[i], array);
    }
    return S_OK;
}
