/* Derived pipeline states (device slot 55).
 *
 * The console creates a graphics PSO as a variant of an existing one: (base PSO, patch count,
 * patches, riid, out), each patch a 0x14C-byte record, a u32 tag and the replacement state at
 * +4 (TomBox, from a UE5 title's RHI; facts only):
 *   0  D3D12_DEPTH_STENCIL_DESC (0x34 bytes)   1  D3D12_RASTERIZER_DESC (0x2C)
 *   2  D3D12_BLEND_DESC (0x148)                3  sample mask (u32)
 *   5  console-only state with no PC field (ignored).
 * PC D3D12 can't derive, so every graphics PSO's desc is kept, as the title passed it (shaders
 * are substituted again when the variant is created), with what it points at copied: shader
 * bytecode (shared between PSOs by content), the input layout and stream output. A PSO address
 * vkd3d-proton reuses gets the new PSO's record. */

#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdlib.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "derived_pso.h"

struct blob
{
    struct blob *next;
    UINT64 hash;
    SIZE_T size;
    BYTE data[];
};

struct record
{
    struct record *next;
    const void *pso;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    BYTE storage[]; /* input layout and stream output, with their strings */
};

#define BUCKETS 4096
static struct blob *blobs[BUCKETS];
static struct record *records[BUCKETS];
static SRWLOCK lock = SRWLOCK_INIT;

