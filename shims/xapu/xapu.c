/* xapu.dll: the console's audio decoder queue: Opus packets in, 48 kHz float PCM out, per stream
 * (an Activate gives a stream's channels, 1 or 2; a Process decodes one packet and keeps
 * frame_count frames from first_frame on; a connection option 4 asks for the two channels one
 * after the other instead of interleaved). What it decodes and those meanings: VIctory (GPL-3.0,
 * facts only). Each command is done when it is queued, so its result is ready at once. Decoding is
 * FFmpeg's Opus decoder (the flake's ffmpeg-xma2, HAVE_FFMPEG_OPUS); without it, or for a packet
 * it refuses, the output is silence of the length asked for, so a title's audio engine (FMOD in
 * a GDK title) still runs.
 * Layouts and behaviour follow xcl (ivall/xcl, GPL-2.0-only, crates/xcl/src/hle/acphal.rs):
 *   HRESULT XApuConnect(const XAPU_CONNECT_INPUT *, XAPU_CONNECT_OUTPUT *, HANDLE *)
 *   HRESULT XApuDisconnect(HANDLE)
 *   HRESULT XApuEnqueueCommand(HANDLE, const XAPU_COMMAND *, UINT32 *results_waiting)
 *   HRESULT XApuDequeueResult(HANDLE, XAPU_RESULT *)   0x8AC81001 when there is none
 *   UINT32 XApuGetResultCount(HANDLE, UINT32 *)
 *   HRESULT XApuGetUtilization(UINT32, UINT32, UINT32 *)
 * All the structs are packed. */

#include <windows.h>

#ifdef HAVE_FFMPEG_OPUS
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#endif

#include "../common/rebox_log.h"

#define XAPU_NO_RESULTS ((HRESULT)0x8AC81001)
enum { XAPU_ACTIVATE = 1, XAPU_DEACTIVATE = 2, XAPU_PROCESS = 3, XAPU_RESET = 5 };
enum { XAPU_DEINTERLEAVED = 4 };
#define MAX_FRAMES 5760 /* the longest Opus packet at 48 kHz, 120 ms */

#pragma pack(push, 1)
struct xapu_command { UINT32 type, stream, sequence, options; void *context; };
struct xapu_activate { struct xapu_command id; UINT32 channels; };
struct xapu_process
{
    struct xapu_command id;
    float ramp, pitch;
    UINT32 first_frame, frames, input_bytes;
    void *input;
    UINT32 output_bytes;
    void *output;
};
struct xapu_result
{
    struct xapu_command id;
    UINT32 state;
    UINT64 state_data, time;
    HRESULT error;
    UINT32 frames, output_bytes;
    void *output;
    float pitch;
    UINT32 input_bytes;
    void *input;
};
struct xapu_connect_input { UINT32 process, streams, commands_per_stream, data_bytes, frames_per_output, options; };
struct xapu_connect_output { void *data; HANDLE ready; };
#pragma pack(pop)

#define STREAMS 256

#define RESULTS 4096

struct xapu
{
    SRWLOCK lock;
    struct xapu_result results[RESULTS];
    UINT head, count;
    void *data;
    HANDLE ready;
    UINT32 frames_per_output, streams, options;
    struct xapu_stream
    {
        UINT32 channels; /* 1 or 2 */
#ifdef HAVE_FFMPEG_OPUS
        AVCodecContext *decoder;
#endif
    } stream[];
};

#ifdef HAVE_FFMPEG_OPUS
static void stream_close(struct xapu_stream *st)
{
    avcodec_free_context(&st->decoder);
}

static void stream_open(struct xapu_stream *st, UINT32 channels)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);

    stream_close(st);
    if (!codec || !(st->decoder = avcodec_alloc_context3(codec)))
        return;
    st->decoder->sample_rate = 48000;
    av_channel_layout_default(&st->decoder->ch_layout, (int)channels);
    if (avcodec_open2(st->decoder, codec, NULL) < 0)
    {
        rebox_log("xapu: FFmpeg's Opus decoder didn't open (%u channels)", channels);
        avcodec_free_context(&st->decoder);
    }
}

