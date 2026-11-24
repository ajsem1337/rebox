#pragma once

#include <windows.h>

/* Installs the first-chance fault reporter (crash.c), unless
 * REBOX_CRASH_LOG=0. */
void crash_report_install(ULONG_PTR gpu_window_base, ULONG_PTR gpu_window_size);

/* What memory a register points at, for the fault report (xmem.c). */
void xmem_describe_address(const char *name, ULONG_PTR address);