static UINT64 fnv1a(const void *data, SIZE_T size)
{
    const BYTE *p = data;
    UINT64 h = 0xcbf29ce484222325ull;

    while (size--)
        h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

/* Called with the lock held. Blobs are never freed. */
static D3D12_SHADER_BYTECODE intern(D3D12_SHADER_BYTECODE bc)
{
    D3D12_SHADER_BYTECODE out = { 0 };
    struct blob *b;
    UINT64 h;

    if (!bc.pShaderBytecode || !bc.BytecodeLength)
        return out;
    h = fnv1a(bc.pShaderBytecode, bc.BytecodeLength);
    for (b = blobs[h % BUCKETS]; b; b = b->next)
        if (b->hash == h && b->size == bc.BytecodeLength && !memcmp(b->data, bc.pShaderBytecode, b->size))
            break;
    if (!b && (b = malloc(sizeof(*b) + bc.BytecodeLength)))
    {
        b->hash = h;
        b->size = bc.BytecodeLength;
        memcpy(b->data, bc.pShaderBytecode, b->size);
        b->next = blobs[h % BUCKETS];
        blobs[h % BUCKETS] = b;
    }
    if (b)
    {
        out.pShaderBytecode = b->data;
        out.BytecodeLength = b->size;
    }
    return out;
}

static size_t bucket(const void *pso)
{
    return ((ULONG_PTR)pso >> 4) % BUCKETS;
}

static SIZE_T string_size(const char *s)
{
    return s ? strlen(s) + 1 : 0;
}

static char *put_string(BYTE **cursor, const char *s)
{
    char *out = (char *)*cursor;

    if (!s)
        return NULL;
    strcpy(out, s);
    *cursor += strlen(s) + 1;
    return out;
}

void derived_pso_record(const void *pso, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
    const D3D12_INPUT_LAYOUT_DESC *il = &desc->InputLayout;
    const D3D12_STREAM_OUTPUT_DESC *so = &desc->StreamOutput;
    struct record *r, **slot;
    SIZE_T size = 0;
    BYTE *cursor;
    UINT i;

    if (!pso || !desc)
        return;
    if (il->pInputElementDescs)
        size += il->NumElements * sizeof(*il->pInputElementDescs);
    for (i = 0; il->pInputElementDescs && i < il->NumElements; i++)
        size += string_size(il->pInputElementDescs[i].SemanticName);
    if (so->pSODeclaration)
        size += so->NumEntries * sizeof(*so->pSODeclaration);
    for (i = 0; so->pSODeclaration && i < so->NumEntries; i++)
        size += string_size(so->pSODeclaration[i].SemanticName);
    if (so->pBufferStrides)
        size += so->NumStrides * sizeof(*so->pBufferStrides);
    if (!(r = malloc(sizeof(*r) + size)))
        return;
    r->pso = pso;
    r->desc = *desc;
    memset(&r->desc.CachedPSO, 0, sizeof(r->desc.CachedPSO));
    cursor = r->storage;
    if (il->pInputElementDescs)
    {
        D3D12_INPUT_ELEMENT_DESC *e = (D3D12_INPUT_ELEMENT_DESC *)cursor;

        memcpy(e, il->pInputElementDescs, il->NumElements * sizeof(*e));
        cursor += il->NumElements * sizeof(*e);
        for (i = 0; i < il->NumElements; i++)
            e[i].SemanticName = put_string(&cursor, e[i].SemanticName);
        r->desc.InputLayout.pInputElementDescs = e;
    }
    if (so->pSODeclaration)
    {
        D3D12_SO_DECLARATION_ENTRY *e = (D3D12_SO_DECLARATION_ENTRY *)cursor;

        memcpy(e, so->pSODeclaration, so->NumEntries * sizeof(*e));
        cursor += so->NumEntries * sizeof(*e);
        for (i = 0; i < so->NumEntries; i++)
            e[i].SemanticName = put_string(&cursor, e[i].SemanticName);
        r->desc.StreamOutput.pSODeclaration = e;
    }
    if (so->pBufferStrides)
    {
        memcpy(cursor, so->pBufferStrides, so->NumStrides * sizeof(*so->pBufferStrides));
        r->desc.StreamOutput.pBufferStrides = (const UINT *)cursor;
    }

    AcquireSRWLockExclusive(&lock);
    r->desc.VS = intern(desc->VS);
    r->desc.PS = intern(desc->PS);
    r->desc.DS = intern(desc->DS);
    r->desc.HS = intern(desc->HS);
    r->desc.GS = intern(desc->GS);
    for (slot = &records[bucket(pso)]; *slot; slot = &(*slot)->next)
        if ((*slot)->pso == pso)
        {
            struct record *old = *slot;
            r->next = old->next;
            *slot = r;
            ReleaseSRWLockExclusive(&lock);
            free(old);
            return;
        }
    r->next = NULL;
    *slot = r;
    ReleaseSRWLockExclusive(&lock);
}

/* The base's desc with the patches applied, in *desc; the input layout and stream output stay
 * the base record's, so the record mustn't be replaced while it's used (the title would have to
 * destroy the base PSO while deriving from it). */
HRESULT derived_pso_desc(const void *base, UINT count, const void *patches, D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
    const BYTE *p = patches;
    struct record *r;
    UINT i;

    AcquireSRWLockShared(&lock);
    for (r = records[bucket(base)]; r && r->pso != base; r = r->next)
        ;
    if (r)
        *desc = r->desc;
    ReleaseSRWLockShared(&lock);
    if (!r)
    {
        rebox_log("d3d12_xs: derived PSO from %p, which isn't a graphics PSO we created", base);
        return E_INVALIDARG;
    }
    for (i = 0; p && i < count; i++, p += DERIVED_PSO_PATCH_SIZE)
    {
        UINT tag = *(const UINT *)p;
        const BYTE *payload = p + 4;

        switch (tag)
        {
        case 0: memcpy(&desc->DepthStencilState, payload, sizeof(desc->DepthStencilState)); break;
        case 1: memcpy(&desc->RasterizerState, payload, sizeof(desc->RasterizerState)); break;
        case 2: memcpy(&desc->BlendState, payload, sizeof(desc->BlendState)); break;
        case 3: memcpy(&desc->SampleMask, payload, sizeof(desc->SampleMask)); break;
        case 5: break;
        default:
        {
            static LONG seen[64];
            if (tag >= 64 || !InterlockedExchange(&seen[tag], 1))
                rebox_log("d3d12_xs: derived PSO patch tag %u unknown, ignored", tag);
        }
        }
    }
    return S_OK;
}
