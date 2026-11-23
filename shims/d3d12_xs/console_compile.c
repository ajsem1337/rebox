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
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "../../anyps5/rebox_anyps5.h"
#include "console_compile.h"
#include "console_desc.h"
#include "gpu_memory.h"
#include "shadow.h"
#include "title_textures.h"

#define TITLE_BASE 0x10000000ull
#define MAX_REGIONS 64
#define MAX_BINDINGS 64
#define MAX_ELEMENTS 4096
/* Graphics stages share the push block, 128 bytes each: AnyPS5's push data, then the addresses of
 * up to two host buffers whose contents change per draw (baked addresses made a module and a
 * pipeline per draw: a UE5 title's render thread spent its frames compiling). */
#define GRAPHICS_PUSH_DATA 112u
#define GRAPHICS_PUSHED_MAX SA5_DRAW_ADDRESSES /* per-draw host buffers: in the draw block */

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
    int (*compile_vertex)(const struct sa5_vertex_request *, struct sa5_result **, char *, size_t);
    int (*compile_pixel)(const struct sa5_pixel_request *, struct sa5_result **, char *, size_t);
    UINT32 (*vertex_input_count)(const struct sa5_result *);
    int (*vertex_input)(const struct sa5_result *, UINT32, UINT32 *, UINT32 *, UINT32 *);
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
    int (*finalize_draw)(const struct sa5_result *, const struct sa5_remap *, UINT32, const struct sa5_host_buffer *,
            UINT32, UINT32, UINT32 **, UINT32 *, char *, size_t);
    UINT32 (*fault_buffer_bytes)(void);
    UINT32 (*gds_bytes)(void);
} sa5;

/* AnyPS5's settings, made before anything loads rebox_anyps5.dll (it reads some of them once,
 * into statics): from DllMain. */
static void set_anyps5_environment(void)
{
    /* Xbox Series vertex shaders take the vertex ID in v0 and the instance ID in v3 (the legacy
     * layout; AnyPS5 patch 0008), not PS5's v5 and v8. */
    if (!getenv("ANYPS5_LEGACY_VERTEX_INPUTS"))
        _putenv("ANYPS5_LEGACY_VERTEX_INPUTS=1");
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
    GET(compile_vertex, "sa5_compile_vertex");
    GET(compile_pixel, "sa5_compile_pixel");
    GET(vertex_input_count, "sa5_result_vertex_input_count");
    GET(vertex_input, "sa5_result_vertex_input");
    GET(spirv, "sa5_result_spirv");
    GET(binding_count, "sa5_result_binding_count");
    GET(binding, "sa5_result_binding");
    GET(push_constants, "sa5_result_push_constants");
    GET(subgroup_size, "sa5_result_subgroup_size");
    GET(identity, "sa5_result_identity");
    GET(analyse, "sa5_analyse");
    GET(result_free, "sa5_result_free");
    GET(finalize, "sa5_finalize");
    GET(finalize_draw, "sa5_finalize_draw");
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

/* ------------------------------------------------------------ late T#s */

/* T#s a title writes after recording the draw that reads them (the target title fills a per-draw
 * descriptor table between recording and submitting; the console GPU reads it when it runs). A
 * graphics stage's heap slots are in its draw block, which the GPU reads: such a texture's slot
 * is written at submission instead (console_resolve_late), from the descriptor at its address
 * then. Until resolved, it holds the dummy texture. */
struct late_texture { UINT32 *slot; UINT64 source; SIZE_T heap; UINT tries; };
static struct late_texture late[4096];
static UINT late_count;
static SRWLOCK late_lock = SRWLOCK_INIT;

void console_resolve_late(void)
{
    BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size);
    UINT kept = 0, resolved = 0;
    static LONG logged;

    if (!late_count)
        return;
    AcquireSRWLockExclusive(&late_lock);
    for (UINT i = 0; i < late_count; i++)
    {
        struct late_texture *t = &late[i];
        UINT32 w[8] = { 0 };
        UINT index;
        BOOL done = FALSE;
        if (ReboxShadowRead(t->source, w, sizeof(w)) && (w[0] || w[1] || w[2] || w[3]))
        {
            ID3D12Resource *res;
            if ((shadow_find_index(t->heap, w, 8, FALSE, TRUE, &index)
                        && shadow_slot_view(t->heap, index) != SHADOW_VIEW_NONE)
                    || ((res = console_texture_of(w)) && shadow_reserve_srv(t->heap, res, &index)))
            {
                if (shadow_slot_view(t->heap, index) == SHADOW_VIEW_UAV_TEXTURE)
                    shadow_reserve_srv_for_uav(t->heap, index, &index);
                *t->slot = index;
                resolved++;
                done = TRUE;
            }
        }
        if (!done && ++t->tries < 8)
            late[kept++] = *t;
    }
    late_count = kept;
    ReleaseSRWLockExclusive(&late_lock);
    if (resolved && InterlockedIncrement(&logged) <= 8)
        rebox_log("d3d12_xs: %u texture(s) the title wrote after recording their draws bound at submission", resolved);
}

/* ------------------------------------------------------------ host buffers */

static SRWLOCK buffers_lock = SRWLOCK_INIT;
static struct host_buffer { UINT64 hash, gpu; SIZE_T size; const void *cpu; struct host_buffer *next; } *buffers[256];

/* GPU memory for contents that change with every draw or dispatch (guest buffer tables, the
 * flattened SRT, shader data, image table maps: they hold the title's per-draw constant buffer
 * addresses and table pointers): a ring, bump-allocated and wrapped. Sharing by content never hit
 * for those, so each draw allocated GPU memory it never freed, about 10 ms each. The ring is large
 * enough that a slot is reused long after the GPU read it (frames are far fewer draws than its
 * DRAW_RING / 256 slots). */
#define DRAW_RING (128u << 20)

static UINT64 draw_buffer(const void *data, SIZE_T size, BYTE **cpu)
{
    static SRWLOCK ring_lock = SRWLOCK_INIT;
    static BYTE *ring_cpu;
    static UINT64 ring_gpu, ring_head;
    SIZE_T aligned = (size + 255) & ~(SIZE_T)255;
    UINT64 at;

    if (!size || aligned > DRAW_RING)
        return 0;
    AcquireSRWLockExclusive(&ring_lock);
    if (!ring_cpu && FAILED(ReboxGpuAlloc(DRAW_RING, 256, (void **)&ring_cpu, &ring_gpu)))
        ring_cpu = NULL;
    if (!ring_cpu)
    {
        ReleaseSRWLockExclusive(&ring_lock);
        return 0;
    }
    if (ring_head + aligned > DRAW_RING)
        ring_head = 0;
    at = ring_head;
    ring_head += aligned;
    memcpy(ring_cpu + at, data, size);
    if (cpu)
        *cpu = ring_cpu + at;
    ReleaseSRWLockExclusive(&ring_lock);
    return ring_gpu + at;
}

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
 * is exactly 4 + 8 * count, through OpArrayLength). The table has one address and a fixed length
 * (PAGE_TABLE_CAPACITY ranges, the unused ones empty at the top of the address space), and is
 * rebuilt in place only when what it maps changes: its address is baked into modules, and a table
 * per change (a UE5 title views new constant buffers all the time) made new modules and pipelines
 * for the same shaders, and cost milliseconds of sorting at every compile. */
