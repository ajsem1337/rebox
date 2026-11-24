/* acphal.dll: the coprocessor's XMA decoder, run on a thread of ours.
 *
 * A title that drives XMA itself (rather than through XAudio2) allocates XMA contexts with
 * AcpHalAllocateShapeContexts: 64 bytes each, which the title and the decoder both read and write.
 * It enables a context with a coprocessor command, and the hardware decodes the context's input
 * packets into a ring of 256-byte blocks of 16-bit PCM. The title's mixer takes what lies between
 * its read block and the hardware's write block, and moves its read block on with another
 * command, which frees room.
 *
 * The context words, as the target title sets them up (its code at 0x785d0) and reads them (its
 * mixer at 0x78120):
 *   0: input 0 size in 2 KiB packets (0-11), loop count (12-19, 255 forever), valid inputs
 *      (20-21), output ring size in blocks (22-26), write block (27-31, ours)
 *   1: input 1 size (0-11), loop end subframe (12-13), loop start subframes to skip (17-19),
 *      subframes to skip at the start (24-26), rate (27-28: 24, 32, 44.1, 48 kHz),
 *      stereo (29), output writable (31: clear when the ring is full)
 *   2: read position in bits (0-25): where the first frame to play starts, counted from the
 *      input's first byte, packet headers included; error status (26-30), error (31)
 *   3: loop start in bits (0-25)   4: loop end in bits (0-25), current input (31)
 *   5, 6: input APU addresses   7: output ring APU address   9: read block (0-4, the title's)
 * A packet's 32-bit header (big-endian) holds the frames that start in it (31-26), the bit
 * offset of the first of them after the header (25-11) and the packets to skip to this stream's
 * next one (7-0); each frame starts with its length in bits (15 bits).
 * Commands: 5 enables a context (data word 0: its index), 6 enables those in a bitmask,
 * 7 and 8 disable likewise, 18 moves the read block on (word 0: index, word 1: blocks).
 *
 * The title starts a voice anywhere in its input (granular sounds start several voices a frame
 * or two apart in one buffer) and counts the samples it takes against the input's length in
 * frames: a voice is done when the count reaches it, and nothing else ends it. So the output is
 * frame-exact: frames before word 2's position are decoded and dropped, the subframe skips are
 * honoured, a loop ends inside the frame holding the loop end and goes on at the loop start, and
 * a voice whose input is used up gets one frame of silence more than it asked for, never less.
 *
 * Packets go to FFmpeg's XMA2 decoder (the flake's ffmpeg-xma2) one at a time, and its output
 * is matched to the frames fed in. Frames FFmpeg doesn't give (a packet it refuses, or a build
 * without FFmpeg) are silence of their length, so the ring always moves at the stream's pace. */

#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "acphal.h"

#ifdef HAVE_FFMPEG_XMA2
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#endif

#define PACKET_BYTES 2048
#define PACKET_BITS (PACKET_BYTES * 8)
#define BLOCK_BYTES 256
#define FRAME_SAMPLES 512
#define SUBFRAME_SAMPLES 128
#define INPUT_ENDED 4 /* error status: the input ran out */
#define PENDING 256   /* frames fed to the decoder, not yet given back */

enum { ENABLE = 5, ENABLE_MASK = 6, DISABLE = 7, DISABLE_MASK = 8, ADVANCE_READ = 18 };

struct voice
{
    /* Held by commands and by the decoder thread for each step, so a voice the title disables
     * and sets up again is never written with the old one's state. */
    SRWLOCK lock;
    LONG enabled, generation; /* set by commands */
    /* The decoder thread's own. */
    LONG seen;
    BOOL active, ended;
    UINT channels, rate;
    INT16 *pcm;
    SIZE_T pcm_len, pcm_cap, pcm_pos;
    SIZE_T skip;               /* samples per channel still to drop */
    UINT32 target;             /* bit position of the first frame to keep in the next packet, or 0 */
    UINT16 keep[PENDING];      /* samples to keep of each pending frame (0..512) */
    UINT head, pending, in_frame;
    UINT packets, frames, padded, blocks, taken;
#ifdef HAVE_FFMPEG_XMA2
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *frame;
#endif
};

static BYTE *contexts;
static UINT count;
static struct voice *voices;
static HANDLE wake, thread;
static volatile LONG logged_enables, logged_stops;

static volatile LONG *word(UINT index, UINT w)
{
    return (volatile LONG *)(contexts + (SIZE_T)index * 64 + w * 4);
}

static UINT32 field(UINT index, UINT w, UINT shift, UINT bits)
{
    return ((UINT32)*word(index, w) >> shift) & ((1u << bits) - 1);
}

