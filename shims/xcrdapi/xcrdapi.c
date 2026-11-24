/* Replacement for the console's XCRDAPI.dll: DirectStorage (dstorage_xs.h).
 *
 * A title imports DStorageGetFactory and drives everything through COM-style objects: the
 * factory, queues, files and status arrays. Structure layouts are the public GDK reference's
 * (DSTORAGE_QUEUE_DESC, DSTORAGE_REQUEST); vtable slots are the ones ScarlettX saw a retail title
 * call. The queue's methods are dstorage_xs.h's (OpenScarlett's compat header, from Microsoft's
 * MIT samples): 3 EnqueueRequest, 4 EnqueueStatus, 5 EnqueueSetEvent (PC's EnqueueSignal sits
 * there; a fence is told from an event handle and signalled), 6 Submit, 7-11 CancelRequestsWithTag,
 * Close, GetErrorEvent, RetrieveErrorRecord, Query, then X1's EnqueueSignal (12) and X2's Query1,
 * SetAutoSubmitDisabled and GetAutoSubmitDisabled (13-15). Auto-submit is on unless the title turns
 * it off: everything enqueued runs without waiting for Submit, as on the console (a UE5 title never
 * calls Submit, and its streaming textures stayed pending until garbage collection gave up on
 * them). Other slots log their first calls and return 0.
 *
 * Requests run in order on one worker thread per queue, reading from host files or memory.
 * zlib-compressed requests are inflated (zlib-wrapped or raw deflate). BCPack requests go to
 * helpers/bcpack, a separate process running Leonida's GPLv3 decoder (rebox-run starts it and
 * sets REBOX_BCPACK_PORT), over a localhost connection per worker thread. A destination in physical
 * pages (options bit 21: an array of 64 KiB page numbers, the offset into the first at +0x10) is
 * decoded into a buffer and written through the pages (xmem ReboxWritePhysicalPages). Swizzled
 * destinations and physical-page sources aren't supported, nor is BCPack without the helper:
 * those requests fail with E_NOTIMPL and are logged, and the next status entry reports it. */

#define COBJMACROS
#include <winsock2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>

#include "../common/rebox_log.h"
#include "../d3d12_xs/puff.h"

#define OPT_ZLIB 0x1ull
#define OPT_BCPACK (0xfull << 1)
#define OPT_SWIZZLE (0xffffull << 5)
#define OPT_DEST_PHYSICAL (1ull << 21)
#define OPT_SOURCE_MEMORY (1ull << 22)
#define OPT_SOURCE_PHYSICAL (1ull << 23)

typedef struct
{
    UINT64 source_type; /* 0 file, 1 memory */
    UINT16 capacity;
    INT8 priority;
    const char *name;
} DSTORAGE_QUEUE_DESC;
C_ASSERT(sizeof(DSTORAGE_QUEUE_DESC) == 0x18);

typedef struct
{
    UINT64 options;
    void *destination;
    UINT64 destination_page_offset;
    UINT32 destination_size;
    UINT32 reserved;
    void *source; /* a file object, or memory */
    UINT64 file_offset;
    UINT32 source_size;
    UINT32 intermediate_size;
    UINT64 cancellation_tag;
    const char *name;
} DSTORAGE_REQUEST;
C_ASSERT(sizeof(DSTORAGE_REQUEST) == 0x48);

struct object
{
    void *const *vtbl;
    LONG refs;
    void (*destroy)(struct object *);
};

static HRESULT WINAPI object_QueryInterface(struct object *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    InterlockedIncrement(&self->refs);
    *out = self;
    return S_OK;
}

static ULONG WINAPI object_AddRef(struct object *self)
{
    return InterlockedIncrement(&self->refs);
}

static ULONG WINAPI object_Release(struct object *self)
{
    LONG left = InterlockedDecrement(&self->refs);
    if (!left && self->destroy)
        self->destroy(self);
    return left;
}

/* Slots no title has been seen calling. */
#define UNNAMED(iface, n) \
    static UINT_PTR WINAPI iface##_slot##n(struct object *self, UINT_PTR a, UINT_PTR b, UINT_PTR c) \
    { \
        static LONG calls; \
        if (InterlockedIncrement(&calls) <= 4) \
            rebox_log("xcrdapi: " #iface " slot " #n " not implemented (%#llx, %#llx, %#llx)", \
                         (unsigned long long)a, (unsigned long long)b, (unsigned long long)c); \
        return 0; \
    }

