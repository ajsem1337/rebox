/* PresentX for the queue proxy.
 *
 * The console presents a "plane" (a render target) straight from the queue.
 * Here each queue that presents gets a window, on its own message thread,
 * and a flip-model DXGI swapchain (DXVK's dxgi.dll over vkd3d-proton). Each
 * PresentX copies plane 0 into the back buffer on the same queue, so it is
 * ordered after the title's rendering, and then presents.
 *
 * Argument layout (ScarlettX, traced from a retail title):
 * PresentX(plane_count, plane_params, present_params), where the qword at
 * plane_params+16 is the plane's ID3D12Resource, or a pointer to it. Both are
 * accepted. The first call is logged in full to settle which, and what the
 * rest holds.
 *
 * The copy needs the plane's current state. It comes from tracking: initial
 * states at resource creation and every barrier the title records (legacy
 * states and enhanced-barrier layouts), see proxy.c. Untracked resources are
 * assumed COMMON, the state a plane is normally presented in.
 *
 * Environment:
 *   REBOX_PRESENT=0        no window or swapchain (frame dumps still work)
 *   REBOX_VSYNC=N          Present sync interval, default 1 (there is no
 *                             other frame pacing yet)
 *   REBOX_DUMP_FRAMES=DIR  write presented frames to DIR\frame_<n>.bmp
 *                             (8-bit RGBA/BGRA planes) or .bin
 *   REBOX_DUMP_FRAMES_AT   which frames, comma-separated; default 1,60,600 */

#define COBJMACROS
/* Struct-returning methods (GetDesc) as hidden-pointer calls, matching
 * vkd3d-proton's MSVC ABI. */
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
/* DXGI IIDs are instantiated here; d3d12_xs.c does the D3D12 ones. */
#include <initguid.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "blit.h"
#include "present.h"
#include "proxy.h"

#define RING 3

struct present_queue
{
    ID3D12CommandQueue *queue;
    ID3D12Device *device;
    ID3D12Fence *fence;
    HANDLE event;
    UINT64 fence_value;
    struct
    {
        ID3D12CommandAllocator *alloc;
        ID3D12GraphicsCommandList *list;
        UINT64 done;
    } ring[RING];
    UINT64 frames;

    ID3D12Resource *convert; /* RGBA8 copy of planes a swapchain can't take */
    UINT convert_w, convert_h;

    HWND hwnd;
    IDXGISwapChain3 *swapchain;
    UINT width, height;
    DXGI_FORMAT format;
    BOOL swapchain_failed;
};

/* ------------------------------------------------------------- tracking */

enum { TRACK_LEGACY = 1, TRACK_LAYOUT = 2 };

#define TRACK_SIZE 65536
#define TRACK_PROBE 16

static struct
{
    void *resource;
    UINT kind, value;
} tracked[TRACK_SIZE];
static SRWLOCK track_lock = SRWLOCK_INIT;

static unsigned track_hash(const void *p)
{
    ULONG_PTR h = (ULONG_PTR)p >> 4;
    return (unsigned)((h ^ (h >> 15) ^ (h >> 31)) & (TRACK_SIZE - 1));
}

static void track(void *resource, UINT kind, UINT value)
{
    unsigned h = track_hash(resource), i, slot = h;

    if (!resource)
        return;
    AcquireSRWLockExclusive(&track_lock);
    for (i = 0; i < TRACK_PROBE; i++)
    {
        unsigned s = (h + i) & (TRACK_SIZE - 1);
        if (tracked[s].resource == resource || !tracked[s].resource)
        {
            slot = s;
            break;
        }
    }
    /* Full neighbourhood: overwrite the home slot. Entries for freed
     * resources are only ever stale, never wrong for live ones. */
    tracked[slot].resource = resource;
    tracked[slot].kind = kind;
    tracked[slot].value = value;
    ReleaseSRWLockExclusive(&track_lock);
}

