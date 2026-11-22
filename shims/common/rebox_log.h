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
 * stack). REBOX_STUB returns E_NOTIMPL, for HRESULT functions and ones
 * whose prototype is unknown; REBOX_STUB_ZERO returns 0 (FALSE, NULL)
 * with ERROR_CALL_NOT_IMPLEMENTED, for functions the headers declare
 * returning anything else; REBOX_STUB_VOID returns nothing. */
#define REBOX_STUB(dll, id, name)                         \
    ULONG_PTR WINAPI stub_##id(void)                         \
    {                                                        \
        static LONG once;                                    \
        rebox_unimpl(#dll, name, &once);                  \
        return REBOX_E_NOTIMPL;                           \
    }
#define REBOX_STUB_ZERO(dll, id, name)                    \
    ULONG_PTR WINAPI stub_##id(void)                         \
    {                                                        \
        static LONG once;                                    \
        rebox_unimpl(#dll, name, &once);                  \
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);            \
        return 0;                                            \
    }
#define REBOX_STUB_VOID(dll, id, name)                    \
    void WINAPI stub_##id(void)                              \
    {                                                        \
        static LONG once;                                    \
        rebox_unimpl(#dll, name, &once);                  \
    }