/* REBOX_SLOT_STATS (or REBOX_DSTORAGE_STATS=seconds): what was enqueued and what has run,
 * logged every few seconds while it changes or something is still waiting, to see how a title
 * waits for its reads (status entries, events or fences) and whether any never complete. */
enum { ST_REQUEST, ST_STATUS, ST_SIGNAL, ST_EVENT, ST_KINDS };
static struct
{
    volatile LONG64 enqueued[ST_KINDS], done[ST_KINDS], submitted, failed, polls, polls_pending;
} stats;

static const char *const stat_names[ST_KINDS] = { "requests", "status entries", "fence signals", "events" };

static DWORD WINAPI stats_thread(void *param)
{
    DWORD seconds = (DWORD)(ULONG_PTR)param;
    LONG64 last = -1;

    for (;;)
    {
        char line[512];
        int at = 0;
        LONG64 sum = 0, waiting = 0;

        Sleep(seconds * 1000);
        for (int k = 0; k < ST_KINDS; k++)
        {
            LONG64 e = stats.enqueued[k], d = stats.done[k];
            sum += e + d;
            waiting += e - d;
            at += snprintf(line + at, sizeof(line) - at, "%s%s %lld/%lld", k ? ", " : "", stat_names[k], d, e);
        }
        sum += stats.polls;
        if (sum == last && !waiting)
            continue;
        last = sum;
        rebox_log("xcrdapi: run/enqueued: %s; %lld failed, %lld still unsubmitted; status polls %lld (%lld "
                     "pending)", line, (long long)stats.failed,
                     (long long)(stats.enqueued[0] + stats.enqueued[1] + stats.enqueued[2] + stats.enqueued[3]
                                 - stats.submitted),
                     (long long)stats.polls, (long long)stats.polls_pending);
    }
    return 0;
}

static void start_stats(void)
{
    const char *env = getenv("REBOX_DSTORAGE_STATS");
    int seconds = env ? atoi(env) : getenv("REBOX_SLOT_STATS") ? 10 : 0;

    if (env && seconds <= 0)
        seconds = 10;
    if (seconds > 0)
        CloseHandle(CreateThread(NULL, 0, stats_thread, (void *)(ULONG_PTR)seconds, 0, NULL));
}

/* ---------------------------------------------------------------------- status arrays */

struct status_entry
{
    volatile LONG complete;
    volatile LONG hr;
};

struct status_array
{
    struct object obj;
    UINT32 capacity;
    struct status_entry *entries;
};

static void status_destroy(struct object *self)
{
    struct status_array *a = (struct status_array *)self;
    free(a->entries);
    free(a);
}

static BOOL WINAPI status_IsComplete(struct status_array *a, UINT32 index)
{
    BOOL complete = index < a->capacity && a->entries[index].complete;

    InterlockedIncrement64(&stats.polls);
    if (!complete)
    {
        static LONG logged;
        InterlockedIncrement64(&stats.polls_pending);
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("xcrdapi: status array %p entry %u of %u polled before it completed (%s)", (void *)a, index,
                      a->capacity, index < a->capacity ? "enqueued, not yet written" : "past its capacity");
    }
    return complete;
}

static HRESULT WINAPI status_GetHResult(struct status_array *a, UINT32 index)
{
    if (index >= a->capacity)
        return E_INVALIDARG;
    return a->entries[index].complete ? a->entries[index].hr : E_PENDING;
}

UNNAMED(status, 5) UNNAMED(status, 6) UNNAMED(status, 7)
static void *const status_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, status_IsComplete, status_GetHResult,
    status_slot5, status_slot6, status_slot7,
};

/* ------------------------------------------------------------------------------ files */

struct file
{
    struct object obj;
    HANDLE handle;
    SRWLOCK lock;
};

static void file_destroy(struct object *self)
{
    struct file *f = (struct file *)self;
    if (f->handle != INVALID_HANDLE_VALUE)
        CloseHandle(f->handle);
    free(f);
}

static void WINAPI file_Close(struct file *f)
{
    AcquireSRWLockExclusive(&f->lock);
    if (f->handle != INVALID_HANDLE_VALUE)
        CloseHandle(f->handle);
    f->handle = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&f->lock);
}

