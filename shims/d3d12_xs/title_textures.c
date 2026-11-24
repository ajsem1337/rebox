/* Textures a title placed at its own GPU addresses (CreatePlacedResourceX, slot 44).
 *
 * On the console the texture is the memory at that address, in a Gfx10 tiled layout, and the
 * title fills it there: DirectStorage writes texture pools into it (already tiled, docs/d3d12x.md
 * "Texture tiling"). Here the texture has its own memory, because vkd3d-proton can't place an
 * image in the buffer-only GPU memory chunks. So each such texture is registered with its
 * address and swizzle mode, writes the shims see (ReboxTitleMemoryWritten: DirectStorage
 * requests) mark it dirty, and before a queue runs the title's command lists every dirty texture
 * is detiled from title memory (detile.cpp, addrlib) into an upload buffer and copied in. A new
 * texture starts dirty, for data written before it was created. Writes nothing reports (the CPU,
 * or GPU copies to the address) are caught by sampling: each upload keeps a fingerprint of a few
 * bytes spread over the texture's memory, and before each submission a slice of the textures
 * (WATCH_BUDGET, round robin) is fingerprinted again; a change makes it dirty. Textures up to
 * FULL_HASH_MAX are hashed whole, within FULL_HASH_BUDGET bytes a submission, so a small edit is
 * seen too (Unreal's font atlas gains glyphs as text is first drawn); larger ones are sampled,
 * which sees a texture rewritten (a streaming slot refilled), not a small edit between samples.
 * REBOX_TEXTURE_WATCH=0 turns the sampling off.
 *
 * The swizzle mode: a console Layout from 0x100 is 0x100 plus the AMD swizzle mode (0x100 linear,
 * 0x105 SW_4KB_S, 0x10A SW_64KB_D; xcl and TuxBox, xcl checked against xg_xs.dll on 29,700 descs).
 * Layout 2, or desc flag 0x08000000 (TuxBox), is SW_64KB_D; Layout 0 (the driver's choice) is
 * SW_4KB_S for a plain single 2D texture whatever its size (the same two). REBOX_TEXTURE_SWIZZLE picks: "auto" (default: those; ROW_MAJOR descs linear;
 * cubemaps and volumes SW_4KB_S, other arrays SW_64KB_D, what the texture pools measured show),
 * "64KB_D", "4KB_S", "linear",
 * or "off" (no uploads). Each distinct console layout is logged once with what it got. */

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "detile.h"
#include "gpu_memory.h"
#include "title_textures.h"

