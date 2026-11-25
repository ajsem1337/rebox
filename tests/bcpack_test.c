/* Unit tests for the BCPack decoder (shims/xcrdapi/bcpack.c), built natively (with sanitizers
 * where the compiler has them) and run by tests/run-smoke.sh.
 *
 *   bcpack_test [REAL_TABLES]                   the tests; with the real table file, also noise
 *                                               and parsing checks on it
 *   bcpack_test decode TABLES CODEC SIZE IN OUT decodes one packed chunk to SIZE bytes of blocks
 *
 * Streams are built by a synthetic encoder: each lane's symbols are rANS-coded in reverse with a
 * table's model (in one or more fragments), and the lanes are interleaved by running a separate,
 * straightforward model of the decoder's schedule that hands out each lane's own bytes. Tables are
 * synthetic: uniform models (and, for shape tests, skewed ones), pair tables giving c%8 and
 * (c/8)%8, and dictionaries with distinctive values. No Microsoft data is needed or included. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shims/xcrdapi/bcpack.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); \
    printf(" (%s:%d)\n", __FILE__, __LINE__); } } while (0)

static void ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("ok   ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* ---------------------------------------------------------------------------- byte vectors */

struct vec
{
    uint8_t *p;
    size_t n, cap, head; /* head: next byte to pop */
};

static void vpush(struct vec *v, uint8_t b)
{
    if (v->n == v->cap)
    {
        v->cap = v->cap ? 2 * v->cap : 64;
        v->p = realloc(v->p, v->cap);
    }
    v->p[v->n++] = b;
}

static void vfree(struct vec *v)
{
    free(v->p);
    memset(v, 0, sizeof(*v));
}

/* ------------------------------------------------------------------------------ format facts */

static const struct
{
    unsigned lanes, per, block;
    unsigned char group[16];
} fmt[8] = {
    [1] = { 12, 1, 8, { 7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7 } },
    [3] = { 12, 2, 16, { 3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7 } },
    [4] = { 12, 1, 8, { 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5 } },
    [5] = { 12, 2, 16, { 3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5 } },
    [6] = { 16, 2, 16, { 3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4 } },
    [7] = { 16, 2, 16, { 3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4 } },
};

static unsigned rec_len(unsigned codec)
{
    return fmt[codec].lanes * fmt[codec].per;
}

/* Bits of record element k's symbols. */
static unsigned elem_bits(unsigned codec, unsigned k)
{
    return fmt[codec].group[k / fmt[codec].per] + 1;
}

/* ---------------------------------------------------------------------------- synthetic tables */

enum { T_MODELS = 0, T_RANK_BC1 = 2304, T_RANK_BC3A = 2560, T_RANK_BC3C = 2624, T_RANK_BC4 = 2880,
       T_RANK_BC5 = 2944, T_CONTROL = 3008 };
static const unsigned dict_offset[8] = { 0, 4032, 0, 9642, 19842, 24432, 33612, 48402 };
static const unsigned dict_size[8] = { 0, 22, 0, 40, 18, 36, 58, 116 };

static void put16(uint8_t *p, unsigned v)
{
    p[0] = v, p[1] = v >> 8;
}

static unsigned get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

/* The control words of the rich synthetic table, by code (code 0 is the spec's 0x46). */
static unsigned control_word(unsigned mode, unsigned rotation, unsigned selector, unsigned subsets, unsigned partition)
{
    return mode | rotation << 3 | selector << 5 | subsets << 6 | partition << 8;
}

static unsigned rich_controls[512];
static unsigned rich_control_count;

/* 12.2's tables. rich: identity quad tables, skewed models for shapes 1-15, BC7 control words for
 * every mode and the variants below, and distinctive dictionaries. */
static void synth_tables(uint8_t *t, int rich)
{
    memset(t, 0, BCPACK_TABLES_SIZE);
    for (unsigned m = 0; m < 128; m++)
    {
        unsigned g = m / 16, shape = m % 16, base = 1u << (15 - g);
        unsigned f[9] = { 0 };
        for (unsigned b = 0; b <= g + 1; b++)
            f[b] = base;
        if (rich && shape)
        {
            /* still tiles: bucket 0 and 1 each count once, bucket 2 twice */
            unsigned delta = shape * (base / 32), e = g >= 1 ? shape * (base / 64) : 0;
            f[0] += delta;
            f[1] -= delta + 2 * e;
            f[2] += e;
        }
        for (unsigned b = 0; b < 9; b++)
            put16(t + T_MODELS + 18 * m + 2 * b, f[b]);
    }
    for (unsigned c = 0; c < 64; c++)
    {
        unsigned v = (c & 7) | (c >> 3) << 4;
        t[T_RANK_BC3A + c] = t[T_RANK_BC4 + c] = t[T_RANK_BC5 + c] = v;
    }
    put16(t + T_CONTROL, 0x46);
    if (!rich)
        return;
    for (unsigned c = 0; c < 256; c++)
        t[T_RANK_BC1 + c] = t[T_RANK_BC3C + c] = c;
    rich_control_count = 1;
    rich_controls[0] = 0x46;
#define ADD(w) (rich_controls[rich_control_count++] = (w))
    for (unsigned p = 0; p < 16; p++)
        ADD(control_word(0, 0, 0, 3, p));
    for (unsigned p = 0; p < 64; p++)
        ADD(control_word(1, 0, 0, 2, p)), ADD(control_word(2, 0, 0, 3, p)), ADD(control_word(3, 0, 0, 2, p)),
            ADD(control_word(7, 0, 0, 2, p));
    for (unsigned r = 0; r < 4; r++)
        ADD(control_word(4, r, 0, 1, 0)), ADD(control_word(4, r, 1, 1, 0)), ADD(control_word(5, r, 0, 1, 0));
    ADD(control_word(6, 0, 0, 1, 0));
#undef ADD
    /* codes 300-511 stay unused (0) */
    for (unsigned c = 0; c < rich_control_count; c++)
        put16(t + T_CONTROL + 2 * c, rich_controls[c]);
    for (unsigned codec = 1; codec < 8; codec++)
    {
        if (!dict_size[codec])
            continue;
        for (unsigned i = 0; i < 255; i++)
        {
            uint8_t *e = t + dict_offset[codec] + dict_size[codec] * i;
            for (unsigned k = 0; k < dict_size[codec]; k++)
                e[k] = (uint8_t)(i * 7 + k * 13 + codec);
            if (codec == 6)
                e[25] = 1 + i % 16; /* index modes 1-14, and 15-16 (width 0) */
            if (codec == 7)
            {
                put16(e, rich_controls[i % rich_control_count]);
                e[50] = i % 5, e[51] = i % 3; /* widths 0-4, 0-2 */
            }
        }
    }
}

/* ------------------------------------------------------------------ the test's model of section 4 */

struct tmodel
{
    unsigned f[9], start[9], used;
};

static void model_of(const uint8_t *t, unsigned m, struct tmodel *out)
{
    unsigned sum = 0;
    out->used = m / 16 + 2;
    for (unsigned b = 0; b < 9; b++)
    {
        out->f[b] = get16(t + 18 * m + 2 * b);
        out->start[b] = b < out->used ? sum : 65536;
        if (b < out->used)
            sum += out->f[b] * (b < 2 ? 1 : 1u << (b - 1));
    }
}

static unsigned bitlen(unsigned v)
{
    unsigned n = 0;
    while (v)
        n++, v >>= 1;
    return n;
}

static void freq_cum(const struct tmodel *m, unsigned s, uint32_t *F, uint32_t *C)
{
    if (s == 0)
    {
        *F = m->f[0] - 1, *C = 1;
        return;
    }
    unsigned k = bitlen(s);
    *F = m->f[k];
    *C = m->start[k] + (s - (1u << (k - 1))) * m->f[k];
}

/* One lane of the straightforward schedule model: per the pseudocode of 4.5, with ints for
 * "absent". */
struct mlane
{
    int stage, G, reset, lng, inbox, wants, pending, frozen;
    unsigned model;
    uint32_t rem, x;
};

static void mstep(const uint8_t *t, struct mlane *l)
{
    if (l->stage != 3)
    {
        if (l->inbox < 0)
            return;
        int h = l->inbox;
        l->inbox = -1;
        l->wants = 1;
        if (l->stage == 0)
        {
            l->model = 16 * (l->G - (h >> 6)) + (h & 15);
            l->reset = h >> 4 & 1, l->lng = h >> 5 & 1, l->rem = 0, l->stage = 1;
        }
        else if (l->stage == 1)
        {
            if (l->lng)
                l->rem = h, l->stage = 2;
            else
                l->rem = h + 1, l->stage = 3;
        }
        else
            l->rem += 256 * h + 257, l->stage = 3;
        return;
    }
    uint32_t y = l->x;
    if (y < (1u << 24) && l->inbox >= 0)
        y = (uint32_t)((uint64_t)y * 256 + l->inbox), l->inbox = -1, l->rem--;
    l->wants = l->rem > 0 && l->inbox < 0;
    if (y > (1u << 24))
    {
        struct tmodel m;
        unsigned slot = y % 65536, b = 0, s;
        uint32_t F, C;
        model_of(t, l->model, &m);
        for (unsigned k = 0; k < m.used; k++)
            if (m.start[k] <= slot)
                b = k;
        s = b == 0 ? 0 : b == 1 ? 1 : (1u << (b - 1)) + (slot - m.start[b]) / m.f[b];
        freq_cum(&m, s, &F, &C);
        l->x = (uint32_t)((uint64_t)(y / 65536) * F + slot - C);
        l->pending = s;
    }
    else if (l->rem > 0)
        l->x = y;
    else
    {
        l->stage = 0;
        l->wants = l->inbox < 0;
        if (l->reset)
            l->x = 0;
    }
}

/* Runs the schedule over per-lane byte queues: each byte handed to lane j is the next of queue j,
 * appended to the stream. records gets what the model decodes. Returns 0, or -1 if a lane wants a
 * byte its queue doesn't have, or the schedule stalls. */
static int interleave(const uint8_t *t, unsigned codec, struct vec *queues, size_t blocks, struct vec *stream,
                      uint16_t *records)
{
    unsigned lanes = fmt[codec].lanes, per = fmt[codec].per, len = rec_len(codec);
    struct mlane L[16];
    size_t pos = 0, total = blocks * per;
    unsigned delivered = 0, idle = 0;

    for (unsigned j = 0; j < lanes; j++)
    {
        memset(&L[j], 0, sizeof(L[j]));
        L[j].G = fmt[codec].group[j], L[j].inbox = -1, L[j].pending = -1, L[j].wants = 1;
    }
    while (pos < total)
    {
        unsigned taken = 0, progress = 0;
        for (unsigned j = 0; j < lanes; j++)
        {
            unsigned before = taken;
            int blocked;
            if (!L[j].frozen)
                mstep(t, &L[j]);
            blocked = L[j].pending >= 0 && (delivered >> j & 1);
            if (L[j].wants && !blocked && taken < 8)
            {
                if (queues[j].head == queues[j].n)
                    return -1;
                L[j].inbox = queues[j].p[queues[j].head++];
                vpush(stream, (uint8_t)L[j].inbox);
                taken++;
            }
            L[j].frozen = (L[j].wants && before >= 8) || blocked;
            if (!L[j].frozen && L[j].pending >= 0)
            {
                if (records)
                    records[(pos / per) * len + j * per + pos % per] = L[j].pending;
                L[j].pending = -1;
                delivered |= 1u << j;
                progress = 1;
            }
        }
        if (delivered == (1u << lanes) - 1)
            pos++, delivered = 0, progress = 1;
        idle = taken == 0 && !progress ? idle + 1 : 0;
        if (idle > 3)
            return -1;
    }
    return 0;
}


