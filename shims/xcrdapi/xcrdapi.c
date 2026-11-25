/* Replacement for the console's XCRDAPI.dll: DirectStorage (dstorage_xs.h).
 *
 * A title imports DStorageGetFactory and drives everything through COM-style objects: the
 * factory, queues, files and status arrays. Structure layouts are the public GDK reference's
 * (DSTORAGE_QUEUE_DESC, DSTORAGE_REQUEST); vtable slots are the ones ScarlettX saw a retail title
 * call. The queue's methods are dstorage_xs.h's (OpenScarlett's compat header, from Microsoft's
 * MIT samples): 3 EnqueueRequest, 4 EnqueueStatus, 5 EnqueueSetEvent (PC's EnqueueSignal sits
 * there; a fence is told from an event handle and signalled), 6 Submit, 7-11 CancelRequestsWithTag,
 * Close, GetErrorEvent, RetrieveErrorRecord, Query, then X1's EnqueueSignal (12) and X2's Query1,
 * SetAutoSubmitDisabled and GetAutoSubmitDisabled (13-15). Other slots log their first calls and
 * return 0.
 *
 * What is enqueued waits for Submit, or until half the queue's capacity is waiting, as xcl and
 * VIctory have it (facts only). A title may finish a request's destination between enqueueing it
 * and Submit: one fills its page table (the page arrays its requests point into) for the whole
 * batch just before EnqueueStatus and Submit, so a request run at enqueue found page numbers still
 * 0, or the page that slot held before, which may be another resource's by then. A UE5 title never
 * calls Submit, and its streaming textures stayed pending until garbage collection gave up on
 * them: a queue that has never been submitted submits by itself once something has waited
 * REBOX_DSTORAGE_SUBMIT_MS (default 20). SetAutoSubmitDisabled(FALSE), or
 * REBOX_DSTORAGE_SUBMIT=enqueue for every queue, runs requests as they are enqueued.
 *
 * Requests run in order on one worker thread per queue, reading from host files or memory.
 * zlib-compressed requests are inflated (zlib-wrapped or raw deflate). BCPack requests (bits 1-4:
 * BC1, BC3-BC7) are decoded by bcpack.c, zlib first when a request has both, with tables the user
 * supplies (see "BCPack" below and README.md). A destination in physical
 * pages (options bit 21: an array of 64 KiB page numbers, the offset into the first at +0x10) is
 * decoded into a buffer and written through the pages (xmem ReboxWritePhysicalPages). The page
 * numbers are read as the request runs, unless the array is on the enqueuing thread's stack (xcl:
 * titles pass those too), which is copied at enqueue. Swizzled
 * destinations and physical-page sources aren't supported, nor is BCPack without its tables:
 * those requests fail with E_NOTIMPL and are logged, and the next status entry reports it. */

#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>

#include "../common/rebox_log.h"
#include "../d3d12_xs/puff.h"
#include "bcpack.h"

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

/* Failed requests by result and options, for the stats line. */
static struct { volatile LONG hr; volatile LONG64 options; volatile LONG64 count; } failures[8];

static void count_failure(HRESULT hr, UINT64 options)
{
    for (int i = 0; i < (int)(sizeof(failures) / sizeof(*failures)); i++)
    {
        if (!failures[i].count && InterlockedCompareExchange64(&failures[i].count, -1, 0) == 0)
        {
            failures[i].hr = hr;
            failures[i].options = (LONG64)options;
            InterlockedExchange64(&failures[i].count, 1);
            return;
        }
        while (failures[i].count < 0)
            YieldProcessor();
        if (failures[i].hr == hr && failures[i].options == (LONG64)options)
        {
            InterlockedIncrement64(&failures[i].count);
            return;
        }
    }
}

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
        if (stats.failed)
        {
            at = 0;
            for (int i = 0; i < (int)(sizeof(failures) / sizeof(*failures)) && failures[i].count > 0; i++)
                at += snprintf(line + at, sizeof(line) - at, "%s%#lx/options %#llx: %lld", i ? ", " : "",
                               failures[i].hr, (unsigned long long)failures[i].options,
                               (long long)failures[i].count);
            rebox_log("xcrdapi: failed by result and options: %s", line);
        }
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
    BOOL pages_live;            /* the array isn't on the enqueuing stack: read again as it runs */
    BOOL traced;                /* its destination covers REBOX_PHYSICAL_TRACE */
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
    int auto_submit;            /* SetAutoSubmitDisabled: -1 never called, 0 disabled, 1 enabled */
    BOOL title_submits;         /* Submit has been called: it never submits by itself after a wait */
    ULONGLONG pending_since;    /* when the oldest unsubmitted entry was enqueued (GetTickCount64) */
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