static BOOL lookup(void *resource, UINT *kind, UINT *value)
{
    unsigned h = track_hash(resource);
    BOOL found = FALSE;

    AcquireSRWLockShared(&track_lock);
    for (unsigned i = 0; i < TRACK_PROBE; i++)
    {
        unsigned s = (h + i) & (TRACK_SIZE - 1);
        if (tracked[s].resource == resource)
        {
            *kind = tracked[s].kind;
            *value = tracked[s].value;
            found = TRUE;
            break;
        }
    }
    ReleaseSRWLockShared(&track_lock);
    return found;
}

void present_track_state(void *resource, D3D12_RESOURCE_STATES state)
{
    track(resource, TRACK_LEGACY, state);
}

void present_track_barriers(UINT count, const D3D12_RESOURCE_BARRIER *barriers)
{
    for (UINT i = 0; barriers && i < count; i++)
    {
        const D3D12_RESOURCE_BARRIER *b = &barriers[i];

        /* Planes are single-subresource; whole-resource or subresource 0
         * transitions are the ones that matter. */
        if (b->Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
            && (b->Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || !b->Transition.Subresource)
            && !(b->Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY))
            track(b->Transition.pResource, TRACK_LEGACY, b->Transition.StateAfter);
    }
}

void present_track_enhanced(UINT32 count, const D3D12_BARRIER_GROUP *groups)
{
    for (UINT32 g = 0; groups && g < count; g++)
    {
        if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
            continue;
        for (UINT32 i = 0; i < groups[g].NumBarriers; i++)
        {
            const D3D12_TEXTURE_BARRIER *t = &groups[g].pTextureBarriers[i];
            if (t->Subresources.IndexOrFirstMipLevel == 0xffffffff || !t->Subresources.IndexOrFirstMipLevel)
                track(t->pResource, TRACK_LAYOUT, t->LayoutAfter);
        }
    }
}

/* ------------------------------------------------------------------ setup */

static int env_int(const char *name, int fallback)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : fallback;
}

static void *resource_vtbl(ID3D12Device *device)
{
    static void *vtbl;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                 D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *probe;

    /* vkd3d-proton shares one vtable between all resources. */
    if (!vtbl && SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&probe)))
    {
        vtbl = *(void **)probe;
        ID3D12Resource_Release(probe);
    }
    return vtbl;
}

static struct present_queue *present_create(ID3D12CommandQueue *queue)
{
    struct present_queue *s = calloc(1, sizeof(*s));
    D3D12_COMMAND_QUEUE_DESC qd;
    HRESULT hr;

    s->queue = queue;
    ID3D12CommandQueue_AddRef(queue);
    if (FAILED(hr = ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&s->device)))
        goto fail;
    qd = ID3D12CommandQueue_GetDesc(queue);
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        rebox_log("d3d12_xs: PresentX on a non-direct queue (type %u)", qd.Type);
    if (FAILED(hr = ID3D12Device_CreateFence(s->device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&s->fence)))
        goto fail;
    s->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < RING; i++)
    {
        if (FAILED(hr = ID3D12Device_CreateCommandAllocator(s->device, qd.Type, &IID_ID3D12CommandAllocator,
                                                            (void **)&s->ring[i].alloc)))
            goto fail;
        if (FAILED(hr = ID3D12Device_CreateCommandList(s->device, 0, qd.Type, s->ring[i].alloc, NULL,
                                                       &IID_ID3D12GraphicsCommandList, (void **)&s->ring[i].list)))
            goto fail;
        ID3D12GraphicsCommandList_Close(s->ring[i].list);
    }
    return s;

fail:
    rebox_log("d3d12_xs: PresentX setup failed: %#lx", hr);
    present_queue_destroy(s);
    return NULL;
}

