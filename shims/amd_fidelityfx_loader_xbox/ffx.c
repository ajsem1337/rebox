/* Stand-in for amd_fidelityfx_loader_xbox.dll: the FidelityFX API with a
 * bilinear upscaler.
 *
 * The console upscaler's shaders are precompiled ISA (ACCC sections, no
 * DXIL), so under d3d12_xs they would become no-op fallbacks and the output
 * would never be written. This stub provides the upscale effect instead,
 * with d3d12_xs's ReboxBlit:
 *   - queries report FSR's quality-mode ratios, so render sizes are as the
 *     title expects, but no jitter: without temporal accumulation an
 *     unjittered camera gives a stable image;
 *   - dispatch scales the render-size part of the colour input into the
 *     output on the title's command list.
 * Other effects (frame generation) report no provider, as the console does
 * when their DLL is absent.
 *
 * Descriptor types and layouts are the public FidelityFX API's, checked
 * against amd_fidelityfx_upscaler_xbox.dll: its provider claims
 * (type & 0xff0000) == 0x10000, configures 0x10007, queries 0x10002-0x1000a,
 * and its dispatch reads the command list at +0x10, colour at +0x18, output
 * at +0x138, render size at +0x178 and upscale size at +0x180. */

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

#include "../common/rebox_log.h"

typedef struct ffx_header
{
    uint64_t type;
    struct ffx_header *next;
} ffx_header;

enum
{
    FFX_OK = 0,
    FFX_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_ERROR_RUNTIME = 3,
    FFX_NO_PROVIDER = 4,
    FFX_ERROR_MEMORY = 5,
    FFX_ERROR_PARAMETER = 6,
};

#define EFFECT_UPSCALE 0x10000
#define CREATE_UPSCALE 0x10000
#define DISPATCH_UPSCALE 0x10001
#define QUERY_UPSCALE_RATIO 0x10003
#define QUERY_RENDER_RESOLUTION 0x10004
#define QUERY_JITTER_PHASE_COUNT 0x10005
#define QUERY_JITTER_OFFSET 0x10006
#define QUERY_GET_VERSIONS 4

#define FIELD(desc, offset, type) (*(type *)((BYTE *)(desc) + (offset)))

struct context
{
    UINT32 max_render_w, max_render_h, max_upscale_w, max_upscale_h;
};

static float quality_ratio(UINT32 mode)
{
    /* NATIVEAA, QUALITY, BALANCED, PERFORMANCE, ULTRA_PERFORMANCE */
    static const float ratios[] = { 1.0f, 1.5f, 1.7f, 2.0f, 3.0f };
    return mode < 5 ? ratios[mode] : 1.0f;
}

static ffx_header *find(ffx_header *h, uint64_t type)
{
    while (h && h->type != type)
        h = h->next;
    return h;
}

uint32_t ffxCreateContext(void **context, ffx_header *desc, const void *alloc)
{
    ffx_header *up;
    struct context *c;

    if (!context || !desc)
        return FFX_ERROR_PARAMETER;
    *context = NULL;
    if (!(up = find(desc, CREATE_UPSCALE)))
    {
        rebox_log("ffx stub: no provider for effect %#llx", (unsigned long long)(desc->type & 0xff0000));
        return FFX_NO_PROVIDER;
    }
    if (!(c = calloc(1, sizeof(*c))))
        return FFX_ERROR_MEMORY;
    c->max_render_w = FIELD(up, 0x14, UINT32);
    c->max_render_h = FIELD(up, 0x18, UINT32);
    c->max_upscale_w = FIELD(up, 0x1c, UINT32);
    c->max_upscale_h = FIELD(up, 0x20, UINT32);
    rebox_log("ffx stub: upscale context (render up to %ux%u, output up to %ux%u): bilinear, no jitter",
                 c->max_render_w, c->max_render_h, c->max_upscale_w, c->max_upscale_h);
    *context = c;
    return FFX_OK;
}

