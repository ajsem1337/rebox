/* acphal.dll: the coprocessor's XMA decoder, run on a thread of ours.
 *
 * A title that drives XMA itself (rather than through XAudio2) allocates XMA contexts with
 * AcpHalAllocateShapeContexts: 64 bytes each, which the title and the decoder both read and write.
 * It enables a context with a coprocessor command, and the hardware decodes the context's input
 * packets into a ring of 256-byte blocks of 16-bit PCM. The title's mixer takes what lies between
 * its read block and the hardware's write block, and moves its read block on with another
 * command, which frees room. Nothing is mixed from a voice whose write block never moves, so a
 * sound (or a cutscene clocked by its audio) never gets anywhere.
 *
 * The context words, as the target title's mixer reads and writes them (checked in its code):
 *   0: input 0 size in 2 KiB packets (0-11), loop count (12-19, 255 forever), valid inputs
 *      (20-21), output ring size in blocks (22-26), write block (27-31, ours)
 *   1: input 1 size (0-11), loop end subframe (12-13), loop start subframes to skip (17-19),
 *      subframes to skip at the start (24-26), rate (27-28: 24, 32, 44.1, 48 kHz),
 *      stereo (29), output writable (31: clear when the ring is full)
 *   2: read position in bits (0-25), error status (26-30), error (31)
 *   3: loop start in bits (0-25)   4: loop end in bits (0-25), current input (31)
 *   5, 6: input APU addresses   7: output ring APU address   9: read block (0-4, the title's)
 * Commands: 5 enables a context (data word 0: its index), 6 enables those in a bitmask,
 * 7 and 8 disable likewise, 18 moves the read block on (word 0: index, word 1: blocks).
 *
 * Packets go to FFmpeg's XMA2 decoder (the flake's ffmpeg-xma2) one at a time. Loops are
 * honoured to the packet: at the packet holding the loop end, decoding goes on from the packet
 * holding the loop start. A packet that doesn't decode, or a build without FFmpeg, gives silence
 * of the packet's length, so the ring always moves at the stream's pace. */

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

enum { ENABLE = 5, ENABLE_MASK = 6, DISABLE = 7, DISABLE_MASK = 8, ADVANCE_READ = 18 };

struct voice
{
    SRWLOCK lock;          /* the ring's words: write block, read block, writable */
    volatile LONG enabled; /* set by commands */
    volatile LONG generation; /* one more on each enable */
    /* The decoder thread's own. */
    LONG seen;
    BOOL active, ended;
    UINT channels;
    INT16 *pcm;
    SIZE_T pcm_len, pcm_cap, pcm_pos;
    SIZE_T skip;            /* samples per channel still to drop */
    UINT64 expected, made;  /* samples per channel: packets' frame counts, decoded or padded */
    UINT packets, bad, blocks;
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
static volatile LONG logged_enables, logged_disables;

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

/* Interleaved samples, start skips dropped. */
static void emit(struct voice *v, const INT16 *samples, SIZE_T frames)
{
    SIZE_T drop = v->skip < frames ? v->skip : frames;
    v->skip -= drop;
    v->made += frames;
    if (frames > drop)
        push(v, samples ? samples + drop * v->channels : NULL, (frames - drop) * v->channels);
}

#ifdef HAVE_FFMPEG_XMA2
static void decoder_close(struct voice *v)
{
    av_frame_free(&v->frame);
    av_packet_free(&v->packet);
    avcodec_free_context(&v->decoder);
}

static void decoder_open(struct voice *v, UINT rate)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_XMA2);
    AVCodecContext *c;
    static LONG failed;

    av_log_set_level(AV_LOG_QUIET);
    if (!codec || !(c = avcodec_alloc_context3(codec)))
        goto fail;
    c->sample_rate = rate;
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
                  v->channels, rate);
}

static void receive(struct voice *v)
{
    INT16 buf[FRAME_SAMPLES * 2 * 2];

    while (avcodec_receive_frame(v->decoder, v->frame) >= 0)
    {
        AVFrame *f = v->frame;
        UINT ch = v->channels, have = f->ch_layout.nb_channels;
        for (int done = 0; done < f->nb_samples;)
        {
            int n = f->nb_samples - done;
            if (n > FRAME_SAMPLES * 2)
                n = FRAME_SAMPLES * 2;
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
            emit(v, buf, n);
            done += n;
        }
        av_frame_unref(f);
    }
}
#endif

/* One packet's audio. A decoder that falls behind the packets' frame counts by more than a few
 * frames (a packet it refused) is caught up with silence. */
