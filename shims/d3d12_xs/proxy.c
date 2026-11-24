/* D3D12.X proxies for the device, command queues, command lists and
 * descriptor heaps.
 *
 * Each proxy has the Xbox vtable layout and forwards to the vkd3d-proton
 * object it wraps. Slots that match PC go through a generated three-instruction
 * thunk (load the inner object, jump to its PC slot), so argument lists don't
 * need to be known. Slots that differ get C implementations, and slots nobody
 * has identified go to a trap that logs the slot and returns E_NOTIMPL. Slot
 * numbers and evidence are in docs/d3d12x.md.
 *
 * Title code inlines hot command-list methods and reads and writes fields of
 * the command-list object (cursor +0x18, limits +0x28/+0x30, state up to
 * ~+0x564). Every proxy keeps those first 0x1000 bytes zeroed: cursor ==
 * limit == 0, so the inline paths always see a full buffer and call the
 * vtable. The wrapped object lives at +0x1000, out of their reach.
 *
 * Resources, heaps, PSOs, root signatures, fences and allocators are not
 * proxied: their vtables match PC wherever we have evidence, and leaving them
 * raw avoids unwrapping them in every call that takes one. Their GetDevice
 * is patched in vkd3d-proton's shared vtables instead (patch_child), so it
 * hands back the device proxy rather than the PC device. */

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "gpu_memory.h"
#include "shadow.h"
#include "console_compile.h"
#include "pacing.h"
#include "present.h"
#include "proxy.h"
#include "title_textures.h"
#include "dxgi_device.h"
#include "derived_pso.h"
#include "state_object.h"

#define INNER_OFFSET 0x1000
#define DEVICE_OUT_OF_LINE_FLAG 0x18
#define MAX_SLOTS 160

struct proxy_class
{
    const char *name;
    unsigned slots;
    void **vtbl;
    LONG trapped[MAX_SLOTS];
    LONG64 calls[MAX_SLOTS]; /* REBOX_SLOT_STATS */
    BOOL custom[MAX_SLOTS], forwarded[MAX_SLOTS];
};

/* Command-list data from slot 148, freed a few Resets after use. */
struct embedded
{
    BYTE *cpu;
    SIZE_T size, used;
    unsigned generation;
    struct embedded *next;
};

struct cpu_write { UINT64 address; UINT32 values[2]; UINT count; };
struct memory_wait { UINT64 address, value, mask; UINT32 comparison, width; };

struct proxy
{
    void **vtbl;
    BYTE inline_state[INNER_OFFSET - sizeof(void *)]; /* must stay zero, see above */
    IUnknown *inner;
    struct proxy_class *cls;
    LONG refs;
    struct embedded *active, *retired;
    unsigned resets;
    struct present_queue *present; /* queues that have presented */
    ID3D12DescriptorHeap *heaps[2]; /* lists: last SetDescriptorHeaps, for ReboxBlit to restore */
    UINT heap_count;
    BOOL ray_pipeline; /* lists: a real ray tracing pipeline is set (not none, not a placeholder: state_object.c) */
    int display_bound; /* lists: 1 + which display-sized target is bound, or 0 (counted for the log) */
    BOOL rect_list;    /* lists: the console's rectangle list (topology 17) is set */
    UINT topology;     /* lists: the last IASetPrimitiveTopology (logged at a watched stage's draws) */
    D3D12_VERTEX_BUFFER_VIEW vertex_buffers[8]; /* lists: as bound, for rect lists (list_DrawInstanced) */
    UINT vertex_buffer_mask;
    D3D12_INDEX_BUFFER_VIEW index_buffer;      /* lists: as bound, for REBOX_WATCH_STAGE's vertex log */
    SIZE_T render_target0;     /* lists: the first bound render target view (REBOX_WATCH_STAGE logs it) */
    struct cpu_write *cpu_writes;  /* lists: writes outside GPU memory, made after the GPU's work (cpu_writes_*) */
    UINT cpu_write_count, cpu_write_capacity;
    struct cpu_write *gpu_writes;  /* lists: writes to GPU memory (WriteBufferImmediate), for list_wait */
    UINT gpu_write_count, gpu_write_capacity;
    struct memory_wait *waits;     /* lists: Wait*BitValueX, evaluated on the CPU before submission */
    UINT wait_count, wait_capacity;
    ID3D12Fence *cpu_fence;        /* queues: signalled after submissions that carry CPU writes */
    UINT64 cpu_fence_value;
    struct fence_cell *fence_cell; /* fences: the completed value slot 12 handed out */
    volatile LONG64 fence_signalled; /* fences: the highest value a queue was told to signal */
};

C_ASSERT(offsetof(struct proxy, inner) == INNER_OFFSET);

static struct proxy_class device_class = { "Device", MAX_SLOTS };
static struct proxy_class queue_class = { "CommandQueue", MAX_SLOTS };
static struct proxy_class queue_v4_class = { "CommandQueue (SDK 0x20004)", MAX_SLOTS };
static struct proxy_class list_class = { "GraphicsCommandList", MAX_SLOTS };
static struct proxy_class list_v4_class = { "GraphicsCommandList (SDK 0x20004)", MAX_SLOTS };
static void *fence_vtbl;              /* vkd3d-proton's fence class (is_fence) */
static volatile LONG newer_layouts;  /* the title's runtime has the newer queue/list layouts */
static struct proxy_class copy_list_class = { "CopyCommandList", MAX_SLOTS };
static struct proxy_class heap_class = { "DescriptorHeap", MAX_SLOTS };
static struct proxy_class allocator_class = { "CommandAllocator", MAX_SLOTS };  /* every class: a call past the end traps, not jumps */
static struct proxy_class fence_class = { "Fence", MAX_SLOTS };

static struct proxy_class *const classes[] = { &device_class, &queue_class, &queue_v4_class, &list_class, &list_v4_class,
                                               &copy_list_class,
                                               &heap_class, &allocator_class, &fence_class };

/* The console's primitive topology type for rectangle lists (TuxBox): drawn as triangle lists
 * here (see the rectangle lists' topology), so the pipeline is a triangle one. */
#define CONSOLE_TOPOLOGY_TYPE_RECTANGLE 0x13

static DXGI_FORMAT pc_format(DXGI_FORMAT format);
static const D3D12_CLEAR_VALUE *pc_clear(const D3D12_CLEAR_VALUE *clear, D3D12_CLEAR_VALUE *storage);

/* D3D12XBOX_CREATE_DEVICE_PARAMETERS.Version the title passed (0 if none). */
static UINT32 sdk_version;

void proxy_set_sdk_version(UINT32 version)
{
    sdk_version = version;
    /* Only 0x20004 runtimes come in two layouts; an older version can't have the newer one. */
    if (version < 0x20004)
        newer_layouts = 0;
}

static struct proxy *device_proxy;

#define INNER(p, type) ((type *)(p)->inner)
/* Call the wrapped object's PC vtable slot directly. */
#define PC_SLOT(obj, slot) ((*(void ***)(obj))[slot])

/* ----------------------------------------------------------------- thunks */

static BYTE *thunk_alloc(SIZE_T n)
{
    static BYTE *cur, *end;
    BYTE *p;

    n = (n + 15) & ~(SIZE_T)15;
    if (!cur || cur + n > end)
    {
        cur = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        end = cur + 0x10000;
    }
    p = cur;
    cur += n;
    return p;
}

/* mov rcx, [rcx+INNER_OFFSET]; mov rax, [rcx]; jmp [rax+slot*8] */
static void *forward_thunk(unsigned pc_slot)
{
    BYTE *t = thunk_alloc(16);
    UINT32 inner = INNER_OFFSET, disp = pc_slot * 8;

    t[0] = 0x48; t[1] = 0x8b; t[2] = 0x89; memcpy(t + 3, &inner, 4);
    t[7] = 0x48; t[8] = 0x8b; t[9] = 0x01;
    t[10] = 0xff; t[11] = 0xa0; memcpy(t + 12, &disp, 4);
    return t;
}

HRESULT WINAPI proxy_trap(struct proxy_class *cls, unsigned slot, void *self, UINT_PTR a1, UINT_PTR a2, UINT_PTR a3);
void proxy_trap_common(void);

/* r11 = class, r10d = slot, then into proxy_trap_common, which shuffles them
 * in front of the original four register arguments. */
__asm__(
    ".text\n"
    ".globl proxy_trap_common\n"
    "proxy_trap_common:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    subq $0x30, %rsp\n"
    "    movq %r8, 0x20(%rsp)\n"
    "    movq %r9, 0x28(%rsp)\n"
    "    movq %rdx, %r9\n"
    "    movq %rcx, %r8\n"
    "    movl %r10d, %edx\n"
    "    movq %r11, %rcx\n"
    "    call proxy_trap\n"
    "    leave\n"
    "    ret\n");

/* mov r11, cls; mov r10d, slot; mov rax, proxy_trap_common; jmp rax */
static void *trap_thunk(struct proxy_class *cls, unsigned slot)
{
    BYTE *t = thunk_alloc(32);
    UINT64 c = (UINT_PTR)cls, target = (UINT_PTR)proxy_trap_common;
    UINT32 s = slot;

    t[0] = 0x49; t[1] = 0xbb; memcpy(t + 2, &c, 8);
    t[10] = 0x41; t[11] = 0xba; memcpy(t + 12, &s, 4);
    t[16] = 0x48; t[17] = 0xb8; memcpy(t + 18, &target, 8);
    t[26] = 0xff; t[27] = 0xe0;
    return t;
}

HRESULT WINAPI proxy_trap(struct proxy_class *cls, unsigned slot, void *self, UINT_PTR a1, UINT_PTR a2, UINT_PTR a3)
{
    char name[96];

    snprintf(name, sizeof(name), "ID3D12%s slot %u (+%#x)", cls->name, slot, slot * 8);
    if (!cls->trapped[slot])
        rebox_log("d3d12_xs: Xbox %s called (this %p, args %#llx %#llx %#llx)", name, self,
                     (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3);
    rebox_unimpl("d3d12_xs", name, &cls->trapped[slot]);
    return E_NOTIMPL;
}

/* --------------------------------------------------------------- objects */

static BOOL is_proxy(const void *object)
{
    void *vtbl;

    if (!object)
        return FALSE;
    vtbl = *(void *const *)object;
    for (unsigned i = 0; i < ARRAYSIZE(classes); i++)
        if (vtbl == classes[i]->vtbl)
            return TRUE;
    return FALSE;
}

/* A struct-returning method (GetResourceAllocationInfo, GetCustomHeapProperties, GetDesc...) is
 * called either as (object, result) or in the console's order (result, object): TRUE for the
 * console's. When both look like objects (a title's stack buffer can hold a stale copy of the
 * object's vtable pointer; xcl, ivall/xcl 7a033c8: the result then went over the device), the one on
 * this thread's stack is the result. */
static BOOL result_first(const void *first, const void *second)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    BOOL first_object = is_proxy(first), second_object = is_proxy(second);

    if (!first_object || !second_object)
        return !first_object && second_object;
    return (ULONG_PTR)first >= (ULONG_PTR)tib->StackLimit && (ULONG_PTR)first < (ULONG_PTR)tib->StackBase;
}

void *proxy_unwrap(void *object)
{
    return is_proxy(object) ? ((struct proxy *)object)->inner : object;
}

/* Takes over the caller's reference to inner. */
static struct proxy *proxy_create(IUnknown *inner, struct proxy_class *cls)
{
    struct proxy *p = calloc(1, sizeof(*p));

    p->vtbl = cls->vtbl;
    p->inner = inner;
    p->cls = cls;
    p->refs = 1;
    return p;
}

static void embedded_free_list(struct embedded *e)
{
    while (e)
    {
        struct embedded *next = e->next;
        ReboxGpuFree(e->cpu);
        free(e);
        e = next;
    }
}

/* Console-only interface IDs (XValkyrie's June 2020 kit tables; Device8 also xbox-emulator
 * 5a25b00), which titles compiled with the console headers ask for by value: the proxy answers for
 * them as it does for the PC versions. IGraphicsUnknown is the base of every console object. */
static const GUID console_device8_iid = { 0x728f1fa2, 0xea11, 0x4a9f, { 0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9 } };
static const GUID console_list6_iid = { 0xc3827890, 0x4024, 0x474c, { 0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74 } };
static const GUID console_dma_list_iid = { 0x30bc77b9, 0x8379, 0x4b25, { 0x95, 0xde, 0xb8, 0x58, 0x8c, 0x42, 0x27, 0x5d } };
static const GUID graphics_unknown_iid = { 0xaceeea63, 0xe0a9, 0x4a1c, { 0xbb, 0xec, 0x71, 0xb2, 0xf4, 0x85, 0xf7, 0x58 } };

static BOOL console_iid(const struct proxy_class *cls, REFIID riid)
{
    if (IsEqualGUID(riid, &graphics_unknown_iid))
        return TRUE;
    if (cls == &device_class)
        return IsEqualGUID(riid, &console_device8_iid);
    if (cls == &list_class || cls == &list_v4_class)
    {
        /* The console's command list IIDs keep PC's first fields and end in 98cf-645b51dc4874
         * (list 6 above; a UE5 title asks for list 5 as 55050859-4024-474c-98cf-...). */
        static const DWORD pc_lists[] = { 0x5b160d0f, 0x553103fb, 0x38c3e585, 0x6fda83a7, 0x8754318e, 0x55050859,
                                          0xc3827890, 0xdd171223, 0xee936ef9, 0x7013c015 };
        if (IsEqualGUID(riid, &console_list6_iid))
            return TRUE;
        if (riid->Data4[0] == 0x98 && riid->Data4[1] == 0xcf)
            for (size_t i = 0; i < ARRAYSIZE(pc_lists); i++)
                if (riid->Data1 == pc_lists[i])
                    return TRUE;
        return FALSE;
    }
    if (cls == &copy_list_class)
        return IsEqualGUID(riid, &console_dma_list_iid);
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE proxy_QueryInterface(struct proxy *p, REFIID riid, void **out)
{
    IUnknown *result;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    /* The console device is also an IDXGIDevice, which titles use without checking. */
    if (p->cls == &device_class && dxgi_device_iid(riid))
    {
        *out = dxgi_device_get((IUnknown *)p);
        return S_OK;
    }
    /* vkd3d-proton returns the same pointer for every version of an
     * interface (and for IUnknown/ID3D12Object/ID3D12DeviceChild), so "same
     * object back" means the proxy can answer for it. */
    if (console_iid(p->cls, riid))
    {
        InterlockedIncrement(&p->refs);
        *out = p;
        return S_OK;
    }
    if (FAILED(hr = IUnknown_QueryInterface(p->inner, riid, (void **)&result)))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("d3d12_xs: Xbox %s has no interface {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                         p->cls->name, riid->Data1, riid->Data2, riid->Data3, riid->Data4[0], riid->Data4[1],
                         riid->Data4[2], riid->Data4[3], riid->Data4[4], riid->Data4[5], riid->Data4[6],
                         riid->Data4[7]);
        return hr;
    }
    if (result == p->inner)
    {
        IUnknown_Release(result);
        InterlockedIncrement(&p->refs);
        *out = p;
        return S_OK;
    }
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: QueryInterface on Xbox %s returned an unwrapped object", p->cls->name);
    }
    *out = result;
    return S_OK;
}

static ULONG STDMETHODCALLTYPE proxy_AddRef(struct proxy *p)
{
    return InterlockedIncrement(&p->refs);
}

/* Command lists made before the title's layout was known (on the older table) move to the newer one
 * when it is (lists_to_newer_layout): the target title makes lists before any fence or newer device
 * method gives it away, and then calls their newer console methods. */
static struct proxy **older_lists;
static SIZE_T older_list_count, older_list_capacity;
static SRWLOCK older_lists_lock = SRWLOCK_INIT;

static void older_list_add(struct proxy *p)
{
    AcquireSRWLockExclusive(&older_lists_lock);
    if (older_list_count == older_list_capacity)
    {
        SIZE_T n = older_list_capacity ? older_list_capacity * 2 : 64;
        struct proxy **grown = realloc(older_lists, n * sizeof(*grown));
        if (grown)
        {
            older_lists = grown;
            older_list_capacity = n;
        }
    }
    if (older_list_count < older_list_capacity)
        older_lists[older_list_count++] = p;
    ReleaseSRWLockExclusive(&older_lists_lock);
}

static void older_list_remove(struct proxy *p)
{
    AcquireSRWLockExclusive(&older_lists_lock);
    for (SIZE_T i = 0; i < older_list_count; i++)
        if (older_lists[i] == p)
        {
            older_lists[i] = older_lists[--older_list_count];
            break;
        }
    ReleaseSRWLockExclusive(&older_lists_lock);
}

static void lists_to_newer_layout(void)
{
    SIZE_T moved;

    AcquireSRWLockExclusive(&older_lists_lock);
    for (SIZE_T i = 0; i < older_list_count; i++)
    {
        older_lists[i]->cls = &list_v4_class;
        InterlockedExchangePointer((void **)&older_lists[i]->vtbl, list_v4_class.vtbl);
    }
    moved = older_list_count;
    older_list_count = 0;
    ReleaseSRWLockExclusive(&older_lists_lock);
    if (moved)
        rebox_log("d3d12_xs: %zu command lists made before the layout was known now have the newer one", moved);
}

/* A device method only the newer layout has: the title's runtime has the newer layouts (xcl's
 * note_newer_device_method). */
static void note_newer_device_method(UINT slot)
{
    if (sdk_version >= 0x20004 && !InterlockedExchange(&newer_layouts, 1))
    {
        rebox_log("d3d12_xs: device slot %u, which only the newer layout has: the title's runtime has the newer "
                  "layouts", slot);
        lists_to_newer_layout();
    }
}

static ULONG STDMETHODCALLTYPE proxy_Release(struct proxy *p)
{
    ULONG refs = InterlockedDecrement(&p->refs);

    if (!refs && p != device_proxy)
    {
        embedded_free_list(p->active);
        embedded_free_list(p->retired);
        present_queue_destroy(p->present);
        if (p->cls == &heap_class)
            shadow_heap_destroyed((ID3D12DescriptorHeap *)p->inner);
        if (p->cls == &list_class)
            older_list_remove(p);
        IUnknown_Release(p->inner);
        free(p);
    }
    return refs;
}

/* ID3D12DeviceChild::GetDevice on a proxied child. */
static HRESULT STDMETHODCALLTYPE child_GetDevice(struct proxy *p, REFIID riid, void **out)
{
    if (!device_proxy)
        return E_FAIL;
    return proxy_QueryInterface(device_proxy, riid, out);
}

/* -------------------------------------------- GetDevice on raw children
 *
 * vkd3d-proton shares one vtable per object class. Slot 7 (GetDevice) of
 * each class's vtable is redirected here; objects of the shared device get
 * the device proxy, objects of any other (PC-path) device keep their own. */

typedef HRESULT (STDMETHODCALLTYPE *get_device_fn)(void *, REFIID, void **);

static struct
{
    void **vtbl;
    get_device_fn orig;
} child_vtbls[32];
static LONG child_vtbl_count;
static SRWLOCK child_lock = SRWLOCK_INIT;

static HRESULT STDMETHODCALLTYPE raw_child_GetDevice(void *self, REFIID riid, void **out)
{
    void **vtbl = *(void ***)self;
    get_device_fn orig = NULL;
    IUnknown *device = NULL;

    AcquireSRWLockShared(&child_lock);
    for (LONG i = 0; i < child_vtbl_count; i++)
        if (child_vtbls[i].vtbl == vtbl)
            orig = child_vtbls[i].orig;
    ReleaseSRWLockShared(&child_lock);
    if (!orig)
        return E_FAIL;
    if (device_proxy && SUCCEEDED(orig(self, &IID_ID3D12Device, (void **)&device)))
    {
        BOOL shared = device == device_proxy->inner;
        IUnknown_Release(device);
        if (shared)
            return proxy_QueryInterface(device_proxy, riid, out);
    }
    return orig(self, riid, out);
}

static void patch_child(void *object)
{
    void **vtbl;
    DWORD old;

    if (!object)
        return;
    vtbl = *(void ***)object;
    if (vtbl[7] == (void *)raw_child_GetDevice)
        return;
    AcquireSRWLockExclusive(&child_lock);
    if (vtbl[7] != (void *)raw_child_GetDevice && child_vtbl_count < (LONG)ARRAYSIZE(child_vtbls))
    {
        /* Register first: another thread may call through the slot as
         * soon as it is written. */
        child_vtbls[child_vtbl_count].vtbl = vtbl;
        child_vtbls[child_vtbl_count].orig = vtbl[7];
        child_vtbl_count++;
        VirtualProtect(&vtbl[7], sizeof(void *), PAGE_READWRITE, &old);
        vtbl[7] = (void *)raw_child_GetDevice;
        VirtualProtect(&vtbl[7], sizeof(void *), old, &old);
    }
    ReleaseSRWLockExclusive(&child_lock);
}

/* ------------------------------------------ texture GPU VAs
 *
 * On the console a texture's GPU VA is the address of its memory, and titles
 * write texel data through it (ScarlettX saw this in a retail title).
 * vkd3d-proton returns 0 for textures, so each texture that is asked gets a
 * block of GPU memory at least its console size, registered as a title texture
 * (title_textures.c): what the title writes there, tiled as on the console, is
 * detiled into the texture before the queue runs, as for CreatePlacedResourceX.
 * Render targets, depth and UAV textures keep plain scratch. The block is freed
 * with the texture. */

typedef UINT64 (STDMETHODCALLTYPE *get_va_fn)(void *);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

static get_va_fn orig_resource_get_va;
static release_fn orig_resource_release;

static struct scratch
{
    void *resource, *cpu;
    BOOL mapped_here;   /* Map gave this memory (vkd3d-proton refused the texture) */
    struct scratch *next;
} *scratch_list;
static SRWLOCK scratch_lock = SRWLOCK_INIT;

/* The CPU address behind an address in a GPU-only view of title memory (xmem's ReboxGpuViewToCpu),
 * or the address itself. */
static UINT64 gpu_view_to_cpu(UINT64 address)
{
    static ULONGLONG (WINAPI *volatile to_cpu)(ULONGLONG);
    static volatile LONG resolved;
    UINT64 cpu;

    if (!resolved) /* a race only looks the export up twice */
    {
        to_cpu = (void *)GetProcAddress(GetModuleHandleA("xmem.dll"), "ReboxGpuViewToCpu");
        resolved = 1;
    }
    cpu = to_cpu ? to_cpu(address) : address;
    return cpu;
}

/* Textures by the address the title knows them at (a T# it writes itself holds it): display
 * targets' console memory (slot 127), textures whose GPU VA it asked (scratch), and textures
 * placed in its memory (title_textures.c). */
static struct { void *resource; UINT64 address; } texture_addresses[64];
static SRWLOCK texture_address_lock = SRWLOCK_INIT;

static void note_texture_address(void *resource, UINT64 address)
{
    AcquireSRWLockExclusive(&texture_address_lock);
    for (UINT i = 0; i < ARRAYSIZE(texture_addresses); i++)
        if (!texture_addresses[i].resource || texture_addresses[i].resource == resource)
        {
            texture_addresses[i].resource = resource;
            texture_addresses[i].address = address;
            break;
        }
    ReleaseSRWLockExclusive(&texture_address_lock);
}

ID3D12Resource *proxy_texture_at(UINT64 address)
{
    ID3D12Resource *res = NULL;
    struct scratch *sc;

    if (!address)
        return NULL;
    address = gpu_view_to_cpu(address); /* title textures there are placed over the CPU view */
    AcquireSRWLockShared(&texture_address_lock);
    for (UINT i = 0; i < ARRAYSIZE(texture_addresses) && !res; i++)
        if (texture_addresses[i].resource && texture_addresses[i].address == address)
            res = texture_addresses[i].resource;
    ReleaseSRWLockShared(&texture_address_lock);
    if (!res)
    {
        AcquireSRWLockShared(&scratch_lock);
        for (sc = scratch_list; sc && !res; sc = sc->next)
            if ((UINT64)(ULONG_PTR)sc->cpu == address)
                res = sc->resource;
        ReleaseSRWLockShared(&scratch_lock);
    }
    return res ? res : title_texture_at(address);
}

