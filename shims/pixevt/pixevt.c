/* Replacement for the console's pixevt.dll (PIX events and captures), which titles built with
 * PIX instrumentation import. The export list is the console's (xbox-emulator's library map of
 * the console System32); the answers follow XValkyrie's: counters and captures succeed and do
 * nothing, and everything else returns 0. PIXEventsReplaceBlock returning 0 hands the title's
 * PIX macros no event block, so they write no events. */

#include <windows.h>

HRESULT WINAPI ConfigurePMCs(void) { return S_OK; }
HRESULT WINAPI ConfigureNBPMCs(void) { return S_OK; }
HRESULT WINAPI ConfigureL3PMCs(void) { return S_OK; }
HRESULT WINAPI PIXBeginCapture1(void) { return S_OK; }
HRESULT WINAPI PIXBeginCapture2(void) { return S_OK; }
HRESULT WINAPI PIXEndCapture(void) { return S_OK; }
void WINAPI PIXReportCounter(const WCHAR *name, float value) {}
UINT64 WINAPI PIXEventsReplaceBlock(void) { return 0; }
UINT64 WINAPI PIXGetCaptureState(void) { return 0; }
UINT64 WINAPI GetCaptureControlState(void) { return 0; }
UINT64 WINAPI RegisterForCaptureControl(void) { return 0; }
UINT64 WINAPI UnregisterForCaptureControl(void) { return 0; }
UINT64 WINAPI SetCaptureCallgraphState(void) { return 0; }
UINT64 WINAPI SetCaptureFunctionDetailsState(void) { return 0; }
UINT64 WINAPI SetCaptureFunctionSummaryState(void) { return 0; }
UINT64 WINAPI SetCaptureInstructionTraceState(void) { return 0; }
