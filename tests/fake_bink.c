/* A stand-in for a title's Bink 2 DLL (build/Bink2Smoke.dll), for the smoke test of
 * REBOX_BINK_TRACE: the calls xmem wraps, with just enough behaviour to see the wrappers pass
 * arguments and results through. A handle's head is the SDK's BINK (width, height, frames, frame
 * number, ...); +0x11c is the paused flag the trace reads. */

#include <windows.h>
#include <string.h>

static LONG calls;

__declspec(dllexport) UINT_PTR BinkOpenWithOptions(const char *name, const void *options, UINT32 flags)
{
    UINT32 *h;
    InterlockedIncrement(&calls);
    if (!name || !strcmp(name, "missing.bk2") || !(h = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x800)))
        return 0;
    h[0] = 64, h[1] = 32, h[2] = 10, h[3] = 1, h[5] = 30, h[6] = 1, h[8] = flags;
    return (UINT_PTR)h;
}

__declspec(dllexport) void BinkClose(UINT32 *h)
{
    InterlockedIncrement(&calls);
    HeapFree(GetProcessHeap(), 0, h);
}

/* "Wait" on the first call, "go" after. */
__declspec(dllexport) INT32 BinkWait(UINT32 *h)
{
    InterlockedIncrement(&calls);
    return h[3] == 1 && !h[0x100];
}

__declspec(dllexport) INT32 BinkDoFrame(UINT32 *h)
{
    InterlockedIncrement(&calls);
    h[0x100] = 1;
    return 0;
}

__declspec(dllexport) void BinkNextFrame(UINT32 *h)
{
    InterlockedIncrement(&calls);
    h[4] = h[3]++;
}

__declspec(dllexport) INT32 BinkPause(UINT32 *h, INT32 pause)
{
    InterlockedIncrement(&calls);
    h[0x11c / 4] = pause;
    return pause;
}

__declspec(dllexport) void BinkGoto(UINT32 *h, UINT32 frame, INT32 flags)
{
    InterlockedIncrement(&calls);
    h[3] = frame;
}

__declspec(dllexport) const char *BinkGetError(void)
{
    return "fake: no such movie";
}

__declspec(dllexport) LONG FakeBinkCalls(void)
{
    return calls;
}