/* The title owns the returned handle. */
static HANDLE WINAPI file_GetHandle(struct file *f)
{
    HANDLE dup = INVALID_HANDLE_VALUE;
    AcquireSRWLockShared(&f->lock);
    if (f->handle != INVALID_HANDLE_VALUE)
        DuplicateHandle(GetCurrentProcess(), f->handle, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    ReleaseSRWLockShared(&f->lock);
    return dup;
}

UNNAMED(file, 5) UNNAMED(file, 6) UNNAMED(file, 7)
static void *const file_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, file_Close, file_GetHandle,
    file_slot5, file_slot6, file_slot7,
};

/* ----------------------------------------------------------------------------- queues */

enum entry_kind { ENTRY_REQUEST, ENTRY_STATUS, ENTRY_SIGNAL, ENTRY_EVENT };

struct entry
{
    enum entry_kind kind;
    DSTORAGE_REQUEST request;
    struct file *file;          /* held until the request has run */
    struct status_array *status; /* held until the status is written */
    UINT32 index;
    ID3D12Fence *fence;
    UINT64 value;
    HANDLE event;               /* ENTRY_EVENT: duplicated, closed once set */
    ULONG_PTR *pages;           /* a physical-page destination's page numbers, as at enqueue */
    UINT32 page_count;
};

struct queue
{
    struct object obj;
    UINT64 source_type;
    UINT32 capacity;
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE wake;
    struct entry *pending, *submitted;
    UINT32 pending_count, pending_size, submitted_count, submitted_size, submitted_head;
    HANDLE worker;
    BOOL stop;
    BOOL manual_submit;         /* SetAutoSubmitDisabled(TRUE) */
    HRESULT first_failure; /* since the last status entry; worker only */
};

static BOOL push(struct entry **list, UINT32 *count, UINT32 *size, const struct entry *e)
{
    if (*count == *size)
    {
        UINT32 grown = *size ? *size * 2 : 64;
        struct entry *n = realloc(*list, grown * sizeof(*n));
        if (!n)
            return FALSE;
        *list = n;
        *size = grown;
    }
    (*list)[(*count)++] = *e;
    return TRUE;
}

static HRESULT inflate_into(BYTE *dst, UINT32 dst_size, const BYTE *src, UINT32 src_size)
{
    unsigned long out = dst_size, in;
    int ret;

    /* A zlib header (CMF/FLG: deflate, check bits) is skipped; otherwise raw deflate. */
    if (src_size >= 2 && (src[0] & 0x0f) == 8 && !(((src[0] << 8) | src[1]) % 31))
        src += 2, src_size -= 2;
    in = src_size;
    ret = puff(dst, &out, src, &in);
    return ret ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK;
}

/* ---------------------------------------------------------------- BCPack, through the helper */

#define BCPACK_MAGIC 0x4b504342u /* 'BCPK' */

static BOOL send_all(SOCKET s, const void *data, size_t size)
{
    const char *p = data;
    while (size)
    {
        int n = send(s, p, size > 0x40000000 ? 0x40000000 : (int)size, 0);
        if (n <= 0)
            return FALSE;
        p += n, size -= n;
    }
    return TRUE;
}

static BOOL recv_all(SOCKET s, void *data, size_t size)
{
    char *p = data;
    while (size)
    {
        int n = recv(s, p, size > 0x40000000 ? 0x40000000 : (int)size, 0);
        if (n <= 0)
            return FALSE;
        p += n, size -= n;
    }
    return TRUE;
}

/* One connection per worker thread, made on first use. A TLS slot rather than __thread, which
 * would make mingw's GCC depend on libmcfgthread-2.dll. */
static DWORD bcpack_tls = TLS_OUT_OF_INDEXES;

static BOOL CALLBACK alloc_tls(INIT_ONCE *once, void *param, void **ctx)
{
    bcpack_tls = TlsAlloc();
    return TRUE;
}

static SOCKET get_socket(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    void *v;

    InitOnceExecuteOnce(&once, alloc_tls, NULL, NULL);
    v = bcpack_tls == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(bcpack_tls);
    return v ? (SOCKET)(ULONG_PTR)v - 1 : INVALID_SOCKET;
}

static void set_socket(SOCKET s)
{
    if (bcpack_tls != TLS_OUT_OF_INDEXES)
        TlsSetValue(bcpack_tls, s == INVALID_SOCKET ? NULL : (void *)(ULONG_PTR)(s + 1));
}

