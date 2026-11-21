/* rebox_anyps5.dll: AnyPS5's RDNA -> SPIR-V recompiler behind a C API for
 * d3d12_xs (docs/anyps5.md). GPL-2.0-only, as AnyPS5. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef REBOX_ANYPS5_BUILD
#define SA5_API __declspec(dllexport)
#else
#define SA5_API __declspec(dllimport)
#endif

/* Guest memory the recompiler may read while resolving descriptors: the
 * descriptor tables and anything they point at, at their guest addresses. */
struct sa5_region
{
    uint64_t address;
    const void *data;
    size_t size;
};

struct sa5_compute_request
{
    const uint32_t *code;       /* RDNA code, starting at the first instruction */
    uint32_t code_dwords;
    uint64_t code_address;      /* guest address of code[0] */
    uint32_t wave;              /* 32 or 64 */
    const uint32_t *user_data;  /* user SGPRs s0.. */
    uint32_t user_data_count;
    uint32_t threads[3];        /* thread-group size */
    uint32_t rsrc1, rsrc2;      /* COMPUTE_PGM_RSRC1/2: float mode, user SGPRs, TGID enables, LDS */
    const struct sa5_region *regions;
    uint32_t region_count;
    uint32_t use_cache;         /* nonzero: AnyPS5's in-memory cache (parse once per code, compile per variant) */
};

/* One resource the compiled shader binds, as AnyPS5 resolved it. */
struct sa5_binding
{
    uint32_t kind;              /* ShaderRecompiler::DescriptorKind */
    uint32_t set, binding, count;
    uint32_t word_count;        /* guest descriptor words: 4 (V#, S#) or 8 (T#) per element */
    const uint32_t *words;
    int32_t image_shape;        /* ShaderRecompiler::DescriptorImageShape, or -1 */
    uint32_t role;              /* ShaderRecompiler::DescriptorRole: 0-2 guest buffers/images/samplers */
};

struct sa5_result;

/* Returns 0 on success; on failure, the recompiler's message in error. */
SA5_API int sa5_compile_compute(const struct sa5_compute_request *request, struct sa5_result **result,
                                char *error, size_t error_size);
SA5_API const uint32_t *sa5_result_spirv(const struct sa5_result *result, uint32_t *word_count);
SA5_API uint32_t sa5_result_binding_count(const struct sa5_result *result);
SA5_API int sa5_result_binding(const struct sa5_result *result, uint32_t index, struct sa5_binding *binding);
SA5_API const void *sa5_result_push_constants(const struct sa5_result *result, uint32_t *size);
SA5_API uint32_t sa5_result_subgroup_size(const struct sa5_result *result);
/* AnyPS5's identity for the result's pipeline: SPIR-V hash, specialization and binding layout. */
SA5_API void sa5_result_identity(const struct sa5_result *result, uint64_t identity[2]);
/* Sizes of the host buffers AnyPS5's driver fills itself (roles FaultBuffer and Gds). */
SA5_API uint32_t sa5_fault_buffer_bytes(void);
SA5_API uint32_t sa5_gds_bytes(void);
SA5_API void sa5_result_free(struct sa5_result *result);

/* Fitting a result to vkd3d-proton's bindings (spirv_remap.cpp). Specialization constants are
 * frozen to the result's values. Each remap moves a guest descriptor array at (set, binding) to
 * (new_set, new_binding) as a runtime array, element i becoming element heap_indices[i] there; kind
 * is the binding's DescriptorKind. Each host buffer (a role AnyPS5's driver fills itself) becomes a
 * buffer-device-address pointer read from the push block: 64-bit address push_slot (0-3) at byte
 * 128 + 8 * push_slot. array_length is what OpArrayLength on it returns (its u32 count). */
struct sa5_remap
{
    uint32_t set, binding, kind;
    uint32_t new_set, new_binding;
    uint32_t count;
    const uint32_t *heap_indices;
};

struct sa5_host_buffer
{
    uint32_t set, binding;
    uint32_t push_slot;
    uint32_t array_length;
};

/* Returns 0 and the validated module in *out (free with sa5_free_words), or -1 and a message. */
SA5_API int sa5_finalize(const struct sa5_result *result, const struct sa5_remap *remaps, uint32_t remap_count,
                         const struct sa5_host_buffer *buffers, uint32_t buffer_count, uint32_t **out,
                         uint32_t *out_count, char *error, size_t error_size);
SA5_API void sa5_free_words(uint32_t *words);

#ifdef __cplusplus
}
#endif
