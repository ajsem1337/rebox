/* Deferred console compute shaders from the game's own containers (docs/anyps5.md).
 *
 * A console shader arrives as DXBC whose DXIL part is an XBCH header and a zlib stream (with a
 * NOBC stub where LLVM bitcode would be) holding FSSM: GFX10.3 code plus its register setup.
 * Measured on all 2,555 compute shaders of one title's shader archive against the PS09 PSO cache
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
 * all of a UE5 title's, as in the newer layout), RSRC2 at 0x130 (0x990: 8 user SGPRs, TGID X/Y; 0x8990 adds
 * LDS), the thread-group size at 0x134-0x13C ((8,8,1), (16,16,1), (4,4,4)), the 24-byte shader ID
 * at 0x140. The newer layout's wave32 bit (0xD4 bit 5) is clear in all of them, which says little, so the wave
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

/* The console driver's own container, FSPX, in a DXBC part ACCC: what AMD FidelityFX's console
 * upscaler (FSR4) creates its compute passes from, with no DXIL (xcl, GPL-2.0-only, fdeacf0529).
 * 'FSPX', version, entry count, size, then 16-byte entries (type, offset, size, 0) from +0x10,
 * offsets from the start of FSPX. Type 0: the thread-group size as three u16 at +0x0C,
 * COMPUTE_PGM_RSRC1 at +0x14 and RSRC2 at +0x18; type 6: the code behind a 0x24-byte header, up to
 * its last s_endpgm (the driver pads with s_code_end). */
#define FSPX_HEAD 0
#define FSPX_CODE 6
#define FSPX_CODE_HEADER 0x24
#define S_ENDPGM 0xbf810000u

static const BYTE *fspx_entry(const BYTE *fspx, SIZE_T size, UINT32 type, SIZE_T *entry_size)
{
    UINT32 count = size >= 0x10 ? min(u32(fspx + 8), 32u) : 0;

    for (UINT32 e = 0; e < count && 0x10 + 16 * (SIZE_T)(e + 1) <= size; e++)
    {
        const BYTE *at = fspx + 0x10 + 16 * e;
        SIZE_T offset = u32(at + 4), bytes = u32(at + 8);

        if (u32(at) == type && offset <= size && bytes <= size - offset)
        {
            *entry_size = bytes;
            return fspx + offset;
        }
    }
    return NULL;
}

static void *fspx_compute(const BYTE *data, SIZE_T size, SIZE_T *blob_size)
{
    const BYTE *fspx = NULL, *head, *code;
    SIZE_T fspx_size = 0, head_size, code_size, dwords;
    struct rebox_deferred_shader *h;
    UINT32 parts, threads[3];

    if (size < 0x20 || memcmp(data, "DXBC", 4))
        return NULL;
    parts = u32(data + 0x1c);
    for (UINT32 i = 0; i < parts && 0x20 + 4 * (SIZE_T)(i + 1) <= size && !fspx; i++)
    {
        SIZE_T at = u32(data + 0x20 + 4 * i);

        if (at <= size - 8 && !memcmp(data + at, "ACCC", 4) && u32(data + at + 4) <= size - at - 8
                && u32(data + at + 4) >= 4 && !memcmp(data + at + 8, "FSPX", 4))
        {
            fspx = data + at + 8;
            fspx_size = u32(data + at + 4);
        }
    }
    if (!fspx || !(head = fspx_entry(fspx, fspx_size, FSPX_HEAD, &head_size)) || head_size < 0x1c
            || !(code = fspx_entry(fspx, fspx_size, FSPX_CODE, &code_size)) || code_size <= FSPX_CODE_HEADER)
        return NULL;
    for (int i = 0; i < 3; i++)
        threads[i] = head[0x0c + 2 * i] | head[0x0d + 2 * i] << 8;
    if (!threads[0] || !threads[1] || !threads[2] || threads[0] * threads[1] * threads[2] > 1024)
        return NULL;
    code += FSPX_CODE_HEADER;
    for (dwords = (code_size - FSPX_CODE_HEADER) / 4; dwords && u32(code + 4 * (dwords - 1)) != S_ENDPGM; dwords--)
        ;
    if (!dwords || !(h = calloc(1, sizeof(*h) + dwords * 4)))
        return NULL;
    h->magic = REBOX_DEFERRED_MAGIC;
    h->version = 1;
    h->size = (UINT32)(sizeof(*h) + dwords * 4);
    h->stage = 0x20; /* VK_SHADER_STAGE_COMPUTE_BIT */
    h->wave_size = 0; /* inferred from the code (console_compile.c) */
    memcpy(h->workgroup_size, threads, sizeof(threads));
    h->rsrc1 = u32(head + 0x14);
    h->rsrc2 = u32(head + 0x18);
    h->code_offset = sizeof(*h);
    h->code_dwords = (UINT32)dwords;
    h->code_address = 0x10000000000ull;
    memcpy(h + 1, code, dwords * 4);
    *blob_size = h->size;
    return h;
}

