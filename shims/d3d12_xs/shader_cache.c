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
            snprintf(path, sizeof(path), "%s\\%016llx.dxil", cache_dir, (unsigned long long)hash);
            if (!(e->data = read_file(path, &e->size)))
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
