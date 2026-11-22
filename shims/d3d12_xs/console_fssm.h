#pragma once

#include <windows.h>

/* A deferred console compute shader (SCRD, console_compile.h) built from a console DXBC container's
 * FSSM, or NULL if the container holds none. Free it with free(). See console_fssm.c. */
void *console_fssm_compute(const void *container, SIZE_T size, SIZE_T *blob_size);
