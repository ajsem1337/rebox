#pragma once

#include <windows.h>
#include <dxgiformat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gfx10 texture layouts through AMD's addrlib (Mesa, MIT), created as Navi21; see
 * docs/d3d12x.md "Texture tiling". Without addrlib in the build (ADDRLIB unset), every call
 * fails and textures keep their own memory. */

enum detile_mode
{
    DETILE_LINEAR,  /* ADDR_SW_LINEAR: rows padded to 256 bytes */
    DETILE_4KB_S,   /* ADDR_SW_4KB_S */
    DETILE_64KB_D,  /* ADDR_SW_64KB_D */
};

struct detile_surface
{
    enum detile_mode mode;
    DXGI_FORMAT format;
    UINT width, height, layers; /* layers: array size (2D) or depth (3D) */
    UINT mips;
    BOOL volume;
    BOOL mip_major; /* 2D arrays: each mip's slices together (the target title's pools), not addrlib's
                       slice-major order; see detile.cpp */
};

/* The bytes the surface occupies in title memory, or 0 if addrlib can't lay it out. */
UINT64 detile_size(const struct detile_surface *s);

/* Copies one subresource (mip, layer) between the tiled surface and linear rows of row_pitch
 * bytes (row = a row of pixels, or of 4x4 blocks for BC formats). A 3D subresource's depth
 * slices follow each other, slice_pitch bytes apart. */
BOOL detile_to_linear(const struct detile_surface *s, const void *surface, UINT mip, UINT layer, void *linear,
                      UINT64 row_pitch, UINT64 slice_pitch);
BOOL detile_from_linear(const struct detile_surface *s, void *surface, UINT mip, UINT layer, const void *linear,
                        UINT64 row_pitch, UINT64 slice_pitch);

/* Bytes per element (pixel, or 4x4 block for BC formats) and the element's size in pixels, or
 * 0 for formats it doesn't know. */
UINT detile_element(DXGI_FORMAT format, UINT *block);

#ifdef __cplusplus
}
#endif
