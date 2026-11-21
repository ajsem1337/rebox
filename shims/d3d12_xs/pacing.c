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
    return S_OK;
}
