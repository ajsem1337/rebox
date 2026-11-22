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
    D3D12_RESOURCE_DESC desc_storage;
    HRESULT hr;

    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        props = buffer_heap(props, &storage);
    desc = pc_desc(desc, &desc_storage);
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

static HRESULT STDMETHODCALLTYPE device_ScheduleFrameEventX(struct proxy *p, UINT type, UINT_PTR offset_us, HANDLE event)
{
    return pacing_schedule(type, offset_us, event);
}

static HRESULT STDMETHODCALLTYPE device_WaitFrameEventX(struct proxy *p, UINT type, UINT timeout_ms, void *event,
        UINT flags, UINT64 *token)
{
    return pacing_wait(type, timeout_ms, token);
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
        FN(27, device_CreateCommittedResource),
        FN(28, device_CreateHeap),
        FN(29, device_CreatePlacedResource),
        FN(30, device_CreateReservedResource),
        FN(38, device_GetCopyableFootprints),
        FN(41, device_CreateCommandSignature),
        FN(43, device_GetAdapterLuid),
        FN(48, device_CreateCommandQueue), /* CreateCommandQueueX */
        FN(60, device_CreateGraphicsPipelineStateX),
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
    /* Queue for titles built against SDK 0x20004 or later (ScarlettX 1e5473d, from a title's
     * calls): two methods were added ahead of Signal, so Signal is 16 and PresentX 32. Where they
     * were added isn't known, so 11-15 (PC's markers, then the two new ones) trap, and only the
     * slots a title was seen calling, plus Wait after Signal, are mapped. */
    static const struct slot queue_v4_slots[] = {
        FN(7, child_GetDevice),
        FN(10, queue_ExecuteCommandLists),
        FWD(16, 14), /* Signal */
        FWD(17, 15), /* Wait, assumed to follow Signal */
        FN(32, queue_PresentX),
    };
    /* Graphics list: PC through ResourceBarrier (26), then PC + 4 for PC
     * slots 28-58 (both sources agree at 32-48, 50-52 and 60-62). */
    static struct slot list_slots[64] = {
        FN(7, child_GetDevice),
        FN(10, list_Reset),
        FN(26, list_ResourceBarrier),
        FN(32, list_SetDescriptorHeaps),
        FN(35, list_SetComputeRootDescriptorTable),   /* PC 31 */
        FN(36, list_SetGraphicsRootDescriptorTable),  /* PC 32 */
        FN(53, list_ClearUnorderedAccessViewUint),    /* PC 49 */
        FN(54, list_ClearUnorderedAccessViewFloat),   /* PC 50 */
        FN(61, list_Slot61),                          /* PC 59 or 57 */
        FN(83, list_CopyTextureRegionX),
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
        FWD(12, 16), /* CopyTextureRegion */
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
    while (n < ARRAYSIZE(list_slots) && list_slots[n].custom)
        n++;
    customs = n;
    for (unsigned x = 33; x <= 62; x++)
    {
        BOOL taken = FALSE;
        for (unsigned j = 0; j < customs; j++)
            taken |= list_slots[j].xbox == x;
        if (!taken)
            list_slots[n++] = (struct slot)FWD(x, x - 4);
    }

    build(&device_class, 44, device_slots, ARRAYSIZE(device_slots));
    build(&queue_class, 19, queue_slots, ARRAYSIZE(queue_slots));
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
