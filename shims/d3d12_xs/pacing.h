#pragma once

#include <windows.h>

/* Frame clock behind device slots 78-80; see pacing.c. */
HRESULT pacing_set_interval(UINT interval_us, UINT period);
HRESULT pacing_schedule(UINT type, UINT_PTR offset_us, HANDLE event);
HRESULT pacing_wait(UINT type, UINT timeout_ms, UINT64 *token);
HRESULT pacing_frame_statistics(UINT64 token, UINT types, UINT *count, void *statistics);
void pacing_report(void);
