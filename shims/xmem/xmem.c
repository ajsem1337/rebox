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
        return xmem_low_alloc2(NULL, size, type, protect);
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
 *   type     MEM_PHYSICAL (0x400000): a range the title maps physical pages into. Here that's an
 *            ordinary reservation, which XMemMapPhysicalPages commits;
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
    if (type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER))
        p = virtual_alloc2(address, size, host_type | (type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER)),
                           host_protect);
    else
        p = xmem_low_virtual_alloc(address, size, host_type, host_protect);

    if (!p ? InterlockedIncrement(&failures) <= 16 : InterlockedIncrement(&calls) <= 4)
        rebox_log("xmem: XMemVirtualAlloc(%p, %#zx, type %#lx, flags %#llx, protect %#lx) -> %p (error %lu)", address,
                     size, type, (unsigned long long)xmem_flags, protect, p, p ? 0 : GetLastError());
    return p;
}

BOOL WINAPI XMemVirtualFree(void *address, SIZE_T size, DWORD type)
{
    return VirtualFree(address, size, type);
}

/* Physical pages. A title allocates page numbers, then maps them into a range it reserved, and
 * unmaps them (a null page list) to reuse the range: a streaming pool fills pages at one address,
 * unmaps them and maps the same pages elsewhere, where it reads what it wrote. A process here can't
 * own physical pages, so a page's contents move with it: mapping commits ordinary memory at the
 * address and copies in what the page last held, unmapping copies the contents aside and decommits.
 * (A page mapped at two addresses at once doesn't alias: the second mapping gets a copy.)
 *
 * Page numbers are 4 KiB frame numbers, as the console's are (xcl, ivall/xcl; XValkyrie #10): a
 * 64 KiB page is (index + 1) * 16, and a title packs them as number >> 4 and back as
 * (packed << 4) & 0x1fffff0, which round-trips only so. A 4 MiB page (MEM_4MB_PAGES) takes 64 units
 * of 64 KiB. Signatures as XValkyrie and TomBox read them: no process argument, BOOL results. */
#define PAGE_UNIT (64u << 10)

struct physical_unit
{
    void *saved;     /* contents while unmapped, or NULL (never written, or mapped) */
    ULONG_PTR mapped; /* the address it is mapped at, or 0 */
    UINT size_units;  /* at a page's first unit: its size in 64 KiB units (1 or 64) */
};

static SRWLOCK physical_lock = SRWLOCK_INIT;
static struct physical_unit *units;
static ULONG_PTR unit_count, unit_capacity;

/* Address -> first unit of the page mapped there: open addressing on 64 KiB-aligned addresses. */
#define MAPPED_SLOTS (1u << 20)
static struct { ULONG_PTR address, unit; } *mapped_at;

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
    return u < unit_count && units[u].size_units ? u : (ULONG_PTR)-1;
}

BOOL WINAPI XMemAllocatePhysicalPages(ULONGLONG flags, ULONG_PTR *count, ULONG_PTR *pages)
{
    UINT size_units = (flags & MEM_4MB_PAGES) ? 64 : 1;
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
            pages[i] = (unit_count + 1) << 4;
            unit_count += size_units;
        }
    ReleaseSRWLockExclusive(&physical_lock);
    if (!ok)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return ok;
}

BOOL WINAPI XMemFreePhysicalPages(ULONG_PTR count, ULONG_PTR *pages)
{
    AcquireSRWLockExclusive(&physical_lock);
    for (ULONG_PTR i = 0; pages && i < count; i++)
    {
        ULONG_PTR u = page_unit(pages[i]);
        if (u == (ULONG_PTR)-1)
            continue;
        if (units[u].saved)
            VirtualFree(units[u].saved, 0, MEM_RELEASE);
        units[u].saved = NULL;
    }
    ReleaseSRWLockExclusive(&physical_lock);
    return TRUE;
}