static SOCKET bcpack_connect(void)
{
    const char *port = getenv("REBOX_BCPACK_PORT");
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    SOCKET s;
    BOOL nodelay = TRUE;

    if ((s = get_socket()) != INVALID_SOCKET)
        return s;
    if (!port || !*port)
        return INVALID_SOCKET;
    {
        static LONG started;
        WSADATA wsa;
        if (!InterlockedExchange(&started, 1))
            WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    addr.sin_port = htons((u_short)atoi(port));
    if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET)
        return INVALID_SOCKET;
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("xcrdapi: can't reach the BCPack helper on port %s (error %d)", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    set_socket(s);
    return s;
}

static HRESULT bcpack_decode(UINT32 codec, const BYTE *raw, UINT32 raw_size, BYTE *dst, UINT32 dst_size)
{
    UINT32 header[4] = { BCPACK_MAGIC, codec, dst_size, raw_size }, reply[2];
    SOCKET s = bcpack_connect();

    if (s == INVALID_SOCKET)
        return E_NOTIMPL;
    if (!send_all(s, header, sizeof(header)) || !send_all(s, raw, raw_size) || !recv_all(s, reply, sizeof(reply)))
    {
        closesocket(s);
        set_socket(INVALID_SOCKET);
        return E_FAIL;
    }
    if (reply[1] > dst_size)
    {
        /* Can't happen with the helper's decoder; drop the connection rather than resync. */
        closesocket(s);
        set_socket(INVALID_SOCKET);
        return E_FAIL;
    }
    if (reply[1] && !recv_all(s, dst, reply[1]))
    {
        closesocket(s);
        set_socket(INVALID_SOCKET);
        return E_FAIL;
    }
    return (HRESULT)reply[0];
}

typedef BOOL (WINAPI *write_pages_fn)(const ULONG_PTR *, ULONG_PTR, SIZE_T, const void *, SIZE_T);

static BOOL CALLBACK find_write_pages(INIT_ONCE *once, void *param, void **out)
{
    HMODULE m = GetModuleHandleA("xmem.dll");
    *out = m ? (void *)GetProcAddress(m, "ReboxWritePhysicalPages") : NULL;
    return TRUE;
}

/* Writes a decoded request into its destination pages. A page number still 0 in the snapshot is
 * read again from the title's array now: a title can fill an entry after enqueueing the request
 * (xcl, ivall/xcl, GPL-2.0-only, dstorage.rs: a retail title's streaming requests). */
static HRESULT write_pages(const struct entry *e, const BYTE *data, UINT32 size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const ULONG_PTR *array = e->request.destination;
    write_pages_fn write;

    InitOnceExecuteOnce(&once, find_write_pages, NULL, (void **)&write);
    if (!write)
        return E_NOTIMPL;
    for (UINT32 i = 0; i < e->page_count; i++)
        if (!e->pages[i] && !IsBadReadPtr(array + i, sizeof(*array)))
            e->pages[i] = array[i];
    if (!write(e->pages, e->page_count, (SIZE_T)e->request.destination_page_offset, data, size))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xcrdapi: writing %u bytes to %u physical pages (first %#llx, offset %llu) failed", size,
                      e->page_count, e->page_count ? (unsigned long long)e->pages[0] : 0ull,
                      (unsigned long long)e->request.destination_page_offset);
        return E_INVALIDARG;
    }
    return S_OK;
}