void *console_fssm_compute(const void *container, SIZE_T size, SIZE_T *blob_size)
{
    struct rebox_deferred_shader *h;
    SIZE_T fssm_size, code_offset, code_bytes;
    BYTE *fssm;
    BOOL short_layout;

    if ((h = fspx_compute(container, size, blob_size)))
        return h;
    if (!(fssm = inflate_fssm(container, size, &fssm_size)))
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

/* For tools that check shader dumps: the SCRD console_fssm_compute builds, in memory the caller
 * frees with ReboxConsoleFree. */
void *WINAPI ReboxConsoleFromContainer(const void *container, SIZE_T size, SIZE_T *blob_size)
{
    return console_fssm_compute(container, size, blob_size);
}

void WINAPI ReboxConsoleFree(void *blob)
{
    free(blob);
}

/* For tests: the deferred graphics stage console_fssm_graphics makes (stage 0x1, 0x10 or 0x80), in
 * memory the caller frees with ReboxConsoleFree. */
void *WINAPI ReboxConsoleGraphicsFromContainer(const void *container, SIZE_T size, UINT32 stage, SIZE_T *blob_size)
{
    return console_fssm_graphics(container, size, stage, blob_size);
}

/* ------------------------------------------------------------------ graphics stages
 *
 * The older graphics FSSM layout (Unreal Engine 5 titles) carries less register state than compute: the code (at
 * the u32 at 0xC0 plus 0x28, its size in bytes at 0xC8), SPI_SHADER_PGM_RSRC1 at 0x108 and the
 * float mode at 0xFC. The rest comes from the code (docs/anyps5.md):
 * - vertex: the first instruction calls the fetch shader (s_swappc_b64 s[4:5], s[4:5]), which
 *   leaves input r in v[4+4r..7+4r]; the inputs are the consecutive slots from v4 it reads before
 *   writing. Without that call, no inputs (full-screen passes). 32 user SGPRs.
 * - pixel: the prologue's s_mov_b32 m0, sN takes the primitive mask from the SGPR after the user
 *   SGPRs (24 in every PS measured); interpolants from v_interp_* (flat: only v_interp_mov);
 *   export formats from exp (packed: FP16, else the 32-bit format of the channels written);
 *   depth from an MRTZ export. */

#define FSSM_GFX_CODE_SIZE 0xc8
#define FSSM_GFX_RSRC1 0x108

/* The next instruction's size in dwords (GFX10). */
static UINT gfx10_size(const UINT32 *code, UINT left);

UINT console_gfx10_size(const UINT32 *code, UINT left)
{
    return left ? gfx10_size(code, left) : 0;
}

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

/* The FSSM's interface tables (xcl, crates/xcl/src/gpu/recomp/mod.rs, GPL-2.0-only; checked there
 * on every cached shader of three titles). Attribute and param numbers name signature registers,
 * numbered by what the code uses, not elements: a UE5 title's Slate pixel shader reads COLOR0 as attr0
 * and TEXCOORD1 as attr1. The tables sit after a sub-version at +0x10 (5 in a UE5 title's GDK, 6 adds
 * 8 bytes). Pixel: 32 records of 8 bytes at +0x130 (kind, register, mask, attribute number, 0, 0,
 * register again, 1 flat / 2 interpolated), the count at +0x100 after them, SPI_PS_INPUT_ENA at
 * +0x110. Vertex: 32 records of 6 bytes at +0x1D4 (kind, register, param number, 0, 0, 0), the
 * count at +0xC0 after them. FALSE if the tables aren't there or don't check out. */
static BOOL fssm_interface(const BYTE *fssm, SIZE_T size, UINT32 stage, struct rebox_deferred_graphics *g,
                           UINT32 *input_ena)
{
    SIZE_T shift, base;
    UINT n;

    if (size < 0x14)
        return FALSE;
    switch (u32(fssm + 0x10))
    {
    case 5: shift = 0; break;
    case 6: shift = 8; break;
    default: return FALSE;
    }
    if (stage == 0x10)
    {
        base = 0x130 + shift;
        if (base + 0x114 > size || (n = u32(fssm + base + 0x100)) > 32)
            return FALSE;
        for (UINT k = 0; k < n; k++)
        {
            const BYTE *r = fssm + base + 8 * k;
            if (r[3] != k || r[4] || r[5] || r[6] != r[1])
                return FALSE;
        }
        for (UINT k = 0; k < n; k++)
        {
            g->registers[k] = fssm[base + 8 * k + 1];
            g->flat[k] = fssm[base + 8 * k + 7] == 1;
        }
        *input_ena = u32(fssm + base + 0x110);
    }
    else
    {
        base = 0x1d4 + shift;
        if (base + 0xc4 > size || (n = u32(fssm + base + 0xc0)) > 32)
            return FALSE;
        for (UINT k = 0; k < n; k++)
        {
            const BYTE *r = fssm + base + 6 * k;
            if (r[2] != k || r[3] || r[4] || r[5])
                return FALSE;
        }
        for (UINT k = 0; k < n; k++)
            g->registers[k] = fssm[base + 6 * k + 1];
    }
    g->register_count = n;
    return TRUE;
}

/* ------------------------------------------------------------------ mesh stages
 *
 * A mesh shader's FSSM, as all 6,685 of the target title's have it (sub-version 6; the layout
 * VIctory describes, crates/victory-shader/src/native.rs): after "FSSM", its version (0x101) and a
 * u64 size, the payload at 0x10 opens with the slot count (6), then a u64 record offset per hardware
 * stage (payload-relative; 0xDEADBEEF_DEADBEEF for none), the mesh stage's at +0x48. Its record:
 *   +0x00  u64 code offset (payload-relative) and +0x08 its size: a 24-byte shader ID, then the
 *          code (every one opens with s_inst_prefetch there)
 *   +0x14  flags: bit 5 wave32 (clear in all: wave64), bits 16-20 user SGPRs (13-17)
 *   +0x44  float mode (0xC0 in all), +0x4C LDS bytes (0 in all but 5)
 * then its stage data, E = record + 0x54:
 *   E+0xC8   32 outputs of 6 bytes (count at E+0x188): usage, OSG1 register, param, a packed
 *            pair's second register, 0, flags (3: two fp16 halves); param bit 7 would mark a
 *            per-primitive output (none in the title)
 *   E+0x194  PA_CL_VS_OUT_CNTL (clip distances in 1,438)
 *   E+0x448  u16 lanes per group, u16 maximum vertices, u16 maximum primitives (128 each in all),
 *            u8 user SGPRs, u8 output topology (2 triangles in all; 1 lines)
 * The group stride the code divides v5 by is max(lanes, vertices, primitives) (VIctory; xcl's
 * notes have v5 >> 7 in the retail code for 128). The title's mesh shaders have no amplification
 * stage. */
#define FSSM_PAYLOAD 0x10
#define FSSM_MESH_SLOT 0x48
#define FSSM_SHADER_ID 24
#define FSSM_RECORD 0x54
#define MESH_OUTPUTS 0xc8
#define MESH_OUTPUT_COUNT 0x188
#define MESH_CLIP_CNTL 0x194
#define MESH_INFO 0x448

static UINT64 u64(const BYTE *p)
{
    return u32(p) | (UINT64)u32(p + 4) << 32;
}

static void *fssm_mesh(const BYTE *fssm, SIZE_T size, SIZE_T *blob_size)
{
    const BYTE *payload = fssm + FSSM_PAYLOAD, *record, *data, *info;
    SIZE_T payload_size = size - FSSM_PAYLOAD;
    struct rebox_deferred_shader *h;
    struct rebox_deferred_graphics *g;
    UINT64 at, code_at, code_bytes;
    UINT32 flags, outputs, topology;

    if (size < FSSM_PAYLOAD + FSSM_MESH_SLOT + 8 || u32(fssm + 4) != 0x101 || u64(payload) != 6
            || (at = u64(payload + FSSM_MESH_SLOT)) > payload_size || payload_size - at < FSSM_RECORD + MESH_INFO + 8)
        return NULL;
    record = payload + at;
    data = record + FSSM_RECORD;
    info = data + MESH_INFO;
    code_at = u64(record) + FSSM_SHADER_ID;
    code_bytes = u64(record + 8);
    if (code_bytes <= FSSM_SHADER_ID || code_at > payload_size || code_at % 4)
        return NULL;
    code_bytes = min(code_bytes - FSSM_SHADER_ID, payload_size - code_at) & ~(UINT64)3;
    flags = u32(record + 0x14);
    outputs = u32(data + MESH_OUTPUT_COUNT);
    topology = info[7];
    if (!code_bytes || outputs > 32 || topology < 1 || topology > 2 || !(info[0] | info[1] << 8)
            || !(info[2] | info[3] << 8) || !(info[4] | info[5] << 8))
        return NULL;
    if (!(h = calloc(1, sizeof(*h) + sizeof(*g) + code_bytes)))
        return NULL;
    g = (struct rebox_deferred_graphics *)(h + 1);
    for (UINT k = 0; k < outputs; k++)
    {
        const BYTE *r = data + MESH_OUTPUTS + 6 * k;
        if (r[2] != k || r[4]) /* per-primitive (param bit 7) isn't modelled; the rest not seen */
        {
            free(h);
            return NULL;
        }
        g->registers[k] = r[1];
    }
    g->register_count = outputs;
    g->mesh.threads = info[0] | info[1] << 8;
    g->mesh.max_vertices = info[2] | info[3] << 8;
    g->mesh.max_primitives = info[4] | info[5] << 8;
    g->mesh.group_stride = max(g->mesh.threads, max(g->mesh.max_vertices, g->mesh.max_primitives));
    g->mesh.primitive_vertices = topology + 1;
    g->mesh.lds_bytes = u32(record + 0x4c);
    g->mesh.clip_cntl = u32(data + MESH_CLIP_CNTL);
    h->magic = REBOX_DEFERRED_MAGIC;
    h->version = 2;
    h->size = (UINT32)(sizeof(*h) + sizeof(*g) + code_bytes);
    h->stage = 0x80; /* VK_SHADER_STAGE_MESH_BIT_EXT */
    h->wave_size = (flags & FSSM_WAVE32) ? 32 : 64;
    h->rsrc1 = 0x40000000u | 1u << 21 /* DX10_CLAMP */ | (u32(record + 0x44) & 0xff) << 12;
    h->rsrc2 = 1u << 27; /* USER_SGPR_MSB: 32 user SGPRs, s8 on the root parameters */
    h->code_offset = sizeof(*h) + sizeof(*g);
    h->code_dwords = (UINT32)(code_bytes / 4);
    h->code_address = 0x10000000000ull;
    memcpy((BYTE *)h + h->code_offset, payload + code_at, code_bytes);
    *blob_size = h->size;
    return h;
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
    if (stage == 0x80)
    {
        h = fssm_mesh(fssm, fssm_size, blob_size);
        free(fssm);
        return h;
    }
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
        /* No fetch call (s_swappc_b64 s[4:5], ...): no vertex inputs (full-screen passes build
         * their vertices from the vertex ID). */
        BOOL fetch = (code[0] & 0xffffff00u) == 0xbe842100u;
        /* VGPRs read before the code wrote them, after the fetch call. */
        for (UINT i = 1, n; fetch && i < dwords; i += n)
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

    {
        UINT32 ena = 0;
        if (fssm_interface(fssm, fssm_size, stage, g, &ena) && stage == 0x10 && ena)
            g->input_ena = ena;
    }
    h->magic = REBOX_DEFERRED_MAGIC;
    h->version = 2;
    h->size = sizeof(*h) + sizeof(*g) + (UINT32)code_bytes;
    h->stage = stage;
    h->wave_size = 0;                                     /* inferred from the code (console_compile.c) */
    h->rsrc1 = (u32(fssm + FSSM_GFX_RSRC1) & ~(0xffu << 12)) | (u32(fssm + FSSM_SHORT_FLOAT_MODE) & 0xff) << 12;
    h->rsrc2 = (user & 0x1f) << 1 | (user >> 5 & 1) << 27; /* USER_SGPR, USER_SGPR_MSB (32 for vertex shaders) */
    h->code_offset = sizeof(*h) + sizeof(*g);
    h->code_dwords = dwords;
    h->code_address = 0x10000000000ull;
    memcpy((BYTE *)h + h->code_offset, code, code_bytes);
    free(fssm);
    *blob_size = h->size;
    return h;
}