struct title_texture
{
    ID3D12Resource *resource;   /* not a reference: removed when it is destroyed */
    UINT64 address, size;       /* the title's (GPU) address */
    const BYTE *cpu;            /* where the CPU reads it */
    struct detile_surface surface;
    D3D12_RESOURCE_STATES state;
    UINT subresources;
    BOOL dirty;
    BOOL unreadable;            /* memory not committed yet: rechecked with backoff */
    BOOL chunk;                 /* in our GPU memory chunks, which stay mapped */
    UINT check_at, backoff;     /* in submissions */
    UINT64 fingerprint;
    UINT uploads;
    BOOL gpu_written;           /* a GPU copy wrote it: title memory isn't its contents (font atlases) */
    UINT callback;
    struct title_texture *next;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct title_texture *textures;
static volatile LONG dirty_count;
static UINT watch_cursor;
static UINT submissions;

#define WATCH_SAMPLES 32
#define WATCH_BUDGET 512                   /* textures fingerprinted per submission */
#define FULL_HASH_MAX (4u << 20)           /* textures this size or smaller are hashed whole */
#define FULL_HASH_BUDGET (8u << 20)        /* bytes hashed whole per submission */

static BOOL full_hash(const struct title_texture *t)
{
    return t->size <= FULL_HASH_MAX;
}

/* Small textures: a multiply-xor hash over every 8 bytes. Others: FNV-1a over 8 bytes at each of
 * WATCH_SAMPLES points spread over the texture, the first and last included. */
static UINT64 fingerprint(const struct title_texture *t)
{
    UINT64 h = 0xcbf29ce484222325ull, step = t->size > 8 ? (t->size - 8) / (WATCH_SAMPLES - 1) : 0;

    if (full_hash(t))
    {
        UINT64 i, v;
        for (i = 0; i + 8 <= t->size; i += 8)
        {
            memcpy(&v, t->cpu + i, 8);
            h = (h ^ v) * 0x9e3779b97f4a7c15ull;
            h ^= h >> 29;
        }
        for (; i < t->size; i++)
            h = (h ^ t->cpu[i]) * 0x100000001b3ull;
        return h;
    }

    for (UINT i = 0; i < WATCH_SAMPLES; i++)
    {
        UINT64 v;
        memcpy(&v, t->cpu + (t->size > 8 ? i * step : 0), t->size >= 8 ? 8 : t->size);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

static BOOL watch_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1)
    {
        char *v = rebox_getenv("REBOX_TEXTURE_WATCH");
        enabled = !v || strcmp(v, "0");
        free(v);
    }
    return enabled;
}

static BOOL readable(UINT64 address, UINT64 size);

/* Called with the lock held: a texture whose memory wasn't readable is checked again after 1, 2,
 * 4, ... up to 1024 submissions (TomBox backs off the same way), and uploaded once it is. */
static void recheck(struct title_texture *t)
{
    if (submissions < t->check_at)
        return;
    if (!readable((UINT64)(ULONG_PTR)t->cpu, t->size))
    {
        t->backoff = t->backoff ? min(t->backoff * 2, 1024) : 1;
        t->check_at = submissions + t->backoff;
        return;
    }
    t->unreadable = FALSE;
    t->dirty = TRUE;
    InterlockedIncrement(&dirty_count);
}

/* Called with the lock held: memory outside our chunks can be decommitted or remapped after the
 * texture was registered (a UE5 title remaps title pages), so it is checked before it is read; gone,
 * the texture waits for it again like one placed on uncommitted memory. */
static BOOL still_readable(struct title_texture *t)
{
    if (t->chunk || readable((UINT64)(ULONG_PTR)t->cpu, t->size))
        return TRUE;
    t->unreadable = TRUE;
    t->check_at = submissions + 1;
    t->backoff = 1;
    if (t->dirty)
    {
        t->dirty = FALSE;
        InterlockedDecrement(&dirty_count);
    }
    return FALSE;
}

/* Called with the lock held. */
static void watch(void)
{
    struct title_texture *t = textures;
    UINT skipped = 0, checked = 0, changed = 0;
    UINT64 hashed = 0;
    BOOL sample = watch_enabled();

    for (; t && skipped < watch_cursor; t = t->next)
        skipped++;
    if (!t)
        t = textures, watch_cursor = 0;
    for (; t && checked < WATCH_BUDGET && hashed < FULL_HASH_BUDGET; t = t->next, checked++)
        if (t->unreadable)
            recheck(t);
        else if (sample && !t->dirty && !t->gpu_written && still_readable(t) && (hashed += full_hash(t) ? t->size : 0, TRUE)
                 && fingerprint(t) != t->fingerprint)
        {
            t->dirty = TRUE;
            InterlockedIncrement(&dirty_count);
            changed++;
        }
    watch_cursor = t ? watch_cursor + checked : 0;
    if (changed)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: %u placed texture(s) changed in title memory without a report", changed);
    }
}

static int mode_setting(void)
{
    static int setting = -2;
    if (setting == -2)
    {
        char *v = rebox_getenv("REBOX_TEXTURE_SWIZZLE");
        setting = !v || !strcmp(v, "auto") ? -1 : !strcmp(v, "off") ? -3 : !strcmp(v, "64KB_D") ? DETILE_64KB_D
                : !strcmp(v, "4KB_S") ? DETILE_4KB_S : !strcmp(v, "linear") ? DETILE_LINEAR : -1;
        free(v);
    }
    return setting;
}


/* 2D arrays are read a mip (all its slices) at a time: the texture pools measured store them that way, and a
 * pool is the texture's memory image (helpers/textures: every array matches Leonida's decoder
 * mip-major, cubemaps and 2D textures match as they are). REBOX_TEXTURE_ARRAYS=slice-major
 * reads them in addrlib's order instead, a slice (its whole mip chain) at a time. */