static UINT64 STDMETHODCALLTYPE resource_GetGPUVirtualAddress(void *self)
{
    UINT64 va = orig_resource_get_va(self);
    D3D12_RESOURCE_ALLOCATION_INFO info;
    D3D12_RESOURCE_DESC desc;
    struct scratch *sc;
    void *cpu = NULL;
    UINT64 size;
    static LONG logged;

    UINT64 title_address;
    UINT uploads;

    if (va || !device_proxy)
        return va;
    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)self)[10])(self, &desc);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return 0;
    /* A texture the title placed at its own address has that address, as on the console (a title
     * clears a texture through the CPU pointer its address is; after xcl, ivall/xcl#168). */
    if (title_texture_info(self, &title_address, &uploads) && title_address)
        return title_address;

    AcquireSRWLockShared(&scratch_lock);
    for (sc = scratch_list; sc && sc->resource != self; sc = sc->next)
        ;
    if (sc)
        cpu = sc->cpu;
    ReleaseSRWLockShared(&scratch_lock);
    if (cpu)
        return (UINT64)(ULONG_PTR)cpu;

    ((D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
            const D3D12_RESOURCE_DESC *))PC_SLOT(device_proxy->inner, 25))(device_proxy->inner, &info, 0, 1, &desc);
    size = max(info.SizeInBytes, title_texture_size(&desc));
    if (FAILED(ReboxGpuAlloc(size ? size : 65536, 64 << 10, &cpu, NULL)))
        return 0;
    memset(cpu, 0, size);
    /* Render targets, depth and UAV textures are the GPU's to write: uploading over them would
     * lose what it drew, so their blocks stay scratch. */
    if (!(desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                        | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
    {
        /* Registered with a zeroed console desc tail (+0x34, +0x38: only logged). */
        BYTE console_desc[0x40] = { 0 };
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        memcpy(console_desc, &desc, sizeof(desc));
        present_tracked_state(self, &state);
        title_texture_register(self, (UINT64)(ULONG_PTR)cpu, (const D3D12_RESOURCE_DESC *)console_desc, &desc, state);
    }
    if (InterlockedIncrement(&logged) <= 4)
        rebox_log("d3d12_xs: title asked for a texture's GPU VA (%ux%u, format %u, flags %#x); %llu bytes of GPU "
                     "memory there%s", (UINT)desc.Width, desc.Height, desc.Format, desc.Flags, (unsigned long long)size,
                     desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                   | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                     ? " (scratch: the GPU writes it)" : ", uploaded into the texture when it changes");
    sc = calloc(1, sizeof(*sc));
    sc->resource = self;
    sc->cpu = cpu;
    AcquireSRWLockExclusive(&scratch_lock);
    sc->next = scratch_list;
    scratch_list = sc;
    ReleaseSRWLockExclusive(&scratch_lock);
    return (UINT64)(ULONG_PTR)cpu;
}

static ULONG STDMETHODCALLTYPE resource_Release(void *self)
{
    ULONG refs = orig_resource_release(self);
    struct scratch **p, *sc = NULL;

    if (refs || !scratch_list)
        return refs;
    AcquireSRWLockExclusive(&scratch_lock);
    for (p = &scratch_list; *p && (*p)->resource != self; p = &(*p)->next)
        ;
    if ((sc = *p))
        *p = sc->next;
    ReleaseSRWLockExclusive(&scratch_lock);
    if (sc)
    {
        ReboxGpuFree(sc->cpu);
        free(sc);
    }
    return refs;
}

/* Struct-returning methods: console titles call them with the return pointer first and the
 * object second (a UE5 title's GetResourceAllocationInfo: rcx the result, rdx the device), the C
 * convention, where PC's MSVC member functions put the object first. Small structs (8 bytes:
 * descriptor handles, LUIDs) come back in RAX instead, already handled where they occur. Our
 * own callers use the PC order, so each wrapper looks at which argument is the object. */
typedef D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *resource_get_desc_fn)(void *, D3D12_RESOURCE_DESC *);
static resource_get_desc_fn orig_resource_get_desc;
static void **resource_vtbl;

/* Map/Unmap (slots 8, 9): on the console every resource is in unified memory, so titles map
 * textures too (a GDK title maps one and clears it at startup; vkd3d-proton refuses a DEFAULT
 * heap texture, the pointer stayed garbage, and the clear wrote to 0xf). A texture's subresource 0
 * maps to its console memory, the block GetGPUVirtualAddress gives it (registered as a title
 * texture), and Unmap marks that memory written so the next submission uploads it. */
typedef HRESULT (STDMETHODCALLTYPE *map_fn)(void *, UINT, const D3D12_RANGE *, void **);
typedef void (STDMETHODCALLTYPE *unmap_fn)(void *, UINT, const D3D12_RANGE *);
static map_fn orig_resource_map;
static unmap_fn orig_resource_unmap;

static BOOL texture_mapped_here(void *self)
{
    struct scratch *sc;
    BOOL here = FALSE;
    AcquireSRWLockShared(&scratch_lock);
    for (sc = scratch_list; sc && sc->resource != self; sc = sc->next)
        ;
    if (sc)
        here = sc->mapped_here;
    ReleaseSRWLockShared(&scratch_lock);
    return here;
}

static BOOL texture_block(void *self, void **cpu, UINT64 *size)
{
    struct scratch *sc;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_ALLOCATION_INFO info;

    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))orig_resource_get_desc)(self, &desc);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || !device_proxy)
        return FALSE;
    *cpu = NULL;
    AcquireSRWLockShared(&scratch_lock);
    for (sc = scratch_list; sc && sc->resource != self; sc = sc->next)
        ;
    if (sc)
        *cpu = sc->cpu;
    ReleaseSRWLockShared(&scratch_lock);
    if (!*cpu)
        *cpu = (void *)(ULONG_PTR)resource_GetGPUVirtualAddress(self);
    ((D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
            const D3D12_RESOURCE_DESC *))PC_SLOT(device_proxy->inner, 25))(device_proxy->inner, &info, 0, 1, &desc);
    *size = max(info.SizeInBytes, title_texture_size(&desc));
    return *cpu != NULL;
}

static HRESULT STDMETHODCALLTYPE resource_Map(void *self, UINT subresource, const D3D12_RANGE *range, void **data)
{
    HRESULT hr = orig_resource_map(self, subresource, range, data);
    void *cpu;
    UINT64 size;

    if (SUCCEEDED(hr) || subresource || !texture_block(self, &cpu, &size))
        return hr;
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: Map of a texture (%#lx from vkd3d-proton): its console memory, %llu bytes", hr,
                         (unsigned long long)size);
    }
    {
        struct scratch *sc;
        AcquireSRWLockExclusive(&scratch_lock);
        for (sc = scratch_list; sc && sc->resource != self; sc = sc->next)
            ;
        if (sc)
            sc->mapped_here = TRUE;
        ReleaseSRWLockExclusive(&scratch_lock);
    }
    if (data)
        *data = cpu;
    return S_OK;
}

static void STDMETHODCALLTYPE resource_Unmap(void *self, UINT subresource, const D3D12_RANGE *range)
{
    D3D12_RESOURCE_DESC desc;
    void *cpu;
    UINT64 size;

    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))orig_resource_get_desc)(self, &desc);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !subresource && texture_mapped_here(self)
            && texture_block(self, &cpu, &size))
    {
        ReboxTitleMemoryWritten((UINT64)(ULONG_PTR)cpu, size);
        return;
    }
    orig_resource_unmap(self, subresource, range);
}

static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE resource_GetDesc(void *a, void *b)
{
    if (*(void ***)a != resource_vtbl && b && *(void ***)b == resource_vtbl)
        return orig_resource_get_desc(b, a); /* console order: (result, resource) */
    return orig_resource_get_desc(a, b);
}

/* Heap properties as the title asked for them: buffer heaps it made DEFAULT are CPU-visible
 * custom heaps here (buffer_heap), which a console title never sees. Unreal 5 checks a CUSTOM
 * heap's CPU page property through a pointer that's null on the console (a UE5 sample
 * crashed reading address 4 after placing a transient resource). */
static BOOL cpu_visible_buffers(void);

static void title_heap_properties(D3D12_HEAP_PROPERTIES *props)
{
    if (props && props->Type == D3D12_HEAP_TYPE_CUSTOM && props->CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
            && props->MemoryPoolPreference == D3D12_MEMORY_POOL_L0 && cpu_visible_buffers())
    {
        props->Type = D3D12_HEAP_TYPE_DEFAULT;
        props->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        props->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
}

static HRESULT (STDMETHODCALLTYPE *orig_resource_get_heap_properties)(void *, D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS *);
static HRESULT STDMETHODCALLTYPE resource_GetHeapProperties(void *resource, D3D12_HEAP_PROPERTIES *props,
        D3D12_HEAP_FLAGS *flags)
{
    HRESULT hr = orig_resource_get_heap_properties(resource, props, flags);
    if (SUCCEEDED(hr))
        title_heap_properties(props);
    return hr;
}

/* ID3D12Heap::GetDesc (slot 8) returns its struct through a hidden pointer, as on PC. */
static D3D12_HEAP_DESC *(STDMETHODCALLTYPE *orig_heap_get_desc)(void *, D3D12_HEAP_DESC *);
static D3D12_HEAP_DESC *STDMETHODCALLTYPE placed_heap_GetDesc(void *heap, D3D12_HEAP_DESC *desc)
{
    D3D12_HEAP_DESC *ret = orig_heap_get_desc(heap, desc);
    title_heap_properties(&ret->Properties);
    return ret;
}

static void patch_heap_class(void *heap)
{
    void **vtbl = *(void ***)heap;
    DWORD old;

    if (vtbl[8] == (void *)placed_heap_GetDesc)
        return;
    VirtualProtect(&vtbl[8], sizeof(void *), PAGE_READWRITE, &old);
    orig_heap_get_desc = vtbl[8];
    vtbl[8] = (void *)placed_heap_GetDesc;
    VirtualProtect(&vtbl[8], sizeof(void *), old, &old);
}

static void patch_resource_class(void *resource)
{
    void **vtbl = *(void ***)resource;
    DWORD old;

    if (vtbl[11] == (void *)resource_GetGPUVirtualAddress)
        return;
    VirtualProtect(vtbl, 15 * sizeof(void *), PAGE_READWRITE, &old);
    resource_vtbl = vtbl;
    orig_resource_get_heap_properties = vtbl[14];
    vtbl[14] = (void *)resource_GetHeapProperties;
    orig_resource_release = vtbl[2];
    orig_resource_get_desc = vtbl[10];
    orig_resource_get_va = vtbl[11];
    orig_resource_map = vtbl[8];
    orig_resource_unmap = vtbl[9];
    vtbl[8] = (void *)resource_Map;
    vtbl[9] = (void *)resource_Unmap;
    vtbl[2] = (void *)resource_Release;
    vtbl[10] = (void *)resource_GetDesc;
    vtbl[11] = (void *)resource_GetGPUVirtualAddress;
    VirtualProtect(vtbl, 15 * sizeof(void *), old, &old);
}

/* One object of each class the title gets unwrapped, created and released
 * just to reach its vtable. PSOs need shader bytecode, so their vtable is
 * patched when the first one is created through the proxy instead. */
static void patch_child_classes(ID3D12Device *device)
{
    D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC buffer = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    D3D12_HEAP_DESC heap_desc = { 64 << 10, { D3D12_HEAP_TYPE_DEFAULT }, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS };
    D3D12_QUERY_HEAP_DESC query_desc = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 0 };
    D3D12_INDIRECT_ARGUMENT_DESC arg = { D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH };
    D3D12_COMMAND_SIGNATURE_DESC sig_desc = { sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &arg, 0 };
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 0 };
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize;
    ID3D10Blob *blob = NULL;
    IUnknown *obj;
    HMODULE d3d12;

#define PROBE(call) do { obj = NULL; if (SUCCEEDED(call)) { patch_child(obj); IUnknown_Release(obj); } } while (0)
    obj = NULL;
    if (SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap_props, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&obj)))
    {
        patch_child(obj);
        patch_resource_class(obj);
        IUnknown_Release(obj);
    }
    obj = NULL;
    if (SUCCEEDED(ID3D12Device_CreateHeap(device, &heap_desc, &IID_ID3D12Heap, (void **)&obj)))
    {
        patch_child(obj);
        patch_heap_class(obj);
        IUnknown_Release(obj);
    }
    obj = NULL;
    if (SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&obj)))
    {
        fence_vtbl = *(void **)obj; /* is_fence() */
        patch_child(obj);
        IUnknown_Release(obj);
    }
    PROBE(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
            (void **)&obj));
    PROBE(ID3D12Device_CreateQueryHeap(device, &query_desc, &IID_ID3D12QueryHeap, (void **)&obj));
    PROBE(ID3D12Device_CreateCommandSignature(device, &sig_desc, NULL, &IID_ID3D12CommandSignature, (void **)&obj));
    if ((d3d12 = GetModuleHandleA("d3d12.dll"))
        && (serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        && SUCCEEDED(serialize(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
    {
        PROBE(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
                ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&obj));
        ID3D10Blob_Release(blob);
    }
#undef PROBE
}

/* Wraps a freshly created inner object for the caller. */
static HRESULT wrap_result(HRESULT hr, IUnknown *inner, struct proxy_class *cls, void **out)
{
    if (out)
        *out = NULL;
    if (FAILED(hr) || !inner)
    {
        /* A title asserts on a failed creation without saying which: name it. */
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 16)
            rebox_log("d3d12_xs: creating a %s failed (%#lx)", cls->name, FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    if (!out)
    {
        IUnknown_Release(inner);
        return hr;
    }
    *out = proxy_create(inner, cls);
    return hr;
}

/* ----------------------------------------------------------------- device */

static D3D12_COMMAND_LIST_TYPE pc_list_type(UINT type)
{
    if (type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_BUNDLE
        || type == D3D12_COMMAND_LIST_TYPE_COMPUTE || type == D3D12_COMMAND_LIST_TYPE_COPY)
        return type;
    rebox_log("d3d12_xs: Xbox command list/queue type %#x treated as DIRECT", type);
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

static HRESULT STDMETHODCALLTYPE device_CreateCommandQueue(struct proxy *p, const D3D12_COMMAND_QUEUE_DESC *desc,
        REFIID riid, void **out)
{
    D3D12_COMMAND_QUEUE_DESC d;
    ID3D12CommandQueue *queue = NULL;
    HRESULT hr;

    if (!desc)
        return E_INVALIDARG;
    d = *desc;
    d.Type = pc_list_type(desc->Type);
    if (d.Type == D3D12_COMMAND_LIST_TYPE_BUNDLE)
        d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    d.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    d.NodeMask = 0;
    hr = ID3D12Device_CreateCommandQueue(INNER(p, ID3D12Device), &d, &IID_ID3D12CommandQueue, (void **)&queue);
    /* Every queue starts on the older layout; queue_Slot16/17 tell the newer one apart. */
    return wrap_result(hr, (IUnknown *)queue, &queue_class, out);
}

/* Command allocators: the console's has a tenth method, slot 9, with which the target title registers
 * an allocation callback (it gives a size and alignment and returns memory of the title's) right
 * after making one. vkd3d-proton's has nine, so the call went past its vtable to a null pointer.
 * Allocators are proxies now: slots 0-8 forwarded, 9 accepted (vkd3d-proton manages its own
 * command memory), and lists unwrap them. */
static HRESULT STDMETHODCALLTYPE allocator_SetAllocationCallbackX(struct proxy *p, void *callback, void *context)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: command allocator slot 9: the title's allocation callback (%p) is accepted, not called",
                  callback);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_CreateCommandAllocator(struct proxy *p, D3D12_COMMAND_LIST_TYPE type,
        REFIID riid, void **out)
{
    ID3D12CommandAllocator *alloc = NULL;
    HRESULT hr = ID3D12Device_CreateCommandAllocator(INNER(p, ID3D12Device), pc_list_type(type),
                                                     &IID_ID3D12CommandAllocator, (void **)&alloc);
    return wrap_result(hr, (IUnknown *)alloc, &allocator_class, out);
}

/* Slot 36: fences are proxies for slot 12 (fence_CompletedValueAddressX). The console's SHARED flag
 * is dropped: the desktop's shared fences are cross-process ones (vkd3d-proton makes them of
 * exportable semaphores), which nothing here needs. */
static HRESULT STDMETHODCALLTYPE device_CreateFence(struct proxy *p, UINT64 initial, D3D12_FENCE_FLAGS flags,
        REFIID riid, void **out)
{
    ID3D12Fence *fence = NULL;
    HRESULT hr;

    if (flags & D3D12_FENCE_FLAG_SHARED)
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: CreateFence flags %#x: made without D3D12_FENCE_FLAG_SHARED", flags);
        flags &= ~D3D12_FENCE_FLAG_SHARED;
    }
    hr = ID3D12Device_CreateFence(INNER(p, ID3D12Device), initial, flags, &IID_ID3D12Fence, (void **)&fence);
    return wrap_result(hr, (IUnknown *)fence, &fence_class, out);
}

/* Fence slot 12 (the newer layout; xcl, crates/xcl/src/hle/d3d12x/fence_x.rs): the address of the
 * fence's completed value, which the title reads and sleeps on. The console GPU writes that
 * memory; here each fence asked gets a cache line of its own that a thread keeps equal to
 * GetCompletedValue: a queue's Signal of such a fence arms an event for its value, and a CPU
 * Signal refreshes at once. Fences with a cell are kept (a reference each). */
struct fence_cell
{
    volatile UINT64 value;
    BYTE pad[56];
};

static struct proxy *fence_cells[256];
static unsigned fence_cell_count;
static SRWLOCK fence_cells_lock = SRWLOCK_INIT;
static HANDLE fence_event;

static void fence_refresh(void)
{
    AcquireSRWLockShared(&fence_cells_lock);
    for (unsigned i = 0; i < fence_cell_count; i++)
        fence_cells[i]->fence_cell->value = ID3D12Fence_GetCompletedValue(INNER(fence_cells[i], ID3D12Fence));
    ReleaseSRWLockShared(&fence_cells_lock);
}

static DWORD WINAPI fence_thread(void *arg)
{
    for (;;)
    {
        WaitForSingleObject(fence_event, INFINITE);
        fence_refresh();
    }
    return 0;
}

static BOOL CALLBACK fence_thread_init(INIT_ONCE *once, void *param, void **ctx)
{
    HANDLE thread;

    if (!(fence_event = CreateEventA(NULL, FALSE, FALSE, NULL)))
        return FALSE;
    if ((thread = CreateThread(NULL, 0, fence_thread, NULL, 0, NULL)))
        CloseHandle(thread);
    return TRUE;
}

/* A queue was told to signal fence (as the title passed it) with value. */
static void fence_note_signal(void *fence, UINT64 value)
{
    struct proxy *f = fence;
    LONG64 old;

    if (!is_proxy(f) || f->cls != &fence_class)
        return;
    while ((old = f->fence_signalled) < (LONG64)value
            && InterlockedCompareExchange64(&f->fence_signalled, value, old) != old)
        ;
    if (f->fence_cell)
        ID3D12Fence_SetEventOnCompletion(INNER(f, ID3D12Fence), value, fence_event);
}

static UINT64 *STDMETHODCALLTYPE fence_CompletedValueAddressX(struct proxy *p)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    struct fence_cell *cell;
    BOOL armed = FALSE;

    InitOnceExecuteOnce(&once, fence_thread_init, NULL, NULL);
    AcquireSRWLockExclusive(&fence_cells_lock);
    if (!(cell = p->fence_cell) && fence_cell_count < ARRAYSIZE(fence_cells)
            && (cell = _aligned_malloc(sizeof(*cell), 64)))
    {
        memset(cell, 0, sizeof(*cell));
        InterlockedIncrement(&p->refs);
        fence_cells[fence_cell_count++] = p;
        p->fence_cell = cell;
        armed = TRUE;
        if (fence_cell_count == 1)
            rebox_log("d3d12_xs: fence slot 12: completed values in memory, kept by a thread");
        if (fence_cell_count <= 32)
            rebox_log("d3d12_xs: fence %p's completed value is at %p", (void *)p, (void *)&cell->value);
    }
    ReleaseSRWLockExclusive(&fence_cells_lock);
    if (!cell)
    {
        static struct fence_cell overflow;
        rebox_log("d3d12_xs: fence slot 12: no room for another fence's completed value");
        overflow.value = ID3D12Fence_GetCompletedValue(INNER(p, ID3D12Fence));
        return (UINT64 *)&overflow.value;
    }
    if (armed && p->fence_signalled && fence_event)
        ID3D12Fence_SetEventOnCompletion(INNER(p, ID3D12Fence), p->fence_signalled, fence_event);
    fence_refresh();
    return (UINT64 *)&cell->value;
}

/* Slot 10: a CPU Signal, which the completed values follow at once. */
static HRESULT STDMETHODCALLTYPE fence_Signal(struct proxy *p, UINT64 value)
{
    HRESULT hr = ID3D12Fence_Signal(INNER(p, ID3D12Fence), value);
    if (p->fence_cell)
        fence_refresh();
    return hr;
}

static HRESULT create_list(struct proxy *p, UINT type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso,
        void **out)
{
    D3D12_COMMAND_LIST_TYPE t = pc_list_type(type);
    ID3D12GraphicsCommandList *list = NULL;

    HRESULT hr = ID3D12Device_CreateCommandList(INNER(p, ID3D12Device), 0, t, proxy_unwrap(alloc), pso,
                                                &IID_ID3D12GraphicsCommandList, (void **)&list);

    hr = wrap_result(hr, (IUnknown *)list, t == D3D12_COMMAND_LIST_TYPE_COPY ? &copy_list_class
                     : newer_layouts ? &list_v4_class : &list_class, out);
    if (SUCCEEDED(hr) && out && *out && ((struct proxy *)*out)->cls == &list_class)
        older_list_add(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateCommandList(struct proxy *p, UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
        ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **out)
{
    return create_list(p, type, alloc, pso, out);
}

/* Slot 73 (ScarlettX): (desc whose first dword is the type, allocator, initial PSO, riid, out). */
static HRESULT STDMETHODCALLTYPE device_CreateCommandListX(struct proxy *p, const UINT *desc,
        ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **out)
{
    if (!desc)
        return E_INVALIDARG;
    return create_list(p, desc[0], alloc, pso, out);
}

static HRESULT STDMETHODCALLTYPE device_CreateDescriptorHeap(struct proxy *p, const D3D12_DESCRIPTOR_HEAP_DESC *desc,
        REFIID riid, void **out)
{
    ID3D12DescriptorHeap *heap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC d;
    UINT reserved = 0;
    HRESULT hr;

    if (!desc)
        return E_INVALIDARG;
    /* Shader-visible heaps get a hidden tail for descriptors the host makes (shadow.h). An
     * unpatched vkd3d-proton refuses a full sampler heap plus the tail: then go without. */
    d = *desc;
    if ((d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
            && (d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER))
        reserved = SHADOW_RESERVED;
    d.NumDescriptors += reserved;
    hr = ID3D12Device_CreateDescriptorHeap(INNER(p, ID3D12Device), &d, &IID_ID3D12DescriptorHeap, (void **)&heap);
    if (FAILED(hr) && reserved)
    {
        reserved = 0;
        hr = ID3D12Device_CreateDescriptorHeap(INNER(p, ID3D12Device), desc, &IID_ID3D12DescriptorHeap,
                                               (void **)&heap);
    }
    if (SUCCEEDED(hr))
    {
        UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(INNER(p, ID3D12Device), desc->Type);
        shadow_heap_created(heap, desc, increment, reserved);
        shadow_place_heap(heap, desc->NumDescriptors, increment);
    }
    return wrap_result(hr, (IUnknown *)heap, &heap_class, out);
}

/* Descriptor writes (slots 17-19, 22-24, as on PC) also write the console
 * format into the shadow heaps (shadow.c). */
static void STDMETHODCALLTYPE device_CreateConstantBufferView(struct proxy *p,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu = shadow_cpu_to_inner(cpu) };
    ID3D12Device_CreateConstantBufferView(INNER(p, ID3D12Device), desc, h);
    shadow_write_cbv(cpu, desc);
}

/* Render target views of display-sized RGBA8 textures (what PresentX shows), and what lists do with
 * them, counted for the log at each of the first presents (proxy_frame_presented). */
static volatile SIZE_T display_rtvs[8];
static volatile LONG display_binds[8], display_clears[8], display_draws[8];

/* Which of display_rtvs this handle is, or -1. */
static int display_rtv(SIZE_T cpu)
{
    for (UINT i = 0; i < ARRAYSIZE(display_rtvs); i++)
        if (cpu && display_rtvs[i] == cpu)
            return i;
    return -1;
}

/* Views of a display-sized texture (what PresentX shows), for finding how the title draws into it. */
static void note_display_view(const char *what, ID3D12Resource *resource, SIZE_T cpu)
{
    static LONG logged;
    D3D12_RESOURCE_DESC d;

    if (!resource)
        return;
    /* GetDesc (slot 10) returns its struct through a hidden pointer. */
    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)resource)[10])(resource, &d);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= 1920 && d.Height >= 1080
            && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
            && !strcmp(what, "render target view"))
        for (UINT i = 0; i < ARRAYSIZE(display_rtvs); i++)
            if (!display_rtvs[i] || display_rtvs[i] == cpu)
            {
                display_rtvs[i] = cpu;
                break;
            }
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= 1920 && d.Height >= 1080
            && InterlockedIncrement(&logged) <= 24)
        rebox_log("d3d12_xs: %s of %p (%llux%u, format %u, flags %#x) at handle %#llx", what, (void *)resource,
                  (unsigned long long)d.Width, d.Height, d.Format, (UINT)d.Flags, (unsigned long long)cpu);
}

