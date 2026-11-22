#include "console_fssm.h"
#include "shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"

struct entry
{
    struct entry *next;
    UINT64 hash;
    void *data; /* NULL for a recorded miss */
    SIZE_T size;
};

static SRWLOCK lock = SRWLOCK_INIT;
static struct entry *entries;
static INIT_ONCE config_once = INIT_ONCE_STATIC_INIT;
static char *cache_dir;
static BOOL dump_shaders;
static LONG hits, misses;

UINT64 shader_hash(const void *data, SIZE_T size)
{
    const BYTE *p = data;
    UINT64 h = 0xcbf29ce484222325ull;

    while (size--)
    {
        h ^= *p++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static UINT32 read_u32(const BYTE *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (UINT32)p[3] << 24;
}

/* Console shaders also come in DXBC containers, but their DXIL part is a
 * NOBC stub (no LLVM bitcode) wrapping a zlib FSSM stream with the GFX10.3
 * code. Only containers holding something a PC D3D12 can compile pass
 * through: a SHEX/SHDR part (DXBC bytecode) or a DXIL part whose program
 * header points at LLVM bitcode. Anything else is looked up in the cache;
 * misclassifying a PC shader as console only costs a lookup and a miss. */
static BOOL is_pc_bytecode(const BYTE *data, SIZE_T size)
{
    UINT32 count, i;

    /* Translated console SPIR-V for the patched vkd3d-proton (patches/
     * vkd3d-proton/0002, docs/shaders.md) goes through unchanged. */
    if (size >= 36 && !memcmp(data, "SCSV", 4))
        return TRUE;
    /* So do deferred console shaders (console_compile.h, patch 0005). */
    if (size >= 64 && !memcmp(data, "SCRD", 4))
        return TRUE;
    if (size < 0x20 || memcmp(data, "DXBC", 4))
        return FALSE;
    count = read_u32(data + 0x1c);
    if (count > (size - 0x20) / 4)
        return FALSE;
    for (i = 0; i < count; i++)
    {
        UINT32 offset = read_u32(data + 0x20 + i * 4), part_size, bitcode;
        const BYTE *part;

        if (offset > size || size - offset < 8)
            continue;
        part = data + offset + 8;
        part_size = read_u32(data + offset + 4);
        if (part_size > size - offset - 8)
            continue;
        if (!memcmp(data + offset, "SHEX", 4) || !memcmp(data + offset, "SHDR", 4))
            return TRUE;
        /* DxilProgramHeader: version, size in dwords, then DxilBitcodeHeader:
         * "DXIL", version, bitcode offset (from the "DXIL" magic), size. */
        if (memcmp(data + offset, "DXIL", 4) || part_size < 24 || memcmp(part + 8, "DXIL", 4))
            continue;
        bitcode = read_u32(part + 16);
        if (bitcode <= part_size - 8 - 4 && !memcmp(part + 8 + bitcode, "BC\xc0\xde", 4))
            return TRUE;
    }
    return FALSE;
}

static BOOL CALLBACK config_init(INIT_ONCE *once, void *param, void **ctx)
{
    char *dump;

    cache_dir = rebox_getenv("REBOX_SHADER_CACHE");
    if ((dump = rebox_getenv("REBOX_DUMP_SHADERS")))
    {
        dump_shaders = dump[0] && dump[0] != '0';
        free(dump);
    }
    if (cache_dir)
        rebox_log("d3d12_xs: shader cache %s%s", cache_dir, dump_shaders ? " (dumping misses)" : "");
    else
        rebox_log("d3d12_xs: REBOX_SHADER_CACHE not set, console shaders pass through untranslated");
    return TRUE;
}

static struct entry *find(UINT64 hash)
{
    struct entry *e;

    for (e = entries; e; e = e->next)
        if (e->hash == hash)
            return e;
    return NULL;
}

static void *read_file(const char *path, SIZE_T *size)
{
    FILE *f = fopen(path, "rb");
    void *data = NULL;
    long len;

    if (!f)
        return NULL;
    if (!fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && !fseek(f, 0, SEEK_SET)
        && (data = malloc(len)) && fread(data, 1, len, f) == (size_t)len)
        *size = len;
    else
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static void record_miss(UINT64 hash, const D3D12_SHADER_BYTECODE *in, const char *stage)
{
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof(path), "%s\\misses.txt", cache_dir);
    if ((f = fopen(path, "a")))
    {
        fprintf(f, "%016llx %s %zu\n", (unsigned long long)hash, stage, (size_t)in->BytecodeLength);
        fclose(f);
    }
    if (dump_shaders)
    {
        snprintf(path, sizeof(path), "%s\\dump", cache_dir);
        CreateDirectoryA(path, NULL);
        snprintf(path, sizeof(path), "%s\\dump\\%016llx.%s.bin", cache_dir, (unsigned long long)hash, stage);
        if ((f = fopen(path, "wb")))
        {
            fwrite(in->pShaderBytecode, 1, in->BytecodeLength, f);
            fclose(f);
        }
    }
}

static void describe_bytecode(const D3D12_SHADER_BYTECODE *bc, char *out, size_t len)
{
    const BYTE *d = bc->pShaderBytecode;

    if (!d || !bc->BytecodeLength)
        snprintf(out, len, "none");
    else if (bc->BytecodeLength >= 36 && !memcmp(d, "SCSV", 4))
        snprintf(out, len, "scsv %zu B", (size_t)bc->BytecodeLength);
    else if (bc->BytecodeLength >= 64 && !memcmp(d, "SCRD", 4))
        snprintf(out, len, "scrd %zu B", (size_t)bc->BytecodeLength);
    else if (bc->BytecodeLength >= 4 && !memcmp(d, "DXBC", 4))
        snprintf(out, len, "%s %zu B", is_pc_bytecode(d, bc->BytecodeLength) ? "pc-dxbc" : "console-dxbc",
                 (size_t)bc->BytecodeLength);
    else if (bc->BytecodeLength >= 4)
        snprintf(out, len, "magic %02x%02x%02x%02x %zu B", d[0], d[1], d[2], d[3], (size_t)bc->BytecodeLength);
    else
        snprintf(out, len, "%zu B", (size_t)bc->BytecodeLength);
}

void shader_cache_census(const char *kind, const char *const *names, const D3D12_SHADER_BYTECODE *stages,
                         UINT count, const D3D12_CACHED_PIPELINE_STATE *cached)
{
    static LONG psos;
    LONG n = InterlockedIncrement(&psos);
    char line[512], part[96];
    size_t used;

    InitOnceExecuteOnce(&config_once, config_init, NULL, NULL);
    if (n > 8 && (n & (n - 1)))
        return;
    used = snprintf(line, sizeof(line), "d3d12_xs: PSO #%ld (%s):", n, kind);
    for (UINT i = 0; i < count && used < sizeof(line); i++)
    {
        if (!stages[i].BytecodeLength)
            continue;
        describe_bytecode(&stages[i], part, sizeof(part));
        used += snprintf(line + used, sizeof(line) - used, " %s %s;", names[i], part);
    }
    if (cached && cached->pCachedBlob && cached->CachedBlobSizeInBytes && used < sizeof(line))
    {
        D3D12_SHADER_BYTECODE blob = { cached->pCachedBlob, cached->CachedBlobSizeInBytes };
        describe_bytecode(&blob, part, sizeof(part));
        snprintf(line + used, sizeof(line) - used, " CachedPSO %s", part);
        if (dump_shaders && cache_dir)
        {
            char path[MAX_PATH];
            FILE *f;

            snprintf(path, sizeof(path), "%s\\dump", cache_dir);
            CreateDirectoryA(path, NULL);
            snprintf(path, sizeof(path), "%s\\dump\\%016llx.cachedpso.bin", cache_dir,
                     (unsigned long long)shader_hash(blob.pShaderBytecode, blob.BytecodeLength));
            if ((f = fopen(path, "wb")))
            {
                fwrite(blob.pShaderBytecode, 1, blob.BytecodeLength, f);
                fclose(f);
            }
        }
    }
    rebox_log("%s", line);
}

/* Console compute shaders become deferred console shaders (SCRD), compiled with AnyPS5 at each
 * dispatch (console_fssm.c, console_compile.c), unless REBOX_CONSOLE_COMPUTE=0. Built once per
 * container and kept, like cache entries. */
static BOOL deferred_compute(const D3D12_SHADER_BYTECODE *in, D3D12_SHADER_BYTECODE *out)
{
    static SRWLOCK scrd_lock = SRWLOCK_INIT;
    static struct entry *scrds;
    static int enabled = -1;
    UINT64 hash;
    struct entry *e;

    if (enabled < 0)
    {
        char *env = rebox_getenv("REBOX_CONSOLE_COMPUTE");
        enabled = !(env && env[0] == '0');
        free(env);
    }
    if (!enabled)
        return FALSE;
    hash = shader_hash(in->pShaderBytecode, in->BytecodeLength);
    AcquireSRWLockExclusive(&scrd_lock);
    for (e = scrds; e && e->hash != hash; e = e->next)
        ;
    if (!e && (e = calloc(1, sizeof(*e))))
    {
        e->hash = hash;
        e->data = console_fssm_compute(in->pShaderBytecode, in->BytecodeLength, &e->size);
        e->next = scrds;
        scrds = e;
        if (e->data)
        {
            static LONG logged;
            if (!InterlockedExchange(&logged, 1))
                rebox_log("d3d12_xs: console compute shaders run through AnyPS5 at dispatch (first: %016llx)",
                             (unsigned long long)hash);
        }
    }
    ReleaseSRWLockExclusive(&scrd_lock);
    if (!e || !e->data)
        return FALSE;
    out->pShaderBytecode = e->data;
    out->BytecodeLength = e->size;
    return TRUE;
}

enum shader_lookup shader_cache_substitute(const D3D12_SHADER_BYTECODE *in, D3D12_SHADER_BYTECODE *out,
                                           const char *stage)
{
    char path[MAX_PATH];
    struct entry *e;
    UINT64 hash;

    InitOnceExecuteOnce(&config_once, config_init, NULL, NULL);

    if (!in->pShaderBytecode || !in->BytecodeLength)
        return SHADER_NONE;
    if (is_pc_bytecode(in->pShaderBytecode, in->BytecodeLength))
        return SHADER_PC;
    if (!strcmp(stage, "cs") && deferred_compute(in, out))
        return SHADER_HIT;
    if (!cache_dir)
    {
        static LONG once;
        if (!InterlockedExchange(&once, 1))
            rebox_log("d3d12_xs: console shader but no REBOX_SHADER_CACHE; every console shader misses");
        return SHADER_MISS;
    }

    hash = shader_hash(in->pShaderBytecode, in->BytecodeLength);

    AcquireSRWLockShared(&lock);
    e = find(hash);
    ReleaseSRWLockShared(&lock);

    if (!e)
    {
        AcquireSRWLockExclusive(&lock);
        if (!(e = find(hash)) && (e = calloc(1, sizeof(*e))))
        {
            e->hash = hash;
            /* A translated console shader for the patched vkd3d-proton
             * (tools/scsv_pack.py, docs/shaders.md), else a PC-format one. */
            snprintf(path, sizeof(path), "%s\\%016llx.scsv", cache_dir, (unsigned long long)hash);
            if (!(e->data = read_file(path, &e->size)))
            {
                snprintf(path, sizeof(path), "%s\\%016llx.dxil", cache_dir, (unsigned long long)hash);
                e->data = read_file(path, &e->size);
            }
            if (!e->data)
            {
                InterlockedIncrement(&misses);
                rebox_log("d3d12_xs: no translation for %s shader %016llx (%zu bytes)",
                             stage, (unsigned long long)hash, (size_t)in->BytecodeLength);
                record_miss(hash, in, stage);
            }
            else
                InterlockedIncrement(&hits);
            e->next = entries;
            entries = e;
        }
        ReleaseSRWLockExclusive(&lock);
    }

    if (!e || !e->data)
        return SHADER_MISS;
    out->pShaderBytecode = e->data;
    out->BytecodeLength = e->size;
    return SHADER_HIT;
}
