/* Replacement for the console's XCRDAPI.dll: DirectStorage (dstorage_xs.h).
 *
 * A title imports DStorageGetFactory and drives everything through COM-style objects: the
 * factory, queues, files and status arrays. Structure layouts are the public GDK reference's
 * (DSTORAGE_QUEUE_DESC, DSTORAGE_REQUEST); vtable slots are the ones ScarlettX saw a retail title
 * call. Queue slot 5 is taken to be EnqueueSignal(ID3D12Fence *, UINT64), as on PC: it sits
 * between EnqueueStatus and Submit there too. Other slots log their first calls and return 0.
 *
 * Requests run in order on one worker thread per queue, reading from host files or memory.
 * zlib-compressed requests are inflated (zlib-wrapped or raw deflate). BCPack requests go to
 * helpers/bcpack, a separate process running Leonida's GPLv3 decoder (rebox-run starts it and
 * sets REBOX_BCPACK_PORT), over a localhost connection per worker thread. Swizzled
 * destinations and physical-page addresses aren't supported, nor is BCPack without the helper:
 * those requests fail with E_NOTIMPL and are logged, and the next status entry reports it. */

#define COBJMACROS
#include <winsock2.h>
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
    return index < a->capacity && a->entries[index].complete;
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

enum entry_kind { ENTRY_REQUEST, ENTRY_STATUS, ENTRY_SIGNAL };

struct entry
{
    enum entry_kind kind;
    DSTORAGE_REQUEST request;
    struct file *file;          /* held until the request has run */
    struct status_array *status; /* held until the status is written */
    UINT32 index;
    ID3D12Fence *fence;
    UINT64 value;
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

static HRESULT run_request(struct queue *q, const struct entry *e)
{
    const DSTORAGE_REQUEST *r = &e->request;
    BOOL from_memory = !!(r->options & OPT_SOURCE_MEMORY);
    BYTE *data = NULL;
    HRESULT hr = S_OK;

    BOOL bcpack = !!(r->options & OPT_BCPACK);

    if ((r->options & (OPT_SWIZZLE | OPT_DEST_PHYSICAL | OPT_SOURCE_PHYSICAL))
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

static void release_entry(const struct entry *e)
{
    if (e->file)
        object_Release(&e->file->obj);
    if (e->status)
        object_Release(&e->status->obj);
    if (e->fence)
        ID3D12Fence_Release(e->fence);
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
        else
        {
            HRESULT hr = run_request(q, &e);
            if (FAILED(hr))
            {
                static LONG logged;
                if (SUCCEEDED(q->first_failure))
                    q->first_failure = hr;
                if (InterlockedIncrement(&logged) <= 16)
                    rebox_log("xcrdapi: request failed %#lx (options %#llx, offset %llu, %u -> %u bytes)", hr,
                                 (unsigned long long)e.request.options, (unsigned long long)e.request.file_offset,
                                 e.request.source_size, e.request.destination_size);
            }
        }
        release_entry(&e);
        EnterCriticalSection(&q->cs);
    }
    LeaveCriticalSection(&q->cs);
    return 0;
}

/* Caller holds q->cs. */
static void submit_locked(struct queue *q)
{
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
    push(&q->pending, &q->pending_count, &q->pending_size, e);
    if (q->pending_count * 2 > q->capacity)
        submit_locked(q);
}

static void WINAPI queue_EnqueueRequest(struct queue *q, const DSTORAGE_REQUEST *request)
{
    struct entry e = { ENTRY_REQUEST };

    if (!request)
        return;
    e.request = *request;
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
    static LONG logged;

    if (!fence)
        return;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("xcrdapi: queue slot 5 taken as EnqueueSignal(fence %p, %llu)", fence, (unsigned long long)value);
    e.fence = fence;
    e.value = value;
    ID3D12Fence_AddRef(fence);
    EnterCriticalSection(&q->cs);
    enqueue_locked(q, &e);
    LeaveCriticalSection(&q->cs);
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

UNNAMED(queue, 7) UNNAMED(queue, 8) UNNAMED(queue, 9) UNNAMED(queue, 10) UNNAMED(queue, 11)
static void *const queue_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, queue_EnqueueRequest, queue_EnqueueStatus,
    queue_EnqueueSignal, queue_Submit, queue_slot7, queue_slot8, queue_slot9, queue_slot10, queue_slot11,
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
    for (UINT32 i = 0; i < capacity; i++)
        a->entries[i].complete = TRUE;
    *out = a;
    return S_OK;
}

UNNAMED(factory, 6) UNNAMED(factory, 7) UNNAMED(factory, 8) UNNAMED(factory, 9) UNNAMED(factory, 10)
UNNAMED(factory, 11)
static void *const factory_vtbl[] = {
    object_QueryInterface, object_AddRef, object_Release, factory_CreateQueue, factory_OpenFile,
    factory_CreateStatusArray, factory_slot6, factory_slot7, factory_slot8, factory_slot9, factory_slot10,
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
        rebox_log("xcrdapi: DirectStorage factory requested");
    object_AddRef(&factory);
    *out = &factory;
    return S_OK;
}