static BOOL arrays_mip_major(void)
{
    static int setting = -1;
    if (setting == -1)
    {
        char *v = rebox_getenv("REBOX_TEXTURE_ARRAYS");
        setting = !v || strcmp(v, "slice-major");
        free(v);
    }
    return setting;
}

static void WINAPI destroyed(void *data)
{
    struct title_texture *t = data, **p;

    AcquireSRWLockExclusive(&lock);
    for (p = &textures; *p; p = &(*p)->next)
        if (*p == t)
        {
            *p = t->next;
            if (t->dirty)
                InterlockedDecrement(&dirty_count);
            free(t);
            break;
        }
    ReleaseSRWLockExclusive(&lock);
}

/* The CPU address of a title GPU address: through GPU memory's mapping when it is in one of its
 * chunks (identity-mapped or not; *chunk set), else the same address (memory the title allocated
 * itself). */
static const BYTE *cpu_address(UINT64 address, UINT64 size, BOOL *chunk)
{
    struct gpu_memory_region r[64];
    UINT n = gpu_memory_regions(r, ARRAYSIZE(r));

    *chunk = FALSE;
    for (UINT i = 0; i < n; i++)
        if (address >= r[i].gpu && address + size <= r[i].gpu + r[i].size)
        {
            *chunk = TRUE;
            return (const BYTE *)r[i].cpu + (address - r[i].gpu);
        }
    return (const BYTE *)(ULONG_PTR)address;
}

/* Whether [address, address + size) is committed, readable memory. Not for GPU memory chunks:
 * Wine's VirtualQuery doesn't know the Vulkan driver's mappings and reports them free. */
static BOOL readable(UINT64 address, UINT64 size)
{
    UINT64 at = address, end = address + size;

    while (at < end)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(ULONG_PTR)at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
                || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
            return FALSE;
        at = (UINT64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    }
    return TRUE;
}

/* The console surface for a texture: its shape from the PC desc, its swizzle mode from the setting,
 * the console layout or the shape. FALSE for what isn't uploaded (buffers, MSAA, unknown formats). */
static BOOL console_surface(const D3D12_RESOURCE_DESC *console_desc, const D3D12_RESOURCE_DESC *desc,
                            struct detile_surface *surface)
{
    int setting = mode_setting();
    UINT block, bytes;

    if (setting == -3 || !desc || desc->Dimension < D3D12_RESOURCE_DIMENSION_TEXTURE2D
            || desc->SampleDesc.Count > 1 || !(bytes = detile_element(desc->Format, &block)))
        return FALSE;
    memset(surface, 0, sizeof(*surface));
    surface->format = desc->Format;
    surface->width = (UINT)desc->Width;
    surface->height = desc->Height;
    surface->layers = desc->DepthOrArraySize;
    surface->volume = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    surface->mips = desc->MipLevels ? desc->MipLevels : 1;
    surface->mip_major = arrays_mip_major();
    if (setting >= 0)
        surface->mode = setting;
    else if (console_desc->Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR || console_desc->Layout == 0x100)
        surface->mode = DETILE_LINEAR;
    else if (console_desc->Layout == 0x105)
        surface->mode = DETILE_4KB_S;
    else if (console_desc->Layout == 0x10a || console_desc->Layout == 2 || (console_desc->Flags & 0x08000000))
        surface->mode = DETILE_64KB_D;
    /* By shape, as Leonida's census of the pools found (docs/textures_arrays.md): cubemaps (6
     * faces) and volumes SW_4KB_S, other 2D arrays SW_64KB_D, whatever their size. */
    else if (surface->volume || surface->layers == 6)
        surface->mode = DETILE_4KB_S;
    else if (surface->layers > 1)
        surface->mode = DETILE_64KB_D;
    else
        surface->mode = DETILE_4KB_S;
    return TRUE;
}

/* A small single-sample colour render target with nothing else allowed (REBOX_SYNC_SMALL_TARGETS=0
 * turns this off): 64x64 to 1024x1024, as xcl's. */
