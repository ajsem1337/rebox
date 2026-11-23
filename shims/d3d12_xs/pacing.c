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
 * The token is the frame number. Win32 events passed to ScheduleFrameEventX
 * are set by a thread at each occurrence.
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
    HANDLE event;    /* signalled at each occurrence, may be NULL */
    LONGLONG next_signal;
} types[MAX_TYPES];
static HANDLE signal_thread, wake;
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
            if (!types[i].event)
                continue;
            if (types[i].next_signal <= t)
            {
                SetEvent(types[i].event);
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

HRESULT pacing_schedule(UINT type, UINT_PTR offset_us, HANDLE event)
{
    if (!ready())
        return S_OK;
    if (type >= MAX_TYPES)
        return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock);
    types[type].offset_ticks = us_to_ticks((LONGLONG)offset_us) % period_ticks;
    types[type].event = event;
    types[type].next_signal = due(type, frame_at(type, now()) + 1);
    if (event && !signal_thread)
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
    if (token)
        *token = (UINT64)frame + 1;
    InterlockedExchange64(&last_wait_token, frame + 1);
    return S_OK;
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
    UINT room;
    LONGLONG origin, next, t = now();

    if (!count)
        return E_POINTER;
    room = statistics ? *count : 0;
    *count = 0;
    if (!ready() || !token)
        return S_OK;
    AcquireSRWLockShared(&lock);
    origin = due(0, (LONGLONG)token - 1);
    next = origin + period_ticks;
    for (UINT i = 0; i < ARRAYSIZE(kinds) && *count < room; i++)
    {
        BYTE *e = out + *count * 56;
        LONGLONG v;
        UINT32 u;
        UINT16 one = 1;

        if (!(types & kinds[i]) || (kinds[i] >= 0x100 && t < next) || (kinds[i] == 0x2 && t < origin))
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
            memcpy(e + 16, &origin, 8);
            memcpy(e + 24, &origin, 8);
            memcpy(e + 32, &origin, 8);
            break;
        case 0x100:
            v = next - origin;
            memcpy(e + 16, &next, 8);
            memcpy(e + 24, &v, 8);
            break;
        case 0x1000:
            e[9] = 1;
            e[10] = 1;
            v = 0;
            memcpy(e + 16, &next, 8);
            memcpy(e + 24, &next, 8);
            memcpy(e + 32, &next, 8);
            memcpy(e + 40, &v, 8);
            memcpy(e + 50, &one, 2);
            break;
        default:
            memcpy(e + 16, &next, 8);
            memcpy(e + 24, &next, 8);
            break;
        }
        (*count)++;
    }
    ReleaseSRWLockShared(&lock);
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