static D3D12_RESOURCE_DESC resource_desc(ID3D12Resource *resource)
{
    D3D12_RESOURCE_DESC d;
    /* GetDesc (slot 10) returns its struct through a hidden pointer. */
    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)resource)[10])(resource, &d);
    return d;
}

/* A console shader view of a depth buffer names its plane by a colour format: R16_UNORM for a
 * D16S8 buffer's depth, R8_UINT for its stencil, R32_FLOAT for a D32S8's depth. The desktop has
 * those buffers as D24S8 (pc_format) and D32S8, whose planes' view formats are their own, the
 * stencil's in plane 1 (after xcl, Robin Snyders, ivall/xcl#163): what format a view of plane
 * `format` of a buffer of `resource` format takes, and which plane; FALSE if it isn't one. */
static BOOL depth_plane_view(DXGI_FORMAT resource, DXGI_FORMAT format, DXGI_FORMAT *view, UINT *plane)
{
    BOOL d24 = resource == DXGI_FORMAT_R24G8_TYPELESS || resource == DXGI_FORMAT_D24_UNORM_S8_UINT;
    BOOL d32 = resource == DXGI_FORMAT_R32G8X24_TYPELESS || resource == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    BOOL stencil = format == DXGI_FORMAT_R8_UINT || format == DXGI_FORMAT_R8_TYPELESS || format == 120
            || format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    BOOL depth = d24 ? format == DXGI_FORMAT_R16_UNORM || format == DXGI_FORMAT_R16_TYPELESS
                       || format == DXGI_FORMAT_D16_UNORM || format == 119 || format == 118
                       || format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                 : format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_D32_FLOAT
                       || format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

    if (!(d24 || d32) || !(depth || stencil))
        return FALSE;
    *plane = stencil;
    *view = d24 ? (stencil ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
                : (stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    return TRUE;
}

static void STDMETHODCALLTYPE device_CreateShaderResourceView(struct proxy *p, ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu = shadow_cpu_to_inner(cpu) };
    D3D12_SHADER_RESOURCE_VIEW_DESC pc;
    DXGI_FORMAT view;
    UINT plane;
    if (desc && pc_format(desc->Format) != desc->Format)
    {
        pc = *desc;
        pc.Format = pc_format(desc->Format);
        desc = &pc;
    }
    if (desc && resource && depth_plane_view(resource_desc(resource).Format, desc->Format, &view, &plane)
            && view != desc->Format)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: a shader view of a depth buffer as format %u is format %u, plane %u", desc->Format,
                      view, plane);
        pc = *desc;
        pc.Format = view;
        if (pc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D)
            pc.Texture2D.PlaneSlice = plane;
        else if (pc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY)
            pc.Texture2DArray.PlaneSlice = plane;
        desc = &pc;
    }
    if (resource && resource_desc(resource).Format == DXGI_FORMAT_A8_UNORM)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("d3d12_xs: a shader view of an A8 texture: %s, format %u, dimension %u, mapping %#x",
                      desc ? "described" : "default", desc ? desc->Format : 0, desc ? desc->ViewDimension : 0,
                      desc ? desc->Shader4ComponentMapping : 0);
    }
    ID3D12Device_CreateShaderResourceView(INNER(p, ID3D12Device), resource, desc, h);
    shadow_write_srv(cpu, resource, desc);
}

static void STDMETHODCALLTYPE device_CreateUnorderedAccessView(struct proxy *p, ID3D12Resource *resource,
        ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu = shadow_cpu_to_inner(cpu) };
    note_display_view("unordered access view", resource, cpu);
    /* A resource the desktop has without unordered access (a depth buffer that is a UAV on the
     * console, pc_desc) gets a null view of the same dimension: the driver would be handed an
     * invalid one (after xcl, ivall/xcl#170). Writes through it are lost. */
    if (resource)
    {
        D3D12_RESOURCE_DESC d = resource_desc(resource);
        if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            static LONG logged;
            D3D12_UNORDERED_ACCESS_VIEW_DESC null = { DXGI_FORMAT_R32_UINT };
            null.ViewDimension = desc ? desc->ViewDimension
                    : d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_UAV_DIMENSION_TEXTURE3D
                    : d.DepthOrArraySize > 1 ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
            if (null.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
                null.Texture2DArray.ArraySize = 1;
            if (null.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D)
                null.Texture3D.WSize = 1;
            if (InterlockedIncrement(&logged) <= 4)
                rebox_log("d3d12_xs: an unordered access view of a %llux%u texture (format %u) without unordered "
                          "access here: a null view", (unsigned long long)d.Width, d.Height, d.Format);
            ID3D12Device_CreateUnorderedAccessView(INNER(p, ID3D12Device), NULL, NULL, &null, h);
            shadow_write_uav(cpu, resource, desc);
            return;
        }
    }
    ID3D12Device_CreateUnorderedAccessView(INNER(p, ID3D12Device), resource, counter, desc, h);
    shadow_write_uav(cpu, resource, desc);
}

/* Slot 21: CreateDepthStencilView, as on PC, with the console's own formats made desktop ones. */
static void STDMETHODCALLTYPE device_CreateDepthStencilView(struct proxy *p, ID3D12Resource *resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    D3D12_DEPTH_STENCIL_VIEW_DESC pc;
    if (desc && pc_format(desc->Format) != desc->Format)
    {
        pc = *desc;
        pc.Format = pc_format(desc->Format);
        desc = &pc;
    }
    ID3D12Device_CreateDepthStencilView(INNER(p, ID3D12Device), resource, desc, h);
}

/* Slot 20: CreateRenderTargetView, as on PC; views of display-sized textures are logged. */
/* Every render target view's texture size and view format by handle (open addressing; a later view
 * at the same handle replaces it), so a watched stage's draws can name their target. */
static struct { volatile SIZE_T cpu; UINT width, height, format; } rtv_info[4096];

static UINT rtv_slot(SIZE_T cpu)
{
    return (UINT)((cpu >> 5) * 2654435761u) % ARRAYSIZE(rtv_info);
}

static void note_rtv(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_RESOURCE_DESC d;
    if (!resource || !cpu)
        return;
    d = resource_desc(resource);
    for (UINT n = 0, i = rtv_slot(cpu); n < ARRAYSIZE(rtv_info); n++, i = (i + 1) % ARRAYSIZE(rtv_info))
    {
        SIZE_T c = rtv_info[i].cpu;
        if (c == cpu || (!c && !InterlockedCompareExchangePointer((void *volatile *)&rtv_info[i].cpu, (void *)cpu, NULL)))
        {
            rtv_info[i].width = (UINT)d.Width;
            rtv_info[i].height = d.Height;
            rtv_info[i].format = desc ? desc->Format : d.Format;
            return;
        }
    }
}

static BOOL find_rtv(SIZE_T cpu, UINT *width, UINT *height, UINT *format)
{
    for (UINT n = 0, i = rtv_slot(cpu); cpu && n < ARRAYSIZE(rtv_info) && rtv_info[i].cpu; n++, i = (i + 1) % ARRAYSIZE(rtv_info))
        if (rtv_info[i].cpu == cpu)
        {
            *width = rtv_info[i].width;
            *height = rtv_info[i].height;
            *format = rtv_info[i].format;
            return TRUE;
        }
    return FALSE;
}

static void STDMETHODCALLTYPE device_CreateRenderTargetView(struct proxy *p, ID3D12Resource *resource,
        const D3D12_RENDER_TARGET_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    D3D12_RENDER_TARGET_VIEW_DESC pc;
    if (desc && pc_format(desc->Format) != desc->Format)
    {
        pc = *desc;
        pc.Format = pc_format(desc->Format);
        desc = &pc;
    }
    note_display_view("render target view", resource, cpu);
    note_rtv(resource, desc, cpu);
    ID3D12Device_CreateRenderTargetView(INNER(p, ID3D12Device), resource, desc, h);
}

static void STDMETHODCALLTYPE device_CreateSampler(struct proxy *p, const D3D12_SAMPLER_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu = shadow_cpu_to_inner(cpu) };
    ID3D12Device_CreateSampler(INNER(p, ID3D12Device), desc, h);
    shadow_write_sampler(cpu, desc);
}

static void STDMETHODCALLTYPE device_CopyDescriptors(struct proxy *p, UINT dst_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes, UINT src_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    /* Handles in heaps placed in title memory become vkd3d-proton's (shadow_place_heap). */
    D3D12_CPU_DESCRIPTOR_HANDLE dst_local[64], src_local[64], *dst = NULL, *src = NULL;
    if (shadow_title_heaps_placed() && dst_starts && src_starts)
    {
        dst = dst_count <= ARRAYSIZE(dst_local) ? dst_local : malloc(dst_count * sizeof(*dst));
        src = src_count <= ARRAYSIZE(src_local) ? src_local : malloc(src_count * sizeof(*src));
        if (dst && src)
        {
            for (UINT i = 0; i < dst_count; i++)
                dst[i].ptr = shadow_cpu_to_inner(dst_starts[i].ptr);
            for (UINT i = 0; i < src_count; i++)
                src[i].ptr = shadow_cpu_to_inner(src_starts[i].ptr);
            dst_starts = dst;
            src_starts = src;
        }
    }
    ID3D12Device_CopyDescriptors(INNER(p, ID3D12Device), dst_count, dst_starts, dst_sizes, src_count, src_starts,
                                 src_sizes, type);
    shadow_copy_ranges(dst_count, dst_starts, dst_sizes, src_count, src_starts, src_sizes);
    if (dst != dst_local) free(dst);
    if (src != src_local) free(src);
}

static void STDMETHODCALLTYPE device_CopyDescriptorsSimple(struct proxy *p, UINT count, SIZE_T dst, SIZE_T src,
        D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12_CPU_DESCRIPTOR_HANDLE d = { dst = shadow_cpu_to_inner(dst) }, s = { src = shadow_cpu_to_inner(src) };
    ID3D12Device_CopyDescriptorsSimple(INNER(p, ID3D12Device), count, d, s, type);
    shadow_copy(dst, src, count);
}

/* Command signatures the title creates (slot 41, as on PC), so list slot 61 can tell
 * ExecuteIndirect from BeginEvent. */
static SRWLOCK signatures_lock = SRWLOCK_INIT;
static void **signatures;
static UINT signature_count, signature_capacity;

/* Signatures that dispatch rays: ExecuteIndirect skips them while a placeholder ray tracing
 * pipeline is set (vkd3d-proton would read a pipeline it was never given). */
static void *ray_signatures[64];
static LONG ray_signature_count;

static BOOL is_ray_signature(void *object)
{
    for (LONG i = 0; i < ray_signature_count && i < (LONG)ARRAYSIZE(ray_signatures); i++)
        if (ray_signatures[i] == object)
            return TRUE;
    return FALSE;
}

static BOOL is_command_signature(void *object)
{
    BOOL found = FALSE;
    AcquireSRWLockShared(&signatures_lock);
    for (UINT i = 0; i < signature_count && !found; i++)
        found = signatures[i] == object;
    ReleaseSRWLockShared(&signatures_lock);
    return found;
}

/* The console's own indirect argument types: 104 and 105 (DISPATCHX, DISPATCH_L2) are dispatches
 * with the desktop's three group counts, 107 a mesh dispatch (16-byte records). Facts from
 * xvalkyrie and TuxBox; Unreal 5's D3D12 RHI on the console (a UE5 sample) makes its
 * dispatch-indirect signature with 105, which vkd3d-proton rejects. Returns the desktop type. */
static D3D12_INDIRECT_ARGUMENT_TYPE desktop_argument_type(D3D12_INDIRECT_ARGUMENT_TYPE type)
{
    static LONG seen[8];
    D3D12_INDIRECT_ARGUMENT_TYPE desktop;
    switch ((UINT)type)
    {
    case 104: case 105: desktop = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH; break;
    case 107: desktop = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH; break;
    default: return type;
    }
    if (!InterlockedExchange(&seen[(UINT)type - 100], 1))
        rebox_log("d3d12_xs: command signature argument type %u (the console's) taken as desktop type %u",
                     (UINT)type, (UINT)desktop);
    return desktop;
}

/* Slot 115: ID3D12Device8::DeserializeStateObjectX(blob, length, root signature, flags, riid, out),
 * a ray tracing state object from console code (state_object.c). */
static HRESULT STDMETHODCALLTYPE device_DeserializeStateObjectX(struct proxy *p, const void *code, SIZE_T length,
        void *root_signature, UINT flags, REFIID riid, void **out)
{
    return state_object_deserialize((IUnknown *)p, code, length, root_signature, flags, riid, out);
}

/* Slot 101: CreateStateObject (desktop 62), a ray tracing pipeline from collections (state_object.c). */
static HRESULT STDMETHODCALLTYPE device_CreateStateObject(struct proxy *p, const D3D12_STATE_OBJECT_DESC *desc, REFIID riid,
        void **out)
{
    return state_object_create((IUnknown *)p, desc, riid, out);
}

static HRESULT STDMETHODCALLTYPE device_CreateCommandSignature(struct proxy *p, const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *rs, REFIID riid, void **out)
{
    D3D12_INDIRECT_ARGUMENT_DESC local[16], *args = NULL;
    D3D12_COMMAND_SIGNATURE_DESC host;
    HRESULT hr;

    if (desc && desc->pArgumentDescs && desc->NumArgumentDescs)
    {
        args = desc->NumArgumentDescs <= ARRAYSIZE(local) ? local : malloc(desc->NumArgumentDescs * sizeof(*args));
        if (args)
        {
            memcpy(args, desc->pArgumentDescs, desc->NumArgumentDescs * sizeof(*args));
            for (UINT i = 0; i < desc->NumArgumentDescs; i++)
                args[i].Type = desktop_argument_type(args[i].Type);
            host = *desc;
            host.pArgumentDescs = args;
            desc = &host;
        }
    }
    /* A root signature only for arguments that change root arguments (constants, views): the
     * console takes one regardless, the desktop refuses it (Unreal 5 passes its global root for
     * the 104-byte dispatch-rays signature; TuxBox found the same). */
    if (rs && desc && desc->pArgumentDescs)
    {
        BOOL changes_root = FALSE;
        for (UINT i = 0; i < desc->NumArgumentDescs; i++)
            changes_root |= desc->pArgumentDescs[i].Type >= D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT
                    && desc->pArgumentDescs[i].Type <= D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
        if (!changes_root)
            rs = NULL;
    }
    hr = ID3D12Device_CreateCommandSignature(INNER(p, ID3D12Device), desc, rs, riid, out);
    if (FAILED(hr) && desc)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
        {
            char types[128];
            int at = 0;
            types[0] = 0;
            for (UINT i = 0; i < desc->NumArgumentDescs && desc->pArgumentDescs && at < 100; i++)
                at += snprintf(types + at, sizeof(types) - at, " %u", (UINT)desc->pArgumentDescs[i].Type);
            rebox_log("d3d12_xs: CreateCommandSignature (stride %u, argument types%s, root signature %p) failed: %#lx",
                         desc->ByteStride, types, (void *)rs, (unsigned long)hr);
        }
    }
    if (args && args != local)
        free(args);

    if (SUCCEEDED(hr) && out && *out)
    {
        AcquireSRWLockExclusive(&signatures_lock);
        if (signature_count == signature_capacity)
        {
            UINT capacity = signature_capacity ? signature_capacity * 2 : 64;
            void **grown = realloc(signatures, capacity * sizeof(*signatures));
            if (grown)
            {
                signatures = grown;
                signature_capacity = capacity;
            }
        }
        if (signature_count < signature_capacity)
            signatures[signature_count++] = *out;
        for (UINT i = 0; desc && desc->pArgumentDescs && i < desc->NumArgumentDescs; i++)
            if (desc->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS
                    && ray_signature_count < (LONG)ARRAYSIZE(ray_signatures))
            {
                ray_signatures[ray_signature_count++] = *out;
                break;
            }
        ReleaseSRWLockExclusive(&signatures_lock);
    }
    return hr;
}

/* Vertex and index buffers are bound by GPU address. The title's addresses are its own memory
 * (identity mapping), but vkd3d-proton only knows memory a heap or resource covers, and binds
 * nothing for an address it doesn't know (its "VBO VA that does not exist"): so a buffer's range
 * that isn't GPU memory yet is made GPU memory in place (gpu_memory_adopt) when it's bound, as
 * CreatePlacedResourceX does. Only addresses that can be the title's are: between 4 GiB and 2^46
 * (where xmem keeps its memory) and outside the GPU window, whose addresses are vkd3d-proton's own
 * resources. Ranges that can't be are logged, the first few. */
static void ia_adopt(const char *what, D3D12_GPU_VIRTUAL_ADDRESS address, UINT size)
{
    static LONG logged;
    UINT64 end;

    if (!size || address < 0x100000000ull || address + size > 0x400000000000ull
            || (address + size > 0x200000000000ull && address < 0x210000000000ull)
            || (gpu_memory_identity_extent(address, &end) && end >= address + size))
        return;
    if (!gpu_memory_adopt(address, size) && InterlockedIncrement(&logged) <= 8)
        rebox_log("d3d12_xs: %s at %#llx+%#x is not GPU memory and could not be made so: the GPU reads nothing there",
                  what, (unsigned long long)address, size);
}

/* Slot 47: IASetIndexBuffer (PC 43). */
static void STDMETHODCALLTYPE list_IASetIndexBuffer(struct proxy *p, const D3D12_INDEX_BUFFER_VIEW *view)
{
    if (view)
        ia_adopt("an index buffer", view->BufferLocation, view->SizeInBytes);
    if (view)
        p->index_buffer = *view;
    else
        memset(&p->index_buffer, 0, sizeof(p->index_buffer));
    ID3D12GraphicsCommandList_IASetIndexBuffer(INNER(p, ID3D12GraphicsCommandList), view);
}

/* Slot 48: IASetVertexBuffers (PC 44). */
static void STDMETHODCALLTYPE list_IASetVertexBuffers(struct proxy *p, UINT start, UINT count,
        const D3D12_VERTEX_BUFFER_VIEW *views)
{
    for (UINT i = 0; views && i < count; i++)
        ia_adopt("a vertex buffer", views[i].BufferLocation, views[i].SizeInBytes);
    for (UINT i = 0; i < count && start + i < ARRAYSIZE(p->vertex_buffers); i++)
        if (views && views[i].BufferLocation)
        {
            p->vertex_buffers[start + i] = views[i];
            p->vertex_buffer_mask |= 1u << (start + i);
        }
        else
            p->vertex_buffer_mask &= ~(1u << (start + i));
    ID3D12GraphicsCommandList_IASetVertexBuffers(INNER(p, ID3D12GraphicsCommandList), start, count, views);
}

/* REBOX_RECT_LISTS=cpu: rect lists made two triangles a rectangle on the CPU (list_DrawInstanced)
 * instead of by AnyPS5's rect-list stages. Read at each use, so a test can switch it. */
static BOOL rect_lists_on_cpu(void)
{
    char value[8];
    DWORD n = GetEnvironmentVariableA("REBOX_RECT_LISTS", value, sizeof(value));
    return n && n < sizeof(value) && !_stricmp(value, "cpu");
}

/* Slot 20: IASetPrimitiveTopology. The console's topology 17 is a rectangle list: three corners of a
 * screen-aligned rectangle per primitive, the fourth made by the hardware. Unreal 5 draws its
 * full-screen passes so (xcl; a UE5 sample), and stock vkd3d-proton drops a draw with a topology it
 * doesn't know (its image stayed black). Ours (patch 0011) takes it: deferred console graphics draw
 * it with AnyPS5's rect-list stages, which make the fourth corner after the vertex shader; other
 * pipelines draw it as a triangle list, each rectangle's first triangle. With REBOX_RECT_LISTS=cpu
 * it's a triangle list here, and list_DrawInstanced makes each rectangle two triangles where its
 * corners come from vertex buffers. */
static void STDMETHODCALLTYPE list_IASetPrimitiveTopology(struct proxy *p, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    p->rect_list = topology == 17;
    p->topology = topology;
    if (topology == 17 && rect_lists_on_cpu())
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: the console's rectangle lists (topology 17) are drawn as triangle lists, two a "
                      "rectangle where its corners come from vertex buffers (REBOX_RECT_LISTS=cpu)");
        topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(INNER(p, ID3D12GraphicsCommandList), topology);
}

/* ExecuteIndirect (PC 59): rays with no real pipeline set are skipped (see ray_signatures). */
static void STDMETHODCALLTYPE list_ExecuteIndirect(struct proxy *p, void *signature, UINT count, void *args, UINT64 offset,
        void *count_buffer, UINT64 count_offset)
{
    if (!signature || !is_command_signature(signature))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: command-list slot 59 called without a command signature (%p, %u, %p): ignored",
                         signature, count, args);
        return;
    }
    if (!p->ray_pipeline && is_ray_signature(signature))
    {
        static LONG skipped;
        if (!InterlockedExchange(&skipped, 1))
            rebox_log("d3d12_xs: ExecuteIndirect of rays without a real ray tracing pipeline: skipped");
        return;
    }
    ((void (STDMETHODCALLTYPE *)(void *, void *, UINT, void *, UINT64, void *, UINT64))PC_SLOT(p->inner, 59))(
            p->inner, signature, count, args, offset, count_buffer, count_offset);
}

/* List slot 61. The two measurements disagree: Leonida (the retail FSR DLL) calls it with
 * ExecuteIndirect's arguments (PC 59), ScarlettX has the PIX trio SetMarker, BeginEvent, EndEvent
 * at 60-62 (PC + 4: BeginEvent, PC 57). ExecuteIndirect's first argument is a command signature,
 * BeginEvent's a small metadata word: dispatch on that. */
static void STDMETHODCALLTYPE list_Slot61(struct proxy *p, void *a, UINT_PTR b, void *c, UINT64 d, void *e, UINT64 f)
{
    static LONG logged;

    if (a && is_command_signature(a))
    {
        if (!p->ray_pipeline && is_ray_signature(a))
        {
            static LONG skipped;
            if (!InterlockedExchange(&skipped, 1))
                rebox_log("d3d12_xs: ExecuteIndirect of rays without a real ray tracing pipeline: skipped");
            return;
        }
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: command-list slot 61 called as ExecuteIndirect (first seen)");
        ((void (STDMETHODCALLTYPE *)(void *, void *, UINT, void *, UINT64, void *, UINT64))PC_SLOT(p->inner, 59))(
                p->inner, a, (UINT)b, c, d, e, f);
        return;
    }
    ((void (STDMETHODCALLTYPE *)(void *, UINT_PTR, void *, UINT_PTR))PC_SLOT(p->inner, 57))(p->inner, (UINT_PTR)a,
            (void *)b, (UINT_PTR)c);
}

/* Slot 43. An 8-byte struct: the Xbox ABI returns those in RAX (see the
 * descriptor heap), while vkd3d-proton uses the MSVC hidden pointer. */
static LUID STDMETHODCALLTYPE device_GetAdapterLuid(struct proxy *p)
{
    LUID luid;
    ((LUID *(STDMETHODCALLTYPE *)(void *, LUID *))PC_SLOT(p->inner, 43))(p->inner, &luid);
    return luid;
}

/* On the console every buffer is CPU-visible at its GPU VA. Here buffers
 * in DEFAULT heaps, and DEFAULT heaps that hold only buffers, are created
 * in CPU-visible custom heaps (WRITE_BACK, L0) instead, which vkd3d-proton
 * keeps mapped and VK_LAYER_REBOX_gpuva_identity places at their GPU VA.
 * Mixed heaps and textures are unchanged. REBOX_CPU_VISIBLE_BUFFERS=0
 * turns it off. */
static BOOL cpu_visible_buffers(void)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        const char *env = getenv("REBOX_CPU_VISIBLE_BUFFERS");
        enabled = !env || *env != '0';
    }
    return enabled;
}

static const D3D12_HEAP_PROPERTIES *buffer_heap(const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_PROPERTIES *storage)
{
    if (!props || props->Type != D3D12_HEAP_TYPE_DEFAULT || !cpu_visible_buffers())
        return props;
    *storage = *props;
    storage->Type = D3D12_HEAP_TYPE_CUSTOM;
    storage->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    storage->MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return storage;
}

/* Xbox resource descs can carry console texture layouts (>= 0x100). The
 * FidelityFX upscaler's recurrent texture uses 0x101 (Xbox2Windows's
 * docs/experiments.md). They select console tilings vkd3d-proton doesn't
 * know, so they become UNKNOWN, the driver's choice; titles that address
 * texels themselves go through scratch GPU VAs instead. Each distinct
 * value is logged once. */