/* The title writes other fields of the same word at any time: change only ours, atomically. */
static void set_field(UINT index, UINT w, UINT shift, UINT bits, UINT32 value)
{
    volatile LONG *p = word(index, w);
    UINT32 mask = ((1u << bits) - 1) << shift;
    LONG old, new;

    do
    {
        old = *p;
        new = (LONG)(((UINT32)old & ~mask) | ((value << shift) & mask));
    } while (InterlockedCompareExchange(p, new, old) != old);
}

static BOOL push(struct voice *v, const INT16 *samples, SIZE_T n)
{
    if (v->pcm_pos && v->pcm_pos == v->pcm_len)
        v->pcm_pos = v->pcm_len = 0;
    if (v->pcm_len + n > v->pcm_cap)
    {
        SIZE_T cap = v->pcm_cap ? v->pcm_cap : 1 << 14;
        INT16 *p;
        if (v->pcm_pos)
        {
            memmove(v->pcm, v->pcm + v->pcm_pos, (v->pcm_len - v->pcm_pos) * sizeof(INT16));
            v->pcm_len -= v->pcm_pos;
            v->pcm_pos = 0;
        }
        while (cap < v->pcm_len + n)
            cap *= 2;
        if (cap > v->pcm_cap)
        {
            if (!(p = realloc(v->pcm, cap * sizeof(INT16))))
                return FALSE;
            v->pcm = p;
            v->pcm_cap = cap;
        }
    }
    if (samples)
        memcpy(v->pcm + v->pcm_len, samples, n * sizeof(INT16));
    else
        memset(v->pcm + v->pcm_len, 0, n * sizeof(INT16));
    v->pcm_len += n;
    return TRUE;
}

/* Kept samples (interleaved; NULL for silence) go to the ring's queue after the skips. */
static void emit(struct voice *v, const INT16 *samples, SIZE_T frames)
{
    SIZE_T drop = v->skip < frames ? v->skip : frames;
    v->skip -= drop;
    if (frames > drop)
        push(v, samples ? samples + drop * v->channels : NULL, (frames - drop) * v->channels);
}

static void queue_frame(struct voice *v, UINT keep)
{
    if (v->pending == PENDING)
        return;
    v->keep[(v->head + v->pending++) % PENDING] = (UINT16)keep;
    v->frames++;
}

/* Decoded samples, matched to the pending frames: each frame's first `keep` samples are kept.
 * Output with no frame pending is dropped. */
static void take_decoded(struct voice *v, const INT16 *samples, SIZE_T n)
{
    while (n && v->pending)
    {
        UINT keep = v->keep[v->head], left = FRAME_SAMPLES - v->in_frame;
        SIZE_T step = n < left ? n : left;
        if (v->in_frame < keep)
        {
            SIZE_T k = keep - v->in_frame < step ? keep - v->in_frame : step;
            emit(v, samples, k);
        }
        samples += step * v->channels;
        n -= step;
        v->in_frame += (UINT)step;
        if (v->in_frame == FRAME_SAMPLES)
        {
            v->head = (v->head + 1) % PENDING;
            v->pending--;
            v->in_frame = 0;
        }
    }
}

/* The oldest pending frames, as silence of their kept length. */
static void pad_frames(struct voice *v, UINT n)
{
    while (n-- && v->pending)
    {
        UINT keep = v->keep[v->head];
        if (v->in_frame < keep)
            emit(v, NULL, keep - v->in_frame);
        v->head = (v->head + 1) % PENDING;
        v->pending--;
        v->in_frame = 0;
        v->padded++;
    }
}

#ifdef HAVE_FFMPEG_XMA2
static void decoder_close(struct voice *v)
{
    av_frame_free(&v->frame);
    av_packet_free(&v->packet);
    avcodec_free_context(&v->decoder);
}

static void decoder_open(struct voice *v)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext *c;
    static LONG failed;

    decoder_close(v);
    av_log_set_level(AV_LOG_QUIET);
    if (!codec || !(c = avcodec_alloc_context3(codec)))
        goto fail;
    c->sample_rate = v->rate;
    av_channel_layout_default(&c->ch_layout, v->channels);
    c->block_align = PACKET_BYTES;
    /* The XMA2WAVEFORMATEX tail FFmpeg reads: one stream, no channel mask. */
    if ((c->extradata = av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE)))
    {
        c->extradata[0] = 1;
        c->extradata_size = 34;
    }
    if (avcodec_open2(c, codec, NULL) < 0)
    {
        avcodec_free_context(&c);
        goto fail;
    }
    v->decoder = c;
    v->packet = av_packet_alloc();
    v->frame = av_frame_alloc();
    if (v->packet && v->frame)
        return;
    decoder_close(v);
