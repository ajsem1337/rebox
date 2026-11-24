#pragma once

/* Points imports bound to Wine's abort-on-call stubs at logging E_NOTIMPL stubs (import_fixup.c). */
void import_fixup_run(void);

/* VirtualAlloc with new reservations below 2^46, the console's address width (xmem.c). */
void *WINAPI xmem_low_virtual_alloc(void *address, SIZE_T size, DWORD type, DWORD protect);

/* REBOX_PHYSICAL_TRACE: logs an operation on [address, address + size) that touches the traced
 * physical page through any of its addresses (import_fixup.c). */
void xmem_note_page_op(const char *what, ULONG_PTR address, ULONG_PTR size);