/* xmem's write tracking (xmem.c): a request's destination is held writable while it is read and
 * decoded, and noted written after. Armed pages are read-only: a read into them fails rather than
 * faulting, and writable_range would refuse them. */
static void *(WINAPI *prepare_write)(ULONGLONG, ULONGLONG);
static void (WINAPI *finish_write)(void *);

static BOOL CALLBACK find_prepare_write(INIT_ONCE *once, void *param, void **out)
{
    HMODULE m = GetModuleHandleA("xmem.dll");
    prepare_write = m ? (void *)GetProcAddress(m, "ReboxPrepareWrite") : NULL;
    finish_write = m ? (void *)GetProcAddress(m, "ReboxFinishWrite") : NULL;
    if (!prepare_write || !finish_write)
        prepare_write = NULL, finish_write = NULL;
    return TRUE;
}

static void *hold_for_write(void *address, SIZE_T size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, find_prepare_write, NULL, NULL);
    return prepare_write && address && size ? prepare_write((ULONGLONG)(ULONG_PTR)address, size) : NULL;
}

static void release_write(void *hold)
{
    if (hold && finish_write)
        finish_write(hold);
}

/* Whether [p, p + size) is all committed, writable memory. */
static BOOL writable_range(const void *p, SIZE_T size)
{
    const BYTE *at = p, *end = (const BYTE *)p + size;
    MEMORY_BASIC_INFORMATION mbi;

    while (at < end)
    {
        if (!VirtualQuery(at, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)
                || !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return FALSE;
        at = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return TRUE;
}

/* produced (if not NULL) gets the bytes written. */
static HRESULT inflate_into(BYTE *dst, UINT32 dst_size, const BYTE *src, UINT32 src_size, UINT32 *produced)
{
    unsigned long out = dst_size, in;
    int ret;

    /* A zlib header (CMF/FLG: deflate, check bits) is skipped; otherwise raw deflate. */
    if (src_size >= 2 && (src[0] & 0x0f) == 8 && !(((src[0] << 8) | src[1]) % 31))
        src += 2, src_size -= 2;
    in = src_size;
    ret = puff(dst, &out, src, &in);
    if (produced)
        *produced = ret ? 0 : (UINT32)out;
    return ret ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK;
}

/* ---------------------------------------------------------------------------------- BCPack */

/* Decoded in process (bcpack.c), with tables the user supplies: they are Microsoft's format data
 * and not part of Rebox. Looked for, once, at REBOX_BCPACK_TABLES, then bcpack-tables.bin next to
 * this DLL, then $XDG_CONFIG_HOME/rebox/bcpack-tables.bin (or ~/.config/rebox/...). Unix paths
 * are read through Wine's Z: drive. Without them BCPack requests fail with E_NOTIMPL. */
static struct bcpack_tables *bcpack_tables;

/* A Unix path (starting with /) as Z:path; Windows paths as they are. */
static void host_path(char *out, size_t size, const char *path)
{
    snprintf(out, size, "%s%s", path[0] == '/' ? "Z:" : "", path);
}

static BOOL env(const char *name, char *out, DWORD size)
{
    DWORD n = GetEnvironmentVariableA(name, out, size);
    return n && n < size;
}

static BYTE *read_whole(const char *path, DWORD *size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER length;
    BYTE *data = NULL;
    DWORD read = 0;

    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(f, &length) && length.QuadPart <= 1 << 20 && (data = malloc(length.QuadPart + 1))
            && (!ReadFile(f, data, (DWORD)length.QuadPart, &read, NULL) || read != length.QuadPart))
    {
        free(data);
        data = NULL;
    }
    CloseHandle(f);
    *size = read;
    return data;
}

static BOOL CALLBACK load_bcpack_tables(INIT_ONCE *once, void *param, void **ctx)
{
    char candidates[3][MAX_PATH + 64], value[MAX_PATH], tried[3 * (MAX_PATH + 64) + 8] = "";
    unsigned count = 0;
    HMODULE self;

    if (env("REBOX_BCPACK_TABLES", value, sizeof(value)))
        host_path(candidates[count++], sizeof(candidates[0]), value);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)load_bcpack_tables, &self)
            && GetModuleFileNameA(self, value, sizeof(value)) && strrchr(value, '\\'))
    {
        strcpy(strrchr(value, '\\') + 1, "bcpack-tables.bin");
        strcpy(candidates[count++], value);
    }
    if (env("XDG_CONFIG_HOME", value, sizeof(value)) && value[0] == '/')
        snprintf(candidates[count++], sizeof(candidates[0]), "Z:%s/rebox/bcpack-tables.bin", value);
    else if (env("HOME", value, sizeof(value)) && value[0] == '/')
        snprintf(candidates[count++], sizeof(candidates[0]), "Z:%s/.config/rebox/bcpack-tables.bin", value);

    for (unsigned i = 0; i < count; i++)
    {
        const char *why = "can't be read";
        DWORD size = 0;
        BYTE *data = read_whole(candidates[i], &size);
        if (data && (bcpack_tables = bcpack_tables_parse(data, size, &why)))
        {
            rebox_log("xcrdapi: BCPack tables from %s", candidates[i]);
            free(data);
            return TRUE;
        }
        free(data);
        if (data || i == 0)
            rebox_log("xcrdapi: BCPack tables at %s not used: %s", candidates[i], why);
        snprintf(tried + strlen(tried), sizeof(tried) - strlen(tried), "%s%s", i ? ", " : "", candidates[i]);
    }
    rebox_log("xcrdapi: no BCPack tables (looked at %s; set REBOX_BCPACK_TABLES): BCPack requests fail with "
              "E_NOTIMPL", count ? tried : "nothing");
    return TRUE;
}

