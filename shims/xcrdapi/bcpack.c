/* BCPack decoder (bcpack.h).
 *
 * A packed stream is 12 or 16 byte-interleaved lanes of interleaved-rANS symbols. Each block takes
 * one or two symbols from every lane, its "record". A record holds two back references (one for
 * endpoints, one for indices) into the last 255 decoded blocks of the request, or past its start
 * into the format's dictionary, plus residuals that are added to what the references give. The
 * result, kept in an internal form (history), is packed into a standard BCn block.
 *
 * Decoding is one pass: the schedule hands stream bytes to lanes, lanes decode symbols, and each
 * block is reconstructed and written as soon as its record is complete. Nothing is read outside
 * the source, the destination, the tables or the history ring, whatever the input. */
#include <stdlib.h>
#include <string.h>

#include "bcpack.h"

/* ------------------------------------------------------------------------------------- tables */

struct model
{
    uint16_t f[9];      /* per-symbol frequency of each bucket */
    uint8_t used;       /* buckets in use: group + 2 */
    uint32_t start[9];  /* cumulative start of each used bucket */
};

/* A decoded block in internal form, also the form of dictionary entries:
 *   BC1/3/4/5  ep: endpoint elements; idx: index elements (widths per format)
 *   BC6H       ep[3*k + c]: endpoint k (w x y z), channel c, 16-bit words; width[0]: index width
 *   BC7        ctl: control word; ep[8*row + col]: subset rows x (R0 R1 G0 G1 B0 B1 A0 A1);
 *              width[0..1], idx[16*set + p]: primary and secondary index sets */
struct entry
{
    uint16_t ctl;
    uint8_t width[2];
    uint16_t ep[24];
    uint16_t idx[32];
};

struct bcpack_tables
{
    struct model models[128];
    uint8_t rank_bc1[256], rank_bc3_alpha[64], rank_bc3_colour[256], rank_bc4[64], rank_bc5[64];
    uint16_t control[512];
    struct entry dict[8][255]; /* by codec */
};