static void decode_packet(struct voice *v, const BYTE *data)
{
    UINT frames = data[0] >> 2;

    v->expected += (UINT64)frames * FRAME_SAMPLES;
    v->packets++;
#ifdef HAVE_FFMPEG_XMA2
    if (v->decoder)
    {
        BYTE padded[PACKET_BYTES + AV_INPUT_BUFFER_PADDING_SIZE];
        memcpy(padded, data, PACKET_BYTES);
        memset(padded + PACKET_BYTES, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        padded[3] = 0; /* the packet skip: the next packet is found here */
        v->packet->data = padded;
        v->packet->size = PACKET_BYTES;
        if (avcodec_send_packet(v->decoder, v->packet) < 0)
            v->bad++;
        receive(v);
    }
#endif
    if (v->expected > v->made + 3 * FRAME_SAMPLES)
        emit(v, NULL, (SIZE_T)(v->expected - v->made));
}

static void drain(struct voice *v)
{
#ifdef HAVE_FFMPEG_XMA2
    if (v->decoder && avcodec_send_packet(v->decoder, NULL) >= 0)
        receive(v);
#endif
    if (v->expected > v->made)
        emit(v, NULL, (SIZE_T)(v->expected - v->made));
}

/* Decodes the next packet of the current input into v->pcm; FALSE when there is nothing left. */
static BOOL decode_more(UINT index, struct voice *v)
{
    UINT current = field(index, 4, 31, 1), valid = field(index, 0, 20, 2);
    UINT32 address, packets, at, p, next, loops;
    const BYTE *data;

    if (v->ended)
        return FALSE;
    if (!(valid & (1u << current)))
    {
        if (!(valid & (1u << (1 - current))))
        {
            /* Out of input: the decoder's tail, then the end. */
            drain(v);
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
    decode_packet(v, data);
    next = p + 1 + data[3];
    loops = field(index, 0, 12, 8);
    if (loops && field(index, 4, 0, 26) && field(index, 4, 0, 26) / PACKET_BITS == p)
    {
        next = field(index, 3, 0, 26) / PACKET_BITS;
        v->skip += field(index, 1, 17, 3) * SUBFRAME_SAMPLES;
        if (loops != 255)
            set_field(index, 0, 12, 8, loops - 1);
#ifdef HAVE_FFMPEG_XMA2
        if (v->decoder)
            avcodec_flush_buffers(v->decoder);
#endif
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

/* Fills the voice's ring as far as it has room. */
static void pump(UINT index, struct voice *v)
{
    UINT size = field(index, 0, 22, 5);
    UINT samples = BLOCK_BYTES / 2;

    if (!size)
        return;
    while (field(index, 1, 31, 1))
    {
        UINT write;
        INT16 *out;
        SIZE_T have;

        while (v->pcm_len - v->pcm_pos < samples && decode_more(index, v))
            ;
        if (v->pcm_len == v->pcm_pos)
            return;
        AcquireSRWLockExclusive(&v->lock);
        write = field(index, 0, 27, 5);
        if (!(out = ApuMapApuAddress((UINT32)*word(index, 7) + write * BLOCK_BYTES)))
        {
            ReleaseSRWLockExclusive(&v->lock);
            return;
        }
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
}

static void voice_stop(UINT index, struct voice *v)
{
    if (v->active && logged_disables < 8 && InterlockedIncrement(&logged_disables) <= 8)
        rebox_log("acphal: XMA voice %u stopped: %u packets (%u refused), %llu samples, %u blocks written",
                  index, v->packets, v->bad, (unsigned long long)v->made, v->blocks);
#ifdef HAVE_FFMPEG_XMA2
    decoder_close(v);
#endif
    v->active = FALSE;
}

static void voice_start(UINT index, struct voice *v)
{
    static const UINT rates[4] = { 24000, 32000, 44100, 48000 };

    voice_stop(index, v);
    v->active = TRUE;
    v->ended = FALSE;
    v->channels = 1 + field(index, 1, 29, 1);
    v->pcm_len = v->pcm_pos = 0;
    v->skip = field(index, 1, 24, 3) * SUBFRAME_SAMPLES;
    v->expected = v->made = 0;
    v->packets = v->bad = v->blocks = 0;
#ifdef HAVE_FFMPEG_XMA2
    decoder_open(v, rates[field(index, 1, 27, 2)]);
#else
    (void)rates;
#endif
}

static DWORD WINAPI decoder_thread(void *arg)
{
    for (;;)
    {
        WaitForSingleObject(wake, 2); /* a 256-byte block lasts 2.7 ms at 48 kHz mono */
        for (UINT i = 0; i < count; i++)
        {
            struct voice *v = &voices[i];
            LONG generation = v->generation;

            if (!v->enabled)
            {
                if (v->active)
                    voice_stop(i, v);
                continue;
            }
            if (!v->active || v->seen != generation)
            {
                v->seen = generation;
                voice_start(i, v);
            }
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
    set_field(index, 2, 26, 6, 0);
    InterlockedIncrement(&v->generation);
    InterlockedExchange(&v->enabled, 1);
    if (logged_enables < 8 && InterlockedIncrement(&logged_enables) <= 8)
        rebox_log("acphal: XMA voice %u enabled (%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx)",
                  index, *word(index, 0), *word(index, 1), *word(index, 2), *word(index, 3), *word(index, 4),
                  *word(index, 5), *word(index, 6), *word(index, 7), *word(index, 8), *word(index, 9));
}

static void disable(UINT index)
{
    if (index < count)
        InterlockedExchange(&voices[index].enabled, 0);
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