/* ------------------------------------------------------------------------- the lane encoder */

struct frag
{
    unsigned r, shape, reset, force_long;
    const uint16_t *sym;
    size_t n;
};

static void emit_fragment(struct vec *out, const struct frag *f, const struct vec *payload)
{
    size_t n = payload->n;
    unsigned h = f->shape | f->reset << 4 | f->r << 6;
    if (n > 256 || f->force_long)
    {
        vpush(out, h | 0x20);
        vpush(out, (uint8_t)(n - 257));
        vpush(out, (uint8_t)((n - 257) >> 8));
    }
    else
    {
        vpush(out, h);
        vpush(out, (uint8_t)(n - 1));
    }
    /* payload is in emission order: the decoder reads it backwards */
    for (size_t k = n; k--;)
        vpush(out, payload->p[k]);
}

/* Encodes one lane's fragments (12.3, extended to fragment chains): fragments joined without the
 * reset bit share one rANS state, and a chain ends at a reset or at the lane's last fragment,
 * which must not reset. The lane is then parked with a filler fragment. A carried boundary the
 * state can't end at becomes a reset (fr is updated). Returns -1 if the plan can't be coded (an
 * oversized payload, a long length too short for its form, symbols too wide). */
static int encode_lane(const uint8_t *t, unsigned G, struct frag *fr, unsigned count, struct vec *out)
{
    struct vec payload[64];
    unsigned first = 0;

    if (!count || count > 64 || fr[count - 1].reset)
        return -1;
    memset(payload, 0, sizeof(payload));
retry:
    first = 0;
    while (first < count)
    {
        unsigned last = first;
        uint64_t x = 1u << 24;
        while (!fr[last].reset && last + 1 < count)
            last++;
        for (unsigned k = last + 1; k-- > first;)
        {
            struct tmodel m;
            if (fr[k].r > 3 || !fr[k].n)
                goto fail;
            model_of(t, 16 * (G - fr[k].r) + fr[k].shape, &m);
            for (size_t i = fr[k].n; i--;)
            {
                uint32_t F, C;
                unsigned s = fr[k].sym[i];
                struct vec *dst = (i == fr[k].n - 1 && k != last) ? &payload[k + 1] : &payload[k];
                size_t emitted = 0;
                if (s >> (G - fr[k].r + 1))
                    goto fail;
                freq_cum(&m, s, &F, &C);
                if (!F)
                    goto fail;
                while (x >= ((uint64_t)F << 16))
                    vpush(dst, (uint8_t)x), x >>= 8, emitted++;
                if (i == fr[k].n - 1 && k != last && !emitted)
                {
                    /* The state after this fragment's last symbol would be above 2^24, so the
                     * fragment wouldn't end there: make it a reset boundary instead. */
                    fr[k].reset = 1;
                    for (unsigned q = 0; q < 64; q++)
                        vfree(&payload[q]);
                    goto retry;
                }
                x = (x / F) * 65536 + x % F + C;
            }
        }
        for (unsigned b = 0; b < 4; b++)
            vpush(&payload[first], (uint8_t)(x >> (8 * b)));
        first = last + 1;
    }
    for (unsigned k = 0; k < count; k++)
    {
        size_t n = payload[k].n;
        if (!n || n > 65792 || (fr[k].force_long && n < 257))
            goto fail;
        emit_fragment(out, &fr[k], &payload[k]);
    }
    /* The filler. The lane's state is 2^24 once the encoder's first bytes are read, but the last of
     * them is read in the step that ends the fragment and is lost (4.5), so the state may be 2^16:
     * the filler's zero bytes bring it to exactly 2^24, where it can neither read nor decode. */
    vpush(out, 0x00), vpush(out, 0x01), vpush(out, 0x00), vpush(out, 0x00);
    for (unsigned k = 0; k < 64; k++)
        vfree(&payload[k]);
    return 0;
fail:
    for (unsigned k = 0; k < 64; k++)
        vfree(&payload[k]);
    return -1;
}

/* A lane plan: how its symbols are split into fragments. */
struct plan
{
    unsigned count;
    struct
    {
        size_t n;
        unsigned r, shape, reset, force_long;
    } f[64];
};

/* Builds a stream from records (blocks x rec_len), one fragment per lane unless plans says
 * otherwise. The model's decode of the stream is checked against the records. */
static int build_stream(const uint8_t *t, unsigned codec, const uint16_t *records, size_t blocks,
                        struct plan *plans, struct vec *stream)
{
    unsigned lanes = fmt[codec].lanes, per = fmt[codec].per, len = rec_len(codec);
    struct vec queues[16];
    uint16_t *syms = malloc(blocks * per * sizeof(*syms) + 2), *check = calloc(blocks * len + 1, sizeof(*check));
    int ret = 0;

    memset(queues, 0, sizeof(queues));
    memset(stream, 0, sizeof(*stream));
    for (unsigned j = 0; j < lanes && !ret; j++)
    {
        struct frag fr[64];
        unsigned count = 1;
        size_t at = 0;
        for (size_t n = 0; n < blocks; n++)
            for (unsigned h = 0; h < per; h++)
                syms[n * per + h] = records[n * len + j * per + h];
        if (plans && plans[j].count)
        {
            count = plans[j].count;
            for (unsigned k = 0; k < count; k++)
            {
                fr[k].r = plans[j].f[k].r, fr[k].shape = plans[j].f[k].shape, fr[k].reset = plans[j].f[k].reset;
                fr[k].force_long = plans[j].f[k].force_long;
                fr[k].sym = syms + at, fr[k].n = plans[j].f[k].n;
                at += fr[k].n;
            }
            if (at != blocks * per)
                ret = -1;
        }
        else
        {
            memset(fr, 0, sizeof(fr[0]));
            fr[0].sym = syms, fr[0].n = blocks * per;
        }
        if (!ret)
            ret = encode_lane(t, fmt[codec].group[j], fr, count, &queues[j]);
        if (!ret && plans && plans[j].count)
            for (unsigned k = 0; k < count; k++)
                plans[j].f[k].reset = fr[k].reset;
    }
    if (!ret)
        ret = interleave(t, codec, queues, blocks, stream, check);
    if (!ret && memcmp(check, records, blocks * len * sizeof(*check)))
        ret = -2;
    for (unsigned j = 0; j < 16; j++)
        vfree(&queues[j]);
    free(syms);
    free(check);
    if (ret)
        vfree(stream);
    return ret;
}

/* Random symbols for every element, within their lane's width (and below `cap` bits if smaller). */
static void random_records(unsigned codec, uint16_t *records, size_t blocks, unsigned cap)
{
    unsigned len = rec_len(codec);
    for (size_t n = 0; n < blocks; n++)
        for (unsigned k = 0; k < len; k++)
        {
            unsigned bits = elem_bits(codec, k);
            if (bits > cap)
                bits = cap;
            records[n * len + k] = rnd() & ((1u << bits) - 1);
        }
}

/* --------------------------------------------------------------------------- test-side models */

static unsigned zz(unsigned u)
{
    return u & 1 ? 0u - (u + 1) / 2 : u / 2;
}

static unsigned il(unsigned a, unsigned b)
{
    unsigned r = 0;
    for (unsigned k = 0; k < 8; k++)
        r |= ((a >> k) & 1) << (2 * k) | ((b >> k) & 1) << (2 * k + 1);
    return r;
}

static void refs_of(unsigned codec, const uint16_t *s, unsigned *E, unsigned *I)
{
    int paired = codec != 1 && codec != 4;
    unsigned d = paired ? il(s[0], s[1]) : s[0];
    *I = paired ? il(s[2], s[3]) : s[1];
    *E = (*I + zz(d)) & 255;
}

/* Plain formats: internal endpoints and indices per block, every block kept. */
struct pblock
{
    unsigned ep[8], idx[32];
};

static void plain_dict(const uint8_t *t, unsigned codec, unsigned i, struct pblock *b)
{
    static const unsigned neps[8] = { 0, 6, 0, 8, 2, 4 }, nidx[8] = { 0, 16, 0, 32, 16, 32 };
    const uint8_t *e = t + dict_offset[codec] + dict_size[codec] * i;
    memset(b, 0, sizeof(*b));
    for (unsigned k = 0; k < neps[codec]; k++)
        b->ep[k] = e[k];
    for (unsigned k = 0; k < nidx[codec]; k++)
        b->idx[k] = e[neps[codec] + k];
}