/* ---------------------------------------------------------------- window */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE)
    {
        /* As closing a game's window does. TerminateProcess, not
         * ExitProcess: DLL teardown with a live device can hang. */
        rebox_log("d3d12_xs: window closed, exiting");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct window_args
{
    UINT width, height;
    HANDLE ready;
    HWND hwnd;
};

/* The window lives on its own thread so it stays responsive however the
 * title's threads behave. */
static DWORD WINAPI window_thread(void *param)
{
    struct window_args *args = param;
    WNDCLASSW wc = { .lpfnWndProc = window_proc, .hInstance = GetModuleHandleW(NULL),
                     .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"ReboxPresent" };
    UINT max_w = GetSystemMetrics(SM_CXSCREEN) * 9 / 10, max_h = GetSystemMetrics(SM_CYSCREEN) * 9 / 10;
    UINT w = args->width, h = args->height;
    RECT rect;
    MSG msg;

    /* Fit large planes on screen, keeping the aspect ratio; the swapchain
     * scales. */
    if (max_w && max_h && (w > max_w || h > max_h))
    {
        if ((UINT64)w * max_h > (UINT64)h * max_w)
            h = (UINT)((UINT64)h * max_w / w), w = max_w;
        else
            w = (UINT)((UINT64)w * max_h / h), h = max_h;
    }
    SetRect(&rect, 0, 0, w, h);
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    RegisterClassW(&wc);
    args->hwnd = CreateWindowExW(0, wc.lpszClassName, L"rebox", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                 NULL, NULL, wc.hInstance, NULL);
    SetEvent(args->ready);
    if (!args->hwnd)
        return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static HWND create_window(UINT width, UINT height)
{
    struct window_args *args = calloc(1, sizeof(*args)); /* the thread outlives this call */
    HANDLE thread;

    args->width = width;
    args->height = height;
    args->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!(thread = CreateThread(NULL, 0, window_thread, args, 0, NULL)))
        return NULL;
    CloseHandle(thread);
    WaitForSingleObject(args->ready, INFINITE);
    CloseHandle(args->ready);
    return args->hwnd;
}

/* Flip-model swapchains take a few formats; pick the one in the plane's
 * copy-compatible family. */
static DXGI_FORMAT swapchain_format(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

static void wait_idle(struct present_queue *s)
{
    if (ID3D12Fence_GetCompletedValue(s->fence) < s->fence_value)
    {
        ID3D12Fence_SetEventOnCompletion(s->fence, s->fence_value, s->event);
        WaitForSingleObject(s->event, INFINITE); /* reading or reusing before the GPU is done corrupts */
    }
}

static BOOL ensure_swapchain(struct present_queue *s, UINT width, UINT height, DXGI_FORMAT format)
{
    HRESULT (WINAPI *create_factory)(UINT, REFIID, void **);
    DXGI_SWAP_CHAIN_DESC1 desc = {
        .Width = width, .Height = height, .Format = format, .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .BufferCount = RING,
        .Scaling = DXGI_SCALING_STRETCH, .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
    };
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *sc1 = NULL;
    HMODULE dxgi;
    HRESULT hr;

    if (s->swapchain && s->width == width && s->height == height && s->format == format)
        return TRUE;
    if (s->swapchain_failed || !env_int("REBOX_PRESENT", 1))
        return FALSE;
    if (s->swapchain)
    {
        /* The plane changed size or format: the old back buffers must be
         * idle before the swapchain goes. */
        wait_idle(s);
        IDXGISwapChain3_Release(s->swapchain);
        s->swapchain = NULL;
    }
    if (!s->hwnd && !(s->hwnd = create_window(width, height)))
    {
        rebox_log("d3d12_xs: PresentX: could not create a window (no display?); frames are not shown");
        s->swapchain_failed = TRUE;
        return FALSE;
    }
    if (!(dxgi = LoadLibraryA("dxgi.dll"))
        || !(create_factory = (void *)GetProcAddress(dxgi, "CreateDXGIFactory2"))
        || FAILED(hr = create_factory(0, &IID_IDXGIFactory2, (void **)&factory)))
    {
        rebox_log("d3d12_xs: PresentX: no DXGI factory; frames are not shown");
        s->swapchain_failed = TRUE;
        return FALSE;
    }
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)s->queue, s->hwnd, &desc, NULL, NULL, &sc1);
    IDXGIFactory2_Release(factory);
    if (SUCCEEDED(hr))
    {
        hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&s->swapchain);
        IDXGISwapChain1_Release(sc1);
    }
    if (FAILED(hr))
    {
        rebox_log("d3d12_xs: PresentX: swapchain creation failed (%#lx); frames are not shown", hr);
        s->swapchain_failed = TRUE;
        return FALSE;
    }
    s->width = width;
    s->height = height;
    s->format = format;
    rebox_log("d3d12_xs: PresentX: %ux%u swapchain, format %u", width, height, format);
    return TRUE;
}