static unsigned u16le(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static unsigned bucket_count(unsigned b)
{
    return b < 2 ? 1 : 1u << (b - 1);
}

static unsigned bc6h_index_width(unsigned mode)
{
    return mode >= 1 && mode <= 10 ? 3 : mode >= 11 && mode <= 14 ? 4 : 0;
}

static void parse_dictionary(struct bcpack_tables *t, unsigned codec, const uint8_t *p)
{
    static const uint8_t endpoints[8] = { 0, 6, 0, 8, 2, 4 }, indices[8] = { 0, 16, 0, 32, 16, 32 };
    static const unsigned sizes[8] = { 0, 22, 0, 40, 18, 36, 58, 116 };

    for (unsigned i = 0; i < 255; i++, p += sizes[codec])
    {
        struct entry *e = &t->dict[codec][i];
        memset(e, 0, sizeof(*e));
        if (codec == 6)
        {
            /* p[0]: a mode byte of the endpoint half, not used in decoding */
            for (unsigned k = 0; k < 12; k++)
                e->ep[k] = u16le(p + 1 + 2 * k);
            e->ctl = p[25];
            e->width[0] = bc6h_index_width(p[25]);
            for (unsigned k = 0; k < 16; k++)
                e->idx[k] = u16le(p + 26 + 2 * k);
        }
        else if (codec == 7)
        {
            e->ctl = u16le(p);
            for (unsigned k = 0; k < 24; k++)
                e->ep[k] = u16le(p + 2 + 2 * k);
            e->width[0] = p[50];
            e->width[1] = p[51];
            for (unsigned k = 0; k < 32; k++)
                e->idx[k] = u16le(p + 52 + 2 * k);
        }
        else
        {
            for (unsigned k = 0; k < endpoints[codec]; k++)
                e->ep[k] = p[k];
            for (unsigned k = 0; k < indices[codec]; k++)
                e->idx[k] = p[endpoints[codec] + k];
        }
    }
}

struct bcpack_tables *bcpack_tables_parse(const uint8_t *data, size_t size, const char **why)
{
    struct bcpack_tables *t;
    const char *dummy;

    if (!why)
        why = &dummy;
    if (!data || size != BCPACK_TABLES_SIZE)
    {
        *why = "the table file isn't 77,982 bytes";
        return NULL;
    }
    if (!(t = calloc(1, sizeof(*t))))
    {
        *why = "out of memory";
        return NULL;
    }
    for (unsigned m = 0; m < 128; m++)
    {
        struct model *model = &t->models[m];
        uint32_t sum = 0;

        model->used = m / 16 + 2;
        for (unsigned b = 0; b < 9; b++)
            model->f[b] = u16le(data + 18 * m + 2 * b);
        for (unsigned b = 0; b < 9; b++)
        {
            model->start[b] = b < model->used ? sum : 65536;
            if (b < model->used)
            {
                if (!model->f[b])
                {
                    free(t);
                    *why = "a model has a used bucket of frequency 0";
                    return NULL;
                }
                sum += (uint32_t)model->f[b] * bucket_count(b);
            }
        }
        if (sum != 65536)
        {
            free(t);
            *why = "a model's buckets don't tile 65,536";
            return NULL;
        }
    }
    memcpy(t->rank_bc1, data + 2304, 256);
    memcpy(t->rank_bc3_alpha, data + 2560, 64);
    memcpy(t->rank_bc3_colour, data + 2624, 256);
    memcpy(t->rank_bc4, data + 2880, 64);
    memcpy(t->rank_bc5, data + 2944, 64);
    for (unsigned i = 0; i < 512; i++)
        t->control[i] = u16le(data + 3008 + 2 * i);
    parse_dictionary(t, 1, data + 4032);
    parse_dictionary(t, 3, data + 9642);
    parse_dictionary(t, 4, data + 19842);
    parse_dictionary(t, 5, data + 24432);
    parse_dictionary(t, 6, data + 33612);
    parse_dictionary(t, 7, data + 48402);
    return t;
}

void bcpack_tables_free(struct bcpack_tables *tables)
{
    free(tables);
}

/* ------------------------------------------------------------------------------ the formats */

struct format
{
    unsigned block_size, lanes, per;
    uint8_t group[16]; /* each lane's group ceiling G: symbols of G+1 bits */
};

static const struct format formats[8] = {
    [1] = { 8, 12, 1, { 7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7 } },
    [3] = { 16, 12, 2, { 3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7 } },
    [4] = { 8, 12, 1, { 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5 } },
    [5] = { 16, 12, 2, { 3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5 } },
    [6] = { 16, 16, 2, { 3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4 } },
    [7] = { 16, 16, 2, { 3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4 } },
};

static const struct format *format_of(unsigned codec)
{
    return codec < 8 && formats[codec].block_size ? &formats[codec] : NULL;
}

unsigned bcpack_block_size(unsigned codec)
{
    const struct format *f = format_of(codec);
    return f ? f->block_size : 0;
}

unsigned bcpack_record_length(unsigned codec)
{
    const struct format *f = format_of(codec);
    return f ? f->lanes * f->per : 0;
}

/* ------------------------------------------------------------------------- the entropy layer */

enum { HEADER, LENGTH_LOW, LENGTH_HIGH, PAYLOAD };

#define RANS_L (1u << 24)

struct lane
{
    const struct model *model;
    uint32_t x, remaining;
    uint16_t pending;
    uint8_t group, stage, reset, long_length, inbox, has_inbox, wants, has_pending, frozen;
};

/* One rANS symbol from y (> 2^24) with the lane's model; the new state goes to lane->x. */
static void decode_symbol(struct lane *lane, uint32_t y)
{
    const struct model *m = lane->model;
    uint32_t slot = y & 0xffff, freq, cum;
    unsigned b = m->used - 1, symbol;

    while (m->start[b] > slot) /* start[0] is 0 */
        b--;
    if (b == 0)
    {
        /* Slot 0 is never used: symbol 0 owns slots 1 to f[0]-1. Slot 0 itself (not in a valid
         * stream) decodes as symbol 0 too, with a wrapped state. */
        symbol = 0, freq = m->f[0] - 1u, cum = 1;
    }
    else
    {
        uint32_t q = b == 1 ? 0 : (slot - m->start[b]) / m->f[b];
        symbol = (b == 1 ? 1 : 1u << (b - 1)) + q;
        freq = m->f[b];
        cum = m->start[b] + q * m->f[b];
    }
    lane->x = (y >> 16) * freq + slot - cum;
    lane->pending = symbol;
    lane->has_pending = 1;
}

static void lane_step(const struct bcpack_tables *t, struct lane *lane)
{
    uint32_t y;

    if (lane->stage != PAYLOAD)
    {
        unsigned h;
        if (!lane->has_inbox)
            return;
        h = lane->inbox;
        lane->has_inbox = 0;
        lane->wants = 1;
        switch (lane->stage)
        {
        case HEADER:
            /* bits 0-3 shape, 4 reset, 5 long length, 6-7 alphabet reduction (G is at least 3) */
            lane->model = &t->models[16 * (lane->group - (h >> 6)) + (h & 15)];
            lane->reset = h >> 4 & 1;
            lane->long_length = h >> 5 & 1;
            lane->remaining = 0;
            lane->stage = LENGTH_LOW;
            break;
        case LENGTH_LOW:
            if (lane->long_length)
                lane->remaining = h, lane->stage = LENGTH_HIGH;
            else
                lane->remaining = h + 1, lane->stage = PAYLOAD;
            break;
        default: /* LENGTH_HIGH */
            lane->remaining += 256 * h + 257;
            lane->stage = PAYLOAD;
            break;
        }
        return;
    }

    y = lane->x;
    if (y < RANS_L && lane->has_inbox)
    {
        y = y << 8 | lane->inbox;
        lane->has_inbox = 0;
        lane->remaining--; /* a byte only arrives while the payload has more */
    }
    lane->wants = lane->remaining > 0 && !lane->has_inbox;
    if (y > RANS_L)
        decode_symbol(lane, y);
    else if (lane->remaining > 0)
        lane->x = y;
    else
    {
        /* The fragment is finished. A byte read in this very step doesn't reach the state. */
        lane->stage = HEADER;
        lane->wants = !lane->has_inbox;
        if (lane->reset)
            lane->x = 0;
    }
}

/* Runs the schedule: rounds in which every lane gets a turn and at most 8 stream bytes are handed
 * out, until every lane has delivered every symbol of `blocks` blocks. block() is called with each
 * complete record; a non-zero return stops decoding with that result. */
typedef int32_t (*block_fn)(void *ctx, size_t n, const uint16_t *record);

static int32_t run_schedule(const struct bcpack_tables *t, const struct format *f, const uint8_t *src,
                            size_t src_size, size_t blocks, block_fn block, void *ctx, size_t *consumed)
{
    struct lane lanes[16];
    uint16_t record[32];
    size_t pos = 0, total = blocks * f->per, in = 0;
    unsigned delivered = 0, all = (1u << f->lanes) - 1, idle = 0;
    int32_t hr = BCPACK_OK;

    memset(lanes, 0, sizeof(lanes));
    memset(record, 0, sizeof(record));
    for (unsigned j = 0; j < f->lanes; j++)
    {
        lanes[j].group = f->group[j];
        lanes[j].stage = HEADER;
        lanes[j].wants = 1;
    }

    while (pos < total)
    {
        unsigned taken = 0, progress = 0;

        for (unsigned j = 0; j < f->lanes; j++)
        {
            struct lane *lane = &lanes[j];
            unsigned taken_before = taken, blocked;

            if (!lane->frozen)
                lane_step(t, lane);
            blocked = lane->has_pending && (delivered >> j & 1);
            if (lane->wants && !blocked && taken < 8 && in < src_size)
            {
                lane->inbox = src[in++];
                lane->has_inbox = 1;
                taken++;
            }
            lane->frozen = (lane->wants && taken_before >= 8) || blocked;
            if (!lane->frozen && lane->has_pending)
            {
                record[j * f->per + pos % f->per] = lane->pending;
                lane->has_pending = 0;
                delivered |= 1u << j;
                progress = 1;
            }
        }
        if (delivered == all)
        {
            pos++;
            delivered = 0;
            progress = 1;
            if (pos % f->per == 0 && (hr = block(ctx, pos / f->per - 1, record)))
                break;
        }
        if (taken == 0 && !progress)
        {
            if (++idle > 3)
            {
                hr = BCPACK_E_INVALID_DATA; /* the stream ends early, or no lane can go on */
                break;
            }
        }
        else
            idle = 0;
    }
    if (consumed)
        *consumed = in;
    return hr;
}

/* ------------------------------------------------------------------------------- reconstruction */

struct decoder
{
    const struct bcpack_tables *t;
    unsigned codec;
    uint8_t *dst;
    struct entry zero;
    struct entry history[256];
};

static const struct entry *resolve(const struct decoder *d, size_t n, unsigned r)
{
    if (r == 0)
        return &d->zero;
    if (r <= n)
        return &d->history[(n - r) & 255];
    return &d->t->dict[d->codec][r - n - 1]; /* r <= 255, so at most entry 254 */
}

static uint32_t zigzag(uint32_t u)
{
    return (u >> 1) ^ (0u - (u & 1));
}

/* a's bits on even positions, b's on odd */
static unsigned interleave(unsigned a, unsigned b)
{
    unsigned r = 0;
    for (unsigned k = 0; k < 8; k++)
        r |= (a >> k & 1) << (2 * k) | (b >> k & 1) << (2 * k + 1);
    return r;
}

/* The endpoint and index references of a record. */
static void references(const uint16_t *s, int paired, unsigned *e, unsigned *i)
{
    unsigned delta = paired ? interleave(s[0], s[1]) : s[0];
    *i = paired ? interleave(s[2], s[3]) : s[1];
    *e = (*i + zigzag(delta)) & 255;
}

/* 128-bit (or 64-bit) block writer, LSB first; bits past 128 are dropped. */
struct bits
{
    uint64_t lo, hi;
    unsigned pos;
};

static void put(struct bits *b, uint64_t v, unsigned n)
{
    unsigned pos = b->pos;
    if (!n)
        return;
    v &= n >= 64 ? ~0ull : (1ull << n) - 1;
    b->pos += n;
    if (pos >= 128)
        return;
    if (pos < 64)
    {
        b->lo |= v << pos;
        if (pos && pos + n > 64)
            b->hi |= v >> (64 - pos);
    }
    else
        b->hi |= v << (pos - 64);
}

static void store_bits(uint8_t *out, const struct bits *b, unsigned bytes)
{
    for (unsigned k = 0; k < bytes; k++)
        out[k] = (uint8_t)(k < 8 ? b->lo >> (8 * k) : b->hi >> (8 * (k - 8)));
}

/* --------------------------------------------------------------------------- BC1, BC3, BC4, BC5 */

/* A quad code's four 2-bit residuals: the 2x2 quad at 8*(q/2) + 2*(q%2). */
static void add_quad(uint16_t *idx, const uint16_t *ref, unsigned q, unsigned rank)
{
    static const uint8_t offsets[4] = { 0, 1, 4, 5 };
    unsigned top = 8 * (q / 2) + 2 * (q % 2);
    for (unsigned k = 0; k < 4; k++)
    {
        unsigned p = top + offsets[k];
        idx[p] = ((rank >> (2 * k)) + ref[p]) & 3;
    }
}

/* A pair code's two 3-bit residuals, for pixels 2k and 2k+1. */
static void add_pair(uint16_t *idx, const uint16_t *ref, unsigned k, unsigned rank)
{
    idx[2 * k] = (rank + ref[2 * k]) & 7;
    idx[2 * k + 1] = ((rank >> 4) + ref[2 * k + 1]) & 7;
}

static void pack_bc1_half(uint8_t *out, const uint16_t *ep, const uint16_t *idx)
{
    static const uint8_t map[4] = { 0, 2, 3, 1 };
    unsigned c0 = ep[0] << 11 | ep[1] << 5 | ep[2], c1 = ep[3] << 11 | ep[4] << 5 | ep[5];
    uint32_t bits = 0;
    for (unsigned p = 0; p < 16; p++)
        bits |= (uint32_t)map[idx[p] & 3] << (2 * p);
    out[0] = c0, out[1] = c0 >> 8, out[2] = c1, out[3] = c1 >> 8;
    for (unsigned k = 0; k < 4; k++)
        out[4 + k] = bits >> (8 * k);
}

static void pack_bc4_half(uint8_t *out, unsigned e0, unsigned e1, const uint16_t *idx)
{
    static const uint8_t map[8] = { 0, 2, 3, 4, 5, 6, 7, 1 };
    uint64_t bits = 0;
    for (unsigned p = 0; p < 16; p++)
        bits |= (uint64_t)map[idx[p] & 7] << (3 * p);
    out[0] = e0 ^ 0x80, out[1] = e1 ^ 0x80;
    for (unsigned k = 0; k < 6; k++)
        out[2 + k] = bits >> (8 * k);
}

static void endpoints_plain(struct entry *b, const struct entry *ref, const uint16_t *s, const uint8_t *from,
                            const uint8_t *widths, unsigned count)
{
    for (unsigned k = 0; k < count; k++)
        b->ep[k] = (zigzag(s[from[k]]) + ref->ep[k]) & ((1u << widths[k]) - 1);
}

static int32_t block_plain(void *ctx, size_t n, const uint16_t *s)
{
    static const uint8_t bc1_from[6] = { 2, 3, 4, 5, 6, 7 }, bc1_widths[6] = { 5, 6, 5, 5, 6, 5 };
    static const uint8_t bc3_from[8] = { 14, 16, 18, 15, 17, 19, 4, 5 }, bc3_widths[8] = { 5, 6, 5, 5, 6, 5, 8, 8 };
    static const uint8_t bc4_from[2] = { 2, 3 }, bc5_from[4] = { 4, 5, 6, 7 }, eights[4] = { 8, 8, 8, 8 };
    struct decoder *d = ctx;
    const struct bcpack_tables *t = d->t;
    struct entry *b = &d->history[n & 255];
    const struct entry *er, *ir;
    unsigned e, i;
    uint8_t *out = d->dst + n * formats[d->codec].block_size;

    references(s, d->codec == 3 || d->codec == 5, &e, &i);
    er = resolve(d, n, e);
    ir = resolve(d, n, i);
    /* b may be the slot of block n-256, which no reference reaches: overwrite it in place, but
     * only after reading er and ir, which are other slots or tables. */
    {
        struct entry next;
        memset(&next, 0, sizeof(next));
        switch (d->codec)
        {
        case 1:
            endpoints_plain(&next, er, s, bc1_from, bc1_widths, 6);
            for (unsigned q = 0; q < 4; q++)
                add_quad(next.idx, ir->idx, q, t->rank_bc1[s[8 + q] & 255]);
            pack_bc1_half(out, next.ep, next.idx);
            break;
        case 3:
            endpoints_plain(&next, er, s, bc3_from, bc3_widths, 8);
            for (unsigned k = 0; k < 8; k++)
                add_pair(next.idx, ir->idx, k, t->rank_bc3_alpha[s[6 + k] & 63]);
            for (unsigned q = 0; q < 4; q++)
                add_quad(next.idx + 16, ir->idx + 16, q, t->rank_bc3_colour[s[20 + q] & 255]);
            pack_bc4_half(out, next.ep[6], next.ep[7], next.idx);
            pack_bc1_half(out + 8, next.ep, next.idx + 16);
            break;
        case 4:
            endpoints_plain(&next, er, s, bc4_from, eights, 2);
            for (unsigned k = 0; k < 8; k++)
                add_pair(next.idx, ir->idx, k, t->rank_bc4[s[4 + k] & 63]);
            pack_bc4_half(out, next.ep[0], next.ep[1], next.idx);
            break;
        default: /* 5 */
            endpoints_plain(&next, er, s, bc5_from, eights, 4);
            for (unsigned k = 0; k < 16; k++)
                add_pair(next.idx, ir->idx, k, t->rank_bc5[s[8 + k] & 63]);
            pack_bc4_half(out, next.ep[0], next.ep[1], next.idx);
            pack_bc4_half(out + 8, next.ep[2], next.ep[3], next.idx + 16);
            break;
        }
        *b = next;
    }
    return BCPACK_OK;
}

/* ------------------------------------------------------------------------- BC6H and BC7 shared */

/* An index residual byte t (0-7) gives pixels 2t and 2t+1: the nibbles of its even bits (E) and
 * odd bits (O), each zigzag-decoded to -8..7. Bytes 0-5 put O first, bytes 6 and 7 E first. */
static void index_residuals(int *res, const uint16_t *bytes, const uint8_t *from)
{
    for (unsigned t = 0; t < 8; t++)
    {
        unsigned v = bytes[from[t]] & 255;
        unsigned even = (v & 1) | (v >> 1 & 2) | (v >> 2 & 4) | (v >> 3 & 8);
        unsigned odd = (v >> 1 & 1) | (v >> 2 & 2) | (v >> 3 & 4) | (v >> 4 & 8);
        int e = (int)(even >> 1) ^ -(int)(even & 1), o = (int)(odd >> 1) ^ -(int)(odd & 1);
        res[2 * t] = t < 6 ? o : e;
        res[2 * t + 1] = t < 6 ? e : o;
    }
}

/* A residual of width b added to a reference of width rb, both as fractions of the circle. */
static unsigned add_index(int res, unsigned b, unsigned ref, unsigned rb)
{
    uint32_t a, r;
    if (b == 0 || b > 16)
        return 0;
    a = ((uint32_t)res & ((1u << b) - 1)) << (16 - b);
    r = rb >= 1 && rb <= 16 ? (ref & ((1u << rb) - 1)) << (16 - rb) : 0;
    return ((a + r) & 0xffff) >> (16 - b);
}

static uint16_t top_bits(unsigned n)
{
    return n >= 16 ? 0xffff : (uint16_t)(0xffff << (16 - n));
}

/* ---------------------------------------------------------------------------------------- BC6H */

/* Appendix B of the format notes: each mode's header fields after its mode bits, LSB first, as
 * {field, low bit, bits}; field 3*endpoint + channel (endpoints w x y z, channels r g b), 12 the
 * partition. */
static const uint8_t bc6h_layout[14][24][3] = {
    /* 1 */ { { 7, 4, 1 }, { 8, 4, 1 }, { 11, 4, 1 }, { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 5 },
              { 10, 4, 1 }, { 7, 0, 4 }, { 4, 0, 5 }, { 11, 0, 1 }, { 10, 0, 4 }, { 5, 0, 5 }, { 11, 1, 1 },
              { 8, 0, 4 }, { 6, 0, 5 }, { 11, 2, 1 }, { 9, 0, 5 }, { 11, 3, 1 }, { 12, 0, 5 } },
    /* 2 */ { { 7, 5, 1 }, { 10, 4, 1 }, { 10, 5, 1 }, { 0, 0, 7 }, { 11, 0, 1 }, { 11, 1, 1 }, { 8, 4, 1 },
              { 1, 0, 7 }, { 8, 5, 1 }, { 11, 2, 1 }, { 7, 4, 1 }, { 2, 0, 7 }, { 11, 3, 1 }, { 11, 5, 1 },
              { 11, 4, 1 }, { 3, 0, 6 }, { 7, 0, 4 }, { 4, 0, 6 }, { 10, 0, 4 }, { 5, 0, 6 }, { 8, 0, 4 },
              { 6, 0, 6 }, { 9, 0, 6 }, { 12, 0, 5 } },
    /* 3 */ { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 5 }, { 0, 10, 1 }, { 7, 0, 4 }, { 4, 0, 4 },
              { 1, 10, 1 }, { 11, 0, 1 }, { 10, 0, 4 }, { 5, 0, 4 }, { 2, 10, 1 }, { 11, 1, 1 }, { 8, 0, 4 },
              { 6, 0, 5 }, { 11, 2, 1 }, { 9, 0, 5 }, { 11, 3, 1 }, { 12, 0, 5 } },
    /* 4 */ { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 4 }, { 0, 10, 1 }, { 10, 4, 1 }, { 7, 0, 4 },
              { 4, 0, 5 }, { 1, 10, 1 }, { 10, 0, 4 }, { 5, 0, 4 }, { 2, 10, 1 }, { 11, 1, 1 }, { 8, 0, 4 },
              { 6, 0, 4 }, { 11, 0, 1 }, { 11, 2, 1 }, { 9, 0, 4 }, { 7, 4, 1 }, { 11, 3, 1 }, { 12, 0, 5 } },
    /* 5 */ { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 4 }, { 0, 10, 1 }, { 8, 4, 1 }, { 7, 0, 4 },
              { 4, 0, 4 }, { 1, 10, 1 }, { 11, 0, 1 }, { 10, 0, 4 }, { 5, 0, 5 }, { 2, 10, 1 }, { 8, 0, 4 },
              { 6, 0, 4 }, { 11, 1, 1 }, { 11, 2, 1 }, { 9, 0, 4 }, { 11, 4, 1 }, { 11, 3, 1 }, { 12, 0, 5 } },
    /* 6 */ { { 0, 0, 9 }, { 8, 4, 1 }, { 1, 0, 9 }, { 7, 4, 1 }, { 2, 0, 9 }, { 11, 4, 1 }, { 3, 0, 5 },
              { 10, 4, 1 }, { 7, 0, 4 }, { 4, 0, 5 }, { 11, 0, 1 }, { 10, 0, 4 }, { 5, 0, 5 }, { 11, 1, 1 },
              { 8, 0, 4 }, { 6, 0, 5 }, { 11, 2, 1 }, { 9, 0, 5 }, { 11, 3, 1 }, { 12, 0, 5 } },
    /* 7 */ { { 0, 0, 8 }, { 10, 4, 1 }, { 8, 4, 1 }, { 1, 0, 8 }, { 11, 2, 1 }, { 7, 4, 1 }, { 2, 0, 8 },
              { 11, 3, 1 }, { 11, 4, 1 }, { 3, 0, 6 }, { 7, 0, 4 }, { 4, 0, 5 }, { 11, 0, 1 }, { 10, 0, 4 },
              { 5, 0, 5 }, { 11, 1, 1 }, { 8, 0, 4 }, { 6, 0, 6 }, { 9, 0, 6 }, { 12, 0, 5 } },
    /* 8 */ { { 0, 0, 8 }, { 11, 0, 1 }, { 8, 4, 1 }, { 1, 0, 8 }, { 7, 5, 1 }, { 7, 4, 1 }, { 2, 0, 8 },
              { 10, 5, 1 }, { 11, 4, 1 }, { 3, 0, 5 }, { 10, 4, 1 }, { 7, 0, 4 }, { 4, 0, 6 }, { 10, 0, 4 },
              { 5, 0, 5 }, { 11, 1, 1 }, { 8, 0, 4 }, { 6, 0, 5 }, { 11, 2, 1 }, { 9, 0, 5 }, { 11, 3, 1 },
              { 12, 0, 5 } },
    /* 9 */ { { 0, 0, 8 }, { 11, 1, 1 }, { 8, 4, 1 }, { 1, 0, 8 }, { 8, 5, 1 }, { 7, 4, 1 }, { 2, 0, 8 },
              { 11, 5, 1 }, { 11, 4, 1 }, { 3, 0, 5 }, { 10, 4, 1 }, { 7, 0, 4 }, { 4, 0, 5 }, { 11, 0, 1 },
              { 10, 0, 4 }, { 5, 0, 6 }, { 8, 0, 4 }, { 6, 0, 5 }, { 11, 2, 1 }, { 9, 0, 5 }, { 11, 3, 1 },
              { 12, 0, 5 } },
    /* 10 */ { { 0, 0, 6 }, { 10, 4, 1 }, { 11, 0, 1 }, { 11, 1, 1 }, { 8, 4, 1 }, { 1, 0, 6 }, { 7, 5, 1 },
               { 8, 5, 1 }, { 11, 2, 1 }, { 7, 4, 1 }, { 2, 0, 6 }, { 10, 5, 1 }, { 11, 3, 1 }, { 11, 5, 1 },
               { 11, 4, 1 }, { 3, 0, 6 }, { 7, 0, 4 }, { 4, 0, 6 }, { 10, 0, 4 }, { 5, 0, 6 }, { 8, 0, 4 },
               { 6, 0, 6 }, { 9, 0, 6 }, { 12, 0, 5 } },
    /* 11 */ { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 10 }, { 4, 0, 10 }, { 5, 0, 10 } },
    /* 12 */ { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 9 }, { 0, 10, 1 }, { 4, 0, 9 }, { 1, 10, 1 },
               { 5, 0, 9 }, { 2, 10, 1 } },
    /* 13: the high base bits in ascending order, as the console writes them */
    { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 8 }, { 0, 10, 2 }, { 4, 0, 8 }, { 1, 10, 2 },
      { 5, 0, 8 }, { 2, 10, 2 } },
    /* 14: likewise */
    { { 0, 0, 10 }, { 1, 0, 10 }, { 2, 0, 10 }, { 3, 0, 4 }, { 0, 10, 6 }, { 4, 0, 4 }, { 1, 10, 6 },
      { 5, 0, 4 }, { 2, 10, 6 } },
};
static const uint8_t bc6h_layout_count[14] = { 20, 24, 19, 21, 21, 20, 20, 22, 22, 24, 6, 9, 9, 9 };

