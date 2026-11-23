/* Call probes on the title's own code (REBOX_PROBE).
 *
 * REBOX_PROBE=RVA[,RVA...] (hex, relative to the exe's base; MODULE!RVA for another module)
 * puts an int3 at each address. A hit logs the thread, the four argument registers, the return
 * address and the stack pointer, then the original byte is put back for one single-stepped
 * instruction and the int3 rearmed. Each probe logs its first REBOX_PROBE_MAX hits (default 20;
 * later hits are counted, not logged). Up to 64 probes. REBOX_PROBE_DUMP=N also logs the first N
 * qwords at rcx (an object's fields: a vtable or function pointer is shown as module+RVA). This is how to see what a title's own code
 * is called with without changing its file: xcl's XCL_PROBE does the same with hardware
 * breakpoints (a GDK title's controller path was found so). A hit on another thread while one byte
 * is restored is missed. */

#include <windows.h>
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
static UINT dump_qwords;
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

static LONG CALLBACK on_probe(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;

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
                rebox_log("probe %s hit %ld, thread %#lx: rcx %#llx rdx %#llx r8 %#llx r9 %#llx, rsp %#llx, from %s",
                             p->name, n, GetCurrentThreadId(), (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
                             (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->Rsp, caller);
                if (dump_qwords && c->Rcx && !IsBadReadPtr((void *)(ULONG_PTR)c->Rcx, dump_qwords * 8))
                {
                    char line[1024], where[MAX_PATH + 32];
                    int at = 0;
                    for (UINT q = 0; q < dump_qwords && at < 900; q++)
                    {
                        ULONG_PTR v = ((ULONG_PTR *)(ULONG_PTR)c->Rcx)[q];
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
                    rebox_log("probe %s hit %ld: [rcx]%s", p->name, n, line);
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

static BOOL add_probe(BYTE *address, const char *name)
{
    struct probe *p;
    if (probe_count == MAX_PROBES || !address)
        return FALSE;
    if (rearm_slot == TLS_OUT_OF_INDEXES && (rearm_slot = TlsAlloc()) == TLS_OUT_OF_INDEXES)
        return FALSE;
    {
        static LONG handler;
        if (!InterlockedExchange(&handler, 1))
            AddVectoredExceptionHandler(1, on_probe);
    }
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

    if (max && atoi(max) > 0)
        probe_max = atoi(max);
    free(max);
    {
        char *dump = rebox_getenv("REBOX_PROBE_DUMP");
        if (dump && atoi(dump) > 0)
            dump_qwords = min(atoi(dump), 32);
        free(dump);
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

        if (!module || !rva)
        {
            rebox_log("probe %s: no such module or address", tok);
            continue;
        }
        snprintf(name, sizeof(name), "%s+%#llx", bang ? tok : "exe", (unsigned long long)rva);
        add_probe((BYTE *)module + rva, name);
    }
    free(spec);
}