fail:
    if (!InterlockedExchange(&failed, 1))
        rebox_log("acphal: FFmpeg's XMA2 decoder didn't open (%u channels, %u Hz): XMA plays as silence",
                  v->channels, v->rate);
}

static void receive(struct voice *v)
{
    INT16 buf[FRAME_SAMPLES * 2];

    while (avcodec_receive_frame(v->decoder, v->frame) >= 0)
    {
        AVFrame *f = v->frame;
        UINT ch = v->channels, have = f->ch_layout.nb_channels;
        for (int done = 0; done < f->nb_samples;)
        {
            int n = f->nb_samples - done;
            if (n > FRAME_SAMPLES)
                n = FRAME_SAMPLES;
            for (int i = 0; i < n; i++)
                for (UINT k = 0; k < ch; k++)
                {
                    float s = 0.0f;
                    if (k < have && f->format == AV_SAMPLE_FMT_FLTP)
                        s = ((const float *)f->extended_data[k])[done + i];
                    else if (k < have && f->format == AV_SAMPLE_FMT_FLT)
                        s = ((const float *)f->extended_data[0])[(done + i) * have + k];
                    s = s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s;
                    buf[i * ch + k] = (INT16)(s * 32767.0f);
                }
            take_decoded(v, buf, n);
            done += n;
        }
        av_frame_unref(f);
    }
}
#endif

/* A starting fresh: a new decoder (it takes up at the first frame that starts in the next packet
 * fed), nothing pending. */
static void restart(struct voice *v)
{
    pad_frames(v, PENDING); /* none expected: frames are matched before a restart */
#ifdef HAVE_FFMPEG_XMA2
    decoder_open(v);
#endif
    v->in_frame = 0;
}