/* REBOX_BCPACK_DUMP=DIR: each packed chunk (after zlib, if the request has both) is written to
 * DIR as bc<codec>-<sequence>-<destination size>.bin, up to REBOX_BCPACK_DUMP_MAX (default 4096). */
static void dump_chunk(UINT32 codec, const BYTE *data, UINT32 size, UINT32 dst_size)
{
    static volatile LONG sequence;
    static int max = -1;
    char dir[MAX_PATH], path[MAX_PATH + 64], value[32];
    HANDLE f;
    LONG n;

    if (!env("REBOX_BCPACK_DUMP", dir, sizeof(dir)))
        return;
    if (max < 0)
        max = env("REBOX_BCPACK_DUMP_MAX", value, sizeof(value)) ? atoi(value) : 4096;
    if ((n = InterlockedIncrement(&sequence)) > max)
        return;
    snprintf(path, sizeof(path), "%s%s\\bc%u-%05ld-%u.bin", dir[0] == '/' ? "Z:" : "", dir, codec, n, dst_size);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(f, data, size, &written, NULL);
        CloseHandle(f);
    }
    if (n == 1 || n == max)
        rebox_log("xcrdapi: BCPack chunk %ld dumped to %s%s", n, path, n == max ? " (the last)" : "");
}

static HRESULT bcpack_decode_request(UINT32 codec, const BYTE *raw, UINT32 raw_size, BYTE *dst, UINT32 dst_size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

    InitOnceExecuteOnce(&once, load_bcpack_tables, NULL, NULL);
    dump_chunk(codec, raw, raw_size, dst_size);
    return bcpack_decode(bcpack_tables, codec, raw, raw_size, dst, dst_size, NULL);
}

typedef BOOL (WINAPI *write_pages_fn)(const ULONG_PTR *, ULONG_PTR, SIZE_T, const void *, SIZE_T);

static BOOL CALLBACK find_write_pages(INIT_ONCE *once, void *param, void **out)
{
    HMODULE m = GetModuleHandleA("xmem.dll");
    *out = m ? (void *)GetProcAddress(m, "ReboxWritePhysicalPages") : NULL;
    return TRUE;
}

/* A request's page numbers as it runs: an array that isn't on the enqueuing thread's stack is read
 * again now, all of it (a title's page table, filled for the batch before Submit; an entry may have
 * held another page at enqueue). A stack array keeps its snapshot, but an entry still 0 there is
 * read again (xcl, ivall/xcl, GPL-2.0-only, dstorage.rs). */
static void resolve_pages(const struct entry *e)
{
    const ULONG_PTR *array = e->request.destination;

    if (!e->pages)
        return;
    if (e->pages_live && !IsBadReadPtr(array, e->page_count * sizeof(*array)))
        memcpy(e->pages, array, e->page_count * sizeof(*array));
    else
        for (UINT32 i = 0; i < e->page_count; i++)
            if (!e->pages[i] && !IsBadReadPtr(array + i, sizeof(*array)))
                e->pages[i] = array[i];
}