static HRESULT run_request(struct queue *q, const struct entry *e)
{
    const DSTORAGE_REQUEST *r = &e->request;
    BOOL from_memory = !!(r->options & OPT_SOURCE_MEMORY);
    BYTE *data = NULL;
    HRESULT hr = S_OK;

    BOOL bcpack = !!(r->options & OPT_BCPACK);
    BOOL to_pages = !!(r->options & OPT_DEST_PHYSICAL);
    DSTORAGE_REQUEST paged;

    /* A destination in physical pages: decoded into a buffer here, then written through the pages
     * (xmem). */
    if (to_pages)
    {
        HRESULT hr;
        struct entry inner = *e;
        BYTE *buffer;

        if (!e->pages || !r->destination_size)
            return r->destination_size ? E_INVALIDARG : S_OK;
        if (!(buffer = calloc(1, r->destination_size)))
            return E_OUTOFMEMORY;
        paged = *r;
        paged.options &= ~OPT_DEST_PHYSICAL;
        paged.destination = buffer;
        inner.request = paged;
        inner.pages = NULL;
        if (SUCCEEDED(hr = run_request(q, &inner)))
            hr = write_pages(e, buffer, r->destination_size);
        free(buffer);
        return hr;
    }
    if ((r->options & (OPT_SWIZZLE | OPT_SOURCE_PHYSICAL))
            || (bcpack && !getenv("REBOX_BCPACK_PORT")))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 16)
            rebox_log("xcrdapi: unsupported request options %#llx (%s%s%s), %u bytes", (unsigned long long)r->options,
                         r->options & OPT_BCPACK ? "BCPack " : "", r->options & OPT_SWIZZLE ? "swizzle " : "",
                         r->options & (OPT_DEST_PHYSICAL | OPT_SOURCE_PHYSICAL) ? "physical" : "", r->source_size);
        return E_NOTIMPL;
    }
    if (!r->source_size)
        return S_OK;
    if (!r->destination || !r->source)
        return E_INVALIDARG;
    if (!(r->options & OPT_ZLIB) && !bcpack && r->destination_size < r->source_size)
        return E_INVALIDARG;

    if (from_memory)
        data = r->source;
    else
    {
        OVERLAPPED at = { .Offset = (DWORD)r->file_offset, .OffsetHigh = (DWORD)(r->file_offset >> 32) };
        DWORD read = 0;
        BOOL ok;

        if (r->options & OPT_ZLIB || bcpack)
        {
            if (!(data = malloc(r->source_size)))
                return E_OUTOFMEMORY;
        }
        else
            data = r->destination;
        AcquireSRWLockShared(&e->file->lock);
        ok = e->file->handle != INVALID_HANDLE_VALUE
                && ReadFile(e->file->handle, data, r->source_size, &read, &at);
        if (!ok)
            hr = HRESULT_FROM_WIN32(e->file->handle == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE : GetLastError());
        else if (read != r->source_size)
            hr = HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        ReleaseSRWLockShared(&e->file->lock);
    }
    if (SUCCEEDED(hr) && bcpack)
    {
        hr = bcpack_decode((UINT32)((r->options & OPT_BCPACK) >> 1), data, r->source_size, r->destination,
                           r->destination_size);
        if (FAILED(hr))
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 16)
                rebox_log("xcrdapi: BCPack codec %u, %u -> %u bytes: %#lx", (unsigned)((r->options & OPT_BCPACK) >> 1),
                             r->source_size, r->destination_size, hr);
        }
    }
    else if (SUCCEEDED(hr) && (r->options & OPT_ZLIB))
        hr = inflate_into(r->destination, r->destination_size, data, r->source_size);
    else if (SUCCEEDED(hr) && from_memory)
        memcpy(r->destination, r->source, r->source_size);
    if (data && data != r->destination && data != r->source)
        free(data);
    return hr;
}

typedef void (WINAPI *written_fn)(UINT64, UINT64);

static BOOL CALLBACK find_written(INIT_ONCE *once, void *param, void **out)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    *out = m ? (void *)GetProcAddress(m, "ReboxTitleMemoryWritten") : NULL;
    return TRUE;
}

/* d3d12_xs uploads textures placed at title addresses when their memory changes: tell it what
 * each request wrote (title_textures.c). */
static void report_written(void *address, UINT32 size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    written_fn written;

    InitOnceExecuteOnce(&once, find_written, NULL, (void **)&written);
    if (written && address && size)
        written((UINT64)(ULONG_PTR)address, size);
}

static void release_entry(const struct entry *e)
{
    if (e->file)
        object_Release(&e->file->obj);
    if (e->status)
        object_Release(&e->status->obj);
    if (e->fence)
        ID3D12Fence_Release(e->fence);
    free(e->pages);
}