/* ---------------------------------------------------------------- dumps */

static BOOL dump_wanted(UINT64 frame)
{
    const char *at = getenv("REBOX_DUMP_FRAMES_AT");
    char *end;

    if (!getenv("REBOX_DUMP_FRAMES"))
        return FALSE;
    if (!at || !*at)
        at = "1,60,600";
    while (*at)
    {
        unsigned long long n = strtoull(at, &end, 10);
        if (end == at)
            break;
        if (n == frame)
            return TRUE;
        at = *end ? end + 1 : end;
    }
    return FALSE;
}

static void write_dump(UINT64 frame, const D3D12_RESOURCE_DESC *desc, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp,
        const BYTE *data)
{
    const char *dir = getenv("REBOX_DUMP_FRAMES");
    BOOL rgba = swapchain_format(desc->Format) == DXGI_FORMAT_R8G8B8A8_UNORM;
    BOOL bgra = swapchain_format(desc->Format) == DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT w = (UINT)desc->Width, h = desc->Height;
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof(path), "%s\\frame_%llu.%s", dir, (unsigned long long)frame, rgba || bgra ? "bmp" : "bin");
    if (!(f = fopen(path, "wb")))
    {
        rebox_log("d3d12_xs: cannot write %s", path);
        return;
    }
    if (rgba || bgra)
    {
        /* 32-bit top-down BMP; BMP pixels are BGRA. */
        BYTE header[54] = { 'B', 'M' };
        UINT32 v;
        INT32 neg_h = -(INT32)h;
        BYTE *row = malloc(w * 4);

        v = 54 + w * h * 4; memcpy(header + 2, &v, 4);
        v = 54; memcpy(header + 10, &v, 4);
        v = 40; memcpy(header + 14, &v, 4);
        memcpy(header + 18, &w, 4);
        memcpy(header + 22, &neg_h, 4);
        header[26] = 1;
        header[28] = 32;
        fwrite(header, 1, sizeof(header), f);
        for (UINT y = 0; y < h; y++)
        {
            const BYTE *src = data + fp->Offset + (SIZE_T)y * fp->Footprint.RowPitch;
            for (UINT x = 0; x < w; x++)
            {
                row[x * 4 + 0] = src[x * 4 + (rgba ? 2 : 0)];
                row[x * 4 + 1] = src[x * 4 + 1];
                row[x * 4 + 2] = src[x * 4 + (rgba ? 0 : 2)];
                row[x * 4 + 3] = src[x * 4 + 3];
            }
            fwrite(row, 1, w * 4, f);
        }
        free(row);
    }
    else
    {
        /* Raw rows as laid out by the GPU, behind a small header. */
        UINT32 hdr[4] = { w, h, desc->Format, fp->Footprint.RowPitch };
        fwrite(hdr, 1, sizeof(hdr), f);
        fwrite(data + fp->Offset, 1, (SIZE_T)fp->Footprint.RowPitch * h, f);
    }
    fclose(f);
    rebox_log("d3d12_xs: dumped frame %llu to %s", (unsigned long long)frame, path);
}

/* --------------------------------------------------------------- present */

static ID3D12Resource *plane_resource(ID3D12Device *device, const void *plane_params)
{
    void *vtbl = resource_vtbl(device);
    void *cand;

    if (!plane_params || !vtbl)
        return NULL;
    cand = (void *)((const UINT64 *)plane_params)[2];
    if (!cand)
        return NULL;
    if (*(void **)cand == vtbl)
        return cand;
    cand = *(void **)cand;
    if (cand && *(void **)cand == vtbl)
        return cand;
    return NULL;
}

/* Moves the plane from its tracked state to the one the presenter reads it
 * in (copy source, or shader resource for the conversion blit), or back. */
