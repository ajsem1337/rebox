/* Replacement for the console's xmem.dll.
 *
 * The XALLOC_ATTRIBUTES layout matches SlimEra's Xbox One ERA definition
 * (MIT): alignment 2^n with a 16-byte floor, and memory type 0 for the CPU
 * heap. Scarlett's own header hasn't been seen. Every allocation comes back
 * zeroed, as on the console. */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include "../common/rebox_log.h"

typedef union
{
    struct
    {
        ULONGLONG object_type : 14;
        ULONGLONG page_size : 2;
        ULONGLONG allocator_id : 8;
        ULONGLONG alignment : 5;
        ULONGLONG memory_type : 4;
        ULONGLONG reserved : 31;
    } s;
    ULONGLONG value;
} XALLOC_ATTRIBUTES;

typedef void *(WINAPI *XMEMALLOC_ROUTINE)(SIZE_T size, ULONGLONG attributes);
typedef void (WINAPI *XMEMFREE_ROUTINE)(void *address, ULONGLONG attributes);

static XMEMALLOC_ROUTINE alloc_hook;
static XMEMFREE_ROUTINE free_hook;

static SIZE_T attr_alignment(XALLOC_ATTRIBUTES attr)
{
    /* 2^n with a 16-byte floor, as in SlimEra's ERA XMemAllocDefault (MIT).
     * Xbox2Windows saw Bink pass n = 5 and rely on 32-byte alignment. */
    unsigned int shift = attr.s.alignment > 4 ? attr.s.alignment : 4;

    if (shift > 22)
        shift = 22;
    return (SIZE_T)1 << shift;
}

/* XALLOC_PAGESIZE_4KB, _64KB, _4MB (SlimEra). Allocations made of larger
 * pages start on a page boundary, as on the console; D3D12 placement of
 * resources at such addresses relies on 64 KiB alignment. */
static SIZE_T attr_page_bytes(XALLOC_ATTRIBUTES attr)
{
    switch (attr.s.page_size)
    {
    case 1: return 64 << 10;
    case 2: return 4 << 20;
    default: return 4096;
    }
}

/* VirtualAlloc only guarantees 64 KiB: reserve extra, then re-reserve the
 * aligned part (another thread can take it in between, so retry). */
static void *virtual_alloc_aligned(SIZE_T size, SIZE_T alignment)
{
    if (alignment <= (64 << 10))
        return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (int i = 0; i < 8; i++)
    {
        BYTE *probe = VirtualAlloc(NULL, size + alignment, MEM_RESERVE, PAGE_NOACCESS), *p;
        if (!probe)
            return NULL;
        p = (BYTE *)(((ULONG_PTR)probe + alignment - 1) & ~(ULONG_PTR)(alignment - 1));
        VirtualFree(probe, 0, MEM_RELEASE);
        if ((p = VirtualAlloc(p, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)))
            return p;
    }
    return NULL;
}

static BOOL wants_pages(XALLOC_ATTRIBUTES attr)
{
    /* Non-4K page sizes and very large alignments get their own VirtualAlloc
     * region. */
    return attr.s.page_size || attr.s.memory_type || attr_alignment(attr) > 4096;
}

/* ---------------------------------------------------------- GPU memory
 *
 * Non-default memory types are the GPU-visible ones. They come from
 * d3d12_xs.dll, which suballocates CPU-visible D3D12 heaps on the device the
 * title renders with. With VK_LAYER_REBOX_gpuva_identity and .#radv-rebox
 * those heaps sit at the same GPU and CPU address, as on the console
 * (docs/d3d12x.md). Without d3d12_xs, or with REBOX_GPU_MEMORY=0, they
 * fall back to VirtualAlloc. */

typedef HRESULT (WINAPI *gpu_alloc_fn)(SIZE_T, SIZE_T, void **, UINT64 *);
typedef void (WINAPI *gpu_free_fn)(void *);
typedef BOOL (WINAPI *gpu_query_fn)(const void *, void **, SIZE_T *);
typedef UINT64 (WINAPI *gpu_usage_fn)(void);

static gpu_alloc_fn gpu_alloc;
static gpu_free_fn gpu_free;
static gpu_query_fn gpu_query;
static gpu_usage_fn gpu_usage;

static BOOL CALLBACK load_gpu(INIT_ONCE *once, void *param, void **ctx)
{
    const char *env = getenv("REBOX_GPU_MEMORY");
    HMODULE d3d12_xs;

    if (env && *env == '0')
        return TRUE;
    if (!(d3d12_xs = LoadLibraryA("d3d12_xs.dll")))
    {
        rebox_log("xmem: d3d12_xs.dll unavailable; GPU-visible memory falls back to VirtualAlloc");
        return TRUE;
    }
    gpu_query = (gpu_query_fn)(void *)GetProcAddress(d3d12_xs, "ReboxGpuQuery");
    gpu_free = (gpu_free_fn)(void *)GetProcAddress(d3d12_xs, "ReboxGpuFree");
    gpu_usage = (gpu_usage_fn)(void *)GetProcAddress(d3d12_xs, "ReboxGpuBytesInUse");
    gpu_alloc = (gpu_alloc_fn)(void *)GetProcAddress(d3d12_xs, "ReboxGpuAlloc");
    return TRUE;
}

