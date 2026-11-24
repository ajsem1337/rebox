#pragma once

#include <windows.h>

/* acphal.c: the memory an APU address stands for, or NULL. */
void *WINAPI ApuMapApuAddress(UINT32 apu);

/* xma.c: the XMA decoder. xma_contexts hands it the context array; xma_command runs an XMA
 * command and says whether it was one. */
void xma_contexts(BYTE *array, UINT count);
BOOL xma_command(UINT32 type, const UINT32 *data);
