/* Fault reports for first runs of a title.
 *
 * Wine's own backtrace names addresses in the dumped exe only by number. This
 * vectored handler logs the first few faults (access violations, illegal or
 * privileged instructions, integer divide by zero, stack overflow) with the
 * module and RVA of the faulting instruction, its bytes, the registers and a
 * stack walk through the images' .pdata unwind data. An access violation also
 * says whether the address lies in the GPU window (docs/d3d12x.md), where a
 * unified-memory mistake would land.
 *
 * Reports are first-chance: the exception then continues to the title's own
 * handlers unchanged, so a fault the title expects and handles is reported
 * too ("first-chance" in the log). xmem.dll is imported by the exe and most
 * package DLLs, so this is installed before any title code runs.
 *
 * REBOX_CRASH_LOG=N sets how many faults are reported (default 8, 0 off).
 *
 * Hangs: d3d12_xs calls ReboxHeartbeat on every present. When none comes for
 * REBOX_HANG_DUMP seconds (default 60, 0 off), every thread's stack is
 * logged once, with its name, to see what a title is waiting for; after the
 * next present it can happen again (three times at most). */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "crash.h"

static LONG remaining;
static DWORD busy_slot = TLS_OUT_OF_INDEXES; /* set while this thread is reporting (__thread needs a DLL) */
static ULONG_PTR window_base, window_size;

static const char *code_name(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    default: return NULL;
    }
}

/* "module+0xrva", or the bare address outside any module. */
static void describe(ULONG_PTR addr, char *out, size_t len)
{
    HMODULE module;
    char path[MAX_PATH], *name;

    /* Wine maps address 0 to the main module; nothing lives in the first 64 KiB. */
    if (addr >= 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (const char *)addr, &module)
        && GetModuleFileNameA(module, path, sizeof(path)))
    {
        name = strrchr(path, '\\');
        snprintf(out, len, "%s+%#llx", name ? name + 1 : path, (unsigned long long)(addr - (ULONG_PTR)module));
    }
    else
        snprintf(out, len, "%#llx", (unsigned long long)addr);
}