/* Device creation can't happen under the loader lock (DllMain-time
 * allocations): vkd3d-proton starts threads and waits for them. */
static BOOL holding_loader_lock(void)
{
    BYTE *peb = (BYTE *)NtCurrentTeb()->ProcessEnvironmentBlock;
    RTL_CRITICAL_SECTION *lock = *(RTL_CRITICAL_SECTION **)(peb + 0x110); /* PEB.LoaderLock */

    return lock && (DWORD_PTR)lock->OwningThread == GetCurrentThreadId();
}

static void *alloc_gpu(SIZE_T size, SIZE_T alignment)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static LONG warned;
    void *p = NULL;

    if (holding_loader_lock())
    {
        if (!InterlockedExchange(&warned, 1))
            rebox_log("xmem: GPU-visible allocation under the loader lock; using VirtualAlloc");
        return NULL;
    }
    InitOnceExecuteOnce(&once, load_gpu, NULL, NULL);
    if (gpu_alloc && FAILED(gpu_alloc(size, alignment, &p, NULL)))
        p = NULL;
    return p;
}

static BOOL is_gpu(const void *address)
{
    return gpu_query && gpu_query(address, NULL, NULL);
}

/* The window VK_LAYER_REBOX_gpuva_identity maps GPU memory into. Reserved
 * here, before anything else runs, so Wine never hands it out. Keep in step
 * with layers/gpuva_identity.c. */
static void reserve_gpu_window(void)
{
    const char *env = getenv("REBOX_GPUVA_WINDOW");
    ULONG_PTR base = 0x200000000000ull, size = 0x10000000000ull;

    if (env && !strcmp(env, "off"))
        return;
    if (env && *env)
    {
        char *end;
        ULONG_PTR b = strtoull(env, &end, 16), sz = size;
        if (*end == ':')
            sz = strtoull(end + 1, NULL, 16);
        if (b && sz)
            base = b, size = sz;
    }
    if (!VirtualAlloc((void *)base, size, MEM_RESERVE, PAGE_NOACCESS))
        rebox_log("xmem: could not reserve GPU VA window %#llx+%#llx (error %lu)",
                     (unsigned long long)base, (unsigned long long)size, GetLastError());
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        reserve_gpu_window();
    }
    return TRUE;
}

static void log_memtype(XALLOC_ATTRIBUTES attr)
{
    static LONG seen[16];

    if (!InterlockedExchange(&seen[attr.s.memory_type], 1))
        rebox_log("xmem: first allocation with memory_type %u (page_size %u, allocator %u)",
                     (unsigned)attr.s.memory_type, (unsigned)attr.s.page_size,
                     (unsigned)attr.s.allocator_id);
}

void *WINAPI XMemAllocDefault(SIZE_T size, ULONGLONG attributes)
{
    XALLOC_ATTRIBUTES attr = { .value = attributes };

    log_memtype(attr);
    if (attr.s.memory_type)
    {
        /* At least the page size, and VirtualAlloc's granularity for larger
         * blocks, as before. */
        SIZE_T alignment = attr_alignment(attr), floor = size >= (64 << 10) ? (64 << 10) : 4096;
        void *p;

        if (floor < attr_page_bytes(attr))
            floor = attr_page_bytes(attr);
        p = alloc_gpu(size, alignment < floor ? floor : alignment);
        if (p)
            return p;
    }
    if (wants_pages(attr))
    {
        SIZE_T alignment = attr_alignment(attr);
        return virtual_alloc_aligned(size, alignment > attr_page_bytes(attr) ? alignment : attr_page_bytes(attr));
    }
    /* Zeroed like the console's: pages are, and SlimEra and Xbox2Windows'
     * stand-ins zero heap blocks too, so titles may rely on it. */
    {
        void *p = _aligned_malloc(size ? size : 1, attr_alignment(attr));
        if (p)
            memset(p, 0, size ? size : 1);
        return p;
    }
}

void WINAPI XMemFreeDefault(void *address, ULONGLONG attributes)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!address)
        return;
    if (is_gpu(address))
    {
        gpu_free(address);
        return;
    }
    /* Decide by inspecting the pointer rather than trusting the attributes:
     * _aligned_malloc results are never the base of a VirtualAlloc region. */
    if (VirtualQuery(address, &mbi, sizeof(mbi)) && mbi.AllocationBase == address
        && mbi.Type == MEM_PRIVATE)
        VirtualFree(address, 0, MEM_RELEASE);
    else
        _aligned_free(address);
}

