#pragma once

#include <windows.h>
#include <d3d12.h>

/* 64-bit FNV-1a of the bytecode exactly as the title passes it to PSO
 * creation. tools/anyps5_cache.py computes the same hash. */
UINT64 shader_hash(const void *data, SIZE_T size);

enum shader_lookup
{
    SHADER_NONE, /* no bytecode */
    SHADER_PC,   /* PC bytecode, used as is */
    SHADER_HIT,  /* console bytecode, *out set to its translation */
    SHADER_MISS, /* console bytecode with no translation */
};

/* Looks up a translated replacement for console bytecode in
 * REBOX_SHADER_CACHE (<hash>.dxil) and fills *out on a hit. PC bytecode is
 * DXBC with SHEX/SHDR, or DXIL with LLVM bitcode. Console containers (DXBC
 * whose DXIL part is a NOBC stub around FSSM GFX10.3 code) are looked up like
 * raw bytes. Misses are appended to misses.txt in the cache dir, and dumped
 * to dump/ when REBOX_DUMP_SHADERS is set. */
enum shader_lookup shader_cache_substitute(const D3D12_SHADER_BYTECODE *in, D3D12_SHADER_BYTECODE *out,
                                           const char *stage);
