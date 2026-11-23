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
#include "pacing.h"
#include "present.h"
#include "proxy.h"
#include "title_textures.h"
#include "dxgi_device.h"
#include "derived_pso.h"

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
};

C_ASSERT(offsetof(struct proxy, inner) == INNER_OFFSET);

static struct proxy_class device_class = { "Device", 128 };
static struct proxy_class queue_class = { "CommandQueue", 64 };
static struct proxy_class queue_v4_class = { "CommandQueue (SDK 0x20004)", 64 };
static struct proxy_class list_class = { "GraphicsCommandList", MAX_SLOTS };
static struct proxy_class list_v4_class = { "GraphicsCommandList (SDK 0x20004)", MAX_SLOTS };
static struct proxy_class copy_list_class = { "CopyCommandList", 64 };
static struct proxy_class heap_class = { "DescriptorHeap", 16 };

static struct proxy_class *const classes[] = { &device_class, &queue_class, &queue_v4_class, &list_class, &list_v4_class,
                                               &copy_list_class,
                                               &heap_class };

/* D3D12XBOX_CREATE_DEVICE_PARAMETERS.Version the title passed (0 if none). */
static UINT32 sdk_version;

void proxy_set_sdk_version(UINT32 version)
{
    sdk_version = version;
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

static UINT64 STDMETHODCALLTYPE resource_GetGPUVirtualAddress(void *self)
{
    UINT64 va = orig_resource_get_va(self);
    D3D12_RESOURCE_ALLOCATION_INFO info;
    D3D12_RESOURCE_DESC desc;
    struct scratch *sc;
    void *cpu = NULL;
    UINT64 size;
    static LONG logged;

    if (va || !device_proxy)
        return va;
    ((D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_DESC *))(*(void ***)self)[10])(self, &desc);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return 0;

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

static void patch_resource_class(void *resource)
{
    void **vtbl = *(void ***)resource;
    DWORD old;

    if (vtbl[11] == (void *)resource_GetGPUVirtualAddress)
        return;
    VirtualProtect(vtbl, 12 * sizeof(void *), PAGE_READWRITE, &old);
    resource_vtbl = vtbl;
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
    VirtualProtect(vtbl, 12 * sizeof(void *), old, &old);
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
    PROBE(ID3D12Device_CreateHeap(device, &heap_desc, &IID_ID3D12Heap, (void **)&obj));
    PROBE(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&obj));
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
    if (FAILED(hr))
        return hr;
    if (!inner)
        return E_FAIL;
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
    return wrap_result(hr, (IUnknown *)queue, sdk_version >= 0x20004 ? &queue_v4_class : &queue_class, out);
}

static HRESULT create_list(struct proxy *p, UINT type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso,
        void **out)
{
    D3D12_COMMAND_LIST_TYPE t = pc_list_type(type);
    ID3D12GraphicsCommandList *list = NULL;

    HRESULT hr = ID3D12Device_CreateCommandList(INNER(p, ID3D12Device), 0, t, alloc, pso,
                                                &IID_ID3D12GraphicsCommandList, (void **)&list);

    return wrap_result(hr, (IUnknown *)list, t == D3D12_COMMAND_LIST_TYPE_COPY ? &copy_list_class
                       : sdk_version >= 0x20004 ? &list_v4_class : &list_class, out);
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
        shadow_heap_created(heap, desc, ID3D12Device_GetDescriptorHandleIncrementSize(INNER(p, ID3D12Device),
                            desc->Type), reserved);
    return wrap_result(hr, (IUnknown *)heap, &heap_class, out);
}

/* Descriptor writes (slots 17-19, 22-24, as on PC) also write the console
 * format into the shadow heaps (shadow.c). */
static void STDMETHODCALLTYPE device_CreateConstantBufferView(struct proxy *p,
        const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    ID3D12Device_CreateConstantBufferView(INNER(p, ID3D12Device), desc, h);
    shadow_write_cbv(cpu, desc);
}

static void STDMETHODCALLTYPE device_CreateShaderResourceView(struct proxy *p, ID3D12Resource *resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    ID3D12Device_CreateShaderResourceView(INNER(p, ID3D12Device), resource, desc, h);
    shadow_write_srv(cpu, resource, desc);
}

static void STDMETHODCALLTYPE device_CreateUnorderedAccessView(struct proxy *p, ID3D12Resource *resource,
        ID3D12Resource *counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    ID3D12Device_CreateUnorderedAccessView(INNER(p, ID3D12Device), resource, counter, desc, h);
    shadow_write_uav(cpu, resource, desc);
}

