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
 * and PS09's are 0x402c03xx throughout (DX10 clamp on, IEEE off).
 *
 * An older layout, 16 bytes shorter (a UE5 title, SDK 0x20002; told apart by its code at 0x158):
 * the same fields 8 bytes earlier, with no gap before the shader ID. Float mode at 0xFC (0xC0 in
 * all of a UE5 title's, as in the target title's), RSRC2 at 0x130 (0x990: 8 user SGPRs, TGID X/Y; 0x8990 adds
 * LDS), the thread-group size at 0x134-0x13C ((8,8,1), (16,16,1), (4,4,4)), the 24-byte shader ID
 * at 0x140. the target title's wave32 bit (0xD4 bit 5) is clear in all of them, which says little, so the wave
 * size is left for sa5_analyse to infer from the code. */

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
#define FSSM_SHORT_CODE 0x158 /* the older layout */
#define FSSM_SHORT_FLOAT_MODE 0xfc
#define FSSM_SHORT_RSRC2 0x130
#define FSSM_SHORT_THREADS 0x134

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
        if (puff(NULL, &dest_len, z + 2, &source_len) || dest_len < FSSM_SHORT_CODE + 4 || !(out = malloc(dest_len)))
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
    BOOL short_layout;

    if (!fssm)
        return NULL;
    code_offset = u32(fssm + FSSM_CODE_POINTER) + 0x28;
    short_layout = code_offset == FSSM_SHORT_CODE;
    if ((!short_layout && code_offset < FSSM_MIN_SIZE - 4) || code_offset % 4 || code_offset >= fssm_size)
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
    if (short_layout)
    {
        h->wave_size = 0; /* inferred from the code (console_compile.c) */
        h->workgroup_size[0] = u32(fssm + FSSM_SHORT_THREADS);
        h->workgroup_size[1] = u32(fssm + FSSM_SHORT_THREADS + 4);
        h->workgroup_size[2] = u32(fssm + FSSM_SHORT_THREADS + 8);
        h->rsrc1 = 0x40000000u | 1u << 21 /* DX10_CLAMP */ | (u32(fssm + FSSM_SHORT_FLOAT_MODE) & 0xff) << 12;
        h->rsrc2 = u32(fssm + FSSM_SHORT_RSRC2);
    }
    else
    {
        h->wave_size = (u32(fssm + FSSM_WAVE_FLAGS) & FSSM_WAVE32) ? 32 : 64;
        h->workgroup_size[0] = u32(fssm + FSSM_THREADS);
        h->workgroup_size[1] = u32(fssm + FSSM_THREADS + 4);
        h->workgroup_size[2] = u32(fssm + FSSM_THREADS + 8);
        h->rsrc1 = 0x40000000u | 1u << 21 /* DX10_CLAMP */ | (u32(fssm + FSSM_FLOAT_MODE) & 0xff) << 12;
        h->rsrc2 = u32(fssm + FSSM_RSRC2);
    }
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

/* ------------------------------------------------------------------ graphics stages
 *
 * a UE5 title's graphics FSSM (older layout) carries less register state than compute: the code (at
 * the u32 at 0xC0 plus 0x28, its size in bytes at 0xC8), SPI_SHADER_PGM_RSRC1 at 0x108 and the
 * float mode at 0xFC. The rest comes from the code (docs/anyps5.md; tools/fssm-ps-plan.py and
 * fssm-vs-plan.py do the same over dumps):
 * - vertex: the first instruction calls the fetch shader (s_swappc_b64 s[4:5], s[4:5]), which
 *   leaves input r in v[4+4r..7+4r]; the inputs are the consecutive slots from v4 it reads before
 *   writing. 32 user SGPRs.
 * - pixel: the prologue's s_mov_b32 m0, sN takes the primitive mask from the SGPR after the user
 *   SGPRs (24 in every a UE5 title PS); interpolants from v_interp_* (flat: only v_interp_mov);
 *   export formats from exp (packed: FP16, else the 32-bit format of the channels written);
 *   depth from an MRTZ export. */

#define FSSM_GFX_CODE_SIZE 0xc8
#define FSSM_GFX_RSRC1 0x108