/* The console's own formats (116-120 in its dxgiformat.h), as the nearest desktop ones: the desktop
 * refuses them (the target title asserts on a D16_UNORM_S8_UINT depth texture's size). */
static DXGI_FORMAT pc_format(DXGI_FORMAT format)
{
    switch ((UINT)format)
    {
    case 116: /* R10G10B10_7E3_A2_FLOAT */
    case 117: /* R10G10B10_6E4_A2_FLOAT */
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 118: return DXGI_FORMAT_D24_UNORM_S8_UINT;     /* D16_UNORM_S8_UINT */
    case 119: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS; /* R16_UNORM_X8_TYPELESS */
    case 120: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;  /* X16_TYPELESS_G8_UINT */
    default: return format;
    }
}

/* A clear value in a console-only format, in the desktop one its resource gets. */
static const D3D12_CLEAR_VALUE *pc_clear(const D3D12_CLEAR_VALUE *clear, D3D12_CLEAR_VALUE *storage)
{
    if (!clear || pc_format(clear->Format) == clear->Format)
        return clear;
    *storage = *clear;
    storage->Format = pc_format(clear->Format);
    return storage;
}

static const D3D12_RESOURCE_DESC *pc_desc(const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_DESC *storage)
{
    static LONG seen[8], depth_uav, formats;
    const D3D12_RESOURCE_FLAGS both = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (!desc || (desc->Layout < 0x100 && (desc->Flags & both) != both && pc_format(desc->Format) == desc->Format))
        return desc;
    *storage = *desc;
    if (pc_format(desc->Format) != desc->Format)
    {
        if (InterlockedIncrement(&formats) <= 4)
            rebox_log("d3d12_xs: console format %u (%ux%u) made desktop format %u", desc->Format, (UINT)desc->Width,
                      desc->Height, pc_format(desc->Format));
        storage->Format = pc_format(desc->Format);
    }
    if (desc->Layout >= 0x100)
    {
        if (!InterlockedExchange(&seen[desc->Layout & 7], 1))
            rebox_log("d3d12_xs: Xbox texture layout %#x (%ux%u, format %u) treated as UNKNOWN", desc->Layout,
                         (UINT)desc->Width, desc->Height, desc->Format);
        storage->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    /* A console depth texture can also be a UAV of its float plane (Unreal 5's Nanite: R32G8X24
     * with depth-stencil, UAV and console flag 0x1000000, a UE5 sample); the desktop refuses
     * the pair. It stays a depth target; its UAV views fail, so writes through them are lost
     * (TuxBox pairs it with an R32 image and copies the plane across instead: to do). */
    if ((desc->Flags & both) == both)
    {
        if (!InterlockedExchange(&depth_uav, 1))
            rebox_log("d3d12_xs: a depth texture with unordered access (%ux%u, format %u, flags %#x): a depth "
                         "target only (its UAV writes are lost)", (UINT)desc->Width, desc->Height, desc->Format,
                         (UINT)desc->Flags);
        storage->Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    return storage;
}

/* A console depth buffer that is also a UAV may start in the UAV state; the desktop has it without
 * unordered access (pc_desc), so it starts as a depth target (after xcl, ivall/xcl#163). */
static D3D12_RESOURCE_STATES pc_state(const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_FLAGS both = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (desc && (desc->Flags & both) == both && state != (D3D12_RESOURCE_STATES)0xffffffff
            && (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return state;
}

/* Slot 25, through the hidden return pointer as on PC (16-byte result). */
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE device_GetResourceAllocationInfo(struct proxy *p,
        D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC *descs)
{
    typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *fn)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT,
            UINT, const D3D12_RESOURCE_DESC *);
    D3D12_RESOURCE_DESC *copy = NULL;

    if (result_first(p, ret)) /* console order: (result, device, ...) */
    {
        struct proxy *device = (struct proxy *)ret;
        ret = (D3D12_RESOURCE_ALLOCATION_INFO *)p;
        p = device;
    }

    for (UINT i = 0; descs && i < count; i++)
    {
        D3D12_RESOURCE_DESC storage;
        if (pc_desc(&descs[i], &storage) != &descs[i])
        {
            if (!copy && (copy = malloc(count * sizeof(*copy))))
                memcpy(copy, descs, count * sizeof(*copy));
            if (copy)
                copy[i] = storage;
        }
    }
    ((fn)PC_SLOT(p->inner, 25))(p->inner, ret, visible_mask, count, copy ? copy : descs);
    if (ret->SizeInBytes == ~(UINT64)0 && descs)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            for (UINT i = 0; i < count; i++)
                rebox_log("d3d12_xs: GetResourceAllocationInfo refused (%u of %u): dimension %u, %llux%ux%u, "
                          "mips %u, format %u, samples %u, layout %#x, flags %#x, alignment %#llx", i, count,
                          descs[i].Dimension, (unsigned long long)descs[i].Width, descs[i].Height,
                          descs[i].DepthOrArraySize, descs[i].MipLevels, descs[i].Format, descs[i].SampleDesc.Count,
                          descs[i].Layout, (UINT)descs[i].Flags, (unsigned long long)descs[i].Alignment);
    }
    free(copy);
    return ret;
}

/* Slot 108 (newer layout, after xcl): GetResourceAllocationInfo2(visible mask, count,
 * D3D12_RESOURCE_DESC1s of 0x48 bytes, D3D12_RESOURCE_ALLOCATION_INFO1 for each or null), returned
 * as slot 25's (either argument order): their total size and largest alignment. */
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE device_GetResourceAllocationInfo2X(struct proxy *p,
        D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT visible_mask, UINT count, const BYTE *descs,
        D3D12_RESOURCE_ALLOCATION_INFO1 *infos)
{
    UINT64 total = 0, alignment = 0;

    note_newer_device_method(108);
    if (result_first(p, ret)) /* console order: (result, device, ...) */
    {
        struct proxy *device = (struct proxy *)ret;
        ret = (D3D12_RESOURCE_ALLOCATION_INFO *)p;
        p = device;
    }
    for (UINT k = 0; k < count && descs; k++)
    {
        D3D12_RESOURCE_ALLOCATION_INFO one = { 0 };
        D3D12_RESOURCE_DESC d;
        UINT64 offset;

        memcpy(&d, descs + (SIZE_T)k * 0x48, sizeof(d));
        device_GetResourceAllocationInfo(p, &one, visible_mask, 1, &d);
        if (one.SizeInBytes == ~(UINT64)0 || !one.Alignment)
        {
            ret->SizeInBytes = ~(UINT64)0;
            ret->Alignment = 0;
            return ret;
        }
        offset = (total + one.Alignment - 1) & ~(one.Alignment - 1);
        if (infos)
            infos[k] = (D3D12_RESOURCE_ALLOCATION_INFO1){ offset, one.Alignment, one.SizeInBytes };
        total = offset + one.SizeInBytes;
        alignment = max(alignment, one.Alignment);
    }
    ret->SizeInBytes = total;
    ret->Alignment = alignment;
    return ret;
}

/* Slot 26: GetCustomHeapProperties, either argument order (see resource_GetDesc). */
static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE device_GetCustomHeapProperties(struct proxy *p,
        D3D12_HEAP_PROPERTIES *ret, UINT node_mask, D3D12_HEAP_TYPE type)
{
    typedef D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *fn)(void *, D3D12_HEAP_PROPERTIES *, UINT, D3D12_HEAP_TYPE);

    if (result_first(p, ret))
    {
        struct proxy *device = (struct proxy *)ret;
        ret = (D3D12_HEAP_PROPERTIES *)p;
        p = device;
    }
    ((fn)PC_SLOT(p->inner, 26))(p->inner, ret, node_mask, type);
    return ret;
}

/* Slot 38. */
static void STDMETHODCALLTYPE device_GetCopyableFootprints(struct proxy *p, const D3D12_RESOURCE_DESC *desc,
        UINT first, UINT count, UINT64 base, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows,
        UINT64 *row_sizes, UINT64 *total)
{
    D3D12_RESOURCE_DESC storage;

    ID3D12Device_GetCopyableFootprints(INNER(p, ID3D12Device), pc_desc(desc, &storage), first, count, base, layouts,
                                       rows, row_sizes, total);
}

/* Resource creation (slots 27, 28, 29, 30): forwarded, recording the
 * initial state for PresentX's copy out of the plane. */
static HRESULT STDMETHODCALLTYPE device_CreateCommittedResource(struct proxy *p, const D3D12_HEAP_PROPERTIES *props,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12_HEAP_PROPERTIES storage;
    D3D12_RESOURCE_DESC desc_storage, rounded;
    D3D12_CLEAR_VALUE clear_storage;
    HRESULT hr;

    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        props = buffer_heap(props, &storage);
    state = pc_state(desc, state);
    desc = pc_desc(desc, &desc_storage);
    /* Console titles put 256-byte constant buffer views on smaller buffers (XValkyrie: 256 on
     * 80 bytes, which got the device removed on Windows). Committed buffers are rounded up to
     * 256 bytes so such a view stays inside the buffer. */
    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && (desc->Width & 0xff))
    {
        rounded = *desc;
        rounded.Width = (desc->Width + 0xff) & ~(UINT64)0xff;
        desc = &rounded;
    }
    hr = ID3D12Device_CreateCommittedResource(INNER(p, ID3D12Device), props, flags, desc, state,
                                              pc_clear(clear, &clear_storage), riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateHeap(struct proxy *p, const D3D12_HEAP_DESC *desc, REFIID riid,
        void **out)
{
    D3D12_HEAP_DESC d;

    if (!desc || (desc->Flags & D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) != D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS)
        return ID3D12Device_CreateHeap(INNER(p, ID3D12Device), desc, riid, out);
    d = *desc;
    d.Properties = *buffer_heap(&desc->Properties, &d.Properties);
    return ID3D12Device_CreateHeap(INNER(p, ID3D12Device), &d, riid, out);
}

static HRESULT STDMETHODCALLTYPE device_CreatePlacedResource(struct proxy *p, ID3D12Heap *heap, UINT64 offset,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid,
        void **out)
{
    D3D12_RESOURCE_DESC storage;
    D3D12_CLEAR_VALUE clear_storage;
    HRESULT hr;
    state = pc_state(desc, state);
    hr = ID3D12Device_CreatePlacedResource(INNER(p, ID3D12Device), heap, offset, pc_desc(desc, &storage),
                                           state, pc_clear(clear, &clear_storage), riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateReservedResource(struct proxy *p, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12_RESOURCE_DESC storage;
    D3D12_CLEAR_VALUE clear_storage;
    HRESULT hr = ID3D12Device_CreateReservedResource(INNER(p, ID3D12Device), pc_desc(desc, &storage), state,
                                                     pc_clear(clear, &clear_storage), riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

/* Deferred PSO destruction. On the console a PSO's shaders are the title's memory, so a title
 * releases PSOs that work in flight still uses; on the desktop that frees the Vulkan pipeline
 * under the GPU (the target title lost the device fetching a freed pipeline's code). Each PSO
 * made here keeps a reference of ours, marked by private data; when the title's last reference
 * goes (Release, patched in vkd3d-proton's PSO class, returns our 1), the PSO is queued and
 * released PSO_KEEP_FRAMES presents later. */
#define PSO_KEEP_FRAMES 8

static const GUID pso_kept_guid = { 0x53435247, 0x0002, 0x4000, { 0x80, 0x00, 0x6b, 0x65, 0x65, 0x70, 0x70, 0x73 } };
static ULONG (STDMETHODCALLTYPE *pso_orig_release)(ID3D12PipelineState *);
static SRWLOCK pso_lock = SRWLOCK_INIT;
static struct { ID3D12PipelineState *pso; LONG64 frame; } *pso_queue;
static size_t pso_queue_count, pso_queue_cap;
static volatile LONG64 frames_presented;

static ULONG STDMETHODCALLTYPE pso_Release(ID3D12PipelineState *pso)
{
    ULONG refs = pso_orig_release(pso);
    UINT size = 0;

    if (refs != 1 || FAILED(ID3D12PipelineState_GetPrivateData(pso, &pso_kept_guid, &size, NULL)))
        return refs;
    ID3D12PipelineState_SetPrivateData(pso, &pso_kept_guid, 0, NULL);
    AcquireSRWLockExclusive(&pso_lock);
    if (pso_queue_count == pso_queue_cap)
    {
        size_t cap = pso_queue_cap ? pso_queue_cap * 2 : 256;
        void *grown = realloc(pso_queue, cap * sizeof(*pso_queue));
        if (grown)
            pso_queue = grown, pso_queue_cap = cap;
    }
    if (pso_queue_count < pso_queue_cap)
    {
        pso_queue[pso_queue_count].pso = pso;
        pso_queue[pso_queue_count++].frame = frames_presented;
        pso = NULL;
    }
    ReleaseSRWLockExclusive(&pso_lock);
    if (pso) /* no room: released now */
        pso_orig_release(pso);
    return 0;
}

/* A PSO for the title: our reference, and Release patched (once; vkd3d-proton's PSOs share a vtable). */
static void pso_keep(void *object)
{
    static const BYTE mark = 1;
    ID3D12PipelineState *pso = object;
    void **vtbl;
    DWORD old;

    if (!pso)
        return;
    vtbl = *(void ***)pso;
    AcquireSRWLockExclusive(&pso_lock);
    if (vtbl[2] != (void *)pso_Release)
    {
        pso_orig_release = vtbl[2];
        VirtualProtect(&vtbl[2], sizeof(void *), PAGE_READWRITE, &old);
        vtbl[2] = (void *)pso_Release;
        VirtualProtect(&vtbl[2], sizeof(void *), old, &old);
    }
    ReleaseSRWLockExclusive(&pso_lock);
    if (SUCCEEDED(ID3D12PipelineState_SetPrivateData(pso, &pso_kept_guid, sizeof(mark), &mark)))
        ID3D12PipelineState_AddRef(pso);
}

/* At each present: the PSOs whose time is up. */
static void pso_release_due(void)
{
    LONG64 now = InterlockedIncrement64(&frames_presented);
    ID3D12PipelineState *due[64];
    size_t n = 0, kept = 0;

    AcquireSRWLockExclusive(&pso_lock);
    for (size_t i = 0; i < pso_queue_count; i++)
        if (n < ARRAYSIZE(due) && pso_queue[i].frame + PSO_KEEP_FRAMES <= now)
            due[n++] = pso_queue[i].pso;
        else
            pso_queue[kept++] = pso_queue[i];
    pso_queue_count = kept;
    ReleaseSRWLockExclusive(&pso_lock);
    for (size_t i = 0; i < n; i++)
        pso_orig_release(due[i]);
}

/* PSO creation (slots 10, 11): forwarded; the first PSO patches the PSO
 * class's GetDevice. */
static HRESULT STDMETHODCALLTYPE device_CreateGraphicsPipelineState(struct proxy *p,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC triangles;
    HRESULT hr;
    BOOL formats = FALSE;

    for (UINT i = 0; desc && i < 8; i++)
        formats |= pc_format(desc->RTVFormats[i]) != desc->RTVFormats[i];
    if (desc && (desc->PrimitiveTopologyType == CONSOLE_TOPOLOGY_TYPE_RECTANGLE || formats
                 || pc_format(desc->DSVFormat) != desc->DSVFormat))
    {
        triangles = *desc;
        if (desc->PrimitiveTopologyType == CONSOLE_TOPOLOGY_TYPE_RECTANGLE)
            triangles.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        for (UINT i = 0; i < 8; i++)
            triangles.RTVFormats[i] = pc_format(desc->RTVFormats[i]);
        triangles.DSVFormat = pc_format(desc->DSVFormat);
        desc = &triangles;
    }
    hr = ID3D12Device_CreateGraphicsPipelineState(INNER(p, ID3D12Device), desc, riid, out);
    if (SUCCEEDED(hr) && out)
    {
        patch_child(*out);
        pso_keep(*out);
        derived_pso_record(*out, desc);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateComputePipelineState(struct proxy *p,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    HRESULT hr = ID3D12Device_CreateComputePipelineState(INNER(p, ID3D12Device), desc, riid, out);
    if (SUCCEEDED(hr) && out)
    {
        patch_child(*out);
        pso_keep(*out);
    }
    return hr;
}

/* A pipeline stream's subobjects: a 32-bit type, then the payload at its alignment, the whole
 * aligned to 8 (alignas(void *)). The desktop's payload sizes and alignments by type; 0 for a type
 * the desktop doesn't have. */
static void stream_payload(UINT type, SIZE_T *size, SIZE_T *align)
{
    static const struct { unsigned short size, align; } payloads[] = {
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE] = { 8, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS] = { 16, 8 }, [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS] = { 16, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS] = { 16, 8 }, [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS] = { 16, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS] = { 16, 8 }, [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS] = { 16, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT] = { 32, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND] = { 328, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER] = { 44, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL] = { 52, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT] = { 16, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS] = { 36, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC] = { 8, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO] = { 16, 8 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS] = { 4, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1] = { 56, 4 },
        [D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING] = { 24, 8 },
        [23] = { 16, 8 },                          /* the console's AS (see desktop_stream) */
        [24] = { 16, 8 }, [25] = { 16, 8 },        /* AS, MS */
        [26] = { 68, 4 },                          /* DEPTH_STENCIL2 */
        [27] = { 44, 4 }, [28] = { 40, 4 },        /* RASTERIZER1, RASTERIZER2 */
    };
    *size = type < ARRAYSIZE(payloads) ? payloads[type].size : 0;
    *align = type < ARRAYSIZE(payloads) ? payloads[type].align : 0;
}

/* The console numbers the mesh pipeline's stages one lower than the desktop: 23 the amplification
 * shader, 24 the mesh shader (a UE5 sample's Nanite streams: 23 a null bytecode, 24 the mesh
 * shader, a pixel shader, no 25; the target title's: a 24 and a pixel shader, no 23 or 25). A
 * stream with a 23, or a 24 without a 25 (on the desktop an amplification shader with no mesh
 * shader, which it refuses), is so numbered; its 23 and 24 become the desktop's 24 and 25. */
static BOOL console_mesh_numbering(const BYTE *base, const BYTE *end)
{
    BOOL has24 = FALSE, has25 = FALSE;

    for (const BYTE *s = base; s + sizeof(UINT) <= end;)
    {
        UINT type = *(const UINT *)s;
        SIZE_T size, align;
        if (type == 23)
            return TRUE;
        has24 |= type == 24;
        has25 |= type == 25;
        stream_payload(type, &size, &align);
        if (!size || type == 26)
            break; /* past here the walk can't be trusted; the shaders come before */
        s += (((sizeof(UINT) + align - 1) & ~(align - 1)) + size + 7) & ~(SIZE_T)7;
    }
    return has24 && !has25;
}

/* The stream without what the desktop can't take: type 26 that isn't a desktop DEPTH_STENCIL2 (The
 * UE5 sample ends streams that already have a DEPTH_STENCIL1 with a 26 of 24 bytes, 8-aligned:
 * four dwords, 0xffff in the last, then a pointer; the console's own), or a type the desktop lacks.
 * Returns the stream to use: the title's, or a copy in *copy (freed by the caller). */
static const D3D12_PIPELINE_STATE_STREAM_DESC *desktop_stream(const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        D3D12_PIPELINE_STATE_STREAM_DESC *storage, BYTE **copy)
{
    const BYTE *base = desc->pPipelineStateSubobjectStream, *end = base + desc->SizeInBytes;
    BOOL renumber = console_mesh_numbering(base, end);
    SIZE_T kept = 0;

    *copy = NULL;
    /* The first mesh pipeline stream (AS or MS), whole, to check its layout against the desktop's. */
    {
        static LONG dumped;
        BOOL mesh = FALSE;
        for (const BYTE *s = base; s + sizeof(UINT) <= end && !mesh;)
        {
            UINT type = *(const UINT *)s;
            SIZE_T size, align;
            mesh = type == 24 || type == 25;
            stream_payload(type, &size, &align);
            if (!size)
                break;
            s += (((sizeof(UINT) + align - 1) & ~(align - 1)) + size + 7) & ~(SIZE_T)7;
        }
        if (mesh && !InterlockedExchange(&dumped, 1))
        {
            const UINT *d = desc->pPipelineStateSubobjectStream;
            rebox_log("d3d12_xs: first mesh pipeline stream, %zu bytes:", (size_t)desc->SizeInBytes);
            for (SIZE_T i = 0; i < desc->SizeInBytes / 4; i += 8)
            {
                char line[128];
                int at = 0;
                for (SIZE_T j = i; j < i + 8 && j < desc->SizeInBytes / 4; j++)
                    at += snprintf(line + at, sizeof(line) - at, " %08x", d[j]);
                rebox_log("d3d12_xs:   stream +%#05zx:%s", i * 4, line);
            }
        }
    }
    /* Pass 0 finds whether anything is dropped; pass 1 copies the rest. */
    for (int pass = 0; pass < 2; pass++)
    {
        BOOL depth_stencil = FALSE, dropped = FALSE, rectangles = FALSE;
        for (const BYTE *s = base; s + sizeof(UINT) <= end;)
        {
            UINT type = *(const UINT *)s;
            SIZE_T size, align, next;
            BOOL drop = FALSE;

            stream_payload(type, &size, &align);
            if (!size || (type == 26 && (depth_stencil || s + ((sizeof(UINT) + align - 1) & ~(align - 1)) + size > end)))
            {
                /* The console's: taken as 24 bytes of 8-aligned payload. */
                drop = TRUE;
                size = 24;
                align = 8;
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL
                    || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1)
                depth_stencil = TRUE;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY && s + 2 * sizeof(UINT) <= end
                    && ((const UINT *)s)[1] == CONSOLE_TOPOLOGY_TYPE_RECTANGLE)
                rectangles = TRUE;
            /* Console-only formats of render targets and depth (pc_format): as rectangles, a copy. */
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT && s + 2 * sizeof(UINT) <= end
                    && pc_format(((const UINT *)s)[1]) != ((const UINT *)s)[1])
                rectangles = TRUE;
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS && s + 10 * sizeof(UINT) <= end)
                for (UINT i = 1; i <= 8; i++)
                    if (pc_format(((const UINT *)s)[i]) != ((const UINT *)s)[i])
                        rectangles = TRUE;
            next = (((sizeof(UINT) + align - 1) & ~(align - 1)) + size + 7) & ~(SIZE_T)7;
            if (next > (SIZE_T)(end - s))
                next = end - s;
            if (drop)
            {
                dropped = TRUE;
                if (pass == 0)
                {
                    static LONG logged;
                    if (InterlockedIncrement(&logged) <= 4)
                        rebox_log("d3d12_xs: pipeline stream subobject type %u at +%#zx (%zu bytes) is the console's: "
                                     "dropped", type, (size_t)(s - base), (size_t)next);
                }
            }
            else if (pass == 1)
            {
                memcpy(*copy + kept, s, next);
                if (renumber && (type == 23 || type == 24))
                    *(UINT *)(*copy + kept) = type + 1;
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY && next >= 2 * sizeof(UINT)
                        && ((UINT *)(*copy + kept))[1] == CONSOLE_TOPOLOGY_TYPE_RECTANGLE)
                    ((UINT *)(*copy + kept))[1] = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT && next >= 2 * sizeof(UINT))
                    ((UINT *)(*copy + kept))[1] = pc_format(((UINT *)(*copy + kept))[1]);
                if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS && next >= 10 * sizeof(UINT))
                    for (UINT i = 1; i <= 8; i++)
                        ((UINT *)(*copy + kept))[i] = pc_format(((UINT *)(*copy + kept))[i]);
                kept += next;
            }
            s += next;
        }
        if (pass == 0 && ((!dropped && !renumber && !rectangles) || !(*copy = malloc(desc->SizeInBytes))))
            return desc;
    }
    storage->SizeInBytes = kept;
    storage->pPipelineStateSubobjectStream = *copy;
    return storage;
}

/* For tests: the stream as the desktop gets it, copied to out (up to max bytes). Returns its size. */
SIZE_T WINAPI ReboxDesktopStream(const void *stream, SIZE_T size, void *out, SIZE_T max)
{
    D3D12_PIPELINE_STATE_STREAM_DESC desc = { size, (void *)stream }, storage;
    const D3D12_PIPELINE_STATE_STREAM_DESC *result;
    BYTE *copy = NULL;
    SIZE_T n;

    result = desktop_stream(&desc, &storage, &copy);
    n = result->SizeInBytes;
    memcpy(out, result->pPipelineStateSubobjectStream, min(n, max));
    free(copy);
    return n;
}