/* Feeds one packet. Frames that start in it are queued with what to keep of each. */
static void feed(struct voice *v, const BYTE *data)
{
#ifdef HAVE_FFMPEG_XMA2
    if (v->decoder)
    {
        BYTE padded[PACKET_BYTES + AV_INPUT_BUFFER_PADDING_SIZE];
        memcpy(padded, data, PACKET_BYTES);
        memset(padded + PACKET_BYTES, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        padded[3] = 0; /* the packet skip: the next packet is found here */
        v->packet->data = padded;
        v->packet->size = PACKET_BYTES;
        avcodec_send_packet(v->decoder, v->packet);
        receive(v);
    }
#endif
    v->packets++;
}

static void drain(struct voice *v)
{
#ifdef HAVE_FFMPEG_XMA2
    if (v->decoder && avcodec_send_packet(v->decoder, NULL) >= 0)
        receive(v);
#endif
    pad_frames(v, PENDING);
}

/* 15 bits at a bit offset in a packet (big-endian bit order), or -1 past its end. */
static int bits15(const BYTE *packet, UINT32 at)
{
    UINT32 v;
    if (at + 15 > PACKET_BITS)
        return -1;
    v = (UINT32)packet[at / 8] << 16 | (UINT32)packet[at / 8 + 1] << 8 | (at / 8 + 2 < PACKET_BYTES ? packet[at / 8 + 2] : 0);
    return (v >> (24 - 15 - at % 8)) & 0x7fff;
}

/* Decodes the next packet of the current input; FALSE when there is nothing left. */
static BOOL decode_more(UINT index, struct voice *v)
{
    UINT current = field(index, 4, 31, 1), valid = field(index, 0, 20, 2);
    UINT32 address, packets, at, p, next, base, loops, loop_end, start;
    const BYTE *data;
    UINT n;
    BOOL looped = FALSE;

    if (v->ended)
        return FALSE;
    if (!(valid & (1u << current)))
    {
        if (!(valid & (1u << (1 - current))))
        {
            /* Out of input: what the decoder still holds, a frame of silence more (the title's
             * count must reach its end), then the end. */
            drain(v);
            emit(v, NULL, FRAME_SAMPLES);
            v->ended = TRUE;
            set_field(index, 2, 26, 5, INPUT_ENDED);
            set_field(index, 2, 31, 1, 1);
            return v->pcm_len > v->pcm_pos;
        }
        current = 1 - current;
        set_field(index, 4, 31, 1, current);
        set_field(index, 2, 0, 26, 0);
    }
    address = (UINT32)*word(index, current ? 6 : 5) & ~0x7ffu;
    packets = current ? field(index, 1, 0, 12) : field(index, 0, 0, 12);
    at = field(index, 2, 0, 26);
    p = at / PACKET_BITS;
    if (p >= packets || !(data = ApuMapApuAddress(address)))
    {
        set_field(index, 0, 20 + current, 1, 0); /* this input is used up */
        set_field(index, 2, 0, 26, 0);
        return TRUE;
    }
    data += (SIZE_T)p * PACKET_BYTES;
    base = p * PACKET_BITS;
    n = data[0] >> 2;
    loops = field(index, 0, 12, 8);
    loop_end = loops ? field(index, 4, 0, 26) : 0;
    next = p + 1 + data[3];

    /* Where each frame that starts here begins, and what to keep of it. */
    start = base + 32 + ((data[0] & 3u) << 13 | (UINT32)data[1] << 5 | data[2] >> 3);
    for (UINT i = 0; i < n; i++)
    {
        int length = bits15(data, start - base);
        /* The next frame's start; one running into the next packet skips its header. */
        UINT32 after = length > 0 ? start + (UINT32)length : base + PACKET_BITS;
        if (length > 0 && after > base + PACKET_BITS)
            after += 32;
        UINT keep = FRAME_SAMPLES;

        if (v->target && start < v->target)
            keep = 0; /* before where the title starts */
        if (!looped && keep && loop_end && start <= loop_end && loop_end < after)
        {
            keep = (field(index, 1, 12, 2) + 1) * SUBFRAME_SAMPLES;
            looped = TRUE;
        }
        else if (looped)
            keep = 0;
        queue_frame(v, keep);
        start = after;
    }
    v->target = 0;
    feed(v, data);
    /* A packet FFmpeg refused leaves its frames pending: silence, so the stream keeps its pace. */
    if (v->pending > n + 2)
        pad_frames(v, v->pending - n - 2);

    if (looped)
    {
        UINT32 loop_start = field(index, 3, 0, 26);
        /* What the decoder holds of this packet's frames, then a new decoder at the loop start. */
        drain(v);
        restart(v);
        v->skip += field(index, 1, 17, 3) * SUBFRAME_SAMPLES;
        v->target = loop_start;
        if (loops != 255)
            set_field(index, 0, 12, 8, loops - 1);
        set_field(index, 2, 0, 26, loop_start);
        return TRUE;
    }
    if (next >= packets)
    {
        set_field(index, 0, 20 + current, 1, 0);
        set_field(index, 2, 0, 26, 0);
    }
    else
        set_field(index, 2, 0, 26, next * PACKET_BITS + 32);
    return TRUE;
}

/* Fills the voice's ring as far as it has room, a block at a time under the voice's lock. */
static void pump(UINT index, struct voice *v)
{
    const UINT samples = BLOCK_BYTES / 2;

    for (;;)
    {
        UINT size, write;
        INT16 *out;
        SIZE_T have;

        AcquireSRWLockExclusive(&v->lock);
        if (!v->enabled || v->generation != v->seen || !(size = field(index, 0, 22, 5))
                || !field(index, 1, 31, 1))
            break;
        while (v->pcm_len - v->pcm_pos < samples && decode_more(index, v))
            ;
        if (v->pcm_len == v->pcm_pos)
            break;
        write = field(index, 0, 27, 5);
        if (!(out = ApuMapApuAddress((UINT32)*word(index, 7) + write * BLOCK_BYTES)))
            break;
        have = v->pcm_len - v->pcm_pos;
        if (have > samples)
            have = samples;
        memcpy(out, v->pcm + v->pcm_pos, have * sizeof(INT16));
        memset(out + have, 0, (samples - have) * sizeof(INT16));
        v->pcm_pos += have;
        write = (write + 1) % size;
        set_field(index, 0, 27, 5, write);
        if (write == field(index, 9, 0, 5))
            set_field(index, 1, 31, 1, 0); /* full until the title reads */
        v->blocks++;
        ReleaseSRWLockExclusive(&v->lock);
    }
    ReleaseSRWLockExclusive(&v->lock);
}

static void voice_stop(UINT index, struct voice *v, const char *why)
{
    if (v->active && logged_stops < 32 && InterlockedIncrement(&logged_stops) <= 32)
        rebox_log("acphal: XMA voice %u %s: %u packets, %u frames (%u as silence), %u blocks written, "
                  "%u taken%s", index, why, v->packets, v->frames, v->padded, v->blocks, v->taken,
                  v->ended ? ", its input used up" : "");
#ifdef HAVE_FFMPEG_XMA2
    decoder_close(v);
#endif
    v->active = FALSE;
}

/* Called with the voice's lock held, for the generation the title just set up. */
static void voice_start(UINT index, struct voice *v)
{
    static const UINT rates[4] = { 24000, 32000, 44100, 48000 };

    voice_stop(index, v, "set up again by the title");
    v->active = TRUE;
    v->ended = FALSE;
    v->channels = 1 + field(index, 1, 29, 1);
    v->rate = rates[field(index, 1, 27, 2)];
    v->pcm_len = v->pcm_pos = 0;
    v->skip = field(index, 1, 24, 3) * SUBFRAME_SAMPLES;
    v->target = field(index, 2, 0, 26);
    v->head = v->pending = v->in_frame = 0;
    v->packets = v->frames = v->padded = v->blocks = v->taken = 0;
    restart(v);
}

static DWORD WINAPI decoder_thread(void *arg)
{
    for (;;)
    {
        WaitForSingleObject(wake, 2); /* a 256-byte block lasts 2.7 ms at 48 kHz mono */
        for (UINT i = 0; i < count; i++)
        {
            struct voice *v = &voices[i];

            if (!v->enabled && !v->active)
                continue;
            AcquireSRWLockExclusive(&v->lock);
            if (!v->enabled)
            {
                voice_stop(i, v, "disabled by the title");
                ReleaseSRWLockExclusive(&v->lock);
                continue;
            }
            if (!v->active || v->seen != v->generation)
            {
                v->seen = v->generation;
                voice_start(i, v);
            }
            ReleaseSRWLockExclusive(&v->lock);
            pump(i, v);
        }
    }
    return 0;
}

void xma_contexts(BYTE *array, UINT n)
{
    if (contexts || !array || !n || !(voices = calloc(n, sizeof(*voices))))
        return;
    for (UINT i = 0; i < n; i++)
        InitializeSRWLock(&voices[i].lock);
    contexts = array;
    count = n;
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if ((thread = CreateThread(NULL, 0, decoder_thread, NULL, 0, NULL)))
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
#ifdef HAVE_FFMPEG_XMA2
    rebox_log("acphal: %u XMA contexts decoded on a thread of ours (FFmpeg's XMA2 decoder)", n);
#else
    rebox_log("acphal: %u XMA contexts run on a thread of ours as silence (no FFmpeg)", n);
#endif
}

static void enable(UINT index)
{
    struct voice *v;

    if (index >= count)
        return;
    v = &voices[index];
    AcquireSRWLockExclusive(&v->lock);
    set_field(index, 2, 26, 6, 0);
    v->generation++;
    v->enabled = 1;
    ReleaseSRWLockExclusive(&v->lock);
    if (logged_enables < 16 && InterlockedIncrement(&logged_enables) <= 16)
        rebox_log("acphal: XMA voice %u enabled (%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx)",
                  index, *word(index, 0), *word(index, 1), *word(index, 2), *word(index, 3), *word(index, 4),
                  *word(index, 5), *word(index, 6), *word(index, 7), *word(index, 8), *word(index, 9));
}

static void disable(UINT index)
{
    if (index >= count)
        return;
    AcquireSRWLockExclusive(&voices[index].lock);
    voices[index].enabled = 0;
    ReleaseSRWLockExclusive(&voices[index].lock);
}

/* The title took `blocks` blocks: its read block moves on, which frees room. */
static void advance_read(UINT index, UINT32 blocks)
{
    struct voice *v;
    UINT size;

    if (index >= count)
        return;
    v = &voices[index];
    AcquireSRWLockExclusive(&v->lock);
    size = field(index, 0, 22, 5);
    if (!size)
        size = 1;
    set_field(index, 9, 0, 5, (field(index, 9, 0, 5) + blocks % size) % size);
    if (blocks)
        set_field(index, 1, 31, 1, 1);
    v->taken += blocks;
    ReleaseSRWLockExclusive(&v->lock);
}

BOOL xma_command(UINT32 type, const UINT32 *data)
{
    if (type != ENABLE && type != ENABLE_MASK && type != DISABLE && type != DISABLE_MASK && type != ADVANCE_READ)
        return FALSE;
    if (!contexts || !data)
        return TRUE;
    switch (type)
    {
    case ENABLE: enable(data[0]); break;
    case DISABLE: disable(data[0]); break;
    case ADVANCE_READ: advance_read(data[0], data[1]); break;
    default:
        for (UINT i = 0; i < count; i++)
            if (data[i / 32] & (1u << (i % 32)))
                type == ENABLE_MASK ? enable(i) : disable(i);
    }
    SetEvent(wake);
    return TRUE;
}