#define PAGE_TABLE_CAPACITY 16384u

static UINT64 page_table(UINT32 *bytes)
{
    static SRWLOCK table_lock = SRWLOCK_INIT;
    static UINT64 table_va, table_key;
    static BYTE *table_cpu;
    struct shadow_region regions[MAX_REGIONS];
    struct gpu_memory_region chunks[MAX_REGIONS];
    UINT64 (*buffers)[2];
    UINT n = shadow_regions(regions, MAX_REGIONS), nchunks = gpu_memory_regions(chunks, MAX_REGIONS);
    UINT nbuffers, count = 0, merged = 0;
    UINT64 key = hash_bytes(hash_bytes(0xcbf29ce484222325ull, regions, n * sizeof(*regions)), chunks,
                            nchunks * sizeof(*chunks)) ^ (UINT64)shadow_buffer_ranges_generation() * 0x9e3779b97f4a7c15ull;
    struct page_range *ranges;
    UINT64 va = 0;

    *bytes = 16 + PAGE_TABLE_CAPACITY * sizeof(struct page_range);
    AcquireSRWLockExclusive(&table_lock);
    if (table_va && key == table_key)
    {
        va = table_va;
        ReleaseSRWLockExclusive(&table_lock);
        return va;
    }
    if (!table_cpu && FAILED(ReboxGpuAlloc(*bytes, 256, (void **)&table_cpu, &table_va)))
    {
        table_cpu = NULL;
        ReleaseSRWLockExclusive(&table_lock);
        return 0;
    }
    buffers = malloc(65536 * sizeof(*buffers));
    nbuffers = buffers ? shadow_buffer_ranges(buffers, 65536) : 0;
    if (!(ranges = malloc((n + nchunks + nbuffers) * sizeof(*ranges))))
    {
        free(buffers);
        ReleaseSRWLockExclusive(&table_lock);
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
    if (count > PAGE_TABLE_CAPACITY)
    {
        static LONG logged;
        if (!InterlockedExchange(&logged, 1))
            rebox_log("d3d12_xs: AnyPS5 page table: %u ranges, %u kept", count, PAGE_TABLE_CAPACITY);
        count = PAGE_TABLE_CAPACITY;
    }
    {
        struct page_range *out = (struct page_range *)(table_cpu + 16);
        ((UINT32 *)table_cpu)[0] = 1;            /* version */
        ((UINT32 *)table_cpu)[1] = PAGE_TABLE_CAPACITY;
        ((UINT32 *)table_cpu)[2] = sizeof(struct page_range);
        ((UINT32 *)table_cpu)[3] = 0;
        memcpy(out, ranges, count * sizeof(*ranges));
        for (UINT i = count; i < PAGE_TABLE_CAPACITY; i++) /* empty, sorted after every real range */
            out[i] = (struct page_range){ ~0ull, ~0ull, 0, 0, 0 };
    }
    table_key = key;
    va = table_va;
    ReleaseSRWLockExclusive(&table_lock);
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
    UINT i = 0, count = 0, scanned = 0;

    /* The setup sits among the prologue's scalar instructions (a UE5 title's pixel shaders move M0
     * first): scan those, up to the first vector or memory instruction. */
    while (i < dwords && scanned++ < 32)
    {
        UINT32 w = code[i], op = w >> 23, id = w & 0x3f;
        if ((op == 0x175 || op == 0x173) && (id == 20 || id == 21))
        {
            code[i++] = S_NOP;
            if (op == 0x175 && i < dwords)
                code[i++] = S_NOP;
            count++;
        }
        else if (op == 0x17d || op == 0x17e)              /* SOP1, SOPC */
            i += 1 + ((w & 0xff) == 0xff || (op == 0x17e && ((w >> 8) & 0xff) == 0xff));
        else if (w >> 28 == 0xb)                          /* SOPK */
            i += 1 + (((w >> 23) & 0x1f) == 0x15);
        else if (w >> 30 == 2 && op != 0x17f)             /* SOP2 */
            i += 1 + ((w & 0xff) == 0xff || ((w >> 8) & 0xff) == 0xff);
        else
            break;
    }
    return count;
}

/* The last compile's outcome, for test harnesses (ReboxConsoleLastResult). */
static SRWLOCK last_lock = SRWLOCK_INIT;
static __thread HRESULT last_hr; /* per thread: the background compiler has its own */
static __thread char last_message[512];

static void set_last(HRESULT hr, const char *what, const char *detail)
{
    AcquireSRWLockExclusive(&last_lock);
    last_hr = hr;
    snprintf(last_message, sizeof(last_message), "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    ReleaseSRWLockExclusive(&last_lock);
}

/* The calling thread's most recent ReboxConsoleCompile result and a one-line description. */
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

/* Graphics stages AnyPS5 refused (their code and stage): not retried at every draw, which falls back
 * at once. AnyPS5's refusals there come from the code (an instruction it doesn't handle); the
 * descriptors don't change them. */
static UINT64 failed_stages[256];
static LONG failed_stage_count;

static volatile LONG graphics_calls;
static volatile LONG64 graphics_ticks, phase_ticks[4]; /* regions, AnyPS5, bindings, module */
static volatile LONG64 compute_ticks;
static volatile LONG compute_calls;
static volatile LONG64 part_ticks[4];                  /* within bindings: identity, lookups, page table, host buffers */

static LONG64 ticks_now(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static void phase_end(LARGE_INTEGER *mark, UINT phase)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (mark->QuadPart)
        InterlockedAdd64(&phase_ticks[phase], now.QuadPart - mark->QuadPart);
    *mark = now;
}

static double qpc_frequency(void)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)f.QuadPart;
}

static BOOL stage_failed(UINT64 id)
{
    LONG n = min(failed_stage_count, (LONG)ARRAYSIZE(failed_stages));
    for (LONG i = 0; i < n; i++)
        if (failed_stages[i] == id)
            return TRUE;
    return FALSE;
}

/* The user SGPR count from RSRC2: graphics stages' USER_SGPR_MSB (bit 27) makes 32 possible
 * (compute's bit 27 is an exception enable). */
static UINT user_sgprs(const struct rebox_deferred_shader *h, BOOL graphics)
{
    return ((h->rsrc2 >> 1) & 0x1f) | (graphics ? ((h->rsrc2 >> 27) & 1) << 5 : 0);
}

/* The address of the n-th texture slot (T#) in a graphics stage's descriptor tables that reads as
 * empty now (sa5_analyse; table t is the pointer in user SGPR table_sgprs[t], or s8 + t), or 0. */