/* For tests: the subobject types of a stream as the desktop gets it. Returns how many. */
UINT WINAPI ReboxDesktopStreamTypes(const void *stream, SIZE_T size, UINT *types, UINT max)
{
    D3D12_PIPELINE_STATE_STREAM_DESC desc = { size, (void *)stream }, storage;
    const D3D12_PIPELINE_STATE_STREAM_DESC *out;
    BYTE *copy = NULL;
    UINT n = 0;

    out = desktop_stream(&desc, &storage, &copy);
    for (const BYTE *s = out->pPipelineStateSubobjectStream, *end = s + out->SizeInBytes; s + sizeof(UINT) <= end && n < max;)
    {
        UINT type = *(const UINT *)s;
        SIZE_T payload, align;
        types[n++] = type;
        stream_payload(type, &payload, &align);
        if (!payload)
            break;
        s += (((sizeof(UINT) + align - 1) & ~(align - 1)) + payload + 7) & ~(SIZE_T)7;
    }
    free(copy);
    return n;
}

/* Slot 86: ID3D12Device2::CreatePipelineState (PC 47), a pipeline from a stream of subobjects, as
 * Unreal 5's Xbox RHI makes them (a UE5 sample, XboxCommonD3D12PipelineState.cpp). Forwarded
 * without the console's own subobjects (desktop_stream): vkd3d-proton takes the console shaders in
 * it as in the other pipeline paths. A stream it still refuses has its first type logged.
 *
 * A stream whose console mesh shader sits in the amplification slot with no mesh shader of its own
 * (the target title's minimap: a 24 and a pixel shader) is refused, E_INVALIDARG: xcl (ivall/xcl)
 * found that the title renders nothing from then on when they're made (translated, black every
 * run), and gets to its menus refusing them. REBOX_MESH_PIPELINES=1 makes them, with the console
 * stages renumbered (and fallback shaders where they don't translate). */
static HRESULT STDMETHODCALLTYPE device_CreatePipelineState(struct proxy *p, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
        REFIID riid, void **out)
{
    D3D12_PIPELINE_STATE_STREAM_DESC storage;
    ID3D12Device2 *device2 = NULL;
    BYTE *copy = NULL;
    HRESULT hr;

    if (desc && desc->pPipelineStateSubobjectStream && desc->SizeInBytes
            && console_mesh_numbering(desc->pPipelineStateSubobjectStream,
                                      (const BYTE *)desc->pPipelineStateSubobjectStream + desc->SizeInBytes))
    {
        const char *make = getenv("REBOX_MESH_PIPELINES");
        if (!make || *make != '1')
        {
            static LONG refused;
            if (InterlockedIncrement(&refused) <= 4)
                rebox_log("d3d12_xs: a mesh pipeline with the console's numbering refused (REBOX_MESH_PIPELINES=1 "
                          "makes it)");
            if (out)
                *out = NULL;
            return E_INVALIDARG;
        }
    }
    hr = IUnknown_QueryInterface(p->inner, &IID_ID3D12Device2, (void **)&device2);
    if (FAILED(hr))
        return hr;
    hr = ID3D12Device2_CreatePipelineState(device2, desc && desc->pPipelineStateSubobjectStream
            ? desktop_stream(desc, &storage, &copy) : desc, riid, out);
    ID3D12Device2_Release(device2);
    free(copy);
    if (SUCCEEDED(hr) && out && *out)
    {
        patch_child(*out);
        pso_keep(*out);
    }
    else if (FAILED(hr) && desc && desc->pPipelineStateSubobjectStream)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: CreatePipelineState (stream of %zu bytes, first subobject type %u) failed: %#lx",
                         (size_t)desc->SizeInBytes, desc->SizeInBytes >= sizeof(UINT)
                         ? *(const UINT *)desc->pPipelineStateSubobjectStream : 0u, (unsigned long)hr);
    }
    return hr;
}

/* Slot 62 (XboxBvhBuilder.dll 0x18000c4e1): CreateComputePipelineState with
 * two extra arguments, zero at the only call seen. */
static HRESULT STDMETHODCALLTYPE device_CreateComputePipelineStateX(struct proxy *p,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, UINT_PTR extra1, UINT_PTR extra2, REFIID riid, void **out)
{
    if (extra1 || extra2)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: Xbox CreateComputePipelineStateX extra args %#llx %#llx ignored",
                         (unsigned long long)extra1, (unsigned long long)extra2);
    }
    return device_CreateComputePipelineState(p, desc, riid, out);
}

/* Slot 60: CreateGraphicsPipelineState with an extension array, as an engine
 * RHI calls it (TomBox, a UE5 title): (desc, extension count, extensions, riid,
 * out). The desc has the PC layout (0x290 bytes); the extensions have no PC
 * equivalent and are ignored. */
static HRESULT STDMETHODCALLTYPE device_CreateGraphicsPipelineStateX(struct proxy *p,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, UINT ext_count, const void *ext, REFIID riid, void **out)
{
    if (ext_count)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: Xbox CreateGraphicsPipelineStateX: %u extension structs ignored", ext_count);
    }
    return device_CreateGraphicsPipelineState(p, desc, riid, out);
}

/* The PC size of a CheckFeatureSupport struct, or 0 if not known. */
static UINT feature_size(D3D12_FEATURE feature)
{
    switch (feature)
    {
    case D3D12_FEATURE_D3D12_OPTIONS: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS);
    case D3D12_FEATURE_ARCHITECTURE: return sizeof(D3D12_FEATURE_DATA_ARCHITECTURE);
    case D3D12_FEATURE_FEATURE_LEVELS: return sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS);
    case D3D12_FEATURE_FORMAT_SUPPORT: return sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT);
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: return sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS);
    case D3D12_FEATURE_FORMAT_INFO: return sizeof(D3D12_FEATURE_DATA_FORMAT_INFO);
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: return sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT);
    case D3D12_FEATURE_SHADER_MODEL: return sizeof(D3D12_FEATURE_DATA_SHADER_MODEL);
    case D3D12_FEATURE_D3D12_OPTIONS1: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1);
    case D3D12_FEATURE_ROOT_SIGNATURE: return sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE);
    case D3D12_FEATURE_ARCHITECTURE1: return sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1);
    case D3D12_FEATURE_D3D12_OPTIONS2: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2);
    case D3D12_FEATURE_SHADER_CACHE: return sizeof(D3D12_FEATURE_DATA_SHADER_CACHE);
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: return sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY);
    case D3D12_FEATURE_D3D12_OPTIONS3: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3);
    case D3D12_FEATURE_EXISTING_HEAPS: return sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS);
    case D3D12_FEATURE_D3D12_OPTIONS4: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4);
    case D3D12_FEATURE_D3D12_OPTIONS5: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5);
    case D3D12_FEATURE_D3D12_OPTIONS6: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6);
    case D3D12_FEATURE_D3D12_OPTIONS7: return sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7);
    default: return 0;
    }
}

/* Slot 13: CheckFeatureSupport. vkd3d-proton wants each struct's PC size exactly; a console SDK's
 * structs can be sized differently (a UE5 title's D3D12_OPTIONS check failed and it crashed), so on
 * a size mismatch the PC struct is filled and what fits copied back, the rest zeroed. */
static HRESULT STDMETHODCALLTYPE device_CheckFeatureSupport(struct proxy *p, D3D12_FEATURE feature, void *data,
        UINT size)
{
    ID3D12Device *device = INNER(p, ID3D12Device);
    UINT pc = feature_size(feature);
    BYTE buffer[512];
    HRESULT hr;

    if (!pc || pc == size || pc > sizeof(buffer) || !data)
    {
        hr = ID3D12Device_CheckFeatureSupport(device, feature, data, size);
        if (FAILED(hr))
        {
            static LONG seen[64];
            if ((UINT)feature >= 64 || InterlockedIncrement(&seen[feature]) <= 2)
                rebox_log("d3d12_xs: CheckFeatureSupport(feature %u, %u bytes) failed %#lx", feature, size, hr);
        }
        return hr;
    }
    /* In-out structs (format support, sample counts, levels, shader model, root signature...)
     * carry the title's input in their first bytes. */
    memset(buffer, 0, pc);
    memcpy(buffer, data, min(size, pc));
    if (SUCCEEDED(hr = ID3D12Device_CheckFeatureSupport(device, feature, buffer, pc)))
    {
        memcpy(data, buffer, min(size, pc));
        if (size > pc)
            memset((BYTE *)data + pc, 0, size - pc);
    }
    {
        static LONG seen[64];
        if ((UINT)feature >= 64 || InterlockedIncrement(&seen[feature]) <= 1)
            rebox_log("d3d12_xs: CheckFeatureSupport(feature %u): the title's struct is %u bytes, PC's %u (%#lx)",
                         feature, size, pc, hr);
    }
    return hr;
}

/* Slot 55: a graphics PSO derived from another with state patches (TomBox, a UE5 title):
 * (base, patch count, patches, riid, out); see derived_pso.c. */
static HRESULT STDMETHODCALLTYPE device_CreateDerivedPipelineStateX(struct proxy *p, void *base, UINT count,
        const void *patches, REFIID riid, void **out)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    HRESULT hr;

    if (out)
        *out = NULL;
    if (FAILED(hr = derived_pso_desc(base, count, patches, &desc)))
        return hr;
    return device_CreateGraphicsPipelineState(p, &desc, riid, out);
}

/* Slot 44: CreatePlacedResourceX(address, desc, state, clear, iid, out) (ScarlettX, XValkyrie):
 * a resource on memory the title already has, at a GPU address it chose. A buffer in GPU memory
 * from ReboxGpuAlloc (XMemAlloc's GPU-visible memory) is placed in that chunk's heap at the
 * address, so the title and the GPU share it; a buffer over other title memory has that memory
 * made GPU memory in place first (gpu_memory_adopt). Anything else gets its own memory and is
 * logged once per kind: textures (the chunk heaps hold buffers only, and console tilings aren't
 * modelled), memory that couldn't be made GPU memory, and placements the heap refuses. An address in
 * a GPU-only view of title memory is placed over the CPU memory behind it. */
static HRESULT STDMETHODCALLTYPE device_CreatePlacedResourceX(struct proxy *p, D3D12_GPU_VIRTUAL_ADDRESS address,
        const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid,
        void **out)
{
    static const D3D12_HEAP_PROPERTIES own = { D3D12_HEAP_TYPE_DEFAULT };
    static LONG logged[3];
    D3D12_RESOURCE_DESC storage;
    const D3D12_RESOURCE_DESC *d = pc_desc(desc, &storage);
    ID3D12Heap *heap;
    UINT64 offset;
    HRESULT hr = E_FAIL;
    int why;

    if (!d || !out)
        return E_INVALIDARG;
    {
        /* Each distinct kind (dimension, size, format) once, the first 64 kinds. */
        static struct { UINT dimension, height, format; UINT64 width; } kinds[64];
        static LONG kind_count;
        static SRWLOCK kind_lock = SRWLOCK_INIT;
        UINT64 cpu = gpu_view_to_cpu(address);
        BOOL first = cpu != address;
        AcquireSRWLockExclusive(&kind_lock);
        for (LONG i = 0; first && i < kind_count; i++)
            if (kinds[i].dimension == (UINT)desc->Dimension && kinds[i].width == desc->Width
                    && kinds[i].height == desc->Height && kinds[i].format == (UINT)desc->Format)
                first = FALSE;
        if (first && kind_count < ARRAYSIZE(kinds))
            kinds[kind_count++] = (typeof(kinds[0])){ desc->Dimension, desc->Height, desc->Format, desc->Width };
        else
            first = FALSE;
        ReleaseSRWLockExclusive(&kind_lock);
        /* Screen-sized ones (video planes) each, the first 64. */
        {
            static LONG large;
            if (!first && cpu != address && desc->Width >= 1920 && desc->Height >= 1080
                    && InterlockedIncrement(&large) <= 64)
                first = TRUE;
        }
        if (first)
            rebox_log("d3d12_xs: placed at %#llx in a GPU view of title memory, over %#llx: dimension %u, %llux%u, "
                      "format %u, flags %#x", (unsigned long long)address, (unsigned long long)cpu, desc->Dimension,
                      (unsigned long long)desc->Width, desc->Height, desc->Format, desc->Flags);
        address = cpu;
    }
    if (d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        why = 0;
    /* A buffer over the title's own memory: that memory is made GPU memory where it is. */
    else if (!gpu_memory_heap_at(address, &heap, &offset)
            && !(gpu_memory_adopt(address, d->Width) && gpu_memory_heap_at(address, &heap, &offset)))
        why = 1;
    else
    {
        D3D12_CLEAR_VALUE clear_storage;
        hr = ID3D12Device_CreatePlacedResource(INNER(p, ID3D12Device), heap, offset, d, state,
                                               pc_clear(clear, &clear_storage), riid, out);
        ID3D12Heap_Release(heap);
        why = 2;
    }
    if (SUCCEEDED(hr))
    {
        present_track_state(*out, state);
        return hr;
    }
    if (!InterlockedExchange(&logged[why], 1))
        rebox_log("d3d12_xs: CreatePlacedResourceX(%#llx, dimension %u, %llu bytes): %s; it gets its own memory",
                     (unsigned long long)address, d->Dimension, (unsigned long long)d->Width,
                     why == 0 ? "a texture, uploaded from title memory when that changes"
                     : why == 1 ? "not in GPU memory" : "the heap refused the placement");
    hr = device_CreateCommittedResource(p, &own, D3D12_HEAP_FLAG_NONE, desc, state, clear, riid, out);
    /* A texture is the memory at its address on the console: keep it filled from there. */
    if (SUCCEEDED(hr) && why == 0 && *out)
        title_texture_register(*out, address, desc, d, state);
    return hr;
}

/* Device methods of the newer layout (SDK 0x20004) a retail title uses, after xcl (ivall/xcl,
 * GPL-2.0-only, crates/xcl/src/hle/d3d12x: device_x.rs, real.rs, kernelx.rs). */

/* The resource state of a D3D12_BARRIER_LAYOUT (undefined, 0xffffffff: COMMON), as xcl's
 * layout_state. */
static D3D12_RESOURCE_STATES layout_state(UINT32 layout)
{
    switch (layout)
    {
    case 1: return D3D12_RESOURCE_STATE_GENERIC_READ;
    case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case 3: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;
    case 6: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case 7: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case 8: return D3D12_RESOURCE_STATE_COPY_DEST;
    case 9: return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    case 10: return D3D12_RESOURCE_STATE_RESOLVE_DEST;
    default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

/* Slot 133: CreatePlacedResourceX with a barrier layout in the initial state's place (the target
 * title places a buffer over its physical pages with 0xffffffff, undefined). */
static HRESULT STDMETHODCALLTYPE device_CreatePlacedResourceLayoutX(struct proxy *p, D3D12_GPU_VIRTUAL_ADDRESS address,
        const D3D12_RESOURCE_DESC *desc, UINT32 layout, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    note_newer_device_method(133);
    return device_CreatePlacedResourceX(p, address, desc, layout_state(layout), clear, riid, out);
}

/* XALLOC_ATTRIBUTES of a display target's memory (xcl's D3D_DISPLAY_TARGET): 4 MiB pages, the D3D
 * allocator (128), graphics memory. The target title's hook gives its back buffers one block for
 * every request with these and asserts that the block gets used up. */
#define XALLOC_DISPLAY_TARGET (2ull << 14 | 0x80ull << 16 | 16ull << 24 | 1ull << 29)

/* Slot 127: CreateCommittedResource with a barrier layout in the initial state's place: the PC's
 * CreateCommittedResource3 arguments (heap properties, flags, desc, layout, clear, protected
 * session, castable format count and list, riid, out). A display target (ALLOW_DISPLAY) also gets
 * its console memory from the title's allocation hook, as the console runtime's does. */
static HRESULT STDMETHODCALLTYPE device_CreateCommittedResourceLayoutX(struct proxy *p,
        const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc, UINT32 layout,
        const D3D12_CLEAR_VALUE *clear, void *protected_session, UINT32 castable_count, const DXGI_FORMAT *castable,
        REFIID riid, void **out)
{
    HRESULT hr;

    note_newer_device_method(127);
    hr = device_CreateCommittedResource(p, props, flags & ~D3D12_HEAP_FLAG_ALLOW_DISPLAY, desc,
                                        layout_state(layout), clear, riid, out);

    if (SUCCEEDED(hr) && (flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) && desc
            && GetProcAddress(GetModuleHandleA(NULL), "__XMemAllocationHooks__"))
    {
        void *(WINAPI *xmem_alloc)(SIZE_T, ULONGLONG) = (void *)GetProcAddress(GetModuleHandleA("xmem.dll"), "XMemAlloc");
        UINT64 size = title_texture_size(desc);
        void *memory = size && xmem_alloc ? xmem_alloc((SIZE_T)size, XALLOC_DISPLAY_TARGET) : NULL;
        static LONG logged;
        if (memory && out && *out)
            note_texture_address(*out, (UINT64)(ULONG_PTR)memory);
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("d3d12_xs: display target %llux%u format %u: %#llx bytes of the title's memory at %p",
                      (unsigned long long)desc->Width, desc->Height, desc->Format, (unsigned long long)size, memory);
    }
    return hr;
}

/* A resource placed at a title address for views of memory (slots 70 and 71), made once per address
 * and description and kept, as xcl's resource_at. */
static struct memory_view_resource { UINT64 address; D3D12_RESOURCE_DESC desc; ID3D12Resource *res; } *memory_views;
static SIZE_T memory_view_count, memory_view_capacity;
static SRWLOCK memory_view_lock = SRWLOCK_INIT;

static ID3D12Resource *resource_at(struct proxy *p, const D3D12_RESOURCE_DESC *desc, UINT64 address)
{
    ID3D12Resource *res = NULL;

    if (!desc || !address)
        return NULL;
    AcquireSRWLockExclusive(&memory_view_lock);
    for (SIZE_T i = 0; i < memory_view_count && !res; i++)
        if (memory_views[i].address == address && !memcmp(&memory_views[i].desc, desc, sizeof(*desc)))
            res = memory_views[i].res;
    if (!res && SUCCEEDED(device_CreatePlacedResourceX(p, address, desc, D3D12_RESOURCE_STATE_COMMON, NULL,
            &IID_ID3D12Resource, (void **)&res)) && res)
    {
        if (memory_view_count == memory_view_capacity)
        {
            SIZE_T n = memory_view_capacity ? memory_view_capacity * 2 : 64;
            struct memory_view_resource *grown = realloc(memory_views, n * sizeof(*grown));
            if (grown)
            {
                memory_views = grown;
                memory_view_capacity = n;
            }
        }
        if (memory_view_count < memory_view_capacity)
            memory_views[memory_view_count++] = (struct memory_view_resource){ address, *desc, res };
    }
    ReleaseSRWLockExclusive(&memory_view_lock);
    return res;
}

/* Slot 70: a shader resource view of memory, without a resource object: (resource desc, { the PC's
 * SRV desc; GPU address at +0x28 }, destination). */
static void STDMETHODCALLTYPE device_CreateShaderResourceViewAtX(struct proxy *p, const D3D12_RESOURCE_DESC *rdesc,
        const BYTE *view, SIZE_T cpu)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC v;
    ID3D12Resource *res;

    note_newer_device_method(70);
    if (!view || IsBadReadPtr(view, 0x30))
        return;
    memcpy(&v, view, sizeof(v));
    res = resource_at(p, rdesc, *(const UINT64 *)(view + 0x28));
    device_CreateShaderResourceView(p, res, res ? &v : NULL, cpu);
}

/* Slot 71: the same for an unordered access view ({ the PC's UAV desc; GPU address at +0x28; the
 * counter's at +0x30 }). The counter isn't kept. */
static void STDMETHODCALLTYPE device_CreateUnorderedAccessViewAtX(struct proxy *p, const D3D12_RESOURCE_DESC *rdesc,
        const BYTE *view, SIZE_T cpu)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC v;
    ID3D12Resource *res;

    note_newer_device_method(71);
    if (!view || IsBadReadPtr(view, 0x38))
        return;
    memcpy(&v, view, sizeof(v));
    res = resource_at(p, rdesc, *(const UINT64 *)(view + 0x28));
    device_CreateUnorderedAccessView(p, res, NULL, res ? &v : NULL, cpu);
}

/* Slot 61: CreateSamplerX(desc, handle) (ScarlettX's retail trace; layout from XValkyrie): a
 * sampler with each console filter separate. Filters are 0 point, 1 linear, 2/3 those with
 * anisotropy; the mip filter 0 none, 1 point, 2 linear; the mode 0 blend, 1 minimum, 2 maximum.
 * The Z filter, the second LOD bias, the anisotropy bias and threshold have no D3D12 field. */
struct console_sampler
{
    UINT flags, filter_mag, filter_min, filter_mip, filter_z, filter_mode;
    D3D12_TEXTURE_ADDRESS_MODE address_u, address_v, address_w;
    FLOAT min_lod, max_lod, lod_bias, lod_bias_secondary;
    UINT max_anisotropy;
    FLOAT anisotropy_bias;
    UINT anisotropy_threshold;
    D3D12_COMPARISON_FUNC comparison;
    FLOAT border[4];
};

static D3D12_TEXTURE_ADDRESS_MODE sampler_address(D3D12_TEXTURE_ADDRESS_MODE mode)
{
    return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE
            ? mode : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
}

static void STDMETHODCALLTYPE device_CreateSamplerX(struct proxy *p, const struct console_sampler *x, SIZE_T handle)
{
    D3D12_SAMPLER_DESC d = { 0 };
    BOOL compare, aniso;
    UINT reduction;

    if (!x)
        return;
    compare = x->comparison > D3D12_COMPARISON_FUNC_NEVER && x->comparison < D3D12_COMPARISON_FUNC_ALWAYS;
    aniso = x->filter_mag >= 2 || x->filter_min >= 2;
    reduction = compare ? 1 : x->filter_mode == 1 ? 2 : x->filter_mode == 2 ? 3 : 0;
    d.Filter = (D3D12_FILTER)((aniso ? 0x55u : ((x->filter_min & 1) << 4 | (x->filter_mag & 1) << 2
                                                | (x->filter_mip == 2)))
                              | reduction << 7);
    d.AddressU = sampler_address(x->address_u);
    d.AddressV = sampler_address(x->address_v);
    d.AddressW = sampler_address(x->address_w);
    d.MipLODBias = x->lod_bias;
    d.MaxAnisotropy = aniso ? min(max(x->max_anisotropy, 1u), 16u) : 1;
    d.ComparisonFunc = compare ? x->comparison : D3D12_COMPARISON_FUNC_NEVER;
    memcpy(d.BorderColor, x->border, sizeof(d.BorderColor));
    d.MinLOD = x->min_lod;
    d.MaxLOD = x->filter_mip ? x->max_lod : x->min_lod;
    device_CreateSampler(p, &d, handle);
}

/* Slots 74/75: RegisterPagePoolX(address, page count, HANDLE *) and UnregisterPagePoolX(handle)
 * (XValkyrie). They tell the console which physical pages back a range. xmem's physical pages are
 * ordinary committed memory, so there is nothing to register: accepted, with a unique handle. */
static HRESULT STDMETHODCALLTYPE device_RegisterPagePoolX(struct proxy *p, void *address, UINT_PTR count, HANDLE *out)
{
    static LONG64 next;
    static LONG logged;

    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: RegisterPagePoolX(%p, %llu pages) accepted (physical pages are ordinary memory)",
                     address, (unsigned long long)count);
    if (out)
        *out = (HANDLE)(ULONG_PTR)(0x7a6f0000 + InterlockedIncrement64(&next));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_UnregisterPagePoolX(struct proxy *p, HANDLE pool)
{
    return S_OK;
}

/* Slot 76 (FFX 0x1800033cc): CreateCommittedResource with a 0x40-byte
 * resource desc; the PC fields come first, Xbox fields fill +0x34..+0x3f.
 * Xbox2Windows saw Layout 0x101 with +0x34 = 3 and +0x38 = 0 there. */
static HRESULT STDMETHODCALLTYPE device_CreateCommittedResourceX(struct proxy *p,
        const D3D12_HEAP_PROPERTIES *props, D3D12_HEAP_FLAGS flags, const BYTE *desc_x,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12_RESOURCE_DESC desc;
    static LONG once;

    if (!desc_x)
        return E_INVALIDARG;
    memset(&desc, 0, sizeof(desc));
    memcpy(&desc, desc_x, offsetof(D3D12_RESOURCE_DESC, Flags) + sizeof(desc.Flags));
    if (!InterlockedExchange(&once, 1))
        rebox_log("d3d12_xs: Xbox resource desc fields ignored: layout %#x, +0x34 %#x, +0x38 %#llx",
                     desc.Layout, *(const UINT *)(desc_x + 0x34), *(const unsigned long long *)(desc_x + 0x38));
    return device_CreateCommittedResource(p, props, flags, &desc, state, clear, riid, out);
}

/* Slots 68, 69, 78-80 (ScarlettX, traced from a retail title): settings and
 * frame pacing. Accepted without effect, so the title isn't paced. */
static HRESULT STDMETHODCALLTYPE device_Slot68(struct proxy *p, UINT_PTR a1, UINT_PTR a2, UINT_PTR a3)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        rebox_log("d3d12_xs: Xbox Device slot 68 (%#llx, %#llx, %#llx) -> S_OK", (unsigned long long)a1,
                     (unsigned long long)a2, (unsigned long long)a3);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_Slot69(struct proxy *p, UINT_PTR id, UINT_PTR value)
{
    static LONG count;
    if (InterlockedIncrement(&count) <= 16)
        rebox_log("d3d12_xs: Xbox Device slot 69 setting %#llx = %#llx ignored", (unsigned long long)id,
                 (unsigned long long)value);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_SetFrameIntervalX(struct proxy *p, void *output, UINT interval_us, UINT period)
{
    return pacing_set_interval(interval_us, period);
}

static HRESULT STDMETHODCALLTYPE device_ScheduleFrameEventX(struct proxy *p, UINT type, UINT_PTR offset_us,
        const void *list, UINT flags)
{
    return pacing_schedule(type, offset_us, list, flags);
}

static HRESULT STDMETHODCALLTYPE device_WaitFrameEventX(struct proxy *p, UINT type, UINT timeout_ms, void *event,
        UINT flags, UINT64 *token)
{
    return pacing_wait(type, timeout_ms, flags, token);
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatisticsX(struct proxy *p, UINT64 token, UINT types, UINT *count,
        void *statistics)
{
    return pacing_frame_statistics(token, types, count, statistics);
}

/* ------------------------------------------------------------------ queue */

volatile UINT64 rebox_table_watch; /* console_compile.c: a table to show at the next submission */
volatile UINT64 rebox_value_watch; /* console_compile.c (REBOX_WATCH_STAGE): 4 floats to show at the next submission */
BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size);

static void lists_wait(UINT count, ID3D12CommandList *const *lists);
static void cpu_writes_after(struct proxy *queue, UINT count, ID3D12CommandList *const *lists);

static void STDMETHODCALLTYPE queue_ExecuteCommandLists(struct proxy *p, UINT count, ID3D12CommandList *const *lists)
{
    ID3D12CommandList *stack[64] = { 0 }, **raw = count <= ARRAYSIZE(stack) ? stack : malloc(count * sizeof(*raw));

    for (UINT i = 0; i < count; i++)
        raw[i] = proxy_unwrap(lists[i]);
    /* Memory waits the lists start with, on the CPU (lists_wait). */
    lists_wait(count, lists);
    /* Placed textures whose title memory changed are uploaded first (title_textures.c). */
    title_textures_upload(INNER(p, ID3D12CommandQueue));
    /* Descriptors the title wrote into heap memory that isn't GPU memory reach the GPU's copy. */
    shadow_sync_mirrors();
    /* Textures the title wrote after recording their draws (console_compile.c). */
    console_resolve_late();
    {
        UINT64 value_at = InterlockedExchange64((volatile LONG64 *)&rebox_value_watch, 0);
        const float *f = (const float *)(ULONG_PTR)value_at;
        if (value_at && !IsBadReadPtr(f, 16))
            rebox_log("d3d12_xs: at submission, %#llx holds %g %g %g %g", (unsigned long long)value_at, f[0], f[1], f[2],
                      f[3]);
    }
    {
        UINT64 at = InterlockedExchange64((volatile LONG64 *)&rebox_table_watch, 0);
        UINT32 t[16];
        if (at && ReboxShadowRead(at, t, sizeof(t)))
            rebox_log("d3d12_xs: at submission, table %#llx holds %08x %08x %08x %08x %08x %08x %08x %08x  %08x %08x "
                      "%08x %08x %08x %08x %08x %08x", (unsigned long long)at, t[0], t[1], t[2], t[3], t[4], t[5], t[6],
                      t[7], t[8], t[9], t[10], t[11], t[12], t[13], t[14], t[15]);
    }
    ID3D12CommandQueue_ExecuteCommandLists(INNER(p, ID3D12CommandQueue), count, raw);
    /* Their writes outside GPU memory, once the GPU is there (cpu_writes_after). */
    cpu_writes_after(p, count, lists);
    if (raw != stack)
        free(raw);
}

/* Older-layout slot 35: the target title submits batches of up to 256 lists through it,
 * (count, lists, value, flags 0 or 3), not checking the result. Taken as ExecuteCommandLists;
 * the value and flags are logged once. */
static HRESULT STDMETHODCALLTYPE queue_ExecuteCommandListsX(struct proxy *p, UINT count, ID3D12CommandList *const *lists,
        UINT64 value, UINT flags)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: queue slot 35 (ExecuteCommandLists, %u lists) with %#llx, flags %#x",
                  count, (unsigned long long)value, flags);
    queue_ExecuteCommandLists(p, count, lists);
    return S_OK;
}

