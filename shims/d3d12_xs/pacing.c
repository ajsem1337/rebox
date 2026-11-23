/* Frame pacing: device slots 78-80 (SetFrameIntervalX, ScheduleFrameEventX,
 * WaitFrameEventX).
 *
 * On the console the display hardware raises an origin event once per frame
 * period (interval x period-in-intervals) and titles start each frame by
 * waiting for it. A retail title was traced calling SetFrameIntervalX with
 * 16667 us and period 2 (30 fps), then WaitFrameEventX(ORIGIN, INFINITE) per
 * frame (ScarlettX). Without a clock such a title runs as fast as PresentX
 * lets it.
 *
 * Here a QPC clock stands in for the hardware. Frame k's event of a given
 * type is due at epoch + k * period + that type's scheduled offset. Each type
 * behaves like an auto-reset event: a wait returns at once if an occurrence
 * has passed since the last wait returned (the newest one, so a late title
 * doesn't burst through missed frames), otherwise it sleeps until the next.
 * Win32 events passed to ScheduleFrameEventX are set by a thread at each
 * occurrence; the flip's (type 2) only when a frame was presented since.
 *
 * Tokens and statistics follow what XValkyrie found a UE5 title to need (its
 * present.cpp, facts only): WaitFrameEventX gives consecutive tokens, one per
 * call, and a PresentX names its frame's token in the first plane's first
 * field. GetFrameStatisticsX answers from a record per token: when the wait
 * happened, and, once that frame has been presented, when its PresentX ran,
 * which stands for rendering, presenting and the display. A token that was
 * never given, or a frame not presented yet, has nothing beyond that to say,
 * as on the console. Our earlier answers came from the clock alone, with
 * the frame number as the token; a UE5 title then stopped after a dozen frames,
 * still asking about its first.
 *
 * REBOX_FRAME_PACING=0 makes the calls no-ops again. */

#include <string.h>
#include <windows.h>

#include "../common/rebox_log.h"
#include "pacing.h"

#define MAX_TYPES 8
#define DEFAULT_INTERVAL_US 16667

static SRWLOCK lock = SRWLOCK_INIT;
static LONGLONG qpc_freq, epoch, period_ticks;
static struct
{
    LONGLONG offset_ticks;
    LONGLONG last;   /* frame number the last wait returned, -1 for none */
    /* Signalled at each occurrence: handles read through the title's pointer each time (it
     * points at the title's own HANDLE fields, which it may replace), or one given directly. */
    HANDLE *const volatile *handles;
    UINT count;
    HANDLE event;
    BOOL enabled;
    LONGLONG next_signal;
} types[MAX_TYPES];
static HANDLE signal_thread, wake;
static volatile LONG presented_since_flip;

#define FLIP_TYPE 2

/* What became of each frame, by its token (see the top). */
static struct record
{
    UINT64 token;
    LONGLONG wait_call, wait_return, present_call, present_return;
    BOOL presented;
} records[256];
static SRWLOCK records_lock = SRWLOCK_INIT;

/* Caller holds records_lock exclusively. */
static struct record *record_of(UINT64 token)
{
    struct record *r = &records[token % ARRAYSIZE(records)];
    if (r->token != token)
        *r = (struct record){ token };
    return r;
}
static UINT interval_us_set = DEFAULT_INTERVAL_US;
static BOOL disabled;

static LONGLONG now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static LONGLONG us_to_ticks(LONGLONG us)
{
    return us * qpc_freq / 1000000;
}

static BOOL CALLBACK init(INIT_ONCE *once, void *param, void **ctx)
{
    LARGE_INTEGER f;
    char buf[8];

    QueryPerformanceFrequency(&f);
    qpc_freq = f.QuadPart;
    period_ticks = us_to_ticks(DEFAULT_INTERVAL_US);
    epoch = now();
    for (int i = 0; i < MAX_TYPES; i++)
        types[i].last = -1;
    disabled = GetEnvironmentVariableA("REBOX_FRAME_PACING", buf, sizeof(buf)) && buf[0] == '0';
    wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    return TRUE;
}