static UINT64 late_source(const struct rebox_deferred_shader *h, const struct rebox_compile_in *in, UINT n)
{
    BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size);
    static struct sa5_slot slots[64]; /* under compile_lock */
    struct sa5_layout layout;
    char ignored[64];
    UINT seen = 0;

    memset(&layout, 0, sizeof(layout));
    if (!sa5.analyse || sa5.analyse((const UINT32 *)((const BYTE *)h + h->code_offset), h->code_dwords,
                                    user_sgprs(h, TRUE), &layout, slots, ARRAYSIZE(slots), ignored, sizeof(ignored)))
        return 0;
    for (UINT32 i = 0; i < layout.slot_count && i < ARRAYSIZE(slots); i++)
    {
        UINT32 sgpr, words[8] = { 0 };
        UINT64 address;
        if (slots[i].kind != SA5_SLOT_TEXTURE || slots[i].size != 8 || slots[i].table >= layout.table_count)
            continue;
        sgpr = layout.table_sgprs[slots[i].table];
        if (sgpr >= 16)
            sgpr = 8 + slots[i].table;
        if (!in->user_sgprs[sgpr])
            continue;
        address = (UINT64)in->user_sgprs[sgpr] + slots[i].dword * 4ull;
        if (ReboxShadowRead(address, words, sizeof(words))
                && !(words[0] | words[1] | words[2] | words[3] | words[4] | words[5] | words[6] | words[7])
                && seen++ == n)
            return address;
    }
    return 0;
}

/* REBOX_CONSOLE_COMPUTE_BISECT=VALUE/BITS: only the compute programs whose code address hashes
 * to VALUE in its low BITS bits run; the rest are skipped (their dispatch dropped). Each program run
 * is logged once. Halving the set run by run finds a program that faults the GPU (The Matrix
 * Awakens: a GPUVM fault at address 0 with console compute on). */
static BOOL compute_bisect_skips(const struct rebox_deferred_shader *h, const struct rebox_compile_in *in)
{
    UINT64 code_address = h->code_address;
    static int state = -1; /* -1 unread, 0 off, 1 on */
    static UINT64 value, mask;
    UINT64 hv = hash_bytes(0xcbf29ce484222325ull, &code_address, sizeof(code_address));

    if (state < 0)
    {
        char v[64];
        unsigned long long a = 0, b = 0;
        DWORD n = GetEnvironmentVariableA("REBOX_CONSOLE_COMPUTE_BISECT", v, sizeof(v));
        state = n && n < sizeof(v) && sscanf(v, "%llu/%llu", &a, &b) == 2 && b < 64;
        mask = state ? (1ull << b) - 1 : 0;
        value = a & mask;
        if (state)
            rebox_log("d3d12_xs: console compute bisection: programs with hash & %#llx == %#llx run", (unsigned long long)mask,
                         (unsigned long long)value);
    }
    if (!state)
        return FALSE;
    if ((hv & mask) != value)
        return TRUE;
    {
        static UINT64 seen[256];
        static LONG count;
        BOOL known = FALSE;
        for (LONG i = 0; i < count && i < 256; i++)
            known |= seen[i] == code_address;
        if (!known && count < 256)
        {
            /* What it's given and what it reads: its user SGPRs, the tables the layout analysis
             * finds, and its code in %TEMP%\rebox-compute-<address>.bin to disassemble. */
            char line[400], tables[128], path[MAX_PATH];
            int at = 0, tat = 0;
            struct sa5_layout layout;
            static struct sa5_slot slots[64];
            char ignored[64];
            HANDLE f;
            seen[count++] = code_address;
            for (UINT32 i = 0; in->user_sgprs && i < in->user_sgpr_count && i < 32 && at < 380; i++)
                at += snprintf(line + at, sizeof(line) - at, " %08x", in->user_sgprs[i]);
            line[at] = 0;
            tables[0] = 0;
            InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
            memset(&layout, 0, sizeof(layout));
            if (sa5.analyse && !sa5.analyse((const UINT32 *)((const BYTE *)h + h->code_offset), h->code_dwords, user_sgprs(h, FALSE),
                                            &layout, slots, ARRAYSIZE(slots), ignored, sizeof(ignored)))
                for (UINT32 t = 0; t < layout.table_count && t < 16 && tat < 110; t++)
                    tat += snprintf(tables + tat, sizeof(tables) - tat, " s%u", layout.table_sgprs[t]);
            rebox_log("d3d12_xs: console compute bisection runs program %#llx (hash %#llx, %u dwords); tables through%s; "
                         "user SGPRs:%s", (unsigned long long)code_address, (unsigned long long)hv, h->code_dwords,
                         tat ? tables : " (none found)", line);
            GetTempPathA(sizeof(path) - 64, path);
            snprintf(path + strlen(path), 64, "rebox-compute-%llx.bin", (unsigned long long)code_address);
            if ((f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)) != INVALID_HANDLE_VALUE)
            {
                DWORD written;
                WriteFile(f, (const BYTE *)h + h->code_offset, h->code_dwords * 4u, &written, NULL);
                CloseHandle(f);
            }
        }
    }
    return FALSE;
}

/* Background compiles. A program's first compile (AnyPS5's translation, hundreds of milliseconds
 * for Unreal 5's large shaders) used to run in the title's render thread at the dispatch or draw;
 * A UE5 sample's game thread gave up waiting for it (120 s). Now the first call for a program
 * queues the compile for a worker thread and fails (the dispatch is skipped, the draw uses the
 * fallback); once the worker has compiled it once, AnyPS5's memo and the caches here make later
 * calls quick, and they compile in the calling thread as before. REBOX_CONSOLE_ASYNC=0 turns it
 * off. */
static BOOL async_compiles(void)
{
    char v[8];
    DWORD n = GetEnvironmentVariableA("REBOX_CONSOLE_ASYNC", v, sizeof(v)); /* each call: tests change it */
    return !(n && n < sizeof(v) && v[0] == '0');
}

enum program_state { PROGRAM_COLD, PROGRAM_PENDING, PROGRAM_WARM, PROGRAM_FAILED };
#define PROGRAMS 16384
static struct { UINT64 key; LONG state; } programs[PROGRAMS];
static SRWLOCK programs_lock = SRWLOCK_INIT;

/* The state of a program (by its blob's hash), claiming it as pending if it was cold. */
static enum program_state program_claim(UINT64 key, BOOL claim)
{
    UINT slot = (UINT)(key % PROGRAMS);
    enum program_state state = PROGRAM_COLD;