/* base bits, then R G B bits of the other endpoints; mode field value and width */
static const uint8_t bc6h_modes[14][6] = {
    { 10, 5, 5, 5, 0, 2 },   { 7, 6, 6, 6, 1, 2 },    { 11, 5, 4, 4, 2, 5 },   { 11, 4, 5, 4, 6, 5 },
    { 11, 4, 4, 5, 10, 5 },  { 9, 5, 5, 5, 14, 5 },   { 8, 6, 5, 5, 18, 5 },   { 8, 5, 6, 5, 22, 5 },
    { 8, 5, 5, 6, 26, 5 },   { 6, 6, 6, 6, 30, 5 },   { 10, 10, 10, 10, 3, 5 }, { 11, 9, 9, 9, 7, 5 },
    { 12, 8, 8, 8, 11, 5 },  { 16, 4, 4, 4, 15, 5 },
};

/* Appendix A: the anchor of the second subset (two subsets), and of the second and third (three). */
static const uint8_t anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
static const uint8_t anchor3_second[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
static const uint8_t anchor3_third[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};

static const uint8_t primary_index_bytes[8] = { 22, 23, 24, 25, 26, 27, 28, 29 };
static const uint8_t secondary_index_bytes[8] = { 12, 13, 14, 15, 16, 17, 20, 21 };

static int32_t block_bc6h(void *ctx, size_t n, const uint16_t *s)
{
    struct decoder *d = ctx;
    const struct entry *er, *ir;
    struct entry b;
    unsigned e, i, mode, two, partition, sh0, base, width;
    int res[16];
    struct bits w = { 0, 0, 0 };
    uint8_t *out = d->dst + n * 16;

    if (s[30] >= 14)
        return BCPACK_E_INVALID_DATA;
    mode = s[30] + 1;
    two = mode <= 10;
    partition = s[31] & 31;
    base = bc6h_modes[mode - 1][0];
    sh0 = 16 - base;
    width = two ? 3 : 4;

    references(s, 1, &e, &i);
    er = resolve(d, n, e);
    ir = resolve(d, n, i);
    memset(&b, 0, sizeof(b));
    b.ctl = mode;
    b.width[0] = width;
    for (unsigned c = 0; c < 3; c++)
    {
        unsigned bits = bc6h_modes[mode - 1][1 + c], shc = 16 - bits;
        uint32_t r0 = zigzag(interleave(s[4 + 2 * c], s[5 + 2 * c]));
        uint32_t r1 = zigzag(interleave(s[10 + 2 * c], s[11 + 2 * c]));

        b.ep[c] = (er->ep[c] + (r0 << sh0)) & top_bits(base);
        b.ep[3 + c] = (er->ep[3 + c] + (r1 << shc)) & top_bits(bits);
        if (two)
        {
            b.ep[6 + c] = (er->ep[6 + c] + (zigzag(s[16 + 2 * c]) << shc)) & top_bits(bits);
            b.ep[9 + c] = (er->ep[9 + c] + (zigzag(s[17 + 2 * c]) << shc)) & top_bits(bits);
        }
        else
        {
            /* Only for later two-region blocks that reference this one. */
            b.ep[6 + c] = (uint16_t)((b.ep[c] >> sh0) << shc);
            b.ep[9 + c] = b.ep[3 + c];
        }
    }
    index_residuals(res, s, primary_index_bytes);
    for (unsigned p = 0; p < 16; p++)
        b.idx[p] = add_index(res[p], width, ir->idx[p], ir->width[0]);

    put(&w, bc6h_modes[mode - 1][4], bc6h_modes[mode - 1][5]);
    for (unsigned k = 0; k < bc6h_layout_count[mode - 1]; k++)
    {
        const uint8_t *field = bc6h_layout[mode - 1][k];
        unsigned v;
        if (field[0] == 12)
            v = partition;
        else
        {
            unsigned endpoint = field[0] / 3, c = field[0] % 3;
            unsigned shift = endpoint ? 16 - bc6h_modes[mode - 1][1 + c] : sh0;
            v = (b.ep[field[0]] ^ 0x8000u) >> shift;
        }
        put(&w, v >> field[1], field[2]);
    }
    for (unsigned p = 0; p < 16; p++)
    {
        unsigned anchor = p == 0 || (two && p == anchor2[partition]);
        put(&w, b.idx[p] ^ (1u << (width - 1)), width - anchor);
    }
    store_bits(out, &w, 16);
    d->history[n & 255] = b;
    return BCPACK_OK;
}

/* ----------------------------------------------------------------------------------------- BC7 */

/* Per mode: R G B A endpoint bits, p-bits (0 none, 1 shared per subset, 2 per endpoint), primary
 * and secondary index bits, subsets. */
static const uint8_t bc7_modes[8][8] = {
    { 4, 4, 4, 0, 2, 3, 0, 3 }, { 6, 6, 6, 0, 1, 3, 0, 2 }, { 5, 5, 5, 0, 0, 2, 0, 3 }, { 7, 7, 7, 0, 2, 2, 0, 2 },
    { 5, 5, 5, 6, 0, 3, 2, 1 }, { 7, 7, 7, 8, 0, 2, 2, 1 }, { 7, 7, 7, 7, 2, 4, 0, 1 }, { 5, 5, 5, 5, 2, 2, 0, 2 },
};

/* Residual symbol per (row, column R0 R1 G0 G1 B0 B1 A0 A1); 0 means none. */
static const uint8_t bc7_residuals[2][3][8] = {
    { { 4, 5, 6, 7, 8, 9, 10, 11 }, { 12, 13, 14, 15, 16, 17, 18, 19 }, { 0 } },
    { { 4, 5, 6, 7, 8, 9, 0, 0 }, { 12, 13, 14, 15, 16, 17, 0, 0 }, { 20, 21, 10, 11, 18, 19, 0, 0 } },
};

static int32_t block_bc7(void *ctx, size_t n, const uint16_t *s)
{
    struct decoder *d = ctx;
    const struct entry *er, *ir;
    const uint8_t *m;
    struct entry b;
    unsigned e, i, control, mode, rotation, selector, subsets, partition, common, columns, pbits, stream_subsets;
    unsigned precision[4];
    int res[16];
    struct bits w = { 0, 0, 0 };
    uint8_t *out = d->dst + n * 16;

    control = d->t->control[(s[31] & 15) << 5 | (s[30] & 31)];
    if (!control)
        return BCPACK_E_INVALID_DATA; /* an unused control code */
    mode = control & 7;
    rotation = control >> 3 & 3;
    selector = control >> 5 & 1;
    subsets = control >> 6 & 3;
    partition = control >> 8 & 63;
    m = bc7_modes[mode];
    pbits = m[4];

    /* Working precisions: the channels after rotation; with p-bits, the p-bit is red's low bit. */
    for (unsigned c = 0; c < 4; c++)
        precision[c] = m[c];
    if (rotation)
    {
        unsigned tmp = precision[3];
        precision[3] = precision[rotation - 1];
        precision[rotation - 1] = tmp;
    }
    else if (pbits)
        precision[0]++;

    references(s, 1, &e, &i);
    er = resolve(d, n, e);
    ir = resolve(d, n, i);
    memset(&b, 0, sizeof(b));
    b.ctl = control;

    /* Endpoints. Rows the reference doesn't have are predicted from its first row. */
    common = er->ctl >> 6 & 3;
    if (common > subsets)
        common = subsets;
    columns = precision[3] ? 8 : 6;
    for (unsigned c = 0; c < columns; c++)
    {
        unsigned p = precision[c / 2];
        if (!p)
            continue;
        for (unsigned row = 0; row < subsets; row++)
        {
            unsigned from = bc7_residuals[subsets == 3][row][c];
            int delta = (int8_t)(from ? zigzag(s[from]) : 0);
            unsigned base = er->ep[8 * (row < common ? row : 0) + c] & top_bits(p);
            b.ep[8 * row + c] = (uint16_t)(base + (unsigned)delta * (1u << (16 - p)));
        }
    }

    /* Indices: the primary set, and in modes 4 and 5 the secondary. */
    b.width[0] = m[5];
    b.width[1] = m[6];
    for (unsigned set = 0; set < 2; set++)
    {
        if (!b.width[set])
            continue;
        index_residuals(res, s, set ? secondary_index_bytes : primary_index_bytes);
        for (unsigned p = 0; p < 16; p++)
            b.idx[16 * set + p] = add_index(res[p], b.width[set], ir->idx[16 * set + p], ir->width[set]);
    }

    /* Packing, by the mode's own subset count. */
    stream_subsets = m[7];
    if (mode == 0)
        partition &= 15;
    put(&w, 1u << mode, mode + 1);
    if (mode == 0)
        put(&w, partition, 4);
    else if (mode == 1 || mode == 2 || mode == 3 || mode == 7)
        put(&w, partition, 6);
    else if (mode == 4)
        put(&w, rotation, 2), put(&w, selector, 1);
    else if (mode == 5)
        put(&w, rotation, 2);
    for (unsigned sc = 0; sc < 4; sc++)
    {
        /* With a rotation, the rotated channel's stream slot takes internal A and vice versa. */
        unsigned ic = !rotation ? sc : sc == rotation - 1 ? 3 : sc == 3 ? rotation - 1 : sc;
        unsigned p = precision[ic], drop = ic == 0 && pbits && !rotation;
        if (!p)
            continue;
        for (unsigned sub = 0; sub < stream_subsets; sub++)
            for (unsigned ep = 0; ep < 2; ep++)
            {
                unsigned v = b.ep[8 * sub + 2 * ic + ep] >> (16 - p);
                put(&w, v >> drop, p - drop);
            }
    }
    if (pbits)
    {
        unsigned shift = 16 - precision[0];
        for (unsigned sub = 0; sub < stream_subsets; sub++)
            for (unsigned ep = 0; ep < (pbits == 1 ? 1u : 2u); ep++)
                put(&w, b.ep[8 * sub + ep] >> shift & 1, 1);
    }
    for (unsigned k = 0; k < 2; k++)
    {
        /* Mode 4 writes its secondary (2-bit) set first. */
        unsigned set = mode == 4 ? 1 - k : k, width = b.width[set];
        if (!width)
            continue;
        for (unsigned p = 0; p < 16; p++)
        {
            unsigned anchor = p == 0;
            if (set == 0 && stream_subsets == 2)
                anchor |= p == anchor2[partition];
            else if (set == 0 && stream_subsets == 3)
                anchor |= p == anchor3_second[partition] || p == anchor3_third[partition];
            put(&w, b.idx[16 * set + p], width - anchor);
        }
    }
    store_bits(out, &w, 16);
    d->history[n & 255] = b;
    return BCPACK_OK;
}

/* ------------------------------------------------------------------------------------- entry */

int32_t bcpack_decode(const struct bcpack_tables *tables, unsigned codec, const uint8_t *src, size_t src_size,
                      uint8_t *dst, size_t dst_size, size_t *consumed)
{
    const struct format *f = format_of(codec);
    struct decoder *d;
    block_fn block;
    int32_t hr;

    if (consumed)
        *consumed = 0;
    if (!f || !tables)
        return BCPACK_E_NOTIMPL;
    if (!dst || !dst_size || dst_size % f->block_size)
        return BCPACK_E_INVALIDARG;
    if (!src)
        src_size = 0;
    if (!(d = calloc(1, sizeof(*d))))
        return BCPACK_E_OUTOFMEMORY;
    d->t = tables;
    d->codec = codec;
    d->dst = dst;
    if (codec == 6)
        d->zero.width[0] = 4, d->zero.ctl = 11; /* the zero entry has 4-bit indices */
    block = codec == 6 ? block_bc6h : codec == 7 ? block_bc7 : block_plain;
    hr = run_schedule(tables, f, src, src_size, dst_size / f->block_size, block, d, consumed);
    free(d);
    return hr;
}

struct records
{
    uint16_t *out;
    unsigned length;
};

static int32_t copy_record(void *ctx, size_t n, const uint16_t *record)
{
    struct records *r = ctx;
    memcpy(r->out + n * r->length, record, r->length * sizeof(*record));
    return BCPACK_OK;
}

int32_t bcpack_unpack_records(const struct bcpack_tables *tables, unsigned codec, const uint8_t *src,
                              size_t src_size, size_t blocks, uint16_t *records, size_t *consumed)
{
    const struct format *f = format_of(codec);
    struct records r;

    if (consumed)
        *consumed = 0;
    if (!f || !tables)
        return BCPACK_E_NOTIMPL;
    if (!blocks || !records)
        return BCPACK_E_INVALIDARG;
    r.out = records;
    r.length = f->lanes * f->per;
    return run_schedule(tables, f, src ? src : (const uint8_t *)"", src ? src_size : 0, blocks, copy_record, &r,
                        consumed);
}
