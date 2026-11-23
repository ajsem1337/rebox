/* xapu.dll: the console's audio decoder queue (XMA and the like, run on the audio coprocessor).
 *
 * Nothing is decoded: each command is done at once, and a Process command's output is silence
 * of the length asked for, so a title's audio engine (FMOD in a GDK title) starts and runs.
 * Layouts and behaviour follow xcl (ivall/xcl, GPL-2.0-only, crates/xcl/src/hle/acphal.rs):
 *   HRESULT XApuConnect(const XAPU_CONNECT_INPUT *, XAPU_CONNECT_OUTPUT *, HANDLE *)
 *   HRESULT XApuDisconnect(HANDLE)
 *   HRESULT XApuEnqueueCommand(HANDLE, const XAPU_COMMAND *, UINT32 *results_waiting)
 *   HRESULT XApuDequeueResult(HANDLE, XAPU_RESULT *)   0x8AC81001 when there is none
 *   UINT32 XApuGetResultCount(HANDLE, UINT32 *)
 *   HRESULT XApuGetUtilization(UINT32, UINT32, UINT32 *)
 * All the structs are packed. */

#include <windows.h>

#include "../common/rebox_log.h"

#define XAPU_NO_RESULTS ((HRESULT)0x8AC81001)
enum { XAPU_ACTIVATE = 1, XAPU_PROCESS = 3 };

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
    UINT32 frames_per_output, streams;
    UINT32 channels[]; /* per stream: 1 or 2 */
};

HRESULT WINAPI XApuConnect(const struct xapu_connect_input *in, struct xapu_connect_output *out, HANDLE *handle)
{
    struct xapu_connect_input i;
    struct xapu *x;
    UINT32 streams;

    if (!in || !out || !handle)
        return E_INVALIDARG;
    i = *in;
    streams = i.streams ? i.streams : 1;
    if (!(x = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*x) + streams * sizeof(UINT32))))
        return E_OUTOFMEMORY;
    InitializeSRWLock(&x->lock);
    x->streams = streams;
    for (UINT32 s = 0; s < streams; s++)
        x->channels[s] = 2;
    x->frames_per_output = i.frames_per_output;
    if (i.data_bytes)
        x->data = VirtualAlloc(NULL, i.data_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (i.options & 8)
        x->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    out->data = x->data;
    out->ready = x->ready;
    *handle = x;
    rebox_log("xapu: decoder connected (process %u, %u streams, %u commands each): it answers with silence",
                 i.process, i.streams, i.commands_per_stream);
    return S_OK;
}

HRESULT WINAPI XApuDisconnect(HANDLE handle)
{
    struct xapu *x = handle;
    if (!x)
        return E_INVALIDARG;
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
        x->channels[s] = ((const struct xapu_activate *)command)->channels == 1 ? 1 : 2;
    else if (command->type == XAPU_PROCESS)
    {
        struct xapu_process p = *(const struct xapu_process *)command;
        UINT32 frame_bytes = x->channels[s] * 4; /* float samples */
        UINT32 asked = p.frames ? p.frames : x->frames_per_output ? x->frames_per_output : 960;
        UINT32 frames = min(asked, p.output_bytes / frame_bytes);
        if (p.output && frames)
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