static BOOL ready(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    return !disabled;
}

/* Newest frame whose occurrence of this type is at or before t, or -1. */
static LONGLONG frame_at(UINT type, LONGLONG t)
{
    LONGLONG since = t - epoch - types[type].offset_ticks;
    return since < 0 ? -1 : since / period_ticks;
}

static LONGLONG due(UINT type, LONGLONG frame)
{
    return epoch + frame * period_ticks + types[type].offset_ticks;
}

/* Sleeps until a QPC time with a high-resolution waitable timer where
 * available; Sleep's granularity would cost several ms a frame. */
static void sleep_until(LONGLONG t)
{
    LONGLONG left = t - now();
    LARGE_INTEGER rel;
    HANDLE timer;

    if (left <= 0)
        return;
    if (!(timer = CreateWaitableTimerExW(NULL, NULL, 2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS)))
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    rel.QuadPart = -(left * 10000000 / qpc_freq);
    if (timer && rel.QuadPart && SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    if (timer)
        CloseHandle(timer);
    while (now() < t)
        SwitchToThread();
}

static DWORD WINAPI signal_proc(void *arg)
{
    for (;;)
    {
        LONGLONG next = MAXLONGLONG, t = now(), wait_ms;

        AcquireSRWLockExclusive(&lock);
        for (UINT i = 0; i < MAX_TYPES; i++)
        {
            if (!types[i].enabled)
                continue;
            if (types[i].next_signal <= t)
            {
                if (i != FLIP_TYPE || InterlockedExchange(&presented_since_flip, 0))
                {
                    if (types[i].event)
                        SetEvent(types[i].event);
                    for (UINT k = 0; k < types[i].count; k++)
                    {
                        HANDLE h = (*types[i].handles)[k];
                        if (h && !SetEvent(h))
                            ReleaseSemaphore(h, 1, NULL);
                    }
                }
                types[i].next_signal = due(i, frame_at(i, t) + 1);
            }
            if (types[i].next_signal < next)
                next = types[i].next_signal;
        }
        ReleaseSRWLockExclusive(&lock);

        wait_ms = next == MAXLONGLONG ? INFINITE : (next - now()) * 1000 / qpc_freq;
        if (wait_ms < 0)
            wait_ms = 0;
        WaitForSingleObject(wake, (DWORD)wait_ms);
    }
    return 0;
}

HRESULT pacing_set_interval(UINT interval_us, UINT period)
{
    if (!ready())
    {
        rebox_log("d3d12_xs: SetFrameIntervalX(%u us, period %u) ignored (REBOX_FRAME_PACING=0)",
                     interval_us, period);
        return S_OK;
    }
    if (!interval_us)
        interval_us = DEFAULT_INTERVAL_US;
    if (!period)
        period = 1;
    rebox_log("d3d12_xs: SetFrameIntervalX(%u us, period %u): origin every %.2f ms", interval_us, period,
                 interval_us * period / 1000.0);
    AcquireSRWLockExclusive(&lock);
    interval_us_set = interval_us * period; /* the frame length, with 1 at +4, as XValkyrie reports it */
    /* Restart the clock so frame numbers stay monotonic from here. */
    period_ticks = us_to_ticks((LONGLONG)interval_us * period);
    epoch = now();
    for (UINT i = 0; i < MAX_TYPES; i++)
    {
        types[i].last = -1;
        types[i].next_signal = due(i, 0);
    }
    ReleaseSRWLockExclusive(&lock);
    if (wake)
        SetEvent(wake);
    return S_OK;
}

/* Device slot 79, ScheduleFrameEventX(type, offset_us, list, flags). The list is
 * { UINT32 count; HANDLE *handles } (TomBox, from a UE5 title's
 * XboxCommonGDKUpdateFrameEvents: count 1, the address of the RHI's flip
 * event field that RHIWaitForFlip waits on; XValkyrie reads it the same way):
 * the handles are read through it at each occurrence. A null list moves only
 * the offset; flag bit 0 turns signalling off. A value too small to be a
 * pointer is taken as a HANDLE itself. Until this read the list, a UE5 title's
 * flip event was never set and its game thread stopped in frame-end sync. */
HRESULT pacing_schedule(UINT type, UINT_PTR offset_us, const void *list, UINT flags)
{
    if (!ready())
        return S_OK;
    if (type >= MAX_TYPES)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    types[type].offset_ticks = us_to_ticks((LONGLONG)offset_us) % period_ticks;
    if ((ULONG_PTR)list >= 0x10000)
    {
        const struct { UINT32 count; UINT32 pad; HANDLE *handles; } *l = list;
        types[type].count = l->handles ? min(l->count, 8) : 0;
        types[type].handles = types[type].count ? (HANDLE *const volatile *)&l->handles : NULL;
        types[type].event = NULL;
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 4)
                rebox_log("d3d12_xs: ScheduleFrameEventX(type %u, %llu us): %u handles read through %p, flags %#x",
                             type, (unsigned long long)offset_us, l->count, (void *)l->handles, flags);
        }
    }
    else if (list)
    {
        types[type].event = (HANDLE)list;
        types[type].count = 0;
    }
    types[type].enabled = !(flags & 1) && (types[type].event || types[type].count);
    types[type].next_signal = due(type, frame_at(type, now()) + 1);
    if (types[type].enabled && !signal_thread)
        signal_thread = CreateThread(NULL, 0, signal_proc, NULL, 0, NULL);
    ReleaseSRWLockExclusive(&lock);
    if (wake)
        SetEvent(wake);
    return S_OK;
}