    AcquireSRWLockExclusive(&programs_lock);
    for (UINT i = 0; i < 64; i++)
    {
        UINT at = (slot + i) % PROGRAMS;
        if (programs[at].state != PROGRAM_COLD && programs[at].key == key)
        {
            state = programs[at].state;
            break;
        }
        if (programs[at].state == PROGRAM_COLD)
        {
            if (claim)
            {
                programs[at].key = key;
                programs[at].state = PROGRAM_PENDING;
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&programs_lock);
    return state;
}

static void program_set(UINT64 key, enum program_state state)
{
    UINT slot = (UINT)(key % PROGRAMS);
    AcquireSRWLockExclusive(&programs_lock);
    for (UINT i = 0; i < 64; i++)
    {
        UINT at = (slot + i) % PROGRAMS;
        if (programs[at].state != PROGRAM_COLD && programs[at].key == key)
        {
            programs[at].state = state;
            break;
        }
    }
    ReleaseSRWLockExclusive(&programs_lock);
}

struct compile_job
{
    struct compile_job *next;
    UINT64 key;
    struct rebox_compile_in in;
    UINT32 user_sgprs[64];
    BYTE shader[];
};
static struct compile_job *jobs_head, *jobs_tail;
static SRWLOCK jobs_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE jobs_ready = CONDITION_VARIABLE_INIT;
static LONG jobs_queued, jobs_done;

static HRESULT compile_now(const struct rebox_compile_in *in, struct rebox_compile_out *out, BOOL background);

static DWORD WINAPI compile_worker(void *unused)
{
    for (;;)
    {
        struct compile_job *job;
        struct rebox_compile_out out;
        HRESULT hr;
        LONG done;

        AcquireSRWLockExclusive(&jobs_lock);
        while (!jobs_head)
            SleepConditionVariableSRW(&jobs_ready, &jobs_lock, INFINITE, 0);
        job = jobs_head;
        if (!(jobs_head = job->next))
            jobs_tail = NULL;
        ReleaseSRWLockExclusive(&jobs_lock);

        memset(&out, 0, sizeof(out));
        hr = compile_now(&job->in, &out, TRUE);
        program_set(job->key, SUCCEEDED(hr) ? PROGRAM_WARM : PROGRAM_FAILED);
        done = InterlockedIncrement(&jobs_done);
        if (!(done & (done - 1)))
            rebox_log("d3d12_xs: console shaders: %ld background compiles done of %ld queued", done, jobs_queued);
        free(job);
    }
    return 0;
}

static void queue_compile(UINT64 key, const struct rebox_compile_in *in)
{
    static INIT_ONCE started = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    struct compile_job *job = malloc(sizeof(*job) + in->shader_size);

    if (!job)
    {
        program_set(key, PROGRAM_COLD);
        return;
    }
    memset(job, 0, sizeof(*job));
    job->key = key;
    job->in = *in;
    memcpy(job->shader, in->shader, in->shader_size);
    job->in.shader = job->shader;
    job->in.user_sgpr_count = min(in->user_sgpr_count, (UINT32)ARRAYSIZE(job->user_sgprs));
    if (in->user_sgprs)
        memcpy(job->user_sgprs, in->user_sgprs, job->in.user_sgpr_count * 4u);
    job->in.user_sgprs = job->user_sgprs;
    if (InitOnceBeginInitialize(&started, 0, &pending, NULL) && pending)
    {
        HANDLE t = CreateThread(NULL, 0, compile_worker, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
        InitOnceComplete(&started, 0, NULL);
    }
    InterlockedIncrement(&jobs_queued);
    AcquireSRWLockExclusive(&jobs_lock);
    if (jobs_tail)
        jobs_tail->next = job;
    else
        jobs_head = job;
    jobs_tail = job;
    ReleaseSRWLockExclusive(&jobs_lock);
    WakeConditionVariable(&jobs_ready);
}

HRESULT WINAPI ReboxConsoleCompile(const struct rebox_compile_in *in, struct rebox_compile_out *out)
{
    UINT64 key;

    if (!async_compiles() || !in || !in->shader || !in->shader_size || !out)
        return compile_now(in, out, FALSE);
    key = hash_bytes(0xcbf29ce484222325ull, in->shader, in->shader_size);
    switch (program_claim(key, TRUE))
    {
    case PROGRAM_COLD: /* claimed: compile it in the background */
        queue_compile(key, in);
        set_last(E_PENDING, "compiling in the background", NULL);
        return E_PENDING;
    case PROGRAM_PENDING:
        set_last(E_PENDING, "compiling in the background", NULL);
        return E_PENDING;
    case PROGRAM_FAILED:
        set_last(E_FAIL, "its background compile failed", NULL);
        return E_FAIL;
    default:
        return compile_now(in, out, FALSE);
    }
}

static HRESULT compile_now(const struct rebox_compile_in *in, struct rebox_compile_out *out, BOOL background)
{
    const struct rebox_deferred_shader *h = in ? in->shader : NULL;
    struct sa5_region regions[MAX_REGIONS];
    struct shadow_region shadows[MAX_REGIONS];
    struct sa5_remap remaps[MAX_BINDINGS];
    struct sa5_host_buffer hosts[8];
    UINT64 host_va[8], key[2], pushed_va[GRAPHICS_PUSHED_MAX];
    UINT32 pushed_count = 0;
    UINT32 remap_count = 0, host_count = 0, n, index_count = 0, *words = NULL, word_count = 0;
    static UINT32 indices[MAX_ELEMENTS]; /* under compile_lock */
    static struct { UINT position; UINT64 source; SIZE_T heap; } pending[64]; /* late T#s of this stage */
    UINT pending_count = 0, zero_textures = 0;
    BYTE *block_cpu = NULL;
    static SRWLOCK compile_lock = SRWLOCK_INIT;
    struct sa5_compute_request req;
    struct sa5_result *result = NULL;
    const void *push;
    struct module *module;
    char error[1024] = "";
    HRESULT hr = E_FAIL;
    UINT32 *code, nops;
    UINT32 push_size;

    const struct rebox_deferred_graphics *g = h ? (const struct rebox_deferred_graphics *)(h + 1) : NULL;
    BOOL graphics;
    UINT64 stage_id;
    LARGE_INTEGER started = { 0 }, mark = { 0 };

    if (!h || !out || in->shader_size < sizeof(*h) || h->magic != REBOX_DEFERRED_MAGIC
            || (h->version != 1 && h->version != 2)
            || h->size > in->shader_size || h->code_offset % 4 || h->code_offset > h->size
            || h->code_dwords > (h->size - h->code_offset) / 4)
        return E_INVALIDARG;
    /* Version 2: a vertex (0x1) or pixel (0x10) stage, rebox_deferred_graphics after the header. */
    graphics = h->version == 2;
    if (graphics && ((h->stage != 0x1 && h->stage != 0x10) || h->code_offset < sizeof(*h) + sizeof(*g)))
        return E_INVALIDARG;
    stage_id = graphics ? hash_bytes(hash_bytes(0xcbf29ce484222325ull, &h->stage, sizeof(h->stage)),
                                     (const BYTE *)h + h->code_offset, h->code_dwords * 4u) : 0;
    if (graphics)
    {
        /* Counts for the log, at powers of two: stage compiles asked for, the time they took (the
         * draw waits for them), and stages AnyPS5 refused. */
        LONG n = InterlockedIncrement(&graphics_calls);
        if (!(n & (n - 1)))
        {
            double ms = 1000.0 / qpc_frequency() / max(n - 1, 1);
            rebox_log("d3d12_xs: console graphics: %ld stage compiles asked for, %.2f ms each on average (memory "
                         "regions %.2f, AnyPS5 %.2f, bindings %.2f [identity %.2f, lookups %.2f, page table %.2f, host buffers %.2f], "
                         "module %.2f); "
                         "%ld distinct stages refused so far", n,
                         graphics_ticks * ms, phase_ticks[0] * ms, phase_ticks[1] * ms, phase_ticks[2] * ms,
                         part_ticks[0] * ms, part_ticks[1] * ms, part_ticks[2] * ms, part_ticks[3] * ms,
                         phase_ticks[3] * ms, min(failed_stage_count, (LONG)ARRAYSIZE(failed_stages)));
        }
        QueryPerformanceCounter(&started);
        mark = started;
    }
    if (!graphics)
    {
        /* Compute compiles, timed the same way (at dispatch, the list's recording waits for them). */
        LONG n = InterlockedIncrement(&compute_calls);
        if (!(n & (n - 1)))
            rebox_log("d3d12_xs: console compute: %ld compiles asked for, %.2f ms each on average", n,
                         compute_ticks * 1000.0 / qpc_frequency() / max(n - 1, 1));
        QueryPerformanceCounter(&started);
    }
    if (graphics && stage_failed(stage_id))
        return E_FAIL;
    if (graphics)
    {
        /* The first stages of each kind: the table pointers the code reads (sa5_analyse) next to the
         * user SGPRs it gets, to check the root parameter packing. */
        static LONG shown[2];
        if (InterlockedIncrement(&shown[h->stage == 0x10]) <= 6)
        {
            struct sa5_layout layout;
            static struct sa5_slot slots[64];
            char line[512], ignored[64];
            int at = 0;
            InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
            memset(&layout, 0, sizeof(layout));
            if (sa5.analyse && !sa5.analyse((const UINT32 *)((const BYTE *)h + h->code_offset), h->code_dwords,
                                            32, &layout, slots, ARRAYSIZE(slots), ignored, sizeof(ignored)))
                for (UINT32 t = 0; t < layout.table_count && t < 16 && at < 400; t++)
                    at += snprintf(line + at, sizeof(line) - at, " s%u", layout.table_sgprs[t]);
            line[at] = 0;
            rebox_log("d3d12_xs: console %s shader %016llx reads tables through%s; user SGPRs s0-s15: %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                         h->stage == 0x10 ? "pixel" : "vertex", (unsigned long long)stage_id, at ? line : " (none found)",
                         in->user_sgprs[0], in->user_sgprs[1], in->user_sgprs[2], in->user_sgprs[3], in->user_sgprs[4],
                         in->user_sgprs[5], in->user_sgprs[6], in->user_sgprs[7], in->user_sgprs[8], in->user_sgprs[9],
                         in->user_sgprs[10], in->user_sgprs[11], in->user_sgprs[12], in->user_sgprs[13],
                         in->user_sgprs[14], in->user_sgprs[15]);
            /* What the table s8 points at holds (in a shadow heap), for the zeros some stages read. */
            {
                BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size);
                UINT32 table[16];
                if (in->user_sgprs[8] && ReboxShadowRead(in->user_sgprs[8], table, sizeof(table)))
                    rebox_log("d3d12_xs:   at s8 %#x: %08x %08x %08x %08x %08x %08x %08x %08x  %08x %08x %08x %08x %08x "
                              "%08x %08x %08x", in->user_sgprs[8], table[0], table[1], table[2], table[3], table[4],
                              table[5], table[6], table[7], table[8], table[9], table[10], table[11], table[12],
                              table[13], table[14], table[15]);
                else if (in->user_sgprs[8])
                    rebox_log("d3d12_xs:   at s8 %#x: not in a shadow heap", in->user_sgprs[8]);
            }
        }
    }
    set_last(E_FAIL, "no result (out of memory?)", NULL);
    if (!graphics && compute_bisect_skips(h, in))
    {
        set_last(E_FAIL, "skipped by REBOX_CONSOLE_COMPUTE_BISECT", NULL);
        return E_FAIL;
    }
    InitOnceExecuteOnce(&sa5.once, load_sa5, NULL, NULL);
    poll_faults();
    if (!sa5.compile || (graphics && (!sa5.compile_vertex || !sa5.compile_pixel || !sa5.vertex_input || !sa5.finalize_draw)))
    {
        set_last(E_NOTIMPL, "rebox_anyps5.dll unavailable (or too old for graphics stages)", NULL);
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
        req.wave = sa5.analyse && !sa5.analyse(code, h->code_dwords, user_sgprs(h, graphics), &layout, slots, 0,
                                                 ignored, sizeof(ignored)) ? layout.wave : 32;
    }
    req.user_data = in->user_sgprs;
    req.user_data_count = min(in->user_sgpr_count, user_sgprs(h, graphics));
    memcpy(req.threads, h->workgroup_size, sizeof(req.threads));
    req.rsrc1 = h->rsrc1;
    req.rsrc2 = h->rsrc2;
    req.regions = regions;
    req.region_count = n;
    req.use_cache = 1;

    /* One compile at a time: AnyPS5's caches are thread-safe, but this keeps the static index
     * table simple and compiles are rare once warm. The render thread never waits for a background
     * compile (see ReboxConsoleCompile): if one holds the lock, this call is skipped. */
    if (background || !async_compiles())
        AcquireSRWLockExclusive(&compile_lock);
    else if (!TryAcquireSRWLockExclusive(&compile_lock))
    {
        free(code);
        set_last(E_PENDING, "a background compile holds the compiler", NULL);
        return E_PENDING;
    }
    if (graphics)
        phase_end(&mark, 0);
    if (graphics && h->stage == 0x1)
    {
        /* The console's fetch call (s_swappc_b64 s[4:5]) becomes Vulkan vertex attributes: input r
         * fills v[4+4r..7+4r]. The V#s only steer AnyPS5's grouping; the attributes' formats and
         * buffers come from the PSO's input layout (patch 0006). s[4:5] holds a fetch address
         * outside the program, as the console driver's. */
        /* REBOX_GRAPHICS_DEBUG=fullscreen: a full-screen triangle from the vertex ID instead
         * (x = 4 * (vid & 1) - 1, y = 4 * ((vid >> 1) & 1) - 1, z = w = 1: nearest with reversed depth), with
         * magenta pixel shaders: tells vertex shaders that put everything off-screen apart from
         * draws that never reach the presented image. */
        static const UINT32 fullscreen[] = {
            0x36020081,             /* v_and_b32 v1, 1, v0 */
            0x2c040081,             /* v_lshrrev_b32 v2, 1, v0 */
            0x36040481,             /* v_and_b32 v2, 1, v2 */
            0x7e020d01,             /* v_cvt_f32_u32 v1, v1 */
            0x7e040d02,             /* v_cvt_f32_u32 v2, v2 */
            0x100202f6,             /* v_mul_f32 v1, 4.0, v1 */
            0x100404f6,             /* v_mul_f32 v2, 4.0, v2 */
            0x060202f3,             /* v_add_f32 v1, -1.0, v1 */
            0x060404f3,             /* v_add_f32 v2, -1.0, v2 */
            0x7e0a02f2,             /* v_mov_b32 v5, 1.0 */
            0x7e0c02f2,             /* v_mov_b32 v6, 1.0 */
            0xf80008cf, 0x06050201, /* exp pos0 v1, v2, v5, v6 done */
            0xbf810000,             /* s_endpgm */
        };
        const char *debug = getenv("REBOX_GRAPHICS_DEBUG");
        BOOL replace = debug && !strcmp(debug, "fullscreen");
        struct sa5_vertex_request vreq;
        UINT32 user[32];
        memset(&vreq, 0, sizeof(vreq));
        memset(user, 0, sizeof(user));
        memcpy(user, req.user_data, min(req.user_data_count, 32u) * 4);
        user[4] = 0x30000000u;
        user[5] = 0;
        vreq.code = replace ? fullscreen : req.code;
        vreq.code_dwords = replace ? ARRAYSIZE(fullscreen) : req.code_dwords;
        vreq.code_address = req.code_address;
        vreq.wave = req.wave;
        vreq.user_data = user;
        vreq.user_data_count = max(req.user_data_count, 6u);
        vreq.rsrc1 = h->rsrc1;
        vreq.input_count = replace ? 0 : min(g->input_count, 32u);
        for (UINT32 r = 0; r < vreq.input_count; r++)
        {
            vreq.inputs[r].vsharp[0] = 0x20000000u + r * 0x100000u;
            vreq.inputs[r].vsharp[1] = 16u << 16;
            vreq.inputs[r].vsharp[2] = 0x10000;
            vreq.inputs[r].vsharp[3] = (77u << 12) | 0xfacu; /* 32_32_32_32_FLOAT, identity swizzle */
            vreq.inputs[r].register_start = 4 + 4 * r;
            vreq.inputs[r].register_count = 4;
        }
        vreq.push_offset = in->push_offset;
        vreq.push_size = GRAPHICS_PUSH_DATA;
        vreq.regions = regions;
        vreq.region_count = n;
        vreq.use_cache = 1;
        if (sa5.compile_vertex(&vreq, &result, error, sizeof(error)))
        {
            log_failure(h, "AnyPS5 failed (vertex)", error);
            {
                LONG slot = InterlockedIncrement(&failed_stage_count) - 1;
                if (slot < (LONG)ARRAYSIZE(failed_stages))
                    failed_stages[slot] = stage_id;
            }
            goto done;
        }
    }
    else if (graphics)
    {
        /* REBOX_GRAPHICS_DEBUG=magenta: every console pixel shader exports magenta to MRT0 instead,
         * to tell whether draws reach the render targets at all (vertices, pipelines) from whether
         * the real pixel shaders' output is right. */
        static const UINT32 magenta[] = {
            0x7e0002f2,             /* v_mov_b32 v0, 1.0 */
            0x7e020280,             /* v_mov_b32 v1, 0 */
            0xf800180f, 0x00000100, /* exp mrt0 v0, v1, v0, v0 done vm */
            0xbf810000,             /* s_endpgm */
        };
        const char *debug = getenv("REBOX_GRAPHICS_DEBUG");
        BOOL solid = debug && (!strcmp(debug, "magenta") || !strcmp(debug, "fullscreen"));
        struct sa5_pixel_request preq;
        memset(&preq, 0, sizeof(preq));
        preq.code = solid ? magenta : req.code;
        preq.code_dwords = solid ? ARRAYSIZE(magenta) : req.code_dwords;
        preq.code_address = req.code_address;
        preq.wave = req.wave;
        preq.user_data = req.user_data;
        preq.user_data_count = req.user_data_count;
        preq.rsrc1 = h->rsrc1;
        preq.rsrc2 = h->rsrc2;
        preq.input_ena = g->input_ena;
        preq.interpolator_count = solid ? 0 : min(g->interpolator_count, 32u);
        memcpy(preq.interpolator_settings, g->interpolators, sizeof(preq.interpolator_settings));
        preq.col_format = solid ? 9 : g->col_format;
        preq.depth_export = g->depth_export;
        preq.push_offset = in->push_offset;
        preq.push_size = GRAPHICS_PUSH_DATA;
        preq.regions = regions;
        preq.region_count = n;
        preq.use_cache = 1;
        if (sa5.compile_pixel(&preq, &result, error, sizeof(error)))
        {
            log_failure(h, "AnyPS5 failed (pixel)", error);
            {
                LONG slot = InterlockedIncrement(&failed_stage_count) - 1;
                if (slot < (LONG)ARRAYSIZE(failed_stages))
                    failed_stages[slot] = stage_id;
            }
            goto done;
        }
    }
    else if (sa5.compile(&req, &result, error, sizeof(error)))
    {
        log_failure(h, "AnyPS5 failed", error);
        goto done;
    }

    if (graphics)
        phase_end(&mark, 1);
    {
        LONG64 t = ticks_now();
        sa5.identity(result, key);
        if (graphics)
            InterlockedAdd64(&part_ticks[0], ticks_now() - t);
    }
    key[1] ^= hash_bytes(0xcbf29ce484222325ull, in->bindless, sizeof(in->bindless));
    if (graphics)
    {
        /* The same code as another stage or at another push offset is another module. */
        key[1] = hash_bytes(key[1], &h->stage, sizeof(h->stage));
        key[1] = hash_bytes(key[1], &in->push_offset, sizeof(in->push_offset));
    }
    for (UINT32 i = 0; i < sa5.binding_count(result); i++)
    {
        struct sa5_binding b;
        sa5.binding(result, i, &b);
        if (graphics)
        {
            /* The first graphics stages' bindings: which roles and kinds their resources come as. */
            static LONG shown[2];
            if (InterlockedIncrement(&shown[h->stage == 0x10]) <= 32)
                rebox_log("d3d12_xs: console %s shader %016llx binding %u: role %u kind %u set %u binding %u, "
                             "%u elements, %u words%s %08x %08x %08x %08x", h->stage == 0x10 ? "pixel" : "vertex",
                             (unsigned long long)stage_id, i, b.role, b.kind, b.set, b.binding, b.count, b.word_count,
                             b.word_count ? ":" : "", b.word_count > 0 ? b.words[0] : 0, b.word_count > 1 ? b.words[1] : 0,
                             b.word_count > 2 ? b.words[2] : 0, b.word_count > 3 ? b.words[3] : 0);
        }
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
                if (graphics)
                {
                    /* The first graphics stages' buffers and their first values (where guest
                     * memory has them): do vertex shaders get real transforms, or zeros? */
                    static LONG shown[2];
                    const float *f = NULL;
                    MEMORY_BASIC_INFORMATION mbi;
                    if (shown[h->stage == 0x10] >= 48)
                        continue;
                    for (UINT r = 0; r < n && !f; r++)
                        if (base >= regions[r].address && base + 32 <= regions[r].address + regions[r].size)
                            f = (const float *)((const BYTE *)regions[r].data + (base - regions[r].address));
                    /* xmem's GPU memory is identity-mapped: the title's own buffers read at their GPU VA. */
                    if (!f && base && VirtualQuery((const void *)(ULONG_PTR)base, &mbi, sizeof(mbi))
                            && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                            && base + 32 <= (UINT64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize)
                        f = (const float *)(ULONG_PTR)base;
                    if (InterlockedIncrement(&shown[h->stage == 0x10]) <= 48)
                        rebox_log("d3d12_xs: console %s shader %016llx buffer %u.%u: base %#llx, stride %u, "
                                     "%u records%s %g %g %g %g %g %g %g %g", h->stage == 0x10 ? "pixel" : "vertex",
                                     (unsigned long long)stage_id, i, e, (unsigned long long)base, stride, w[2],
                                     f ? ";" : " (not readable here)", f ? f[0] : 0, f ? f[1] : 0, f ? f[2] : 0,
                                     f ? f[3] : 0, f ? f[4] : 0, f ? f[5] : 0, f ? f[6] : 0, f ? f[7] : 0);
                }
            }
            {
                LONG64 t = ticks_now();
                va = draw_buffer(table, (b.count ? b.count : 1) * 16, NULL);
                InterlockedAdd64(&part_ticks[3], ticks_now() - t);
            }
            free(table);
            if (!va)
            {
                log_failure(h, "no GPU memory for the guest buffer table", NULL);
                goto done;
            }
            host_va[host_count] = va;
            /* Per-draw contents: pushed (graphics: after the stage's push data). */
            if (graphics && pushed_count == GRAPHICS_PUSHED_MAX)
            {
                log_failure(h, "more per-draw host buffers than a graphics stage has push room for", NULL);
                goto done;
            }
            hosts[host_count] = (struct sa5_host_buffer){ b.set, b.binding, graphics ? pushed_count : host_count, 0, 1,
                    graphics ? in->push_offset + GRAPHICS_PUSH_DATA : 0, 0 };
            pushed_va[pushed_count++] = va;
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
                if (graphics && !sampler)
                {
                    /* The first graphics stages' textures: which resource, and whether its texels
                     * come from title memory (uploaded how often) or through the API. */
                    static LONG shown;
                    UINT64 address = 0;
                    UINT uploads = 0;
                    /* Only when logging: console_texture_of walks every texture the title made. */
                    if (shown < 64 && InterlockedIncrement(&shown) <= 64)
                    {
                        ID3D12Resource *res = console_texture_of(w);
                        D3D12_RESOURCE_DESC rd = { 0 };
                        BOOL title = res && title_texture_info(res, &address, &uploads);
                        if (res)
                            rd = ID3D12Resource_GetDesc(res);
                        rebox_log("d3d12_xs: console %s shader %016llx texture %u.%u: T# %08x %08x, resource %p "
                                     "%llux%u format %u flags %#x, %s %#llx, uploaded %u times",
                                     h->stage == 0x10 ? "pixel" : "vertex", (unsigned long long)stage_id, i, e, w[0],
                                     w[1], (void *)res, (unsigned long long)rd.Width, rd.Height, rd.Format, rd.Flags,
                                     title ? "title memory at" : "not from title memory", (unsigned long long)address,
                                     uploads);
                    }
                }
                UINT *index = &indices[index_count + e];
                /* Samplers not in the heap (built from literals, or AnyPS5's point-filtered twins)
                 * are made in its reserved tail; matching that ignores rewritten fields is the
                 * last resort. */
                LONG64 lookup_start = ticks_now();
                BOOL found = shadow_find_index(heap, w, per, sampler, !sampler, index)
                        || (sampler && shadow_reserve_sampler(heap, w, index))
                        || (sampler && shadow_find_index(heap, w, per, sampler, TRUE, index));
                BOOL null_view = FALSE;
                if (graphics)
                    InterlockedAdd64(&part_ticks[1], ticks_now() - lookup_start);
                /* A texture descriptor the title wrote outside our heaps, or an empty slot, reads
                 * as zero, like a console null descriptor (a UE5 title: dummy T#s with base 0x9200):
                 * a zeroed dummy texture (shadow_reserve_null); binding the empty slot itself
                 * crashed lavapipe. */
                /* A T# the title wrote itself: the texture at its address, viewed in the reserved tail. */
                if (b.kind == 4 && (!found || shadow_slot_view(heap, *index) == SHADOW_VIEW_NONE))
                {
                    ID3D12Resource *res = (w[0] || (w[1] & 0xff)) ? console_texture_of(w) : NULL;
                    if (res && shadow_reserve_srv(heap, res, index))
                    {
                        static LONG logged;
                        found = TRUE;
                        if (InterlockedIncrement(&logged) <= 16)
                            rebox_log("d3d12_xs: console shader %016llx: binding %u: a T# the title wrote itself "
                                      "(%08x %08x) is texture %p", (unsigned long long)stage_id, b.binding, w[0], w[1],
                                      (void *)res);
                    }
                }
                if ((b.kind == 4 || b.kind == 5) && (!found || shadow_slot_view(heap, *index) == SHADOW_VIEW_NONE))
                    null_view = shadow_reserve_null(heap, b.kind == 5, index);
                /* An empty T# of a graphics stage: maybe one the title writes before it submits. Its
                 * address is the n-th texture slot of the code's tables that is empty now. */
                if (null_view && graphics && b.kind == 4 && !(w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])
                        && pending_count < ARRAYSIZE(pending))
                {
                    UINT64 source = late_source(h, in, zero_textures++);
                    if (source)
                    {
                        pending[pending_count].position = index_count + e;
                        pending[pending_count].source = source;
                        pending[pending_count].heap = heap;
                        pending_count++;
                    }
                }
                if (null_view)
                {
                    static LONG logged;
                    if (InterlockedIncrement(&logged) <= 8)
                    {
                        BOOL WINAPI ReboxShadowRead(UINT64 address, void *out, SIZE_T size);
                        UINT32 table[8] = { 0 };
                        BOOL read = in->user_sgprs[8] && ReboxShadowRead(in->user_sgprs[8], table, sizeof(table));
                        rebox_log("d3d12_xs: console shader %016llx: binding %u (%s) gets a dummy texture: "
                                     "the descriptor %s (T# %08x %08x %08x %08x; s8 %#x %s %08x %08x %08x %08x %08x "
                                     "%08x %08x %08x)", (unsigned long long)hash_bytes(0xcbf29ce484222325ull,
                                     (const BYTE *)h + h->code_offset, h->code_dwords * 4u), b.binding,
                                     b.kind == 5 ? "storage image" : "sampled image",
                                     found ? "slot is empty" : "isn't in the bound heap", w[0], w[1], w[2], w[3],
                                     in->user_sgprs[8], read ? "holds" : "unreadable", table[0], table[1], table[2],
                                     table[3], table[4], table[5], table[6], table[7]);
                        /* The table again at the next submission (proxy.c): whether the title fills it
                         * between recording the draw and submitting it. */
                        {
                            extern volatile UINT64 rebox_table_watch;
                            if (in->user_sgprs[8])
                                rebox_table_watch = in->user_sgprs[8];
                        }
                        /* With REBOX_DUMP_SPIRV, the stage's code and the buffer the table's first V# names,
                         * for disassembly. */
                        if (getenv("REBOX_DUMP_SPIRV") && logged == 1)
                        {
                            char path[MAX_PATH];
                            FILE *f;
                            UINT64 base = read ? ((UINT64)(table[1] & 0xffff) << 32 | table[0]) : 0;
                            UINT64 bytes = (UINT64)table[2] * ((table[1] >> 16) & 0x3fff ? (table[1] >> 16) & 0x3fff : 1);
                            snprintf(path, sizeof(path), "%s\\%016llx.code.bin", getenv("REBOX_DUMP_SPIRV"),
                                     (unsigned long long)stage_id);
                            if ((f = fopen(path, "wb")))
                            {
                                fwrite((const BYTE *)h + h->code_offset, 4, h->code_dwords, f);
                                fclose(f);
                            }
                            snprintf(path, sizeof(path), "%s\\%016llx.cb.bin", getenv("REBOX_DUMP_SPIRV"),
                                     (unsigned long long)stage_id);
                            if (base && bytes && bytes <= 65536 && !IsBadReadPtr((void *)(ULONG_PTR)base, bytes)
                                    && (f = fopen(path, "wb")))
                            {
                                fwrite((void *)(ULONG_PTR)base, 1, bytes, f);
                                fclose(f);
                            }
                        }
                    }
                    found = TRUE;
                }
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
                if (!null_view && b.kind == 4 && shadow_slot_view(heap, *index) == SHADOW_VIEW_UAV_TEXTURE)
                    shadow_reserve_srv_for_uav(heap, *index, index);
                if (!null_view && !view_fits(b.kind, shadow_slot_view(heap, *index)))
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
                                                        in->bindless[b.kind][1], b.count, &indices[index_count],
                                                        index_count };
            key[0] = hash_bytes(key[0], &b.binding, sizeof(b.binding));
            key[0] = hash_bytes(key[0], &b.count, sizeof(b.count));
            /* Graphics: the heap slots are in the draw block, not the module (a UE5 title copies its
             * descriptors to new slots every draw: one module per draw, and a pipeline each). */
            if (!graphics)
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
            {
                LONG64 t = ticks_now();
                va = b.word_count ? draw_buffer(b.words, bytes, NULL) : zero_buffer(bytes = 16);
                InterlockedAdd64(&part_ticks[3], ticks_now() - t);
                break;
            }
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
            {
                LONG64 t = ticks_now();
                va = page_table(&bytes);
                if (graphics)
                    InterlockedAdd64(&part_ticks[2], ticks_now() - t);
                break;
            }
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
            /* Graphics stages share the push block (128 bytes each): buffers whose contents change per
             * draw (the flattened SRT, shader data, image table maps) are pushed after the stage's
             * push data; the process-wide ones (fault buffer, GDS, page table) are baked in. */
            if (graphics && (b.role == ROLE_IMAGE_TABLE_MAP || b.role == ROLE_FLATTENED_SRT || b.role == ROLE_SHADER_DATA))
            {
                if (pushed_count == GRAPHICS_PUSHED_MAX)
                {
                    log_failure(h, "more per-draw host buffers than a graphics stage has push room for", NULL);
                    goto done;
                }
                hosts[host_count] = (struct sa5_host_buffer){ b.set, b.binding, pushed_count, bytes / 4, 0,
                        in->push_offset + GRAPHICS_PUSH_DATA, 0 };
                pushed_va[pushed_count++] = va;
            }
            else
            {
                hosts[host_count] = (struct sa5_host_buffer){ b.set, b.binding, host_count, bytes / 4, 0, 0,
                        graphics ? va : 0 };
                if (graphics)
                    key[0] = hash_bytes(key[0], &va, sizeof(va));
            }
            key[0] = hash_bytes(key[0], &hosts[host_count], sizeof(hosts[host_count]));
            host_count++;
        }
    }

    if (graphics)
        phase_end(&mark, 2);
    if (!(module = find_module(key)))
    {
        if (graphics ? sa5.finalize_draw(result, remaps, remap_count, hosts, host_count,
                                         in->push_offset + GRAPHICS_PUSH_DATA, &words, &word_count, error, sizeof(error))
                     : sa5.finalize(result, remaps, remap_count, hosts, host_count, &words, &word_count, error,
                                    sizeof(error)))
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
        /* The key is what vkd3d-proton's breadcrumbs record before the dispatch or draw (patch 0008). */
        rebox_log("d3d12_xs: console shader %016llx: compiled a variant (%u SPIR-V words, %u guest bindings, "
                     "%u host buffers; key %016llx)", (unsigned long long)hash_bytes(0xcbf29ce484222325ull, req.code,
                     req.code_dwords * 4u), module->count, remap_count, host_count, (unsigned long long)key[0]);
    }

    if (graphics)
        phase_end(&mark, 3);
    memset(out, 0, sizeof(*out));
    out->spirv = module->words;
    out->spirv_words = module->count;
    out->subgroup_size = sa5.subgroup_size(result);
    memcpy(out->key, key, sizeof(key));
    push = sa5.push_constants(result, &push_size);
    memcpy(out->push, push, min(push_size, graphics ? GRAPHICS_PUSH_DATA : 128u));
    for (UINT32 i = 0; i < host_count && !graphics; i++)
        memcpy(&out->push[32 + 2 * i], &host_va[i], sizeof(UINT64));
    if (graphics)
    {
        /* The draw block: the per-draw host buffers' addresses, then every remapped binding's heap
         * slots (sa5_finalize_draw); its address follows the stage's push data. */
        static UINT32 block[2 * SA5_DRAW_ADDRESSES + MAX_ELEMENTS]; /* under compile_lock */
        UINT64 va;
        memset(block, 0, 2 * SA5_DRAW_ADDRESSES * sizeof(UINT32));
        memcpy(block, pushed_va, pushed_count * sizeof(UINT64));
        memcpy(block + 2 * SA5_DRAW_ADDRESSES, indices, index_count * sizeof(UINT32));
        if (!(va = draw_buffer(block, (2 * SA5_DRAW_ADDRESSES + max(index_count, 1u)) * sizeof(UINT32), &block_cpu)))
        {
            log_failure(h, "no GPU memory for the draw block", NULL);
            goto done;
        }
        memcpy(&out->push[GRAPHICS_PUSH_DATA / 4], &va, sizeof(UINT64));
        if (pending_count && block_cpu)
        {
            AcquireSRWLockExclusive(&late_lock);
            for (UINT i = 0; i < pending_count && late_count < ARRAYSIZE(late); i++)
                late[late_count++] = (struct late_texture){
                        (UINT32 *)(block_cpu + (2 * SA5_DRAW_ADDRESSES + pending[i].position) * sizeof(UINT32)),
                        pending[i].source, pending[i].heap, 0 };
            ReleaseSRWLockExclusive(&late_lock);
        }
    }
    if (graphics && h->stage == 0x1)
    {
        out->vertex_input_count = min(sa5.vertex_input_count(result), 32u);
        for (UINT32 i = 0; i < out->vertex_input_count; i++)
            sa5.vertex_input(result, i, &out->vertex_inputs[i][0], &out->vertex_inputs[i][1], &out->vertex_inputs[i][2]);
        {
            static LONG shown;
            char line[256];
            int at = 0;
            for (UINT32 i = 0; i < out->vertex_input_count && at < 200; i++)
                at += snprintf(line + at, sizeof(line) - at, " %u:%u/%u", out->vertex_inputs[i][0],
                               out->vertex_inputs[i][1], out->vertex_inputs[i][2]);
            line[at] = 0;
            if (InterlockedIncrement(&shown) <= 12)
                rebox_log("d3d12_xs: console vertex shader %016llx: %u inputs (location:components/fetch)%s",
                             (unsigned long long)stage_id, out->vertex_input_count, line);
        }
    }
    hr = S_OK;
    {
        char detail[96];
        snprintf(detail, sizeof(detail), "%u SPIR-V words, %u guest bindings, %u host buffers", module->count,
                 remap_count, host_count);
        set_last(S_OK, words ? "compiled" : "cached", detail);
    }

done:
    ReleaseSRWLockExclusive(&compile_lock);
    if (started.QuadPart)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        InterlockedAdd64(graphics ? &graphics_ticks : &compute_ticks, now.QuadPart - started.QuadPart);
    }
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