static void STDMETHODCALLTYPE device_CreateSampler(struct proxy *p, const D3D12_SAMPLER_DESC *desc, SIZE_T cpu)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu };
    ID3D12Device_CreateSampler(INNER(p, ID3D12Device), desc, h);
    shadow_write_sampler(cpu, desc);
}

static void STDMETHODCALLTYPE device_CopyDescriptors(struct proxy *p, UINT dst_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *dst_starts, const UINT *dst_sizes, UINT src_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE *src_starts, const UINT *src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    ID3D12Device_CopyDescriptors(INNER(p, ID3D12Device), dst_count, dst_starts, dst_sizes, src_count, src_starts,
                                 src_sizes, type);
    shadow_copy_ranges(dst_count, dst_starts, dst_sizes, src_count, src_starts, src_sizes);
}

static void STDMETHODCALLTYPE device_CopyDescriptorsSimple(struct proxy *p, UINT count, SIZE_T dst, SIZE_T src,
        D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    D3D12_CPU_DESCRIPTOR_HANDLE d = { dst }, s = { src };
    ID3D12Device_CopyDescriptorsSimple(INNER(p, ID3D12Device), count, d, s, type);
    shadow_copy(dst, src, count);
}

/* Command signatures the title creates (slot 41, as on PC), so list slot 61 can tell
 * ExecuteIndirect from BeginEvent. */
static SRWLOCK signatures_lock = SRWLOCK_INIT;
static void **signatures;
static UINT signature_count, signature_capacity;

static BOOL is_command_signature(void *object)
{
    BOOL found = FALSE;
    AcquireSRWLockShared(&signatures_lock);
    for (UINT i = 0; i < signature_count && !found; i++)
        found = signatures[i] == object;
    ReleaseSRWLockShared(&signatures_lock);
    return found;
}

static HRESULT STDMETHODCALLTYPE device_CreateCommandSignature(struct proxy *p, const D3D12_COMMAND_SIGNATURE_DESC *desc,
        ID3D12RootSignature *rs, REFIID riid, void **out)
{
    HRESULT hr = ID3D12Device_CreateCommandSignature(INNER(p, ID3D12Device), desc, rs, riid, out);

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
        ReleaseSRWLockExclusive(&signatures_lock);
    }
    return hr;
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
 * FidelityFX upscaler's recurrent texture uses 0x101 (Xbox2Windows,
 * docs/experiments.md). They select console tilings vkd3d-proton doesn't
 * know, so they become UNKNOWN, the driver's choice; titles that address
 * texels themselves go through scratch GPU VAs instead. Each distinct
 * value is logged once. */
static const D3D12_RESOURCE_DESC *pc_desc(const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_DESC *storage)
{
    static LONG seen[8];

    if (!desc || desc->Layout < 0x100)
        return desc;
    if (!InterlockedExchange(&seen[desc->Layout & 7], 1))
        rebox_log("d3d12_xs: Xbox texture layout %#x (%ux%u, format %u) treated as UNKNOWN", desc->Layout,
                     (UINT)desc->Width, desc->Height, desc->Format);
    *storage = *desc;
    storage->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return storage;
}

/* Slot 25, through the hidden return pointer as on PC (16-byte result). */
static D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE device_GetResourceAllocationInfo(struct proxy *p,
        D3D12_RESOURCE_ALLOCATION_INFO *ret, UINT visible_mask, UINT count, const D3D12_RESOURCE_DESC *descs)
{
    typedef D3D12_RESOURCE_ALLOCATION_INFO *(STDMETHODCALLTYPE *fn)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT,
            UINT, const D3D12_RESOURCE_DESC *);
    D3D12_RESOURCE_DESC *copy = NULL;

    if (!is_proxy(p) && is_proxy(ret)) /* console order: (result, device, ...) */
    {
        struct proxy *device = (struct proxy *)ret;
        ret = (D3D12_RESOURCE_ALLOCATION_INFO *)p;
        p = device;
    }

    for (UINT i = 0; descs && i < count; i++)
        if (descs[i].Layout >= 0x100)
        {
            if (!copy && (copy = malloc(count * sizeof(*copy))))
                memcpy(copy, descs, count * sizeof(*copy));
            if (copy)
                pc_desc(&descs[i], &copy[i]);
        }
    ((fn)PC_SLOT(p->inner, 25))(p->inner, ret, visible_mask, count, copy ? copy : descs);
    free(copy);
    return ret;
}