static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res, UINT kind, UINT from, BOOL shader_read,
        BOOL forward)
{
    if (kind == TRACK_LAYOUT)
    {
        UINT layout = shader_read ? D3D12_BARRIER_LAYOUT_SHADER_RESOURCE : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        UINT sync = shader_read ? D3D12_BARRIER_SYNC_COMPUTE_SHADING : D3D12_BARRIER_SYNC_COPY;
        UINT access = shader_read ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE : D3D12_BARRIER_ACCESS_COPY_SOURCE;
        D3D12_TEXTURE_BARRIER t = {
            .SyncBefore = forward ? D3D12_BARRIER_SYNC_ALL : sync,
            .SyncAfter = forward ? sync : D3D12_BARRIER_SYNC_ALL,
            .AccessBefore = forward ? D3D12_BARRIER_ACCESS_COMMON : access,
            .AccessAfter = forward ? access : D3D12_BARRIER_ACCESS_COMMON,
            .LayoutBefore = forward ? from : layout,
            .LayoutAfter = forward ? layout : from,
            .pResource = res,
            .Subresources = { 0xffffffff },
        };
        D3D12_BARRIER_GROUP g = { .Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &t };

        if (from != layout)
            ID3D12GraphicsCommandList7_Barrier((ID3D12GraphicsCommandList7 *)list, 1, &g);
    }
    else
    {
        UINT state = shader_read ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER b = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };

        if (from == state)
            return;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = forward ? from : state;
        b.Transition.StateAfter = forward ? state : from;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    }
}

/* The RGBA8 target planes in other formats are converted into. */
static BOOL ensure_convert(struct present_queue *s, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };

    if (s->convert && s->convert_w == width && s->convert_h == height)
        return TRUE;
    if (s->convert)
    {
        wait_idle(s);
        ID3D12Resource_Release(s->convert);
        s->convert = NULL;
    }
    if (FAILED(ID3D12Device_CreateCommittedResource(s->device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&s->convert)))
        return FALSE;
    s->convert_w = width;
    s->convert_h = height;
    return TRUE;
}

static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res, D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };

    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

