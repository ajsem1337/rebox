#pragma once

#include <windows.h>
#include <d3d12.h>

/* 64-bit FNV-1a of the bytecode exactly as the title passes it to PSO
 * creation. tools/anyps5_cache.py computes the same hash. */
UINT64 shader_hash(const void *data, SIZE_T size);

/* Looks up a translated replacement for console bytecode in
 * REBOX_SHADER_CACHE (<hash>.dxil). Returns TRUE and fills *out on a hit.
 * PC bytecode (DXBC with SHEX/SHDR, or DXIL with LLVM bitcode), empty input
 * and misses return FALSE. Console containers (DXBC whose DXIL part is a
 * NOBC stub around FSSM GFX10.3 code) are looked up like raw bytes. Misses are
 * appended to misses.txt in the cache dir, and dumped to dump/ when
 * REBOX_DUMP_SHADERS is set. */
BOOL shader_cache_substitute(const D3D12_SHADER_BYTECODE *in, D3D12_SHADER_BYTECODE *out,
                             const char *stage);
