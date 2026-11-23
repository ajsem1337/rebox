/* chad.dll: the console's audio DSP (XDsp). Its interface is in no public header. Nobody runs one
 * here, but a title stops at a breakpoint when it can't connect (xcl, ivall/xcl, GPL-2.0-only,
 * crates/xcl/src/hle/acphal.rs, which connects for the same reason): so the DSP connects and takes
 * commands that do nothing. The first of each call is logged with its arguments.
 *
 * Signatures as the target title's call sites pass them (its own wrappers):
 *   HRESULT XDspConnectWithMaximumStreamLimit(void *, UINT32, UINT32, UINT32 max_streams (0x80),
 *                                             UINT32 (1), HANDLE *connection)
 *   HRESULT XDspActivate(HANDLE, ?, ?, ?)       S_OK: the caller hands out an id; else it uses -1
 *   HRESULT XDspSubmitCommand(HANDLE, const void *command)
 *   HRESULT XDspDeactivate(...), XDspDisconnect(HANDLE)
 * XDspConnect is the older name of the connect call. */

#include <windows.h>
#include <stdlib.h>

#include "../common/rebox_log.h"

/* What a connection handle points at. */
struct dsp
{
    UINT32 magic;
    UINT32 streams;
};

#define DSP_MAGIC 0x50534458 /* "XDSP" */

static BOOL valid(HANDLE connection)
{
    const struct dsp *d = connection;
    return d && !IsBadReadPtr(d, sizeof(*d)) && d->magic == DSP_MAGIC;
}

static HRESULT dsp_connect(UINT32 streams, HANDLE *connection)
{
    struct dsp *d;
    static LONG once;

    if (!connection)
        return E_INVALIDARG;
    if (!(d = calloc(1, sizeof(*d))))
        return E_OUTOFMEMORY;
    d->magic = DSP_MAGIC;
    d->streams = streams;
    *connection = d;
    if (!InterlockedExchange(&once, 1))
        rebox_log("chad: the audio DSP connected (%u streams): its commands are taken, nothing runs", streams);
    return S_OK;
}

HRESULT WINAPI XDspConnectWithMaximumStreamLimit(void *a, UINT32 b, UINT32 c, UINT32 max_streams, UINT32 e,
        HANDLE *connection)
{
    return dsp_connect(max_streams, connection);
}

HRESULT WINAPI XDspConnect(void *a, UINT32 b, UINT32 c, HANDLE *connection)
{
    return dsp_connect(0, connection);
}

HRESULT WINAPI XDspDisconnect(HANDLE connection)
{
    struct dsp *d = connection;

    if (!valid(connection))
        return E_INVALIDARG;
    d->magic = 0;
    free(d);
    return S_OK;
}

HRESULT WINAPI XDspActivate(HANDLE connection, UINT64 b, UINT64 c, UINT64 d)
{
    return valid(connection) ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI XDspDeactivate(HANDLE connection, UINT64 b)
{
    return valid(connection) ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI XDspSubmitCommand(HANDLE connection, const void *command)
{
    static LONG once;

    if (!valid(connection))
        return E_INVALIDARG;
    if (!InterlockedExchange(&once, 1))
        rebox_log("chad: first DSP command (%p): taken, not run", command);
    return S_OK;
}