static DWORD WINAPI queue_worker(void *param)
{
    struct queue *q = param;

    EnterCriticalSection(&q->cs);
    for (;;)
    {
        struct entry e;

        while (!q->stop && q->submitted_head == q->submitted_count)
            SleepConditionVariableCS(&q->wake, &q->cs, INFINITE);
        if (q->stop)
            break;
        e = q->submitted[q->submitted_head++];
        if (q->submitted_head == q->submitted_count)
            q->submitted_head = q->submitted_count = 0;
        LeaveCriticalSection(&q->cs);

        if (e.kind == ENTRY_STATUS)
        {
            e.status->entries[e.index].hr = q->first_failure;
            InterlockedExchange(&e.status->entries[e.index].complete, TRUE);
            q->first_failure = S_OK;
        }
        else if (e.kind == ENTRY_SIGNAL)
            ID3D12Fence_Signal(e.fence, e.value);
        else if (e.kind == ENTRY_EVENT)
        {
            SetEvent(e.event);
            CloseHandle(e.event);
        }
        else
        {
            HRESULT hr = run_request(q, &e);
            if (FAILED(hr))
                InterlockedIncrement64(&stats.failed);
            if (SUCCEEDED(hr) && !(e.request.options & OPT_DEST_PHYSICAL)) /* pages: xmem reports */
                report_written(e.request.destination, e.request.destination_size);
            if (FAILED(hr))
            {
                static LONG logged;
                /* REBOX_DSTORAGE_IGNORE_FAILURES=1: the status entries read as success anyway, to
                 * see whether a title gives up on a whole batch for one failed request. */
                static int ignore = -1;
                if (ignore < 0)
                {
                    const char *v = getenv("REBOX_DSTORAGE_IGNORE_FAILURES");
                    ignore = v && *v == '1';
                }
                if (SUCCEEDED(q->first_failure) && !ignore)
                    q->first_failure = hr;
                if (InterlockedIncrement(&logged) <= 16)
                    rebox_log("xcrdapi: request failed %#lx (options %#llx, offset %llu, %u -> %u bytes)", hr,
                                 (unsigned long long)e.request.options, (unsigned long long)e.request.file_offset,
                                 e.request.source_size, e.request.destination_size);
            }
        }
        InterlockedIncrement64(&stats.done[e.kind]);
        release_entry(&e);
        EnterCriticalSection(&q->cs);
    }
    LeaveCriticalSection(&q->cs);
    return 0;
}

/* Caller holds q->cs. */
static void submit_locked(struct queue *q)
{
    InterlockedAdd64(&stats.submitted, q->pending_count);
    for (UINT32 i = 0; i < q->pending_count; i++)
        push(&q->submitted, &q->submitted_count, &q->submitted_size, &q->pending[i]);
    q->pending_count = 0;
    if (!q->worker)
        q->worker = CreateThread(NULL, 0, queue_worker, q, 0, NULL);
    WakeConditionVariable(&q->wake);
}

/* Caller holds q->cs. The console submits by itself once more than half the queue is waiting. */
static void enqueue_locked(struct queue *q, const struct entry *e)
{
    InterlockedIncrement64(&stats.enqueued[e->kind]);
    push(&q->pending, &q->pending_count, &q->pending_size, e);
    if (!q->manual_submit || q->pending_count * 2 > q->capacity)
        submit_locked(q);
}

static void WINAPI queue_EnqueueRequest(struct queue *q, const DSTORAGE_REQUEST *request)
{
    struct entry e = { ENTRY_REQUEST };

    if (!request)
        return;
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("xcrdapi: request (options %#llx, %s, %u -> %u bytes)", (unsigned long long)request->options,
                         request->options & OPT_SOURCE_MEMORY ? "from memory" : "from a file", request->source_size,
                         request->destination_size);
    }
    e.request = *request;
    {
        /* Each kind of request once (its options), with an example: what a title asks for. */
        static UINT64 kinds[32];
        static LONG kind_count;
        BOOL seen = FALSE;
        for (LONG k = 0; k < min(kind_count, 32); k++)
            seen |= kinds[k] == request->options;
        if (!seen)
        {
            LONG k = InterlockedIncrement(&kind_count) - 1;
            if (k < 32)
            {
                kinds[k] = request->options;
                rebox_log("xcrdapi: a request with options %#llx (%s, %u -> %u bytes, intermediate %u, destination %p "
                          "+%llu)", (unsigned long long)request->options,
                          request->options & OPT_SOURCE_MEMORY ? "from memory" : "from a file", request->source_size,
                          request->destination_size, request->intermediate_size, request->destination,
                          (unsigned long long)request->destination_page_offset);
            }
        }
    }
    /* A page array is copied now: titles also pass arrays on the stack (xcl). Pages are 64 KiB. */
    if ((request->options & OPT_DEST_PHYSICAL) && request->destination && request->destination_size)
    {
        UINT64 span = request->destination_page_offset + (UINT64)request->destination_size;
        e.page_count = (UINT32)((span + 0xffff) >> 16);
        if ((e.pages = calloc(e.page_count, sizeof(*e.pages)))
                && !IsBadReadPtr(request->destination, e.page_count * sizeof(*e.pages)))
            memcpy(e.pages, request->destination, e.page_count * sizeof(*e.pages));
    }
    if (!(request->options & OPT_SOURCE_MEMORY) && request->source)
    {
        e.file = request->source;
        object_AddRef(&e.file->obj);
    }
    EnterCriticalSection(&q->cs);
    enqueue_locked(q, &e);
    LeaveCriticalSection(&q->cs);
}

