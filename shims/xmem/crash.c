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
 * REBOX_CRASH_LOG=N sets how many faults are reported (default 8, 0 off). */

#include <windows.h>
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

static void log_stack(CONTEXT ctx)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    ULONG_PTR low = (ULONG_PTR)tib->StackLimit, high = (ULONG_PTR)tib->StackBase;
    char where[MAX_PATH + 32];

    for (int i = 0; i < 24 && ctx.Rip; i++)
    {
        DWORD64 image_base;
        RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);

        describe(ctx.Rip, where, sizeof(where));
        rebox_log("  #%-2d %s", i, where);
        if (ctx.Rsp < low || ctx.Rsp >= high)
            break;
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

void crash_report_install(ULONG_PTR gpu_window_base, ULONG_PTR gpu_window_size)
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("REBOX_CRASH_LOG", buf, sizeof(buf));

    remaining = n && n < sizeof(buf) ? atoi(buf) : 8;
    window_base = gpu_window_base;
    window_size = gpu_window_size;
    if (remaining > 0 && (busy_slot = TlsAlloc()) != TLS_OUT_OF_INDEXES)
        AddVectoredExceptionHandler(0, on_exception);
}