static BOOL readable(ULONG_PTR addr, SIZE_T len)
{
    MEMORY_BASIC_INFORMATION mbi;

    if (!VirtualQuery((void *)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
        || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
        return FALSE;
    return addr + len <= (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
}

static void log_stack_frames(CONTEXT ctx, ULONG_PTR low, ULONG_PTR high, int frames)
{
    char where[MAX_PATH + 32];

    for (int i = 0; i < frames && ctx.Rip; i++)
    {
        DWORD64 image_base;
        RUNTIME_FUNCTION *fn;
        HMODULE module;

        describe(ctx.Rip, where, sizeof(where));
        rebox_log("  #%-2d %s", i, where);
        if (ctx.Rsp < low || ctx.Rsp >= high || !readable(ctx.Rsp, 8))
            break;
        /* Only addresses in a module are unwound: for others Wine's RtlLookupFunctionEntry consults
         * dynamically registered tables, and a stale return address (a UE5 title's render thread gave
         * 0xc040) made it fault inside the hang dump, which killed the title. */
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (const WCHAR *)(ULONG_PTR)ctx.Rip, &module))
            break;
        fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
        if (fn)
        {
            void *handler_data;
            DWORD64 frame;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &frame, NULL);
        }
        else
        {
            /* Leaf function: the return address is at the top of the stack. */
            ctx.Rip = *(DWORD64 *)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
}

static void log_stack(CONTEXT ctx)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    log_stack_frames(ctx, (ULONG_PTR)tib->StackLimit, (ULONG_PTR)tib->StackBase, 24);
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    const char *name = code_name(rec->ExceptionCode);
    char where[MAX_PATH + 32], bytes[3 * 16 + 1] = "";

    if (!name || TlsGetValue(busy_slot) || InterlockedDecrement(&remaining) < 0)
        return EXCEPTION_CONTINUE_SEARCH;
    TlsSetValue(busy_slot, (void *)1);

    describe(c->Rip, where, sizeof(where));
    rebox_log("xmem: first-chance %s (%#lx) at %s, thread %#lx", name, rec->ExceptionCode, where,
                 GetCurrentThreadId());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    {
        ULONG_PTR target = rec->ExceptionInformation[1];
        const char *op = rec->ExceptionInformation[0] == 1 ? "writing" : rec->ExceptionInformation[0] == 8
                       ? "executing" : "reading";

        rebox_log("  %s %#llx%s", op, (unsigned long long)target,
                     target - window_base < window_size ? " (inside the GPU window: unmapped or freed GPU memory?)" : "");
    }
    if (readable(c->Rip, 16))
        for (int i = 0; i < 16; i++)
            snprintf(bytes + 3 * i, 4, "%02x ", ((BYTE *)c->Rip)[i]);
    rebox_log("  code: %s", bytes);
    rebox_log("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", c->Rax, c->Rbx, c->Rcx, c->Rdx);
    rebox_log("  rsi %016llx rdi %016llx rbp %016llx rsp %016llx", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    rebox_log("  r8  %016llx r9  %016llx r10 %016llx r11 %016llx", c->R8, c->R9, c->R10, c->R11);
    rebox_log("  r12 %016llx r13 %016llx r14 %016llx r15 %016llx", c->R12, c->R13, c->R14, c->R15);
    if (rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        log_stack(*c);

    TlsSetValue(busy_slot, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* -------------------------------------------------------------------- hangs */

static volatile LONG64 heartbeats;

void WINAPI ReboxHeartbeat(void)
{
    InterlockedIncrement64(&heartbeats);
}

/* The stack of another thread of this process: its context while briefly
 * suspended, unwound live (a hung thread's stack doesn't move). */
static void log_thread(DWORD tid)
{
    typedef HRESULT (WINAPI *get_description_fn)(HANDLE, WCHAR **);
    static get_description_fn get_description;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, tid);
    CONTEXT ctx = { .ContextFlags = CONTEXT_FULL };
    WCHAR *name = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    BOOL ok;

    if (!thread)
        return;
    if (!get_description)
        get_description = (get_description_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    ok = SuspendThread(thread) != (DWORD)-1;
    if (ok)
    {
        ok = GetThreadContext(thread, &ctx);
        ResumeThread(thread);
    }
    if (get_description && FAILED(get_description(thread, &name)))
        name = NULL;
    rebox_log("xmem: thread %#lx%s%ls%s", tid, name && *name ? " \"" : "", name && *name ? name : L"",
                 name && *name ? "\"" : "");
    if (name)
        LocalFree(name);
    if (ok && VirtualQuery((void *)ctx.Rsp, &mbi, sizeof(mbi)))
        log_stack_frames(ctx, ctx.Rsp, (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize, 16);
    CloseHandle(thread);
}

static void dump_threads(DWORD seconds)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof(te) };
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();

    rebox_log("xmem: no frame presented for %lu s: every thread's stack follows (REBOX_HANG_DUMP)", seconds);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            log_thread(te.th32ThreadID);
    CloseHandle(snap);
    {
        /* d3d12_xs's fences and slot counts (proxy.c), when it is loaded. */
        HMODULE d3d12 = GetModuleHandleA("d3d12_xs.dll");
        void (WINAPI *report)(void) = d3d12 ? (void *)GetProcAddress(d3d12, "ReboxHangReport") : NULL;
        if (report)
            report();
    }
    rebox_log("xmem: end of thread stacks");
}

static DWORD WINAPI hang_watch(void *param)
{
    DWORD seconds = (DWORD)(ULONG_PTR)param, idle = 0;
    LONG64 last = -1;
    int dumps = 0;
    BOOL armed = TRUE;

    while (dumps < 3)
    {
        LONG64 now;
        Sleep(1000);
        if ((now = heartbeats) != last)
        {
            last = now;
            idle = 0;
            armed = TRUE;
        }
        else if (++idle >= seconds && armed)
        {
            dump_threads(seconds);
            dumps++;
            armed = FALSE;
        }
    }
    return 0;
}

void crash_report_install(ULONG_PTR gpu_window_base, ULONG_PTR gpu_window_size)
{
    char hang[16];
    DWORD hang_len = GetEnvironmentVariableA("REBOX_HANG_DUMP", hang, sizeof(hang));
    int hang_seconds = hang_len && hang_len < sizeof(hang) ? atoi(hang) : 60;
    char buf[16];
    DWORD n = GetEnvironmentVariableA("REBOX_CRASH_LOG", buf, sizeof(buf));

    remaining = n && n < sizeof(buf) ? atoi(buf) : 8;
    window_base = gpu_window_base;
    window_size = gpu_window_size;
    if (remaining > 0 && (busy_slot = TlsAlloc()) != TLS_OUT_OF_INDEXES)
        AddVectoredExceptionHandler(0, on_exception);
    if (hang_seconds > 0)
        CloseHandle(CreateThread(NULL, 0, hang_watch, (void *)(ULONG_PTR)hang_seconds, 0, NULL));
}