/* The next instruction's size in dwords (GFX10). */
static UINT gfx10_size(const UINT32 *code, UINT left)
{
    UINT32 w = code[0], top9 = w >> 23, top6 = w >> 26;

    if (top9 == 0x17f)                                   /* SOPP */
        return 1;
    if (top9 == 0x17d)                                   /* SOP1 */
        return 1 + ((w & 0xff) == 0xff);
    if (top9 == 0x17e)                                   /* SOPC */
        return 1 + ((w & 0xff) == 0xff || ((w >> 8) & 0xff) == 0xff);
    if (w >> 28 == 0xb)                                  /* SOPK */
        return 1 + (((w >> 23) & 0x1f) == 0x15);
    if (w >> 30 == 2)                                    /* SOP2 */
        return 1 + ((w & 0xff) == 0xff || ((w >> 8) & 0xff) == 0xff);
    if (top6 == 0x35 || top6 == 0x33)                    /* VOP3, VOP3P */
    {
        UINT32 s = left > 1 ? code[1] : 0;
        return 2 + ((s & 0x1ff) == 0xff || ((s >> 9) & 0x1ff) == 0xff || ((s >> 18) & 0x1ff) == 0xff);
    }
    if (top6 == 0x3d || top6 == 0x36 || top6 == 0x3a || top6 == 0x38 || top6 == 0x37 || top6 == 0x3e)
        return 2;                                        /* SMEM DS MTBUF MUBUF FLAT EXP */
    if (top6 == 0x3c)                                    /* MIMG, with NSA address dwords */
        return 2 + (((w >> 1) & 3) ? (((w >> 1) & 3) * 4 + 3) / 4 : 0);
    if (top6 == 0x32)                                    /* VINTRP */
        return 1;
    {
        UINT32 src0 = w & 0x1ff;                         /* VOP1, VOPC, VOP2 */
        return 1 + (src0 == 0xff || src0 == 0xf9 || src0 == 0xfa || src0 == 0xe9 || src0 == 0xea);
    }
}

/* VGPRs an instruction reads as sources, as a bitmask of v0-v63; and writes. */
static void gfx10_vgprs(const UINT32 *ins, UINT size, UINT64 *read, UINT64 *written)
{
    UINT32 w = ins[0], top6 = w >> 26;
#define V(set, r) do { if ((r) < 64) *(set) |= 1ull << (r); } while (0)
#define SRC9(set, x) do { if ((x) >= 256 && (x) < 320) V(set, (x) - 256); } while (0)
    if (w >> 31 == 0)                                    /* VOP2 */
    {
        SRC9(read, w & 0x1ff);
        V(read, (w >> 9) & 0xff);
        V(written, (w >> 17) & 0xff);
    }
    else if (w >> 25 == 0x3f)                            /* VOP1 */
    {
        SRC9(read, w & 0x1ff);
        V(written, (w >> 17) & 0xff);
    }
    else if (w >> 25 == 0x3e)                            /* VOPC */
    {
        SRC9(read, w & 0x1ff);
        V(read, (w >> 9) & 0xff);
    }
    else if ((top6 == 0x35 || top6 == 0x33) && size > 1)
    {
        SRC9(read, ins[1] & 0x1ff);
        SRC9(read, (ins[1] >> 9) & 0x1ff);
        SRC9(read, (ins[1] >> 18) & 0x1ff);
        V(written, w & 0xff);
        V(written, (w & 0xff) + 1);
    }
    else if (top6 == 0x3e && size > 1)                   /* EXP */
    {
        for (UINT k = 0; k < 4; k++)
            V(read, (ins[1] >> (8 * k)) & 0xff);
    }
    else if ((top6 == 0x38 || top6 == 0x3a || top6 == 0x37 || top6 == 0x3c) && size > 1)
    {
        V(read, ins[1] & 0xff);
        for (UINT k = 0; k < 4; k++)
            V(written, ((ins[1] >> 8) & 0xff) + k);
    }
#undef SRC9
#undef V
}

