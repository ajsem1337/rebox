#pragma once

#include <windows.h>

/* A deferred console compute shader (SCRD, console_compile.h) built from a console DXBC container's
 * FSSM, or NULL if the container holds none. Free it with free(). See console_fssm.c. */
void *console_fssm_compute(const void *container, SIZE_T size, SIZE_T *blob_size);

/* A deferred console vertex (stage 0x1) or pixel (0x10) shader (SCRD version 2, with
 * rebox_deferred_graphics after the header) from a console container's FSSM, or NULL. */
void *console_fssm_graphics(const void *container, SIZE_T size, UINT32 stage, SIZE_T *blob_size);

/* The size in dwords of the GFX10 instruction at code (left dwords there). */
UINT console_gfx10_size(const UINT32 *code, UINT left);
