/* Compiles deferred console shaders with AnyPS5 at dispatch (docs/anyps5.md).
 *
 * vkd3d-proton (patch 0005) calls ReboxConsoleCompile at each dispatch of a deferred console
 * PSO with the user SGPRs it packed (descriptor tables at shadow-heap title addresses), the bound
 * heaps and where each descriptor kind lives among its bindless sets. Here:
 *
 * 1. AnyPS5 (rebox_anyps5.dll) compiles the code against the shadow heaps as guest memory,
 *    through its in-memory cache: the code is parsed once, and each new set of descriptors it
 *    reads gives a variant.
 * 2. Each guest binding's descriptors are found in the bound heap's shadow (ReboxShadowFind's
 *    lookup), giving heap slots; host buffers (flattened SRT, BDA page table, fault buffer, GDS)
 *    get GPU memory.
 * 3. sa5_finalize rebinds the module onto the bindless sets at those slots, with the host buffers
 *    as BDA pointers in the push block. Finalized modules are cached by a key over AnyPS5's
 *    pipeline identity and the slots; vkd3d-proton caches pipelines by the same key.
 *
 * Nothing is freed: modules and host buffers live for the process. A failing shader logs once
 * and its dispatches are skipped. REBOX_DUMP_SPIRV=DIR writes each finalized module there. */

#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "../../anyps5/rebox_anyps5.h"
#include "console_compile.h"
#include "gpu_memory.h"
#include "shadow.h"

#define TITLE_BASE 0x10000000ull
#define MAX_REGIONS 64
#define MAX_BINDINGS 64
#define MAX_ELEMENTS 4096

enum role { ROLE_GDS = 3, ROLE_BDA_PAGETABLE = 4, ROLE_FAULT_BUFFER = 5, ROLE_FLATTENED_SRT = 6, ROLE_SHADER_DATA = 7 };

static struct
{
    INIT_ONCE once;
    int (*compile)(const struct sa5_compute_request *, struct sa5_result **, char *, size_t);
    const UINT32 *(*spirv)(const struct sa5_result *, UINT32 *);
    UINT32 (*binding_count)(const struct sa5_result *);
    int (*binding)(const struct sa5_result *, UINT32, struct sa5_binding *);
    const void *(*push_constants)(const struct sa5_result *, UINT32 *);
    UINT32 (*subgroup_size)(const struct sa5_result *);
    void (*identity)(const struct sa5_result *, UINT64 *);
    void (*result_free)(struct sa5_result *);
    int (*finalize)(const struct sa5_result *, const struct sa5_remap *, UINT32, const struct sa5_host_buffer *,
            UINT32, UINT32 **, UINT32 *, char *, size_t);
    UINT32 (*fault_buffer_bytes)(void);
    UINT32 (*gds_bytes)(void);
} sa5;

static BOOL CALLBACK load_sa5(INIT_ONCE *once, void *param, void **context)
{
    HMODULE m = LoadLibraryA("rebox_anyps5.dll");

    if (!m)
    {
        rebox_log("d3d12_xs: rebox_anyps5.dll not found; console shaders can't be compiled");
        return TRUE;
    }
#define GET(field, name) sa5.field = (void *)GetProcAddress(m, name)
    GET(compile, "sa5_compile_compute");
    GET(spirv, "sa5_result_spirv");
    GET(binding_count, "sa5_result_binding_count");
    GET(binding, "sa5_result_binding");
    GET(push_constants, "sa5_result_push_constants");
    GET(subgroup_size, "sa5_result_subgroup_size");
    GET(identity, "sa5_result_identity");
    GET(result_free, "sa5_result_free");
    GET(finalize, "sa5_finalize");
    GET(fault_buffer_bytes, "sa5_fault_buffer_bytes");
    GET(gds_bytes, "sa5_gds_bytes");
#undef GET
    if (!sa5.finalize || !sa5.identity || !sa5.gds_bytes)
    {
        rebox_log("d3d12_xs: rebox_anyps5.dll is too old (no sa5_finalize)");
        sa5.compile = NULL;
    }
    return TRUE;
}

