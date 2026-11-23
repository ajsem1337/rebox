/* chad.dll: the console's audio DSP (XDsp). Its interface is in no public header, and nobody runs
 * one here, so connecting reports the DSP isn't there (as xcl and xvalkyrie do) and a title's audio
 * engine takes its path without it. Pretending it connected would let the title submit DSP work
 * nothing completes. Every call is logged once with its arguments, to show how a title uses it.
 *
 * Signatures as the target title's call sites pass them (its own wrappers):
 *   HRESULT XDspConnectWithMaximumStreamLimit(void *, UINT32, UINT32, UINT32 max_streams (0x80),
 *                                             UINT32 (1), HANDLE *connection)  wrapper: S_OK or fails
 *   HRESULT XDspActivate(HANDLE, ?, ?, ?)       S_OK: the caller hands out an id; else it uses -1
 *   HRESULT XDspSubmitCommand(HANDLE, const void *command)
 *   HRESULT XDspDeactivate(...), XDspDisconnect(HANDLE)  imported, not called directly
 * XDspConnect is the older name of the connect call. */

#include <windows.h>

#include "../common/rebox_log.h"

#define XDSP_NOT_PRESENT E_NOTIMPL

static void note(volatile LONG *once, const char *what)
{
    if (!InterlockedExchange(once, 1))
        rebox_log("chad: %s: the audio DSP isn't there", what);
}

HRESULT WINAPI XDspConnectWithMaximumStreamLimit(void *a, UINT32 b, UINT32 c, UINT32 max_streams, UINT32 e,
        HANDLE *connection)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        rebox_log("chad: XDspConnectWithMaximumStreamLimit(%p, %#x, %#x, %u streams, %#x, %p): the audio DSP "
                  "isn't there", a, b, c, max_streams, e, (void *)connection);
    if (connection)
        *connection = NULL;
    return XDSP_NOT_PRESENT;
}

HRESULT WINAPI XDspConnect(void *a, UINT32 b, UINT32 c, HANDLE *connection)
{
    static LONG once;
    note(&once, "XDspConnect");
    if (connection)
        *connection = NULL;
    return XDSP_NOT_PRESENT;
}

HRESULT WINAPI XDspDisconnect(HANDLE connection)
{
    return S_OK;
}

HRESULT WINAPI XDspActivate(HANDLE connection, UINT64 b, UINT64 c, UINT64 d)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        rebox_log("chad: XDspActivate(%p, %#llx, %#llx, %#llx): the audio DSP isn't there", connection,
                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
    return XDSP_NOT_PRESENT;
}

HRESULT WINAPI XDspDeactivate(HANDLE connection, UINT64 b)
{
    return S_OK;
}

HRESULT WINAPI XDspSubmitCommand(HANDLE connection, const void *command)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        rebox_log("chad: XDspSubmitCommand(%p, %p): the audio DSP isn't there", connection, command);
    return XDSP_NOT_PRESENT;
}
