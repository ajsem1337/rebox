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

enum role { ROLE_GDS = 3, ROLE_BDA_PAGETABLE = 4, ROLE_FAULT_BUFFER = 5, ROLE_FLATTENED_SRT = 6, ROLE_SHADER_DATA = 7,
            ROLE_IMAGE_TABLE = 8, ROLE_SAMPLER_TABLE = 9, ROLE_IMAGE_TABLE_MAP = 10, ROLE_IMAGE_STORAGE_TABLE = 11 };

/* Roles whose elements are guest descriptors bound like guest images and samplers: AnyPS5's
 * tables for T#s and S#s that a shader indexes at run time (from a table in memory). */
static BOOL guest_descriptor_role(UINT32 role)
{
    return role <= 2 || role == ROLE_IMAGE_TABLE || role == ROLE_SAMPLER_TABLE || role == ROLE_IMAGE_STORAGE_TABLE;
}

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
    int (*analyse)(const UINT32 *, UINT32, UINT32, struct sa5_layout *, struct sa5_slot *, UINT32, char *, size_t);
    void (*result_free)(struct sa5_result *);
    int (*finalize)(const struct sa5_result *, const struct sa5_remap *, UINT32, const struct sa5_host_buffer *,
            UINT32, UINT32 **, UINT32 *, char *, size_t);
    UINT32 (*fault_buffer_bytes)(void);
    UINT32 (*gds_bytes)(void);
} sa5;

/* AnyPS5's settings, made before anything loads rebox_anyps5.dll (it reads some of them once,
 * into statics): from DllMain. */
static void set_anyps5_environment(void)
{
    const char *local = getenv("LOCALAPPDATA");

    /* AnyPS5 keeps compiled variants on disk, by default in shader_cache next to the exe: the
     * game's directory. Unless ANYPS5_SHADER_CACHE_DIR or ANYPS5_NO_SHADER_CACHE says otherwise,
     * use %LOCALAPPDATA%\rebox\anyps5-cache. Both DLLs use msvcrt, so _putenv reaches
     * AnyPS5's getenv. */
    if (!getenv("ANYPS5_SHADER_CACHE_DIR") && !getenv("ANYPS5_NO_SHADER_CACHE") && local)
    {
        char dir[MAX_PATH], setting[MAX_PATH + 32];
        snprintf(dir, sizeof(dir), "%s\\rebox", local);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof(dir), "%s\\rebox\\anyps5-cache", local);
        CreateDirectoryA(dir, NULL);
        snprintf(setting, sizeof(setting), "ANYPS5_SHADER_CACHE_DIR=%s", dir);
        _putenv(setting);
        rebox_log("d3d12_xs: AnyPS5 shader cache in %s", dir);
    }
    /* patches/anyps5/0004: when a descriptor or table pointer is a phi AnyPS5 can't evaluate (a
     * path the dispatch never takes clobbers it), speculate the evaluable value and check it at
     * run time. On by default; REBOX_ANYPS5_SPECULATE=0 turns it off. The host doesn't read
     * the check's fault buffer back yet, so a wrong guess goes unnoticed. */
    {
        const char *speculate = getenv("REBOX_ANYPS5_SPECULATE");
        if (!(speculate && speculate[0] == '0') && !getenv("APS5_SPECULATE_DESCRIPTORS"))
            _putenv("APS5_SPECULATE_DESCRIPTORS=1");
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        set_anyps5_environment();
    }
    return TRUE;
}