static BOOL small_cpu_filled_target(const D3D12_RESOURCE_DESC *d)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        char *v = rebox_getenv("REBOX_SYNC_SMALL_TARGETS");
        enabled = !v || strcmp(v, "0");
        free(v);
    }
    return enabled && d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        && (d->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                        | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
        && d->SampleDesc.Count <= 1 && d->DepthOrArraySize == 1
        && d->Width >= 64 && d->Width <= 1024 && d->Height >= 64 && d->Height <= 1024;
}

void title_texture_gpu_written(ID3D12Resource *resource)
{
    struct title_texture *t;

    if (!resource || !textures)
        return;
    AcquireSRWLockExclusive(&lock);
    for (t = textures; t && t->resource != resource; t = t->next)
        ;
    if (t && !t->gpu_written)
    {
        static LONG logged;
        t->gpu_written = TRUE;
        if (t->dirty)
        {
            t->dirty = FALSE;
            InterlockedDecrement(&dirty_count);
        }
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: texture at %#llx (%ux%u) is written by GPU copies; title memory is no longer "
                         "uploaded over it", (unsigned long long)t->address, t->surface.width, t->surface.height);
    }
    ReleaseSRWLockExclusive(&lock);
}

ID3D12Resource *title_texture_at(UINT64 address)
{
    struct title_texture *t;
    ID3D12Resource *res = NULL;

    AcquireSRWLockShared(&lock);
    for (t = textures; t && !res; t = t->next)
        if (t->address == address)
            res = t->resource;
    ReleaseSRWLockShared(&lock);
    return res;
}

BOOL title_texture_info(ID3D12Resource *resource, UINT64 *address, UINT *uploads)
{
    struct title_texture *t;
    BOOL found = FALSE;

    AcquireSRWLockShared(&lock);
    for (t = textures; t && !found; t = t->next)
        if ((found = t->resource == resource))
            *address = t->address, *uploads = t->uploads;
    ReleaseSRWLockShared(&lock);
    return found;
}

/* The bytes a texture takes in console memory (0 if it isn't uploaded). */
UINT64 title_texture_size(const D3D12_RESOURCE_DESC *desc)
{
    struct detile_surface surface;
    return console_surface(desc, desc, &surface) ? detile_size(&surface) : 0;
}