/* One packet decoded into pcm (interleaved floats, up to MAX_FRAMES frames): the frames, or -1. */
static int decode_packet(struct xapu_stream *st, const BYTE *input, UINT32 size, float *pcm)
{
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int frames = -1;

    if (packet && frame && av_new_packet(packet, (int)size) >= 0)
    {
        memcpy(packet->data, input, size);
        if (avcodec_send_packet(st->decoder, packet) >= 0)
        {
            frames = 0;
            while (avcodec_receive_frame(st->decoder, frame) >= 0)
            {
                int n = min(frame->nb_samples, MAX_FRAMES - frames), c = (int)st->channels;
                int have = frame->ch_layout.nb_channels;
                for (int i = 0; i < n; i++)
                    for (int ch = 0; ch < c; ch++)
                    {
                        int from = min(ch, have - 1);
                        float v = frame->format == AV_SAMPLE_FMT_FLTP ? ((const float *)frame->extended_data[from])[i]
                                : frame->format == AV_SAMPLE_FMT_FLT ? ((const float *)frame->data[0])[i * have + from]
                                : 0.0f;
                        pcm[(frames + i) * c + ch] = v;
                    }
                frames += n;
                av_frame_unref(frame);
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
    return frames;
}
#endif

HRESULT WINAPI XApuConnect(const struct xapu_connect_input *in, struct xapu_connect_output *out, HANDLE *handle)
{
    struct xapu_connect_input i;
    struct xapu *x;
    UINT32 streams;

    if (!in || !out || !handle)
        return E_INVALIDARG;
    i = *in;
    streams = i.streams ? i.streams : 1;
    if (!(x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*x) + streams * sizeof(x->stream[0]))))
        return E_OUTOFMEMORY;
    InitializeSRWLock(&x->lock);
    x->streams = streams;
    x->options = i.options;
    for (UINT32 s = 0; s < streams; s++)
        x->stream[s].channels = 2;
    x->frames_per_output = i.frames_per_output;
    if (i.data_bytes)
        x->data = VirtualAlloc(NULL, i.data_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (i.options & 8)
        x->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    out->data = x->data;
    out->ready = x->ready;
    *handle = x;
#ifdef HAVE_FFMPEG_OPUS
    rebox_log("xapu: decoder connected (process %u, %u streams, %u commands each, options %#x): Opus decoded by FFmpeg",
              i.process, i.streams, i.commands_per_stream, i.options);
#else
    rebox_log("xapu: decoder connected (process %u, %u streams, %u commands each): it answers with silence (no FFmpeg)",
              i.process, i.streams, i.commands_per_stream);
#endif
    return S_OK;
}

HRESULT WINAPI XApuDisconnect(HANDLE handle)
{
    struct xapu *x = handle;
    if (!x)
        return E_INVALIDARG;
#ifdef HAVE_FFMPEG_OPUS
    for (UINT32 s = 0; s < x->streams; s++)
        stream_close(&x->stream[s]);
#endif
    if (x->data)
        VirtualFree(x->data, 0, MEM_RELEASE);
    if (x->ready)
        CloseHandle(x->ready);
    HeapFree(GetProcessHeap(), 0, x);
    return S_OK;
}

HRESULT WINAPI XApuEnqueueCommand(HANDLE handle, const struct xapu_command *command, UINT32 *waiting)
{
    struct xapu *x = handle;
    struct xapu_result r;
    UINT32 s, n;

    if (!x || !command)
        return E_INVALIDARG;
    memset(&r, 0, sizeof(r));
    r.id = *command;
    r.pitch = 1.0f;
    s = command->stream < x->streams ? command->stream : 0;
    AcquireSRWLockExclusive(&x->lock);
    if (command->type == XAPU_ACTIVATE)
    {
        x->stream[s].channels = ((const struct xapu_activate *)command)->channels == 1 ? 1 : 2;
#ifdef HAVE_FFMPEG_OPUS
        stream_open(&x->stream[s], x->stream[s].channels);
#endif
    }
#ifdef HAVE_FFMPEG_OPUS
    else if (command->type == XAPU_DEACTIVATE)
        stream_close(&x->stream[s]);
    else if (command->type == XAPU_RESET && x->stream[s].decoder)
        avcodec_flush_buffers(x->stream[s].decoder);
#endif
    else if (command->type == XAPU_PROCESS)
    {
        struct xapu_process p = *(const struct xapu_process *)command;
        UINT32 channels = x->stream[s].channels, frame_bytes = channels * 4; /* float samples */
        UINT32 asked = p.frames ? p.frames : x->frames_per_output ? x->frames_per_output : 960;
        UINT32 frames = min(asked, p.output_bytes / frame_bytes);
        BOOL decoded = FALSE;
#ifdef HAVE_FFMPEG_OPUS
        static float pcm[MAX_FRAMES * 2]; /* under the connection's lock; one connection decodes at a time */
        static LONG shown, refused;
        int got = x->stream[s].decoder && p.input && p.input_bytes
                ? decode_packet(&x->stream[s], p.input, p.input_bytes, pcm) : -1;
        if (got >= 0 && p.output)
        {
            UINT32 first = min(p.first_frame, (UINT32)got);
            frames = min(min((UINT32)got - first, p.frames ? p.frames : (UINT32)got), p.output_bytes / frame_bytes);
            if (channels == 2 && (x->options & XAPU_DEINTERLEAVED))
                for (UINT32 f = 0; f < frames; f++)
                {
                    ((float *)p.output)[f] = pcm[(first + f) * 2];
                    ((float *)p.output)[frames + f] = pcm[(first + f) * 2 + 1];
                }
            else
                memcpy(p.output, pcm + first * channels, frames * frame_bytes);
            decoded = TRUE;
            if (InterlockedIncrement(&shown) <= 4)
                rebox_log("xapu: stream %u: an Opus packet of %u bytes decoded to %d frames (%u kept from %u, %u "
                          "channels)", s, p.input_bytes, got, frames, first, channels);
        }
        /* Not decoded: silence, with no error (one might make the title stop the stream). */
        else if (InterlockedIncrement(&refused) <= 8)
            rebox_log("xapu: stream %u: an Opus packet of %u bytes not decoded (%s); silence", s, p.input_bytes,
                      x->stream[s].decoder ? "FFmpeg refused it" : "no decoder");
#endif
        if (!decoded && p.output && frames)
            memset(p.output, 0, frames * frame_bytes);
        r.state = 1 | 2; /* output ready, more input wanted */
        r.frames = frames;
        r.output_bytes = frames * frame_bytes;
        r.output = p.output;
        r.pitch = p.pitch;
        r.input_bytes = p.input_bytes;
        r.input = p.input;
    }
    if (x->count < RESULTS)
        x->results[(x->head + x->count++) % RESULTS] = r;
    n = x->count;
    ReleaseSRWLockExclusive(&x->lock);
    if (waiting)
        *waiting = n;
    if (x->ready && (command->options & 1))
        SetEvent(x->ready);
    return S_OK;
}

HRESULT WINAPI XApuDequeueResult(HANDLE handle, struct xapu_result *result)
{
    struct xapu *x = handle;
    HRESULT hr = XAPU_NO_RESULTS;

    if (!x || !result)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&x->lock);
    if (x->count)
    {
        *result = x->results[x->head];
        x->head = (x->head + 1) % RESULTS;
        x->count--;
        hr = S_OK;
    }
    ReleaseSRWLockExclusive(&x->lock);
    return hr;
}

UINT32 WINAPI XApuGetResultCount(HANDLE handle, UINT32 *total)
{
    struct xapu *x = handle;
    UINT32 n;

    if (!x)
        return 0;
    AcquireSRWLockShared(&x->lock);
    n = x->count;
    ReleaseSRWLockShared(&x->lock);
    if (total)
        *total = n;
    return n;
}

/* Streams available and the most there can be. */
HRESULT WINAPI XApuGetUtilization(UINT32 a, UINT32 b, UINT32 *utilization)
{
    if (!utilization)
        return E_INVALIDARG;
    utilization[0] = utilization[1] = STREAMS;
    return S_OK;
}
