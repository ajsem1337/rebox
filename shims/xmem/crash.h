#pragma once

#include <windows.h>

/* Installs the first-chance fault reporter (crash.c), unless
 * REBOX_CRASH_LOG=0. */
void crash_report_install(ULONG_PTR gpu_window_base, ULONG_PTR gpu_window_size);
