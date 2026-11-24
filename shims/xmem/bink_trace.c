/* REBOX_BINK_TRACE=1: what a title asks of the package's Bink 2 (a DLL named Bink2*.dll, RAD's
 * video codec), and what each open movie is doing, in the log.
 *
 * The title's imports of the calls below (and its GetProcAddress of them) go through wrappers here
 * (import_fixup.c's replacements), which pass every call through unchanged. Opens, closes, pauses,
 * seeks, frame buffer registrations and the sound and file setup are logged as they happen; the
 * per-frame calls (BinkWait, BinkShouldSkip, BinkNextFrame, BinkDoFrame and the async decode) are
 * counted, and the first few of each per movie logged. Every 5 seconds each open movie gets a line
 * with those counts since the last one and its state: the frame it is on, whether it is paused,
 * whether its next frame's data is still being read, its read error and its sound tracks. A movie
 * the title has stopped driving shows as counts of 0.
 *
 * The public head of a movie handle (BINK: width, height, frames, frame number, last frame number,
 * frame rate and divisor, read error, open flags) is the SDK's. Three internal fields are this
 * DLL build's, read from its BinkWait and its I/O thread: +0x40 its sound has started, +0x48 the
 * next frame's data is still to be read (the I/O thread clears it), +0x11c paused (BinkPause), and
 * +0x12c the number of sound tracks it opened. BinkWait answers "wait" while +0x48 is set, while a
 * movie with sound tracks hasn't started its sound, or until the frame's time comes. */

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "bink_trace.h"

enum
{
    B_OPEN, B_CLOSE, B_WAIT, B_SKIP, B_NEXT, B_DO, B_ASYNC, B_ASYNC_WAIT, B_PAUSE, B_GOTO, B_REGISTER,
    B_INFO, B_LOOP, B_SOUND, B_FILES, B_COUNT
};

static const char *const names[B_COUNT] = {
    "BinkOpenWithOptions", "BinkClose", "BinkWait", "BinkShouldSkip", "BinkNextFrame", "BinkDoFrame",
    "BinkDoFrameAsyncMulti", "BinkDoFrameAsyncWait", "BinkPause", "BinkGoto", "BinkRegisterFrameBuffers",
    "BinkGetFrameBuffersInfo", "BinkSetWillLoop", "BinkSetSoundSystem", "BinkSetOSFileCallbacks",
};

/* Every call here has at most four integer or pointer arguments, in rcx, rdx, r8 and r9. */
typedef UINT_PTR (*bink_fn)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);

static void *real[B_COUNT];
static HMODULE bink_module;
static char *(*get_error)(void);
static int enabled = -1;

#define FROM_MEMORY 0x04000000u /* BINKFROMMEMORY: the name is the movie's bytes */
#define HANDLE_BYTES 0x700      /* what the fields read here span */

struct movie
{
    UINT_PTR handle;
    char name[96];
    ULONGLONG opened_at, wait_busy_since;
    LONG calls[B_COUNT], reported[B_COUNT], logged[B_COUNT];
    LONG wait_busy, wait_busy_reported, wait_last;
    char last_state[160];
    ULONGLONG last_report;
};

static struct movie movies[16];
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE reporter;

struct fields
{
    UINT32 width, height, frames, frame_num, last_frame_num, rate, rate_div, read_error, open_flags;
    UINT32 sound_started, frame_pending, paused, tracks;
};

static BOOL read_fields(UINT_PTR handle, struct fields *f)
{
    const BYTE *h = (const BYTE *)handle;
    if (!h || IsBadReadPtr(h, HANDLE_BYTES))
        return FALSE;
    memcpy(f, h, 9 * sizeof(UINT32));
    f->sound_started = *(const UINT32 *)(h + 0x40);
    f->frame_pending = *(const UINT32 *)(h + 0x48);
    f->paused = *(const UINT32 *)(h + 0x11c);
    f->tracks = *(const UINT32 *)(h + 0x12c);
    return TRUE;
}

