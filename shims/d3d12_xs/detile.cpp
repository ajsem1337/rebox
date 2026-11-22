/* Gfx10 texture layouts through AMD's addrlib; see detile.h.
 *
 * addrlib is created as Navi21 (FAMILY_NV, external revision 0x29, gb_addr_config 0x444, the
 * values Mesa's amdgpu_devices.c records). The modes used here, SW_4KB_S and SW_64KB_D, have no
 * pipe or bank XOR, so the pipe configuration doesn't change them: checked against bcpack-rs's
 * measured layout of the target title's texture pools (docs/d3d12x.md, "Texture tiling"). */

#include "detile.h"

#ifdef REBOX_ADDRLIB

#include <cstdlib>
#include <cstring>

#include "addrinterface.h"

static void *ADDR_API alloc_mem(const ADDR_ALLOCSYSMEM_INPUT *in) { return malloc(in->sizeInBytes); }
static ADDR_E_RETURNCODE ADDR_API free_mem(const ADDR_FREESYSMEM_INPUT *in)
{
    free(in->pVirtAddr);
    return ADDR_OK;
}

static ADDR_HANDLE lib;

static BOOL CALLBACK create(INIT_ONCE *, void *, void **)
{
    ADDR_CREATE_INPUT in = {};
    ADDR_CREATE_OUTPUT out = {};

    in.size = sizeof(in);
    out.size = sizeof(out);
    in.chipEngine = 0xd; /* CIASICIDGFXENGINE_ARCTICISLAND */
    in.chipFamily = 0x8f; /* FAMILY_NV */
    in.chipRevision = 0x29;
    in.callbacks.allocSysMem = alloc_mem;
    in.callbacks.freeSysMem = free_mem;
    in.regValue.gbAddrConfig = 0x444;
    if (AddrCreate(&in, &out) == ADDR_OK)
        lib = out.hLib;
    return TRUE;
}

static ADDR_HANDLE handle()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, create, nullptr, nullptr);
    return lib;
}

static AddrSwizzleMode swizzle(enum detile_mode mode)
{
    return mode == DETILE_4KB_S ? ADDR_SW_4KB_S : mode == DETILE_64KB_D ? ADDR_SW_64KB_D : ADDR_SW_LINEAR;
}

/* addrlib's format for an element of this size: BC formats by block, others by width. */
static AddrFormat addr_format(DXGI_FORMAT f, UINT bytes, UINT block)
{
    if (block == 4)
        return bytes == 8 ? ADDR_FMT_BC1 : ADDR_FMT_BC3;
    switch (bytes)
    {
    case 1: return ADDR_FMT_8;
    case 2: return ADDR_FMT_16;
    case 4: return ADDR_FMT_32;
    case 8: return ADDR_FMT_32_32;
    default: return ADDR_FMT_32_32_32_32;
    }
}

static BOOL surface_info(const struct detile_surface *s, ADDR2_COMPUTE_SURFACE_INFO_OUTPUT *out,
                         ADDR2_MIP_INFO *mips, UINT *bytes, UINT *block)
{
    ADDR2_COMPUTE_SURFACE_INFO_INPUT in = {};
    ADDR_HANDLE h = handle();

    *bytes = detile_element(s->format, block);
    if (!h || !*bytes || !s->width || !s->height || !s->mips || s->mips > 16)
        return FALSE;
    in.size = sizeof(in);
    out->size = sizeof(*out);
    out->pMipInfo = mips;
    in.swizzleMode = swizzle(s->mode);
    in.resourceType = s->volume ? ADDR_RSRC_TEX_3D : ADDR_RSRC_TEX_2D;
    in.format = addr_format(s->format, *bytes, *block);
    in.bpp = *bytes * 8;
    in.width = s->width;
    in.height = s->height;
    in.numSlices = s->layers ? s->layers : 1;
    in.numMipLevels = s->mips;
    in.numSamples = 1;
    in.numFrags = 1;
    in.flags.texture = 1;
    return Addr2ComputeSurfaceInfo(h, &in, out) == ADDR_OK;
}

UINT64 detile_size(const struct detile_surface *s)
{
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT out = {};
    ADDR2_MIP_INFO mips[16] = {};
    UINT bytes, block;

    return surface_info(s, &out, mips, &bytes, &block) ? out.surfSize : 0;
}