/* For pacing_report: what the title last asked the clock. */
static volatile LONG64 last_wait_token, last_query_token, queries, unanswered;
static volatile LONG last_query_types, last_query_answered;

HRESULT pacing_wait(UINT type, UINT timeout_ms, UINT64 *token)
{
    static LONG64 counter;
    LONGLONG start, frame, t;
    UINT64 given;

    if (!ready())
    {
        if (token)
            *token = InterlockedIncrement64(&counter);
        return S_OK;
    }
    if (type >= MAX_TYPES)
        type = 0;
    start = now();

    AcquireSRWLockExclusive(&lock);
    frame = frame_at(type, start);
    if (frame <= types[type].last)
        frame = types[type].last + 1;
    t = due(type, frame);
    if (timeout_ms != INFINITE && t > start + us_to_ticks((LONGLONG)timeout_ms * 1000))
    {
        ReleaseSRWLockExclusive(&lock);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    types[type].last = frame;
    ReleaseSRWLockExclusive(&lock);

    sleep_until(t);
    given = InterlockedIncrement64(&counter);
    AcquireSRWLockExclusive(&records_lock);
    {
        struct record *r = record_of(given);
        r->wait_call = start;
        r->wait_return = now();
    }
    ReleaseSRWLockExclusive(&records_lock);
    if (token)
        *token = given;
    InterlockedExchange64(&last_wait_token, given);
    return S_OK;
}

/* A PresentX for the frame of this token ran from call to ret (QPC). */
void pacing_presented(UINT64 token, LONGLONG call, LONGLONG ret)
{
    if (!token)
        return;
    AcquireSRWLockExclusive(&records_lock);
    {
        struct record *r = record_of(token);
        r->present_call = call;
        r->present_return = ret;
        r->presented = TRUE;
    }
    ReleaseSRWLockExclusive(&records_lock);
    InterlockedExchange(&presented_since_flip, 1);
}

LONGLONG pacing_now(void)
{
    return now();
}

/* Device slot 81, GetFrameStatisticsX(token, types, count, statistics) (XValkyrie: an Unreal
 * title waits for these per frame token and stops after a few frames without them). Each entry
 * is D3D12XBOX_FRAME_STATISTICS, 56 bytes: a type, padding, and 48 bytes for that type, one per
 * type asked for that has something to report. The numbers come from the frame clock: frame k's
 * origin at its due time, and rendering, presenting and the display taken to finish by the next
 * origin, so those three are reported only once that has passed. Offsets into the 48 data bytes
 * (which start at +8), as XValkyrie fills them:
 *   0x1      interval: +0 the frame length (interval x period) in us, +4 1
 *   0x2      frame event: +8 waited, +16 and +24 came (QPC)
 *   0x100    rendering: +8 done, +16 how long
 *   0x1000   presenting: +1 planes, +2 views, +8/+16 started, +24 done, +32 how long, +42 1
 *   0x10000  display: +8 vertical blank, +16 flip */
HRESULT pacing_frame_statistics(UINT64 token, UINT types, UINT *count, void *statistics)
{
    static const UINT kinds[] = { 0x1, 0x2, 0x100, 0x1000, 0x10000 };
    BYTE *out = statistics;
    struct record r;
    UINT room;

    if (!count)
        return E_POINTER;
    room = statistics ? *count : 0;
    *count = 0;
    if (!ready() || !token)
        return S_OK;
    AcquireSRWLockShared(&records_lock);
    r = records[token % ARRAYSIZE(records)];
    ReleaseSRWLockShared(&records_lock);
    for (UINT i = 0; i < ARRAYSIZE(kinds) && *count < room && r.token == token; i++)
    {
        BYTE *e = out + *count * 56;
        LONGLONG v;
        UINT32 u;
        UINT16 one = 1;

        if (!(types & kinds[i]) || (kinds[i] >= 0x100 && !r.presented))
            continue;
        memset(e, 0, 56);
        memcpy(e, &kinds[i], 4);
        switch (kinds[i])
        {
        case 0x1:
            u = interval_us_set;
            memcpy(e + 8, &u, 4);
            u = 1;
            memcpy(e + 12, &u, 4);
            break;
        case 0x2:
            memcpy(e + 16, &r.wait_call, 8);
            memcpy(e + 24, &r.wait_return, 8);
            memcpy(e + 32, &r.wait_return, 8);
            break;
        case 0x100:
            v = r.present_return - r.present_call;
            memcpy(e + 16, &r.present_return, 8);
            memcpy(e + 24, &v, 8);
            break;
        case 0x1000:
            e[9] = 1;
            e[10] = 1;
            v = r.present_return - r.present_call;
            memcpy(e + 16, &r.present_call, 8);
            memcpy(e + 24, &r.present_call, 8);
            memcpy(e + 32, &r.present_return, 8);
            memcpy(e + 40, &v, 8);
            memcpy(e + 50, &one, 2);
            break;
        default:
            memcpy(e + 16, &r.present_return, 8);
            memcpy(e + 24, &r.present_return, 8);
            break;
        }
        (*count)++;
    }
    InterlockedIncrement64(&queries);
    InterlockedExchange64(&last_query_token, (LONG64)token);
    InterlockedExchange(&last_query_types, (LONG)types);
    InterlockedExchange(&last_query_answered, (LONG)*count);
    if (!*count)
        InterlockedIncrement64(&unanswered);
    return S_OK;
}

void pacing_report(void)
{
    LONGLONG frame = -1;

    if (ready())
    {
        AcquireSRWLockShared(&lock);
        frame = frame_at(0, now());
        ReleaseSRWLockShared(&lock);
    }
    rebox_log("d3d12_xs: frame clock at frame %lld; last WaitFrameEventX token %lld; %lld GetFrameStatisticsX "
                 "calls, %lld with nothing to report, the last for token %lld types %#lx: %ld entries",
                 (long long)frame, (long long)last_wait_token, (long long)queries, (long long)unanswered,
                 (long long)last_query_token, last_query_types, last_query_answered);
}