/* Writes a decoded request into its destination pages (resolve_pages has read them). */
static HRESULT write_pages(const struct entry *e, const BYTE *data, UINT32 size)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const ULONG_PTR *array = e->request.destination;
    write_pages_fn write;

    InitOnceExecuteOnce(&once, find_write_pages, NULL, (void **)&write);
    if (!write)
        return E_NOTIMPL;
    if (!write(e->pages, e->page_count, (SIZE_T)e->request.destination_page_offset, data, size))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 8)
        {
            /* Which entries are still 0, and what the array's memory is (xmem). */
            HMODULE xmem = GetModuleHandleA("xmem.dll");
            void (WINAPI *describe)(ULONGLONG, ULONGLONG) =
                    xmem ? (void *)GetProcAddress(xmem, "ReboxPhysicalDescribe") : NULL;
            UINT32 zero = 0;
            for (UINT32 i = 0; i < e->page_count; i++)
                zero += !e->pages[i];
            rebox_log("xcrdapi: writing %u bytes to %u physical pages (first %#llx, offset %llu) failed; %u of them "
                      "0, array at %p", size, e->page_count, e->page_count ? (unsigned long long)e->pages[0] : 0ull,
                      (unsigned long long)e->request.destination_page_offset, zero, array);
            if (describe)
                describe((ULONGLONG)(ULONG_PTR)array, e->page_count * sizeof(*array));
        }
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
    void *hold = NULL;

    BOOL bcpack = !!(r->options & OPT_BCPACK);
    BOOL to_pages = !!(r->options & OPT_DEST_PHYSICAL);
    DSTORAGE_REQUEST paged;

    if (to_pages)
        resolve_pages(e);
    {
        /* REBOX_PHYSICAL_TRACE: a request writing the physical page mapped at the traced address,
         * through any address of it or as a physical-page destination. */
        static UINT64 trace = ~0ull;
        static ULONG_PTR (WINAPI *page_at)(ULONGLONG);
        static LONG logged;
        if (trace == ~0ull)
        {
            const char *v = getenv("REBOX_PHYSICAL_TRACE");
            HMODULE xmem = GetModuleHandleA("xmem.dll");
            trace = v ? _strtoui64(v, NULL, 16) : 0;
            page_at = xmem ? (void *)GetProcAddress(xmem, "ReboxPhysicalPageAt") : NULL;
        }
        if (trace && page_at && r->destination_size)
        {
            ULONG_PTR traced = page_at(trace), hit = 0;
            if (traced && to_pages)
                for (UINT32 i = 0; i < e->page_count && !hit; i++)
                    hit = e->pages[i] == traced ? 1 : 0;
            else if (traced)
                for (UINT64 at = (UINT64)(ULONG_PTR)r->destination & ~0xffffull;
                     at < (UINT64)(ULONG_PTR)r->destination + r->destination_size && !hit; at += 0x10000)
                    hit = page_at(at) == traced ? at : 0;
            if (hit && InterlockedIncrement(&logged) <= 200)
                rebox_log("xcrdapi: trace: a request writes page %#llx (options %#llx, file offset %llu, %u -> %u "
                          "bytes, destination %p%s, at %#llx)", (unsigned long long)traced,
                          (unsigned long long)r->options, (unsigned long long)r->file_offset, r->source_size,
                          r->destination_size, r->destination, to_pages ? " pages" : "", (unsigned long long)hit);
        }
    }
    if (e->traced)
    {
        /* What its destination is as it runs (xmem). */
        HMODULE xmem = GetModuleHandleA("xmem.dll");
        void (WINAPI *describe)(ULONGLONG, ULONGLONG) = xmem ? (void *)GetProcAddress(xmem, "ReboxPhysicalDescribe") : NULL;
        rebox_log("xcrdapi: trace: the request into %p+%#x runs now", r->destination, r->destination_size);
        if (describe)
            describe((ULONGLONG)(ULONG_PTR)r->destination, r->destination_size);
    }

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
    if (r->options & (OPT_SWIZZLE | OPT_SOURCE_PHYSICAL))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 16)
            rebox_log("xcrdapi: unsupported request options %#llx (%s%s%s), %u bytes", (unsigned long long)r->options,
                         r->options & OPT_BCPACK ? "BCPack " : "", r->options & OPT_SWIZZLE ? "swizzle " : "",
                         r->options & (OPT_DEST_PHYSICAL | OPT_SOURCE_PHYSICAL) ? "physical" : "", r->source_size);
        return E_NOTIMPL;
    }
    if (!r->source_size)
        return bcpack ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK; /* an empty packed stream codes no blocks */
    if (!r->destination || !r->source)
        return E_INVALIDARG;
    if (!(r->options & OPT_ZLIB) && !bcpack && r->destination_size < r->source_size)
        return E_INVALIDARG;

    hold = hold_for_write(r->destination, r->destination_size);
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
            {
                release_write(hold);
                return E_OUTOFMEMORY;
            }
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
    /* A destination the title unmapped meanwhile (pages in a tile pool come and go): the request
     * fails, as a read into it does, instead of decoding into it and faulting. */
    if (SUCCEEDED(hr) && (bcpack || (r->options & OPT_ZLIB) || from_memory)
            && !writable_range(r->destination, r->destination_size))
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 16)
            rebox_log("xcrdapi: a request's destination %p+%#x isn't all committed and writable: it fails",
                      r->destination, r->destination_size);
        hr = HRESULT_FROM_WIN32(ERROR_NOACCESS);
    }
    if (SUCCEEDED(hr) && bcpack)
    {
        /* zlib first, if both: into intermediate_size bytes (or, if the title gives none, room
         * for twice the blocks), whose bytes produced are the packed stream. */
        UINT32 codec = (UINT32)((r->options & OPT_BCPACK) >> 1), packed_size = r->source_size;
        const BYTE *packed = data;
        BYTE *middle = NULL;

        if (r->options & OPT_ZLIB)
        {
            UINT64 room = r->intermediate_size ? r->intermediate_size : 2ull * r->destination_size + 65536;
            if (room > 0x7fffffff || !(middle = malloc(room)))
                hr = E_OUTOFMEMORY;
            else
                hr = inflate_into(middle, (UINT32)room, data, r->source_size, &packed_size);
            packed = middle;
        }
        if (SUCCEEDED(hr))
            hr = bcpack_decode_request(codec, packed, packed_size, r->destination, r->destination_size);
        free(middle);
        if (FAILED(hr))
        {
            static LONG logged;
            if (InterlockedIncrement(&logged) <= 16)
                rebox_log("xcrdapi: BCPack codec %u, %u -> %u bytes: %#lx", codec, packed_size, r->destination_size, hr);
        }
    }
    else if (SUCCEEDED(hr) && (r->options & OPT_ZLIB))
        hr = inflate_into(r->destination, r->destination_size, data, r->source_size, NULL);
    else if (SUCCEEDED(hr) && from_memory)
        memcpy(r->destination, r->source, r->source_size);
    if (data && data != r->destination && data != r->source)
        free(data);
    release_write(hold);
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