/* SignalX(fence, value, flush flags): Signal; the flags (cache flushes before the signal, e.g.
 * 0x8000000 from Unreal) have nothing to do here, the queue is already ordered. */
/* The last value each fence was told to reach, for ReboxHangReport: a fence whose GPU value
 * lags it is work the GPU never finished; one that matches it, with the title still waiting, is a
 * signal the title expected from something we don't do. */
static struct
{
    ID3D12Fence *fence;
    UINT64 value;
    LONG64 signals;
} signalled[64];
static SRWLOCK signalled_lock = SRWLOCK_INIT;

static void note_signal(ID3D12Fence *fence, UINT64 value)
{
    AcquireSRWLockExclusive(&signalled_lock);
    for (unsigned i = 0; i < ARRAYSIZE(signalled); i++)
        if (signalled[i].fence == fence || !signalled[i].fence)
        {
            /* Kept alive for the report (at most 64): a fence released since would be read freed. */
            if (!signalled[i].fence)
                ID3D12Fence_AddRef(fence);
            signalled[i].fence = fence;
            signalled[i].value = value;
            signalled[i].signals++;
            break;
        }
    ReleaseSRWLockExclusive(&signalled_lock);
}

/* Two queue layouts share SDK version 0x20004 (xcl, GPL-2.0-only, crates/xcl/src/hle/d3d12x/real.rs):
 * the older (several titles) is the PC's through 18 (Signal 14, Wait 15,
 * GetTimestampFrequency 16, GetClockCalibration 17) with SignalX 23 and PresentX 30; the newer
 * (a later title) has two methods ahead of Signal (Signal 16 ... SignalX 25, PresentX 32), and
 * its lists' console methods sit two slots later too. Nothing at device creation tells them apart,
 * but Signal and Wait take a fence and the older 16/17 never do: the first fence at 16 or 17 means
 * the newer layouts from then on. A GDK title (0x20004) called 16 as GetTimestampFrequency, which the
 * newer table took for Signal (vkd3d-proton asserted on the non-fence). fence_vtbl and
 * newer_layouts are declared with the classes. */

static BOOL is_fence(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (is_proxy(p))
        return ((const struct proxy *)p)->cls == &fence_class;
    if (!p || !fence_vtbl || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
            || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || (const BYTE *)p + 8 > (const BYTE *)mbi.BaseAddress + mbi.RegionSize)
        return FALSE;
    return *(void *const *)p == fence_vtbl;
}

static void note_newer_layouts(UINT slot)
{
    if (!InterlockedExchange(&newer_layouts, 1))
    {
        rebox_log("d3d12_xs: a fence passed to queue slot %u: the title's runtime has the newer layouts (queue: "
                     "Signal 16, PresentX 32; lists: console methods two slots later)", slot);
        lists_to_newer_layout();
    }
}

static HRESULT STDMETHODCALLTYPE queue_SignalX(struct proxy *p, ID3D12Fence *fence, UINT64 value, UINT flags);

/* Signal and Wait: the fence is the title's proxy. */
static HRESULT STDMETHODCALLTYPE queue_Signal(struct proxy *p, ID3D12Fence *fence, UINT64 value)
{
    fence_note_signal(fence, value);
    return ID3D12CommandQueue_Signal(INNER(p, ID3D12CommandQueue), proxy_unwrap(fence), value);
}

static HRESULT STDMETHODCALLTYPE queue_Wait(struct proxy *p, ID3D12Fence *fence, UINT64 value)
{
    return ID3D12CommandQueue_Wait(INNER(p, ID3D12CommandQueue), proxy_unwrap(fence), value);
}

static HRESULT STDMETHODCALLTYPE queue_PresentX(struct proxy *p, UINT planes, const void *plane_params,
        const void *params);

/* Slot 16: newer Signal(fence, value), or older GetTimestampFrequency(&frequency). */
static HRESULT STDMETHODCALLTYPE queue_Slot16(struct proxy *p, void *a1, UINT64 a2)
{
    if (is_fence(a1))
    {
        note_newer_layouts(16);
        return queue_Signal(p, a1, a2);
    }
    return ID3D12CommandQueue_GetTimestampFrequency(INNER(p, ID3D12CommandQueue), a1);
}

/* Slot 17: newer Wait(fence, value), or older GetClockCalibration(&gpu, &cpu). */
static HRESULT STDMETHODCALLTYPE queue_Slot17(struct proxy *p, void *a1, UINT64 a2)
{
    if (is_fence(a1))
    {
        note_newer_layouts(17);
        return queue_Wait(p, a1, a2);
    }
    return ID3D12CommandQueue_GetClockCalibration(INNER(p, ID3D12CommandQueue), a1, (UINT64 *)(ULONG_PTR)a2);
}

/* Slot 18: the newer layout's GetTimestampFrequency(UINT64 *) (xcl). The target title calls it
 * before any fence reaches 16 or 17, so before the layout is known; it writes 8 bytes either way. */
static HRESULT STDMETHODCALLTYPE queue_Slot18(struct proxy *p, UINT64 *frequency)
{
    return ID3D12CommandQueue_GetTimestampFrequency(INNER(p, ID3D12CommandQueue), frequency);
}

/* Slots 25 and 32: the newer layout's SignalX and PresentX. */
static HRESULT STDMETHODCALLTYPE queue_Slot25(struct proxy *p, ID3D12Fence *fence, UINT64 value, UINT flags)
{
    if (newer_layouts || is_fence(fence))
    {
        note_newer_layouts(25);
        return queue_SignalX(p, fence, value, flags);
    }
    rebox_log("d3d12_xs: queue slot 25 on the older layout: unknown method, E_NOTIMPL");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE queue_Slot32(struct proxy *p, UINT planes, const void *plane_params, const void *params)
{
    if (newer_layouts)
        return queue_PresentX(p, planes, plane_params, params);
    rebox_log("d3d12_xs: queue slot 32 on the older layout: unknown method, E_NOTIMPL");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE queue_SignalX(struct proxy *p, ID3D12Fence *fence, UINT64 value, UINT flags)
{
    note_signal(proxy_unwrap(fence), value);
    return queue_Signal(p, fence, value);
}

/* xmem's hang watchdog calls this after the thread stacks (crash.c). */
void WINAPI ReboxHangReport(void)
{
    AcquireSRWLockShared(&signalled_lock);
    for (unsigned i = 0; i < ARRAYSIZE(signalled) && signalled[i].fence; i++)
    {
        UINT64 done = ID3D12Fence_GetCompletedValue(signalled[i].fence);
        rebox_log("d3d12_xs: fence %p: signalled to %llu (%lld signals), GPU at %llu%s", (void *)signalled[i].fence,
                     (unsigned long long)signalled[i].value, (long long)signalled[i].signals, (unsigned long long)done,
                     done < signalled[i].value ? " (behind)" : "");
    }
    ReleaseSRWLockShared(&signalled_lock);
    pacing_report();
    proxy_log_slot_stats();
}

/* GetDesc, in either argument order (see resource_GetDesc). */
static D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_GetDesc(struct proxy *p, D3D12_COMMAND_QUEUE_DESC *ret)
{
    if (result_first(p, ret))
    {
        struct proxy *queue = (struct proxy *)ret;
        ret = (D3D12_COMMAND_QUEUE_DESC *)p;
        p = queue;
    }
    ((D3D12_COMMAND_QUEUE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_COMMAND_QUEUE_DESC *))PC_SLOT(p->inner, 18))(
            p->inner, ret);
    return ret;
}

/* Slot 30 (ScarlettX): PresentX(plane count, plane params, present params).
 * See present.c. */
static HRESULT STDMETHODCALLTYPE queue_PresentX(struct proxy *p, UINT planes, const void *plane_params,
        const void *params)
{
    /* The first plane's first field is the token WaitFrameEventX gave for this frame (pacing.c). */
    UINT64 token = planes && plane_params ? *(const UINT64 *)plane_params : 0;
    LONGLONG call = pacing_now();
    HRESULT hr = present_x(INNER(p, ID3D12CommandQueue), &p->present, planes, plane_params, params);

    pacing_presented(token, call, pacing_now());
    return hr;
}

/* ---------------------------------------------------------- command list */

static UINT64 heap_handle_to_vkd3d(UINT64 handle);

static void STDMETHODCALLTYPE list_SetDescriptorHeaps(struct proxy *p, UINT count, ID3D12DescriptorHeap *const *heaps)
{
    ID3D12DescriptorHeap *raw[8];

    if (count > ARRAYSIZE(raw))
        count = ARRAYSIZE(raw); /* D3D12 allows one CBV/SRV/UAV and one sampler heap */
    for (UINT i = 0; i < count; i++)
        raw[i] = proxy_unwrap(heaps[i]);
    p->heap_count = count < 2 ? count : 2;
    memcpy(p->heaps, raw, p->heap_count * sizeof(*raw));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(INNER(p, ID3D12GraphicsCommandList), count, raw);
}

/* GPU descriptor handles may be real heap addresses (heap_handle_to_vkd3d). */
static void STDMETHODCALLTYPE list_SetComputeRootDescriptorTable(struct proxy *p, UINT index, UINT64 handle)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = { heap_handle_to_vkd3d(handle) };
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(INNER(p, ID3D12GraphicsCommandList), index, h);
}

static void STDMETHODCALLTYPE list_SetGraphicsRootDescriptorTable(struct proxy *p, UINT index, UINT64 handle)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = { heap_handle_to_vkd3d(handle) };
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(INNER(p, ID3D12GraphicsCommandList), index, h);
}

static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewUint(struct proxy *p, UINT64 gpu, SIZE_T cpu,
        ID3D12Resource *resource, const UINT values[4], UINT rect_count, const D3D12_RECT *rects)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = { heap_handle_to_vkd3d(gpu) };
    D3D12_CPU_DESCRIPTOR_HANDLE c = { shadow_cpu_to_inner(cpu) };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(INNER(p, ID3D12GraphicsCommandList), g, c, resource,
            values, rect_count, rects);
}

static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewFloat(struct proxy *p, UINT64 gpu, SIZE_T cpu,
        ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = { heap_handle_to_vkd3d(gpu) };
    D3D12_CPU_DESCRIPTOR_HANDLE c = { shadow_cpu_to_inner(cpu) };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(INNER(p, ID3D12GraphicsCommandList), g, c, resource,
            values, rect_count, rects);
}

/* Barriers are forwarded and also feed PresentX's resource-state tracking:
 * graphics slot 26 / copy slot 15 (PC 26), and enhanced barriers at slot 146
 * (PC 80, ID3D12GraphicsCommandList7::Barrier). */
/* Transition barriers with no resource, which vkd3d-proton reads through (a UE5 sample,
 * Unreal 5, crashed in its ResourceBarrier reading address 4): left out. UAV and aliasing barriers
 * may name no resource, as on the desktop. */
static void STDMETHODCALLTYPE list_ResourceBarrier(struct proxy *p, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
{
    D3D12_RESOURCE_BARRIER local[32], *kept = NULL;
    UINT n = 0, i;

    if (!barriers)
        return;
    for (i = 0; i < count; i++)
        if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && !barriers[i].Transition.pResource)
            break;
    if (i < count)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: ResourceBarrier: a transition with no resource (barrier %u of %u, states %#x -> %#x) "
                         "left out", i, count, barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
        kept = count <= ARRAYSIZE(local) ? local : malloc(count * sizeof(*kept));
        if (!kept)
            return;
        for (i = 0; i < count; i++)
            if (barriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || barriers[i].Transition.pResource)
                kept[n++] = barriers[i];
        barriers = kept;
        count = n;
    }
    present_track_barriers(count, barriers);
    if (count)
        ID3D12GraphicsCommandList_ResourceBarrier(INNER(p, ID3D12GraphicsCommandList), count, barriers);
    if (kept && kept != local)
        free(kept);
}

static void STDMETHODCALLTYPE list_Barrier(struct proxy *p, UINT32 count, const D3D12_BARRIER_GROUP *groups)
{
    present_track_enhanced(count, groups);
    ((void (STDMETHODCALLTYPE *)(void *, UINT32, const D3D12_BARRIER_GROUP *))PC_SLOT(p->inner, 80))(p->inner,
            count, groups);
}

/* Copies into a title texture (one filled from title memory) make it the GPU's: a UE5 title's font
 * atlas is zeros in memory and gets its glyphs by CopyTextureRegion (xcl's finding), so uploading
 * memory over it would wipe them. */
static void note_copy_dest(ID3D12Resource *dst)
{
    D3D12_RESOURCE_DESC rd;
    if (!dst)
        return;
    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)dst)[10])(dst, &rd);
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        title_texture_gpu_written(dst);
}

/* Slot 16 (and the copy list's 12): CopyTextureRegion. */
static void STDMETHODCALLTYPE list_CopyTextureRegion(struct proxy *p, const D3D12_TEXTURE_COPY_LOCATION *dst,
        UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
    if (dst)
        note_copy_dest(dst->pResource);
    if (dst)
        note_copy_dest(dst->pResource);
    ID3D12GraphicsCommandList_CopyTextureRegion(INNER(p, ID3D12GraphicsCommandList), dst, x, y, z, src, box);
}

/* Slot 17: CopyResource; 84: CopyResourceX, the same with a flags argument. */
static void STDMETHODCALLTYPE list_CopyResource(struct proxy *p, ID3D12Resource *dst, ID3D12Resource *src)
{
    note_copy_dest(dst);
    ID3D12GraphicsCommandList_CopyResource(INNER(p, ID3D12GraphicsCommandList), dst, src);
}

static void STDMETHODCALLTYPE list_CopyResourceX(struct proxy *p, ID3D12Resource *dst, ID3D12Resource *src,
        UINT flags)
{
    note_copy_dest(dst);
    ID3D12GraphicsCommandList_CopyResource(INNER(p, ID3D12GraphicsCommandList), dst, src);
}

/* Slot 83 (ScarlettX): CopyTextureRegion plus a trailing dword, 0 at every
 * call they saw. */
static void STDMETHODCALLTYPE list_CopyTextureRegionX(struct proxy *p, const D3D12_TEXTURE_COPY_LOCATION *dst,
        UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box, UINT extra)
{
    if (extra)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: CopyTextureRegionX extra argument %#x ignored", extra);
    }
    ID3D12GraphicsCommandList_CopyTextureRegion(INNER(p, ID3D12GraphicsCommandList), dst, x, y, z, src, box);
}

/* Embedded data lives until the list has been reset this many more times;
 * by then the GPU work that read it has normally finished. */
#define EMBEDDED_KEEP_RESETS 3

/* Console methods with nothing to do on the host (cache flushes, depth bounds). */
/* Ray tracing on a list (PC 75 and 76: slots 120 and 121, 122 and 123 from SDK 0x20004): a placeholder pipeline
 * (state_object.c) never reaches vkd3d-proton, and rays dispatched with one are skipped. Every
 * state object we hand out is a placeholder, so anything else isn't vkd3d-proton's either: Unreal 5
 * passed such objects here, which vkd3d-proton's breadcrumbs read as one of its own and crashed on
 * (a UE5 sample). They're skipped too, and where the object's methods live is logged once. */
static void STDMETHODCALLTYPE list_SetPipelineState1(struct proxy *p, ID3D12StateObject *state_object)
{
    p->ray_pipeline = FALSE;
    if (state_object && !state_object_is_placeholder(state_object))
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
        {
            const void *vtbl = IsBadReadPtr(state_object, sizeof(void *)) ? NULL : *(const void *const *)state_object;
            HMODULE module = NULL;
            char name[MAX_PATH] = "?";

            if (vtbl && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, vtbl, &module))
                GetModuleFileNameA(module, name, sizeof(name));
            rebox_log("d3d12_xs: SetPipelineState1 with a state object that isn't ours (%p, methods at %p in %s+%#llx); "
                         "its rays are skipped", (void *)state_object, vtbl, name,
                         (unsigned long long)((const BYTE *)vtbl - (const BYTE *)module));
        }
    }
}

static void STDMETHODCALLTYPE list_DispatchRays(struct proxy *p, const D3D12_DISPATCH_RAYS_DESC *desc)
{
    if (!p->ray_pipeline)
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: DispatchRays without a real ray tracing pipeline: skipped (console ray tracing isn't translated)");
        return;
    }
    ID3D12GraphicsCommandList4_DispatchRays(INNER(p, ID3D12GraphicsCommandList4), desc);
}

static void STDMETHODCALLTYPE list_Ignore(struct proxy *p)
{
}

/* Slots 50 and 52 (PC 46 and 48): OMSetRenderTargets and ClearRenderTargetView, forwarded, with
 * the display's render targets counted; slots 12 and 13 (draws) count while one is bound. */
static void STDMETHODCALLTYPE list_OMSetRenderTargets(struct proxy *p, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE *rts,
        BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
{
    int display = -1;
    for (UINT i = 0; rts && i < (single ? 1 : count) && display < 0; i++)
        display = display_rtv(rts[i].ptr);
    p->display_bound = display + 1;
    p->render_target0 = rts && count ? rts[0].ptr : 0;
    if (display >= 0)
        InterlockedIncrement(&display_binds[display]);
    ID3D12GraphicsCommandList_OMSetRenderTargets(INNER(p, ID3D12GraphicsCommandList), count, rts, single, ds);
}

static void STDMETHODCALLTYPE list_ClearRenderTargetView(struct proxy *p, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        const FLOAT *color, UINT rect_count, const D3D12_RECT *rects)
{
    int display = display_rtv(rtv.ptr);
    if (display >= 0)
        InterlockedIncrement(&display_clears[display]);
    ID3D12GraphicsCommandList_ClearRenderTargetView(INNER(p, ID3D12GraphicsCommandList), rtv, color, rect_count, rects);
}

/* A DrawInstanced of a rect list as two triangles a rectangle (after xcl, ivall/xcl#195,
 * theaustinayers): each bound vertex buffer's vertices are copied as v0 v1 v2, v0 v2 v3, the fourth
 * corner made on the CPU as v3 = v0 + v2 - v1. The layout isn't looked at: a 4-byte word the same
 * in all three vertices is kept (a packed colour), any other is taken as a float (a retail title's
 * full-screen passes: (-1,1) (-1,-1) (1,-1), texture coordinates (0,0) (0,1) (1,1)). FALSE: no
 * vertex buffers (a shader that makes its vertices from the index) or one this can't read. */
static BOOL draw_rects(struct proxy *p, UINT vertices, UINT instances, UINT first_vertex, UINT first_instance)
{
    D3D12_VERTEX_BUFFER_VIEW expanded[ARRAYSIZE(p->vertex_buffers)];
    UINT rects = vertices / 3, mask = p->vertex_buffer_mask;
    static LONG logged;

    if (!mask || !rects || rects > 4096)
        return FALSE;
    for (UINT slot = 0; slot < ARRAYSIZE(p->vertex_buffers); slot++)
    {
        const D3D12_VERTEX_BUFFER_VIEW *v = &p->vertex_buffers[slot];
        SIZE_T s = v->StrideInBytes, len = (SIZE_T)rects * 3 * s;
        const BYTE *at = (const BYTE *)(ULONG_PTR)v->BufferLocation + (SIZE_T)first_vertex * s;
        BYTE *out;

        if (!(mask & (1u << slot)))
            continue;
        if (!s || s % 4 || (UINT64)first_vertex * s + len > v->SizeInBytes || IsBadReadPtr(at, len)
                || !(out = malloc(rects * 6 * s)))
            return FALSE;
        for (UINT r = 0; r < rects; r++)
        {
            const BYTE *src = at + r * 3 * s;
            BYTE *dst = out + r * 6 * s;
            memcpy(dst, src, 3 * s);
            memcpy(dst + 3 * s, src, s);
            memcpy(dst + 4 * s, src + 2 * s, s);
            for (SIZE_T w = 0; w < s / 4; w++)
            {
                UINT32 a, b, c, d;
                float fa, fb, fc, fd;
                memcpy(&a, src + w * 4, 4);
                memcpy(&b, src + s + w * 4, 4);
                memcpy(&c, src + 2 * s + w * 4, 4);
                if (a == b && b == c)
                    d = a;
                else
                {
                    memcpy(&fa, &a, 4), memcpy(&fb, &b, 4), memcpy(&fc, &c, 4);
                    fd = fa + fc - fb;
                    memcpy(&d, &fd, 4);
                }
                memcpy(dst + 5 * s + w * 4, &d, 4);
            }
        }
        expanded[slot].BufferLocation = console_draw_buffer(out, rects * 6 * s);
        free(out);
        if (!expanded[slot].BufferLocation)
            return FALSE;
        expanded[slot].SizeInBytes = rects * 6 * s;
        expanded[slot].StrideInBytes = s;
    }
    for (UINT slot = 0; slot < ARRAYSIZE(p->vertex_buffers); slot++)
        if (mask & (1u << slot))
            ID3D12GraphicsCommandList_IASetVertexBuffers(INNER(p, ID3D12GraphicsCommandList), slot, 1, &expanded[slot]);
    ID3D12GraphicsCommandList_DrawInstanced(INNER(p, ID3D12GraphicsCommandList), rects * 6, instances, 0, first_instance);
    for (UINT slot = 0; slot < ARRAYSIZE(p->vertex_buffers); slot++)
        if (mask & (1u << slot))
            ID3D12GraphicsCommandList_IASetVertexBuffers(INNER(p, ID3D12GraphicsCommandList), slot, 1,
                                                         &p->vertex_buffers[slot]);
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: rect lists are drawn as two triangles a rectangle, the fourth corner made on the CPU "
                  "(first: %u rectangles)", rects);
    return TRUE;
}

