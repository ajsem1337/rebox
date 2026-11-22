/* Detiles textures with d3d12_xs's own code (shims/d3d12_xs/detile.cpp, AMD addrlib), for
 * helpers/textures/texcheck.py, which compares the result with an independent decoder.
 *
 *   texcheck.exe JOBS OUT
 *
 * JOBS holds a u32 count, then per texture: u32 mode (detile_mode), format, width, height,
 * layers, mips, volume, mip_major; u64 size; size bytes of tiled memory. OUT gets, per texture,
 * u32 ok and u64 size, then the subresources in DDS order (each slice's mips in turn; a volume's
 * depth slices), each tightly packed: rows of visible elements (pixels, or 4x4 blocks). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../shims/d3d12_xs/detile.h"

static BOOL read_all(FILE *f, void *p, size_t n)
{
    return fread(p, 1, n, f) == n;
}

static unsigned dim(unsigned v, unsigned mip, unsigned block)
{
    v >>= mip;
    if (!v)
        v = 1;
    return (v + block - 1) / block;
}

int main(int argc, char **argv)
{
    FILE *in, *out;
    UINT32 count;

    if (argc != 3 || !(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb")))
    {
        fprintf(stderr, "usage: texcheck.exe JOBS OUT\n");
        return 2;
    }
    if (!read_all(in, &count, 4))
        return 2;
    for (UINT32 j = 0; j < count; j++)
    {
        UINT32 h[8];
        UINT64 size, total = 0;
        struct detile_surface s;
        BYTE *tiled, *linear = NULL;
        UINT bytes, block, ok = 1;

        if (!read_all(in, h, sizeof(h)) || !read_all(in, &size, 8) || !(tiled = malloc(size ? size : 1))
                || !read_all(in, tiled, size))
            return 2;
        memset(&s, 0, sizeof(s));
        s.mode = (enum detile_mode)h[0];
        s.format = (DXGI_FORMAT)h[1];
        s.width = h[2];
        s.height = h[3];
        s.layers = h[4];
        s.mips = h[5];
        s.volume = h[6];
        s.mip_major = h[7];
        if (!(bytes = detile_element(s.format, &block)) || detile_size(&s) > size)
            ok = 0;
        else if (s.volume)
        {
            UINT64 row = (UINT64)dim(s.width, 0, block) * bytes, slice = row * dim(s.height, 0, block);
            total = slice * s.layers;
            linear = malloc(total);
            ok = linear && detile_to_linear(&s, tiled, 0, 0, linear, row, slice);
        }
        else
        {
            for (UINT l = 0; l < s.layers; l++)
                for (UINT m = 0; m < s.mips; m++)
                    total += (UINT64)dim(s.width, m, block) * dim(s.height, m, block) * bytes;
            linear = malloc(total ? total : 1);
            for (UINT l = 0, at = 0; ok && linear && l < s.layers; l++)
                for (UINT m = 0; ok && m < s.mips; m++)
                {
                    UINT64 row = (UINT64)dim(s.width, m, block) * bytes, slice = row * dim(s.height, m, block);
                    ok = detile_to_linear(&s, tiled, m, l, linear + at, row, slice);
                    at += slice;
                }
            ok = ok && linear;
        }
        if (!ok)
            total = 0;
        fwrite(&ok, 4, 1, out);
        fwrite(&total, 8, 1, out);
        if (total)
            fwrite(linear, 1, total, out);
        free(linear);
        free(tiled);
    }
    fclose(out);
    return 0;
}