/* "module+0x1234" for a return address. */
static void caller_text(void *address, char *out, size_t size)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           address, &m) && GetModuleFileNameA(m, path, sizeof(path)))
    {
        const char *file = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        snprintf(out, size, "%s+%#llx", file, (unsigned long long)((const BYTE *)address - (const BYTE *)m));
    }
    else
        snprintf(out, size, "%p", address);
}

/* Under the lock (shared is enough to read; slots change only exclusively). */
static struct movie *find(UINT_PTR handle)
{
    for (UINT i = 0; i < ARRAYSIZE(movies); i++)
        if (movies[i].handle && movies[i].handle == handle)
            return &movies[i];
    return NULL;
}

static void state_text(UINT_PTR handle, char *out, size_t size)
{
    struct fields f;
    if (!read_fields(handle, &f))
        snprintf(out, size, "(unreadable)");
    else
        snprintf(out, size, "frame %u (last %u) of %u, %spaused, %s, read error %u, %u sound tracks%s", f.frame_num,
                 f.last_frame_num, f.frames, f.paused ? "" : "not ", f.frame_pending ? "frame data pending" : "data read",
                 f.read_error, f.tracks, f.tracks ? (f.sound_started ? " (started)" : " (not started)") : "");
}

/* The 5-second line: counts since the last one, and the movie's state. Logged when either changed,
 * and every 30 seconds anyway. Under the lock. */
static void report(struct movie *m, ULONGLONG now, BOOL force)
{
    static const int counted[] = { B_WAIT, B_SKIP, B_NEXT, B_DO, B_ASYNC, B_ASYNC_WAIT, B_PAUSE, B_GOTO };
    LONG n[ARRAYSIZE(counted)], busy = m->wait_busy;
    char counts[320], state[160];
    int at = 0;
    BOOL any = FALSE;

    for (UINT i = 0; i < ARRAYSIZE(counted); i++)
        any |= (n[i] = m->calls[counted[i]]) != m->reported[counted[i]];
    state_text(m->handle, state, sizeof(state));
    if (!force && !any && !strcmp(state, m->last_state) && now - m->last_report < 30000)
        return;
    for (UINT i = 0; i < ARRAYSIZE(counted); i++)
    {
        at += snprintf(counts + at, sizeof(counts) - at, "%s%s %ld", i ? ", " : "", names[counted[i]] + 4,
                       n[i] - m->reported[counted[i]]);
        m->reported[counted[i]] = n[i];
    }
    at += snprintf(counts + at, sizeof(counts) - at, " (Wait said wait %ld times)", busy - m->wait_busy_reported);
    m->wait_busy_reported = busy;
    if (m->wait_last)
        snprintf(counts + at, sizeof(counts) - at, "; BinkWait has said wait for %llu ms", now - m->wait_busy_since);
    rebox_log("bink %#llx (%s), open %llu s: %s; calls in the last %llu s: %s", (unsigned long long)m->handle,
              m->name, (now - m->opened_at) / 1000, state, (now - m->last_report + 500) / 1000, counts);
    strcpy(m->last_state, state);
    m->last_report = now;
}

static DWORD WINAPI report_thread(void *unused)
{
    for (;;)
    {
        ULONGLONG now;
        Sleep(5000);
        now = GetTickCount64();
        AcquireSRWLockExclusive(&lock);
        for (UINT i = 0; i < ARRAYSIZE(movies); i++)
            if (movies[i].handle)
                report(&movies[i], now, FALSE);
        ReleaseSRWLockExclusive(&lock);
    }
    return 0;
}

static UINT_PTR call(int i, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d)
{
    return ((bink_fn)real[i])(a, b, c, d);
}