static BOOL CALLBACK load_sa5(INIT_ONCE *once, void *param, void **context)
{
    HMODULE m;

    m = LoadLibraryA("rebox_anyps5.dll");

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
    GET(analyse, "sa5_analyse");
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
struct page_range { UINT64 begin, end, device; UINT32 permissions, reserved; };

static int compare_ranges(const void *a, const void *b)
{
    const struct page_range *x = a, *y = b;
    return x->begin < y->begin ? -1 : x->begin > y->begin;
}

/* Returns the table's GPU address and its size in *bytes (AnyPS5 checks that its length in words
 * is exactly 4 + 8 * count, through OpArrayLength). */
static UINT64 page_table(UINT32 *bytes)
{
    struct shadow_region regions[MAX_REGIONS];
    struct gpu_memory_region chunks[MAX_REGIONS];
    UINT64 (*buffers)[2] = malloc(65536 * sizeof(*buffers));
    UINT n = shadow_regions(regions, MAX_REGIONS), nchunks = gpu_memory_regions(chunks, MAX_REGIONS);
    UINT nbuffers = buffers ? shadow_buffer_ranges(buffers, 65536) : 0, count = 0, merged = 0;
    struct page_range *ranges = malloc((n + nchunks + nbuffers) * sizeof(*ranges));
    BYTE *table;
    UINT64 va = 0;

    if (!ranges)
    {
        free(buffers);
        return 0;
    }
    /* Memory that exists: GPU memory chunks and buffers the title viewed, at their GPU VAs, and
     * the shadow heaps' title addresses mapped to their GPU copies. Anything else is unmapped, so
     * a stray access becomes an AnyPS5 fault record, not a GPU page fault. */
    for (UINT i = 0; i < nchunks; i++)
        ranges[count++] = (struct page_range){ chunks[i].gpu, chunks[i].gpu + chunks[i].size, chunks[i].gpu, 3, 0 };
    for (UINT i = 0; i < nbuffers; i++)
        ranges[count++] = (struct page_range){ buffers[i][0], buffers[i][1], buffers[i][0], 3, 0 };
    free(buffers);
    qsort(ranges, count, sizeof(*ranges), compare_ranges);
    /* Merge overlapping identity ranges (buffers inside chunks, repeated views). */
    for (UINT i = 0; i < count; i++)
    {
        if (merged && ranges[i].begin <= ranges[merged - 1].end)
        {
            if (ranges[i].end > ranges[merged - 1].end)
                ranges[merged - 1].end = ranges[i].end;
        }
        else
            ranges[merged++] = ranges[i];
    }
    count = merged;
    /* Shadow heaps win where they overlap (identity ranges below 4 GiB, as with RADV's low window,
     * could): identity ranges that touch one are dropped. */
    for (UINT i = 0; i < n; i++)
    {
        UINT64 begin = regions[i].title_va, end = begin + regions[i].size;
        if (!regions[i].gpu_va)
            continue;
        merged = 0;
        for (UINT j = 0; j < count; j++)
            if (ranges[j].end <= begin || ranges[j].begin >= end || ranges[j].device != ranges[j].begin)
                ranges[merged++] = ranges[j];
        count = merged;
        ranges[count++] = (struct page_range){ begin, end, regions[i].gpu_va, 3, 0 };
    }
    qsort(ranges, count, sizeof(*ranges), compare_ranges);
    if (!(table = malloc(16 + count * sizeof(*ranges))))
    {
        free(ranges);
        return 0;
    }
    ((UINT32 *)table)[0] = 1;            /* version */
    ((UINT32 *)table)[1] = count;
    ((UINT32 *)table)[2] = sizeof(struct page_range);
    ((UINT32 *)table)[3] = 0;
    memcpy(table + 16, ranges, count * sizeof(*ranges));
    va = host_buffer(table, 16 + count * sizeof(*ranges));
    *bytes = (UINT32)(16 + count * sizeof(*ranges));
    free(table);
    free(ranges);
    return va;
}

/* Buffers the GPU writes (AnyPS5's fault buffer and GDS) can't come from the content-shared pool:
 * one of each for the process, as AnyPS5's own driver keeps them. */
static struct { UINT64 gpu; volatile UINT32 *cpu; UINT32 size; } fault_buffer, gds_buffer;

static BOOL CALLBACK init_fault_buffer(INIT_ONCE *once, void *param, void **context)
{
    void *cpu;
    fault_buffer.size = sa5.fault_buffer_bytes();
    if (SUCCEEDED(ReboxGpuAlloc(fault_buffer.size, 256, &cpu, &fault_buffer.gpu)))
    {
        memset(cpu, 0, fault_buffer.size);
        fault_buffer.cpu = cpu;
    }
    return TRUE;
}

static BOOL CALLBACK init_gds_buffer(INIT_ONCE *once, void *param, void **context)
{
    void *cpu;
    gds_buffer.size = sa5.gds_bytes();
    if (SUCCEEDED(ReboxGpuAlloc(gds_buffer.size, 256, &cpu, &gds_buffer.gpu)))
    {
        memset(cpu, 0, gds_buffer.size);
        gds_buffer.cpu = cpu;
    }
    return TRUE;
}

/* AnyPS5's fault record (BdaAbi.hpp): state (2 = ready), reason, address, bytes, stage,
 * instruction, ...; and the SRT guard word, which a failed speculation check (patches/anyps5/0004)
 * sets. Read at each compile, logged (the first few of each kind) and cleared. Faults come from
 * any earlier dispatch, so they aren't tied to the shader being compiled. */
#define FAULT_SRT_GUARD_WORD 6156
static LONG faults_seen;
static void poll_faults(void);
#define poll_faults_now poll_faults

/* Console shader faults and failed speculation checks seen so far (for tests). */
UINT WINAPI ReboxConsoleFaults(void)
{
    InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
    poll_faults_now();
    return (UINT)faults_seen;
}

static void poll_faults(void)
{
    static const char *const reasons[] = { "?", "unmapped address", "permission", "overflow", "invalid table",
            "invalid rectangle", "loop limit", "unaligned", "invalid descriptor", "image table", "unlinked call",
            "window lane spill", "split state" };
    static LONG fault_logs, guard_logs;
    volatile UINT32 *f = fault_buffer.cpu;

    if (!f)
        return;
    if (f[0] == 2)
    {
        UINT32 reason = f[1];
        if (InterlockedIncrement(&fault_logs) <= 20)
            rebox_log("d3d12_xs: console shader fault: %s at %#llx (%u bytes), instruction %#x",
                         reason < ARRAYSIZE(reasons) ? reasons[reason] : "?",
                         (unsigned long long)(f[2] | (UINT64)f[3] << 32), f[4], f[6]);
        for (UINT i = 0; i < 10; i++)
            f[i] = 0;
        InterlockedIncrement(&faults_seen);
    }
    if (fault_buffer.size > FAULT_SRT_GUARD_WORD * 4 && f[FAULT_SRT_GUARD_WORD])
    {
        if (InterlockedIncrement(&guard_logs) <= 20)
            rebox_log("d3d12_xs: a speculated console descriptor didn't match at run time (SRT guard %#x); "
                         "REBOX_ANYPS5_SPECULATE=0 turns speculation off", f[FAULT_SRT_GUARD_WORD]);
        f[FAULT_SRT_GUARD_WORD] = 0;
        InterlockedIncrement(&faults_seen);
    }
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

/* Console shaders often start by setting FLAT_SCRATCH with s_setreg_imm32_b32 (SOPK top 9 bits
 * 0x175, a literal follows) or s_setreg_b32 (0x173) to HW_REG_FLAT_SCR_LO/HI (hwreg ids 20/21).
 * Scratch is the host's business here, so the leading run of those becomes s_nops of the same
 * size. Only the run at the start is touched: further in, a literal could look like one.
 * Returns how many instructions were replaced. */
#define S_NOP 0xbf800000u
static UINT nop_flat_scratch_setup(UINT32 *code, UINT32 dwords)
{
    UINT i = 0, count = 0;

    while (i < dwords)
    {
        UINT32 w = code[i], op = w >> 23, id = w & 0x3f;
        if ((op == 0x175 || op == 0x173) && (id == 20 || id == 21))
        {
            code[i++] = S_NOP;
            if (op == 0x175 && i < dwords)
                code[i++] = S_NOP;
            count++;
        }
        else
            break;
    }
    return count;
}

/* The last compile's outcome, for test harnesses (ReboxConsoleLastResult). */
static SRWLOCK last_lock = SRWLOCK_INIT;
static HRESULT last_hr;
static char last_message[512];

static void set_last(HRESULT hr, const char *what, const char *detail)
{
    AcquireSRWLockExclusive(&last_lock);
    last_hr = hr;
    snprintf(last_message, sizeof(last_message), "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    ReleaseSRWLockExclusive(&last_lock);
}

/* The most recent ReboxConsoleCompile's result and a one-line description. */
HRESULT WINAPI ReboxConsoleLastResult(char *message, SIZE_T size)
{
    HRESULT hr;

    AcquireSRWLockShared(&last_lock);
    hr = last_hr;
    if (message && size)
        snprintf(message, size, "%s", last_message);
    ReleaseSRWLockShared(&last_lock);
    return hr;
}

/* Whether a vkd3d-proton descriptor of this view kind can back an AnyPS5 binding of this kind:
 * bindless sets alias every type at one slot, so a mismatch reads the wrong descriptor type. */
static BOOL view_fits(UINT32 kind, enum shadow_view view)
{
    switch (kind)
    {
    case 0: return view == SHADOW_VIEW_CBV;
    case 1: return view == SHADOW_VIEW_SRV_RAW || view == SHADOW_VIEW_UAV_RAW;
    case 2: return view == SHADOW_VIEW_SRV_TYPED || view == SHADOW_VIEW_SRV_RAW;
    case 3: return view == SHADOW_VIEW_UAV_TYPED || view == SHADOW_VIEW_UAV_RAW;
    case 4: return view == SHADOW_VIEW_SRV_TEXTURE;
    case 5: return view == SHADOW_VIEW_UAV_TEXTURE;
    case 6: return view == SHADOW_VIEW_SAMPLER;
    default: return FALSE;
    }
}

static const char *const view_names[] = { "nothing", "a texture SRV", "a typed buffer SRV", "a raw buffer SRV",
        "a texture UAV", "a typed buffer UAV", "a raw buffer UAV", "a CBV", "a sampler" };

/* Logs a shader's first failure only (keyed by its code). */
static void log_failure(const struct rebox_deferred_shader *h, const char *what, const char *detail)
{
    static SRWLOCK log_lock = SRWLOCK_INIT;
    static UINT64 logged[256];
    static UINT logged_count;
    UINT64 id = hash_bytes(0xcbf29ce484222325ull, (const BYTE *)h + h->code_offset, h->code_dwords * 4u);
    BOOL seen = FALSE;

    set_last(E_FAIL, what, detail);
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
    struct sa5_host_buffer hosts[8];
    UINT64 host_va[8], key[2];
    UINT32 remap_count = 0, host_count = 0, n, index_count = 0, *words = NULL, word_count = 0;
    static UINT32 indices[MAX_ELEMENTS]; /* under compile_lock */
    static SRWLOCK compile_lock = SRWLOCK_INIT;
    struct sa5_compute_request req;
    struct sa5_result *result = NULL;
    const void *push;
    struct module *module;
    char error[1024] = "";
    HRESULT hr = E_FAIL;
    UINT32 *code, nops;
    UINT32 push_size;

    if (!h || !out || in->shader_size < sizeof(*h) || h->magic != REBOX_DEFERRED_MAGIC || h->version != 1
            || h->size > in->shader_size || h->code_offset % 4 || h->code_offset > h->size
            || h->code_dwords > (h->size - h->code_offset) / 4)
        return E_INVALIDARG;
    set_last(E_FAIL, "no result (out of memory?)", NULL);
    InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
    poll_faults();
    if (!sa5.compile)
    {
        set_last(E_NOTIMPL, "rebox_anyps5.dll unavailable", NULL);
        return E_NOTIMPL;
    }
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
    if (!(code = malloc(h->code_dwords * 4u)))
        return E_OUTOFMEMORY;
    memcpy(code, (const BYTE *)h + h->code_offset, h->code_dwords * 4u);
    if ((nops = nop_flat_scratch_setup(code, h->code_dwords)))
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: console shaders: NOPed %u FLAT_SCRATCH setup instruction(s) (first seen)", nops);
    }
    memset(&req, 0, sizeof(req));
    req.code = code;
    req.code_dwords = h->code_dwords;
    req.code_address = h->code_address;
    req.wave = h->wave_size;
    if (!req.wave)
    {
        /* No wave size given (game containers carry one, console_fssm.c; SCRDs from other
         * sources may not): infer it from the code, as the layout analysis does (wave64 code
         * uses 64-bit exec and VCC). */
        struct sa5_layout layout;
        static struct sa5_slot slots[1];
        char ignored[64];
        req.wave = sa5.analyse && !sa5.analyse(code, h->code_dwords, (h->rsrc2 >> 1) & 0x1f, &layout, slots, 0,
                                                 ignored, sizeof(ignored)) ? layout.wave : 32;
    }
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
        if (b.role == 0 && b.kind == 1)
        {
            /* Guest storage buffers (every V#, constant buffers included) are reached through their
             * addresses, which buffers' GPU VAs make valid device addresses: a table of
             * {address, length in dwords} the module reads (sa5_host_buffer.guest_array). That
             * needs no descriptor, so V#s from memory work too, and CBVs (uniform-buffer
             * descriptors in vkd3d-proton) can be read as storage buffers. */
            UINT32 *table, per = b.count ? b.word_count / b.count : 0;
            UINT64 va;
            if (host_count == ARRAYSIZE(hosts) || per != 4 || !(table = calloc(b.count ? b.count : 1, 16)))
            {
                log_failure(h, "no room for the guest buffer table", NULL);
                goto done;
            }
            for (UINT32 e = 0; e < b.count; e++)
            {
                const UINT32 *w = b.words + e * 4;
                UINT64 base = w[0] | (UINT64)(w[1] & 0xffff) << 32;
                UINT32 stride = (w[1] >> 16) & 0x3fff;
                UINT64 bytes = stride ? (UINT64)w[2] * stride : w[2];
                table[e * 4] = (UINT32)base;
                table[e * 4 + 1] = (UINT32)(base >> 32);
                table[e * 4 + 2] = (UINT32)min(bytes / 4, 0xffffffffull);
            }
            va = host_buffer(table, (b.count ? b.count : 1) * 16);
            free(table);
            if (!va)
            {
                log_failure(h, "no GPU memory for the guest buffer table", NULL);
                goto done;
            }
            host_va[host_count] = va;
            hosts[host_count] = (struct sa5_host_buffer){ b.set, b.binding, host_count, 0, 1 };
            key[0] = hash_bytes(key[0], &hosts[host_count], sizeof(hosts[host_count]));
            host_count++;
        }
        else if (guest_descriptor_role(b.role) && b.role >= ROLE_IMAGE_TABLE && !b.count)
        {
            /* AnyPS5 fills image tables with the T#s it finds by walking tables in guest memory;
             * none found (as with synthetic inputs) leaves nothing to bind. */
            log_failure(h, "an image table AnyPS5 found no descriptors for", NULL);
            goto done;
        }
        else if (guest_descriptor_role(b.role))
        {
            BOOL sampler = b.kind == 6;
            UINT32 per = b.count ? b.word_count / b.count : 0;
            if (remap_count == MAX_BINDINGS || index_count + b.count > MAX_ELEMENTS || b.kind > 6
                    || in->bindless[b.kind][0] == ~0u || !in->heap_cpu[sampler] || (per != 4 && per != 8))
            {
                char detail[128];
                snprintf(detail, sizeof(detail), "role %u kind %u binding %u: %u elements of %u words", b.role,
                         b.kind, b.binding, b.count, per);
                log_failure(h, "a guest binding has no bindless set or heap here", detail);
                goto done;
            }
            for (UINT32 e = 0; e < b.count; e++)
            {
                const UINT32 *w = b.words + e * per;
                SIZE_T heap = (SIZE_T)in->heap_cpu[sampler];
                UINT *index = &indices[index_count + e];
                /* Samplers not in the heap (built from literals, or AnyPS5's point-filtered twins)
                 * are made in its reserved tail; matching that ignores rewritten fields is the
                 * last resort. */
                BOOL found = shadow_find_index(heap, w, per, sampler, !sampler, index)
                        || (sampler && shadow_reserve_sampler(heap, w, index))
                        || (sampler && shadow_find_index(heap, w, per, sampler, TRUE, index));
                if (!found)
                {
                    char detail[160];
                    const UINT32 *w = b.words + e * per;
                    snprintf(detail, sizeof(detail), "kind %u binding %u element %u: %08x %08x %08x %08x%s", b.kind,
                             b.binding, e, w[0], w[1], w[2], w[3], per == 8 ? " ..." : "");
                    if (per == 8)
                        snprintf(detail + strlen(detail) - 4, sizeof(detail) - strlen(detail) + 4,
                                 " %08x %08x %08x %08x", w[4], w[5], w[6], w[7]);
                    log_failure(h, "a descriptor it binds isn't in the bound heap", detail);
                    goto done;
                }
                /* A storage image AnyPS5 also binds as a sampled image (size queries): an SRV of
                 * the same texture in the reserved tail. Empty slots are refused: binding one
                 * crashed lavapipe. */
                if (b.kind == 4 && shadow_slot_view(heap, *index) == SHADOW_VIEW_UAV_TEXTURE)
                    shadow_reserve_srv_for_uav(heap, *index, index);
                if (!view_fits(b.kind, shadow_slot_view(heap, *index)))
                {
                    static const char *const kinds[] = { "uniform buffer", "storage buffer", "uniform texel buffer",
                            "storage texel buffer", "sampled image", "storage image", "sampler" };
                    char detail[128];
                    snprintf(detail, sizeof(detail), "binding %u wants a %s, heap slot %u holds %s", b.binding,
                             kinds[b.kind], *index, view_names[shadow_slot_view(heap, *index)]);
                    log_failure(h, "a binding doesn't match the view in its slot", detail);
                    goto done;
                }
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
                log_failure(h, "more than 8 host buffers", NULL);
                goto done;
            }
            switch (b.role)
            {
            case ROLE_IMAGE_TABLE_MAP: /* the image tables' map (ImageTableAbi.hpp), host data like the SRT */
            case ROLE_FLATTENED_SRT:
            case ROLE_SHADER_DATA:
                va = b.word_count ? host_buffer(b.words, bytes) : zero_buffer(bytes = 16);
                break;
            case ROLE_FAULT_BUFFER:
            {
                static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
                InitOnceExecuteOnce(&once, init_fault_buffer, NULL, NULL);
                va = fault_buffer.gpu;
                bytes = fault_buffer.size;
                break;
            }
            case ROLE_GDS:
            {
                static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
                InitOnceExecuteOnce(&once, init_gds_buffer, NULL, NULL);
                va = gds_buffer.gpu;
                bytes = gds_buffer.size;
                break;
            }
            case ROLE_BDA_PAGETABLE:
                va = page_table(&bytes);
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
    {
        char detail[96];
        snprintf(detail, sizeof(detail), "%u SPIR-V words, %u guest bindings, %u host buffers", module->count,
                 remap_count, host_count);
        set_last(S_OK, words ? "compiled" : "cached", detail);
    }

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
    free(code);
    return hr;
}