/* 64-bit FNV-1a, seeded. */
static UINT64 hash_bytes(UINT64 h, const void *data, SIZE_T size)
{
    const BYTE *p = data;
    for (SIZE_T i = 0; i < size; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* ------------------------------------------------------------ host buffers */

static SRWLOCK buffers_lock = SRWLOCK_INIT;
static struct host_buffer { UINT64 hash, gpu; SIZE_T size; const void *cpu; struct host_buffer *next; } *buffers[256];

/* GPU memory holding these bytes; equal contents share it. */
static UINT64 host_buffer(const void *data, SIZE_T size)
{
    UINT64 h = hash_bytes(0xcbf29ce484222325ull, data, size), gpu = 0;
    struct host_buffer **bucket = &buffers[h & 255], *b;
    void *cpu;

    AcquireSRWLockExclusive(&buffers_lock);
    for (b = *bucket; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->cpu, data, size))
            break;
    if (b)
        gpu = b->gpu;
    else if (SUCCEEDED(ReboxGpuAlloc(size, 256, &cpu, &gpu)) && (b = malloc(sizeof(*b))))
    {
        memcpy(cpu, data, size);
        *b = (struct host_buffer){ h, gpu, size, cpu, *bucket };
        *bucket = b;
    }
    ReleaseSRWLockExclusive(&buffers_lock);
    return gpu;
}

/* AnyPS5's BDA page table (BdaAbi.hpp): a header, then ranges sorted by guest address. GPU memory
 * is identity-mapped; shadow heaps map their title addresses to their GPU copies. */
static UINT64 page_table(void)
{
    struct shadow_region regions[MAX_REGIONS];
    struct range { UINT64 begin, end, device; UINT32 permissions, reserved; } ranges[MAX_REGIONS + 2];
    BYTE table[16 + sizeof(ranges)];
    UINT n = shadow_regions(regions, MAX_REGIONS), count = 0;

    /* Insertion sort by title address. */
    for (UINT i = 1; i < n; i++)
        for (UINT j = i; j > 0 && regions[j].title_va < regions[j - 1].title_va; j--)
        {
            struct shadow_region t = regions[j];
            regions[j] = regions[j - 1];
            regions[j - 1] = t;
        }
    ranges[count++] = (struct range){ 0x10000, TITLE_BASE, 0x10000, 3, 0 };
    for (UINT i = 0; i < n; i++)
        if (regions[i].gpu_va)
            ranges[count++] = (struct range){ regions[i].title_va, regions[i].title_va + regions[i].size,
                                              regions[i].gpu_va, 3, 0 };
    ranges[count++] = (struct range){ 0x100000000ull, 1ull << 48, 0x100000000ull, 3, 0 };
    ((UINT32 *)table)[0] = 1;            /* version */
    ((UINT32 *)table)[1] = count;
    ((UINT32 *)table)[2] = sizeof(struct range);
    ((UINT32 *)table)[3] = 0;
    memcpy(table + 16, ranges, count * sizeof(struct range));
    return host_buffer(table, 16 + count * sizeof(struct range));
}

static UINT64 zero_buffer(UINT32 size)
{
    void *zeros = calloc(1, size);
    UINT64 gpu = zeros ? host_buffer(zeros, size) : 0;
    free(zeros);
    return gpu;
}

/* ------------------------------------------------------- finalized modules */

static SRWLOCK modules_lock = SRWLOCK_INIT;
static struct module { UINT64 key[2]; UINT32 *words, count; struct module *next; } *modules[256];

static struct module *find_module(const UINT64 key[2])
{
    struct module *m;

    AcquireSRWLockShared(&modules_lock);
    for (m = modules[key[0] & 255]; m && memcmp(m->key, key, sizeof(m->key)); m = m->next)
        ;
    ReleaseSRWLockShared(&modules_lock);
    return m;
}

static struct module *add_module(const UINT64 key[2], const UINT32 *words, UINT32 count)
{
    struct module *m = malloc(sizeof(*m)), *other;

    if (!m || !(m->words = malloc(count * sizeof(*words))))
    {
        free(m);
        return NULL;
    }
    memcpy(m->key, key, sizeof(m->key));
    memcpy(m->words, words, count * sizeof(*words));
    m->count = count;
    AcquireSRWLockExclusive(&modules_lock);
    for (other = modules[key[0] & 255]; other && memcmp(other->key, key, sizeof(other->key)); other = other->next)
        ;
    if (other)
    {
        free(m->words);
        free(m);
        m = other;
    }
    else
    {
        m->next = modules[key[0] & 255];
        modules[key[0] & 255] = m;
    }
    ReleaseSRWLockExclusive(&modules_lock);
    return m;
}

/* -------------------------------------------------------------- compiling */

/* Logs a shader's first failure only (keyed by its code). */
static void log_failure(const struct rebox_deferred_shader *h, const char *what, const char *detail)
{
    static SRWLOCK log_lock = SRWLOCK_INIT;
    static UINT64 logged[256];
    static UINT logged_count;
    UINT64 id = hash_bytes(0xcbf29ce484222325ull, (const BYTE *)h + h->code_offset, h->code_dwords * 4u);
    BOOL seen = FALSE;

    AcquireSRWLockExclusive(&log_lock);
    for (UINT i = 0; i < logged_count && !seen; i++)
        seen = logged[i] == id;
    if (!seen && logged_count < ARRAYSIZE(logged))
        logged[logged_count++] = id;
    ReleaseSRWLockExclusive(&log_lock);
    if (!seen)
        rebox_log("d3d12_xs: console shader %016llx: %s%s%s; its dispatches are skipped",
                     (unsigned long long)id, what, detail ? ": " : "", detail ? detail : "");
}

HRESULT WINAPI ReboxConsoleCompile(const struct rebox_compile_in *in, struct rebox_compile_out *out)
{
    const struct rebox_deferred_shader *h = in ? in->shader : NULL;
    struct sa5_region regions[MAX_REGIONS];
    struct shadow_region shadows[MAX_REGIONS];
    struct sa5_remap remaps[MAX_BINDINGS];
    struct sa5_host_buffer hosts[4];
    UINT64 host_va[4], key[2];
    UINT32 remap_count = 0, host_count = 0, n, index_count = 0, *words = NULL, word_count = 0;
    static UINT32 indices[MAX_ELEMENTS]; /* under compile_lock */
    static SRWLOCK compile_lock = SRWLOCK_INIT;
    struct sa5_compute_request req;
    struct sa5_result *result = NULL;
    const void *push;
    struct module *module;
    char error[1024] = "";
    HRESULT hr = E_FAIL;
    UINT32 push_size;

    if (!h || !out || in->shader_size < sizeof(*h) || h->magic != REBOX_DEFERRED_MAGIC || h->version != 1
            || h->size > in->shader_size || h->code_offset % 4 || h->code_offset > h->size
            || h->code_dwords > (h->size - h->code_offset) / 4)
        return E_INVALIDARG;
    InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
    if (!sa5.compile)
        return E_NOTIMPL;
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: console shaders: bindless sets (set/binding) ubo %d/%u ssbo %d/%u utexel %d/%u "
                         "stexel %d/%u sampled %d/%u storage %d/%u sampler %d/%u",
                         (int)in->bindless[0][0], in->bindless[0][1], (int)in->bindless[1][0], in->bindless[1][1],
                         (int)in->bindless[2][0], in->bindless[2][1], (int)in->bindless[3][0], in->bindless[3][1],
                         (int)in->bindless[4][0], in->bindless[4][1], (int)in->bindless[5][0], in->bindless[5][1],
                         (int)in->bindless[6][0], in->bindless[6][1]);
    }

    /* Guest memory: the shadow heaps at their title addresses, then the GPU memory chunks
     * (xmem's allocations, at their GPU VAs) that don't overlap them, so the SRT walk can follow
     * user-data pointers into constant data. Title-created resources aren't included. */
    n = shadow_regions(shadows, MAX_REGIONS);
    for (UINT i = 0; i < n; i++)
        regions[i] = (struct sa5_region){ shadows[i].title_va, shadows[i].data, shadows[i].size };
    {
        struct gpu_memory_region chunks[MAX_REGIONS];
        UINT count = gpu_memory_regions(chunks, MAX_REGIONS - n), shadow_count = n;
        for (UINT i = 0; i < count; i++)
        {
            BOOL overlaps = FALSE;
            for (UINT j = 0; j < shadow_count && !overlaps; j++)
                overlaps = chunks[i].gpu < regions[j].address + regions[j].size
                        && regions[j].address < chunks[i].gpu + chunks[i].size;
            if (!overlaps && chunks[i].size)
                regions[n++] = (struct sa5_region){ chunks[i].gpu, chunks[i].cpu, chunks[i].size };
        }
    }
    memset(&req, 0, sizeof(req));
    req.code = (const UINT32 *)((const BYTE *)h + h->code_offset);
    req.code_dwords = h->code_dwords;
    req.code_address = h->code_address;
    req.wave = h->wave_size;
    req.user_data = in->user_sgprs;
    req.user_data_count = min(in->user_sgpr_count, (h->rsrc2 >> 1) & 0x1f);
    memcpy(req.threads, h->workgroup_size, sizeof(req.threads));
    req.rsrc1 = h->rsrc1;
    req.rsrc2 = h->rsrc2;
    req.regions = regions;
    req.region_count = n;
    req.use_cache = 1;

    /* One compile at a time: AnyPS5's caches are thread-safe, but this keeps the static index
     * table simple and compiles are rare once warm. */
    AcquireSRWLockExclusive(&compile_lock);
    if (sa5.compile(&req, &result, error, sizeof(error)))
    {
        log_failure(h, "AnyPS5 failed", error);
        goto done;
    }

    sa5.identity(result, key);
    key[1] ^= hash_bytes(0xcbf29ce484222325ull, in->bindless, sizeof(in->bindless));
    for (UINT32 i = 0; i < sa5.binding_count(result); i++)
    {
        struct sa5_binding b;
        sa5.binding(result, i, &b);
        if (b.role <= 2)
        {
            BOOL sampler = b.kind == 6;
            UINT32 per = b.count ? b.word_count / b.count : 0;
            if (remap_count == MAX_BINDINGS || index_count + b.count > MAX_ELEMENTS || b.kind > 6
                    || in->bindless[b.kind][0] == ~0u || !in->heap_cpu[sampler] || (per != 4 && per != 8))
            {
                log_failure(h, "a guest binding has no bindless set or heap here", NULL);
                goto done;
            }
            for (UINT32 e = 0; e < b.count; e++)
                if (!shadow_find_index((SIZE_T)in->heap_cpu[sampler], b.words + e * per, per, sampler,
                                       &indices[index_count + e]))
                {
                    log_failure(h, "a descriptor it binds isn't in the bound heap", NULL);
                    goto done;
                }
            remaps[remap_count++] = (struct sa5_remap){ b.set, b.binding, b.kind, in->bindless[b.kind][0],
                                                        in->bindless[b.kind][1], b.count, &indices[index_count] };
            key[0] = hash_bytes(key[0], &b.binding, sizeof(b.binding));
            key[0] = hash_bytes(key[0], &indices[index_count], b.count * sizeof(UINT32));
            index_count += b.count;
        }
        else
        {
            UINT64 va = 0;
            UINT32 bytes = b.word_count * 4;
            if (host_count == ARRAYSIZE(hosts))
            {
                log_failure(h, "more than 4 host buffers", NULL);
                goto done;
            }
            switch (b.role)
            {
            case ROLE_FLATTENED_SRT:
            case ROLE_SHADER_DATA:
                va = b.word_count ? host_buffer(b.words, bytes) : zero_buffer(bytes = 16);
                break;
            case ROLE_FAULT_BUFFER:
                va = zero_buffer(bytes = sa5.fault_buffer_bytes());
                break;
            case ROLE_GDS:
                va = zero_buffer(bytes = sa5.gds_bytes());
                break;
            case ROLE_BDA_PAGETABLE:
                va = page_table();
                bytes = 0;
                break;
            default:
            {
                char role[32];
                snprintf(role, sizeof(role), "role %u", b.role);
                log_failure(h, "unsupported host binding", role);
                goto done;
            }
            }
            if (!va)
            {
                log_failure(h, "no GPU memory for a host buffer", NULL);
                goto done;
            }
            host_va[host_count] = va;
            hosts[host_count] = (struct sa5_host_buffer){ b.set, b.binding, host_count, bytes / 4 };
            key[0] = hash_bytes(key[0], &hosts[host_count], sizeof(hosts[host_count]));
            host_count++;
        }
    }

    if (!(module = find_module(key)))
    {
        if (sa5.finalize(result, remaps, remap_count, hosts, host_count, &words, &word_count, error, sizeof(error)))
        {
            log_failure(h, "fitting it to vkd3d-proton's bindings failed", error);
            goto done;
        }
        module = add_module(key, words, word_count);
        if (!module)
            goto done;
        if (getenv("REBOX_DUMP_SPIRV"))
        {
            char path[MAX_PATH];
            FILE *f;
            snprintf(path, sizeof(path), "%s\\%016llx%016llx.spv", getenv("REBOX_DUMP_SPIRV"),
                     (unsigned long long)key[1], (unsigned long long)key[0]);
            if ((f = fopen(path, "wb")))
            {
                fwrite(module->words, 4, module->count, f);
                fclose(f);
            }
        }
        rebox_log("d3d12_xs: console shader %016llx: compiled a variant (%u SPIR-V words, %u guest bindings, "
                     "%u host buffers)", (unsigned long long)hash_bytes(0xcbf29ce484222325ull, req.code,
                     req.code_dwords * 4u), module->count, remap_count, host_count);
    }

    memset(out, 0, sizeof(*out));
    out->spirv = module->words;
    out->spirv_words = module->count;
    out->subgroup_size = sa5.subgroup_size(result);
    memcpy(out->key, key, sizeof(key));
    push = sa5.push_constants(result, &push_size);
    memcpy(out->push, push, min(push_size, 128u));
    for (UINT32 i = 0; i < host_count; i++)
        memcpy(&out->push[32 + 2 * i], &host_va[i], sizeof(UINT64));
    hr = S_OK;

done:
    ReleaseSRWLockExclusive(&compile_lock);
    if (words)
    {
        void (*free_words)(UINT32 *) = (void *)GetProcAddress(GetModuleHandleA("rebox_anyps5.dll"), "sa5_free_words");
        if (free_words)
            free_words(words);
    }
    if (result)
        sa5.result_free(result);
    return hr;
}
