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
#include "present.h"
#include "proxy.h"

#define INNER_OFFSET 0x1000
#define MAX_SLOTS 160

struct proxy_class
{
    const char *name;
    unsigned slots;
    void **vtbl;
    LONG trapped[MAX_SLOTS];
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
static struct proxy_class list_class = { "GraphicsCommandList", MAX_SLOTS };
static struct proxy_class copy_list_class = { "CopyCommandList", 64 };
static struct proxy_class heap_class = { "DescriptorHeap", 16 };

static struct proxy_class *const classes[] = { &device_class, &queue_class, &list_class, &copy_list_class, &heap_class };

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

static HRESULT STDMETHODCALLTYPE proxy_QueryInterface(struct proxy *p, REFIID riid, void **out)
{
    IUnknown *result;
    HRESULT hr;

    if (!out)
        return E_POINTER;
    *out = NULL;
    /* vkd3d-proton returns the same pointer for every version of an
     * interface (and for IUnknown/ID3D12Object/ID3D12DeviceChild), so "same
     * object back" means the proxy can answer for it. */
    if (FAILED(hr = IUnknown_QueryInterface(p->inner, riid, (void **)&result)))
        return hr;
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

/* ------------------------------------------ texture GPU VAs (mitigation)
 *
 * On the console a texture's GPU VA is the address of its memory, and titles
 * write texel data through it (ScarlettX saw this in a retail title).
 * vkd3d-proton returns 0 for textures, so such a write would crash. Until
 * textures are backed by identity memory with detiling, each texture that is
 * asked gets a scratch block of GPU memory its size: writes land
 * harmlessly, the texture doesn't see them, and it is logged once. The block
 * is freed with the texture. */

typedef UINT64 (STDMETHODCALLTYPE *get_va_fn)(void *);
typedef ULONG (STDMETHODCALLTYPE *release_fn)(void *);

static get_va_fn orig_resource_get_va;
static release_fn orig_resource_release;

static struct scratch
{
    void *resource, *cpu;
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
    if (FAILED(ReboxGpuAlloc(info.SizeInBytes ? info.SizeInBytes : 65536, 64 << 10, &cpu, NULL)))
        return 0;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("d3d12_xs: title asked for a texture's GPU VA (%ux%u, format %u); giving scratch memory: "
                     "texel writes through it are not seen by the texture yet", (UINT)desc.Width, desc.Height,
                     desc.Format);
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

static void patch_resource_class(void *resource)
{
    void **vtbl = *(void ***)resource;
    DWORD old;

    if (vtbl[11] == (void *)resource_GetGPUVirtualAddress)
        return;
    VirtualProtect(vtbl, 12 * sizeof(void *), PAGE_READWRITE, &old);
    orig_resource_release = vtbl[2];
    orig_resource_get_va = vtbl[11];
    vtbl[2] = (void *)resource_Release;
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
    return wrap_result(hr, (IUnknown *)queue, &queue_class, out);
}

static HRESULT create_list(struct proxy *p, UINT type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso,
        void **out)
{
    D3D12_COMMAND_LIST_TYPE t = pc_list_type(type);
    ID3D12GraphicsCommandList *list = NULL;

    HRESULT hr = ID3D12Device_CreateCommandList(INNER(p, ID3D12Device), 0, t, alloc, pso,
                                                &IID_ID3D12GraphicsCommandList, (void **)&list);

    return wrap_result(hr, (IUnknown *)list, t == D3D12_COMMAND_LIST_TYPE_COPY ? &copy_list_class : &list_class, out);
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

    HRESULT hr = ID3D12Device_CreateDescriptorHeap(INNER(p, ID3D12Device), desc, &IID_ID3D12DescriptorHeap,
                                                   (void **)&heap);

    return wrap_result(hr, (IUnknown *)heap, &heap_class, out);
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

/* Resource creation (slots 27, 28, 29, 30): forwarded, recording the
 * initial state for PresentX's copy out of the plane. */
static HRESULT STDMETHODCALLTYPE device_CreateCommittedResource(struct proxy *p, const D3D12_HEAP_PROPERTIES *props,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    D3D12_HEAP_PROPERTIES storage;
    HRESULT hr;

    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        props = buffer_heap(props, &storage);
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
    HRESULT hr = ID3D12Device_CreatePlacedResource(INNER(p, ID3D12Device), heap, offset, desc, state, clear, riid, out);
    if (SUCCEEDED(hr) && out && *out)
        present_track_state(*out, state);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateReservedResource(struct proxy *p, const D3D12_RESOURCE_DESC *desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    HRESULT hr = ID3D12Device_CreateReservedResource(INNER(p, ID3D12Device), desc, state, clear, riid, out);
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
        patch_child(*out);
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

/* Slot 76 (FFX 0x1800033cc): CreateCommittedResource with a 0x40-byte
 * resource desc; the PC fields come first, Xbox fields fill +0x34..+0x3f. */
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
        rebox_log("d3d12_xs: Xbox resource desc fields ignored: +0x34 %#x, +0x38 %#llx",
                     *(const UINT *)(desc_x + 0x34), *(const unsigned long long *)(desc_x + 0x38));
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
    rebox_log("d3d12_xs: SetFrameIntervalX(%u us, period %u) ignored", interval_us, period);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_ScheduleFrameEventX(struct proxy *p, UINT type, UINT_PTR offset, void *event)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_WaitFrameEventX(struct proxy *p, UINT type, UINT timeout, void *event,
        UINT flags, UINT64 *token)
{
    static LONG64 next_token;
    if (token)
        *token = InterlockedIncrement64(&next_token);
    return S_OK;
}

/* ------------------------------------------------------------------ queue */

static void STDMETHODCALLTYPE queue_ExecuteCommandLists(struct proxy *p, UINT count, ID3D12CommandList *const *lists)
{
    ID3D12CommandList *stack[64] = { 0 }, **raw = count <= ARRAYSIZE(stack) ? stack : malloc(count * sizeof(*raw));

    for (UINT i = 0; i < count; i++)
        raw[i] = proxy_unwrap(lists[i]);
    ID3D12CommandQueue_ExecuteCommandLists(INNER(p, ID3D12CommandQueue), count, raw);
    if (raw != stack)
        free(raw);
}

/* Slot 30 (ScarlettX): PresentX(plane count, plane params, present params).
 * See present.c. */
static HRESULT STDMETHODCALLTYPE queue_PresentX(struct proxy *p, UINT planes, const void *plane_params,
        const void *params)
{
    return present_x(INNER(p, ID3D12CommandQueue), &p->present, planes, plane_params, params);
}

/* ---------------------------------------------------------- command list */

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
    ((D3D12_GPU_DESCRIPTOR_HANDLE *(STDMETHODCALLTYPE *)(void *, D3D12_GPU_DESCRIPTOR_HANDLE *))
        PC_SLOT(p->inner, 10))(p->inner, &h);
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

static void build(struct proxy_class *cls, unsigned pc_prefix, const struct slot *slots, unsigned count)
{
    void **vtbl = calloc(cls->slots, sizeof(*vtbl));
    unsigned i;

    for (i = 0; i < cls->slots; i++)
        vtbl[i] = i < pc_prefix ? forward_thunk(i) : trap_thunk(cls, i);
    vtbl[0] = (void *)proxy_QueryInterface;
    vtbl[1] = (void *)proxy_AddRef;
    vtbl[2] = (void *)proxy_Release;
    for (i = 0; i < count; i++)
        vtbl[slots[i].xbox] = slots[i].custom ? slots[i].custom : forward_thunk(slots[i].pc);
    cls->vtbl = vtbl;
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
        FN(27, device_CreateCommittedResource),
        FN(28, device_CreateHeap),
        FN(29, device_CreatePlacedResource),
        FN(30, device_CreateReservedResource),
        FN(43, device_GetAdapterLuid),
        FN(48, device_CreateCommandQueue), /* CreateCommandQueueX */
        FN(62, device_CreateComputePipelineStateX),
        FN(68, device_Slot68),
        FN(69, device_Slot69),
        FN(73, device_CreateCommandListX),
        FN(76, device_CreateCommittedResourceX),
        FN(78, device_SetFrameIntervalX),
        FN(79, device_ScheduleFrameEventX),
        FN(80, device_WaitFrameEventX),
    };
    /* Queue: PC through GetDesc (18). */
    static const struct slot queue_slots[] = {
        FN(7, child_GetDevice),
        FN(10, queue_ExecuteCommandLists),
        FN(30, queue_PresentX),
    };
    /* Graphics list: PC through ResourceBarrier (26), then PC + 4 for PC
     * slots 28-58 (both sources agree at 32-48, 50-52 and 60-62). */
    static struct slot list_slots[64] = {
        FN(7, child_GetDevice),
        FN(10, list_Reset),
        FN(26, list_ResourceBarrier),
        FN(32, list_SetDescriptorHeaps),
        FN(83, list_CopyTextureRegionX),
        FN(146, list_Barrier),
        FN(148, list_AllocateEmbeddedX),
    };
    /* Copy-queue lists have their own short layout (ScarlettX). */
    static const struct slot copy_slots[] = {
        FN(7, child_GetDevice),
        FN(10, list_Reset),
        FWD(11, 15), /* CopyBufferRegion */
        FWD(12, 16), /* CopyTextureRegion */
        FN(15, list_ResourceBarrier),
    };
    static const struct slot heap_slots[] = {
        FN(7, child_GetDevice),
        FN(9, heap_GetCPUDescriptorHandleForHeapStart),
        FN(10, heap_GetGPUDescriptorHandleForHeapStart),
    };
    unsigned n = 7;

    for (unsigned x = 33; x <= 62; x++)
        list_slots[n++] = (struct slot)FWD(x, x - 4);

    build(&device_class, 44, device_slots, ARRAYSIZE(device_slots));
    build(&queue_class, 19, queue_slots, ARRAYSIZE(queue_slots));
    build(&list_class, 27, list_slots, n);
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
    if (p->cls != &list_class && p->cls != &copy_list_class)
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
        patch_child_classes(inner);
    }
    ReleaseSRWLockExclusive(&lock);
    if (device_proxy->inner != (IUnknown *)inner)
        return E_FAIL;
    return proxy_QueryInterface(device_proxy, riid, out);
}