/* REBOX_DSTORAGE_SUBMIT=enqueue: every queue runs what is enqueued at once (as Rebox did before). */
static BOOL submit_on_enqueue(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char *v = getenv("REBOX_DSTORAGE_SUBMIT");
        on = v && !strcmp(v, "enqueue");
    }
    return on;
}

/* REBOX_DSTORAGE_SUBMIT_MS: how long an entry waits in a queue never submitted (default 20). */
static DWORD submit_delay(void)
{
    static LONG ms = -1;
    if (ms < 0)
    {
        const char *v = getenv("REBOX_DSTORAGE_SUBMIT_MS");
        ms = v && *v ? atoi(v) : 20;
        if (ms < 0)
            ms = 20;
    }
    return (DWORD)ms;
}

/* Caller holds q->cs. */
static BOOL runs_on_enqueue(const struct queue *q)
{
    return q->auto_submit == 1 || (q->auto_submit < 0 && submit_on_enqueue());
}

/* Caller holds q->cs: a queue never submitted (UE5's), not set to wait for Submit. */
static BOOL submits_after_wait(const struct queue *q)
{
    return !q->title_submits && q->auto_submit != 0;
}

static DWORD WINAPI queue_worker(void *param);

/* Caller holds q->cs. */
static void wake_worker(struct queue *q)
{
    if (!q->worker)
        q->worker = CreateThread(NULL, 0, queue_worker, q, 0, NULL);
    WakeConditionVariable(&q->wake);
}

static void submit_locked(struct queue *q);