void *WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes)
{
    if (alloc_hook)
        return alloc_hook(size, attributes);
    return XMemAllocDefault(size, attributes);
}

void WINAPI XMemFree(void *address, ULONGLONG attributes)
{
    if (free_hook)
        free_hook(address, attributes);
    else
        XMemFreeDefault(address, attributes);
}

void WINAPI XMemSetAllocationHooks(XMEMALLOC_ROUTINE alloc, XMEMFREE_ROUTINE free)
{
    rebox_log("xmem: allocation hooks %s", alloc ? "installed" : "cleared");
    alloc_hook = alloc;
    free_hook = free;
}

/* Layout from XboxBvhBuilder.dll (0x180019c58): the caller zeroes 0x40 bytes,
 * checks State and Protect at their MEMORY_BASIC_INFORMATION offsets, then
 * requires (gpu_access & 3) before trusting a D3D12_GPU_VIRTUAL_ADDRESS as a
 * CPU pointer. Reading bit 0/1 as GPU read/write is a guess. */
typedef struct
{
    MEMORY_BASIC_INFORMATION mbi;
    ULONG gpu_access;
    ULONG reserved[3];
} XMEM_BASIC_INFORMATION;

/* Called as (GetCurrentProcess(), address, info) with no length argument, so
 * this is not VirtualQuery's signature. Every committed readable page is
 * reported GPU-accessible: on the console GPU and CPU addresses are the same,
 * and the shim's job is to make that true here (see docs/d3d12x.md). */
BOOL WINAPI XMemVirtualQuery(HANDLE process, const void *address, XMEM_BASIC_INFORMATION *info)
{
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                         | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    void *base;
    SIZE_T size;

    if (!info)
        return FALSE;
    memset(info, 0, sizeof(*info));
    /* Wine doesn't know GPU memory is mapped (it sees the reserved window, or
     * nothing), so answer for it from d3d12_xs's table. */
    if (gpu_query && gpu_query(address, &base, &size))
    {
        ULONG_PTR page = (ULONG_PTR)address & ~(ULONG_PTR)0xfff;
        info->mbi.BaseAddress = (void *)page;
        info->mbi.AllocationBase = base;
        info->mbi.AllocationProtect = PAGE_READWRITE;
        info->mbi.RegionSize = (ULONG_PTR)base + size - page;
        info->mbi.State = MEM_COMMIT;
        info->mbi.Protect = PAGE_READWRITE;
        info->mbi.Type = MEM_PRIVATE;
        info->gpu_access = 3;
        return TRUE;
    }
    if (!VirtualQueryEx(process, address, &info->mbi, sizeof(info->mbi)))
        return FALSE;
    if (info->mbi.State == MEM_COMMIT && !(info->mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    {
        if (info->mbi.Protect & readable)
            info->gpu_access |= 1;
        if (info->mbi.Protect & writable)
            info->gpu_access |= 2;
    }
    return TRUE;
}

/* Not checked against a disassembly here. Per Xbox2Windows's call-site notes
 * (docs/experiments.md, "Six API call sites"), the AMD upscaler calls this
 * seven times with selector 0x20000001 and a local output struct. A zero
 * return counts as zero; otherwise it reads the 64-bit value at +8 and diffs
 * before/after readings to update a counter. The caller probably measures its
 * own memory use, so report private commit, which grows with every
 * allocation. That's PagefileUsage, not PrivateUsage: Windows sets both to the
 * commit charge, but Wine leaves PrivateUsage at 0 and counts only touched
 * pages in PagefileUsage. Only +8 is written: nothing else in the struct is
 * known, even its size. A nonzero return reads as success whether the caller
 * tests it as a BOOL or as an HRESULT. Other selectors fail without writing
 * anything. */
#define XMEM_WSS_SELECTOR_OBSERVED 0x20000001

BOOL WINAPI XMemGetWorkingSetStatistics(ULONG selector, void *stats)
{
    static LONG warned;
    PROCESS_MEMORY_COUNTERS pmc = { .cb = sizeof(pmc) };

    if (selector != XMEM_WSS_SELECTOR_OBSERVED || !stats)
    {
        if (!InterlockedExchange(&warned, 1))
            rebox_log("xmem: XMemGetWorkingSetStatistics(%#lx, %p) unsupported", selector, stats);
        return FALSE;
    }
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return FALSE;
    /* GPU-visible allocations live in D3D12 heaps, which Wine doesn't count. */
    *(ULONGLONG *)((BYTE *)stats + 8) = pmc.PagefileUsage + (gpu_usage ? gpu_usage() : 0);
    return TRUE;
}