static void WINAPI queue_EnqueueStatus(struct queue *q, struct status_array *status, UINT32 index)
{
    struct entry e = { ENTRY_STATUS };

    if (!status || index >= status->capacity)
        return;
    InterlockedExchange(&status->entries[index].complete, FALSE);
    e.status = status;
    e.index = index;
    object_AddRef(&status->obj);
    EnterCriticalSection(&q->cs);
    enqueue_locked(q, &e);
    LeaveCriticalSection(&q->cs);
}

static void WINAPI queue_EnqueueSignal(struct queue *q, ID3D12Fence *fence, UINT64 value)
{
    struct entry e = { ENTRY_SIGNAL };

    if (!fence)
        return;
    e.fence = fence;
    e.value = value;
    ID3D12Fence_AddRef(fence);
    EnterCriticalSection(&q->cs);
    enqueue_locked(q, &e);
    LeaveCriticalSection(&q->cs);
}

/* Slot 5: the console's EnqueueSetEvent(HANDLE); PC's EnqueueSignal(fence, value) sits there, so
 * an object pointer is taken for a fence. Handles are small multiples of 4. */
static void WINAPI queue_EnqueueSetEvent(struct queue *q, void *handle, UINT64 value)
{
    struct entry e = { ENTRY_EVENT };
    static LONG logged;

    if (!handle)
        return;
    if ((ULONG_PTR)handle > 0x100000)
    {
        if (!InterlockedExchange(&logged, 1))
            rebox_log("xcrdapi: queue slot 5 called with an object: taken as EnqueueSignal(fence, %llu)",
                         (unsigned long long)value);
        queue_EnqueueSignal(q, handle, value);
        return;
    }
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &e.event, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return;
    EnterCriticalSection(&q->cs);
    enqueue_locked(q, &e);
    LeaveCriticalSection(&q->cs);
}

/* X2: SetAutoSubmitDisabled(bool) and GetAutoSubmitDisabled(). */
static HRESULT WINAPI queue_SetAutoSubmitDisabled(struct queue *q, BOOL disabled)
{
    EnterCriticalSection(&q->cs);
    q->manual_submit = (BYTE)disabled != 0;
    LeaveCriticalSection(&q->cs);
    return S_OK;
}

static BOOL WINAPI queue_GetAutoSubmitDisabled(struct queue *q)
{
    return q->manual_submit;
}

static void WINAPI queue_Submit(struct queue *q)
{
    EnterCriticalSection(&q->cs);
    submit_locked(q);
    LeaveCriticalSection(&q->cs);
}

static void queue_destroy(struct object *self)
{
    struct queue *q = (struct queue *)self;

    EnterCriticalSection(&q->cs);
    submit_locked(q);
    q->stop = TRUE;
    WakeAllConditionVariable(&q->wake);
    LeaveCriticalSection(&q->cs);
    if (q->worker)
    {
        WaitForSingleObject(q->worker, INFINITE);
        CloseHandle(q->worker);
    }
    for (UINT32 i = q->submitted_head; i < q->submitted_count; i++)
        release_entry(&q->submitted[i]);
    DeleteCriticalSection(&q->cs);
    free(q->pending);
    free(q->submitted);
    free(q);
}

UNNAMED(queue, 7) UNNAMED(queue, 8) UNNAMED(queue, 9) UNNAMED(queue, 10) UNNAMED(queue, 11) UNNAMED(queue, 13)
static void *const queue_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, queue_EnqueueRequest, queue_EnqueueStatus,
    queue_EnqueueSetEvent, queue_Submit, queue_slot7, queue_slot8, queue_slot9, queue_slot10, queue_slot11,
    queue_EnqueueSignal, queue_slot13, queue_SetAutoSubmitDisabled, queue_GetAutoSubmitDisabled,
};

