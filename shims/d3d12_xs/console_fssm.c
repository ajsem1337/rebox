/* Deferred console compute shaders from the game's own containers (docs/anyps5.md).
 *
 * A console shader arrives as DXBC whose DXIL part is an XBCH header and a zlib stream (with a
 * NOBC stub where LLVM bitcode would be) holding FSSM: GFX10.3 code plus its register setup.
 * Measured on all 2,555 compute shaders of the target title's the shader archive against the PS09 PSO cache
 * (2026-11-22):
 *   0x104  float mode (RSRC1 bits 12-19): equal to PS09's RSRC1 for 2,472
 *   0x138  COMPUTE_PGM_RSRC2: user SGPR count equal for all; the enables and LDS size describe
 *          this code (PS09 holds the driver's recompile, which may differ)
 *   0x13C, 0x140, 0x144  thread-group size: equal for all
 *   0x150  the 24-byte shader ID PS09 also carries
 *   0x168  code (u32 at 0xC0 plus 0x28), then the tail Leonida parses
 *   0xD4   bit 5: wave32, clear for wave64 (americas-recomp bd0a9013d3; of the 2,598 we matched,
 *          the 1,464 with it set are all wave32 by sa5_analyse, and 828 of the others wave64;
 *          the analysis says wave32 for the other 306, which have nothing it can tell by)
 * RSRC1 itself isn't in the header; AnyPS5 needs only its float mode, DX10 clamp and IEEE bits,
 * and PS09's are 0x402c03xx throughout (DX10 clamp on, IEEE off). */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "console_compile.h"
#include "console_fssm.h"
#include "puff.h"

#define FSSM_WAVE_FLAGS 0xd4
#define FSSM_WAVE32 0x20
#define FSSM_FLOAT_MODE 0x104
#define FSSM_RSRC2 0x138
#define FSSM_THREADS 0x13c
#define FSSM_CODE_POINTER 0xc0
#define FSSM_MIN_SIZE 0x16c

static UINT32 u32(const BYTE *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (UINT32)p[3] << 24;
}

/* The FSSM a console container holds, inflated (free it), or NULL. */
static BYTE *inflate_fssm(const BYTE *data, SIZE_T size, SIZE_T *out_size)
{
    const BYTE *xbch = NULL;

    for (SIZE_T i = 0; i + 4 <= size && !xbch; i++)
        if (!memcmp(data + i, "XBCH", 4))
            xbch = data + i;
    if (!xbch)
        return NULL;
    /* The zlib stream follows the XBCH header somewhere: a CMF/FLG pair (deflate, 32 KiB window,
     * check bits) whose stream inflates to FSSM. */
    for (const BYTE *z = xbch; z + 2 < data + size; z++)
    {
        unsigned long dest_len = 0, source_len = (unsigned long)(data + size - z - 2);
        BYTE *out;

        if (z[0] != 0x78 || ((z[0] << 8) | z[1]) % 31)
            continue;
        if (puff(NULL, &dest_len, z + 2, &source_len) || dest_len < FSSM_MIN_SIZE || !(out = malloc(dest_len)))
            continue;
        source_len = (unsigned long)(data + size - z - 2);
        if (!puff(out, &dest_len, z + 2, &source_len) && !memcmp(out, "FSSM", 4))
        {
            *out_size = dest_len;
            return out;
        }
        free(out);
    }
    return NULL;
}

void *console_fssm_compute(const void *container, SIZE_T size, SIZE_T *blob_size)
{
    struct rebox_deferred_shader *h;
    SIZE_T fssm_size, code_offset, code_bytes;
    BYTE *fssm = inflate_fssm(container, size, &fssm_size);

    if (!fssm)
        return NULL;
    code_offset = u32(fssm + FSSM_CODE_POINTER) + 0x28;
    if (code_offset < FSSM_MIN_SIZE - 4 || code_offset % 4 || code_offset >= fssm_size)
    {
        free(fssm);
        return NULL;
    }
    code_bytes = (fssm_size - code_offset) & ~(SIZE_T)3;
    if (!(h = calloc(1, sizeof(*h) + code_bytes)))
    {
        free(fssm);
        return NULL;
    }
    h->magic = REBOX_DEFERRED_MAGIC;
    h->version = 1;
    h->size = sizeof(*h) + code_bytes;
    h->stage = 0x20; /* VK_SHADER_STAGE_COMPUTE_BIT */
    h->wave_size = (u32(fssm + FSSM_WAVE_FLAGS) & FSSM_WAVE32) ? 32 : 64;
    h->workgroup_size[0] = u32(fssm + FSSM_THREADS);
    h->workgroup_size[1] = u32(fssm + FSSM_THREADS + 4);
    h->workgroup_size[2] = u32(fssm + FSSM_THREADS + 8);
    h->rsrc1 = 0x40000000u | 1u << 21 /* DX10_CLAMP */ | (u32(fssm + FSSM_FLOAT_MODE) & 0xff) << 12;
    h->rsrc2 = u32(fssm + FSSM_RSRC2);
    h->code_offset = sizeof(*h);
    h->code_dwords = (UINT32)(code_bytes / 4);
    h->code_address = 0x10000000000ull;
    memcpy(h + 1, fssm + code_offset, code_bytes);
    free(fssm);
    *blob_size = h->size;
    return h;
}

/* For checks over shader dumps (smoke.exe --fssm-check): the SCRD console_fssm_compute builds, in
 * memory the caller frees with ReboxConsoleFree. */
void *WINAPI ReboxConsoleFromContainer(const void *container, SIZE_T size, SIZE_T *blob_size)
{
    return console_fssm_compute(container, size, blob_size);
}

void WINAPI ReboxConsoleFree(void *blob)
{
    free(blob);
}