void title_texture_register(ID3D12Resource *resource, UINT64 address, const D3D12_RESOURCE_DESC *console_desc,
                            const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state)
{
    static struct { UINT layout, x34; } seen[32];
    static LONG seen_count;
    struct title_texture *t;
    ID3DDestructionNotifier *notifier;
    UINT x34 = *(const UINT *)((const BYTE *)console_desc + 0x34);
    BOOL logged = FALSE;

    if (!resource || !(t = calloc(1, sizeof(*t))))
        return;
    if (!console_surface(console_desc, desc, &t->surface))
    {
        free(t);
        return;
    }
    t->resource = resource;
    t->address = address;
    t->state = state;
    t->subresources = t->surface.mips * (t->surface.volume ? 1 : t->surface.layers);
    t->size = detile_size(&t->surface);

    for (LONG i = 0; i < seen_count && !logged; i++)
        logged = seen[i].layout == (UINT)console_desc->Layout && seen[i].x34 == x34;
    if (!logged && seen_count < (LONG)ARRAYSIZE(seen))
    {
        LONG i = InterlockedIncrement(&seen_count) - 1;
        if (i < (LONG)ARRAYSIZE(seen))
            seen[i].layout = console_desc->Layout, seen[i].x34 = x34;
        rebox_log("d3d12_xs: placed texture layout %#x (+0x34 %#x, +0x38 %#llx), %ux%u x%u, %u mips, format %u: "
                     "%s, %llu bytes at %#llx", console_desc->Layout, x34,
                     *(const unsigned long long *)((const BYTE *)console_desc + 0x38), t->surface.width,
                     t->surface.height, t->surface.layers, t->surface.mips, desc->Format,
                     t->surface.mode == DETILE_64KB_D ? "SW_64KB_D" : t->surface.mode == DETILE_4KB_S ? "SW_4KB_S" : "linear",
                     (unsigned long long)t->size, (unsigned long long)address);
    }
    {
        BOOL chunk;
        t->cpu = cpu_address(address, t->size, &chunk);
        t->chunk = chunk;
        if (t->size && !chunk && !readable((UINT64)(ULONG_PTR)t->cpu, t->size))
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 4)
                rebox_log("d3d12_xs: placed texture at %#llx: its memory isn't readable yet; rechecked until it is",
                             (unsigned long long)address);
            t->unreadable = TRUE;
        }
    }
    if (!t->size)
    {
        static LONG warned;
        if (!InterlockedExchange(&warned, 1))
            rebox_log("d3d12_xs: placed texture at %#llx: no layout for it; it stays as created",
                         (unsigned long long)address);
        free(t);
        return;
    }
    if (FAILED(ID3D12Resource_QueryInterface(resource, &IID_ID3DDestructionNotifier, (void **)&notifier)))
    {
        free(t);
        return;
    }
    ID3DDestructionNotifier_RegisterDestructionCallback(notifier, destroyed, t, &t->callback);
    ID3DDestructionNotifier_Release(notifier);
    /* A render target, depth or UAV texture is the GPU's (as xcl's GPU-owned textures): never
     * filled from title memory, which would overwrite what the GPU drew into it. Except a small
     * single-sample colour target: a title's glyph caches are 256x256 R8 render targets the CPU
     * fills as text is laid out (xcl, ivall/xcl #211), and without them most letters were missing.
     * Those are filled when their memory changes, which a GPU draw into them doesn't do. */
    t->gpu_written = (console_desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
            | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0;
    if (t->gpu_written && small_cpu_filled_target(console_desc))
        t->gpu_written = FALSE;
    /* A console-only format made a desktop one (proxy.c's pc_format) has another layout than the
     * console memory's: it isn't uploaded (detiling it overran the upload buffer). */
    if (desc->Format != console_desc->Format)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: placed texture at %#llx in console format %u (desktop %u): not filled from title memory",
                      (unsigned long long)address, console_desc->Format, desc->Format);
        t->gpu_written = TRUE;
    }
    t->dirty = !t->unreadable && !t->gpu_written;
    AcquireSRWLockExclusive(&lock);
    t->next = textures;
    textures = t;
    if (t->dirty)
        InterlockedIncrement(&dirty_count);
    ReleaseSRWLockExclusive(&lock);
}

/* A range the title's memory changed in, by CPU or GPU address. */
void WINAPI ReboxTitleMemoryWritten(UINT64 address, UINT64 size)
{
    if (!textures)
        return;
    AcquireSRWLockExclusive(&lock);
    for (struct title_texture *t = textures; t; t = t->next)
        if (!t->dirty && !t->unreadable && ((address < t->address + t->size && t->address < address + size)
                          || (address < (UINT64)(ULONG_PTR)t->cpu + t->size && (UINT64)(ULONG_PTR)t->cpu < address + size)))
        {
            t->dirty = TRUE;
            InterlockedIncrement(&dirty_count);
        }
    ReleaseSRWLockExclusive(&lock);
}

/* Uploads one texture through an upload buffer: detile each subresource into its copyable
 * footprint, then copy. Returns the buffer, which must live until the copy has run. */
