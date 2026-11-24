#pragma once

#include <windows.h>

/* REBOX_BINK_TRACE=1: the title's calls into the package's Bink 2 DLL go through logging wrappers
 * (bink_trace.c). */
BOOL bink_trace_enabled(void);

/* A module loaded: if it is a Bink 2 DLL (Bink2*.dll) and tracing is on, its calls are traced. */
void bink_trace_module(HMODULE module);

/* The wrapper for one of the traced Bink DLL's exports, or NULL. */
void *bink_trace_replacement(void *target);