/* At REBOX_WATCH_STAGE's vertex stage's first 4 draws (with REBOX_DRAW_CENSUS, which names the
 * stage of a draw): the bound vertex buffers and the first vertices' bytes. */
static void log_watched_vertex_buffers(struct proxy *p, UINT first_vertex, UINT vertices)
{
    static LONG logged;
    if (!console_stage_watched(console_last_draw_vs()) || InterlockedIncrement(&logged) > 4)
        return;
    for (UINT i = 0; i < ARRAYSIZE(p->vertex_buffers); i++)
        if (p->vertex_buffer_mask & (1u << i))
        {
            const D3D12_VERTEX_BUFFER_VIEW *v = &p->vertex_buffers[i];
            const UINT32 *d = (const UINT32 *)(ULONG_PTR)(v->BufferLocation + (UINT64)first_vertex * v->StrideInBytes);
            UINT n = min(min(vertices, 4u) * v->StrideInBytes / 4, 24u);
            char line[300];
            int at = 0;
            for (UINT k = 0; k < n && !IsBadReadPtr(d + k, 4) && at < 280; k++)
                at += snprintf(line + at, sizeof(line) - at, " %08x", d[k]);
            line[at] = 0;
            rebox_log("d3d12_xs: watched stage's draw (topology %u): vertex buffer %u at %#llx, %u bytes, stride %u:%s",
                      p->topology, i, (unsigned long long)v->BufferLocation, v->SizeInBytes, v->StrideInBytes, line);
        }
}

/* REBOX_DRAW_CENSUS=1: the per-100-presents draw log names draws into every target, not only the
 * display-sized ones (target -1), to find a stage (a UI text shader) for REBOX_WATCH_STAGE or
 * REBOX_GRAPHICS_DEBUG=magenta:<id>. */
static BOOL draw_census(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("REBOX_DRAW_CENSUS");
        on = v && *v && strcmp(v, "0");
    }
    return on;
}

static void STDMETHODCALLTYPE list_DrawInstanced(struct proxy *p, UINT vertices, UINT instances, UINT first_vertex,
        UINT first_instance)
{
    if (p->display_bound)
        InterlockedIncrement(&display_draws[p->display_bound - 1]);
    if (p->rect_list && rect_lists_on_cpu() && draw_rects(p, vertices, instances, first_vertex, first_instance))
        return;
    if (p->display_bound || draw_census())
        console_display_draw_begin(p->display_bound - 1, vertices, p->rect_list, FALSE);
    ID3D12GraphicsCommandList_DrawInstanced(INNER(p, ID3D12GraphicsCommandList), vertices, instances, first_vertex,
                                            first_instance);
    console_display_draw_end();
    log_watched_vertex_buffers(p, first_vertex, vertices);
}

static void STDMETHODCALLTYPE list_DrawIndexedInstanced(struct proxy *p, UINT indices, UINT instances, UINT first_index,
        INT base_vertex, UINT first_instance)
{
    if (p->display_bound)
        InterlockedIncrement(&display_draws[p->display_bound - 1]);
    if (p->display_bound || draw_census())
        console_display_draw_begin(p->display_bound - 1, indices, p->rect_list, TRUE);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(INNER(p, ID3D12GraphicsCommandList), indices, instances, first_index,
                                                   base_vertex, first_instance);
    if (console_stage_watched(console_last_draw_vs()) && p->index_buffer.BufferLocation)
    {
        /* The vertices from the draw's first index on (a quad batch's indices run in order). */
        BOOL wide = p->index_buffer.Format == DXGI_FORMAT_R32_UINT;
        const BYTE *at = (const BYTE *)(ULONG_PTR)p->index_buffer.BufferLocation + (UINT64)first_index * (wide ? 4 : 2);
        UINT first = 0;
        if (!IsBadReadPtr(at, wide ? 4 : 2))
            first = wide ? *(const UINT32 *)at : *(const UINT16 *)at;
        log_watched_vertex_buffers(p, base_vertex + first, indices);
    }
    if (console_stage_watched(console_last_draw_vs()))
    {
        static LONG logged;
        UINT width = 0, height = 0, format = 0;
        BOOL known = find_rtv(p->render_target0, &width, &height, &format);
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("d3d12_xs: watched stage's draw: render target 0 %s %ux%u, format %u", known ? "is" : "unknown;",
                      width, height, format);
    }
    console_display_draw_end();
}

/* Console memory writes and waits (xcl, ivall/xcl, GPL-2.0-only, list_x.rs and cpu_writes.rs; older
 * layout 69-72 and 88-90, newer two later): a value written to a GPU address at the bottom or top of
 * the pipe, or a wait on one. A write to our GPU memory (identity-mapped) is WriteBufferImmediate.
 * One anywhere else is kept with the list and made by the CPU once the GPU has finished the
 * submission before it (cpu_writes_after): a title marks its GPU progress so, in its own memory,
 * and waits on it. Waits are evaluated on the CPU before the list's submission, unless an earlier
 * write in the same list covers them. */
static void list_write_values(struct proxy *p, UINT64 address, const UINT32 *values, UINT count,
        D3D12_WRITEBUFFERIMMEDIATE_MODE mode, const char *what)
{
    static LONG logged_cpu;
    ID3D12Heap *heap;
    UINT64 offset;

    if (!address)
        return;
    if (gpu_memory_heap_at(address, &heap, &offset))
    {
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[2];
        D3D12_WRITEBUFFERIMMEDIATE_MODE modes[2] = { mode, mode };
        ID3D12Heap_Release(heap);
        for (UINT i = 0; i < count; i++)
            params[i] = (D3D12_WRITEBUFFERIMMEDIATE_PARAMETER){ address + 4 * i, values[i] };
        ID3D12GraphicsCommandList2_WriteBufferImmediate(INNER(p, ID3D12GraphicsCommandList2), count, params, modes);
        if (p->gpu_write_count == p->gpu_write_capacity)
        {
            UINT capacity = p->gpu_write_capacity ? p->gpu_write_capacity * 2 : 16;
            struct cpu_write *grown = realloc(p->gpu_writes, capacity * sizeof(*grown));
            if (!grown)
                return;
            p->gpu_writes = grown;
            p->gpu_write_capacity = capacity;
        }
        p->gpu_writes[p->gpu_write_count++] = (struct cpu_write){ address, { values[0], count > 1 ? values[1] : 0 }, count };
    }
    else
    {
        if (p->cpu_write_count == p->cpu_write_capacity)
        {
            UINT capacity = p->cpu_write_capacity ? p->cpu_write_capacity * 2 : 16;
            struct cpu_write *grown = realloc(p->cpu_writes, capacity * sizeof(*grown));
            if (!grown)
                return;
            p->cpu_writes = grown;
            p->cpu_write_capacity = capacity;
        }
        p->cpu_writes[p->cpu_write_count++] = (struct cpu_write){ address, { values[0], count > 1 ? values[1] : 0 }, count };
        if (!InterlockedExchange(&logged_cpu, 1))
            rebox_log("d3d12_xs: %s to %#llx, outside GPU memory: written by the CPU once the GPU is there",
                      what, (unsigned long long)address);
    }
}

static void list_wait(struct proxy *p, UINT64 address, UINT32 comparison, UINT64 value, UINT64 mask, UINT width)
{
    if (!address || comparison == D3D12_COMPARISON_FUNC_ALWAYS)
        return;
    /* A write earlier in this list satisfies it once the GPU is there; waiting before the
     * submission would wait for ever. */
    for (UINT i = 0; i < p->cpu_write_count; i++)
        if (p->cpu_writes[i].address <= address && address < p->cpu_writes[i].address + 4 * p->cpu_writes[i].count)
            return;
    for (UINT i = 0; i < p->gpu_write_count; i++)
        if (p->gpu_writes[i].address <= address && address < p->gpu_writes[i].address + 4 * p->gpu_writes[i].count)
            return;
    if (p->wait_count == p->wait_capacity)
    {
        UINT capacity = p->wait_capacity ? p->wait_capacity * 2 : 8;
        struct memory_wait *grown = realloc(p->waits, capacity * sizeof(*grown));
        if (!grown)
            return;
        p->waits = grown;
        p->wait_capacity = capacity;
    }
    p->waits[p->wait_count++] = (struct memory_wait){ address, value, mask, comparison, width };
}

static void STDMETHODCALLTYPE list_Wait32BitValueX(struct proxy *p, UINT64 address, UINT32 comparison, UINT32 value,
        UINT32 mask, UINT flags)
{
    list_wait(p, address, comparison, value, mask, 4);
}

/* The whole 64-bit value is compared: what follows it isn't a mask. A retail title passes a 32-bit
 * 1 there, the upper half of the stack slot left over (0x231_0000_0001); read as a mask, its waits
 * for a counter (2 >= 1, 4 >= 2...) never held and each ran to the timeout, every frame (xcl,
 * ivall/xcl d4f17ca, PR #218). */
static void STDMETHODCALLTYPE list_Wait64BitValueX(struct proxy *p, UINT64 address, UINT32 comparison, UINT64 value,
        UINT64 not_a_mask, UINT flags)
{
    list_wait(p, address, comparison, value, ~(UINT64)0, 8);
}

/* (memory & mask) <comparison> value, D3D12_COMPARISON_FUNC; TRUE for what can't be read. */
static BOOL wait_satisfied(const struct memory_wait *w)
{
    UINT64 now;
    if (IsBadReadPtr((void *)(ULONG_PTR)w->address, w->width))
        return TRUE;
    now = (w->width == 8 ? *(volatile UINT64 *)(ULONG_PTR)w->address : *(volatile UINT32 *)(ULONG_PTR)w->address)
          & w->mask;
    switch (w->comparison)
    {
    case D3D12_COMPARISON_FUNC_NEVER: return TRUE; /* would never return: skipped */
    case D3D12_COMPARISON_FUNC_LESS: return now < w->value;
    case D3D12_COMPARISON_FUNC_EQUAL: return now == w->value;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return now <= w->value;
    case D3D12_COMPARISON_FUNC_GREATER: return now > w->value;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return now != w->value;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return now >= w->value;
    default: return TRUE;
    }
}

/* Before a submission: each list's waits, on the CPU (up to 2 s each, then logged and passed). */
static void lists_wait(UINT count, ID3D12CommandList *const *lists)
{
    for (UINT i = 0; i < count; i++)
    {
        struct proxy *l = (struct proxy *)lists[i];
        if (!is_proxy(l))
            continue;
        for (UINT k = 0; k < l->wait_count; k++)
        {
            const struct memory_wait *w = &l->waits[k];
            LARGE_INTEGER start, now, freq;
            BOOL ok;
            QueryPerformanceCounter(&start);
            QueryPerformanceFrequency(&freq);
            while (!(ok = wait_satisfied(w)))
            {
                QueryPerformanceCounter(&now);
                if (now.QuadPart - start.QuadPart > 2 * freq.QuadPart)
                    break;
                Sleep(0);
            }
            if (!ok)
            {
                static LONG logged;
                if (InterlockedIncrement(&logged) <= 4)
                    rebox_log("d3d12_xs: Wait%uBitValueX at %#llx (comparison %u, %#llx, mask %#llx) timed out after 2 s",
                              w->width * 8, (unsigned long long)w->address, w->comparison,
                              (unsigned long long)w->value, (unsigned long long)w->mask);
            }
        }
    }
}

/* After a submission: the lists' CPU writes, made by a worker once the queue's fence passes the
 * value signalled after them, in submission order (up to 10 s, then made anyway). */
struct cpu_write_job { ID3D12Fence *fence; UINT64 value; struct cpu_write *writes; UINT count; struct cpu_write_job *next; };
static struct cpu_write_job *write_jobs, **write_jobs_tail = &write_jobs;
static CRITICAL_SECTION write_jobs_cs;
static CONDITION_VARIABLE write_jobs_cv;

static DWORD WINAPI cpu_write_worker(void *arg)
{
    HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (;;)
    {
        struct cpu_write_job *job;
        EnterCriticalSection(&write_jobs_cs);
        while (!write_jobs)
            SleepConditionVariableCS(&write_jobs_cv, &write_jobs_cs, INFINITE);
        job = write_jobs;
        if (!(write_jobs = job->next))
            write_jobs_tail = &write_jobs;
        LeaveCriticalSection(&write_jobs_cs);
        if (ID3D12Fence_GetCompletedValue(job->fence) < job->value
                && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(job->fence, job->value, event)))
            WaitForSingleObject(event, 10000);
        for (UINT i = 0; i < job->count; i++)
        {
            const struct cpu_write *w = &job->writes[i];
            if (!IsBadWritePtr((void *)(ULONG_PTR)w->address, 4 * w->count))
            {
                volatile UINT32 *to = (volatile UINT32 *)(ULONG_PTR)w->address;
                for (UINT k = 0; k < w->count; k++)
                    to[k] = w->values[k];
            }
        }
        ID3D12Fence_Release(job->fence);
        free(job->writes);
        free(job);
    }
    return 0;
}

static BOOL CALLBACK cpu_writes_init(INIT_ONCE *once, void *param, void **ctx)
{
    HANDLE thread;
    InitializeCriticalSection(&write_jobs_cs);
    InitializeConditionVariable(&write_jobs_cv);
    if ((thread = CreateThread(NULL, 0, cpu_write_worker, NULL, 0, NULL)))
        CloseHandle(thread);
    return thread != NULL;
}

static void cpu_writes_after(struct proxy *queue, UINT count, ID3D12CommandList *const *lists)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    struct cpu_write_job *job;
    UINT total = 0, at = 0;

    for (UINT i = 0; i < count; i++)
        if (is_proxy(lists[i]))
            total += ((struct proxy *)lists[i])->cpu_write_count;
    if (!total || !InitOnceExecuteOnce(&once, cpu_writes_init, NULL, NULL))
        return;
    if (!queue->cpu_fence)
    {
        ID3D12Device *device = NULL;
        if (SUCCEEDED(ID3D12CommandQueue_GetDevice(INNER(queue, ID3D12CommandQueue), &IID_ID3D12Device, (void **)&device)))
        {
            ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&queue->cpu_fence);
            ID3D12Device_Release(device);
        }
        if (!queue->cpu_fence)
            return;
    }
    if (!(job = calloc(1, sizeof(*job))) || !(job->writes = malloc(total * sizeof(*job->writes))))
    {
        free(job);
        return;
    }
    for (UINT i = 0; i < count; i++)
        if (is_proxy(lists[i]))
        {
            struct proxy *l = (struct proxy *)lists[i];
            memcpy(job->writes + at, l->cpu_writes, l->cpu_write_count * sizeof(*job->writes));
            at += l->cpu_write_count;
        }
    job->count = total;
    job->value = ++queue->cpu_fence_value;
    job->fence = queue->cpu_fence;
    ID3D12Fence_AddRef(job->fence);
    ID3D12CommandQueue_Signal(INNER(queue, ID3D12CommandQueue), queue->cpu_fence, job->value);
    EnterCriticalSection(&write_jobs_cs);
    *write_jobs_tail = job;
    write_jobs_tail = &job->next;
    WakeConditionVariable(&write_jobs_cv);
    LeaveCriticalSection(&write_jobs_cs);
}

static void STDMETHODCALLTYPE list_Write32BitValueBottomOfPipeX(struct proxy *p, UINT64 address, UINT32 value)
{
    list_write_values(p, address, &value, 1, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT, "Write32BitValueBottomOfPipeX");
}

static void STDMETHODCALLTYPE list_Write64BitValueBottomOfPipeX(struct proxy *p, UINT64 address, UINT64 value)
{
    UINT32 v[2] = { (UINT32)value, (UINT32)(value >> 32) };
    list_write_values(p, address, v, 2, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT, "Write64BitValueBottomOfPipeX");
}

static void STDMETHODCALLTYPE list_Write32BitValueTopOfPipeX(struct proxy *p, UINT64 address, UINT32 value)
{
    list_write_values(p, address, &value, 1, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN, "Write32BitValueTopOfPipeX");
}

static void STDMETHODCALLTYPE list_Write64BitValueTopOfPipeX(struct proxy *p, UINT64 address, UINT64 value)
{
    UINT32 v[2] = { (UINT32)value, (UINT32)(value >> 32) };
    list_write_values(p, address, v, 2, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN, "Write64BitValueTopOfPipeX");
}


/* Acceleration structure builds, postbuild info and copies (PC 72-74). Ray tracing isn't translated
 * (state_object.c), so no ray reads them, and building them faulted the GPU: a UE5 sample fills its
 * bottom levels by copies in a mode vkd3d-proton doesn't support (4), and the top-level build then
 * read instances pointing at unbuilt memory (device lost, breadcrumbs in run 39).
 * REBOX_ACCELERATION_STRUCTURES=1 passes them on. */
static BOOL acceleration_structures(void)
{
    static int on = -1;
    if (on < 0)
    {
        char *env = rebox_getenv("REBOX_ACCELERATION_STRUCTURES");
        on = env && env[0] == '1';
        free(env);
    }
    return on;
}

static void note_skipped_acceleration_structures(void)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: acceleration structure builds and copies are skipped: ray tracing isn't translated "
                     "(REBOX_ACCELERATION_STRUCTURES=1 passes them on)");
}

static void STDMETHODCALLTYPE list_BuildRaytracingAccelerationStructure(struct proxy *p,
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT count,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *info)
{
    if (!acceleration_structures())
        return note_skipped_acceleration_structures();
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(INNER(p, ID3D12GraphicsCommandList4), desc, count,
            info);
}

static void STDMETHODCALLTYPE list_EmitRaytracingAccelerationStructurePostbuildInfo(struct proxy *p,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT count,
        const D3D12_GPU_VIRTUAL_ADDRESS *sources)
{
    if (!acceleration_structures())
        return note_skipped_acceleration_structures();
    ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(INNER(p, ID3D12GraphicsCommandList4),
            desc, count, sources);
}

static void STDMETHODCALLTYPE list_CopyRaytracingAccelerationStructure(struct proxy *p,
        D3D12_GPU_VIRTUAL_ADDRESS dst, D3D12_GPU_VIRTUAL_ADDRESS src,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode)
{
    if (!acceleration_structures())
        return note_skipped_acceleration_structures();
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(INNER(p, ID3D12GraphicsCommandList4), dst, src, mode);
}

static HRESULT STDMETHODCALLTYPE list_Reset(struct proxy *p, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso)
{
    struct embedded **e, *keep = NULL, *drop = NULL;

    p->cpu_write_count = 0;
    p->gpu_write_count = 0;
    p->wait_count = 0;
    p->rect_list = FALSE;
    p->vertex_buffer_mask = 0;

    p->resets++;
    p->ray_pipeline = FALSE;
    while (p->active)
    {
        struct embedded *a = p->active;
        p->active = a->next;
        a->generation = p->resets;
        a->next = p->retired;
        p->retired = a;
    }
    for (e = &p->retired; *e;)
    {
        struct embedded *a = *e;
        *e = a->next;
        if (a->generation + EMBEDDED_KEEP_RESETS <= p->resets)
            a->next = drop, drop = a;
        else
            a->next = keep, keep = a;
    }
    p->retired = keep;
    embedded_free_list(drop);
    p->heap_count = 0;
    return ID3D12GraphicsCommandList_Reset(INNER(p, ID3D12GraphicsCommandList), proxy_unwrap(alloc), pso);
}

/* Newer-layout slot 139: (allocator, NULL, NULL), HRESULT checked; the target title calls it on
 * a pooled list with its allocator where Reset would be. Taken as Reset with a third argument;
 * a list still recording (fresh from creation) is closed first, which the desktop requires. */
static HRESULT STDMETHODCALLTYPE list_ResetX(struct proxy *p, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso,
        UINT64 extra)
{
    static LONG logged;
    HRESULT hr;

    if (extra && !InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: command list slot 139 (a Reset) with a third argument %#llx", (unsigned long long)extra);
    if (FAILED(hr = list_Reset(p, alloc, pso))
            && SUCCEEDED(ID3D12GraphicsCommandList_Close(INNER(p, ID3D12GraphicsCommandList))))
        hr = list_Reset(p, alloc, pso);
    return hr;
}

/* Slot 148 (XboxBvhBuilder.dll 0x180020ed6): allocate(size, alignment)
 * returns memory the title fills and then binds by address, so it must be
 * GPU-visible and, for the title, its own GPU VA. */
static void *STDMETHODCALLTYPE list_AllocateEmbeddedX(struct proxy *p, UINT64 size, UINT64 alignment)
{
    struct embedded *a = p->active;
    SIZE_T offset;

    if (alignment < 16)
        alignment = 16;
    if (a)
    {
        offset = ((ULONG_PTR)a->cpu + a->used + alignment - 1) & ~(alignment - 1);
        offset -= (ULONG_PTR)a->cpu;
        if (offset + size <= a->size)
        {
            a->used = offset + size;
            return a->cpu + offset;
        }
    }
    a = calloc(1, sizeof(*a));
    a->size = size + alignment > (1u << 20) ? size + alignment : (1u << 20);
    if (FAILED(ReboxGpuAlloc(a->size, 64 << 10, (void **)&a->cpu, NULL)))
    {
        rebox_log("d3d12_xs: embedded command-list allocation of %llu bytes failed", (unsigned long long)size);
        free(a);
        return NULL;
    }
    a->next = p->active;
    p->active = a;
    offset = (((ULONG_PTR)a->cpu + alignment - 1) & ~(alignment - 1)) - (ULONG_PTR)a->cpu;
    a->used = offset + size;
    return a->cpu + offset;
}

/* ---------------------------------------------------------- descriptor heap */

/* Heap addresses the title sees (heap slot 10). Console shaders, and titles
 * that store descriptor-table pointers in memory, use them as addresses. By
 * default a shader-visible heap reports its shadow's title address (shadow.c,
 * docs/anyps5.md), which AnyPS5 resolves in the console-format copy.
 * REBOX_HEAP_VA=real reports the real GPU address of vkd3d-proton's
 * descriptors instead (patches/vkd3d-proton/0001, descriptor buffers only),
 * and =virtual vkd3d-proton's own handles. Either way the command-list slots
 * that take a GPU handle map the address back to vkd3d-proton's handle. */
struct heap_range
{
    UINT64 real, virt, size;  /* real: the address the title sees */
};

static SRWLOCK heap_ranges_lock = SRWLOCK_INIT;
static struct heap_range heap_ranges[256];
static UINT heap_range_count;

typedef HRESULT (WINAPI *heap_va_fn)(ID3D12DescriptorHeap *, UINT64 *, UINT *);
static heap_va_fn heap_va_export;

static BOOL CALLBACK find_heap_va_export(INIT_ONCE *once, void *param, void **ctx)
{
    HMODULE core = GetModuleHandleA("d3d12core.dll");

    if (shadow_heap_va_mode() == SHADOW_HEAP_VA_REAL && core)
        heap_va_export = (heap_va_fn)(void *)GetProcAddress(core, "ReboxGetDescriptorHeapVA");
    return TRUE;
}

static heap_va_fn real_heap_va(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce(&once, find_heap_va_export, NULL, NULL);
    return heap_va_export;
}

static void heap_range_add(UINT64 real, UINT64 virt, UINT64 size)
{
    UINT i;

    AcquireSRWLockExclusive(&heap_ranges_lock);
    /* A freed heap's range may be reused by a new one: drop overlaps. */
    for (i = 0; i < heap_range_count;)
    {
        struct heap_range *r = &heap_ranges[i];
        if ((real < r->real + r->size && r->real < real + size) || r->virt == virt)
            *r = heap_ranges[--heap_range_count];
        else
            i++;
    }
    if (heap_range_count < ARRAYSIZE(heap_ranges))
        heap_ranges[heap_range_count++] = (struct heap_range){ real, virt, size };
    else
        rebox_log("d3d12_xs: too many shader-visible heaps for real addresses");
    ReleaseSRWLockExclusive(&heap_ranges_lock);
}

/* Real heap address -> vkd3d-proton handle; anything else is returned as is. */
static UINT64 heap_handle_to_vkd3d(UINT64 handle)
{
    UINT64 out = handle;

    AcquireSRWLockShared(&heap_ranges_lock);
    for (UINT i = 0; i < heap_range_count; i++)
        if (handle - heap_ranges[i].real < heap_ranges[i].size)
        {
            out = heap_ranges[i].virt + (handle - heap_ranges[i].real);
            break;
        }
    ReleaseSRWLockShared(&heap_ranges_lock);
    return out;
}

/* Slot 8, GetDesc: without the hidden tail (shadow.h). */
static D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE heap_GetDesc(struct proxy *p, D3D12_DESCRIPTOR_HEAP_DESC *desc)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { 0 };

    ((D3D12_DESCRIPTOR_HEAP_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_DESCRIPTOR_HEAP_DESC *))
        PC_SLOT(p->inner, 8))(p->inner, desc);
    ((D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))
        PC_SLOT(p->inner, 9))(p->inner, &h);
    desc->NumDescriptors -= shadow_reserved(h.ptr);
    return desc;
}