static ID3D12Resource *record_upload(ID3D12Device *device, ID3D12GraphicsCommandList *list, struct title_texture *t)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp;
    UINT *rows;
    D3D12_HEAP_PROPERTIES upload = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 0, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    ID3D12Resource *buffer = NULL;
    UINT64 total = 0;
    BYTE *mapped;
    BOOL ok = TRUE;

    desc = ID3D12Resource_GetDesc(t->resource);
    fp = calloc(t->subresources, sizeof(*fp));
    rows = calloc(t->subresources, sizeof(*rows));
    if (!fp || !rows)
    {
        free(fp);
        free(rows);
        return NULL;
    }
    ID3D12Device_GetCopyableFootprints(device, &desc, 0, t->subresources, 0, fp, rows, NULL, &total);
    /* Slack past the last footprint: detiling writes whole tiles, padded rows included, so the
     * last subresource's can run past its footprint's end (it wrote past the buffer and crashed). */
    bd.Width = total + (256u << 10);
    if (FAILED(ID3D12Device_CreateCommittedResource(device, &upload, D3D12_HEAP_FLAG_NONE, &bd,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
                                                    (void **)&buffer))
            || FAILED(ID3D12Resource_Map(buffer, 0, NULL, (void **)&mapped)))
    {
        if (buffer)
            ID3D12Resource_Release(buffer);
        free(fp);
        free(rows);
        return NULL;
    }
    /* The console surface has to fit the desktop resource's footprints: detiling writes each of
     * its mips' rows at the footprint's pitch, unchecked (a mismatch overran the upload buffer). */
    if (t->subresources > (UINT)desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize))
        ok = FALSE;
    for (UINT i = 0; i < t->subresources && ok; i++)
    {
        UINT mip = i % t->surface.mips;
        UINT w = max(1u, t->surface.width >> mip), h = max(1u, t->surface.height >> mip);
        if (fp[i].Footprint.Width < w || fp[i].Footprint.Height < h)
            ok = FALSE;
    }
    if (!ok)
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("d3d12_xs: placed texture at %#llx (%ux%u, %u mips): its console layout doesn't fit the "
                      "resource (%llux%u, %u mips, format %u); not filled from title memory",
                      (unsigned long long)t->address, t->surface.width, t->surface.height, t->surface.mips,
                      (unsigned long long)desc.Width, desc.Height, desc.MipLevels, desc.Format);
        t->gpu_written = TRUE;
    }
    for (UINT i = 0; i < t->subresources && ok; i++)
    {
        UINT mip = i % t->surface.mips, layer = i / t->surface.mips;
        /* rows: rows of pixels, or of 4x4 blocks for BC formats; a 3D slice is that many rows. */
        ok = detile_to_linear(&t->surface, t->cpu, mip, layer, mapped + fp[i].Offset,
                              fp[i].Footprint.RowPitch, (UINT64)fp[i].Footprint.RowPitch * rows[i]);
    }
    ID3D12Resource_Unmap(buffer, 0, NULL);
    if (!ok)
    {
        static LONG warned;
        if (!InterlockedExchange(&warned, 1))
            rebox_log("d3d12_xs: detiling a placed texture failed (format %u, %ux%u)", t->surface.format,
                         t->surface.width, t->surface.height);
        ID3D12Resource_Release(buffer);
        free(fp);
        free(rows);
        return NULL;
    }
    barrier.Transition.pResource = t->resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = t->state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    if (t->state != D3D12_RESOURCE_STATE_COPY_DEST)
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    for (UINT i = 0; i < t->subresources; i++)
    {
        D3D12_TEXTURE_COPY_LOCATION dst = { t->resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, { .SubresourceIndex = i } };
        D3D12_TEXTURE_COPY_LOCATION src = { buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, { .PlacedFootprint = fp[i] } };
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = t->state;
    if (t->state != D3D12_RESOURCE_STATE_COPY_DEST)
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    free(fp);
    free(rows);
    return buffer;
}

/* Uploads in flight: their buffers, list and allocator live until the fence says the GPU is done.
 * Nothing waits on the CPU; a queue other than the one an upload ran on waits for it on the GPU
 * before running the title's lists, so a texture is never read before it's filled. */
struct batch
{
    ID3D12Fence *fence;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12CommandQueue *queue; /* not a reference: only compared */
    ID3D12Resource *buffers[64];
    UINT count;
};

static struct batch *batches[32];
static SRWLOCK batch_lock = SRWLOCK_INIT;

static void batch_free(struct batch *b)
{
    while (b->count)
        ID3D12Resource_Release(b->buffers[--b->count]);
    if (b->fence) ID3D12Fence_Release(b->fence);
    if (b->list) ID3D12GraphicsCommandList_Release(b->list);
    if (b->alloc) ID3D12CommandAllocator_Release(b->alloc);
    free(b);
}

/* Frees finished batches and makes queue wait for unfinished ones another queue ran. Returns a
 * free slot, or -1 when all are busy (the dirty textures then wait for a later submission). */