HRESULT present_x(ID3D12CommandQueue *queue, struct present_queue **state, UINT planes, const void *plane_params,
        const void *params)
{
    struct present_queue *s = *state;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    ID3D12Resource *plane, *src, *back = NULL, *readback = NULL;
    D3D12_RESOURCE_DESC desc;
    DXGI_FORMAT format;
    UINT kind = TRACK_LEGACY, from = D3D12_RESOURCE_STATE_COMMON;
    BOOL show, dump, convert;
    UINT64 frame, total = 0;
    unsigned r;
    static LONG logged;

    if (!InterlockedExchange(&logged, 1))
    {
        const UINT64 *q = plane_params, *p = params;
        rebox_log("d3d12_xs: first PresentX: %u planes, plane params %p [%#llx %#llx %#llx %#llx %#llx %#llx], "
                     "params %p [%#llx %#llx %#llx %#llx]", planes, plane_params,
                     q ? q[0] : 0, q ? q[1] : 0, q ? q[2] : 0, q ? q[3] : 0, q ? q[4] : 0, q ? q[5] : 0,
                     params, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0);
    }
    proxy_frame_presented();
    if (!s && !(s = *state = present_create(queue)))
        return S_OK;
    frame = ++s->frames;

    if (!planes || !(plane = plane_resource(s->device, plane_params)))
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: PresentX: no plane resource found in the plane params; nothing shown");
        return S_OK;
    }
    if (planes > 1)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: PresentX with %u planes: only plane 0 is shown", planes);
    }

    desc = ID3D12Resource_GetDesc(plane);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: PresentX: plane (dimension %u, %u samples) can't be presented",
                         desc.Dimension, desc.SampleDesc.Count);
        return S_OK;
    }
    /* Formats a swapchain can't take are converted to RGBA8 with a blit
     * (HDR values are clamped, not tone-mapped). */
    if ((convert = (format = swapchain_format(desc.Format)) == DXGI_FORMAT_UNKNOWN))
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: PresentX: plane format %u is converted to RGBA8 for display", desc.Format);
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!ensure_convert(s, (UINT)desc.Width, desc.Height))
            return S_OK;
    }
    lookup(plane, &kind, &from);

    show = ensure_swapchain(s, (UINT)desc.Width, desc.Height, format);
    dump = dump_wanted(frame);
    if (!show && !dump)
        return S_OK;

    /* Reuse the oldest list once the GPU is done with it. */
    r = frame % RING;
    if (ID3D12Fence_GetCompletedValue(s->fence) < s->ring[r].done)
    {
        ID3D12Fence_SetEventOnCompletion(s->fence, s->ring[r].done, s->event);
        WaitForSingleObject(s->event, INFINITE); /* reading or reusing before the GPU is done corrupts */
    }
    ID3D12CommandAllocator_Reset(s->ring[r].alloc);
    ID3D12GraphicsCommandList_Reset(s->ring[r].list, s->ring[r].alloc, NULL);

    transition(s->ring[r].list, plane, kind, from, convert, TRUE);
    src = plane;
    if (convert)
    {
        ReboxBlit(s->ring[r].list, plane, (UINT)desc.Width, desc.Height, s->convert, (UINT)desc.Width, desc.Height);
        barrier(s->ring[r].list, s->convert, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        src = s->convert;
        desc = ID3D12Resource_GetDesc(src);
    }
    if (show && SUCCEEDED(IDXGISwapChain3_GetBuffer(s->swapchain, IDXGISwapChain3_GetCurrentBackBufferIndex(s->swapchain),
                                                    &IID_ID3D12Resource, (void **)&back)))
    {
        barrier(s->ring[r].list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(s->ring[r].list, back, src);
        barrier(s->ring[r].list, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (dump)
    {
        D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };

        ID3D12Device_GetCopyableFootprints(s->device, &desc, 0, 1, 0, &fp, NULL, NULL, &total);
        bd.Width = total;
        if (SUCCEEDED(ID3D12Device_CreateCommittedResource(s->device, &heap, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback)))
        {
            D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            D3D12_TEXTURE_COPY_LOCATION from_loc = { src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };

            dst.PlacedFootprint = fp;
            from_loc.SubresourceIndex = 0;
            ID3D12GraphicsCommandList_CopyTextureRegion(s->ring[r].list, &dst, 0, 0, 0, &from_loc, NULL);
        }
    }
    if (convert)
        barrier(s->ring[r].list, s->convert, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(s->ring[r].list, plane, kind, from, convert, FALSE);

    if (SUCCEEDED(ID3D12GraphicsCommandList_Close(s->ring[r].list)))
    {
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)s->ring[r].list };
        ID3D12CommandQueue_ExecuteCommandLists(s->queue, 1, lists);
        if (back)
            IDXGISwapChain3_Present(s->swapchain, env_int("REBOX_VSYNC", 1), 0);
        ID3D12CommandQueue_Signal(s->queue, s->fence, ++s->fence_value);
        s->ring[r].done = s->fence_value;
    }
    if (back)
        ID3D12Resource_Release(back);

    if (readback)
    {
        void *data;
        D3D12_RANGE range = { 0, (SIZE_T)total };

        wait_idle(s);
        if (SUCCEEDED(ID3D12Resource_Map(readback, 0, &range, &data)))
        {
            write_dump(frame, &desc, &fp, data);
            ID3D12Resource_Unmap(readback, 0, NULL);
        }
        ID3D12Resource_Release(readback);
    }
    return S_OK;
}

void present_queue_destroy(struct present_queue *s)
{
    if (!s)
        return;
    if (s->fence)
        wait_idle(s);
    if (s->swapchain)
        IDXGISwapChain3_Release(s->swapchain);
    if (s->convert)
        ID3D12Resource_Release(s->convert);
    if (s->hwnd)
        PostMessageW(s->hwnd, WM_QUIT, 0, 0);
    for (int i = 0; i < RING; i++)
    {
        if (s->ring[i].list)
            ID3D12GraphicsCommandList_Release(s->ring[i].list);
        if (s->ring[i].alloc)
            ID3D12CommandAllocator_Release(s->ring[i].alloc);
    }
    if (s->fence)
        ID3D12Fence_Release(s->fence);
    if (s->event)
        CloseHandle(s->event);
    if (s->device)
        ID3D12Device_Release(s->device);
    ID3D12CommandQueue_Release(s->queue);
    free(s);
}