void *console_fssm_graphics(const void *container, SIZE_T size, UINT32 stage, SIZE_T *blob_size)
{
    struct rebox_deferred_shader *h;
    struct rebox_deferred_graphics *g;
    SIZE_T fssm_size, code_offset, code_bytes;
    BYTE *fssm = inflate_fssm(container, size, &fssm_size);
    const UINT32 *code;
    UINT dwords, user = 0;

    if (!fssm)
        return NULL;
    code_offset = u32(fssm + FSSM_CODE_POINTER) + 0x28;
    code_bytes = fssm_size > FSSM_GFX_RSRC1 + 4 ? u32(fssm + FSSM_GFX_CODE_SIZE) : 0;
    if (code_offset % 4 || code_offset >= fssm_size || code_offset < FSSM_GFX_RSRC1 + 4)
    {
        free(fssm);
        return NULL;
    }
    code_bytes = min(code_bytes, fssm_size - code_offset) & ~(SIZE_T)3;
    code = (const UINT32 *)(fssm + code_offset);
    dwords = (UINT)(code_bytes / 4);
    if (!dwords || !(h = calloc(1, sizeof(*h) + sizeof(*g) + code_bytes)))
    {
        free(fssm);
        return NULL;
    }
    g = (struct rebox_deferred_graphics *)(h + 1);
    if (stage == 0x1)
    {
        UINT64 read, written, live = 0, done = 0;
        if ((code[0] & 0xffffff00u) != 0xbe842100u)       /* s_swappc_b64 s[4:5], ... */
        {
            free(h);
            free(fssm);
            return NULL;
        }
        /* VGPRs read before the code wrote them, after the fetch call. */
        for (UINT i = 1, n; i < dwords; i += n)
        {
            n = gfx10_size(code + i, dwords - i);
            read = written = 0;
            gfx10_vgprs(code + i, n, &read, &written);
            live |= read & ~done;
            done |= written;
            if (code[i] == 0xbf810000u)
                break;
        }
        while (g->input_count < 15 && (live >> (4 + 4 * g->input_count)) & 0xf)
            g->input_count++;
        user = 32;
    }
    else
    {
        UINT32 flat_only = 0, used = 0;
        user = ~0u;
        for (UINT i = 0, n; i < dwords; i += n)
        {
            UINT32 w = code[i];
            n = gfx10_size(code + i, dwords - i);
            if (user == ~0u && w >> 23 == 0x17d && ((w >> 16) & 0x7f) == 0x7c && ((w >> 8) & 0xff) == 3
                    && (w & 0xff) < 106)
                user = w & 0xff;                          /* s_mov_b32 m0, sN */
            if (w >> 26 == 0x32)                          /* v_interp_p1/p2/mov_f32 */
            {
                UINT attr = (w >> 10) & 0x3f, op = (w >> 16) & 3;
                if (attr < 32)
                {
                    if (!(used & (1u << attr)))
                        flat_only |= 1u << attr;
                    used |= 1u << attr;
                    if (op != 2)
                        flat_only &= ~(1u << attr);
                    g->interpolator_count = max(g->interpolator_count, attr + 1);
                }
            }
            if (w >> 26 == 0x3e && n == 2)                /* exp */
            {
                UINT target = (w >> 4) & 0x3f, en = w & 0xf, compr = (w >> 10) & 1, fmt;
                if (target < 8)
                {
                    fmt = compr ? 4 : en == 1 ? 1 : en == 3 ? 2 : en == 9 ? 3 : 9;
                    g->col_format = (g->col_format & ~(0xfu << (4 * target))) | fmt << (4 * target);
                }
                else if (target == 8)
                    g->depth_export = 1;
            }
            if (w == 0xbf810000u)
                break;
        }
        if (user == ~0u)
            user = 24;
        for (UINT n = 0; n < g->interpolator_count; n++)
            g->interpolators[n] = n | ((flat_only & (1u << n)) ? 1u << 10 : 0);
        g->input_ena = 0x2;                               /* perspective centre */
    }

    h->magic = REBOX_DEFERRED_MAGIC;
    h->version = 2;
    h->size = sizeof(*h) + sizeof(*g) + (UINT32)code_bytes;
    h->stage = stage;
    h->wave_size = 0;                                     /* inferred from the code (console_compile.c) */
    h->rsrc1 = (u32(fssm + FSSM_GFX_RSRC1) & ~(0xffu << 12)) | (u32(fssm + FSSM_SHORT_FLOAT_MODE) & 0xff) << 12;
    h->rsrc2 = user << 1;
    h->code_offset = sizeof(*h) + sizeof(*g);
    h->code_dwords = dwords;
    h->code_address = 0x10000000000ull;
    memcpy((BYTE *)h + h->code_offset, code, code_bytes);
    free(fssm);
    *blob_size = h->size;
    return h;
}
