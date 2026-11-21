#include "rebox_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static INIT_ONCE log_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION log_lock;
static FILE *log_file;
static BOOL stub_fatal;

char *rebox_getenv(const char *name)
{
    DWORD len = GetEnvironmentVariableA(name, NULL, 0);
    char *buf;

    if (!len || !(buf = malloc(len)))
        return NULL;
    if (!GetEnvironmentVariableA(name, buf, len))
    {
        free(buf);
        return NULL;
    }
    return buf;
}

static BOOL CALLBACK log_init(INIT_ONCE *once, void *param, void **ctx)
{
    char *path, *fatal;

    InitializeCriticalSection(&log_lock);
    if ((path = rebox_getenv("REBOX_LOG")))
    {
        log_file = fopen(path, "a");
        free(path);
    }
    if ((fatal = rebox_getenv("REBOX_STUB_FATAL")))
    {
        stub_fatal = fatal[0] && fatal[0] != '0';
        free(fatal);
    }
    return TRUE;
}

void rebox_log(const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    int len;

    InitOnceExecuteOnce(&log_once, log_init, NULL, NULL);

    len = snprintf(buf, sizeof(buf), "[rebox %04lx] ", GetCurrentThreadId());
    va_start(args, fmt);
    vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
    va_end(args);

    EnterCriticalSection(&log_lock);
    fprintf(stderr, "%s\n", buf);
    if (log_file)
    {
        fprintf(log_file, "%s\n", buf);
        fflush(log_file);
    }
    LeaveCriticalSection(&log_lock);
}

void rebox_unimpl(const char *dll, const char *name, LONG *once)
{
    InitOnceExecuteOnce(&log_once, log_init, NULL, NULL);

    if (stub_fatal)
    {
        rebox_log("FATAL: unimplemented %s!%s", dll, name);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    if (!InterlockedExchange(once, 1))
        rebox_log("unimplemented %s!%s (returning E_NOTIMPL)", dll, name);
}