/* Slots 9 and 10 return the handle in RAX on Xbox (ScarlettX, verified in a
 * retail title); vkd3d-proton uses the MSVC hidden return pointer. */
static SIZE_T heap_cpu_start(struct proxy *p)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { 0 };
    ((D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))
        PC_SLOT(p->inner, 9))(p->inner, &h);
    return h.ptr;
}

/* The title's own memory for a heap placed there (shadow_place_heap), else vkd3d-proton's start. */
static SIZE_T STDMETHODCALLTYPE heap_GetCPUDescriptorHandleForHeapStart(struct proxy *p)
{
    SIZE_T inner = heap_cpu_start(p), title = shadow_title_cpu_start(inner);
    return title ? title : inner;
}

static UINT64 STDMETHODCALLTYPE heap_GetGPUDescriptorHandleForHeapStart(struct proxy *p)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = { 0 };
    heap_va_fn heap_va = real_heap_va();
    UINT64 real;
    UINT stride;

    ((D3D12_GPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))
        PC_SLOT(p->inner, 10))(p->inner, &h);
    if (shadow_heap_va_mode() == SHADOW_HEAP_VA_TITLE)
    {
        UINT64 title, size;
        static LONG logged;

        if (!h.ptr || !shadow_title(heap_cpu_start(p), &title, &size))
            return h.ptr;
        heap_range_add(title, h.ptr, size);
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: descriptor heaps report shadow-heap title addresses (first %#llx)",
                         (unsigned long long)title);
        return title;
    }
    if (heap_va && SUCCEEDED(heap_va(INNER(p, ID3D12DescriptorHeap), &real, &stride)))
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc;
        static LONG logged;

        ((D3D12_DESCRIPTOR_HEAP_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_DESCRIPTOR_HEAP_DESC *))
            PC_SLOT(p->inner, 8))(p->inner, &desc);
        heap_range_add(real, h.ptr, (UINT64)desc.NumDescriptors * stride);
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: descriptor heaps report real GPU addresses (first %#llx, stride %u)",
                         (unsigned long long)real, stride);
        return real;
    }
    return h.ptr;
}

/* ---------------------------------------------------------------- vtables */

struct slot
{
    unsigned xbox;
    int pc;       /* forward to this PC slot, or -1 */
    void *custom; /* or call this */
};

#define FWD(x, p) { x, p, NULL }
#define FN(x, f) { x, -1, (void *)(f) }

/* REBOX_SLOT_STATS=N: count calls per slot and log the counts every N
 * presents (and at exit), to see which slots a title really uses. Each slot
 * then goes through mov rax, &counter; lock inc qword [rax]; mov rax,
 * target; jmp rax. RAX carries no argument, so the call is otherwise
 * untouched. */
static int slot_stats_interval(void)
{
    static int interval = -1;
    if (interval < 0)
    {
        const char *env = getenv("REBOX_SLOT_STATS");
        interval = env ? atoi(env) : 0;
        if (env && interval <= 0)
            interval = 600;
    }
    return interval;
}

static void *counting_thunk(LONG64 *counter, void *target)
{
    BYTE *t = thunk_alloc(32);
    UINT64 c = (UINT_PTR)counter, f = (UINT_PTR)target;

    t[0] = 0x48; t[1] = 0xb8; memcpy(t + 2, &c, 8);
    t[10] = 0xf0; t[11] = 0x48; t[12] = 0xff; t[13] = 0x00;
    t[14] = 0x48; t[15] = 0xb8; memcpy(t + 16, &f, 8);
    t[24] = 0xff; t[25] = 0xe0;
    return t;
}

static void build(struct proxy_class *cls, unsigned pc_prefix, const struct slot *slots, unsigned count)
{
    void **vtbl = calloc(cls->slots, sizeof(*vtbl));
    unsigned i;

    for (i = 0; i < cls->slots; i++)
    {
        vtbl[i] = i < pc_prefix ? forward_thunk(i) : trap_thunk(cls, i);
        cls->forwarded[i] = i < pc_prefix;
    }
    vtbl[0] = (void *)proxy_QueryInterface;
    vtbl[1] = (void *)proxy_AddRef;
    vtbl[2] = (void *)proxy_Release;
    cls->custom[0] = cls->custom[1] = cls->custom[2] = TRUE;
    for (i = 0; i < count; i++)
    {
        vtbl[slots[i].xbox] = slots[i].custom ? slots[i].custom : forward_thunk(slots[i].pc);
        cls->custom[slots[i].xbox] = slots[i].custom != NULL;
        cls->forwarded[slots[i].xbox] = !slots[i].custom;
    }
    if (slot_stats_interval())
        for (i = 0; i < cls->slots; i++)
            vtbl[i] = counting_thunk(&cls->calls[i], vtbl[i]);
    cls->vtbl = vtbl;
}

void proxy_log_slot_stats(void)
{
    char line[1024];

    if (!slot_stats_interval())
        return;
    for (unsigned c = 0; c < ARRAYSIZE(classes); c++)
    {
        struct proxy_class *cls = classes[c];
        int n = snprintf(line, sizeof(line), "slot stats %s:", cls->name);

        for (unsigned i = 3; i < cls->slots && n < (int)sizeof(line) - 40; i++)
            if (cls->calls[i])
                n += snprintf(line + n, sizeof(line) - n, " %u%s=%lld", i,
                              cls->custom[i] ? "c" : cls->forwarded[i] ? "" : "T", (long long)cls->calls[i]);
        rebox_log("d3d12_xs: %s", line);
    }
}

static void log_slot_stats_at_exit(void)
{
    proxy_log_slot_stats();
}

void proxy_frame_presented(void)
{
    static LONG64 frames;
    int interval = slot_stats_interval();

    pso_release_due();
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 12)
        {
            char line[400];
            int at = 0;
            for (UINT i = 0; i < ARRAYSIZE(display_rtvs) && display_rtvs[i] && at < 360; i++)
                at += snprintf(line + at, sizeof(line) - at, " %#llx: %ld/%ld/%ld;",
                               (unsigned long long)display_rtvs[i], InterlockedExchange(&display_binds[i], 0),
                               InterlockedExchange(&display_clears[i], 0), InterlockedExchange(&display_draws[i], 0));
            line[at] = 0;
            rebox_log("d3d12_xs: since the last present, display-sized RGBA8 targets bound/cleared/drawn:%s", line);
            console_display_draws_log();
        }
        /* Later, every 100 presents: the distinct draws since the last of these (a frame can change
         * long after the first presents). */
        else if (!(logged % 100))
        {
            rebox_log("d3d12_xs: present %ld: draws into display-sized targets since present %ld:", logged, logged - 100);
            console_display_draws_log();
        }
    }
    if (interval && !(InterlockedIncrement64(&frames) % interval))
        proxy_log_slot_stats();
}

static BOOL CALLBACK init_vtables(INIT_ONCE *once, void *param, void **ctx)
{
    /* Device: PC through slot 43 (original ID3D12Device). */
    static const struct slot device_slots[] = {
        FN(8, device_CreateCommandQueue),
        FN(9, device_CreateCommandAllocator),
        FN(36, device_CreateFence),
        FN(10, device_CreateGraphicsPipelineState),
        FN(11, device_CreateComputePipelineState),
        FN(12, device_CreateCommandList),
        FN(14, device_CreateDescriptorHeap),
        FN(17, device_CreateConstantBufferView),
        FN(18, device_CreateShaderResourceView),
        FN(19, device_CreateUnorderedAccessView),
        FN(20, device_CreateRenderTargetView),
        FN(21, device_CreateDepthStencilView),
        FN(22, device_CreateSampler),
        FN(23, device_CopyDescriptors),
        FN(24, device_CopyDescriptorsSimple),
        FN(25, device_GetResourceAllocationInfo),
        FN(26, device_GetCustomHeapProperties),
        FN(27, device_CreateCommittedResource),
        FN(28, device_CreateHeap),
        FN(29, device_CreatePlacedResource),
        FN(30, device_CreateReservedResource),
        FN(38, device_GetCopyableFootprints),
        FN(41, device_CreateCommandSignature),
        FWD(85, 46),                                  /* SetResidencyPriority */
        FN(86, device_CreatePipelineState),
        FN(101, device_CreateStateObject),
        FWD(102, 63),                                 /* GetRaytracingAccelerationStructurePrebuildInfo */
        FWD(103, 64),                                 /* CheckDriverMatchingIdentifier */
        FN(115, device_DeserializeStateObjectX),
        FN(43, device_GetAdapterLuid),
        FN(44, device_CreatePlacedResourceX),
        /* Slot 133, SDK 0x20004: the same arguments (the target title places a buffer over its
         * physical pages right after mapping them, and stops if it fails). The device vtable used to
         * end at 128, so this call jumped through whatever followed it. */
        FN(133, device_CreatePlacedResourceLayoutX),
        FN(127, device_CreateCommittedResourceLayoutX),
        FN(70, device_CreateShaderResourceViewAtX),
        FN(108, device_GetResourceAllocationInfo2X),
        FN(71, device_CreateUnorderedAccessViewAtX),
        FN(48, device_CreateCommandQueue), /* CreateCommandQueueX */
        FN(13, device_CheckFeatureSupport),
        FN(55, device_CreateDerivedPipelineStateX),
        FN(60, device_CreateGraphicsPipelineStateX),
        FN(61, device_CreateSamplerX),
        FN(62, device_CreateComputePipelineStateX),
        FN(68, device_Slot68),
        FN(69, device_Slot69),
        FN(73, device_CreateCommandListX),
        FN(74, device_RegisterPagePoolX),
        FN(75, device_UnregisterPagePoolX),
        FN(76, device_CreateCommittedResourceX),
        FN(78, device_SetFrameIntervalX),
        FN(79, device_ScheduleFrameEventX),
        FN(80, device_WaitFrameEventX),
        FN(81, device_GetFrameStatisticsX),
    };
    /* Queue: PC 0-17, then console methods 18-25 with SignalX at 23 (XValkyrie; a UE5 title, SDK
     * 0x20002, calls it with (fence, value, flush flags)), GetDesc at 26, PresentX at 30. */
    static const struct slot queue_slots[] = {
        FN(7, child_GetDevice),
        FN(10, queue_ExecuteCommandLists),
        FN(14, queue_Signal),
        FN(15, queue_Wait),
        FN(16, queue_Slot16),
        FN(17, queue_Slot17),
        FN(18, queue_Slot18),
        FN(25, queue_Slot25),
        FN(32, queue_Slot32),
        FN(23, queue_SignalX),
        FN(26, queue_GetDesc),
        FN(30, queue_PresentX),
        FN(35, queue_ExecuteCommandListsX),
    };
    /* Queue for titles built against SDK 0x20004 or later (ScarlettX 1e5473d, from a title's
     * calls): two methods were added ahead of Signal, so Signal is 16 and PresentX 32. Where they
     * were added isn't known, so 11-15 (PC's markers, then the two new ones) trap, and only the
     * slots a title was seen calling, plus Wait after Signal, are mapped. */
    static const struct slot queue_v4_slots[] = {
        FN(7, child_GetDevice),
        FN(10, queue_ExecuteCommandLists),
        FN(16, queue_Signal),
        FN(17, queue_Wait), /* assumed to follow Signal */
        FN(25, queue_SignalX), /* inferred: 23 + 2, as PresentX 30 + 2 */
        FN(28, queue_GetDesc), /* inferred: 26 + 2 */
        FN(32, queue_PresentX),
    };
    /* Graphics list: PC through ResourceBarrier (26), then PC + 4 for PC
     * slots 28-58 (both sources agree at 32-48, 50-52 and 60-62). */
    /* Graphics list for older SDKs. XValkyrie's June 2020 kit tables (the era of a UE5 title's
     * SDK 0x20002) put PC's four query methods (52-55) at 28-31, which a UE5 title calls as
     * BeginQuery(heap, 0, 0) and EndQuery/ResolveQueryData(heap, 2 timestamp, 2), and name 67
     * FlushPipelineX and 68 OMSetDepthBoundsX, which have nothing to do here. The same tables
     * have PC's SetMarker, BeginEvent, EndEvent and ExecuteIndirect in place at 56-59,
     * DispatchX (78) and CopyResourceX (84) as Dispatch and CopyResource with a flags argument,
     * PC 60-76 at 105-121, and the shading rate methods and DispatchMesh at 124, 125 and 127
     * (a UE5 title calls 124 as RSSetShadingRate(1x1, combiners)). 60 and 62 are console methods. */
    static struct slot list_slots[96] = {
        FN(7, child_GetDevice),
        FN(59, list_ExecuteIndirect),                 /* PC 59, in place (Unreal 5 calls it so) */
        FN(20, list_IASetPrimitiveTopology),
        FN(47, list_IASetIndexBuffer), FN(48, list_IASetVertexBuffers), /* PC 43, 44 */
        FN(117, list_BuildRaytracingAccelerationStructure),            /* PC 72 */
        FN(118, list_EmitRaytracingAccelerationStructurePostbuildInfo), /* PC 73 */
        FN(119, list_CopyRaytracingAccelerationStructure),             /* PC 74 */
        FN(120, list_SetPipelineState1),              /* PC 75 (Unreal 5: 54 calls in a run) */
        FN(121, list_DispatchRays),                   /* PC 76 */
        FN(10, list_Reset),
        FN(12, list_DrawInstanced), FN(13, list_DrawIndexedInstanced),
        FN(50, list_OMSetRenderTargets), FN(52, list_ClearRenderTargetView),
        FN(26, list_ResourceBarrier),
        FWD(28, 52), FWD(29, 53), FWD(30, 54), FWD(31, 55), /* BeginQuery .. SetPredication */
        FN(67, list_Ignore),                          /* FlushPipelineX */
        FN(68, list_Ignore),                          /* OMSetDepthBoundsX */
        FN(69, list_Write32BitValueBottomOfPipeX),    /* xcl */
        FN(70, list_Write64BitValueBottomOfPipeX),
        FN(71, list_Wait32BitValueX),
        FN(72, list_Wait64BitValueX),
        FN(88, list_Write32BitValueTopOfPipeX),
        FN(89, list_Write64BitValueTopOfPipeX),
        FN(90, list_Write32BitValueBottomOfPipeX),    /* Write32BitValueBottomOfShaderX */
        FN(32, list_SetDescriptorHeaps),
        FN(35, list_SetComputeRootDescriptorTable),   /* PC 31 */
        FN(36, list_SetGraphicsRootDescriptorTable),  /* PC 32 */
        FN(53, list_ClearUnorderedAccessViewUint),    /* PC 49 */
        FN(54, list_ClearUnorderedAccessViewFloat),   /* PC 50 */
        FN(61, list_Slot61),                          /* PC 59 or 57 */
        FN(16, list_CopyTextureRegion),
        FN(17, list_CopyResource),
        FN(83, list_CopyTextureRegionX),
        FN(84, list_CopyResourceX),
        FN(146, list_Barrier),
        FN(148, list_AllocateEmbeddedX),
    };
    /* Graphics list for titles built against SDK 0x20004 or later (ScarlettX 1e5473d): two
     * methods were added at 59 and 60, so every method from 61 on is the 0x20001 one two slots
     * later. The 0x20001 positions are XValkyrie's (a June 2020 kit's header tables): 56-59 PC
     * 56-59, 69-90 console memory writes and waits, 83 CopyTextureRegionX, 105-121 PC 60-76,
     * 124/125 shading rate, 127 DispatchMesh. A newer title's own DLLs fit this layout: FSR's
     * ExecuteIndirect at 61, the BVH builder's write/wait calls at 71, 73 and 90, and 141-148
     * past the end of the older table. docs/d3d12x.md. */
    static struct slot list_v4_slots[96] = {
        FN(7, child_GetDevice),
        FN(59, list_ExecuteIndirect),                 /* PC 59 in place too (Unreal 5) */
        FN(20, list_IASetPrimitiveTopology),
        FN(47, list_IASetIndexBuffer), FN(48, list_IASetVertexBuffers), /* PC 43, 44 */
        FN(119, list_BuildRaytracingAccelerationStructure),            /* PC 72 */
        FN(120, list_EmitRaytracingAccelerationStructurePostbuildInfo), /* PC 73 */
        FN(121, list_CopyRaytracingAccelerationStructure),             /* PC 74 */
        FN(122, list_SetPipelineState1),              /* PC 75 */
        FN(71, list_Write32BitValueBottomOfPipeX),    /* the older 69-72 and 88-90, two later (xcl) */
        FN(72, list_Write64BitValueBottomOfPipeX),
        FN(73, list_Wait32BitValueX),
        FN(74, list_Wait64BitValueX),
        FN(90, list_Write32BitValueTopOfPipeX),
        FN(91, list_Write64BitValueTopOfPipeX),
        FN(92, list_Write32BitValueBottomOfPipeX),
        FN(123, list_DispatchRays),                   /* PC 76 */
        FN(10, list_Reset),
        FN(12, list_DrawInstanced), FN(13, list_DrawIndexedInstanced),
        FN(50, list_OMSetRenderTargets), FN(52, list_ClearRenderTargetView),
        FN(26, list_ResourceBarrier),
        FN(32, list_SetDescriptorHeaps),
        FN(35, list_SetComputeRootDescriptorTable),   /* PC 31 */
        FN(36, list_SetGraphicsRootDescriptorTable),  /* PC 32 */
        FN(53, list_ClearUnorderedAccessViewUint),    /* PC 49 */
        FN(54, list_ClearUnorderedAccessViewFloat),   /* PC 50 */
        FWD(56, 56), FWD(57, 57), FWD(58, 58),        /* SetMarker, BeginEvent, EndEvent */
        FN(61, list_Slot61),                          /* ExecuteIndirect (PC 59) */
        FN(85, list_CopyTextureRegionX),
        FN(139, list_ResetX),
        FN(146, list_Barrier),
        FN(148, list_AllocateEmbeddedX),
    };
    /* Copy-queue lists have their own short layout (ScarlettX). */
    static const struct slot copy_slots[] = {
        FN(7, child_GetDevice),
        FN(10, list_Reset),
        FWD(11, 15), /* CopyBufferRegion */
        FN(12, list_CopyTextureRegion),
        FN(15, list_ResourceBarrier),
    };
    static const struct slot heap_slots[] = {
        FN(7, child_GetDevice),
        FN(8, heap_GetDesc),
        FN(9, heap_GetCPUDescriptorHandleForHeapStart),
        FN(10, heap_GetGPUDescriptorHandleForHeapStart),
    };
    unsigned n = 0, customs;

    if (slot_stats_interval())
        atexit(log_slot_stats_at_exit);
    while (n < ARRAYSIZE(list_slots) && list_slots[n].xbox)
        n++;
    customs = n;
    for (unsigned x = 33; x <= 59; x++)
    {
        BOOL taken = FALSE;
        for (unsigned j = 0; j < customs; j++)
            taken |= list_slots[j].xbox == x;
        if (!taken)
            list_slots[n++] = (struct slot)FWD(x, x < 56 ? x - 4 : x);
    }
    list_slots[n++] = (struct slot)FWD(78, 14);  /* DispatchX */
    for (unsigned pc = 60; pc <= 76; pc++)
    {
        BOOL taken = FALSE;
        for (unsigned j = 0; j < customs; j++)
            taken |= list_slots[j].xbox == pc + 45;
        if (!taken)
            list_slots[n++] = (struct slot)FWD(pc + 45, pc);
    }
    list_slots[n++] = (struct slot)FWD(124, 77); /* RSSetShadingRate */
    list_slots[n++] = (struct slot)FWD(125, 78); /* RSSetShadingRateImage */
    list_slots[n++] = (struct slot)FWD(127, 79); /* DispatchMesh */

    build(&device_class, 44, device_slots, ARRAYSIZE(device_slots));
    build(&queue_class, 18, queue_slots, ARRAYSIZE(queue_slots));
    build(&queue_v4_class, 11, queue_v4_slots, ARRAYSIZE(queue_v4_slots));
    build(&list_class, 27, list_slots, n);

    n = 0;
    while (n < ARRAYSIZE(list_v4_slots) && list_v4_slots[n].xbox)
        n++;
    customs = n;
    for (unsigned x = 33; x <= 55; x++)
    {
        BOOL taken = FALSE;
        for (unsigned j = 0; j < customs; j++)
            taken |= list_v4_slots[j].xbox == x;
        if (!taken)
            list_v4_slots[n++] = (struct slot)FWD(x, x - 4);
    }
    for (unsigned pc = 60; pc <= 76; pc++)          /* PC 60-76: 105-121 before, 107-123 here */
    {
        BOOL taken = FALSE;
        for (unsigned j = 0; j < customs; j++)
            taken |= list_v4_slots[j].xbox == pc + 47;
        if (!taken)
            list_v4_slots[n++] = (struct slot)FWD(pc + 47, pc);
    }
    list_v4_slots[n++] = (struct slot)FWD(126, 77); /* RSSetShadingRate */
    list_v4_slots[n++] = (struct slot)FWD(127, 78); /* RSSetShadingRateImage */
    list_v4_slots[n++] = (struct slot)FWD(129, 79); /* DispatchMesh */
    build(&list_v4_class, 27, list_v4_slots, n);
    build(&copy_list_class, 11, copy_slots, ARRAYSIZE(copy_slots));
    build(&heap_class, 9, heap_slots, ARRAYSIZE(heap_slots));
    {
        static const struct slot allocator_slots[] = {
            FN(7, child_GetDevice),
            FN(9, allocator_SetAllocationCallbackX),
        };
        build(&allocator_class, 9, allocator_slots, ARRAYSIZE(allocator_slots));
    }
    {
        static const struct slot fence_slots[] = {
            FN(7, child_GetDevice),
            FN(10, fence_Signal),
            FN(12, fence_CompletedValueAddressX),
        };
        build(&fence_class, 12, fence_slots, ARRAYSIZE(fence_slots));
    }
    return TRUE;
}

ID3D12GraphicsCommandList *proxy_list(void *list, ID3D12DescriptorHeap **heaps, UINT *heap_count)
{
    struct proxy *p = list;

    *heap_count = 0;
    if (!is_proxy(list))
        return list;
    if (p->cls != &list_class && p->cls != &list_v4_class && p->cls != &copy_list_class)
        return NULL;
    *heap_count = p->heap_count;
    memcpy(heaps, p->heaps, p->heap_count * sizeof(*heaps));
    return INNER(p, ID3D12GraphicsCommandList);
}

HRESULT proxy_query_device(ID3D12Device *inner, REFIID riid, void **out)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static SRWLOCK lock = SRWLOCK_INIT;

    InitOnceExecuteOnce(&once, init_vtables, NULL, NULL);
    AcquireSRWLockExclusive(&lock);
    if (!device_proxy)
    {
        ID3D12Device_AddRef(inner);
        device_proxy = proxy_create((IUnknown *)inner, &device_class);
        /* The console device's flags byte at +0x18 (XValkyrie, docs/research/d3d12x.md; Xbox One
         * and Series): when it's zero, title code writes CBV descriptors and copies
         * CBV_SRV_UAV/RTV/DSV descriptors itself, 16 bytes at a time, into the CPU handle. Ours
         * are vkd3d-proton's, so those writes would corrupt its heaps and skip the shadow heaps;
         * nonzero makes titles call slots 17 and 24. */
        ((BYTE *)device_proxy)[DEVICE_OUT_OF_LINE_FLAG] = 1;
        patch_child_classes(inner);
    }
    ReleaseSRWLockExclusive(&lock);
    if (device_proxy->inner != (IUnknown *)inner)
        return E_FAIL;
    return proxy_QueryInterface(device_proxy, riid, out);
}
