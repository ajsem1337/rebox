#pragma once

/* Logging int3 probes on the title's code, from REBOX_PROBE (probe.c). */
void probe_install(void);
void probe_watch_add(ULONG_PTR start, ULONG_PTR length);