/* Slot 26: GetCustomHeapProperties, either argument order (see resource_GetDesc). */
static D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE device_GetCustomHeapProperties(struct proxy *p,
        D3D12_HEAP_PROPERTIES *ret, UINT node_mask, D3D12_HEAP_TYPE type)
{
    typedef D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *fn)(void *, D3D12_HEAP_PROPERTIES *, UINT, D3D12_HEAP_TYPE);

    if (!is_proxy(p) && is_proxy(ret))
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
    HRESULT hr;

    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        props = buffer_heap(props, &storage);
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
    hr = ID3D12Device_CreateCommittedResource(INNER(p, ID3D12Device), props, flags, desc, state, clear, riid, out);
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
    HRESULT hr = ID3D12Device_CreatePlacedResource(INNER(p, ID3D12Device), heap, offset, pc_desc(desc, &storage),
                                                   state, clear, riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateReservedResource(struct proxy *p, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12_RESOURCE_DESC storage;
    HRESULT hr = ID3D12Device_CreateReservedResource(INNER(p, ID3D12Device), pc_desc(desc, &storage), state, clear,
                                                     riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

/* PSO creation (slots 10, 11): forwarded; the first PSO patches the PSO
 * class's GetDevice. */
static HRESULT STDMETHODCALLTYPE device_CreateGraphicsPipelineState(struct proxy *p,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(INNER(p, ID3D12Device), desc, riid, out);
    if (SUCCEEDED(hr) && out)
    {
        patch_child(*out);
        derived_pso_record(*out, desc);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateComputePipelineState(struct proxy *p,
        const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **out)
{
    HRESULT hr = ID3D12Device_CreateComputePipelineState(INNER(p, ID3D12Device), desc, riid, out);
    if (SUCCEEDED(hr) && out)
        patch_child(*out);
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
 * address, so the title and the GPU share it. Anything else gets its own memory and is logged
 * once per kind: textures (the chunk heaps hold buffers only, and console tilings aren't
 * modelled), addresses outside GPU memory, and placements the heap refuses. */
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
    if (d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        why = 0;
    else if (!gpu_memory_heap_at(address, &heap, &offset))
        why = 1;
    else
    {
        hr = ID3D12Device_CreatePlacedResource(INNER(p, ID3D12Device), heap, offset, d, state, clear, riid, out);
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
    return pacing_wait(type, timeout_ms, token);
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatisticsX(struct proxy *p, UINT64 token, UINT types, UINT *count,
        void *statistics)
{
    return pacing_frame_statistics(token, types, count, statistics);
}

/* ------------------------------------------------------------------ queue */

static void STDMETHODCALLTYPE queue_ExecuteCommandLists(struct proxy *p, UINT count, ID3D12CommandList *const *lists)
{
    ID3D12CommandList *stack[64] = { 0 }, **raw = count <= ARRAYSIZE(stack) ? stack : malloc(count * sizeof(*raw));

    for (UINT i = 0; i < count; i++)
        raw[i] = proxy_unwrap(lists[i]);
    /* Placed textures whose title memory changed are uploaded first (title_textures.c). */
    title_textures_upload(INNER(p, ID3D12CommandQueue));
    ID3D12CommandQueue_ExecuteCommandLists(INNER(p, ID3D12CommandQueue), count, raw);
    if (raw != stack)
        free(raw);
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
            signalled[i].fence = fence;
            signalled[i].value = value;
            signalled[i].signals++;
            break;
        }
    ReleaseSRWLockExclusive(&signalled_lock);
}

static HRESULT STDMETHODCALLTYPE queue_SignalX(struct proxy *p, ID3D12Fence *fence, UINT64 value, UINT flags)
{
    note_signal(proxy_unwrap(fence), value);
    return ID3D12CommandQueue_Signal(INNER(p, ID3D12CommandQueue), proxy_unwrap(fence), value);
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
    if (!is_proxy(p) && is_proxy(ret))
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
    D3D12_CPU_DESCRIPTOR_HANDLE c = { cpu };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(INNER(p, ID3D12GraphicsCommandList), g, c, resource,
            values, rect_count, rects);
}

static void STDMETHODCALLTYPE list_ClearUnorderedAccessViewFloat(struct proxy *p, UINT64 gpu, SIZE_T cpu,
        ID3D12Resource *resource, const float values[4], UINT rect_count, const D3D12_RECT *rects)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g = { heap_handle_to_vkd3d(gpu) };
    D3D12_CPU_DESCRIPTOR_HANDLE c = { cpu };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(INNER(p, ID3D12GraphicsCommandList), g, c, resource,
            values, rect_count, rects);
}

/* Barriers are forwarded and also feed PresentX's resource-state tracking:
 * graphics slot 26 / copy slot 15 (PC 26), and enhanced barriers at slot 146
 * (PC 80, ID3D12GraphicsCommandList7::Barrier). */
static void STDMETHODCALLTYPE list_ResourceBarrier(struct proxy *p, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
{
    present_track_barriers(count, barriers);
    ID3D12GraphicsCommandList_ResourceBarrier(INNER(p, ID3D12GraphicsCommandList), count, barriers);
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
static void STDMETHODCALLTYPE list_Ignore(struct proxy *p)
{
}

static HRESULT STDMETHODCALLTYPE list_Reset(struct proxy *p, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso)
{
    struct embedded **e, *keep = NULL, *drop = NULL;

    p->resets++;
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
    return ID3D12GraphicsCommandList_Reset(INNER(p, ID3D12GraphicsCommandList), alloc, pso);
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
static SIZE_T STDMETHODCALLTYPE heap_GetCPUDescriptorHandleForHeapStart(struct proxy *p)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = { 0 };
    ((D3D12_CPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_CPU_DESCRIPTOR_HANDLE *))
        PC_SLOT(p->inner, 9))(p->inner, &h);
    return h.ptr;
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

        if (!h.ptr || !shadow_title(heap_GetCPUDescriptorHandleForHeapStart(p), &title, &size))
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

    if (interval && !(InterlockedIncrement64(&frames) % interval))
        proxy_log_slot_stats();
}

static BOOL CALLBACK init_vtables(INIT_ONCE *once, void *param, void **ctx)
{
    /* Device: PC through slot 43 (original ID3D12Device). */
    static const struct slot device_slots[] = {
        FN(8, device_CreateCommandQueue),
        FN(10, device_CreateGraphicsPipelineState),
        FN(11, device_CreateComputePipelineState),
        FN(12, device_CreateCommandList),
        FN(14, device_CreateDescriptorHeap),
        FN(17, device_CreateConstantBufferView),
        FN(18, device_CreateShaderResourceView),
        FN(19, device_CreateUnorderedAccessView),
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
        FN(43, device_GetAdapterLuid),
        FN(44, device_CreatePlacedResourceX),
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
        FN(23, queue_SignalX),
        FN(26, queue_GetDesc),
        FN(30, queue_PresentX),
    };
    /* Queue for titles built against SDK 0x20004 or later (ScarlettX 1e5473d, from a title's
     * calls): two methods were added ahead of Signal, so Signal is 16 and PresentX 32. Where they
     * were added isn't known, so 11-15 (PC's markers, then the two new ones) trap, and only the
     * slots a title was seen calling, plus Wait after Signal, are mapped. */
    static const struct slot queue_v4_slots[] = {
        FN(7, child_GetDevice),
        FN(10, queue_ExecuteCommandLists),
        FWD(16, 14), /* Signal */
        FWD(17, 15), /* Wait, assumed to follow Signal */
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
        FN(10, list_Reset),
        FN(26, list_ResourceBarrier),
        FWD(28, 52), FWD(29, 53), FWD(30, 54), FWD(31, 55), /* BeginQuery .. SetPredication */
        FN(67, list_Ignore),                          /* FlushPipelineX */
        FN(68, list_Ignore),                          /* OMSetDepthBoundsX */
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
     * 124/125 shading rate, 127 DispatchMesh. the target title's own DLLs fit this layout: FSR's
     * ExecuteIndirect at 61, the BVH builder's write/wait calls at 71, 73 and 90, and 141-148
     * past the end of the older table. docs/d3d12x.md. */
    static struct slot list_v4_slots[96] = {
        FN(7, child_GetDevice),
        FN(10, list_Reset),
        FN(26, list_ResourceBarrier),
        FN(32, list_SetDescriptorHeaps),
        FN(35, list_SetComputeRootDescriptorTable),   /* PC 31 */
        FN(36, list_SetGraphicsRootDescriptorTable),  /* PC 32 */
        FN(53, list_ClearUnorderedAccessViewUint),    /* PC 49 */
        FN(54, list_ClearUnorderedAccessViewFloat),   /* PC 50 */
        FWD(56, 56), FWD(57, 57), FWD(58, 58),        /* SetMarker, BeginEvent, EndEvent */
        FN(61, list_Slot61),                          /* ExecuteIndirect (PC 59) */
        FN(85, list_CopyTextureRegionX),
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
        list_slots[n++] = (struct slot)FWD(pc + 45, pc);
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
        list_v4_slots[n++] = (struct slot)FWD(pc + 47, pc);
    list_v4_slots[n++] = (struct slot)FWD(126, 77); /* RSSetShadingRate */
    list_v4_slots[n++] = (struct slot)FWD(127, 78); /* RSSetShadingRateImage */
    list_v4_slots[n++] = (struct slot)FWD(129, 79); /* DispatchMesh */
    build(&list_v4_class, 27, list_v4_slots, n);
    build(&copy_list_class, 11, copy_slots, ARRAYSIZE(copy_slots));
    build(&heap_class, 9, heap_slots, ARRAYSIZE(heap_slots));
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
