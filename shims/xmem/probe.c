/* Call probes on the title's own code (REBOX_PROBE).
 *
 * REBOX_PROBE=RVA[,RVA...] (hex, relative to the exe's base; MODULE!RVA for another module)
 * puts an int3 at each address. A hit logs the thread, the four argument registers, the return
 * address and the stack pointer, then the original byte is put back for one single-stepped
 * instruction and the int3 rearmed. Each probe logs its first REBOX_PROBE_MAX hits (default 20;
 * later hits are counted, not logged). Up to 64 probes. REBOX_PROBE_DUMP=N also logs the first N
 * qwords at rcx (an object's fields: a vtable or function pointer is shown as module+RVA);
 * REBOX_PROBE_DUMP=N:rdx (or r8, r9) dumps at that register instead.
 * REBOX_PROBE_MEMORY=RVA:BYTES[,...] (hex, relative to the exe) also logs those ranges of the exe's
 * memory (its globals) at each logged hit, as dwords. This is how to see what a title's own code
 * is called with without changing its file: xcl's XCL_PROBE does the same with hardware
 * breakpoints (a GDK title's controller path was found so). A hit on another thread while one byte
 * is restored is missed. */

#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "probe.h"

#define MAX_PROBES 64

static struct probe
{
    BYTE *address;
    BYTE original;
    char name[64];
    volatile LONG hits;
} probes[MAX_PROBES];
static UINT probe_count;
static LONG probe_max = 20;
static UINT dump_qwords, dump_register; /* 0 rcx, 1 rdx, 2 r8, 3 r9 */
static struct { ULONG_PTR rva, bytes; } memory_ranges[8];
static UINT memory_range_count;
static DWORD rearm_slot = TLS_OUT_OF_INDEXES; /* the probe a thread is single-stepping past, plus one */

static void write_byte(BYTE *at, BYTE value)
{
    DWORD old;
    if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old))
    {
        *at = value;
        VirtualProtect(at, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 1);
    }
}

static void describe(ULONG_PTR addr, char *out, size_t len)
{
    HMODULE module;
    char path[MAX_PATH], *name;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)addr, &module)
            && GetModuleFileNameA(module, path, sizeof(path)))
    {
        name = strrchr(path, '\\');
        snprintf(out, len, "%s+%#llx", name ? name + 1 : path, (unsigned long long)(addr - (ULONG_PTR)module));
    }
    else
        snprintf(out, len, "%#llx", (unsigned long long)addr);
}

/* REBOX_WRITE_WATCH=ADDRESS:LENGTH[,...] (hex, absolute addresses): who writes there. Once the range
 * is committed (a thread looks every 100 ms), its pages are made read-only; a write into them is
 * let through (the page made writable for one single-stepped instruction, then read-only again)
 * and, when it lands in the range, logged with the writing instruction, its thread and the address
 * (each instruction once, the first 64). A write by the system (ReadFile into the range) fails
 * instead: for finding a writer, not for a title's normal run. */
static struct watch { ULONG_PTR start, length, page_start, page_end; BOOL armed; } watches[8];
static UINT watch_count;
static DWORD watch_slot = TLS_OUT_OF_INDEXES;

static BOOL in_watched_page(ULONG_PTR address, struct watch **found)
{
    for (UINT i = 0; i < watch_count; i++)
        if (watches[i].armed && address >= watches[i].page_start && address < watches[i].page_end)
        {
            *found = &watches[i];
            return TRUE;
        }
    return FALSE;
}