/* The expected blocks of a plain-format record sequence, written plainly from sections 6, 7, 10. */
static void expect_plain(const uint8_t *t, unsigned codec, const uint16_t *records, size_t blocks, uint8_t *out)
{
    unsigned len = rec_len(codec);
    struct pblock *all = calloc(blocks, sizeof(*all));
    for (size_t n = 0; n < blocks; n++)
    {
        const uint16_t *s = records + n * len;
        struct pblock eb, ib, *b = &all[n];
        unsigned E, I;
        refs_of(codec, s, &E, &I);
        if (E == 0) memset(&eb, 0, sizeof(eb)); else if (E <= n) eb = all[n - E]; else plain_dict(t, codec, E - n - 1, &eb);
        if (I == 0) memset(&ib, 0, sizeof(ib)); else if (I <= n) ib = all[n - I]; else plain_dict(t, codec, I - n - 1, &ib);
        uint8_t *o = out + n * fmt[codec].block;
        if (codec == 1 || codec == 3)
        {
            /* colour: R0 G0 B0 R1 G1 B1, residual elements by format */
            static const unsigned from1[6] = { 2, 3, 4, 5, 6, 7 }, from3[6] = { 14, 16, 18, 15, 17, 19 };
            static const unsigned w[6] = { 5, 6, 5, 5, 6, 5 };
            const unsigned *from = codec == 1 ? from1 : from3;
            unsigned ib0 = codec == 1 ? 0 : 16, rank = codec == 1 ? T_RANK_BC1 : T_RANK_BC3C, q0 = codec == 1 ? 8 : 20;
            for (unsigned k = 0; k < 6; k++)
                b->ep[k] = (zz(s[from[k]]) + eb.ep[k]) % (1u << w[k]);
            for (unsigned q = 0; q < 4; q++)
            {
                unsigned v = t[rank + s[q0 + q]];
                unsigned px[4] = { 8 * (q / 2) + 2 * (q % 2), 8 * (q / 2) + 2 * (q % 2) + 1,
                                   8 * (q / 2) + 2 * (q % 2) + 4, 8 * (q / 2) + 2 * (q % 2) + 5 };
                for (unsigned k = 0; k < 4; k++)
                    b->idx[ib0 + px[k]] = ((v >> (2 * k) & 3) + ib.idx[ib0 + px[k]]) % 4;
            }
            unsigned c0 = b->ep[0] * 2048 + b->ep[1] * 32 + b->ep[2], c1 = b->ep[3] * 2048 + b->ep[4] * 32 + b->ep[5];
            uint8_t *c = o + (codec == 3 ? 8 : 0);
            static const unsigned map[4] = { 0, 2, 3, 1 };
            uint32_t bits = 0;
            for (unsigned p = 0; p < 16; p++)
                bits += map[b->idx[ib0 + p]] << (2 * p);
            c[0] = c0 & 255, c[1] = c0 >> 8, c[2] = c1 & 255, c[3] = c1 >> 8;
            c[4] = bits & 255, c[5] = bits >> 8 & 255, c[6] = bits >> 16 & 255, c[7] = bits >> 24;
        }
        /* single-channel halves: (endpoint elements, residual elements, pair codes, rank table, index base, out) */
        struct { unsigned e0, r0, r1, pairs, rank, ib, off; } halves[2];
        unsigned nh = 0;
        if (codec == 3) halves[nh++] = (typeof(halves[0])){ 6, 4, 5, 6, T_RANK_BC3A, 0, 0 };
        if (codec == 4) halves[nh++] = (typeof(halves[0])){ 0, 2, 3, 4, T_RANK_BC4, 0, 0 };
        if (codec == 5)
        {
            halves[nh++] = (typeof(halves[0])){ 0, 4, 5, 8, T_RANK_BC5, 0, 0 };
            halves[nh++] = (typeof(halves[0])){ 2, 6, 7, 16, T_RANK_BC5, 16, 8 };
        }
        for (unsigned hh = 0; hh < nh; hh++)
        {
            static const unsigned map[8] = { 0, 2, 3, 4, 5, 6, 7, 1 };
            unsigned e0 = halves[hh].e0, ibase = halves[hh].ib;
            b->ep[e0] = (zz(s[halves[hh].r0]) + eb.ep[e0]) % 256;
            b->ep[e0 + 1] = (zz(s[halves[hh].r1]) + eb.ep[e0 + 1]) % 256;
            for (unsigned k = 0; k < 8; k++)
            {
                unsigned v = t[halves[hh].rank + (s[halves[hh].pairs + k] & 63)];
                b->idx[ibase + 2 * k] = ((v & 7) + ib.idx[ibase + 2 * k]) % 8;
                b->idx[ibase + 2 * k + 1] = ((v >> 4 & 7) + ib.idx[ibase + 2 * k + 1]) % 8;
            }
            uint8_t *h = o + halves[hh].off;
            uint64_t bits = 0;
            h[0] = b->ep[e0] ^ 0x80, h[1] = b->ep[e0 + 1] ^ 0x80;
            for (unsigned p = 0; p < 16; p++)
                bits |= (uint64_t)map[b->idx[ibase + p]] << (3 * p);
            for (unsigned k = 0; k < 6; k++)
                h[2 + k] = bits >> (8 * k) & 255;
        }
    }
    free(all);
}

/* -------------------------------------------------------------------- standard BC6H/BC7 readers */

struct rbits
{
    const uint8_t *p;
    unsigned pos;
};

static unsigned rd(struct rbits *r, unsigned n)
{
    unsigned v = 0;
    for (unsigned k = 0; k < n; k++, r->pos++)
        if (r->pos < 128)
            v |= (unsigned)(r->p[r->pos / 8] >> (r->pos % 8) & 1) << k;
    return v;
}

static const unsigned char A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
static const unsigned char A3b[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3 };
static const unsigned char A3c[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8 };

/* Appendix B, as text, parsed here independently of the decoder's table. */
static const char *const bc6h_text[14] = {
    "gy[4], by[4], bz[4], rw[9:0], gw[9:0], bw[9:0], rx[4:0], gz[4], gy[3:0], gx[4:0], bz[0], gz[3:0], bx[4:0], bz[1], by[3:0], ry[4:0], bz[2], rz[4:0], bz[3], d[4:0]",
    "gy[5], gz[4], gz[5], rw[6:0], bz[0], bz[1], by[4], gw[6:0], by[5], bz[2], gy[4], bw[6:0], bz[3], bz[5], bz[4], rx[5:0], gy[3:0], gx[5:0], gz[3:0], bx[5:0], by[3:0], ry[5:0], rz[5:0], d[4:0]",
    "rw[9:0], gw[9:0], bw[9:0], rx[4:0], rw[10], gy[3:0], gx[3:0], gw[10], bz[0], gz[3:0], bx[3:0], bw[10], bz[1], by[3:0], ry[4:0], bz[2], rz[4:0], bz[3], d[4:0]",
    "rw[9:0], gw[9:0], bw[9:0], rx[3:0], rw[10], gz[4], gy[3:0], gx[4:0], gw[10], gz[3:0], bx[3:0], bw[10], bz[1], by[3:0], ry[3:0], bz[0], bz[2], rz[3:0], gy[4], bz[3], d[4:0]",
    "rw[9:0], gw[9:0], bw[9:0], rx[3:0], rw[10], by[4], gy[3:0], gx[3:0], gw[10], bz[0], gz[3:0], bx[4:0], bw[10], by[3:0], ry[3:0], bz[1], bz[2], rz[3:0], bz[4], bz[3], d[4:0]",
    "rw[8:0], by[4], gw[8:0], gy[4], bw[8:0], bz[4], rx[4:0], gz[4], gy[3:0], gx[4:0], bz[0], gz[3:0], bx[4:0], bz[1], by[3:0], ry[4:0], bz[2], rz[4:0], bz[3], d[4:0]",
    "rw[7:0], gz[4], by[4], gw[7:0], bz[2], gy[4], bw[7:0], bz[3], bz[4], rx[5:0], gy[3:0], gx[4:0], bz[0], gz[3:0], bx[4:0], bz[1], by[3:0], ry[5:0], rz[5:0], d[4:0]",
    "rw[7:0], bz[0], by[4], gw[7:0], gy[5], gy[4], bw[7:0], gz[5], bz[4], rx[4:0], gz[4], gy[3:0], gx[5:0], gz[3:0], bx[4:0], bz[1], by[3:0], ry[4:0], bz[2], rz[4:0], bz[3], d[4:0]",
    "rw[7:0], bz[1], by[4], gw[7:0], by[5], gy[4], bw[7:0], bz[5], bz[4], rx[4:0], gz[4], gy[3:0], gx[4:0], bz[0], gz[3:0], bx[5:0], by[3:0], ry[4:0], bz[2], rz[4:0], bz[3], d[4:0]",
    "rw[5:0], gz[4], bz[0], bz[1], by[4], gw[5:0], gy[5], by[5], bz[2], gy[4], bw[5:0], gz[5], bz[3], bz[5], bz[4], rx[5:0], gy[3:0], gx[5:0], gz[3:0], bx[5:0], by[3:0], ry[5:0], rz[5:0], d[4:0]",
    "rw[9:0], gw[9:0], bw[9:0], rx[9:0], gx[9:0], bx[9:0]",
    "rw[9:0], gw[9:0], bw[9:0], rx[8:0], rw[10], gx[8:0], gw[10], bx[8:0], bw[10]",
    "rw[9:0], gw[9:0], bw[9:0], rx[7:0], rw[11:10], gx[7:0], gw[11:10], bx[7:0], bw[11:10]",
    "rw[9:0], gw[9:0], bw[9:0], rx[3:0], rw[15:10], gx[3:0], gw[15:10], bx[3:0], bw[15:10]",
};
static const unsigned bc6h_mode_value[14] = { 0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15 };
static const unsigned bc6h_prec[14][4] = {
    { 10, 5, 5, 5 }, { 7, 6, 6, 6 }, { 11, 5, 4, 4 }, { 11, 4, 5, 4 }, { 11, 4, 4, 5 }, { 9, 5, 5, 5 }, { 8, 6, 5, 5 },
    { 8, 5, 6, 5 }, { 8, 5, 5, 6 }, { 6, 6, 6, 6 }, { 10, 10, 10, 10 }, { 11, 9, 9, 9 }, { 12, 8, 8, 8 }, { 16, 4, 4, 4 } };

struct bc6r
{
    unsigned mode, field[4][3], part, idx[16], bits;
};

/* Reads a BC6H block with the given (expected) mode; fields[endpoint][channel]. */
static int read_bc6h(const uint8_t *blk, unsigned mode, struct bc6r *out)
{
    struct rbits r = { blk, 0 };
    const char *p = bc6h_text[mode - 1];
    unsigned two = mode <= 10, width = two ? 3 : 4;
    memset(out, 0, sizeof(*out));
    out->mode = rd(&r, mode <= 2 ? 2 : 5);
    if (out->mode != bc6h_mode_value[mode - 1])
        return -1;
    while (*p)
    {
        unsigned hi, lo, *dst;
        int used = 0;
        if (*p == 'd')
        {
            sscanf(p, "d[%u:%u]%n", &hi, &lo, &used);
            dst = &out->part;
        }
        else
        {
            unsigned c = p[0] == 'r' ? 0 : p[0] == 'g' ? 1 : 2, e = p[1] - 'w';
            if (sscanf(p + 2, "[%u:%u]%n", &hi, &lo, &used) != 2)
            {
                sscanf(p + 2, "[%u]%n", &hi, &used);
                lo = hi;
            }
            used += 2;
            dst = &out->field[e][c];
        }
        for (unsigned b = lo; b <= hi; b++)
            *dst |= rd(&r, 1) << b;
        p += used;
        while (*p == ',' || *p == ' ')
            p++;
    }
    for (unsigned px = 0; px < 16; px++)
        out->idx[px] = rd(&r, width - (px == 0 || (two && px == A2[out->part])));
    out->bits = r.pos;
    return 0;
}

struct bc7r
{
    unsigned mode, part, rot, isel, ep[4][3][2], pbit[3][2], idx[2][16], bits;
};