/* ---------------------------------------------------------------------------- factory */

static HRESULT WINAPI factory_CreateQueue(struct object *self, const DSTORAGE_QUEUE_DESC *desc, REFIID riid, void **out)
{
    struct queue *q;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!desc || desc->source_type > 1)
        return E_INVALIDARG;
    rebox_log("xcrdapi: queue created (%s source, capacity %u, priority %d, %s)",
                 desc->source_type ? "memory" : "file", desc->capacity, desc->priority, desc->name ? desc->name : "unnamed");
    if (!(q = calloc(1, sizeof(*q))))
        return E_OUTOFMEMORY;
    q->obj = (struct object){ queue_vtbl, 1, queue_destroy };
    q->source_type = desc->source_type;
    q->capacity = desc->capacity < 0x80 ? 0x80 : desc->capacity > 0x2000 ? 0x2000 : desc->capacity;
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->wake);
    *out = q;
    return S_OK;
}

static HRESULT WINAPI factory_OpenFile(struct object *self, const WCHAR *path, REFIID riid, void **out)
{
    struct file *f;
    HANDLE h;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!path)
        return E_INVALIDARG;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        static LONG logged;
        DWORD error = GetLastError();
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xcrdapi: OpenFile(%ls) failed (error %lu)", path, error);
        return HRESULT_FROM_WIN32(error);
    }
    if (!(f = calloc(1, sizeof(*f))))
    {
        CloseHandle(h);
        return E_OUTOFMEMORY;
    }
    f->obj = (struct object){ file_vtbl, 1, file_destroy };
    f->handle = h;
    InitializeSRWLock(&f->lock);
    *out = f;
    return S_OK;
}

static HRESULT WINAPI factory_CreateStatusArray(struct object *self, UINT32 capacity, const char *name, REFIID riid,
                                                void **out)
{
    struct status_array *a;

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!capacity)
        return E_INVALIDARG;
    if (!(a = calloc(1, sizeof(*a))) || !(a->entries = calloc(capacity, sizeof(*a->entries))))
    {
        free(a);
        return E_OUTOFMEMORY;
    }
    a->obj = (struct object){ status_vtbl, 1, status_destroy };
    a->capacity = capacity;
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
            rebox_log("xcrdapi: status array %p of %u entries (%s)", (void *)a, capacity,
                      name && !IsBadStringPtrA(name, 64) ? name : "no name");
    }
    for (UINT32 i = 0; i < capacity; i++)
        a->entries[i].complete = TRUE;
    *out = a;
    return S_OK;
}

/* Slot 10 of a newer SDK's factory (IID {62959d11-d45f-4884-a0a3-f7862ea243ae}, xcl): CreateQueue
 * with a 0x20-byte desc, CreateQueue's 0x18 then a u64 (the target title: 1), for the newer queue
 * interface ({3c02cb0c-c050-4195-99a1-a1ce94ba33c5}). Without it the title used the null queue. */
static HRESULT WINAPI factory_CreateQueue1(struct object *self, const DSTORAGE_QUEUE_DESC *desc, REFIID riid, void **out)
{
    static LONG logged;
    if (desc && !InterlockedExchange(&logged, 1))
        rebox_log("xcrdapi: CreateQueue with the newer 0x20-byte desc (u64 at 0x18: %#llx)",
                  (unsigned long long)*(const UINT64 *)((const BYTE *)desc + 0x18));
    return factory_CreateQueue(self, desc, riid, out);
}

UNNAMED(factory, 6) UNNAMED(factory, 7) UNNAMED(factory, 8) UNNAMED(factory, 9)
UNNAMED(factory, 11)
static void *const factory_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, factory_CreateQueue, factory_OpenFile,
    factory_CreateStatusArray, factory_slot6, factory_slot7, factory_slot8, factory_slot9, factory_CreateQueue1,
    factory_slot11,
};

/* A process-wide singleton, never destroyed. */
static struct object factory = { factory_vtbl, 1, NULL };

HRESULT WINAPI DStorageGetFactory(REFIID riid, void **out)
{
    static LONG logged;

    if (!out)
        return E_POINTER;
    if (!InterlockedExchange(&logged, 1))
    {
        rebox_log("xcrdapi: DirectStorage factory requested");
        start_stats();
    }
    object_AddRef(&factory);
    *out = &factory;
    return S_OK;
}
