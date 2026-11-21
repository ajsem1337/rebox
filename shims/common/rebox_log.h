#pragma once

#include <windows.h>

/* Logging shared by every shim DLL. Output goes to stderr (which Wine passes
 * through to the terminal) and, if REBOX_LOG names a file, is appended
 * there too. */
void rebox_log(const char *fmt, ...) __attribute__((format(gnu_printf, 1, 2)));

/* Called by generated stubs. Logs the first call of each stub; if
 * REBOX_STUB_FATAL is set, terminates the process instead so the first
 * unimplemented entry point the title actually reaches is obvious. */
void rebox_unimpl(const char *dll, const char *name, LONG *once);

/* Returns a heap copy of an environment variable, or NULL. */
char *rebox_getenv(const char *name);

#define REBOX_E_NOTIMPL ((ULONG_PTR)0x80004001)

/* Generated stubs ignore their arguments (safe on x64: the caller owns the
 * stack) and return E_NOTIMPL. */
#define REBOX_STUB(dll, id, name)                         \
    ULONG_PTR WINAPI stub_##id(void)                         \
    {                                                        \
        static LONG once;                                    \
        rebox_unimpl(#dll, name, &once);                  \
        return REBOX_E_NOTIMPL;                           \
    }