static const struct { unsigned ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; } bc7info[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 }, { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 }, { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
    { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 }, { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 }, { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
    { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 }, { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 } };

/* Reads a standard BC7 block; ep[stream channel][subset][endpoint], idx[first set][pixel]. */
static int read_bc7(const uint8_t *blk, struct bc7r *o)
{
    struct rbits r = { blk, 0 };
    unsigned m = 0;
    memset(o, 0, sizeof(*o));
    while (m < 8 && !rd(&r, 1))
        m++;
    if (m == 8)
        return -1;
    o->mode = m;
    o->part = rd(&r, bc7info[m].pb);
    o->rot = rd(&r, bc7info[m].rb);
    o->isel = rd(&r, bc7info[m].isb);
    for (unsigned c = 0; c < 4; c++)
        for (unsigned s = 0; s < bc7info[m].ns; s++)
            for (unsigned e = 0; e < 2; e++)
                o->ep[c][s][e] = rd(&r, c < 3 ? bc7info[m].cb : bc7info[m].ab);
    for (unsigned s = 0; s < bc7info[m].ns; s++)
    {
        if (bc7info[m].epb)
            o->pbit[s][0] = rd(&r, 1), o->pbit[s][1] = rd(&r, 1);
        if (bc7info[m].spb)
            o->pbit[s][0] = o->pbit[s][1] = rd(&r, 1);
    }
    for (unsigned px = 0; px < 16; px++)
    {
        unsigned anchor = px == 0;
        if (bc7info[m].ns == 2)
            anchor |= px == A2[o->part];
        if (bc7info[m].ns == 3)
            anchor |= px == A3b[o->part] || px == A3c[o->part];
        o->idx[0][px] = rd(&r, bc7info[m].ib - anchor);
    }
    if (bc7info[m].ib2)
        for (unsigned px = 0; px < 16; px++)
            o->idx[1][px] = rd(&r, bc7info[m].ib2 - (px == 0));
    o->bits = r.pos;
    return 0;
}

/* -------------------------------------------------- test-side internal models for BC6H and BC7 */

struct hblock /* BC6H: ep[k][c] 16-bit internal words, idx, index width */
{
    unsigned ep[4][3], idx[16], width, mode;
};

static unsigned topn(unsigned n)
{
    return (0xffffu << (16 - n)) & 0xffff;
}

static int idx_res(const uint16_t *s, const unsigned *from, int *res)
{
    for (unsigned t = 0; t < 8; t++)
    {
        unsigned v = s[from[t]] & 255, E = 0, O = 0;
        for (unsigned k = 0; k < 4; k++)
            E |= (v >> (2 * k) & 1) << k, O |= (v >> (2 * k + 1) & 1) << k;
        int e = E & 1 ? -(int)(E + 1) / 2 : (int)E / 2, o = O & 1 ? -(int)(O + 1) / 2 : (int)O / 2;
        res[2 * t] = t < 6 ? o : e;
        res[2 * t + 1] = t < 6 ? e : o;
    }
    return 0;
}

static unsigned add_frac(int res, unsigned b, unsigned ref, unsigned rb)
{
    if (!b)
        return 0;
    unsigned A = ((unsigned)res % (1u << b)) << (16 - b);
    unsigned B = rb >= 1 && rb <= 16 ? (ref % (1u << rb)) << (16 - rb) : 0;
    return ((A + B) % 65536) >> (16 - b);
}

static void bc6h_dict(const uint8_t *t, unsigned i, struct hblock *b)
{
    const uint8_t *e = t + dict_offset[6] + 58 * i;
    memset(b, 0, sizeof(*b));
    for (unsigned k = 0; k < 12; k++)
        b->ep[k / 3][k % 3] = get16(e + 1 + 2 * k);
    b->mode = e[25];
    b->width = e[25] >= 1 && e[25] <= 10 ? 3 : e[25] >= 11 && e[25] <= 14 ? 4 : 0;
    for (unsigned k = 0; k < 16; k++)
        b->idx[k] = get16(e + 26 + 2 * k);
}

static void model_bc6h(const uint8_t *t, const uint16_t *s, struct hblock *all, size_t n)
{
    static const unsigned primary[8] = { 22, 23, 24, 25, 26, 27, 28, 29 };
    struct hblock er, ir, *b = &all[n];
    unsigned E, I, mode = s[30] + 1, base = bc6h_prec[mode - 1][0];
    int res[16];
    refs_of(6, s, &E, &I);
    memset(&er, 0, sizeof(er)), memset(&ir, 0, sizeof(ir));
    er.width = ir.width = 4;
    if (E && E <= n) er = all[n - E]; else if (E) bc6h_dict(t, E - n - 1, &er);
    if (I && I <= n) ir = all[n - I]; else if (I) bc6h_dict(t, I - n - 1, &ir);
    memset(b, 0, sizeof(*b));
    b->mode = mode;
    b->width = mode <= 10 ? 3 : 4;
    for (unsigned c = 0; c < 3; c++)
    {
        unsigned bits = bc6h_prec[mode - 1][1 + c];
        b->ep[0][c] = (er.ep[0][c] + zz(il(s[4 + 2 * c], s[5 + 2 * c])) * (1u << (16 - base))) % 65536 & topn(base);
        b->ep[1][c] = (er.ep[1][c] + zz(il(s[10 + 2 * c], s[11 + 2 * c])) * (1u << (16 - bits))) % 65536 & topn(bits);
        if (mode <= 10)
        {
            b->ep[2][c] = (er.ep[2][c] + zz(s[16 + 2 * c]) * (1u << (16 - bits))) % 65536 & topn(bits);
            b->ep[3][c] = (er.ep[3][c] + zz(s[17 + 2 * c]) * (1u << (16 - bits))) % 65536 & topn(bits);
        }
        else
        {
            b->ep[2][c] = ((b->ep[0][c] >> (16 - base)) << (16 - bits)) % 65536;
            b->ep[3][c] = b->ep[1][c];
        }
    }
    idx_res(s, primary, res);
    for (unsigned p = 0; p < 16; p++)
        b->idx[p] = add_frac(res[p], b->width, ir.idx[p], ir.width);
}

/* Checks a decoded BC6H block against the model's internal values through the reader. */
static int check_bc6h_block(const uint8_t *blk, const struct hblock *b, unsigned partition)
{
    struct bc6r r;
    unsigned mode = b->mode, two = mode <= 10;
    if (read_bc6h(blk, mode, &r) || r.bits != 128)
        return -1;
    for (unsigned k = 0; k < (two ? 4u : 2u); k++)
        for (unsigned c = 0; c < 3; c++)
        {
            unsigned bits = k ? bc6h_prec[mode - 1][1 + c] : bc6h_prec[mode - 1][0];
            if (r.field[k][c] != (b->ep[k][c] ^ 0x8000) >> (16 - bits))
                return -2;
        }
    if (two && r.part != partition)
        return -3;
    for (unsigned p = 0; p < 16; p++)
    {
        unsigned w = b->width - (p == 0 || (two && p == A2[partition]));
        if (r.idx[p] != ((b->idx[p] ^ (1u << (b->width - 1))) & ((1u << w) - 1)))
            return -4;
    }
    return 0;
}

struct sblock /* BC7 */
{
    unsigned ctl, rows[3][8], width[2], idx[2][16];
};

static void bc7_dict(const uint8_t *t, unsigned i, struct sblock *b)
{
    const uint8_t *e = t + dict_offset[7] + 116 * i;
    memset(b, 0, sizeof(*b));
    b->ctl = get16(e);
    for (unsigned k = 0; k < 24; k++)
        b->rows[k / 8][k % 8] = get16(e + 2 + 2 * k);
    b->width[0] = e[50], b->width[1] = e[51];
    for (unsigned k = 0; k < 32; k++)
        b->idx[k / 16][k % 16] = get16(e + 52 + 2 * k);
}

static const unsigned bc7_bits[8][4] = { { 4, 4, 4, 0 }, { 6, 6, 6, 0 }, { 5, 5, 5, 0 }, { 7, 7, 7, 0 },
                                         { 5, 5, 5, 6 }, { 7, 7, 7, 8 }, { 7, 7, 7, 7 }, { 5, 5, 5, 5 } };
static const unsigned bc7_pbits[8] = { 1, 1, 0, 1, 0, 0, 1, 1 };

static void bc7_precisions(unsigned ctl, unsigned *P)
{
    unsigned mode = ctl & 7, rot = ctl >> 3 & 3;
    for (unsigned c = 0; c < 4; c++)
        P[c] = bc7_bits[mode][c];
    if (rot)
    {
        unsigned tmp = P[3];
        P[3] = P[rot - 1], P[rot - 1] = tmp;
    }
    else if (bc7_pbits[mode])
        P[0]++;
}

static void model_bc7(const uint8_t *t, const uint16_t *s, struct sblock *all, size_t n)
{
    static const unsigned primary[8] = { 22, 23, 24, 25, 26, 27, 28, 29 }, secondary[8] = { 12, 13, 14, 15, 16, 17, 20, 21 };
    static const int two[3][8] = { { 4, 5, 6, 7, 8, 9, 10, 11 }, { 12, 13, 14, 15, 16, 17, 18, 19 }, { -1, -1, -1, -1, -1, -1, -1, -1 } };
    static const int three[3][8] = { { 4, 5, 6, 7, 8, 9, -1, -1 }, { 12, 13, 14, 15, 16, 17, -1, -1 }, { 20, 21, 10, 11, 18, 19, -1, -1 } };
    struct sblock er, ir, *b = &all[n];
    unsigned E, I, ctl = get16(t + T_CONTROL + 2 * ((s[31] % 16) * 32 + s[30] % 32)), P[4];
    unsigned mode = ctl & 7, nsub = ctl >> 6 & 3, common;
    refs_of(7, s, &E, &I);
    memset(&er, 0, sizeof(er)), memset(&ir, 0, sizeof(ir));
    if (E && E <= n) er = all[n - E]; else if (E) bc7_dict(t, E - n - 1, &er);
    if (I && I <= n) ir = all[n - I]; else if (I) bc7_dict(t, I - n - 1, &ir);
    memset(b, 0, sizeof(*b));
    b->ctl = ctl;
    bc7_precisions(ctl, P);
    common = (er.ctl >> 6 & 3) < nsub ? (er.ctl >> 6 & 3) : nsub;
    for (unsigned c = 0; c < (P[3] ? 8u : 6u); c++)
    {
        unsigned p = P[c / 2];
        if (!p)
            continue;
        for (unsigned row = 0; row < nsub; row++)
        {
            int from = (nsub == 3 ? three : two)[row][c];
            int d = (int)(signed char)(from < 0 ? 0 : zz(s[from]) & 255);
            unsigned baseval = er.rows[row < common ? row : 0][c] & topn(p);
            b->rows[row][c] = (unsigned)(baseval + d * (int)(1u << (16 - p))) % 65536;
        }
    }
    b->width[0] = (unsigned[]){ 3, 3, 2, 2, 3, 2, 4, 2 }[mode];
    b->width[1] = mode == 4 || mode == 5 ? 2 : 0;
    for (unsigned set = 0; set < 2; set++)
    {
        int res[16];
        if (!b->width[set])
            continue;
        idx_res(s, set ? secondary : primary, res);
        for (unsigned p = 0; p < 16; p++)
            b->idx[set][p] = add_frac(res[p], b->width[set], ir.idx[set][p], ir.width[set]);
    }
}

static int check_bc7_block(const uint8_t *blk, const struct sblock *b)
{
    struct bc7r r;
    unsigned mode = b->ctl & 7, rot = b->ctl >> 3 & 3, P[4], ns = bc7info[mode].ns;
    if (read_bc7(blk, &r) || r.bits != 128)
        return -1;
    if (r.mode != mode || (bc7info[mode].pb && r.part != (b->ctl >> 8 & (mode == 0 ? 15 : 63))))
        return -2;
    if (bc7info[mode].rb && r.rot != rot)
        return -3;
    if (bc7info[mode].isb && r.isel != (b->ctl >> 5 & 1))
        return -4;
    bc7_precisions(b->ctl, P);
    for (unsigned sc = 0; sc < 4; sc++)
    {
        unsigned ic = sc;
        if (rot && sc == rot - 1) ic = 3;
        else if (rot && sc == 3) ic = rot - 1;
        if (!P[ic])
            continue;
        for (unsigned sub = 0; sub < ns; sub++)
            for (unsigned e = 0; e < 2; e++)
            {
                unsigned v = b->rows[sub][2 * ic + e] >> (16 - P[ic]);
                if (ic == 0 && bc7_pbits[mode] && !rot)
                {
                    if (r.pbit[sub][e] != (v & 1) && !(bc7info[mode].spb && e == 1))
                        return -5;
                    v >>= 1;
                }
                if (r.ep[sc][sub][e] != v)
                    return -6;
            }
    }
    for (unsigned set = 0; set < 2; set++)
    {
        unsigned which = mode == 4 ? 1 - set : set; /* the reader's first set is mode 4's secondary */
        if (!b->width[set])
            continue;
        for (unsigned p = 0; p < 16; p++)
        {
            unsigned anchor = p == 0;
            if (set == 0 && ns == 2) anchor |= p == A2[r.part];
            if (set == 0 && ns == 3) anchor |= p == A3b[r.part] || p == A3c[r.part];
            if (r.idx[which][p] != (b->idx[set][p] & ((1u << (b->width[set] - anchor)) - 1)))
                return -7;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------------------- helpers */

static struct bcpack_tables *parse(const uint8_t *t)
{
    const char *why = NULL;
    struct bcpack_tables *p = bcpack_tables_parse(t, BCPACK_TABLES_SIZE, &why);
    if (!p)
        printf("FAIL synthetic tables rejected: %s\n", why), failures++;
    return p;
}

static void hex(const char *s, struct vec *out)
{
    memset(out, 0, sizeof(*out));
    for (; s[0] && s[1]; s += 2)
    {
        unsigned v;
        sscanf(s, "%2x", &v);
        vpush(out, v);
    }
}

/* Encodes records, decodes them both ways, and compares records and (for plain formats) blocks. */
static int round_trip(const uint8_t *t, const struct bcpack_tables *pt, unsigned codec, const uint16_t *records,
                      size_t blocks, struct plan *plans, uint8_t *out, size_t *stream_size)
{
    struct vec stream;
    unsigned len = rec_len(codec);
    uint16_t *back = calloc(blocks * len, sizeof(*back));
    size_t used = 0, used2 = 0;
    int ret = build_stream(t, codec, records, blocks, plans, &stream);
    if (ret)
    {
        free(back);
        return ret == -2 ? -2 : -1; /* -1: not codable as planned */
    }
    if (bcpack_unpack_records(pt, codec, stream.p, stream.n, blocks, back, &used) != BCPACK_OK
            || memcmp(back, records, blocks * len * sizeof(*back)) || used != stream.n)
        ret = -3;
    else if (out && bcpack_decode(pt, codec, stream.p, stream.n, out, blocks * fmt[codec].block, &used2) != BCPACK_OK)
        ret = -4;
    if (stream_size)
        *stream_size = stream.n;
    vfree(&stream);
    free(back);
    return ret;
}

/* ------------------------------------------------------------------------------------- tests */

static void test_tables(const uint8_t *spec_tables)
{
    uint8_t *t = malloc(BCPACK_TABLES_SIZE);
    struct bcpack_tables *p;

    p = bcpack_tables_parse(spec_tables, BCPACK_TABLES_SIZE, NULL);
    CHECK(p != NULL, "12.2's synthetic tables parse");
    bcpack_tables_free(p);
    CHECK(!bcpack_tables_parse(spec_tables, 16, NULL), "a 16-byte table file is rejected");
    CHECK(!bcpack_tables_parse(spec_tables, BCPACK_TABLES_SIZE - 1, NULL), "a short table file is rejected");
    memcpy(t, spec_tables, BCPACK_TABLES_SIZE);
    put16(t, get16(t) + 1);
    CHECK(!bcpack_tables_parse(t, BCPACK_TABLES_SIZE, NULL), "a first model that no longer tiles is rejected");
    memcpy(t, spec_tables, BCPACK_TABLES_SIZE);
    /* model 127 (group 7): move bucket 8's weight into bucket 7, bucket 8 then 0 */
    put16(t + 18 * 127 + 16, 0);
    put16(t + 18 * 127 + 14, 3 * get16(t + 18 * 127 + 14));
    CHECK(!bcpack_tables_parse(t, BCPACK_TABLES_SIZE, NULL), "a used bucket of frequency 0 is rejected");
    memcpy(t, spec_tables, BCPACK_TABLES_SIZE);
    put16(t + 18 * 0 + 2 * 5, 999); /* group 0 uses buckets 0-1 only */
    p = bcpack_tables_parse(t, BCPACK_TABLES_SIZE, NULL);
    CHECK(p != NULL, "buckets past a model's group aren't checked");
    bcpack_tables_free(p);
    synth_tables(t, 1);
    p = bcpack_tables_parse(t, BCPACK_TABLES_SIZE, NULL);
    CHECK(p != NULL, "the skewed synthetic tables parse");
    bcpack_tables_free(p);
    free(t);
    ok("table files: size and model tiling are checked");
}

static void test_vectors(const uint8_t *t)
{
    static const char bc4_stream[] =
        "0000000000000000050505050404040401010101101010100201000000000000"
        "000014052068b0f803030202242424240200000000000000000000000404040410"
        "101010000000002068b0f8242424240000000000000000";
    static const char bc7_stream[] =
        "00000000000000000404050505050504010101010101014000000001020102400202aa06"
        "007e0000010101000300034102020002020202410000000000000000040404050505050440"
        "041001010101044001080202020201000000000000000041020903030303024105110202"
        "020205000000000000000000000000000000000101010101010101000000000000000000"
        "000000000000000101010000000000";
    static const uint16_t bc4_records[24] = { 0, 0, 20, 5, 8, 26, 44, 62, 8, 26, 44, 62,
                                              0, 1, 2, 2, 9, 9, 9, 9, 9, 9, 9, 9 };
    static const uint8_t bc4_expected[16] = { 0x8a, 0x7d, 0xd0, 0x58, 0x3f, 0xd0, 0x58, 0x3f,
                                              0x8b, 0x7e, 0x1a, 0xeb, 0x07, 0x1a, 0xeb, 0x07 };
    static const uint8_t bc7_expected[16] = { 0x40, 0xd5, 0x7f, 0, 0, 0, 0x7e, 0x80, 0x01, 0, 0, 0, 0, 0, 0, 0 };
    struct bcpack_tables *p = parse(t);
    struct vec s, built;
    uint8_t out[16];
    uint16_t rec[64];
    size_t used = 0;

    if (!p)
        return;
    hex(bc4_stream, &s);
    CHECK(s.n == 88, "the BC4 vector is 88 bytes (%zu)", s.n);
    CHECK(bcpack_decode(p, 4, s.p, s.n, out, 16, &used) == BCPACK_OK && !memcmp(out, bc4_expected, 16),
          "12.4's BC4 vector decodes to 8a7dd0583fd0583f8b7e1aeb071aeb07");
    CHECK(bcpack_unpack_records(p, 4, s.p, s.n, 2, rec, NULL) == BCPACK_OK && !memcmp(rec, bc4_records, sizeof(bc4_records)),
          "12.4's BC4 vector's records");
    CHECK(build_stream(t, 4, bc4_records, 2, NULL, &built) == 0 && built.n == s.n && !memcmp(built.p, s.p, s.n),
          "the synthetic encoder reproduces 12.4's BC4 stream");
    vfree(&built);
    vfree(&s);

    hex(bc7_stream, &s);
    CHECK(s.n == 160, "the BC7 vector is 160 bytes (%zu)", s.n);
    memset(out, 0xcc, 16);
    CHECK(bcpack_decode(p, 7, s.p, s.n, out, 16, &used) == BCPACK_OK && !memcmp(out, bc7_expected, 16),
          "12.4's BC7 vector decodes to 40d57f0000007e800100000000000000");
    memset(rec, 0, sizeof(rec));
    rec[4] = 0xAA, rec[5] = 1, rec[6] = 6, rec[10] = 0x7E; /* R0 +0x55, R1 -1, G0 +3, A0 +0x3F */
    {
        uint16_t back[32];
        CHECK(bcpack_unpack_records(p, 7, s.p, s.n, 1, back, NULL) == BCPACK_OK && !memcmp(back, rec, 64),
              "12.4's BC7 vector's record");
    }
    CHECK(build_stream(t, 7, rec, 1, NULL, &built) == 0 && built.n == s.n && !memcmp(built.p, s.p, s.n),
          "the synthetic encoder reproduces 12.4's BC7 stream");
    vfree(&built);
    vfree(&s);
    bcpack_tables_free(p);
    ok("12.4's vectors decode exactly, and the encoder rebuilds them");
}

/* 12.1 test 2, built by the encoder. */
static void test_bc4_references(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    uint16_t rec[24] = { 0, 0, 20, 5, 8, 26, 44, 62, 8, 26, 44, 62, 0, 1, 2, 2, 9, 9, 9, 9, 9, 9, 9, 9 };
    uint8_t out[16], want[16];
    if (!p)
        return;
    CHECK(round_trip(t, p, 4, rec, 2, NULL, out, NULL) == 0, "BC4 two blocks round trip");
    expect_plain(t, 4, rec, 2, want);
    CHECK(!memcmp(out, want, 16) && out[0] == 138 && out[1] == 125 && out[8] == 139 && out[9] == 126,
          "BC4 with references: 138 125, then 139 126");
    bcpack_tables_free(p);
    ok("BC4 with and without references");
}

/* Every plain format: random records (every residual non-zero), references all over: E != I with
 * both signs, 255 back across the ring, into the dictionary (n+1 is entry 0, 255 at block 0 is entry
 * 254), wraparound sums. Compared with the plain model. */
static void test_plain_formats(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    static const unsigned codecs[4] = { 1, 3, 4, 5 };
    if (!p)
        return;
    for (unsigned ci = 0; ci < 4; ci++)
    {
        unsigned codec = codecs[ci], len = rec_len(codec);
        size_t blocks = 600;
        uint16_t *rec = calloc(blocks * len, sizeof(*rec));
        uint8_t *out = calloc(blocks, 16), *want = calloc(blocks, 16);
        unsigned saw_dict0 = 0, saw_dict254 = 0, saw_255 = 0, saw_neg = 0, saw_pos = 0;

        random_records(codec, rec, blocks, 16);
        for (size_t n = 0; n < blocks; n++)
        {
            uint16_t *s = rec + n * len;
            unsigned I, E, choice = rnd() % 8;
            /* residuals non-zero */
            for (unsigned k = codec == 1 || codec == 4 ? 2 : 4; k < len; k++)
                if (!s[k])
                    s[k] = 1;
            I = choice == 0 ? 0 : choice == 1 ? (unsigned)(n + 1 < 256 ? n + 1 : 255) : choice == 2 ? 255
                : choice == 3 ? (n ? 1 : 0) : rnd() % 256;
            if (n == 0 && choice == 2)
                saw_dict254++;
            if (I == n + 1)
                saw_dict0++;
            if (I == 255 && n >= 255)
                saw_255++;
            /* endpoint delta: zigzag code d, E = I + zz(d) */
            unsigned d = rnd() % 256;
            E = (I + zz(d)) & 255;
            (void)E;
            if (zz(d) & 0x80000000u) saw_neg++; else if (d) saw_pos++;
            if (codec == 1 || codec == 4)
                s[0] = d, s[1] = I;
            else
            {
                unsigned d0 = 0, d1 = 0, i0 = 0, i1 = 0;
                for (unsigned k = 0; k < 4; k++)
                    d0 |= (d >> (2 * k) & 1) << k, d1 |= (d >> (2 * k + 1) & 1) << k,
                    i0 |= (I >> (2 * k) & 1) << k, i1 |= (I >> (2 * k + 1) & 1) << k;
                s[0] = d0, s[1] = d1, s[2] = i0, s[3] = i1;
            }
        }
        /* block 0 references 255 for both */
        if (codec == 1 || codec == 4)
            rec[0] = 0, rec[1] = 255;
        else
            rec[0] = rec[1] = 0, rec[2] = 15, rec[3] = 15;
        saw_dict254++;
        int r = round_trip(t, p, codec, rec, blocks, NULL, out, NULL);
        expect_plain(t, codec, rec, blocks, want);
        CHECK(r == 0 && !memcmp(out, want, blocks * fmt[codec].block), "BC%u: %zu blocks with references (%d)", codec, blocks, r);
        CHECK(saw_dict0 && saw_dict254 && saw_255 && saw_neg && saw_pos, "BC%u: the reference cases were all made", codec);
        if (r == 0 && memcmp(out, want, blocks * fmt[codec].block))
            for (size_t n = 0; n < blocks; n++)
                if (memcmp(out + n * fmt[codec].block, want + n * fmt[codec].block, fmt[codec].block))
                {
                    printf("     first difference at block %zu\n", n);
                    break;
                }
        free(rec), free(out), free(want);
    }
    bcpack_tables_free(p);
    ok("BC1, BC3, BC4, BC5: residuals, references (E != I, 255 back, the dictionary), wraparound");
}

/* One block per plain format with hand-built expectations, field by field. */
static void test_plain_fields(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    uint8_t out[16];
    if (!p)
        return;
    {
        /* BC1: R0 +3 (zz 6), G0 -2 (zz 3), B0 +1, R1 -1 -> 31, G1 +5, B1 -16 -> 16; quads identity
         * rank: code q gives residuals q's bits; I = 0 */
        uint16_t s[12] = { 0, 0, 6, 3, 2, 1, 10, 31, 0x1b, 0xe4, 0x00, 0xff };
        CHECK(round_trip(t, p, 1, s, 1, NULL, out, NULL) == 0, "BC1 field block round trip");
        unsigned c0 = 3 << 11 | 62 << 5 | 1, c1 = 31 << 11 | 5 << 5 | 16;
        /* quad 0 (pixels 0 1 4 5) residuals 3 2 1 0 from 0x1b; quad 1 (2 3 6 7) 0 1 2 3; quad 2 all 0; quad 3 all 3 */
        unsigned idx[16] = { 3, 2, 0, 1, 1, 0, 2, 3, 0, 0, 3, 3, 0, 0, 3, 3 }, map[4] = { 0, 2, 3, 1 };
        uint32_t bits = 0;
        for (unsigned k = 0; k < 16; k++)
            bits |= map[idx[k]] << (2 * k);
        uint8_t want[8] = { c0 & 255, c0 >> 8, c1 & 255, c1 >> 8, bits & 255, bits >> 8 & 255, bits >> 16 & 255, bits >> 24 };
        CHECK(!memcmp(out, want, 8), "BC1: hand-built block");
    }
    {
        /* BC5: red E0 +1, E1 -1 -> 255, green E0 +127 (zz 254), E1 -128 (zz 255); pairs (c%8, c/8%8) */
        uint16_t s[24] = { 0, 0, 0, 0, 2, 1, 254, 255 };
        for (unsigned k = 0; k < 16; k++)
            s[8 + k] = (k * 9 + 5) & 63;
        CHECK(round_trip(t, p, 5, s, 1, NULL, out, NULL) == 0, "BC5 field block round trip");
        CHECK(out[0] == (1 ^ 0x80) && out[1] == (255 ^ 0x80) && out[8] == (127 ^ 0x80) && out[9] == (128 ^ 0x80),
              "BC5: endpoints have their top bit inverted");
        unsigned map[8] = { 0, 2, 3, 4, 5, 6, 7, 1 };
        for (unsigned half = 0; half < 2; half++)
        {
            uint64_t bits = 0, got = 0;
            for (unsigned k = 0; k < 8; k++)
            {
                unsigned c = s[8 + 8 * half + k];
                bits |= (uint64_t)map[c & 7] << (6 * k) | (uint64_t)map[c >> 3 & 7] << (6 * k + 3);
            }
            for (unsigned k = 0; k < 6; k++)
                got |= (uint64_t)out[8 * half + 2 + k] << (8 * k);
            CHECK(got == bits, "BC5: half %u indices", half);
        }
    }
    {
        /* BC3: alpha A0 -3, A1 +2; colour R0 1 R1 2 G0 3 G1 4 B0 5 B1 6 (as zz codes) */
        uint16_t s[24] = { 0, 0, 0, 0, 5, 4, 1, 2, 3, 4, 5, 6, 7, 8, 2, 4, 6, 8, 10, 12, 0x55, 0xaa, 0x0f, 0xf0 };
        uint8_t want[16];
        CHECK(round_trip(t, p, 3, s, 1, NULL, out, NULL) == 0, "BC3 field block round trip");
        expect_plain(t, 3, s, 1, want);
        /* c0 = R0 1, G0 3, B0 5 -> 0x0865 */
        CHECK(!memcmp(out, want, 16) && out[0] == (253 ^ 0x80) && out[1] == (2 ^ 0x80) && out[8] == 0x65
              && out[9] == 0x08, "BC3: hand-checked alpha and colour");
    }
    bcpack_tables_free(p);
    ok("BC1, BC3, BC5: hand-built blocks");
}

/* 400 BC4 blocks of varied symbols: long fragments, the schedule's order. */
static void test_long_fragments(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    size_t blocks = 400;
    uint16_t *rec = calloc(blocks * 12, sizeof(*rec));
    uint8_t *out = calloc(blocks, 8), *want = calloc(blocks, 8);
    size_t size = 0;
    if (!p)
        return;
    random_records(4, rec, blocks, 16);
    for (size_t n = 0; n < blocks; n++)
        rec[n * 12 + 1] = rnd() % (n + 1 < 256 ? n + 1 : 256); /* history only */
    CHECK(round_trip(t, p, 4, rec, blocks, NULL, out, &size) == 0, "400 BC4 blocks round trip (%zu bytes)", size);
    expect_plain(t, 4, rec, blocks, want);
    CHECK(!memcmp(out, want, blocks * 8), "400 BC4 blocks decode as modelled");
    free(rec), free(out), free(want);
    bcpack_tables_free(p);
    ok("400 BC4 blocks: two-byte lengths and the schedule round trip");
}

/* Fragments: several per lane, with and without reset, changing models (alphabet reduction 1-3,
 * shapes 1-15 on skewed models) mid-lane, forced long lengths. Every format. */
static void test_fragments(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    unsigned done[8] = { 0 }, carried = 0, resets = 0, reductions[4] = { 0 }, shaped = 0;
    if (!p)
        return;
    for (unsigned codec = 1; codec < 8; codec++)
    {
        if (!fmt[codec].lanes)
            continue;
        for (unsigned attempt = 0; attempt < 400 && done[codec] < 25; attempt++)
        {
            unsigned len = rec_len(codec), lanes = fmt[codec].lanes, per = fmt[codec].per;
            size_t blocks = 40 + rnd() % 160;
            uint16_t *rec = calloc(blocks * len, sizeof(*rec));
            uint8_t *out = calloc(blocks, 16);
            struct plan plans[16];
            unsigned r_c = 0, r_r = 0, r_red[4] = { 0 }, r_sh = 0;

            random_records(codec, rec, blocks, 16);
            /* BC6H/BC7: keep the control and mode symbols valid */
            for (size_t n = 0; n < blocks; n++)
            {
                if (codec == 6)
                    rec[n * len + 30] %= 14;
                if (codec == 7)
                    rec[n * len + 30] = 0, rec[n * len + 31] = 0;
            }
            memset(plans, 0, sizeof(plans));
            for (unsigned j = 0; j < lanes; j++)
            {
                size_t total = blocks * per, at = 0, cuts[6];
                unsigned G = fmt[codec].group[j];
                unsigned count = 1 + rnd() % 5;
                /* count-1 distinct cut points in 1..total-1, ascending */
                for (unsigned k = 0; k + 1 < count; k++)
                {
                    size_t c = 1 + rnd() % (total - 1);
                    unsigned dup = 0;
                    for (unsigned q = 0; q < k; q++)
                        dup |= cuts[q] == c;
                    if (dup)
                    {
                        count = k + 1;
                        break;
                    }
                    cuts[k] = c;
                    for (unsigned q = k; q > 0 && cuts[q - 1] > cuts[q]; q--)
                    {
                        size_t tmp = cuts[q];
                        cuts[q] = cuts[q - 1], cuts[q - 1] = tmp;
                    }
                }
                cuts[count - 1] = total;
                for (unsigned k = 0; k < count; k++)
                {
                    size_t n = cuts[k] - at;
                    unsigned r = rnd() % 4;
                    plans[j].f[k].n = n;
                    plans[j].f[k].shape = rnd() % 3 ? rnd() % 16 : 0;
                    plans[j].f[k].reset = k < count - 1 && rnd() % 2;
                    plans[j].f[k].force_long = 0;
                    if ((codec == 6 || codec == 7) && j == 15)
                        r = 0; /* mode and control symbols stay whole */
                    plans[j].f[k].r = r;
                    /* alphabet reduction: smaller symbols in this fragment */
                    for (size_t i = at; i < at + n; i++)
                        rec[(i / per) * len + j * per + i % per] &= (1u << (G - r + 1)) - 1;
                    at += n;
                }
                plans[j].count = count;
            }
            int ret = round_trip(t, p, codec, rec, blocks, plans, out, NULL);
            if (ret == -1)
            {
                free(rec), free(out);
                continue; /* not codable as planned (a carry boundary, an empty payload): another */
            }
            CHECK(ret == 0, "BC%u: a multi-fragment stream round trips (%d)", codec, ret);
            done[codec]++;
            for (unsigned j = 0; j < lanes; j++)
                for (unsigned k = 0; k < plans[j].count; k++)
                {
                    if (k + 1 < plans[j].count)
                        *(plans[j].f[k].reset ? &r_r : &r_c) += 1;
                    r_red[plans[j].f[k].r]++;
                    r_sh += plans[j].f[k].shape != 0;
                }
            carried += r_c, resets += r_r, shaped += r_sh;
            for (unsigned k = 0; k < 4; k++)
                reductions[k] += r_red[k];
            /* plain formats: the blocks too */
            if (codec != 6 && codec != 7)
            {
                uint8_t *want = calloc(blocks, 16);
                expect_plain(t, codec, rec, blocks, want);
                CHECK(!memcmp(out, want, blocks * fmt[codec].block), "BC%u: multi-fragment blocks as modelled", codec);
                free(want);
            }
            free(rec), free(out);
        }
        CHECK(done[codec] >= 10, "BC%u: enough multi-fragment streams were codable (%u)", codec, done[codec]);
    }
    CHECK(carried && resets && reductions[1] && reductions[2] && reductions[3] && shaped,
          "fragment boundaries with and without reset, reductions 1-3 and shapes were all exercised");
    printf("     (%u carried and %u reset boundaries; reductions 1/2/3: %u/%u/%u; %u shaped fragments)\n", carried,
           resets, reductions[1], reductions[2], reductions[3], shaped);
    bcpack_tables_free(p);
    ok("fragments: resets, carried state, model changes mid-lane, reductions and shapes");
}

/* Long lengths forced on short payloads too (257+ only: a long length can't say less). */
static void test_forced_long(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    size_t blocks = 300;
    uint16_t *rec = calloc(blocks * 12, sizeof(*rec));
    struct plan plans[16];
    uint8_t *out = calloc(blocks, 8);
    if (!p)
        return;
    random_records(4, rec, blocks, 16);
    for (size_t n = 0; n < blocks; n++)
        rec[n * 12] = rec[n * 12 + 1] = 0;
    memset(plans, 0, sizeof(plans));
    for (unsigned j = 0; j < 4; j++)
    {
        plans[j].count = 1;
        plans[j].f[0].n = blocks;
        plans[j].f[0].force_long = j >= 2; /* 8-bit lanes: 300 symbols need more than 256 bytes */
    }
    CHECK(round_trip(t, p, 4, rec, blocks, plans, out, NULL) == 0, "long lengths round trip");
    free(rec), free(out);
    bcpack_tables_free(p);
    ok("long (two-byte) lengths");
}

/* Budget pressure: every lane wanting bytes at once. With 16 lanes of 8-bit symbols the 8-byte
 * budget freezes the later lanes in most rounds; the decoder must hand out bytes exactly as the
 * schedule model does. Also checks the decoder stops at the same byte as the model. */
static void test_budget(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    size_t blocks = 200;
    uint16_t *rec = calloc(blocks * 32, sizeof(*rec)), *back = calloc(blocks * 32, sizeof(*back));
    struct vec s;
    size_t used = 0;
    if (!p)
        return;
    random_records(7, rec, blocks, 16);
    for (size_t n = 0; n < blocks; n++)
        rec[n * 32 + 30] = 0, rec[n * 32 + 31] = 0;
    CHECK(build_stream(t, 7, rec, blocks, NULL, &s) == 0, "a 16-lane stream builds");
    CHECK(bcpack_unpack_records(p, 7, s.p, s.n, blocks, back, &used) == 0 && !memcmp(back, rec, blocks * 64)
          && used == s.n, "16 lanes under budget pressure decode as the schedule model hands bytes");
    vfree(&s);
    free(rec), free(back);
    bcpack_tables_free(p);
    ok("budget pressure: frozen lanes, blocked lanes");
}

/* BC6H: every mode, both region counts, partitions and anchors, one-region blocks referenced by
 * two-region ones (derived endpoints 2 and 3), index references across widths 3 and 4, the
 * dictionary (index modes outside 1-14 give width 0). */
static void test_bc6h(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    size_t blocks = 700;
    uint16_t *rec = calloc(blocks * 32, sizeof(*rec));
    uint8_t *out = calloc(blocks, 16);
    struct hblock *model = calloc(blocks, sizeof(*model));
    unsigned modes[15] = { 0 }, bad = 0, cross = 0;
    if (!p)
        return;
    random_records(6, rec, blocks, 16);
    for (size_t n = 0; n < blocks; n++)
    {
        uint16_t *s = rec + n * 32;
        s[30] = n < 28 ? n % 14 : rnd() % 14;
        if (n % 2 == 1 && n < 28)
        {
            /* a two-region block right after a one-region one, referencing it */
            s[30] = rnd() % 10;
            rec[(n - 1) * 32 + 30] = 10 + rnd() % 4;
            s[0] = s[1] = 0, s[2] = 1, s[3] = 0; /* I = 1, E = I */
        }
        modes[s[30] + 1]++;
    }
    int r = round_trip(t, p, 6, rec, blocks, NULL, out, NULL);
    CHECK(r == 0, "BC6H: %zu blocks round trip (%d)", blocks, r);
    for (size_t n = 0; n < blocks; n++)
    {
        unsigned E, I;
        model_bc6h(t, rec + n * 32, model, n);
        refs_of(6, rec + n * 32, &E, &I);
        if (I && I <= n && model[n - I].width != model[n].width)
            cross++;
        int c = check_bc6h_block(out + 16 * n, &model[n], rec[n * 32 + 31] & 31);
        if (c && bad++ < 5)
            printf("     BC6H block %zu (mode %u) differs: %d\n", n, model[n].mode, c);
    }
    CHECK(!bad, "BC6H: every block's fields, partition and indices as modelled (%u differ)", bad);
    for (unsigned m = 1; m <= 14; m++)
        CHECK(modes[m] >= 2, "BC6H mode %u was exercised", m);
    CHECK(cross > 10, "BC6H: index references across widths (%u)", cross);
    free(rec), free(out), free(model);
    bcpack_tables_free(p);
    ok("BC6H: all 14 modes, partitions, anchors, references across widths and regions");
}

/* A BC6H mode 11 block by hand: the standard layout, with nothing to interpret. */
static void test_bc6h_hand(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    uint16_t s[32] = { 0 };
    uint8_t out[16], want[16] = { 0 };
    unsigned pos = 0;
    if (!p)
        return;
    /* mode 11: R0 residual 5 (interleave(10, 0) = 0x44 -> zz 34 ... computed below) */
    s[30] = 10;
    s[4] = 3, s[5] = 1;   /* R0: il(3,1) = 0b0111 = 7 -> zz -4 */
    s[6] = 2;              /* G0: il(2,0) = 4 -> 2 */
    s[10] = 1;             /* R1: il(1,0) = 1 -> -1 */
    s[22] = 0x02;          /* byte 0: O nibble 1 -> -1 to pixel 0, E 0 -> pixel 1 */
    s[28] = 0x01;          /* byte 6: E nibble 1 -> -1 to pixel 12, O 0 to pixel 13 */
    CHECK(round_trip(t, p, 6, s, 1, NULL, out, NULL) == 0, "BC6H mode 11 round trip");
#define PUT(v, n) do { for (unsigned k_ = 0; k_ < (n); k_++, pos++) if ((v) >> k_ & 1) want[pos / 8] |= 1 << (pos % 8); } while (0)
    PUT(3u, 5);                       /* mode 11 */
    PUT((unsigned)(-4 & 1023) ^ 512, 10); /* rw */
    PUT(2u ^ 512, 10);                /* gw */
    PUT(0u ^ 512, 10);                /* bw */
    PUT(1023u ^ 512, 10);             /* rx: -1 */
    PUT(512u, 10);                    /* gx */
    PUT(512u, 10);                    /* bx */
    for (unsigned px = 0; px < 16; px++)
    {
        int res = px == 0 || px == 12 ? -1 : 0;
        PUT(((unsigned)res & 15) ^ 8, px == 0 ? 3 : 4);
    }
#undef PUT
    CHECK(!memcmp(out, want, 16), "BC6H mode 11: a hand-built block");
    bcpack_tables_free(p);
    ok("BC6H: a hand-built mode 11 block (index nibble order, anchors)");
}

/* BC7: every mode, every rotation of modes 4 and 5, the mode 4 selector, two- and three-subset
 * partitions with their anchors, rows predicted from a reference's first row, index references
 * across widths 2, 3, 4, and the dictionary. */
static void test_bc7(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    size_t blocks = 1200;
    uint16_t *rec = calloc(blocks * 32, sizeof(*rec));
    uint8_t *out = calloc(blocks, 16);
    struct sblock *model = calloc(blocks, sizeof(*model));
    unsigned bad = 0, modes[8] = { 0 }, rotations[2][4] = { { 0 } }, selector = 0, three_from_one = 0, cross = 0;
    if (!p)
        return;
    random_records(7, rec, blocks, 16);
    for (size_t n = 0; n < blocks; n++)
    {
        uint16_t *s = rec + n * 32;
        unsigned code = n < rich_control_count ? n : rnd() % rich_control_count;
        s[30] = code & 31, s[31] = code >> 5;
        if (n % 5 == 4)
        {
            /* a three-subset block referencing a one-subset one */
            unsigned prev = 1 + rnd() % 14;     /* codes 1-16 are mode 0 (3 subsets); mode 6 is the last */
            unsigned one = rich_control_count - 1;
            code = prev;
            s[30] = code & 31, s[31] = code >> 5;
            rec[(n - 1) * 32 + 30] = one & 31, rec[(n - 1) * 32 + 31] = one >> 5;
            s[0] = s[1] = 0, s[2] = 1, s[3] = 0;
        }
    }
    int r = round_trip(t, p, 7, rec, blocks, NULL, out, NULL);
    CHECK(r == 0, "BC7: %zu blocks round trip (%d)", blocks, r);
    for (size_t n = 0; n < blocks; n++)
    {
        unsigned E, I;
        model_bc7(t, rec + n * 32, model, n);
        unsigned ctl = model[n].ctl, mode = ctl & 7;
        modes[mode]++;
        if (mode == 4 || mode == 5)
            rotations[mode - 4][ctl >> 3 & 3]++;
        selector += mode == 4 && (ctl >> 5 & 1);
        refs_of(7, rec + n * 32, &E, &I);
        if (E && E <= n && (ctl >> 6 & 3) == 3 && (model[n - E].ctl >> 6 & 3) == 1)
            three_from_one++;
        if (I && I <= n && model[n - I].width[0] != model[n].width[0])
            cross++;
        int c = check_bc7_block(out + 16 * n, &model[n]);
        if (c && bad++ < 5)
            printf("     BC7 block %zu (control %#x) differs: %d\n", n, ctl, c);
    }
    CHECK(!bad, "BC7: every block's fields as modelled (%u differ)", bad);
    for (unsigned m = 0; m < 8; m++)
        CHECK(modes[m] > 0, "BC7 mode %u was exercised", m);
    for (unsigned k = 0; k < 4; k++)
        CHECK(rotations[0][k] && rotations[1][k], "BC7 rotation %u in modes 4 and 5", k);
    CHECK(selector && three_from_one && cross, "BC7: selector, three-from-one rows, index widths across");
    free(rec), free(out), free(model);
    bcpack_tables_free(p);
    ok("BC7: all modes, rotations, the selector, partitions and anchors, references");
}

static void test_errors(const uint8_t *t)
{
    struct bcpack_tables *p = parse(t);
    uint16_t rec[64 * 12];
    uint8_t out[66 * 16], full[64 * 8];
    struct vec s;
    size_t used = 0;
    int hr;
    if (!p)
        return;
    random_records(4, rec, 64, 16);
    for (unsigned n = 0; n < 64; n++)
        rec[n * 12] = rec[n * 12 + 1] = 0;
    CHECK(build_stream(t, 4, rec, 64, NULL, &s) == 0, "64 BC4 blocks build");
    CHECK(bcpack_decode(p, 4, s.p, s.n, full, sizeof(full), &used) == BCPACK_OK && used == s.n, "64 BC4 blocks decode");
    CHECK(bcpack_decode(p, 4, s.p, s.n / 2, out, 64 * 8, NULL) == BCPACK_E_INVALID_DATA, "a stream cut in half fails");
    {
        unsigned failed = 0, same = 0, other = 0;
        for (size_t n = 0; n < used; n++)
        {
            hr = bcpack_decode(p, 4, s.p, n, out, 64 * 8, NULL);
            if (hr == BCPACK_E_INVALID_DATA)
                failed++;
            else if (hr == BCPACK_OK && !memcmp(out, full, sizeof(full)))
                same++;
            else
                other++;
        }
        CHECK(!other && failed + same == used && failed > used - 16,
              "truncated at every length: fails, or (only the last few bytes, for parked lanes) decodes the same "
              "(%u failed, %u same, %u other)", failed, same, other);
    }
    {
        struct vec junk = { 0 };
        for (size_t k = 0; k < s.n; k++)
            vpush(&junk, s.p[k]);
        for (unsigned k = 0; k < 100; k++)
            vpush(&junk, rnd());
        CHECK(bcpack_decode(p, 4, junk.p, junk.n, out, 64 * 8, &used) == BCPACK_OK && !memcmp(out, full, sizeof(full))
              && used == s.n, "trailing junk is ignored");
        vfree(&junk);
    }
    CHECK(bcpack_decode(p, 4, s.p, s.n, out, 65 * 8, NULL) == BCPACK_E_INVALID_DATA, "65 blocks from a 64-block stream fail");
    CHECK(bcpack_decode(p, 4, s.p, s.n, out, 64 * 8 + 4, NULL) == BCPACK_E_INVALIDARG, "a partial block fails");
    CHECK(bcpack_decode(p, 4, s.p, s.n, out, 0, NULL) == BCPACK_E_INVALIDARG, "an empty destination fails");
    CHECK(bcpack_decode(p, 4, s.p, 0, out, 8, NULL) == BCPACK_E_INVALID_DATA, "empty input fails");
    CHECK(bcpack_decode(p, 4, NULL, 0, out, 8, NULL) == BCPACK_E_INVALID_DATA, "no input fails");
    {
        uint8_t headers[16];
        memset(headers, 0x30, 8), memset(headers + 8, 0xff, 8);
        CHECK(bcpack_decode(p, 4, headers, 16, out, 8, NULL) == BCPACK_E_INVALID_DATA,
              "headers promising more payload than exists fail");
    }
    for (unsigned codec = 0; codec < 20; codec++)
        if (codec != 1 && (codec < 3 || codec > 7))
            CHECK(bcpack_decode(p, codec, s.p, s.n, out, 64, NULL) == BCPACK_E_NOTIMPL && !bcpack_block_size(codec),
                  "codec %u is unsupported", codec);
    CHECK(bcpack_decode(NULL, 4, s.p, s.n, out, 64 * 8, NULL) == BCPACK_E_NOTIMPL, "no tables: E_NOTIMPL");
    vfree(&s);

    /* BC7 control word 0, BC6H mode symbol 14-31: a valid block first, the bad one second */
    {
        uint16_t r7[64] = { 0 };
        r7[32 + 30] = 300 & 31, r7[32 + 31] = 300 >> 5; /* unused code in the rich table */
        CHECK(build_stream(t, 7, r7, 2, NULL, &s) == 0, "a BC7 stream with an unused control code builds");
        CHECK(bcpack_decode(p, 7, s.p, s.n, out, 32, NULL) == BCPACK_E_INVALID_DATA, "BC7: control word 0 fails");
        CHECK(bcpack_decode(p, 7, s.p, s.n, out, 16, NULL) == BCPACK_OK, "BC7: ... though the block before it decodes");
        vfree(&s);
        for (unsigned m = 13; m < 32; m++)
        {
            uint16_t r6[64] = { 0 };
            r6[30] = 3, r6[32 + 30] = m;
            CHECK(build_stream(t, 6, r6, 2, NULL, &s) == 0, "a BC6H stream builds");
            CHECK(bcpack_decode(p, 6, s.p, s.n, out, 32, NULL) == (m < 14 ? BCPACK_OK : BCPACK_E_INVALID_DATA),
                  "BC6H mode symbol %u %s", m, m < 14 ? "decodes" : "fails");
            vfree(&s);
        }
        CHECK(bcpack_decode(p, 7, (const uint8_t *)"", 0, out, 16, NULL) == BCPACK_E_INVALID_DATA, "an empty BC7 stream fails");
    }
    bcpack_tables_free(p);
    ok("errors: truncation at every length, trailing junk, sizes, codecs, control word 0, BC6H modes 15+");
}

/* Noise: pseudo-random inputs (and mutated valid streams) for every format, with any tables
 * given. Must return success or an error, never crash (the sanitizers watch for the rest). */
static void test_noise(const struct bcpack_tables *p, const char *what)
{
    static const unsigned codecs[6] = { 1, 3, 4, 5, 6, 7 };
    unsigned results[3] = { 0 };
    uint8_t *in = malloc(70000), *out = malloc(4096 * 16);
    for (unsigned ci = 0; ci < 6; ci++)
        for (unsigned k = 0; k < 300; k++)
        {
            unsigned codec = codecs[ci], blocks = 1 + rnd() % (k < 250 ? 300 : 4096);
            size_t n = rnd() % (k % 3 == 0 ? 64 : k % 3 == 1 ? 4096 : 70000);
            unsigned style = rnd() % 4;
            for (size_t i = 0; i < n; i++)
                in[i] = style == 0 ? rnd() : style == 1 ? (rnd() % 8 ? 0 : rnd()) : style == 2 ? (rnd() & 0xcf) : 0xff;
            int32_t hr = bcpack_decode(p, codec, in, n, out, (size_t)blocks * bcpack_block_size(codec), NULL);
            results[hr == BCPACK_OK ? 0 : hr == BCPACK_E_INVALID_DATA ? 1 : 2]++;
        }
    CHECK(!results[2], "%s noise: only success or invalid data (%u other)", what, results[2]);
    printf("     (%s: %u decoded, %u rejected)\n", what, results[0], results[1]);
    free(in), free(out);
    ok("noise on every format (%s): no crash", what);
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(n > 0 ? n : 1);
    *size = fread(data, 1, n > 0 ? n : 0, f);
    fclose(f);
    return data;
}

static int decode_file(int argc, char **argv)
{
    size_t tsize = 0, size = 0, out_size, used = 0;
    uint8_t *tables, *in, *out;
    struct bcpack_tables *p;
    const char *why = "";
    FILE *f;
    int32_t hr;
    if (argc != 7)
    {
        fprintf(stderr, "usage: bcpack_test decode TABLES CODEC SIZE IN OUT\n");
        return 2;
    }
    if (!(tables = load(argv[2], &tsize)) || !(p = bcpack_tables_parse(tables, tsize, &why)))
    {
        fprintf(stderr, "bcpack_test: %s: can't use the tables (%s)\n", argv[2], why);
        return 2;
    }
    out_size = strtoul(argv[4], NULL, 0);
    if (!(in = load(argv[5], &size)) || !(out = calloc(1, out_size ? out_size : 1)))
    {
        fprintf(stderr, "bcpack_test: can't read %s\n", argv[5]);
        return 2;
    }
    hr = bcpack_decode(p, atoi(argv[3]), in, size, out, out_size, &used);
    printf("%#x %zu\n", (unsigned)hr, used);
    if (hr == BCPACK_OK && (f = fopen(argv[6], "wb")))
    {
        fwrite(out, 1, out_size, f);
        fclose(f);
    }
    return hr == BCPACK_OK ? 0 : 1;
}

int main(int argc, char **argv)
{
    uint8_t *spec = malloc(BCPACK_TABLES_SIZE), *rich = malloc(BCPACK_TABLES_SIZE);

    if (argc > 1 && !strcmp(argv[1], "decode"))
        return decode_file(argc, argv);
    synth_tables(spec, 0);
    synth_tables(rich, 1);

    test_tables(spec);
    test_vectors(spec);
    test_bc4_references(spec);
    test_plain_formats(rich);
    test_plain_fields(rich);
    test_long_fragments(spec);
    test_fragments(rich);
    test_forced_long(spec);
    test_budget(rich);
    test_bc6h(rich);
    test_bc6h_hand(rich);
    test_bc7(rich);
    test_errors(rich);
    {
        struct bcpack_tables *p = parse(spec), *q = parse(rich);
        test_noise(p, "12.2's tables");
        test_noise(q, "skewed tables");
        bcpack_tables_free(p), bcpack_tables_free(q);
    }
    if (argc > 1)
    {
        size_t size = 0;
        uint8_t *real = load(argv[1], &size);
        const char *why = "can't read it";
        struct bcpack_tables *p = real ? bcpack_tables_parse(real, size, &why) : NULL;
        CHECK(p != NULL, "the table file %s parses (%s)", argv[1], p ? "ok" : why);
        if (p)
        {
            test_noise(p, "the given table file");
            bcpack_tables_free(p);
        }
        free(real);
    }
    free(spec), free(rich);
    printf("%s: %d of %d checks failed\n", failures ? "FAIL" : "ok  ", failures, checks);
    return failures ? 1 : 0;
}