static DWORD WINAPI arm_watches(void *param)
{
    for (UINT tries = 0; tries < 6000; tries++)
    {
        BOOL all = TRUE;
        for (UINT i = 0; i < watch_count; i++)
        {
            struct watch *w = &watches[i];
            MEMORY_BASIC_INFORMATION mbi;
            DWORD old;
            if (w->armed)
                continue;
            if (VirtualQuery((void *)w->page_start, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT
                    && (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize >= w->page_end
                    && VirtualProtect((void *)w->page_start, w->page_end - w->page_start, PAGE_READONLY, &old))
            {
                w->armed = TRUE;
                rebox_log("write watch %#llx+%#llx armed (pages %#llx-%#llx, were protect %#lx)",
                          (unsigned long long)w->start, (unsigned long long)w->length,
                          (unsigned long long)w->page_start, (unsigned long long)w->page_end, old);
            }
            else
                all = FALSE;
        }
        if (all)
            return 0;
        Sleep(100);
    }
    return 0;
}

static LONG CALLBACK on_probe(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    struct watch *w;

    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2
            && rec->ExceptionInformation[0] == 1 && watch_slot != TLS_OUT_OF_INDEXES
            && in_watched_page(rec->ExceptionInformation[1], &w))
    {
        ULONG_PTR at = rec->ExceptionInformation[1], page = at & ~(ULONG_PTR)0xfff;
        DWORD old;
        if (at >= w->start && at < w->start + w->length)
        {
            static ULONG_PTR seen[64];
            static LONG seen_count;
            ULONG_PTR rip = (ULONG_PTR)rec->ExceptionAddress;
            BOOL known = FALSE;
            for (LONG i = 0; i < min(seen_count, 64); i++)
                known |= seen[i] == rip;
            if (!known)
            {
                LONG n = InterlockedIncrement(&seen_count);
                char where[MAX_PATH + 32], caller[MAX_PATH + 32];
                ULONG_PTR ret = 0;
                if (n <= 64)
                    seen[n - 1] = rip;
                if (n <= 64)
                {
                    describe(rip, where, sizeof(where));
                    if (!IsBadReadPtr((void *)c->Rsp, 8))
                        ret = *(ULONG_PTR *)c->Rsp;
                    describe(ret, caller, sizeof(caller));
                    rebox_log("write watch: %s writes %#llx (thread %#lx; [rsp] %s)", where, (unsigned long long)at,
                              GetCurrentThreadId(), caller);
                }
            }
        }
        VirtualProtect((void *)page, 0x1000, PAGE_READWRITE, &old);
        TlsSetValue(watch_slot, (void *)(page | 1));
        c->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_slot != TLS_OUT_OF_INDEXES)
    {
        ULONG_PTR page = (ULONG_PTR)TlsGetValue(watch_slot);
        if (page)
        {
            DWORD old;
            VirtualProtect((void *)(page & ~(ULONG_PTR)1), 0x1000, PAGE_READONLY, &old);
            TlsSetValue(watch_slot, NULL);
            c->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT)
    {
        for (UINT i = 0; i < probe_count; i++)
        {
            struct probe *p = &probes[i];
            LONG n;
            if ((BYTE *)rec->ExceptionAddress != p->address)
                continue;
            n = InterlockedIncrement(&p->hits);
            if (n <= probe_max)
            {
                char caller[MAX_PATH + 32];
                ULONG_PTR ret = 0;
                if (!IsBadReadPtr((void *)(ULONG_PTR)c->Rsp, 8))
                    ret = *(ULONG_PTR *)(ULONG_PTR)c->Rsp;
                describe(ret, caller, sizeof(caller));
                rebox_log("probe %s hit %ld, thread %#lx: rcx %#llx rdx %#llx r8 %#llx r9 %#llx, rsi %#llx rdi %#llx "
                          "rax %#llx rbx %#llx, rsp %#llx, from %s",
                             p->name, n, GetCurrentThreadId(), (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
                             (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->Rsi,
                             (unsigned long long)c->Rdi, (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
                             (unsigned long long)c->Rsp, caller);
                ULONG_PTR base = dump_register == 1 ? c->Rdx : dump_register == 2 ? c->R8 : dump_register == 3 ? c->R9
                        : c->Rcx;
                static const char *const register_names[] = { "rcx", "rdx", "r8", "r9" };
                if (dump_qwords && base && !IsBadReadPtr((void *)base, dump_qwords * 8))
                {
                    char line[900], where[MAX_PATH + 32];
                    /* Eight qwords a line (the log's lines are short), each labelled with its offset. */
                    for (UINT first = 0; first < dump_qwords; first += 8)
                    {
                        int at = 0;
                        for (UINT q = first; q < dump_qwords && q < first + 8; q++)
                        {
                            ULONG_PTR v = ((ULONG_PTR *)base)[q];
                            HMODULE m;
                            if (v && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (const char *)v, &m))
                            {
                                describe(v, where, sizeof(where));
                                at += snprintf(line + at, sizeof(line) - at, " %s", where);
                            }
                            else
                                at += snprintf(line + at, sizeof(line) - at, " %#llx", (unsigned long long)v);
                        }
                        rebox_log("probe %s hit %ld: [%s+%#x]%s", p->name, n, register_names[dump_register], first * 8,
                                  line);
                    }
                }
                for (UINT m = 0; m < memory_range_count; m++)
                {
                    const UINT32 *at = (const UINT32 *)((BYTE *)GetModuleHandleA(NULL) + memory_ranges[m].rva);
                    for (ULONG_PTR off = 0; off < memory_ranges[m].bytes && !IsBadReadPtr(at, memory_ranges[m].bytes);
                         off += 32)
                    {
                        char line[160];
                        int len = 0;
                        for (ULONG_PTR k = off; k < off + 32 && k < memory_ranges[m].bytes; k += 4)
                            len += snprintf(line + len, sizeof(line) - len, " %08x", at[k / 4]);
                        rebox_log("probe %s hit %ld: exe+%#llx:%s", p->name, n,
                                  (unsigned long long)(memory_ranges[m].rva + off), line);
                    }
                }
            }
            /* Run the real instruction once, then rearm (single step). */
            write_byte(p->address, p->original);
            c->Rip = (ULONG_PTR)p->address;
            c->EFlags |= 0x100;
            TlsSetValue(rearm_slot, (void *)(ULONG_PTR)(i + 1));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    else if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && rearm_slot != TLS_OUT_OF_INDEXES)
    {
        ULONG_PTR pending = (ULONG_PTR)TlsGetValue(rearm_slot);
        if (pending)
        {
            write_byte(probes[pending - 1].address, 0xcc);
            TlsSetValue(rearm_slot, NULL);
            c->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void ensure_handler(void)
{
    static LONG installed;
    if (!InterlockedExchange(&installed, 1))
        AddVectoredExceptionHandler(1, on_probe);
}

static BOOL add_probe(BYTE *address, const char *name)
{
    struct probe *p;
    if (probe_count == MAX_PROBES || !address)
        return FALSE;
    if (rearm_slot == TLS_OUT_OF_INDEXES && (rearm_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES)
        return FALSE;
    ensure_handler();
    p = &probes[probe_count];
    p->address = address;
    p->original = *address;
    p->hits = 0;
    snprintf(p->name, sizeof(p->name), "%s", name);
    probe_count++;
    write_byte(address, 0xcc);
    rebox_log("probe %s armed (byte %02x), first %ld hits logged", p->name, p->original, probe_max);
    return TRUE;
}

/* Probes on a module that isn't loaded yet (MODULE!RVA: xgameruntime.dll comes in later, through
 * the Gaming Runtime's API sets) are armed when it loads, from ntdll's load notification. */
static struct pending { char module[64]; ULONG_PTR rva; } pendings[MAX_PROBES];
static UINT pending_count;

typedef struct
{
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    void *DllBase;
    ULONG SizeOfImage;
} PROBE_DLL_LOADED;

static void CALLBACK probe_dll_loaded(ULONG reason, const PROBE_DLL_LOADED *data, void *context)
{
    char name[64];
    if (reason != 1 || !data->BaseDllName)
        return;
    snprintf(name, sizeof(name), "%.*ls", (int)(data->BaseDllName->Length / 2), data->BaseDllName->Buffer);
    for (UINT i = 0; i < pending_count; i++)
        if (pendings[i].rva && !_stricmp(pendings[i].module, name))
        {
            char probe_name[64];
            snprintf(probe_name, sizeof(probe_name), "%s+%#llx", pendings[i].module, (unsigned long long)pendings[i].rva);
            add_probe((BYTE *)data->DllBase + pendings[i].rva, probe_name);
            pendings[i].rva = 0;
        }
}

/* For tests and tools: probe an address at run time, and read a probe's hit count. */
BOOL WINAPI ReboxProbeAdd(void *address, const char *name)
{
    return add_probe(address, name ? name : "probe");
}

LONG WINAPI ReboxProbeHits(const void *address)
{
    for (UINT i = 0; i < probe_count; i++)
        if (probes[i].address == address)
            return probes[i].hits;
    return -1;
}

void probe_install(void)
{
    char *spec = rebox_getenv("REBOX_PROBE"), *max = rebox_getenv("REBOX_PROBE_MAX"), *tok, *save = NULL;
    char *watch = rebox_getenv("REBOX_WRITE_WATCH");

    for (char *t = watch ? strtok_s(watch, ",", &save) : NULL; t && watch_count < ARRAYSIZE(watches);
         t = strtok_s(NULL, ",", &save))
    {
        char *colon = strchr(t, ':');
        struct watch *w = &watches[watch_count];
        w->start = strtoull(t, NULL, 16);
        w->length = colon ? strtoull(colon + 1, NULL, 16) : 8;
        if (!w->start || !w->length)
            continue;
        w->page_start = w->start & ~(ULONG_PTR)0xfff;
        w->page_end = (w->start + w->length + 0xfff) & ~(ULONG_PTR)0xfff;
        watch_count++;
    }
    free(watch);
    save = NULL;
    if (watch_count)
    {
        if ((watch_slot = TlsAlloc()) != TLS_OUT_OF_INDEXES)
        {
            ensure_handler();
            CloseHandle(CreateThread(NULL, 0, arm_watches, NULL, 0, NULL));
        }
    }

    if (max && atoi(max) > 0)
        probe_max = atoi(max);
    free(max);
    {
        char *dump = rebox_getenv("REBOX_PROBE_DUMP"), *memory = rebox_getenv("REBOX_PROBE_MEMORY"), *t, *ctx = NULL;
        if (dump && atoi(dump) > 0)
        {
            const char *colon = strchr(dump, ':');
            dump_qwords = min(atoi(dump), 64);
            dump_register = !colon ? 0 : !_stricmp(colon + 1, "rdx") ? 1 : !_stricmp(colon + 1, "r8") ? 2
                    : !_stricmp(colon + 1, "r9") ? 3 : 0;
        }
        free(dump);
        for (t = memory ? strtok_s(memory, ",", &ctx) : NULL; t && memory_range_count < ARRAYSIZE(memory_ranges);
             t = strtok_s(NULL, ",", &ctx))
        {
            char *colon = strchr(t, ':');
            memory_ranges[memory_range_count].rva = strtoull(t, NULL, 16);
            memory_ranges[memory_range_count].bytes = colon ? min(strtoull(colon + 1, NULL, 16), 0x400) : 4;
            memory_range_count++;
        }
        free(memory);
    }
    if (!spec || !*spec)
    {
        free(spec);
        return;
    }
    for (tok = strtok_s(spec, ",", &save); tok && probe_count < MAX_PROBES; tok = strtok_s(NULL, ",", &save))
    {
        char *bang = strchr(tok, '!');
        HMODULE module = bang ? (*bang = 0, GetModuleHandleA(tok)) : GetModuleHandleA(NULL);
        ULONG_PTR rva = strtoull(bang ? bang + 1 : tok, NULL, 16);
        char name[64];

        if (!rva)
        {
            rebox_log("probe %s: no address", tok);
            continue;
        }
        if (!module && bang && pending_count < MAX_PROBES)
        {
            snprintf(pendings[pending_count].module, sizeof(pendings[0].module), "%s", tok);
            pendings[pending_count++].rva = rva;
            continue;
        }
        if (!module)
        {
            rebox_log("probe %s: no such module", tok);
            continue;
        }
        snprintf(name, sizeof(name), "%s+%#llx", bang ? tok : "exe", (unsigned long long)rva);
        add_probe((BYTE *)module + rva, name);
    }
    free(spec);
    if (pending_count)
    {
        NTSTATUS (WINAPI *register_notification)(ULONG, void *, void *, void **) =
                (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
        static void *cookie;
        if (register_notification && !register_notification(0, probe_dll_loaded, NULL, &cookie))
            rebox_log("probes: %u wait for their modules to load", pending_count);
        else
            rebox_log("probes: no DLL load notifications; %u probes on modules not loaded yet are dropped",
                         pending_count);
    }
}