static DWORD WINAPI queue_worker(void *param)
{
    struct queue *q = param;

    EnterCriticalSection(&q->cs);
    for (;;)
    {
        struct entry e;

        while (!q->stop && q->submitted_head == q->submitted_count)
        {
            DWORD wait = INFINITE;
            if (q->pending_count && submits_after_wait(q))
            {
                ULONGLONG waited = GetTickCount64() - q->pending_since;
                if (waited >= submit_delay())
                {
                    static LONG logged;
                    if (InterlockedIncrement(&logged) <= 4)
                        rebox_log("xcrdapi: queue %p, never submitted, submits %u entries by itself after %llu ms",
                                  (void *)q, q->pending_count, waited);
                    submit_locked(q);
                    continue;
                }
                wait = (DWORD)(submit_delay() - waited);
            }
            SleepConditionVariableCS(&q->wake, &q->cs, wait);
        }
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
            {
                InterlockedIncrement64(&stats.failed);
                count_failure(hr, e.request.options);
            }
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
    wake_worker(q);
}

/* Caller holds q->cs. The console submits by itself once more than half the queue is waiting; a
 * queue never submitted also after a short wait (worker), and one set to auto-submit at once. */
static void enqueue_locked(struct queue *q, const struct entry *e)
{
    InterlockedIncrement64(&stats.enqueued[e->kind]);
    push(&q->pending, &q->pending_count, &q->pending_size, e);
    if (q->pending_count == 1)
        q->pending_since = GetTickCount64();
    if (runs_on_enqueue(q) || q->pending_count * 2 > q->capacity)
        submit_locked(q);
    else if (q->pending_count == 1 && submits_after_wait(q))
        wake_worker(q); /* to time the wait */
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
        /* REBOX_PHYSICAL_TRACE=<address> (xmem's): a request whose virtual destination covers it. */
        static UINT64 trace = ~0ull;
        if (trace == ~0ull)
        {
            const char *v = getenv("REBOX_PHYSICAL_TRACE");
            trace = v ? _strtoui64(v, NULL, 16) : 0;
        }
        if (trace && !(request->options & OPT_DEST_PHYSICAL)
                && trace - (UINT64)(ULONG_PTR)request->destination < request->destination_size)
        {
            e.traced = TRUE;
            rebox_log("xcrdapi: trace: a request (options %#llx, file offset %llu, %u -> %u bytes) writes %p+%#x, "
                      "covering %#llx (thread %#lx)", (unsigned long long)request->options,
                      (unsigned long long)request->file_offset, request->source_size, request->destination_size,
                      request->destination, request->destination_size, (unsigned long long)trace, GetCurrentThreadId());
        }
    }
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
    /* A page array is copied now, for one on the stack (xcl: titles pass those too); any other is
     * read again as the request runs (resolve_pages). Pages are 64 KiB. */
    if ((request->options & OPT_DEST_PHYSICAL) && request->destination && request->destination_size)
    {
        UINT64 span = request->destination_page_offset + (UINT64)request->destination_size;
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        ULONG_PTR at = (ULONG_PTR)request->destination;
        e.page_count = (UINT32)((span + 0xffff) >> 16);
        e.pages_live = !(at >= (ULONG_PTR)tib->StackLimit && at < (ULONG_PTR)tib->StackBase);
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

/* X2: SetAutoSubmitDisabled(bool) and GetAutoSubmitDisabled(). Enabled: what is enqueued runs at
 * once; disabled: it waits for Submit (or half the queue), with no submitting after a wait. */
static HRESULT WINAPI queue_SetAutoSubmitDisabled(struct queue *q, BOOL disabled)
{
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 4)
        rebox_log("xcrdapi: queue %p: SetAutoSubmitDisabled(%d)", (void *)q, (BYTE)disabled != 0);
    EnterCriticalSection(&q->cs);
    q->auto_submit = (BYTE)disabled == 0;
    if (q->auto_submit && q->pending_count)
        submit_locked(q);
    LeaveCriticalSection(&q->cs);
    return S_OK;
}

static BOOL WINAPI queue_GetAutoSubmitDisabled(struct queue *q)
{
    return q->auto_submit == 0;
}

static void WINAPI queue_Submit(struct queue *q)
{
    EnterCriticalSection(&q->cs);
    if (!q->title_submits)
    {
        static LONG logged;
        q->title_submits = TRUE;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("xcrdapi: queue %p submitted by the title: what it enqueues waits for Submit", (void *)q);
    }
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
    q->auto_submit = -1;
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