/* Unmaps whatever page is mapped at address (its contents kept with the page); returns the bytes
 * it covered, or 0 if none was. Under physical_lock. */
static SIZE_T unmap_page_at(ULONG_PTR address)
{
    SIZE_T slot = mapped_at ? mapped_slot(address, FALSE) : (SIZE_T)-1;
    ULONG_PTR u;
    SIZE_T size;

    if (slot == (SIZE_T)-1 || mapped_at[slot].address != address)
        return 0;
    u = mapped_at[slot].unit;
    mapped_at[slot].address = 1; /* a tombstone */
    size = (SIZE_T)units[u].size_units * PAGE_UNIT;
    if (units[u].mapped == address)
    {
        if (!units[u].saved)
            units[u].saved = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (units[u].saved && !IsBadReadPtr((void *)address, size))
            memcpy(units[u].saved, (void *)address, size);
        units[u].mapped = 0;
    }
    VirtualFree((void *)address, size, MEM_DECOMMIT);
    return size;
}

/* Maps page unit u at address: commits it and copies in the page's contents. Under physical_lock. */
static BOOL map_page_at(ULONG_PTR address, ULONG_PTR u)
{
    SIZE_T size = (SIZE_T)units[u].size_units * PAGE_UNIT, slot;
    static LONG logged;

    unmap_page_at(address);
    /* The range is normally reserved already; if not, reserve it too. A placeholder of exactly
     * this size takes no commit, but can be replaced by committed memory. */
    if (!VirtualAlloc((void *)address, size, MEM_COMMIT, PAGE_READWRITE)
            && !VirtualAlloc((void *)address, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
            && !virtual_alloc2((void *)address, size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE))
    {
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xmem: mapping a physical page of %lu KiB at %#llx failed (error %lu)",
                      (unsigned long)(size >> 10), (unsigned long long)address, GetLastError());
        return FALSE;
    }
    if (units[u].mapped)
        memcpy((void *)address, (void *)units[u].mapped, size); /* mapped elsewhere too: a copy */
    else if (units[u].saved)
    {
        memcpy((void *)address, units[u].saved, size);
        VirtualFree(units[u].saved, 0, MEM_RELEASE);
        units[u].saved = NULL;
    }
    else
        memset((void *)address, 0, size);
    if (!units[u].mapped)
        units[u].mapped = address;
    if ((slot = mapped_slot(address, TRUE)) != (SIZE_T)-1)
    {
        mapped_at[slot].address = address;
        mapped_at[slot].unit = u;
    }
    return TRUE;
}

BOOL WINAPI XMemMapPhysicalPages(void *address, ULONG_PTR count, ULONG_PTR *pages)
{
    ULONG_PTR at = (ULONG_PTR)address;
    BOOL ok = TRUE;

    if (!address || !count)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    AcquireSRWLockExclusive(&physical_lock);
    for (ULONG_PTR i = 0; i < count; i++)
    {
        ULONG_PTR u = pages ? page_unit(pages[i]) : (ULONG_PTR)-1;
        if (!pages || !pages[i])
        {
            /* Unmapping: the size is the page's that was there (64 KiB if none). */
            SIZE_T size = unmap_page_at(at);
            if (!size)
            {
                VirtualFree((void *)at, PAGE_UNIT, MEM_DECOMMIT);
                size = PAGE_UNIT;
            }
            at += size;
            continue;
        }
        if (u == (ULONG_PTR)-1)
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 8)
                rebox_log("xmem: XMemMapPhysicalPages(%p): %#llx isn't a page number this process allocated",
                          address, (unsigned long long)pages[i]);
            SetLastError(ERROR_INVALID_PARAMETER);
            ok = FALSE;
            break;
        }
        ok &= map_page_at(at, u);
        at += (SIZE_T)units[u].size_units * PAGE_UNIT;
    }
    ReleaseSRWLockExclusive(&physical_lock);
    return ok;
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

/* Moves memory between the title's and the system's budgets, which aren't kept apart here. */
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
