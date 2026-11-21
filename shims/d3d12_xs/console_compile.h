#pragma once

#include <windows.h>

/* Deferred console shaders (docs/anyps5.md): the blob d3d12_xs hands vkd3d-proton in place of DXBC
 * for a console compute shader. vkd3d-proton (patch 0005) reads only magic, version and size, keeps
 * the blob with the PSO, and at each dispatch passes it back to ReboxConsoleCompile. */
#define REBOX_DEFERRED_MAGIC 0x44524353u /* "SCRD" */
struct rebox_deferred_shader
{
    UINT32 magic;
    UINT32 version;            /* 1 */
    UINT32 size;               /* of the whole blob */
    UINT32 stage;              /* VK_SHADER_STAGE_COMPUTE_BIT */
    UINT32 wave_size;          /* 32 or 64 */
    UINT32 workgroup_size[3];
    UINT32 rsrc1, rsrc2;       /* COMPUTE_PGM_RSRC1/2 */
    UINT32 code_offset;        /* bytes from the start of the blob, 4-aligned */
    UINT32 code_dwords;
    UINT64 code_address;       /* the code's guest address */
};

/* ReboxConsoleCompile's arguments; mirrored in patch 0005 (vkd3d_rebox_compile_in/out). */
struct rebox_compile_in
{
    const void *shader;
    UINT32 shader_size;
    UINT32 user_sgpr_count;
    const UINT32 *user_sgprs;
    UINT64 heap_cpu[2];        /* CPU handle of descriptor 0 of the bound CBV/SRV/UAV and sampler heaps */
    UINT32 increment[2];
    UINT32 bindless[7][2];     /* per AnyPS5 DescriptorKind: Vulkan set and binding, set ~0u if none */
};

struct rebox_compile_out
{
    const UINT32 *spirv;       /* cached for the process's lifetime */
    UINT32 spirv_words;
    UINT32 subgroup_size;
    UINT64 key[2];
    UINT32 push[40];
};

HRESULT WINAPI ReboxConsoleCompile(const struct rebox_compile_in *in, struct rebox_compile_out *out);