static BOOL copy(const struct detile_surface *s, void *surface, UINT mip, UINT layer, void *linear, UINT64 row_pitch,
                 UINT64 slice_pitch, BOOL to_linear)
{
    ADDR2_COMPUTE_SURFACE_INFO_OUTPUT info = {};
    ADDR2_MIP_INFO mips[16] = {};
    ADDR2_COPY_MEMSURFACE_INPUT in = {};
    ADDR2_COPY_MEMSURFACE_REGION region = {};
    UINT bytes, block, w, h, d;

    if (mip >= s->mips || !surface_info(s, &info, mips, &bytes, &block))
        return FALSE;
    w = s->width >> mip ? s->width >> mip : 1;
    h = s->height >> mip ? s->height >> mip : 1;
    d = s->volume ? (s->layers >> mip ? s->layers >> mip : 1) : 1;
    if (!s->volume && layer >= (s->layers ? s->layers : 1))
        return FALSE;
    in.size = sizeof(in);
    in.swizzleMode = swizzle(s->mode);
    in.format = addr_format(s->format, bytes, block);
    in.flags.texture = 1;
    in.resourceType = s->volume ? ADDR_RSRC_TEX_3D : ADDR_RSRC_TEX_2D;
    in.bpp = bytes * 8;
    in.unAlignedDims.width = s->width;
    in.unAlignedDims.height = s->height;
    in.unAlignedDims.depth = s->layers ? s->layers : 1;
    in.numMipLevels = s->mips;
    in.numSamples = 1;
    in.pitchInElement = info.pitch;
    in.pMappedSurface = surface;
    region.size = sizeof(region);
    region.slice = s->volume ? 0 : layer;
    region.mipId = mip;
    region.copyDims.width = (w + block - 1) / block;
    region.copyDims.height = (h + block - 1) / block;
    region.copyDims.depth = d;
    region.pMem = linear;
    region.memRowPitch = row_pitch;
    region.memSlicePitch = slice_pitch;
    return (to_linear ? Addr2CopySurfaceToMem(handle(), &in, &region, 1)
                      : Addr2CopyMemToSurface(handle(), &in, &region, 1)) == ADDR_OK;
}

BOOL detile_to_linear(const struct detile_surface *s, const void *surface, UINT mip, UINT layer, void *linear,
                      UINT64 row_pitch, UINT64 slice_pitch)
{
    return copy(s, const_cast<void *>(surface), mip, layer, linear, row_pitch, slice_pitch, TRUE);
}

BOOL detile_from_linear(const struct detile_surface *s, void *surface, UINT mip, UINT layer, const void *linear,
                        UINT64 row_pitch, UINT64 slice_pitch)
{
    return copy(s, surface, mip, layer, const_cast<void *>(linear), row_pitch, slice_pitch, FALSE);
}

#else

UINT64 detile_size(const struct detile_surface *) { return 0; }
BOOL detile_to_linear(const struct detile_surface *, const void *, UINT, UINT, void *, UINT64, UINT64) { return FALSE; }
BOOL detile_from_linear(const struct detile_surface *, void *, UINT, UINT, const void *, UINT64, UINT64) { return FALSE; }

#endif

UINT detile_element(DXGI_FORMAT f, UINT *block)
{
    *block = 1;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) /* BC1, BC4 */
    {
        *block = 4;
        return 8;
    }
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) /* BC2, BC3, BC5, BC6H, BC7 */
    {
        *block = 4;
        return 16;
    }
    if (f >= 1 && f <= 4) return 16;   /* R32G32B32A32 */
    if (f >= 9 && f <= 22) return 8;   /* R16G16B16A16, R32G32, R32G8X24 and its views */
    if (f >= 23 && f <= 47) return 4;  /* R10G10B10A2 .. R24G8 and its views */
    if (f >= 48 && f <= 59) return 2;  /* R8G8, R16, D16 */
    if (f >= 60 && f <= 65) return 1;  /* R8, A8 */
    if (f == 67) return 4;             /* R9G9B9E5 */
    if (f == 85 || f == 86 || f == 115) return 2; /* B5G6R5, B5G5R5A1, B4G4R4A4 */
    if (f >= 87 && f <= 93) return 4;  /* B8G8R8A8, B8G8R8X8 and views, R10G10B10_XR_BIAS_A2 */
    return 0;
}