uint32_t ffxDestroyContext(void **context, const void *alloc)
{
    if (!context)
        return FFX_ERROR_PARAMETER;
    free(*context);
    *context = NULL;
    return FFX_OK;
}

uint32_t ffxConfigure(void **context, const ffx_header *desc)
{
    return desc ? FFX_OK : FFX_ERROR_PARAMETER;
}

uint32_t ffxQuery(void **context, ffx_header *desc)
{
    if (!desc)
        return FFX_ERROR_PARAMETER;
    switch (desc->type)
    {
    case QUERY_UPSCALE_RATIO:
        if (FIELD(desc, 0x18, float *))
            *FIELD(desc, 0x18, float *) = quality_ratio(FIELD(desc, 0x10, UINT32));
        return FFX_OK;
    case QUERY_RENDER_RESOLUTION:
    {
        float r = quality_ratio(FIELD(desc, 0x18, UINT32));
        if (FIELD(desc, 0x20, UINT32 *))
            *FIELD(desc, 0x20, UINT32 *) = (UINT32)(FIELD(desc, 0x10, UINT32) / r);
        if (FIELD(desc, 0x28, UINT32 *))
            *FIELD(desc, 0x28, UINT32 *) = (UINT32)(FIELD(desc, 0x14, UINT32) / r);
        return FFX_OK;
    }
    case QUERY_JITTER_PHASE_COUNT:
        if (FIELD(desc, 0x18, INT32 *))
            *FIELD(desc, 0x18, INT32 *) = 1;
        return FFX_OK;
    case QUERY_JITTER_OFFSET:
        if (FIELD(desc, 0x18, float *))
            *FIELD(desc, 0x18, float *) = 0.0f;
        if (FIELD(desc, 0x20, float *))
            *FIELD(desc, 0x20, float *) = 0.0f;
        return FFX_OK;
    case QUERY_GET_VERSIONS:
    {
        /* { header; createDescType; device; uint64 *count; uint64 *ids; const char **names } */
        static const char *name = "rebox bilinear";
        BOOL ours = (FIELD(desc, 0x10, uint64_t) & 0xff0000) == EFFECT_UPSCALE;
        uint64_t *count = FIELD(desc, 0x20, uint64_t *);

        if (!count)
            return FFX_ERROR_PARAMETER;
        if (ours && *count && FIELD(desc, 0x28, uint64_t *))
            FIELD(desc, 0x28, uint64_t *)[0] = 1;
        if (ours && *count && FIELD(desc, 0x30, const char **))
            FIELD(desc, 0x30, const char **)[0] = name;
        *count = ours ? 1 : 0;
        return FFX_OK;
    }
    default:
        return context ? FFX_ERROR_UNKNOWN_DESCTYPE : FFX_NO_PROVIDER;
    }
}

uint32_t ffxDispatch(void **context, const ffx_header *desc)
{
    static HRESULT (WINAPI *blit)(void *, void *, UINT, UINT, void *, UINT, UINT);
    static LONG logged;
    HRESULT hr;

    if (!context || !*context || !desc)
        return FFX_ERROR_PARAMETER;
    if (desc->type != DISPATCH_UPSCALE)
        return FFX_ERROR_UNKNOWN_DESCTYPE;
    if (!blit && !(blit = (void *)GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "ReboxBlit")))
        return FFX_ERROR_RUNTIME;
    hr = blit(FIELD(desc, 0x10, void *), FIELD(desc, 0x18, void *), FIELD(desc, 0x178, UINT32),
              FIELD(desc, 0x17c, UINT32), FIELD(desc, 0x138, void *), FIELD(desc, 0x180, UINT32),
              FIELD(desc, 0x184, UINT32));
    if (!InterlockedExchange(&logged, 1))
        rebox_log("ffx stub: first upscale %ux%u -> %ux%u: %#lx", FIELD(desc, 0x178, UINT32),
                     FIELD(desc, 0x17c, UINT32), FIELD(desc, 0x180, UINT32), FIELD(desc, 0x184, UINT32), hr);
    return SUCCEEDED(hr) ? FFX_OK : FFX_ERROR_RUNTIME;
}
