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
#include "../../layers/gpuva_mailbox.h"
#include "crash.h"
#include "import_fixup.h"
#include "probe.h"

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

/* The console's addresses are 46-bit: a title packs them so (a GPU fault at an adopted buffer's
 * address with bit 46 cleared), while Wine hands out reservations near the top of the 47-bit user
 * space. So new reservations, the title's own VirtualAlloc included (import_fixup.c), are asked
 * below 2^46 with VirtualAlloc2's address requirement, and get Wine's choice if that fails. */
#define TITLE_ADDRESS_LIMIT 0x3fffffffffffull

#define MEM_RESERVE_PLACEHOLDER_BIT 0x40000
struct address_requirements { void *lowest, *highest; SIZE_T alignment; };
struct extended_parameter { ULONG64 type; union { ULONG64 ulong64; void *pointer; }; };

void *xmem_low_alloc2(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    static PVOID (WINAPI *va2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
    /* From 4 GiB: lower addresses are RADV's 32-bit range, or below its VA space, so memory there
     * can't be made GPU memory at its own address (gpu_memory_adopt). */
    struct address_requirements req = { (void *)0x100000000ull, (void *)TITLE_ADDRESS_LIMIT, 0 };
    struct extended_parameter param = { 1 /* MemExtendedParameterAddressRequirements */, { .pointer = &req } };
    static LONG failed;
    void *p;

    if (!va2 && !(va2 = (void *)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2")))
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (address || !(type & (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER_BIT)))
        return va2(NULL, address, size, type, protect, NULL, 0);
    if ((p = va2(NULL, NULL, size, type, protect, &param, 1)))
        return p;
    if (!InterlockedExchange(&failed, 1))
        rebox_log("xmem: no reservation of %#zx bytes below %#llx (error %lu): Wine places it", size,
                  TITLE_ADDRESS_LIMIT, GetLastError());
    return va2(NULL, NULL, size, type, protect, NULL, 0);
}

/* VirtualAlloc for the title (import_fixup.c) and xmem: reservations below 2^46. Anything else
 * goes to ntdll directly, never through kernel32's VirtualAlloc, which may be this (xmem loads from
 * outside the Windows directory in the smoke test, so import_fixup.c replaces its imports, and
 * GetProcAddress, too). */
void *WINAPI xmem_low_virtual_alloc(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    static NTSTATUS (WINAPI *nt_alloc)(HANDLE, void **, ULONG_PTR, SIZE_T *, ULONG, ULONG);
    NTSTATUS status;

    if (!address && (type & MEM_RESERVE))
    {
        void *p = xmem_low_alloc2(NULL, size, type, protect);
        static LONG logged;
        if (size >= (16u << 20) && InterlockedIncrement(&logged) <= 64)
            rebox_log("xmem: VirtualAlloc reserve of %#zx bytes, type %#lx, protect %#lx -> %p", size, type, protect, p);
        return p;
    }
    if (!nt_alloc)
        nt_alloc = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtAllocateVirtualMemory");
    if ((status = nt_alloc(GetCurrentProcess(), &address, 0, &size, type, protect)))
    {
        static ULONG (WINAPI *to_error)(NTSTATUS);
        if (!to_error)
            to_error = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlNtStatusToDosError");
        SetLastError(to_error ? to_error(status) : ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return address;
}

/* VirtualAlloc only guarantees 64 KiB: reserve extra, then re-reserve the
 * aligned part (another thread can take it in between, so retry). */
static void *virtual_alloc_aligned(SIZE_T size, SIZE_T alignment)
{
    if (alignment <= (64 << 10))
        return xmem_low_virtual_alloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (int i = 0; i < 8; i++)
    {
        BYTE *probe = xmem_low_virtual_alloc(NULL, size + alignment, MEM_RESERVE, PAGE_NOACCESS), *p;
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

static struct gpuva_mailbox *mailbox;

/* A write into title memory the layer is making GPU memory (layers/gpuva_mailbox.h) waits until the
 * new pages are mapped there, then is retried in them. */
static LONG CALLBACK hold_busy_writes(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *r = ep->ExceptionRecord;
    UINT64 address, start;

    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1
            || !mailbox || !mailbox->busy_size)
        return EXCEPTION_CONTINUE_SEARCH;
    address = r->ExceptionInformation[1];
    start = mailbox->busy_address;
    if (address < start || address - start >= mailbox->busy_size)
        return EXCEPTION_CONTINUE_SEARCH;
    while (mailbox->busy_size && mailbox->busy_address == start)
        Sleep(0);
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* The mailbox for making title memory GPU-visible (layers/gpuva_mailbox.h), or NULL. */
void *WINAPI ReboxGpuvaMailbox(void)
{
    return mailbox;
}

/* The window VK_LAYER_REBOX_gpuva_identity maps GPU memory into. Reserved
 * here, before anything else runs, so Wine never hands it out; its last 64 KiB
 * is committed as the layer's mailbox. Keep in step with layers/gpuva_identity.c. */
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
    else if ((mailbox = VirtualAlloc((void *)(base + size - GPUVA_MAILBOX_SIZE), GPUVA_MAILBOX_SIZE, MEM_COMMIT,
                                     PAGE_READWRITE)))
    {
        mailbox->magic = GPUVA_MAILBOX_MAGIC;
        AddVectoredExceptionHandler(1, hold_busy_writes);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        reserve_gpu_window();
        crash_report_install(0x200000000000ull, 0x10000000000ull);
        /* The title's imports are bound before its DLLs initialise. */
        char *fixup = rebox_getenv("REBOX_IMPORT_FIXUP");
        if (!fixup || strcmp(fixup, "0"))
            import_fixup_run();
        free(fixup);
        probe_install();
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

/* A title that exports __XMemAllocationHooks__ ({alloc, free, ...}) has them used for every XMemAlloc
 * and XMemFree, as the console's xmem.dll does without an XMemSetAllocationHooks call: the
 * runtime's own allocations then reach them too (the target title places descriptor heaps through
 * its hook for the D3D allocator, 128). A hook calling back into XMemAlloc gets the default. */
static INIT_ONCE title_hooks_once = INIT_ONCE_STATIC_INIT;
static DWORD in_hook = TLS_OUT_OF_INDEXES; /* a TLS slot: set while this thread is in a hook */

static BOOL CALLBACK find_title_hooks(INIT_ONCE *once, void *param, void **context)
{
    void *const *hooks = (void *const *)GetProcAddress(GetModuleHandleA(NULL), "__XMemAllocationHooks__");

    in_hook = TlsAlloc();

    if (hooks && hooks[0] && hooks[1] && !alloc_hook)
    {
        alloc_hook = (XMEMALLOC_ROUTINE)hooks[0];
        free_hook = (XMEMFREE_ROUTINE)hooks[1];
        rebox_log("xmem: the title's exported allocation hooks are used (alloc %p, free %p)", hooks[0], hooks[1]);
    }
    return TRUE;
}

void *WINAPI XMemAlloc(SIZE_T size, ULONGLONG attributes)
{
    void *p;

    InitOnceExecuteOnce(&title_hooks_once, find_title_hooks, NULL, NULL);
    if (!alloc_hook || in_hook == TLS_OUT_OF_INDEXES || TlsGetValue(in_hook))
        return XMemAllocDefault(size, attributes);
    TlsSetValue(in_hook, (void *)1);
    p = alloc_hook(size, attributes);
    TlsSetValue(in_hook, NULL);
    return p;
}

void WINAPI XMemFree(void *address, ULONGLONG attributes)
{
    InitOnceExecuteOnce(&title_hooks_once, find_title_hooks, NULL, NULL);
    if (!free_hook || in_hook == TLS_OUT_OF_INDEXES || TlsGetValue(in_hook))
    {
        XMemFreeDefault(address, attributes);
        return;
    }
    TlsSetValue(in_hook, (void *)1);
    free_hook(address, attributes);
    TlsSetValue(in_hook, NULL);
}

void WINAPI XMemSetAllocationHooks(XMEMALLOC_ROUTINE alloc, XMEMFREE_ROUTINE free)
{
    InitOnceExecuteOnce(&title_hooks_once, find_title_hooks, NULL, NULL);
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
    {
        SetLastError(ERROR_NOACCESS); /* as VirtualQueryEx (Xbox2Windows) */
        return FALSE;
    }
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
 * (its docs/experiments.md, "Six API call sites"), the AMD upscaler calls this
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

/* Selector 0x10000001 fills the whole working-set struct (XValkyrie's layout, facts only):
 * game/tools limits and use, stack and kernel pool, GPU limits and use. Unreal reads its
 * memory budget from it and, with nothing filled in, stops for want of memory (a UE5 title: "Ran out
 * of memory allocating 65536 bytes" with every MemoryStats field zero). The limit is what a Series
 * X title gets, 13 GiB, or the host's memory if less. */
#define XMEM_WSS_SELECTOR_FULL 0x10000001

struct xmem_working_set
{
    SIZE_T game_limit, game_used, tools_limit, tools_used;
    UINT32 stack_used, kernel_pool_used;
    SIZE_T gpu_limit, gpu_used, tools_gpu_limit, tools_gpu_used;
};

BOOL WINAPI XMemGetWorkingSetStatistics(ULONG selector, void *stats)
{
    static LONG warned;
    PROCESS_MEMORY_COUNTERS_EX pmc = { .cb = sizeof(pmc) };

    if (selector == XMEM_WSS_SELECTOR_FULL && stats)
    {
        MEMORYSTATUSEX host = { .dwLength = sizeof(host) };
        struct xmem_working_set ws = { 0 };
        SIZE_T gpu = gpu_usage ? gpu_usage() : 0;

        if (!GlobalMemoryStatusEx(&host)
                || !K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
            return FALSE;
        ws.game_limit = ws.gpu_limit = (SIZE_T)min(13ull << 30, host.ullTotalPhys);
        ws.game_used = min(pmc.PrivateUsage + gpu, ws.game_limit);
        ws.gpu_used = min(gpu, ws.gpu_limit);
        memcpy(stats, &ws, sizeof(ws));
        return TRUE;
    }
    if (selector != XMEM_WSS_SELECTOR_OBSERVED || !stats)
    {
        if (!InterlockedExchange(&warned, 1))
            rebox_log("xmem: XMemGetWorkingSetStatistics(%#lx, %p) unsupported", selector, stats);
        return FALSE;
    }
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
        return FALSE;
    /* GPU-visible allocations live in D3D12 heaps, which Wine doesn't count. */
    *(ULONGLONG *)((BYTE *)stats + 8) = pmc.PagefileUsage + (gpu_usage ? gpu_usage() : 0);
    return TRUE;
}

/* ------------------------------------------------------- virtual memory and physical pages */

#ifndef MEM_4MB_PAGES
#define MEM_4MB_PAGES 0x80000000
#endif
#define CONSOLE_PAGE_FLAGS (MEM_LARGE_PAGES | MEM_4MB_PAGES)

/* VirtualAlloc with the XMem flags inserted before the protection (ScarlettX checked the order
 * against the console's xmem.dll and a title's calls; XValkyrie agrees). The flags only matter
 * to the console's allocator. The page-size flags go too: they need a privilege on Windows, and
 * our Wine patch drops them anyway. */
/* Console-only bits Windows rejects (a UE5 title: type 0x20401000 / protect 0x2404 to commit,
 * type 0x20402000 / protect 0x10404 to reserve 112 MiB for physical pages):
 *   type     MEM_PHYSICAL (0x400000): a range the title maps physical pages into. Here that's a
 *            placeholder, which holds views of the pages' sections (Physical pages, below);
 *   protect  everything above the base protection and PAGE_GUARD: 0x800-0x4000 and 0x10000 (GPU
 *            access in the console's winnt.h), PAGE_WRITECOMBINE (0x400) and PAGE_NOCACHE (0x200).
 * The pages become plain CPU memory: GPU access at a title-chosen address is unified memory's
 * business (docs/d3d12x.md). */
#define HOST_PROTECT_BITS 0x1ff

/* Placeholders (the target title reserves 185 MiB with type 0x42000, MEM_RESERVE |
 * MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, then splits it with kernel32's VirtualFree(MEM_RELEASE |
 * MEM_PRESERVE_PLACEHOLDER) itself): VirtualAlloc refuses the flags, so they go to VirtualAlloc2,
 * whose placeholders Wine has. A placeholder takes no plain commit: XMemMapPhysicalPages replaces
 * one with committed memory. */
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x40000
#define MEM_REPLACE_PLACEHOLDER 0x4000
#define MEM_COALESCE_PLACEHOLDERS 0x1
#define MEM_PRESERVE_PLACEHOLDER 0x2
#endif

static void *virtual_alloc2(void *address, SIZE_T size, DWORD type, DWORD protect)
{
    static PVOID (WINAPI *va2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
    if (!va2)
        va2 = (void *)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    if (!va2)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }
    if (!address)
        return xmem_low_alloc2(NULL, size, type, protect);
    return va2(NULL, address, size, type, protect, NULL, 0);
}

/* GPU-only views. A title reserves a range twice in a row, the same size, first CPU-readable and
 * then with no CPU access and GPU access bits (protect 0x801); it maps its physical pages into the
 * first only and places GPU resources at the same offsets in the second: on the console the second
 * is a GPU view of the first (found by xcl, ivall/xcl #283, for a retail title's video frames).
 * Here the second is separate memory, so the pair is recorded and ReboxGpuViewToCpu gives the CPU
 * address behind a GPU-view address, for d3d12_xs to place resources over. */
static struct gpu_view { ULONG_PTR gpu, cpu, size; } gpu_views[16];
static ULONG_PTR last_cpu_reservation, last_cpu_reservation_size;
static SRWLOCK gpu_view_lock = SRWLOCK_INIT;

static void note_reservation(void *p, SIZE_T size, DWORD type, DWORD protect)
{
    BOOL gpu_only = (protect & HOST_PROTECT_BITS) == PAGE_NOACCESS && (protect & ~HOST_PROTECT_BITS & ~0x600);

    if (!p || !(type & MEM_RESERVE))
        return;
    AcquireSRWLockExclusive(&gpu_view_lock);
    if (gpu_only && last_cpu_reservation && last_cpu_reservation_size == size)
    {
        for (UINT i = 0; i < ARRAYSIZE(gpu_views); i++)
            if (!gpu_views[i].size)
            {
                gpu_views[i].gpu = (ULONG_PTR)p;
                gpu_views[i].cpu = last_cpu_reservation;
                gpu_views[i].size = size;
                rebox_log("xmem: %#zx bytes at %p are a GPU view of the reservation at %#llx", size, p,
                          (unsigned long long)last_cpu_reservation);
                break;
            }
        last_cpu_reservation = 0;
    }
    else if (!gpu_only && (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ
                                      | PAGE_EXECUTE_READWRITE)))
    {
        last_cpu_reservation = (ULONG_PTR)p;
        last_cpu_reservation_size = size;
    }
    ReleaseSRWLockExclusive(&gpu_view_lock);
}

static void forget_reservation(void *p)
{
    AcquireSRWLockExclusive(&gpu_view_lock);
    for (UINT i = 0; i < ARRAYSIZE(gpu_views); i++)
        if (gpu_views[i].size && (gpu_views[i].gpu == (ULONG_PTR)p || gpu_views[i].cpu == (ULONG_PTR)p))
            gpu_views[i].size = 0;
    if (last_cpu_reservation == (ULONG_PTR)p)
        last_cpu_reservation = 0;
    ReleaseSRWLockExclusive(&gpu_view_lock);
}

/* The CPU address behind a GPU-view address, or the address itself. */
ULONGLONG WINAPI ReboxGpuViewToCpu(ULONGLONG address)
{
    ULONGLONG result = address;
    AcquireSRWLockShared(&gpu_view_lock);
    for (UINT i = 0; i < ARRAYSIZE(gpu_views); i++)
        if (gpu_views[i].size && address - gpu_views[i].gpu < gpu_views[i].size)
            result = address - gpu_views[i].gpu + gpu_views[i].cpu;
    ReleaseSRWLockShared(&gpu_view_lock);
    return result;
}

static void note_physical_range(void *base, SIZE_T size);
static BOOL release_physical_range(void *base);
static BOOL in_physical_range(const void *address);

void *WINAPI XMemVirtualAlloc(void *address, SIZE_T size, DWORD type, ULONGLONG xmem_flags, DWORD protect)
{
    DWORD host_type = type & ~(CONSOLE_PAGE_FLAGS | MEM_PHYSICAL | MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER),
          host_protect = protect & HOST_PROTECT_BITS;
    void *p;
    static LONG calls, failures, graphics, placeholders;

    if ((protect & ~HOST_PROTECT_BITS & ~0x600) && !InterlockedExchange(&graphics, 1))
        rebox_log("xmem: XMemVirtualAlloc with GPU access bits (protect %#lx): plain CPU memory here", protect);
    if ((type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)) && !InterlockedExchange(&placeholders, 1))
        rebox_log("xmem: XMemVirtualAlloc with placeholders (type %#lx): real ones, through VirtualAlloc2", type);
    /* A range for physical pages is a placeholder, to hold views of their sections (see Physical
     * pages below); one made from a placeholder of the title's stays one. */
    if ((type & MEM_PHYSICAL) && (type & MEM_RESERVE) && !(type & MEM_COMMIT) && (type & MEM_REPLACE_PLACEHOLDER))
        p = address;
    else if ((type & MEM_PHYSICAL) && (type & MEM_RESERVE) && !(type & MEM_COMMIT)
             && (p = address ? virtual_alloc2(address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS)
                             : xmem_low_alloc2(NULL, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS)))
        note_physical_range(p, size);
    else if (type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER))
        p = virtual_alloc2(address, size, host_type | (type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)),
                           host_protect);
    else
        p = xmem_low_virtual_alloc(address, size, host_type, host_protect);
    /* Memory committed inside a range reserved for physical pages (a UE5 title commits 64 KiB of
     * one): its 64 KiB pieces of placeholder become committed memory. */
    if (!p && address && (type & MEM_COMMIT) && in_physical_range(address))
    {
        ULONG_PTR at = (ULONG_PTR)address & ~(ULONG_PTR)0xffff,
                  end = ((ULONG_PTR)address + size + 0xffff) & ~(ULONG_PTR)0xffff;
        p = address;
        for (; p && at < end; at += 0x10000)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void *)at, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
                continue;
            VirtualFree((void *)at, 0x10000, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            if (!virtual_alloc2((void *)at, 0x10000, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, host_protect))
                p = NULL;
        }
    }
    note_reservation(p, size, type, protect);

    if (!p ? InterlockedIncrement(&failures) <= 16
           : InterlockedIncrement(&calls) <= 4 || ((type & MEM_RESERVE) && size >= (16u << 20) && calls <= 256))
        rebox_log("xmem: XMemVirtualAlloc(%p, %#zx, type %#lx, flags %#llx, protect %#lx) -> %p (error %lu)", address,
                     size, type, (unsigned long long)xmem_flags, protect, p, p ? 0 : GetLastError());
    return p;
}

BOOL WINAPI XMemVirtualFree(void *address, SIZE_T size, DWORD type)
{
    if (type & MEM_RELEASE)
        forget_reservation(address);
    if ((type & MEM_RELEASE) && release_physical_range(address))
        return TRUE;
    return VirtualFree(address, size, type);
}

/* Physical pages. A title allocates page numbers, then maps them into ranges it reserved, and
 * unmaps them (a null page list) to reuse a range. It maps the same pages at several addresses at
 * once and reads at one what it wrote at another: a page pool holds every page at its home in one
 * reservation (RegisterPagePoolX) while streaming maps them again where the data is used, and
 * DirectStorage writes into the pages (ReboxWritePhysicalPages). So the pages are shared memory
 * here too: each XMemAllocatePhysicalPages call makes a committed section (Wine backs it with a
 * sparse file: untouched pages cost nothing), and a page is mapped as a view of its slice of that
 * section. A view needs a placeholder at its address, so reservations made for physical pages
 * (MEM_PHYSICAL, XMemVirtualAlloc) are placeholders, split to a page's size at each mapping, and
 * an unmapped page leaves its placeholder behind. VIctory does the same (facts only, GPL-3.0).
 * A page mapped where no placeholder is (ordinary memory) gets a copy instead, written back at
 * unmapping, which other mappings of the page don't see meanwhile. xmem keeps a view of each
 * whole section ("home") to read and write pages through wherever they are mapped.
 *
 * Page numbers are 4 KiB frame numbers, as the console's are (xcl, ivall/xcl; XValkyrie #10): a
 * 64 KiB page is (index + 1) * 16, and a title packs them as number >> 4 and back as
 * (packed << 4) & 0x1fffff0, which round-trips only so. A 4 MiB page (MEM_4MB_PAGES) takes 64 units
 * of 64 KiB. Signatures as XValkyrie and TomBox read them: no process argument, BOOL results. */
#define PAGE_UNIT (64u << 10)

struct page_section
{
    HANDLE handle;
    BYTE *home;     /* a view of the whole section */
    LONG live;      /* pages not freed yet */
};

struct physical_unit
{
    struct page_section *section; /* NULL once freed */
    UINT64 offset;    /* into the section */
    ULONG_PTR mapped; /* the address it was last mapped at, or 0 */
    BOOL copy;        /* mapped there as a copy (no placeholder), not a view */
    ULONG_PTR copy_at; /* where it is mapped as a copy, or 0: writes go there too */
    UINT size_units;  /* at a page's first unit: its size in 64 KiB units (1 or 64) */
};

static SRWLOCK physical_lock = SRWLOCK_INIT;
static struct physical_unit *units;
static ULONG_PTR unit_count, unit_capacity;

/* Address -> the page mapped there: open addressing on 64 KiB-aligned addresses. A page mapped as
 * part of a run shares that run's view (view, view_bytes); a copy is its own. */
#define MAPPED_SLOTS (1u << 20)
static struct { ULONG_PTR address, unit, view; SIZE_T view_bytes; BOOL copy; } *mapped_at;

static SIZE_T mapped_slot(ULONG_PTR address, BOOL insert)
{
    SIZE_T i = (SIZE_T)((address >> 16) * 0x9e3779b97f4a7c15ull >> 44) & (MAPPED_SLOTS - 1);
    for (SIZE_T n = 0; n < MAPPED_SLOTS; n++, i = (i + 1) & (MAPPED_SLOTS - 1))
        if (mapped_at[i].address == address || (insert ? mapped_at[i].address <= 1 : !mapped_at[i].address))
            return i;
    return (SIZE_T)-1;
}

/* The unit of page number p, or -1. */
static ULONG_PTR page_unit(ULONG_PTR page)
{
    ULONG_PTR u;
    if (!page || page & 15)
        return (ULONG_PTR)-1;
    u = (page >> 4) - 1;
    return u < unit_count && units[u].size_units && units[u].section ? u : (ULONG_PTR)-1;
}

static BYTE *unit_home(ULONG_PTR u)
{
    return units[u].section->home + units[u].offset;
}

typedef void *(WINAPI *map_view3_fn)(HANDLE, HANDLE, void *, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *unmap_view2_fn)(HANDLE, void *, ULONG);
static map_view3_fn map_view3;
static unmap_view2_fn unmap_view2;

static BOOL CALLBACK find_views(INIT_ONCE *once, void *param, void **context)
{
    HMODULE kb = GetModuleHandleA("kernelbase.dll");
    map_view3 = kb ? (map_view3_fn)(void *)GetProcAddress(kb, "MapViewOfFile3") : NULL;
    unmap_view2 = kb ? (unmap_view2_fn)(void *)GetProcAddress(kb, "UnmapViewOfFile2") : NULL;
    return TRUE;
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONGLONG flags, ULONG_PTR *count, ULONG_PTR *pages)
{
    UINT size_units = (flags & MEM_4MB_PAGES) ? 64 : 1;
    struct page_section *s = NULL;
    UINT64 bytes;
    BOOL ok = TRUE;

    if (!count || !pages)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    {
        static LONG calls;
        if (InterlockedIncrement(&calls) <= 4)
            rebox_log("xmem: XMemAllocatePhysicalPages(flags %#llx, %lu pages)", (unsigned long long)flags,
                         (unsigned long)*count);
    }
    if (!*count)
        return TRUE;
    bytes = (UINT64)*count * size_units * PAGE_UNIT;
    if (!(s = calloc(1, sizeof(*s)))
            || !(s->handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_COMMIT,
                                                (DWORD)(bytes >> 32), (DWORD)bytes, NULL))
            || !(s->home = MapViewOfFile(s->handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes)))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xmem: a section for %lu physical pages (%llu MiB) failed (error %lu)", (unsigned long)*count,
                      (unsigned long long)(bytes >> 20), GetLastError());
        if (s && s->handle)
            CloseHandle(s->handle);
        free(s);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    s->live = (LONG)*count;
    AcquireSRWLockExclusive(&physical_lock);
    if (!mapped_at && !(mapped_at = VirtualAlloc(NULL, MAPPED_SLOTS * sizeof(*mapped_at), MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE)))
        ok = FALSE;
    if (ok && unit_count + *count * size_units > unit_capacity)
    {
        ULONG_PTR capacity = max(unit_capacity * 2, unit_count + *count * size_units + 65536);
        struct physical_unit *grown = realloc(units, capacity * sizeof(*units));
        if (grown)
        {
            memset(grown + unit_capacity, 0, (capacity - unit_capacity) * sizeof(*units));
            units = grown;
            unit_capacity = capacity;
        }
        else
            ok = FALSE;
    }
    if (ok)
        for (ULONG_PTR i = 0; i < *count; i++)
        {
            units[unit_count].size_units = size_units;
            units[unit_count].section = s;
            units[unit_count].offset = (UINT64)i * size_units * PAGE_UNIT;
            pages[i] = (unit_count + 1) << 4;
            unit_count += size_units;
        }
    ReleaseSRWLockExclusive(&physical_lock);
    if (!ok)
    {
        UnmapViewOfFile(s->home);
        CloseHandle(s->handle);
        free(s);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return ok;
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR *pages)
{
    AcquireSRWLockExclusive(&physical_lock);
    for (ULONG_PTR i = 0; pages && i < count; i++)
    {
        ULONG_PTR u = page_unit(pages[i]);
        struct page_section *s;
        if (u == (ULONG_PTR)-1)
            continue;
        s = units[u].section;
        units[u].section = NULL;
        /* Views still mapped keep the section's memory; the home view and handle go with the
         * last page. */
        if (!--s->live)
        {
            UnmapViewOfFile(s->home);
            CloseHandle(s->handle);
            free(s);
        }
    }
    ReleaseSRWLockExclusive(&physical_lock);
    return TRUE;
}

/* Ranges d3d12_xs made GPU memory in place (ReboxPhysicalAdopted): their pages were replaced by a
 * GPU heap holding a copy, so pages mapped there are copies from then on, and the memory is never
 * decommitted or split here. */
static struct { ULONG_PTR start, end; } adopted[32];
static UINT adopted_count;

static BOOL in_adopted(ULONG_PTR address)
{
    for (UINT i = 0; i < adopted_count; i++)
        if (address >= adopted[i].start && address < adopted[i].end)
            return TRUE;
    return FALSE;
}

/* The slot of the page mapped at address, or -1. Under physical_lock. */
static SIZE_T slot_at(ULONG_PTR address)
{
    SIZE_T slot = mapped_at ? mapped_slot(address, FALSE) : (SIZE_T)-1;
    return slot != (SIZE_T)-1 && mapped_at[slot].address == address ? slot : (SIZE_T)-1;
}

static void set_slot(ULONG_PTR address, ULONG_PTR u, ULONG_PTR view, SIZE_T view_bytes, BOOL copy)
{
    SIZE_T slot = mapped_slot(address, TRUE);
    units[u].mapped = address;
    units[u].copy = copy;
    if (copy)
        units[u].copy_at = address;
    if (slot == (SIZE_T)-1)
        return;
    mapped_at[slot].address = address;
    mapped_at[slot].unit = u;
    mapped_at[slot].view = view;
    mapped_at[slot].view_bytes = view_bytes;
    mapped_at[slot].copy = copy;
}

static void clear_slot(SIZE_T slot)
{
    ULONG_PTR u = mapped_at[slot].unit;
    if (units[u].mapped == mapped_at[slot].address)
        units[u].mapped = 0;
    if (units[u].copy_at == mapped_at[slot].address)
        units[u].copy_at = 0;
    mapped_at[slot].address = 1; /* a tombstone */
}

/* A view of size bytes of section s at offset, at address: in the placeholder there, split off a
 * larger one, or made of adjacent ones coalesced. Each step is a wineserver call under Wine, so
 * the plain map is tried first. */
static BOOL place_view(ULONG_PTR address, SIZE_T size, struct page_section *s, UINT64 offset)
{
    void *p = (void *)address;
    if (!map_view3 || in_adopted(address))
        return FALSE;
    if (map_view3(s->handle, GetCurrentProcess(), p, offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0))
        return TRUE;
    if (VirtualFree(p, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)
            && map_view3(s->handle, GetCurrentProcess(), p, offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0))
        return TRUE;
    return VirtualFree(p, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)
            && map_view3(s->handle, GetCurrentProcess(), p, offset, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
}

/* Page unit u at address as ordinary memory with a copy of its contents: where there's no
 * placeholder. */
static BOOL place_copy(ULONG_PTR address, ULONG_PTR u)
{
    SIZE_T size = (SIZE_T)units[u].size_units * PAGE_UNIT;
    static LONG logged, copies;

    if (!in_adopted(address) && !VirtualAlloc((void *)address, size, MEM_COMMIT, PAGE_READWRITE)
            && !VirtualAlloc((void *)address, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
    {
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xmem: mapping a physical page of %lu KiB at %#llx failed (error %lu)",
                      (unsigned long)(size >> 10), (unsigned long long)address, GetLastError());
        return FALSE;
    }
    memcpy((void *)address, unit_home(u), size);
    if (InterlockedIncrement(&copies) <= 8)
        rebox_log("xmem: physical page %#llx at %#llx is a copy: no placeholder there",
                  (unsigned long long)((u + 1) << 4), (unsigned long long)address);
    set_slot(address, u, address, size, TRUE);
    return TRUE;
}

/* Maps n page units at consecutive addresses from at, where nothing is mapped: runs of pages that
 * follow each other in one section as one view each; a run that can't be one view page by page;
 * a page with no placeholder as a copy. Under physical_lock. */
static BOOL map_units(ULONG_PTR at, const ULONG_PTR *us, ULONG_PTR n)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL ok = TRUE;

    InitOnceExecuteOnce(&once, find_views, NULL, NULL);
    for (ULONG_PTR i = 0, j; i < n; i = j)
    {
        struct page_section *s = units[us[i]].section;
        SIZE_T bytes = (SIZE_T)units[us[i]].size_units * PAGE_UNIT;

        for (j = i + 1; j < n && units[us[j]].section == s
                && units[us[j]].offset == units[us[i]].offset + bytes; j++)
            bytes += (SIZE_T)units[us[j]].size_units * PAGE_UNIT;
        if (place_view(at, bytes, s, units[us[i]].offset))
        {
            for (ULONG_PTR k = i, a = at; k < j; a += (SIZE_T)units[us[k]].size_units * PAGE_UNIT, k++)
                set_slot(a, us[k], at, bytes, FALSE);
            at += bytes;
            continue;
        }
        for (ULONG_PTR k = i; k < j; k++)
        {
            SIZE_T size = (SIZE_T)units[us[k]].size_units * PAGE_UNIT;
            if (j - i > 1 && place_view(at, size, s, units[us[k]].offset))
                set_slot(at, us[k], at, size, FALSE);
            else
                ok &= place_copy(at, us[k]);
            at += size;
        }
    }
    return ok;
}

/* Unmaps every page mapped in [start, end): a copy is written back to its page and decommitted; a
 * view leaves its placeholder, and pages of it outside the range are mapped again. Under
 * physical_lock. */
static void unmap_range(ULONG_PTR start, ULONG_PTR end)
{
    for (ULONG_PTR at = start; at < end; at += PAGE_UNIT)
    {
        SIZE_T slot = slot_at(at);
        ULONG_PTR u, view, keep_at[2] = { 0, 0 }, *keep = NULL;
        ULONG_PTR keep_count[2] = { 0, 0 };
        SIZE_T view_bytes;

        if (slot == (SIZE_T)-1)
            continue;
        u = mapped_at[slot].unit;
        if (mapped_at[slot].copy)
        {
            SIZE_T size = (SIZE_T)units[u].size_units * PAGE_UNIT;
            if (units[u].section && !IsBadReadPtr((void *)at, size))
                memcpy(unit_home(u), (void *)at, size);
            if (!in_adopted(at))
                VirtualFree((void *)at, size, MEM_DECOMMIT);
            clear_slot(slot);
            continue;
        }
        view = mapped_at[slot].view;
        view_bytes = mapped_at[slot].view_bytes;
        /* The pages of the view before and after the range stay mapped: remembered, then mapped
         * again once the view is gone. */
        if (view < start || view + view_bytes > end)
            keep = malloc((view_bytes / PAGE_UNIT) * sizeof(*keep));
        for (ULONG_PTR a = view; a < view + view_bytes; a += PAGE_UNIT)
        {
            SIZE_T s = slot_at(a);
            if (s == (SIZE_T)-1)
                continue;
            if (keep && (a < start || a >= end))
            {
                UINT side = a >= end;
                if (!keep_count[side])
                    keep_at[side] = a;
                keep[(side ? (start - view) / PAGE_UNIT : 0) + keep_count[side]++] = mapped_at[s].unit;
            }
            clear_slot(s);
        }
        if (!unmap_view2 || !unmap_view2(GetCurrentProcess(), (void *)view, MEM_PRESERVE_PLACEHOLDER))
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 8)
                rebox_log("xmem: unmapping the physical page view at %#llx failed (error %lu)",
                          (unsigned long long)view, GetLastError());
        }
        if (keep)
        {
            if (keep_count[0])
                map_units(keep_at[0], keep, keep_count[0]);
            if (keep_count[1])
                map_units(keep_at[1], keep + (start - view) / PAGE_UNIT, keep_count[1]);
            free(keep);
        }
    }
}

/* The address just past what count entries of a page list cover from at: a page's size each, and
 * for an empty entry the size of the page mapped there (64 KiB if none). -1 for a page number this
 * process didn't allocate. Under physical_lock. */
static ULONG_PTR list_end(ULONG_PTR at, ULONG_PTR count, const ULONG_PTR *pages)
{
    for (ULONG_PTR i = 0; i < count; i++)
    {
        ULONG_PTR u = pages && pages[i] ? page_unit(pages[i]) : (ULONG_PTR)-1;
        SIZE_T slot;
        if (pages && pages[i] && u == (ULONG_PTR)-1)
            return (ULONG_PTR)-1;
        if (u != (ULONG_PTR)-1)
            at += (SIZE_T)units[u].size_units * PAGE_UNIT;
        else
            at += (slot = slot_at(at)) != (SIZE_T)-1 ? (SIZE_T)units[mapped_at[slot].unit].size_units * PAGE_UNIT
                                                     : PAGE_UNIT;
    }
    return at;
}

BOOL WINAPI XMemMapPhysicalPages(void *address, ULONG_PTR count, ULONG_PTR *pages)
{
    ULONG_PTR start = (ULONG_PTR)address, end, *us;
    BOOL ok = TRUE;

    if (!address || !count)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    AcquireSRWLockExclusive(&physical_lock);
    if (!mapped_at)
    {
        ReleaseSRWLockExclusive(&physical_lock);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((end = list_end(start, count, pages)) == (ULONG_PTR)-1)
    {
        static LONG logged;
        ReleaseSRWLockExclusive(&physical_lock);
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xmem: XMemMapPhysicalPages(%p): a page number this process didn't allocate", address);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    unmap_range(start, end);
    /* Then the pages, a stretch of non-empty entries at a time. */
    if (pages && (us = malloc(count * sizeof(*us))))
    {
        ULONG_PTR at = start, n = 0, run_at = start;
        for (ULONG_PTR i = 0; i <= count; i++)
        {
            if (i < count && pages[i])
            {
                if (!n)
                    run_at = at;
                us[n++] = page_unit(pages[i]);
                at += (SIZE_T)units[us[n - 1]].size_units * PAGE_UNIT;
                continue;
            }
            if (n)
                ok &= map_units(run_at, us, n);
            n = 0;
            if (i < count)
            {
                if (!in_adopted(at)) VirtualFree((void *)at, PAGE_UNIT, MEM_DECOMMIT);
                at += PAGE_UNIT;
            }
        }
        free(us);
    }
    else if (pages)
        ok = FALSE;
    else
        for (ULONG_PTR at = start; at < end; at += PAGE_UNIT)
            if (!in_adopted(at)) VirtualFree((void *)at, PAGE_UNIT, MEM_DECOMMIT); /* ordinary memory there: decommitted, as on unmapping */
    ReleaseSRWLockExclusive(&physical_lock);
    return ok;
}


/* d3d12_xs made [address, address + size) GPU memory in place: a GPU heap holding a copy of what
 * was there replaced the pages. Pages mapped there become copies (a write through
 * ReboxWritePhysicalPages goes to both), and pages mapped there later are copied in. */
void WINAPI ReboxPhysicalAdopted(ULONGLONG address, ULONGLONG size)
{
    UINT converted = 0;

    AcquireSRWLockExclusive(&physical_lock);
    if (adopted_count < ARRAYSIZE(adopted))
    {
        adopted[adopted_count].start = (ULONG_PTR)address;
        adopted[adopted_count].end = (ULONG_PTR)(address + size);
        adopted_count++;
    }
    for (ULONG_PTR at = (ULONG_PTR)address & ~(ULONG_PTR)(PAGE_UNIT - 1); mapped_at && at < address + size; at += PAGE_UNIT)
    {
        SIZE_T slot = slot_at(at);
        ULONG_PTR u;
        if (slot == (SIZE_T)-1 || mapped_at[slot].copy)
            continue;
        u = mapped_at[slot].unit;
        mapped_at[slot].copy = TRUE;
        mapped_at[slot].view = at;
        mapped_at[slot].view_bytes = (SIZE_T)units[u].size_units * PAGE_UNIT;
        units[u].copy_at = at;
        if (units[u].mapped == at)
            units[u].copy = TRUE;
        converted++;
    }
    ReleaseSRWLockExclusive(&physical_lock);
    rebox_log("xmem: %#llx+%#llx is GPU memory in place now: %u physical pages mapped there are copies",
              (unsigned long long)address, (unsigned long long)size, converted);
}
/* XMemMapPhysicalPages for scattered addresses, as Windows' MapUserPhysicalPagesScatter: page i
 * goes to addresses[i] (a null pages array, or a zero page, unmaps). */
BOOL WINAPI XMemMapPhysicalPagesScatter(void **addresses, ULONG_PTR count, ULONG_PTR *pages)
{
    BOOL ok = TRUE;

    if (!addresses)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    {
        static LONG calls;
        if (InterlockedIncrement(&calls) <= 4)
            rebox_log("xmem: XMemMapPhysicalPagesScatter(%lu pages, first at %p)", (unsigned long)count,
                      count ? addresses[0] : NULL);
    }
    for (ULONG_PTR i = 0; i < count; i++)
        if (addresses[i])
            ok &= XMemMapPhysicalPages(addresses[i], 1, pages ? &pages[i] : NULL);
    return ok;
}



/* Placeholder ranges made for physical pages (XMemVirtualAlloc). Releasing one releases each of
 * the placeholders it was split into, unmapping pages still mapped there. */
static struct { ULONG_PTR base, size; } physical_ranges[256];

static void note_physical_range(void *base, SIZE_T size)
{
    AcquireSRWLockExclusive(&physical_lock);
    for (UINT i = 0; i < ARRAYSIZE(physical_ranges); i++)
        if (!physical_ranges[i].size)
        {
            physical_ranges[i].base = (ULONG_PTR)base;
            physical_ranges[i].size = size;
            break;
        }
    ReleaseSRWLockExclusive(&physical_lock);
}


static BOOL in_physical_range(const void *address)
{
    BOOL found = FALSE;
    AcquireSRWLockShared(&physical_lock);
    for (UINT i = 0; i < ARRAYSIZE(physical_ranges) && !found; i++)
        found = physical_ranges[i].size && (ULONG_PTR)address - physical_ranges[i].base < physical_ranges[i].size;
    ReleaseSRWLockShared(&physical_lock);
    return found;
}
static BOOL release_physical_range(void *base)
{
    ULONG_PTR start = 0, end = 0;

    AcquireSRWLockExclusive(&physical_lock);
    for (UINT i = 0; i < ARRAYSIZE(physical_ranges); i++)
        if (physical_ranges[i].size && physical_ranges[i].base == (ULONG_PTR)base)
        {
            start = physical_ranges[i].base;
            end = start + physical_ranges[i].size;
            physical_ranges[i].size = 0;
            break;
        }
    if (end)
        unmap_range(start, end); /* views leave placeholders: released below */
    for (ULONG_PTR at = start; at < end;)
    {
        MEMORY_BASIC_INFORMATION mbi;
        SIZE_T size;
        if (!VirtualQuery((void *)at, &mbi, sizeof(mbi)) || !mbi.RegionSize)
            break;
        size = min(mbi.RegionSize, end - at);
        if (mbi.State != MEM_FREE)
            VirtualFree((void *)at, 0, MEM_RELEASE);
        at += size;
    }
    ReleaseSRWLockExclusive(&physical_lock);
    return end != 0;
}
typedef void (WINAPI *title_memory_written_fn)(UINT64, UINT64);

/* Writes size bytes into physical pages, from offset bytes into the first: a DirectStorage request
 * whose destination is a page array (xcrdapi). The bytes go to the pages' sections, which every
 * view of them shares; a page mapped as a copy gets them too. The address each page was last
 * mapped at is reported to d3d12_xs, which uploads textures placed there. FALSE if a page isn't
 * one this process allocated. */
BOOL WINAPI ReboxWritePhysicalPages(const ULONG_PTR *pages, ULONG_PTR count, SIZE_T offset, const void *data,
                                    SIZE_T size)
{
    static title_memory_written_fn written;
    static BOOL resolved;
    const BYTE *from = data;
    BOOL ok = TRUE;

    if (!resolved)
    {
        HMODULE m = GetModuleHandleA("d3d12_xs.dll");
        written = m ? (title_memory_written_fn)(void *)GetProcAddress(m, "ReboxTitleMemoryWritten") : NULL;
        resolved = TRUE;
    }
    AcquireSRWLockExclusive(&physical_lock);
    for (ULONG_PTR i = 0; i < count && size; i++)
    {
        ULONG_PTR u = page_unit(pages[i]);
        SIZE_T page_size, n;

        if (u == (ULONG_PTR)-1)
        {
            ok = FALSE;
            break;
        }
        page_size = (SIZE_T)units[u].size_units * PAGE_UNIT;
        if (offset >= page_size)
        {
            offset -= page_size;
            continue;
        }
        n = min(size, page_size - offset);
        memcpy(unit_home(u) + offset, from, n);
        if (units[u].copy_at)
            memcpy((BYTE *)units[u].copy_at + offset, from, n);
        if (units[u].mapped && written)
            written((UINT64)(units[u].mapped + offset), n);
        if (units[u].copy_at && units[u].copy_at != units[u].mapped && written)
            written((UINT64)(units[u].copy_at + offset), n);
        from += n;
        size -= n;
        offset = 0;
    }
    ReleaseSRWLockExclusive(&physical_lock);
    return ok && !size;
}

BOOL WINAPI XMemTransferMemory(UINT direction, SIZE_T size)
{
    return TRUE;
}

ULONGLONG WINAPI XMemMakeTag(UINT32 tag)
{
    return (ULONGLONG)tag << 32;
}

BOOL WINAPI XMemIsToolsMemAvailable(void)
{
    return FALSE;
}

SIZE_T WINAPI XMemGetAllocationHysteresis(void)
{
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return (SIZE_T)-1;
}