/* A per-frame call: counted, the first 3 of each per movie logged. */
static void count(int i, UINT_PTR handle, UINT_PTR a, UINT_PTR result)
{
    struct movie *m;
    LONG n;
    char state[160];

    AcquireSRWLockShared(&lock);
    if (!(m = find(handle)))
    {
        ReleaseSRWLockShared(&lock);
        return;
    }
    InterlockedIncrement(&m->calls[i]);
    if (i == B_WAIT)
    {
        LONG busy = result != 0;
        if (busy)
            InterlockedIncrement(&m->wait_busy);
        if (InterlockedExchange(&m->wait_last, busy) != busy)
        {
            if (busy)
                m->wait_busy_since = GetTickCount64();
            if (InterlockedIncrement(&m->logged[B_WAIT]) <= 16)
            {
                state_text(handle, state, sizeof(state));
                rebox_log("bink %#llx: BinkWait now says %s (%s)", (unsigned long long)handle, busy ? "wait" : "go",
                          state);
            }
        }
    }
    else if ((n = InterlockedIncrement(&m->logged[i])) <= 3)
    {
        char returned[32] = "";
        if (i != B_NEXT) /* returns nothing */
            snprintf(returned, sizeof(returned), " = %#llx", (unsigned long long)result);
        state_text(handle, state, sizeof(state));
        rebox_log("bink %#llx: %s(%#llx)%s (call %ld; %s)", (unsigned long long)handle, names[i],
                  (unsigned long long)a, returned, n, state);
    }
    ReleaseSRWLockShared(&lock);
}

static UINT_PTR open_wrapper(UINT_PTR name, UINT_PTR options, UINT_PTR flags, UINT_PTR d)
{
    void *from = __builtin_return_address(0);
    ULONGLONG t0 = GetTickCount64(), t1;
    UINT_PTR handle = call(B_OPEN, name, options, flags, d);
    char what[96], where[MAX_PATH + 24], state[160];
    struct fields f;

    t1 = GetTickCount64();
    caller_text(from, where, sizeof(where));
    if ((UINT32)flags & FROM_MEMORY)
        snprintf(what, sizeof(what), "memory at %#llx", (unsigned long long)name);
    else if (name && !IsBadStringPtrA((const char *)name, sizeof(what) - 3))
        snprintf(what, sizeof(what), "\"%.*s\"", (int)sizeof(what) - 3, (const char *)name);
    else
        snprintf(what, sizeof(what), "%#llx", (unsigned long long)name);
    if (!handle)
    {
        const char *error = get_error ? get_error() : NULL;
        rebox_log("bink: BinkOpenWithOptions(%s, flags %#x) from %s failed in %llu ms: %s", what, (UINT32)flags, where,
                  t1 - t0, error && !IsBadStringPtrA(error, 256) ? error : "(no error text)");
        return handle;
    }
    if (read_fields(handle, &f))
        rebox_log("bink: BinkOpenWithOptions(%s, flags %#x) from %s = %#llx in %llu ms: %ux%u, %u frames at %u/%u, "
                  "open flags %#x, %u sound tracks", what, (UINT32)flags, where, (unsigned long long)handle, t1 - t0,
                  f.width, f.height, f.frames, f.rate, f.rate_div, f.open_flags, f.tracks);
    AcquireSRWLockExclusive(&lock);
    for (UINT i = 0; i < ARRAYSIZE(movies); i++)
        if (!movies[i].handle)
        {
            struct movie *m = &movies[i];
            memset(m, 0, sizeof(*m));
            m->handle = handle;
            snprintf(m->name, sizeof(m->name), "%s", what);
            m->opened_at = m->last_report = t1;
            state_text(handle, state, sizeof(state));
            strcpy(m->last_state, state);
            break;
        }
    if (!reporter)
        reporter = CreateThread(NULL, 0, report_thread, NULL, 0, NULL);
    ReleaseSRWLockExclusive(&lock);
    return handle;
}