static int batches_reclaim(ID3D12CommandQueue *queue)
{
    int free_slot = -1;

    AcquireSRWLockExclusive(&batch_lock);
    for (int i = 0; i < (int)ARRAYSIZE(batches); i++)
    {
        struct batch *b = batches[i];

        if (b && ID3D12Fence_GetCompletedValue(b->fence) >= 1)
        {
            batch_free(b);
            batches[i] = b = NULL;
        }
        if (!b && free_slot < 0)
            free_slot = i;
        else if (b && b->queue != queue)
            ID3D12CommandQueue_Wait(queue, b->fence, 1);
    }
    ReleaseSRWLockExclusive(&batch_lock);
    return free_slot;
}

void title_textures_upload(ID3D12CommandQueue *queue)
{
    struct title_texture *t;
    struct batch *b = NULL;
    ID3D12Device *device = NULL;
    D3D12_COMMAND_QUEUE_DESC qd;
    int slot;

    if (!textures)
        return;
    slot = batches_reclaim(queue);
    AcquireSRWLockExclusive(&lock);
    submissions++;
    watch();
    ReleaseSRWLockExclusive(&lock);
    if (!dirty_count || slot < 0)
        return;
    qd = ID3D12CommandQueue_GetDesc(queue);
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && qd.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return;
    if (FAILED(ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&device)))
        return;
    if (!(b = calloc(1, sizeof(*b)))
            || FAILED(ID3D12Device_CreateCommandAllocator(device, qd.Type, &IID_ID3D12CommandAllocator, (void **)&b->alloc))
            || FAILED(ID3D12Device_CreateCommandList(device, 0, qd.Type, b->alloc, NULL, &IID_ID3D12GraphicsCommandList,
                                                     (void **)&b->list))
            || FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&b->fence)))
        goto done;
    b->queue = queue;

    /* Under the lock so a texture can't be destroyed while its upload is recorded; up to 64 a
     * submission, the rest the next time. */
    AcquireSRWLockExclusive(&lock);
    for (t = textures; t && b->count < ARRAYSIZE(b->buffers); t = t->next)
        if (t->dirty && still_readable(t))
        {
            t->dirty = FALSE;
            InterlockedDecrement(&dirty_count);
            t->fingerprint = fingerprint(t);
            if ((b->buffers[b->count] = record_upload(device, b->list, t)))
            {
                b->count++;
                t->uploads++;
            }
        }
    ReleaseSRWLockExclusive(&lock);
    if (FAILED(ID3D12GraphicsCommandList_Close(b->list)) || !b->count)
        goto done;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&b->list);
    if (FAILED(ID3D12CommandQueue_Signal(queue, b->fence, 1)))
    {
        /* Can't tell when it's done: wait for the queue the slow way. */
        for (int i = 0; i < 1000 && ID3D12Fence_GetCompletedValue(b->fence) < 1; i++)
            Sleep(1);
        goto done;
    }
    {
        static LONG uploads;
        if (InterlockedIncrement(&uploads) <= 8)
            rebox_log("d3d12_xs: uploaded %u placed texture(s) from title memory", b->count);
    }
    /* Another queue may have taken the slot meanwhile: any free one will do. */
    AcquireSRWLockExclusive(&batch_lock);
    for (int i = 0; i < (int)ARRAYSIZE(batches) && b; i++)
        if (!batches[i])
            batches[i] = b, b = NULL;
    ReleaseSRWLockExclusive(&batch_lock);
    if (b)
        for (int i = 0; i < 1000 && ID3D12Fence_GetCompletedValue(b->fence) < 1; i++)
            Sleep(1);

done:
    if (b)
        batch_free(b);
    if (device) ID3D12Device_Release(device);
}

/* For tests and tools: the size of a surface in title memory, and tiling linear data into it. */
UINT64 WINAPI ReboxDetileSize(const struct detile_surface *s)
{
    return detile_size(s);
}

BOOL WINAPI ReboxTile(const struct detile_surface *s, void *surface, UINT mip, UINT layer, const void *linear,
                         UINT64 row_pitch, UINT64 slice_pitch)
{
    return detile_from_linear(s, surface, mip, layer, linear, row_pitch, slice_pitch);
}
