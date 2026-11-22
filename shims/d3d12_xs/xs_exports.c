/* The rest of the console d3d12_xs.dll's export table, as listed by the real DLL in a title's
 * system partition (EmbeddedXvd): titles import its interface IDs as data (a UE5 title imports
 * IID_ID3D12Device), so they are exported as GUIDs, not as stub functions. The public ones are
 * instantiated by the headers (initguid.h in d3d12_xs.c and present.c); the console-only ones are
 * defined here, with the values the real DLL exports. */

#include <windows.h>
#include <initguid.h>

#include "../common/rebox_log.h"

DEFINE_GUID(IID_ID3D12XboxDmaCommandList, 0x30bc77b9, 0x8379, 0x4b25, 0x95, 0xde, 0xb8, 0x58, 0x8c, 0x42, 0x27, 0x5d);
DEFINE_GUID(IID_IGraphicsUnknown, 0xaceeea63, 0xe0a9, 0x4a1c, 0xbb, 0xec, 0x71, 0xb2, 0xf4, 0x85, 0xf7, 0x58);
/* d3d12sdklayers.h's and d3dcommon.h's, which no shim instantiates. */
DEFINE_GUID(IID_ID3D12Debug, 0x344488b7, 0x6846, 0x474b, 0xb9, 0x89, 0xf0, 0x27, 0x44, 0x82, 0x45, 0xe0);
DEFINE_GUID(WKPDID_D3DDebugObjectNameW, 0x4cca5fd8, 0x921f, 0x42c8, 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41);

static volatile LONG debug_flags;

/* Debug-layer and profiling options for the process: kept, so Get returns what Set was given. */
HRESULT WINAPI D3D12XboxSetProcessDebugFlags(UINT flags)
{
    static LONG once;
    if (!InterlockedExchange(&once, 1))
        rebox_log("d3d12_xs: D3D12XboxSetProcessDebugFlags(%#x) noted, not applied", flags);
    InterlockedExchange(&debug_flags, (LONG)flags);
    return S_OK;
}

UINT WINAPI D3D12XboxGetProcessDebugFlags(void)
{
    return (UINT)debug_flags;
}

/* The console's CPU and GPU caches aren't coherent for some memory types; here they are. */
void WINAPI D3DFlushEntireCpuCache(void)
{
}