static UINT_PTR close_wrapper(UINT_PTR handle, UINT_PTR b, UINT_PTR c, UINT_PTR d)
{
    struct movie *m;

    AcquireSRWLockExclusive(&lock);
    if ((m = find(handle)))
    {
        report(m, GetTickCount64(), TRUE);
        rebox_log("bink %#llx: BinkClose after %llu ms open", (unsigned long long)handle,
                  GetTickCount64() - m->opened_at);
        m->handle = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    return call(B_CLOSE, handle, b, c, d);
}

/* Calls logged each time (up to a limit): with their arguments, result, caller and the movie's state
 * after. */
static UINT_PTR logged_call(int i, UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d, void *from)
{
    static LONG logged[B_COUNT];
    UINT_PTR result = call(i, a, b, c, d);
    BOOL handle_first = i != B_SOUND && i != B_FILES;
    char where[MAX_PATH + 24], state[160] = "";

    if (handle_first)
    {
        AcquireSRWLockShared(&lock);
        struct movie *m = find(a);
        if (m)
            InterlockedIncrement(&m->calls[i]);
        ReleaseSRWLockShared(&lock);
    }
    if (InterlockedIncrement(&logged[i]) > (i == B_PAUSE || i == B_GOTO ? 256 : 32))
        return result;
    caller_text(from, where, sizeof(where));
    if (handle_first)
    {
        /* BinkGoto, BinkRegisterFrameBuffers, BinkGetFrameBuffersInfo and BinkSetWillLoop return nothing. */
        BOOL none = i == B_GOTO || i == B_REGISTER || i == B_INFO || i == B_LOOP;
        char returned[32] = "";
        if (!none)
            snprintf(returned, sizeof(returned), " = %#llx", (unsigned long long)result);
        state_text(a, state, sizeof(state));
        rebox_log("bink %#llx: %s(%#llx, %#llx, %#llx) from %s%s (%s)", (unsigned long long)a, names[i],
                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, where, returned, state);
    }
    else
        rebox_log("bink: %s(%#llx, %#llx, %#llx, %#llx) from %s = %#llx", names[i], (unsigned long long)a,
                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, where,
                  (unsigned long long)result);
    return result;
}

#define COUNTED(id, fn) \
    static UINT_PTR fn(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) \
    { \
        UINT_PTR r = call(id, a, b, c, d); \
        count(id, a, b, r); \
        return r; \
    }
#define LOGGED(id, fn) \
    static UINT_PTR fn(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) \
    { \
        return logged_call(id, a, b, c, d, __builtin_return_address(0)); \
    }

COUNTED(B_WAIT, wait_wrapper)
COUNTED(B_SKIP, skip_wrapper)
COUNTED(B_NEXT, next_wrapper)
COUNTED(B_DO, do_wrapper)
COUNTED(B_ASYNC, async_wrapper)
COUNTED(B_ASYNC_WAIT, async_wait_wrapper)
LOGGED(B_PAUSE, pause_wrapper)
LOGGED(B_GOTO, goto_wrapper)
LOGGED(B_REGISTER, register_wrapper)
LOGGED(B_INFO, info_wrapper)
LOGGED(B_LOOP, loop_wrapper)
LOGGED(B_SOUND, sound_wrapper)
LOGGED(B_FILES, files_wrapper)

static void *const wrappers[B_COUNT] = {
    open_wrapper, close_wrapper, wait_wrapper, skip_wrapper, next_wrapper, do_wrapper, async_wrapper,
    async_wait_wrapper, pause_wrapper, goto_wrapper, register_wrapper, info_wrapper, loop_wrapper, sound_wrapper,
    files_wrapper,
};

BOOL bink_trace_enabled(void)
{
    if (enabled < 0)
    {
        char *v = rebox_getenv("REBOX_BINK_TRACE");
        enabled = v && *v && strcmp(v, "0");
        free(v);
    }
    return enabled;
}

void bink_trace_module(HMODULE module)
{
    char path[MAX_PATH];
    const char *file;

    if (!bink_trace_enabled() || bink_module || !module || !GetModuleFileNameA(module, path, sizeof(path)))
        return;
    file = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    if (_strnicmp(file, "bink2", 5))
        return;
    for (UINT i = 0; i < B_COUNT; i++)
        real[i] = (void *)GetProcAddress(module, names[i]);
    get_error = (void *)GetProcAddress(module, "BinkGetError");
    bink_module = module;
    rebox_log("bink: tracing %s's calls (REBOX_BINK_TRACE)", file);
}

void *bink_trace_replacement(void *target)
{
    if (!target || !bink_module)
        return NULL;
    for (UINT i = 0; i < B_COUNT; i++)
        if (real[i] == target)
            return wrappers[i];
    return NULL;
}
