/* Smoke test for the shims, run under Wine by tests/run-smoke.sh.
 *
 *   smoke.exe FAKE_CONSOLE_SHADER [nocache]
 *
 * Exercises xmem.dll directly, then creates a device through d3d12_xs.dll and
 * builds compute PSOs from FAKE_CONSOLE_SHADER (bytes vkd3d-proton cannot
 * compile). Those only succeed if the shader cache substitutes a translation. */

#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <d3d12.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../generated/test_shaders.h"
#include "../anyps5/rebox_anyps5.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (cond) printf("ok   " __VA_ARGS__); \
    else { printf("FAIL " __VA_ARGS__); failures++; } \
    printf("\n"); } while (0)

typedef void *(WINAPI *alloc_fn)(SIZE_T, ULONGLONG);
typedef void (WINAPI *free_fn)(void *, ULONGLONG);
typedef void (WINAPI *hooks_fn)(alloc_fn, free_fn);
/* Layout as XboxBvhBuilder.dll uses it: MEMORY_BASIC_INFORMATION, then GPU access bits. */
typedef struct { MEMORY_BASIC_INFORMATION mbi; ULONG gpu_access, reserved[3]; } xmem_info;
typedef BOOL (WINAPI *vquery_fn)(HANDLE, const void *, xmem_info *);
typedef BOOL (WINAPI *wss_fn)(ULONG, void *);

static alloc_fn alloc_default;
static free_fn free_default;
static int hook_calls;

static void *WINAPI my_alloc(SIZE_T size, ULONGLONG attr) { hook_calls++; return alloc_default(size, attr); }
static void WINAPI my_free(void *p, ULONGLONG attr) { hook_calls++; free_default(p, attr); }

/* Faults xmem's reporter should describe (crash.c): a read of 0x10, then one
 * inside the GPU window. This handler runs after xmem's and skips the
 * 2-byte "mov eax, [rax]" so the test continues; run-smoke.sh checks the
 * report in the log. */
static LONG CALLBACK skip_test_fault(EXCEPTION_POINTERS *ep)
{
    BYTE *rip = (BYTE *)ep->ContextRecord->Rip;

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rip[0] == 0x8b && rip[1] == 0x00
        && (ep->ContextRecord->Rax == 0x10 || ep->ContextRecord->Rax == 0x20ff00000040ull))
    {
        ep->ContextRecord->Rip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void __attribute__((noinline)) fault_read(ULONG_PTR addr)
{
    __asm__ volatile("movl (%%rax), %%eax" : "+a"(addr) :: "memory");
}

static void test_crash_report(HMODULE xmem)
{
    typedef BOOL (WINAPI *vquery_fn2)(HANDLE, const void *, void *);
    vquery_fn2 vquery = (vquery_fn2)(void *)GetProcAddress(xmem, "XMemVirtualQuery");
    void *veh = AddVectoredExceptionHandler(0, skip_test_fault);
    BOOL ok;

    fault_read(0x10);
    fault_read(0x20ff00000040ull);
    RemoveVectoredExceptionHandler(veh);
    printf("ok   resumed after two reported faults\n");

    SetLastError(0);
    ok = vquery(GetCurrentProcess(), &ok, NULL);
    CHECK(!ok && GetLastError() == ERROR_NOACCESS, "XMemVirtualQuery with a null buffer fails with ERROR_NOACCESS (%lu)",
          GetLastError());
}

static void test_xmem(void)
{
    HMODULE m = LoadLibraryA("xmem.dll");
    alloc_fn xalloc;
    free_fn xfree;
    hooks_fn set_hooks;
    BYTE *a, *b;

    CHECK(m != NULL, "load xmem.dll");
    if (!m) return;
    xalloc = (alloc_fn)(void *)GetProcAddress(m, "XMemAlloc");
    xfree = (free_fn)(void *)GetProcAddress(m, "XMemFree");
    set_hooks = (hooks_fn)(void *)GetProcAddress(m, "XMemSetAllocationHooks");
    alloc_default = (alloc_fn)(void *)GetProcAddress(m, "XMemAllocDefault");
    free_default = (free_fn)(void *)GetProcAddress(m, "XMemFreeDefault");
    CHECK(xalloc && xfree && set_hooks && alloc_default && free_default, "xmem exports present");
    if (!(xalloc && xfree && set_hooks && alloc_default && free_default)) return;
    test_crash_report(m);

    /* alignment field (bits 24-28) = 7 -> 2^7 = 128-byte alignment, heap path */
    a = xalloc(100, 7ull << 24);
    CHECK(a && !((ULONG_PTR)a & 127), "XMemAlloc aligned heap allocation %p", a);
    if (a) { a[0] = a[99] = 1; xfree(a, 7ull << 24); }

    /* Blocks come back zeroed even when the allocator reuses a dirty address
     * (Xbox2Windows' era_heap_reuse): heap path and GPU-visible path. */
    {
        static const ULONGLONG kinds[] = { 5ull << 24, 0xa8004000 };
        for (int k = 0; k < 2; k++)
        {
            BYTE *held[32] = { 0 }, *dirty = xalloc(4096, kinds[k]);
            BOOL zero = dirty != NULL, reused = FALSE;
            int n;

            if (dirty) { memset(dirty, 0xa5, 4096); xfree(dirty, kinds[k]); }
            for (n = 0; n < 32 && zero && !reused; n++)
            {
                if (!(held[n] = xalloc(4096, kinds[k]))) { zero = FALSE; break; }
                for (int i = 0; i < 4096; i++)
                    zero &= held[n][i] == 0;
                reused = held[n] == dirty;
            }
            CHECK(zero, "XMemAlloc %#llx blocks are zeroed (dirty address reused: %s)",
                  kinds[k], reused ? "yes" : "no");
            while (n-- > 0)
                if (held[n]) xfree(held[n], kinds[k]);
        }
    }

    /* memory_type field (bits 29-32) != 0 -> VirtualAlloc path */
    b = xalloc(1 << 20, 1ull << 29);
    CHECK(b && !((ULONG_PTR)b & 0xffff), "XMemAlloc page allocation %p", b);
    if (b) { b[0] = b[(1 << 20) - 1] = 1; xfree(b, 1ull << 29); }

    /* Attribute words real callers pass (Xbox2Windows call-site notes; not checked against a
     * disassembly here). Bink's fallback allocator asks for len + 64 with 0x05e50000, steps
     * the base forward 33-64 bytes to a 32-byte boundary, stores the step at [-1] and a tag
     * at [-2], and later frees base = user - step with the same word. Here: alignment 5
     * (32 bytes), memory_type 0, so the heap path. */
    {
        const ULONGLONG bink = 0x05e50000;
        BYTE *base = xalloc(1000 + 64, bink), *user;

        CHECK(base && !((ULONG_PTR)base & 31), "XMemAlloc Bink attributes %#llx -> %p", bink, base);
        if (base)
        {
            user = base + 64 - ((ULONG_PTR)base & 31);
            user[-1] = (BYTE)(user - base);
            user[-2] = 0xb1;
            memset(user, 0x5a, 1000);
            CHECK(!((ULONG_PTR)user & 31) && user + 1000 <= base + 1064 && user - user[-1] == base,
                  "Bink-style 32-byte user pointer fits and recovers its base");
            xfree(user - user[-1], bink);
        }
    }

    /* The AMD upscaler rounds sizes up to 256 and passes 0xa8004000 (its on-disk default),
     * then copies shader bytes in and hands the pointer to a compute PSO, so the GPU reads
     * it. Here: alignment 8 (256 bytes, matching its rounding), page_size 1 (64 KiB pages, so a
     * 64 KiB boundary, as Xbox2Windows' xmem.dll gives), memory_type 5, so GPU-visible memory,
     * which XMemVirtualQuery must report as GPU-accessible. */
    {
        const ULONGLONG amd = 0xa8004000;
        vquery_fn vquery = (vquery_fn)(void *)GetProcAddress(m, "XMemVirtualQuery");
        BYTE *p = xalloc(0x1100, amd);
        xmem_info info;

        CHECK(p && !((ULONG_PTR)p & 0xffff), "XMemAlloc AMD attributes %#llx -> %p", amd, p);
        if (p)
        {
            memset(p, 0xa5, 0x1100);
            memset(&info, 0xcc, sizeof(info));
            CHECK(vquery && vquery(GetCurrentProcess(), p + 0x10ff, &info) && info.mbi.State == MEM_COMMIT
                  && (info.gpu_access & 3) == 3,
                  "AMD allocation is committed and GPU-accessible (state %#lx gpu_access %#lx)",
                  info.mbi.State, info.gpu_access);
            xfree(p, amd);
        }
    }

    /* 4 MiB pages (page_size 2) start on a 4 MiB boundary, GPU-visible
     * (memory_type 1) and CPU heap pages alike. */
    {
        static const ULONGLONG words[] = { (1ull << 29) | (2ull << 14), 2ull << 14 };
        for (int k = 0; k < 2; k++)
        {
            BYTE *p = xalloc(100 << 10, words[k]);
            CHECK(p && !((ULONG_PTR)p & ((4 << 20) - 1)), "XMemAlloc 4 MiB pages %#llx -> %p", words[k], p);
            if (p) { p[0] = p[(100 << 10) - 1] = 1; xfree(p, words[k]); }
        }
    }

    set_hooks(my_alloc, my_free);
    xfree(xalloc(16, 0), 0);
    set_hooks(NULL, NULL);
    CHECK(hook_calls == 2, "allocation hooks called (%d)", hook_calls);

    /* Called as XboxBvhBuilder.dll does: (process, address, info), 0x40-byte buffer. */
    {
        vquery_fn vquery = (vquery_fn)(void *)GetProcAddress(m, "XMemVirtualQuery");
        BYTE *p = xalloc(1 << 16, 1ull << 29);
        xmem_info info;

        CHECK(vquery && p, "XMemVirtualQuery present");
        if (vquery && p)
        {
            memset(&info, 0xcc, sizeof(info));
            CHECK(vquery(GetCurrentProcess(), p + 100, &info) && info.mbi.State == MEM_COMMIT
                  && (info.mbi.Protect & PAGE_READWRITE) && (info.gpu_access & 3) == 3,
                  "XMemVirtualQuery state %#lx protect %#lx gpu_access %#lx",
                  info.mbi.State, info.mbi.Protect, info.gpu_access);
            xfree(p, 1ull << 29);
        }
    }

    /* Called as the AMD upscaler does: (0x20000001, &local), reading the u64 at +8. */
    {
        wss_fn wss = (wss_fn)(void *)GetProcAddress(m, "XMemGetWorkingSetStatistics");
        ULONGLONG out[4], before, after;
        BYTE *p;

        CHECK(wss != NULL, "XMemGetWorkingSetStatistics present");
        if (wss)
        {
            memset(out, 0xcc, sizeof(out));
            CHECK(wss(0x20000001, out) && out[1] != 0xccccccccccccccccull
                  && out[0] == 0xccccccccccccccccull && out[2] == 0xccccccccccccccccull,
                  "XMemGetWorkingSetStatistics writes only +8 (%#llx)", out[1]);
            before = out[1];
            p = xalloc(16 << 20, 1ull << 29);
            if (p) memset(p, 1, 16 << 20); /* Wine counts only touched pages */
            wss(0x20000001, out);
            after = out[1];
            CHECK(p && after >= before + (16 << 20),
                  "XMemGetWorkingSetStatistics tracks allocations (+%lld bytes)", (long long)(after - before));
            if (p) xfree(p, 1ull << 29);
            memset(out, 0xcc, sizeof(out));
            CHECK(!wss(0x1234, out) && out[1] == 0xccccccccccccccccull,
                  "XMemGetWorkingSetStatistics rejects unknown selector");
        }
    }
}

static BYTE *read_file(const char *path, SIZE_T *size)
{
    FILE *f = fopen(path, "rb");
    BYTE *data;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(len);
    fread(data, 1, len, f);
    fclose(f);
    *size = len;
    return data;
}

static void test_d3d12(const char *fake_path, BOOL expect_hit)
{
    static const char *forwarded[] = {
        "D3D12GetDebugInterface", "D3D12SerializeRootSignature", "D3D12SerializeVersionedRootSignature",
        "D3D12CreateRootSignatureDeserializer", "D3D12CreateVersionedRootSignatureDeserializer",
        "D3D12EnableExperimentalFeatures", "D3D12GetInterface",
    };
    HMODULE m = LoadLibraryA("d3d12_xs.dll");
    PFN_D3D12_CREATE_DEVICE create_device;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cdesc = {0};
    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    ID3D10Blob *rs_blob = NULL, *err = NULL;
    ID3D12RootSignature *rs = NULL;
    ID3D12PipelineState *pso;
    ID3D12Device2 *device;
    SIZE_T fake_size;
    BYTE *fake;
    unsigned int i;
    HRESULT hr;

    CHECK(m != NULL, "load d3d12_xs.dll");
    if (!m) return;
    for (i = 0; i < ARRAYSIZE(forwarded); i++)
        CHECK(GetProcAddress(m, forwarded[i]) != NULL, "forwarded export %s resolves", forwarded[i]);
    CHECK(GetProcAddress(m, "D3D12XboxCreateDevice") != NULL, "D3D12XboxCreateDevice present");

    create_device = (PFN_D3D12_CREATE_DEVICE)(void *)GetProcAddress(m, "D3D12CreateDevice");
    serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(m, "D3D12SerializeRootSignature");
    hr = create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device2, (void **)&device);
    if (FAILED(hr))
    {
        printf("skip D3D12CreateDevice failed %#lx (no usable Vulkan device?)\n", hr);
        return;
    }
    CHECK(1, "D3D12CreateDevice through d3d12_xs");

    hr = serialize(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err);
    CHECK(SUCCEEDED(hr), "serialize root signature via forwarder (%#lx)", hr);
    if (FAILED(hr)) return;
    hr = ID3D12Device2_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(rs_blob),
            ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature, (void **)&rs);
    CHECK(SUCCEEDED(hr), "create root signature (%#lx)", hr);
    if (FAILED(hr)) return;

    /* Console root signatures carry flags 0x03000000 (XboxBvhBuilder.dll adds them at desc +0x28
     * before serializing; its 52 embedded RTS0 parts all have them). Windows' runtime rejects these
     * bits with E_INVALIDARG. vkd3d-proton ignores them, so d3d12_xs passes them through. If this
     * starts failing, mask them in the forwarders and in CreateRootSignature. */
    {
        PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serialize_v = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)
                (void *)GetProcAddress(m, "D3D12SerializeVersionedRootSignature");
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC vdesc = { .Version = D3D_ROOT_SIGNATURE_VERSION_1_1,
                .Desc_1_1.Flags = 0x03000000 };
        ID3D12RootSignature *console_rs = NULL;
        ID3D10Blob *blob = NULL;

        hr = serialize_v(&vdesc, &blob, NULL);
        CHECK(SUCCEEDED(hr), "serialize root signature with console flags 0x03000000 (%#lx)", hr);
        if (SUCCEEDED(hr))
        {
            hr = ID3D12Device2_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
                    ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&console_rs);
            CHECK(SUCCEEDED(hr), "create root signature with console flags (%#lx)", hr);
            if (SUCCEEDED(hr)) ID3D12RootSignature_Release(console_rs);
            ID3D10Blob_Release(blob);
        }
    }

    if (!(fake = read_file(fake_path, &fake_size)))
    {
        CHECK(0, "read %s", fake_path);
        return;
    }

    cdesc.pRootSignature = rs;
    cdesc.CS.pShaderBytecode = fake;
    cdesc.CS.BytecodeLength = fake_size;
    hr = ID3D12Device2_CreateComputePipelineState(device, &cdesc, &IID_ID3D12PipelineState, (void **)&pso);
    CHECK(SUCCEEDED(hr) == expect_hit, "CreateComputePipelineState with console bytecode %s (%#lx)", expect_hit ? "substituted" : "rejected without cache entry", hr);
    if (SUCCEEDED(hr)) ID3D12PipelineState_Release(pso);

    {
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature *v; } __attribute__((aligned(8))) s_rs;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } __attribute__((aligned(8))) s_cs;
        struct { __typeof__(s_rs) rs; __typeof__(s_cs) cs; } stream;
        D3D12_PIPELINE_STATE_STREAM_DESC sdesc = { sizeof(stream), &stream };

        stream.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        stream.rs.v = rs;
        stream.cs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        stream.cs.v = cdesc.CS;
        hr = ID3D12Device2_CreatePipelineState(device, &sdesc, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) == expect_hit, "CreatePipelineState stream with console bytecode %s (%#lx)", expect_hit ? "substituted" : "rejected without cache entry", hr);
        if (SUCCEEDED(hr)) ID3D12PipelineState_Release(pso);
    }

    /* A graphics stream with the newer subobjects (DEPTH_STENCIL2 = 26,
     * RASTERIZER2 = 28) and untranslated console VS/PS: d3d12_xs must parse
     * past them to substitute fallbacks; strict mode must still fail. */
    {
        const char *fb = getenv("REBOX_SHADER_FALLBACK");
        BOOL strict = fb && *fb == '0';
        static const char vs_bytes[] = "REBOX-FAKE-CONSOLE-VS", ps_bytes[] = "REBOX-FAKE-CONSOLE-PS";
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature *v; } __attribute__((aligned(8))) s_rs;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } __attribute__((aligned(8))) s_bc;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v[10]; } __attribute__((aligned(8))) s_rast2;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; UINT v[15]; } __attribute__((aligned(8))) s_ds2;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; struct D3D12_RT_FORMAT_ARRAY v; } __attribute__((aligned(8))) s_rt;
        struct { __typeof__(s_rs) rs; __typeof__(s_bc) vs; __typeof__(s_rast2) rast; __typeof__(s_ds2) ds;
                 __typeof__(s_bc) ps; __typeof__(s_rt) rt; } gstream;
        D3D12_PIPELINE_STATE_STREAM_DESC gdesc = { sizeof(gstream), &gstream };

        memset(&gstream, 0, sizeof(gstream));
        gstream.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        gstream.rs.v = rs;
        gstream.vs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
        gstream.vs.v.pShaderBytecode = vs_bytes;
        gstream.vs.v.BytecodeLength = sizeof(vs_bytes);
        gstream.rast.t = 28; /* RASTERIZER2: solid, cull back, depth clip on */
        gstream.rast.v[0] = D3D12_FILL_MODE_SOLID;
        gstream.rast.v[1] = D3D12_CULL_MODE_BACK;
        gstream.rast.v[6] = TRUE;
        gstream.ds.t = 26; /* DEPTH_STENCIL2: depth and stencil off */
        gstream.ds.v[1] = D3D12_DEPTH_WRITE_MASK_ALL;
        gstream.ds.v[2] = D3D12_COMPARISON_FUNC_LESS;
        gstream.ps.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
        gstream.ps.v.pShaderBytecode = ps_bytes;
        gstream.ps.v.BytecodeLength = sizeof(ps_bytes);
        gstream.rt.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
        gstream.rt.v.NumRenderTargets = 1;
        gstream.rt.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = ID3D12Device2_CreatePipelineState(device, &gdesc, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr) != strict, "graphics stream with DEPTH_STENCIL2/RASTERIZER2 and console VS/PS %s (%#lx)",
              strict ? "rejected in strict mode" : "gets fallbacks", hr);
        if (SUCCEEDED(hr)) ID3D12PipelineState_Release(pso);
    }

    /* A CachedPSO blob vkd3d-proton didn't make (as a console PSO cache entry
     * would be) must not fail creation: d3d12_xs retries without it. */
    {
        static const BYTE foreign_blob[64] = { 'P', 'S', '0', '9' };
        D3D12_COMPUTE_PIPELINE_STATE_DESC cached_desc = cdesc;

        cached_desc.CS.pShaderBytecode = test_empty_cs;
        cached_desc.CS.BytecodeLength = sizeof(test_empty_cs);
        cached_desc.CachedPSO.pCachedBlob = foreign_blob;
        cached_desc.CachedPSO.CachedBlobSizeInBytes = sizeof(foreign_blob);
        hr = ID3D12Device2_CreateComputePipelineState(device, &cached_desc, &IID_ID3D12PipelineState, (void **)&pso);
        CHECK(SUCCEEDED(hr), "compute PSO with a foreign CachedPSO blob is created without it (%#lx)", hr);
        if (SUCCEEDED(hr)) ID3D12PipelineState_Release(pso);
    }

    /* A mesh pipeline from untranslated console AS/MS/PS gets the mesh
     * fallbacks (when the device has mesh shaders and fallbacks are on). */
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = { 0 };
        const char *fb = getenv("REBOX_SHADER_FALLBACK");
        static const char as_bytes[] = "REBOX-FAKE-CONSOLE-AS", ms_bytes[] = "REBOX-FAKE-CONSOLE-MS",
                          ps_bytes[] = "REBOX-FAKE-CONSOLE-MESH-PS";
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; ID3D12RootSignature *v; } __attribute__((aligned(8))) s_rs;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; } __attribute__((aligned(8))) s_as, s_ms, s_ps;
        struct { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; struct D3D12_RT_FORMAT_ARRAY v; } __attribute__((aligned(8))) s_rt;
        struct { __typeof__(s_rs) rs; __typeof__(s_as) as, ms, ps; __typeof__(s_rt) rt; } mstream;
        D3D12_PIPELINE_STATE_STREAM_DESC mdesc = { sizeof(mstream), &mstream };

        if (FAILED(ID3D12Device2_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))
            || o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED || (fb && *fb == '0'))
            printf("skip mesh fallback check (mesh tier %u)\n", o7.MeshShaderTier);
        else
        {
            memset(&mstream, 0, sizeof(mstream));
            mstream.rs.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            mstream.rs.v = rs;
            mstream.as.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            mstream.as.v.pShaderBytecode = as_bytes;
            mstream.as.v.BytecodeLength = sizeof(as_bytes);
            mstream.ms.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
            mstream.ms.v.pShaderBytecode = ms_bytes;
            mstream.ms.v.BytecodeLength = sizeof(ms_bytes);
            mstream.ps.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
            mstream.ps.v.pShaderBytecode = ps_bytes;
            mstream.ps.v.BytecodeLength = sizeof(ps_bytes);
            mstream.rt.t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
            mstream.rt.v.NumRenderTargets = 1;
            mstream.rt.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            hr = ID3D12Device2_CreatePipelineState(device, &mdesc, &IID_ID3D12PipelineState, (void **)&pso);
            CHECK(SUCCEEDED(hr), "mesh PSO from untranslated console AS/MS/PS gets fallbacks (%#lx)", hr);
            if (SUCCEEDED(hr)) ID3D12PipelineState_Release(pso);
        }
    }

    ID3D12RootSignature_Release(rs);
    ID3D12Device2_Release(device);
}

/* Call an Xbox vtable slot by number, as title code does. */
#define XSLOT(obj, n, ret, ...) ((ret (STDMETHODCALLTYPE *)(void *, ##__VA_ARGS__))((*(void ***)(obj))[n]))

typedef HRESULT (WINAPI *xbox_create_fn)(IUnknown *, const void *, REFIID, void **);
typedef HRESULT (WINAPI *gpu_alloc_fn)(SIZE_T, SIZE_T, void **, UINT64 *);
typedef void (WINAPI *gpu_free_fn)(void *);

static BOOL gpu_identity; /* set by test_gpu_memory */

static void test_gpu_memory(HMODULE m)
{
    gpu_alloc_fn galloc = (gpu_alloc_fn)(void *)GetProcAddress(m, "ReboxGpuAlloc");
    gpu_free_fn gfree = (gpu_free_fn)(void *)GetProcAddress(m, "ReboxGpuFree");
    void *cpu = NULL;
    UINT64 gpu = 0;
    HRESULT hr;

    CHECK(galloc && gfree, "ReboxGpuAlloc/Free exported");
    if (!galloc || !gfree) return;
    hr = galloc(1 << 20, 64 << 10, &cpu, &gpu);
    CHECK(SUCCEEDED(hr) && cpu && gpu && !((ULONG_PTR)cpu & 0xffff), "GPU memory %p, GPU VA %#llx (%#lx)",
          cpu, (unsigned long long)gpu, hr);
    if (FAILED(hr)) return;
    memset(cpu, 0x5a, 1 << 20);
    /* Identity depends on the driver and layer; lavapipe gives it for free. */
    gpu_identity = (UINT64)(ULONG_PTR)cpu == gpu;
    printf("info GPU memory is %sidentity-mapped\n", gpu_identity ? "" : "NOT ");

    /* A second heap (32 MiB gets its own): if the first was identity-mapped,
     * later ones must be too. RADV pads each allocation's VA range, which once
     * made the next address collide. */
    {
        void *cpu2 = NULL;
        UINT64 gpu2 = 0;
        BOOL first = (UINT64)(ULONG_PTR)cpu == gpu;

        hr = galloc(32 << 20, 64 << 10, &cpu2, &gpu2);
        CHECK(SUCCEEDED(hr) && cpu2, "second GPU heap %p, GPU VA %#llx (%#lx)", cpu2, (unsigned long long)gpu2, hr);
        if (SUCCEEDED(hr))
        {
            memset(cpu2, 0x5a, 32 << 20);
            if (first)
                CHECK((UINT64)(ULONG_PTR)cpu2 == gpu2, "second GPU heap is identity-mapped too");
            gfree(cpu2);
        }
        /* Right after a free: RADV releases VA ranges lazily, so an
         * allocator that reuses the freed address gets refused. */
        hr = galloc(32 << 20, 64 << 10, &cpu2, &gpu2);
        CHECK(SUCCEEDED(hr) && cpu2, "GPU heap after a free %p, GPU VA %#llx (%#lx)", cpu2,
              (unsigned long long)gpu2, hr);
        if (SUCCEEDED(hr))
        {
            if (first)
                CHECK((UINT64)(ULONG_PTR)cpu2 == gpu2, "GPU heap after a free is identity-mapped too");
            gfree(cpu2);
        }
    }
    gfree(cpu);
}

/* Reads pixel (0,0) of a 32-bit BMP written by PresentX's frame dump. */
static BOOL read_dump_pixel(const char *dir, int frame, BYTE bgra[4])
{
    char path[MAX_PATH];
    BYTE header[58];
    FILE *f;
    BOOL ok;

    snprintf(path, sizeof(path), "%s\\frame_%d.bmp", dir, frame);
    if (!(f = fopen(path, "rb")))
        return FALSE;
    ok = fread(header, 1, sizeof(header), f) == sizeof(header) && header[0] == 'B' && header[1] == 'M';
    fclose(f);
    if (ok)
        memcpy(bgra, header + 54, 4);
    return ok;
}

/* Render a cleared target through Xbox slots, then PresentX it (queue slot
 * 30), once with the resource in the plane params and once with a pointer
 * to it. The frame dumps (run-smoke.sh sets REBOX_DUMP_FRAMES) show
 * whether the right pixels were copied out; a window is shown only if
 * there's a display. */
static void test_present(void *dev, void *queue)
{
    const char *dump_dir = getenv("REBOX_DUMP_FRAMES");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 48, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE clear = { DXGI_FORMAT_R8G8B8A8_UNORM, { { 1.0f, 0.5f, 0.25f, 1.0f } } };
    D3D12_RESOURCE_BARRIER to_present = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *tex = NULL;
    void *rtv_heap = NULL, *list = NULL, *rs = NULL, *pso = NULL; /* alive until the GPU is done */
    UINT64 plane_params[6] = { 0 }, scratch_before = 0;
    SIZE_T rtv;
    BYTE px[4];
    HRESULT hr;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &rtv_heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, &IID_ID3D12Resource,
                (void **)&tex);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList,
                &list);
    CHECK(SUCCEEDED(hr), "render target, RTV heap and list for PresentX (%#lx)", hr);
    if (FAILED(hr)) goto done;

    /* A texture's GPU VA is 0 under vkd3d-proton; titles write texels
     * through it, so d3d12_xs hands out scratch GPU memory instead. */
    {
        UINT64 (WINAPI *usage)(void) = (void *)GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "ReboxGpuBytesInUse");
        UINT64 va;

        scratch_before = usage ? usage() : 0;
        va = ID3D12Resource_GetGPUVirtualAddress(tex);
        CHECK(va != 0, "texture GPU VA is backed by scratch memory (%#llx)", (unsigned long long)va);
        if (va)
        {
            memset((void *)(ULONG_PTR)va, 0x11, 64 * 48 * 4);
            CHECK(ID3D12Resource_GetGPUVirtualAddress(tex) == va && (!usage || usage() > scratch_before),
                  "texel writes through it land, and the address is stable");
        }
    }

    /* CreateRenderTargetView (device slot 20), ClearRenderTargetView (list
     * slot 52 = PC 48), ResourceBarrier (slot 26) to PRESENT. */
    rtv = XSLOT(rtv_heap, 9, SIZE_T)(rtv_heap);
    XSLOT(dev, 20, void, ID3D12Resource *, const void *, SIZE_T)(dev, tex, NULL, rtv);
    XSLOT(list, 52, void, SIZE_T, const FLOAT *, UINT, const D3D12_RECT *)(list, rtv, clear.Color, 0, NULL);

    /* A graphics PSO from console bytes with no translation gets fallback
     * shaders, which must draw nothing: the frame stays the clear colour.
     * Skipped when fallbacks are off (the strict run). */
    if (!getenv("REBOX_SHADER_FALLBACK") || *getenv("REBOX_SHADER_FALLBACK") != '0')
    {
        static const char console_vs[] = "REBOX-FAKE-CONSOLE-VS", console_ps[] = "REBOX-FAKE-CONSOLE-PS";
        PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
                GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "D3D12SerializeRootSignature");
        D3D12_ROOT_SIGNATURE_DESC rsd = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = { 0 };
        D3D12_VIEWPORT vp = { 0, 0, 64, 48, 0, 1 };
        D3D12_RECT sc = { 0, 0, 64, 48 };
        ID3D10Blob *blob = NULL;

        if (serialize && SUCCEEDED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
        {
            XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                    ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
            ID3D10Blob_Release(blob);
        }
        gd.pRootSignature = rs;
        gd.VS.pShaderBytecode = console_vs;
        gd.VS.BytecodeLength = sizeof(console_vs);
        gd.PS.pShaderBytecode = console_ps;
        gd.PS.BytecodeLength = sizeof(console_ps);
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = ~0u;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gd.SampleDesc.Count = 1;
        hr = rs ? XSLOT(dev, 10, HRESULT, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &gd,
                &IID_ID3D12PipelineState, &pso) : E_FAIL;
        CHECK(SUCCEEDED(hr), "graphics PSO from untranslated console shaders gets fallbacks (%#lx)", hr);
        if (pso)
        {
            /* RSSetViewports 21, RSSetScissorRects 22, IASetPrimitiveTopology
             * 20, SetGraphicsRootSignature 34, SetPipelineState 25,
             * OMSetRenderTargets 50, DrawInstanced 12. */
            XSLOT(list, 21, void, UINT, const D3D12_VIEWPORT *)(list, 1, &vp);
            XSLOT(list, 22, void, UINT, const D3D12_RECT *)(list, 1, &sc);
            XSLOT(list, 20, void, D3D12_PRIMITIVE_TOPOLOGY)(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            XSLOT(list, 34, void, void *)(list, rs);
            XSLOT(list, 25, void, void *)(list, pso);
            XSLOT(list, 50, void, UINT, const SIZE_T *, BOOL, const SIZE_T *)(list, 1, &rtv, FALSE, NULL);
            XSLOT(list, 12, void, UINT, UINT, UINT, UINT)(list, 3, 1, 0, 0);
        }
    }

    to_present.Transition.pResource = tex;
    to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    XSLOT(list, 26, void, UINT, const D3D12_RESOURCE_BARRIER *)(list, 1, &to_present);
    hr = XSLOT(list, 9, HRESULT)(list);
    CHECK(SUCCEEDED(hr), "clear recorded through Xbox slots (%#lx)", hr);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);

    plane_params[1] = 1;
    plane_params[2] = (UINT64)(ULONG_PTR)tex;
    hr = XSLOT(queue, 30, HRESULT, UINT, const void *, const void *)(queue, 1, plane_params, NULL);
    CHECK(hr == S_OK, "PresentX with the plane resource in the params (%#lx)", hr);
    plane_params[2] = (UINT64)(ULONG_PTR)&tex;
    hr = XSLOT(queue, 30, HRESULT, UINT, const void *, const void *)(queue, 1, plane_params, NULL);
    CHECK(hr == S_OK, "PresentX with a pointer to the plane resource (%#lx)", hr);

    if (!dump_dir)
    {
        printf("skip frame dump checks (REBOX_DUMP_FRAMES not set)\n");
        goto done;
    }
    /* Clear colour (1, 0.5, 0.25) is RGB 255/128/64; BMP stores BGRA. */
    for (int frame = 1; frame <= 2; frame++)
    {
        BOOL found = read_dump_pixel(dump_dir, frame, px);
        CHECK(found && px[2] == 255 && abs(px[1] - 128) <= 1 && abs(px[0] - 64) <= 1,
              "presented frame %d has the cleared pixels (RGB %u %u %u)", frame, found ? px[2] : 0,
              found ? px[1] : 0, found ? px[0] : 0);
    }

done:
    /* The draw must finish before its PSO and root signature go. */
    if (pso || rs)
    {
        ID3D12Fence *fence = NULL;
        if (SUCCEEDED(XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0,
                D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        {
            XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
            for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
                Sleep(10);
            ID3D12Fence_Release(fence);
        }
    }
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (tex)
    {
        UINT64 (WINAPI *usage)(void) = (void *)GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "ReboxGpuBytesInUse");
        ID3D12Resource_Release(tex);
        if (usage && scratch_before)
            CHECK(usage() == scratch_before, "texture scratch memory freed with the texture");
    }
    if (rtv_heap) XSLOT(rtv_heap, 2, ULONG)(rtv_heap);
}

/* The FidelityFX stub (amd_fidelityfx_loader_xbox.dll): FSR-style queries,
 * then an upscale dispatch through d3d12_xs's bilinear blit. A 32x24 green
 * render is scaled into a 64x48 output, which is presented as frame 3 and
 * read back from the frame dump. */
static void test_ffx(void *dev, void *queue)
{
    HMODULE ffx = LoadLibraryA("amd_fidelityfx_loader_xbox.dll");
    UINT32 (*create)(void **, void *, const void *) = ffx ? (void *)GetProcAddress(ffx, "ffxCreateContext") : NULL;
    UINT32 (*query)(void **, void *) = ffx ? (void *)GetProcAddress(ffx, "ffxQuery") : NULL;
    UINT32 (*dispatch)(void **, const void *) = ffx ? (void *)GetProcAddress(ffx, "ffxDispatch") : NULL;
    UINT32 (*destroy)(void **, const void *) = ffx ? (void *)GetProcAddress(ffx, "ffxDestroyContext") : NULL;
    const char *dump_dir = getenv("REBOX_DUMP_FRAMES");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC cd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 24, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_RESOURCE_DESC od = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 48, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_CLEAR_VALUE green = { DXGI_FORMAT_R8G8B8A8_UNORM, { { 0.0f, 1.0f, 0.0f, 1.0f } } };
    D3D12_RESOURCE_BARRIER b[2] = { { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION }, { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION } };
    struct { UINT64 type; void *next; void *device; } backend = { 0x1000002, NULL, dev };
    BYTE create_desc[0x30] = { 0 }, query_desc[0x30] = { 0 }, dispatch_desc[0x1b0] = { 0 };
    ID3D12Resource *color = NULL, *output = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Fence *fence = NULL;
    void *rtv_heap = NULL, *list = NULL, *ctx = NULL;
    UINT64 plane_params[6] = { 0 };
    UINT32 rw = 0, rh = 0, rc;
    SIZE_T rtv;
    BYTE px[4];
    HRESULT hr;

    CHECK(create && query && dispatch && destroy, "FidelityFX stub exports the FFX API");
    if (!create || !query || !dispatch || !destroy) return;

    /* GETRENDERRESOLUTIONFROMQUALITYMODE: 64x48 at PERFORMANCE (2x) -> 32x24. */
    *(UINT64 *)query_desc = 0x10004;
    *(UINT32 *)(query_desc + 0x10) = 64;
    *(UINT32 *)(query_desc + 0x14) = 48;
    *(UINT32 *)(query_desc + 0x18) = 3;
    *(UINT32 **)(query_desc + 0x20) = &rw;
    *(UINT32 **)(query_desc + 0x28) = &rh;
    rc = query(NULL, query_desc);
    CHECK(rc == 0 && rw == 32 && rh == 24, "FFX render resolution query (%u: %ux%u)", rc, rw, rh);

    *(UINT64 *)create_desc = 0x10000;
    *(void **)(create_desc + 8) = &backend;
    *(UINT32 *)(create_desc + 0x14) = 32;
    *(UINT32 *)(create_desc + 0x18) = 24;
    *(UINT32 *)(create_desc + 0x1c) = 64;
    *(UINT32 *)(create_desc + 0x20) = 48;
    rc = create(&ctx, create_desc, NULL);
    CHECK(rc == 0 && ctx, "FFX upscale context (%u)", rc);
    if (rc) return;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &rtv_heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_RENDER_TARGET, &green, &IID_ID3D12Resource,
                (void **)&color);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource,
                (void **)&output);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList,
                &list);
    CHECK(SUCCEEDED(hr), "resources for the FFX dispatch (%#lx)", hr);
    if (FAILED(hr)) goto done;

    rtv = XSLOT(rtv_heap, 9, SIZE_T)(rtv_heap);
    XSLOT(dev, 20, void, ID3D12Resource *, const void *, SIZE_T)(dev, color, NULL, rtv);
    XSLOT(list, 52, void, SIZE_T, const FLOAT *, UINT, const D3D12_RECT *)(list, rtv, green.Color, 0, NULL);
    b[0].Transition.pResource = color;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    XSLOT(list, 26, void, UINT, const D3D12_RESOURCE_BARRIER *)(list, 1, b);

    *(UINT64 *)dispatch_desc = 0x10001;
    *(void **)(dispatch_desc + 0x10) = list;
    *(void **)(dispatch_desc + 0x18) = color;
    *(void **)(dispatch_desc + 0x138) = output;
    *(UINT32 *)(dispatch_desc + 0x178) = 32;
    *(UINT32 *)(dispatch_desc + 0x17c) = 24;
    *(UINT32 *)(dispatch_desc + 0x180) = 64;
    *(UINT32 *)(dispatch_desc + 0x184) = 48;
    rc = dispatch(&ctx, dispatch_desc);
    CHECK(rc == 0, "FFX upscale dispatch (%u)", rc);

    b[0].Transition.pResource = output;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    XSLOT(list, 26, void, UINT, const D3D12_RESOURCE_BARRIER *)(list, 1, b);
    hr = XSLOT(list, 9, HRESULT)(list);
    CHECK(SUCCEEDED(hr), "list with the FFX dispatch closes (%#lx)", hr);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    plane_params[1] = 1;
    plane_params[2] = (UINT64)(ULONG_PTR)output;
    XSLOT(queue, 30, HRESULT, UINT, const void *, const void *)(queue, 1, plane_params, NULL);
    if (dump_dir)
    {
        BOOL found = read_dump_pixel(dump_dir, 3, px);
        CHECK(found && px[2] == 0 && px[1] == 255 && px[0] == 0, "upscaled frame is the render's green (RGB %u %u %u)",
              found ? px[2] : 0, found ? px[1] : 0, found ? px[0] : 0);
    }

done:
    if (SUCCEEDED(XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence)))
    {
        XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
        for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
            Sleep(10);
        ID3D12Fence_Release(fence);
    }
    destroy(&ctx, NULL);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (color) ID3D12Resource_Release(color);
    if (output) ID3D12Resource_Release(output);
    if (rtv_heap) XSLOT(rtv_heap, 2, ULONG)(rtv_heap);
}

/* A plane in a format no swapchain takes (R11G11B10_FLOAT) is converted to
 * RGBA8 by PresentX; presented as frame 4. */
static void test_present_format(void *dev, void *queue)
{
    const char *dump_dir = getenv("REBOX_DUMP_FRAMES");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 48, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT,
                                 { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_CLEAR_VALUE clear = { DXGI_FORMAT_R11G11B10_FLOAT, { { 1.0f, 0.5f, 0.25f, 1.0f } } };
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *tex = NULL;
    ID3D12Fence *fence = NULL;
    void *rtv_heap = NULL, *list = NULL;
    UINT64 plane_params[6] = { 0 };
    SIZE_T rtv;
    BYTE px[4];
    HRESULT hr;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &rtv_heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, &IID_ID3D12Resource,
                (void **)&tex);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList,
                &list);
    CHECK(SUCCEEDED(hr), "R11G11B10 plane for PresentX (%#lx)", hr);
    if (FAILED(hr)) goto done;

    rtv = XSLOT(rtv_heap, 9, SIZE_T)(rtv_heap);
    XSLOT(dev, 20, void, ID3D12Resource *, const void *, SIZE_T)(dev, tex, NULL, rtv);
    XSLOT(list, 52, void, SIZE_T, const FLOAT *, UINT, const D3D12_RECT *)(list, rtv, clear.Color, 0, NULL);
    b.Transition.pResource = tex;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    XSLOT(list, 26, void, UINT, const D3D12_RESOURCE_BARRIER *)(list, 1, &b);
    XSLOT(list, 9, HRESULT)(list);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    plane_params[1] = 1;
    plane_params[2] = (UINT64)(ULONG_PTR)tex;
    hr = XSLOT(queue, 30, HRESULT, UINT, const void *, const void *)(queue, 1, plane_params, NULL);
    CHECK(hr == S_OK, "PresentX of an R11G11B10 plane (%#lx)", hr);
    if (dump_dir)
    {
        BOOL found = read_dump_pixel(dump_dir, 4, px);
        CHECK(found && px[2] == 255 && abs(px[1] - 128) <= 1 && abs(px[0] - 64) <= 1,
              "R11G11B10 plane presented as RGBA8 (RGB %u %u %u)", found ? px[2] : 0, found ? px[1] : 0,
              found ? px[0] : 0);
    }

done:
    if (SUCCEEDED(XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence)))
    {
        XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
        for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
            Sleep(10);
        ID3D12Fence_Release(fence);
    }
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (tex) ID3D12Resource_Release(tex);
    if (rtv_heap) XSLOT(rtv_heap, 2, ULONG)(rtv_heap);
}

/* Frame pacing through device slots 78-80, as the traced retail title uses
 * them: SetFrameIntervalX, then WaitFrameEventX(ORIGIN, INFINITE) per frame. */
static double ms_since(LARGE_INTEGER a)
{
    LARGE_INTEGER b, f;
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

static void test_pacing(void *dev)
{
    typedef HRESULT (STDMETHODCALLTYPE *wait_fn)(void *, UINT, UINT, void *, UINT, UINT64 *);
    wait_fn wait = (wait_fn)(*(void ***)dev)[80];
    UINT64 tok[6] = { 0 }, late = 0;
    LARGE_INTEGER t0;
    HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
    HRESULT hr;
    double ms;
    BOOL ok = TRUE;
    int i;

    /* 10 ms x period 2: an origin every 20 ms. */
    hr = XSLOT(dev, 78, HRESULT, void *, UINT, UINT)(dev, NULL, 10000, 2);
    CHECK(SUCCEEDED(hr), "SetFrameIntervalX (slot 78) %#lx", hr);
    wait(dev, 0, INFINITE, NULL, 0, &tok[0]);
    QueryPerformanceCounter(&t0);
    for (i = 1; i < 6; i++)
    {
        hr = wait(dev, 0, INFINITE, NULL, 0, &tok[i]);
        ok &= SUCCEEDED(hr) && tok[i] == tok[i - 1] + 1;
    }
    ms = ms_since(t0);
    CHECK(ok && ms > 90 && ms < 200, "WaitFrameEventX (slot 80) paces 5 frames at 20 ms: %.1f ms, tokens %llu..%llu",
          ms, (unsigned long long)tok[0], (unsigned long long)tok[5]);

    /* A late title gets the newest missed frame at once, not a burst. */
    Sleep(70);
    QueryPerformanceCounter(&t0);
    wait(dev, 0, INFINITE, NULL, 0, &late);
    ms = ms_since(t0);
    CHECK(ms < 10 && late >= tok[5] + 3, "late WaitFrameEventX returns at once (%.1f ms) and skips to frame %llu",
          ms, (unsigned long long)late);

    hr = wait(dev, 0, 0, NULL, 0, &late);
    CHECK(hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT), "WaitFrameEventX with timeout 0 between frames times out (%#lx)", hr);

    /* An event scheduled 5 ms after the origin is set once per frame. */
    hr = XSLOT(dev, 79, HRESULT, UINT, UINT_PTR, HANDLE)(dev, 1, 5000, event);
    ok = SUCCEEDED(hr);
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 3; i++)
        ok &= WaitForSingleObject(event, 200) == WAIT_OBJECT_0;
    ms = ms_since(t0);
    CHECK(ok && ms > 25 && ms < 120, "ScheduleFrameEventX (slot 79) sets its event each frame: 3 in %.1f ms", ms);

    XSLOT(dev, 79, HRESULT, UINT, UINT_PTR, HANDLE)(dev, 1, 0, NULL);
    XSLOT(dev, 78, HRESULT, void *, UINT, UINT)(dev, NULL, 16667, 1);
    CloseHandle(event);
}

/* Real heap addresses (patched vkd3d-proton, docs/shaders.md): heap slot 10
 * may return the descriptors' real GPU address. A descriptor table set from it
 * (list slot 35) must still reach the right UAV, which a compute shader then
 * writes; vkd3d-proton would misread an untranslated address. */
static void test_real_heap_va(void *dev, void *queue)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                   .DescriptorTable = { 1, &range } };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = { 0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW } };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    void *heap = NULL, *rs = NULL, *pso = NULL, *list = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *buf = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *blob = NULL;
    SIZE_T cpu;
    UINT64 gpu;
    UINT inc, *mapped = NULL;
    HRESULT hr;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
    if (SUCCEEDED(hr))
    {
        cd.pRootSignature = rs;
        cd.CS.pShaderBytecode = test_uav_write_cs;
        cd.CS.BytecodeLength = sizeof(test_uav_write_cs);
        hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                &IID_ID3D12PipelineState, &pso);
    }
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void **)&fence);
    CHECK(SUCCEEDED(hr), "objects for the descriptor table test (%#lx)", hr);
    if (FAILED(hr)) goto done;

    cpu = XSLOT(heap, 9, SIZE_T)(heap);
    gpu = XSLOT(heap, 10, UINT64)(heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    printf("info heap GPU start %#llx, CPU start %#llx, increment %u: %s, %s 4 GiB\n", (unsigned long long)gpu,
           (unsigned long long)cpu, inc, gpu == cpu ? "real and identity-mapped" : "not the CPU address",
           gpu < (1ull << 32) ? "below" : "above");
    /* The view goes in entry 1, so the table address carries an offset. */
    XSLOT(dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(dev,
            buf, NULL, &uav, cpu + inc);

    XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
    XSLOT(list, 33, void, void *)(list, rs);
    XSLOT(list, 25, void, void *)(list, pso);
    XSLOT(list, 35, void, UINT, UINT64)(list, 0, gpu + inc);
    XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
    hr = XSLOT(list, 9, HRESULT)(list);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
    for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
        Sleep(10);
    if (SUCCEEDED(hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&mapped)))
    {
        CHECK(mapped[0] == 0x5ca71e77, "compute shader wrote through a descriptor table set from heap slot 10 "
              "(%#x)", mapped[0]);
        ID3D12Resource_Unmap(buf, 0, NULL);
    }
    else
        CHECK(0, "map the UAV buffer (%#lx)", hr);

done:
    if (fence) ID3D12Fence_Release(fence);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (buf) ID3D12Resource_Release(buf);
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (blob) ID3D10Blob_Release(blob);
    if (heap) XSLOT(heap, 2, ULONG)(heap);
}

/* Console compute pipelines (vkd3d-proton patch 0002, docs/shaders.md): a
 * SPIR-V shader under an SCSV header gets its user SGPRs packed from the root
 * parameters at dispatch. It writes s0-s31 and a marker to the address in
 * s0:s1, root parameter 0 (a root UAV). Expected packing: s0:s1 the UAV's GPU
 * VA, s2-s5 the four root constants, s6:s7 the table's address as heap slot 10 reports it
 * (0 without descriptor buffers, as on lavapipe). */
static void test_console_shader(void *dev, void *queue)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    static const UINT constants[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };
    D3D12_ROOT_PARAMETER params[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 } },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 4 } },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &range } },
    };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    void *heap = NULL, *rs = NULL, *pso = NULL, *list = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *buf = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *blob = NULL;
    UINT64 gpu, va, table;
    UINT inc, *out = NULL;
    HRESULT hr;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
    if (SUCCEEDED(hr))
    {
        cd.pRootSignature = rs;
        cd.CS.pShaderBytecode = test_console_comp;
        cd.CS.BytecodeLength = sizeof(test_console_comp);
        hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                &IID_ID3D12PipelineState, &pso);
        CHECK(SUCCEEDED(hr), "compute PSO from SCSV SPIR-V (patched vkd3d-proton) %#lx", hr);
    }
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void **)&fence);
    if (FAILED(hr)) goto done;

    gpu = XSLOT(heap, 10, UINT64)(heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    va = ID3D12Resource_GetGPUVirtualAddress(buf);

    XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
    XSLOT(list, 33, void, void *)(list, rs);
    XSLOT(list, 25, void, void *)(list, pso);
    XSLOT(list, 45, void, UINT, UINT64)(list, 0, va);                                 /* PC 41 */
    XSLOT(list, 39, void, UINT, UINT, const void *, UINT)(list, 1, 4, constants, 0);  /* PC 35 */
    XSLOT(list, 35, void, UINT, UINT64)(list, 2, gpu + 2 * inc);                      /* PC 31 */
    XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
    hr = XSLOT(list, 9, HRESULT)(list);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
    for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
        Sleep(10);
    if (FAILED(hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&out)))
    {
        CHECK(0, "map the console shader's output (%#lx)", hr);
        goto done;
    }
    table = out[6] | (UINT64)out[7] << 32;
    CHECK(out[32] == 0x5ca7c0de, "console shader ran (marker %#x)", out[32]);
    CHECK((out[0] | (UINT64)out[1] << 32) == va, "s0:s1 = root UAV address %#llx (got %#x:%#x)",
          (unsigned long long)va, out[1], out[0]);
    CHECK(!memcmp(out + 2, constants, sizeof(constants)), "s2-s5 = root constants (%#x %#x %#x %#x)",
          out[2], out[3], out[4], out[5]);
    CHECK(table == 0 || table == gpu + 2 * inc, "s6:s7 = descriptor table at the heap's slot-10 address %#llx (got %#llx)",
          (unsigned long long)(gpu + 2 * inc), (unsigned long long)table);
    printf("info console SGPR table address: %s\n", !table ? "0 (no title or real heap addresses)"
           : table < 0x100000000ull ? "shadow-heap title address" : "real heap address");
    ID3D12Resource_Unmap(buf, 0, NULL);

done:
    if (fence) ID3D12Fence_Release(fence);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (buf) ID3D12Resource_Release(buf);
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (blob) ID3D10Blob_Release(blob);
    if (heap) XSLOT(heap, 2, ULONG)(heap);
}

/* Console graphics pipelines (patch 0002): SCSV VS and PS carrying DXBC
 * I/O signatures. The VS draws a fullscreen triangle into an 8x8 target; the
 * PS writes its SGPRs to dwords 64-95 of the root UAV in s0:s1, and a marker
 * at 96. Same root parameters as the compute test, set through the graphics
 * slots, so the draw path's packing is checked. */
static void test_console_graphics(void *dev, void *queue)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    static const UINT constants[4] = { 0x55555555, 0x66666666, 0x77777777, 0x88888888 };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rtvd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };
    D3D12_ROOT_PARAMETER params[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 } },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 4 } },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &range } },
    };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 512, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_VIEWPORT vp = { 0, 0, 8, 8, 0, 1 };
    D3D12_RECT sc = { 0, 0, 8, 8 };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = { 0 };
    void *heap = NULL, *rtv_heap = NULL, *rs = NULL, *pso = NULL, *list = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *buf = NULL, *rt = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *blob = NULL;
    UINT64 gpu, va, table;
    SIZE_T rtv;
    UINT inc, *out = NULL;
    HRESULT hr;

    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &rtvd,
                &IID_ID3D12DescriptorHeap, &rtv_heap);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
    if (SUCCEEDED(hr))
    {
        gd.pRootSignature = rs;
        gd.VS = (D3D12_SHADER_BYTECODE){ test_console_vert, sizeof(test_console_vert) };
        gd.PS = (D3D12_SHADER_BYTECODE){ test_console_frag, sizeof(test_console_frag) };
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = ~0u;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gd.SampleDesc.Count = 1;
        hr = XSLOT(dev, 10, HRESULT, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &gd,
                &IID_ID3D12PipelineState, &pso);
        CHECK(SUCCEEDED(hr), "graphics PSO from SCSV VS/PS with embedded signatures (patched vkd3d-proton) %#lx", hr);
    }
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void **)&fence);
    if (FAILED(hr)) goto done;

    gpu = XSLOT(heap, 10, UINT64)(heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    va = ID3D12Resource_GetGPUVirtualAddress(buf);
    rtv = XSLOT(rtv_heap, 9, SIZE_T)(rtv_heap);
    XSLOT(dev, 20, void, ID3D12Resource *, const void *, SIZE_T)(dev, rt, NULL, rtv);

    XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
    XSLOT(list, 34, void, void *)(list, rs);                                           /* PC 30 */
    XSLOT(list, 25, void, void *)(list, pso);
    XSLOT(list, 46, void, UINT, UINT64)(list, 0, va);                                 /* PC 42 */
    XSLOT(list, 40, void, UINT, UINT, const void *, UINT)(list, 1, 4, constants, 0);  /* PC 36 */
    XSLOT(list, 36, void, UINT, UINT64)(list, 2, gpu + 3 * inc);                      /* PC 32 */
    XSLOT(list, 20, void, D3D12_PRIMITIVE_TOPOLOGY)(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    XSLOT(list, 21, void, UINT, const D3D12_VIEWPORT *)(list, 1, &vp);
    XSLOT(list, 22, void, UINT, const D3D12_RECT *)(list, 1, &sc);
    XSLOT(list, 50, void, UINT, const SIZE_T *, BOOL, const SIZE_T *)(list, 1, &rtv, FALSE, NULL); /* PC 46 */
    XSLOT(list, 12, void, UINT, UINT, UINT, UINT)(list, 3, 1, 0, 0);
    hr = XSLOT(list, 9, HRESULT)(list);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
    for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
        Sleep(10);
    if (FAILED(hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&out)))
    {
        CHECK(0, "map the console pixel shader's output (%#lx)", hr);
        goto done;
    }
    table = out[64 + 6] | (UINT64)out[64 + 7] << 32;
    CHECK(out[96] == 0x5ca7f4a9, "console VS/PS drew (pixel shader marker %#x)", out[96]);
    CHECK((out[64] | (UINT64)out[65] << 32) == va, "graphics s0:s1 = root UAV address %#llx (got %#x:%#x)",
          (unsigned long long)va, out[65], out[64]);
    CHECK(!memcmp(out + 66, constants, sizeof(constants)), "graphics s2-s5 = root constants (%#x %#x %#x %#x)",
          out[66], out[67], out[68], out[69]);
    CHECK(table == 0 || table == gpu + 3 * inc, "graphics s6:s7 = descriptor table at the heap's real address %#llx "
          "(got %#llx)", (unsigned long long)(gpu + 3 * inc), (unsigned long long)table);
    ID3D12Resource_Unmap(buf, 0, NULL);

done:
    if (fence) ID3D12Fence_Release(fence);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (buf) ID3D12Resource_Release(buf);
    if (rt) ID3D12Resource_Release(rt);
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (blob) ID3D10Blob_Release(blob);
    if (rtv_heap) XSLOT(rtv_heap, 2, ULONG)(rtv_heap);
    if (heap) XSLOT(heap, 2, ULONG)(heap);
}

/* Descriptor lookup (patch 4): two dispatches of a console shader that reads
 * the raw descriptor at its table address and finds its heap index in the
 * bound heap's lookup table. Entry 2 is written straight into the
 * shader-visible heap (slot 19), entry 3 copied in from a CPU heap (slot
 * 24). Needs descriptor buffers (RADV); elsewhere the shader reports a skip. */
/* RDNA2 compute shader for the shadow-heap round trip (llvm-mc -mcpu=gfx1030).
 * s[0:1] points at a CBV/SRV/UAV descriptor table, s[2:3] at a sampler table;
 * the s_load offsets (in dwords 1, 3 and 5) are patched to the heap increments.
 *   s_load_dwordx8 s[4:11], s[0:1], 2*inc         ; T# at index 2
 *   s_load_dwordx4 s[12:15], s[0:1], 1*inc        ; V# at index 1
 *   s_load_dwordx4 s[16:19], s[2:3], 1*sampler_inc ; S# at index 1
 *   s_waitcnt lgkmcnt(0)
 *   v_mov_b32 v1, 0 ; v_mov_b32 v2, 0 ; v_mov_b32 v3, 0
 *   image_sample_lz v[8:11], v[2:3], s[4:11], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D
 *   s_waitcnt vmcnt(0)
 *   buffer_store_dword v8, v0, s[12:15], 0 offen
 *   s_endpgm */
static const UINT32 rdna_shadow_round_trip[] = {
    0xf40c0100, 0xfa000040, 0xf4080300, 0xfa000020, 0xf4080401, 0xfa000010, 0xbf8cc07f, 0x7e020280,
    0x7e040280, 0x7e060280, 0xf09c0f08, 0x00810802, 0xbf8c3f70, 0xe0701000, 0x80030800, 0xbf810000,
};

/* Compiles rdna_shadow_round_trip with AnyPS5 against the shadow heaps and
 * checks that its own descriptor decoders found the views written through the
 * API, and that each binding maps back to vkd3d-proton's descriptor. */
static void anyps5_round_trip(UINT64 res_va, const void *res_data, UINT res_size, UINT inc, SIZE_T res_cpu,
        UINT64 smp_va, const void *smp_data, UINT smp_size, UINT sinc, SIZE_T smp_cpu)
{
    HMODULE sa5 = LoadLibraryA("rebox_anyps5.dll");
    BOOL (WINAPI *find)(const UINT32 *, UINT, BOOL, SIZE_T *) =
            (void *)GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "ReboxShadowFind");
    int (*compile)(const struct sa5_compute_request *, struct sa5_result **, char *, size_t);
    UINT32 (*binding_count)(const struct sa5_result *);
    int (*binding)(const struct sa5_result *, UINT32, struct sa5_binding *);
    void (*result_free)(struct sa5_result *);
    struct sa5_region regions[2] = { { res_va, res_data, res_size }, { smp_va, smp_data, smp_size } };
    UINT32 code[ARRAYSIZE(rdna_shadow_round_trip)], user[4] = { (UINT32)res_va, 0, (UINT32)smp_va, 0 };
    struct sa5_compute_request req = { code, ARRAYSIZE(code), 0x100000, 32, user, 4, { 1, 1, 1 },
                                       0x00ac0000, 4 << 1 /* USER_SGPR */, regions, 2 };
    const UINT32 *t = (const UINT32 *)((const BYTE *)res_data + 2 * inc);
    const UINT32 *v = (const UINT32 *)((const BYTE *)res_data + inc);
    const UINT32 *s = (const UINT32 *)((const BYTE *)smp_data + sinc);
    BOOL found_t = FALSE, found_v = FALSE, found_s = FALSE;
    struct sa5_result *result = NULL;
    char error[512] = "";

    if (!sa5)
    {
        printf("skip AnyPS5 round trip: no rebox_anyps5.dll\n");
        return;
    }
    compile = (void *)GetProcAddress(sa5, "sa5_compile_compute");
    binding_count = (void *)GetProcAddress(sa5, "sa5_result_binding_count");
    binding = (void *)GetProcAddress(sa5, "sa5_result_binding");
    result_free = (void *)GetProcAddress(sa5, "sa5_result_free");
    memcpy(code, rdna_shadow_round_trip, sizeof(code));
    /* OFFSET is bits 0-20 of the second dword; SOFFSET (null) stays. */
    code[1] = (code[1] & ~0x1fffffu) | 2 * inc;
    code[3] = (code[3] & ~0x1fffffu) | inc;
    code[5] = (code[5] & ~0x1fffffu) | sinc;
    if (compile(&req, &result, error, sizeof(error)))
    {
        CHECK(0, "AnyPS5 compiles the shadow-heap round-trip shader: %s", error);
        return;
    }
    if (getenv("SMOKE_DUMP_SPIRV"))
    {
        const UINT32 *(*spirv)(const struct sa5_result *, UINT32 *) = (void *)GetProcAddress(sa5, "sa5_result_spirv");
        UINT32 words;
        const UINT32 *code_out = spirv(result, &words);
        FILE *f = fopen(getenv("SMOKE_DUMP_SPIRV"), "wb");
        if (f) { fwrite(code_out, 4, words, f); fclose(f); }
    }
    for (UINT32 i = 0; i < binding_count(result); i++)
    {
        struct sa5_binding b;
        SIZE_T cpu = 0;

        binding(result, i, &b);
        printf("     binding %u: kind %u set %u binding %u count %u words %u shape %d:", i, b.kind, b.set, b.binding,
               b.count, b.word_count, b.image_shape);
        for (UINT32 w = 0; w < b.word_count; w++)
            printf(" %08x", b.words[w]);
        printf("\n");
        if (b.kind == 4 /* SampledImage */ && b.word_count >= 8 && !memcmp(b.words, t, 32))
        {
            found_t = TRUE;
            CHECK(b.image_shape == 1, "AnyPS5 reads the shadow T# as a 2D image (shape %d)", b.image_shape);
            CHECK(find(b.words, 8, FALSE, &cpu) && cpu == res_cpu + 2 * inc,
                  "the T# binding maps back to vkd3d-proton's descriptor 2 (%#Ix)", cpu);
        }
        else if (b.kind == 6 /* Sampler */ && b.word_count >= 4 && !memcmp(b.words, s, 16))
        {
            found_s = TRUE;
            CHECK(find(b.words, 4, TRUE, &cpu) && cpu == smp_cpu + sinc,
                  "the S# binding maps back to vkd3d-proton's sampler 1 (%#Ix)", cpu);
        }
        else if (b.kind <= 3 && b.word_count >= 4 && !memcmp(b.words, v, 16))
        {
            found_v = TRUE;
            CHECK(find(b.words, 4, FALSE, &cpu) && cpu == res_cpu + inc,
                  "the V# binding maps back to vkd3d-proton's descriptor 1 (%#Ix)", cpu);
        }
    }
    CHECK(found_t, "AnyPS5 binds the T# written through CreateShaderResourceView");
    CHECK(found_v, "AnyPS5 binds the V# copied in through CopyDescriptorsSimple");
    CHECK(found_s, "AnyPS5 binds the S# written through CreateSampler");

    /* Fit the module to bindless arrays (made-up sets here) at the heap slots found above, with
     * the flattened SRT as a host buffer, and check the result validates. */
    {
        int (*finalize)(const struct sa5_result *, const struct sa5_remap *, UINT32, const struct sa5_host_buffer *,
                UINT32, UINT32 **, UINT32 *, char *, size_t) = (void *)GetProcAddress(sa5, "sa5_finalize");
        void (*free_words)(UINT32 *) = (void *)GetProcAddress(sa5, "sa5_free_words");
        struct sa5_remap remaps[8];
        struct sa5_host_buffer hosts[4];
        UINT32 indices[8][16], nremap = 0, nhost = 0, *words = NULL, word_count = 0;
        BOOL resolved = TRUE;

        for (UINT32 i = 0; i < binding_count(result) && nremap < 8 && nhost < 4; i++)
        {
            struct sa5_binding b;
            binding(result, i, &b);
            if (b.role <= 2 && b.count <= 16)
            {
                UINT32 per = b.count ? b.word_count / b.count : 0;
                for (UINT32 e = 0; e < b.count; e++)
                {
                    SIZE_T cpu = 0, start = b.kind == 6 ? smp_cpu : res_cpu;
                    if (!find(b.words + e * per, per, b.kind == 6, &cpu))
                        resolved = FALSE;
                    indices[nremap][e] = (UINT32)((cpu - start) / (b.kind == 6 ? sinc : inc));
                }
                remaps[nremap] = (struct sa5_remap){ b.set, b.binding, b.kind, 1 + b.kind, 0, b.count, indices[nremap] };
                nremap++;
            }
            else
                hosts[nhost++] = (struct sa5_host_buffer){ b.set, b.binding, nhost, b.word_count };
        }
        CHECK(resolved && nremap == 3 && nhost == 1, "every guest binding resolves to a heap slot (%u guest, %u host)",
              nremap, nhost);
        if (finalize(result, remaps, nremap, hosts, nhost, &words, &word_count, error, sizeof(error)))
            CHECK(0, "sa5_finalize rebinds the module onto bindless arrays: %s", error);
        else
        {
            CHECK(word_count > 5, "sa5_finalize rebinds the module onto bindless arrays (%u words, valid)", word_count);
            if (getenv("SMOKE_DUMP_SPIRV"))
            {
                char path[MAX_PATH];
                FILE *f;
                snprintf(path, sizeof(path), "%s.final", getenv("SMOKE_DUMP_SPIRV"));
                if ((f = fopen(path, "wb"))) { fwrite(words, 4, word_count, f); fclose(f); }
            }
            free_words(words);
        }
    }
    result_free(result);
}

/* Console-format shadow heaps (shadow.c): views written through the API come
 * out as RDNA2 descriptors, readable at the heap's title address, and map
 * back to vkd3d-proton's descriptors. */
static void test_shadow_heap(void *dev)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    HRESULT (WINAPI *shadow_heap)(SIZE_T, UINT64 *, void **, UINT *, UINT *) =
            (void *)GetProcAddress(m, "ReboxShadowHeap");
    BOOL (WINAPI *shadow_read)(UINT64, void *, SIZE_T) = (void *)GetProcAddress(m, "ReboxShadowRead");
    BOOL (WINAPI *shadow_find)(const UINT32 *, UINT, BOOL, SIZE_T *) = (void *)GetProcAddress(m, "ReboxShadowFind");
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC cd_heap = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 32, 1, 3, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = { 0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW } };
    D3D12_SAMPLER_DESC smp = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                               D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, 0, 1,
                               D3D12_COMPARISON_FUNC_NEVER, { 0 }, 0, 8 };
    void *heap = NULL, *cpu_heap = NULL, *smp_heap = NULL;
    ID3D12Resource *buf = NULL, *tex = NULL;
    UINT64 title_va = 0, smp_va = 0;
    UINT32 *res, *sam, readback[8];
    UINT inc, sinc, stride, count, scount;
    SIZE_T cpu, cpu_only, scpu, found = 0;
    HRESULT hr;

    CHECK(shadow_heap && shadow_read && shadow_find, "d3d12_xs exports the shadow heap functions");
    if (!shadow_heap || !shadow_read || !shadow_find) return;
    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &cd_heap,
                &IID_ID3D12DescriptorHeap, &cpu_heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &sd,
                &IID_ID3D12DescriptorHeap, &smp_heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex);
    CHECK(SUCCEEDED(hr), "objects for the shadow heap test (%#lx)", hr);
    if (FAILED(hr)) goto done;

    cpu = XSLOT(heap, 9, SIZE_T)(heap);
    cpu_only = XSLOT(cpu_heap, 9, SIZE_T)(cpu_heap);
    scpu = XSLOT(smp_heap, 9, SIZE_T)(smp_heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sinc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (FAILED(shadow_heap(cpu, &title_va, (void **)&res, &stride, &count)))
    {
        printf("skip shadow heaps: none for this device (increments %u/%u too small?)\n", inc, sinc);
        goto done;
    }
    CHECK(stride == inc && count == 4 && title_va >= 0x10000000 && title_va < 0x100000000ull && !(title_va & 0xffff),
          "shader-visible heap has a shadow: title address %#llx, stride %u, %u descriptors", (unsigned long long)title_va, stride, count);
    CHECK(SUCCEEDED(shadow_heap(scpu, &smp_va, (void **)&sam, NULL, &scount)) && smp_va && smp_va != title_va,
          "sampler heap has its own title address (%#llx)", (unsigned long long)smp_va);
    {
        UINT64 none = 1;
        CHECK(SUCCEEDED(shadow_heap(cpu_only, &none, NULL, NULL, NULL)) && !none,
              "a heap that isn't shader-visible has a shadow but no title address");
    }

    XSLOT(dev, 18, void, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, SIZE_T)(dev, tex, NULL, cpu + 2 * inc);
    XSLOT(dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(dev,
            buf, NULL, &uav, cpu_only);
    XSLOT(dev, 24, void, UINT, SIZE_T, SIZE_T, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, 1, cpu + inc, cpu_only,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    XSLOT(dev, 22, void, const D3D12_SAMPLER_DESC *, SIZE_T)(dev, &smp, scpu + sinc);

    {
        const UINT32 *t = (const UINT32 *)((BYTE *)res + 2 * inc);
        UINT width = ((t[1] >> 30) | (t[2] & 0xfff) << 2) + 1, height = ((t[2] >> 14) & 0x3fff) + 1;
        CHECK(((t[1] >> 20) & 0x1ff) == 56 && width == 64 && height == 32 && (t[3] >> 28) == 9
              && ((t[3] >> 12) & 0xf) == 0 && ((t[3] >> 16) & 0xf) == 2 && (t[3] & 0xfff) == 0xfac,
              "SRV becomes a T#: 8_8_8_8 UNORM, %ux%u, 2D, mips 0-%u, swizzle %#x", width, height,
              (t[3] >> 16) & 0xf, t[3] & 0xfff);
    }
    {
        const UINT32 *v = (const UINT32 *)((BYTE *)res + inc);
        UINT64 base = v[0] | (UINT64)(v[1] & 0xffff) << 32;
        CHECK(base == ID3D12Resource_GetGPUVirtualAddress(buf) && ((v[1] >> 16) & 0x3fff) == 4 && v[2] == 64,
              "UAV copied from a CPU heap becomes a V#: base %#llx, stride %u, %u records", base,
              (v[1] >> 16) & 0x3fff, v[2]);
    }
    {
        const UINT32 *s = (const UINT32 *)((BYTE *)sam + sinc);
        CHECK((s[0] & 0x1ff) == (2 | 0 << 3 | 1 << 6) && ((s[2] >> 20) & 3) == 1 && ((s[2] >> 22) & 3) == 1
              && ((s[2] >> 26) & 3) == 2 && ((s[1] >> 12) & 0xfff) == 8 * 256,
              "sampler becomes an S#: clamp/wrap/mirror, linear filters, max LOD 8");
    }
    CHECK(shadow_read(title_va + 2 * inc, readback, 32) && !memcmp(readback, (BYTE *)res + 2 * inc, 32),
          "ReboxShadowRead reads the T# at its title address");
    CHECK(!shadow_read(title_va + 3 * inc, readback, 2 * inc), "ReboxShadowRead refuses a read past the heap");
    CHECK(shadow_find((const UINT32 *)((BYTE *)res + inc), 4, FALSE, &found) && found == cpu + inc,
          "the V# maps back to the shader-visible copy, not the CPU heap (%#Ix)", found);

    anyps5_round_trip(title_va, res, 4 * inc, inc, cpu, smp_va, sam, scount * sinc, sinc, scpu);

done:
    if (buf) ID3D12Resource_Release(buf);
    if (tex) ID3D12Resource_Release(tex);
    if (smp_heap) XSLOT(smp_heap, 2, ULONG)(smp_heap);
    if (cpu_heap) XSLOT(cpu_heap, 2, ULONG)(cpu_heap);
    if (heap) XSLOT(heap, 2, ULONG)(heap);
}

/* RDNA2 compute shader for the deferred dispatch test (llvm-mc -mcpu=gfx1030): stores
 * 0x1234abcd through the V# at the start of the descriptor table s[0:1] points at. It opens with
 * the FLAT_SCRATCH setup console shaders have, which d3d12_xs NOPs out.
 *   s_setreg_imm32_b32 hwreg(HW_REG_FLAT_SCR_LO), 0x1000
 *   s_setreg_imm32_b32 hwreg(HW_REG_FLAT_SCR_HI, 0, 16), 2
 *   s_load_dwordx4 s[4:7], s[0:1], 0x0
 *   s_waitcnt lgkmcnt(0)
 *   v_mov_b32 v1, 0x1234abcd
 *   v_mov_b32 v2, 0
 *   buffer_store_dword v1, v2, s[4:7], 0 offen
 *   s_endpgm */
static const UINT32 rdna_uav_write[] = {
    0xba80f814, 0x00001000, 0xba807815, 0x00000002, 0xf4080100, 0xfa000000, 0xbf8cc07f, 0x7e0202ff, 0x1234abcd, 0x7e040280, 0xe0701000, 0x80010102, 0xbf810000,
};

/* A deferred console compute PSO (patch 0005, console_compile.c): vkd3d-proton keeps the RDNA
 * code and, at dispatch, d3d12_xs compiles it with AnyPS5 against the shadow heaps and rebinds it
 * onto vkd3d-proton's bindless sets. The table starts at heap slot 1, so a wrong slot shows. */
static void test_console_dispatch(void *dev, void *queue)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    struct
    {
        UINT32 magic, version, size, stage, wave_size, workgroup_size[3], rsrc1, rsrc2, code_offset, code_dwords;
        UINT64 code_address;
        UINT32 code[ARRAYSIZE(rdna_uav_write)];
    } blob = { 0x44524353, 1, sizeof(blob), 0x20 /* VK_SHADER_STAGE_COMPUTE_BIT */, 32, { 1, 1, 1 },
               0x00ac0000, 2 << 1 /* two user SGPRs */, offsetof(typeof(blob), code), ARRAYSIZE(rdna_uav_write),
               0x200000 };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
                                   .DescriptorTable = { 1, &range } };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { .Format = DXGI_FORMAT_R32_TYPELESS,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = { 0, 64, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW } };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    void *heap = NULL, *rs = NULL, *pso = NULL, *list = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Resource *out_buf = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *rs_blob = NULL;
    UINT inc, *out = NULL;
    SIZE_T cpu;
    UINT64 gpu;
    HRESULT hr;

    memcpy(blob.code, rdna_uav_write, sizeof(blob.code));
    if (!GetModuleHandleA("rebox_anyps5.dll") && !LoadLibraryA("rebox_anyps5.dll"))
    {
        printf("skip deferred console dispatch: no rebox_anyps5.dll\n");
        return;
    }
    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, NULL)))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature, &rs);
    if (SUCCEEDED(hr))
    {
        cd.pRootSignature = rs;
        cd.CS = (D3D12_SHADER_BYTECODE){ &blob, sizeof(blob) };
        hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                &IID_ID3D12PipelineState, &pso);
        CHECK(SUCCEEDED(hr), "a deferred console compute PSO is created without compiling (%#lx)", hr);
    }
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&out_buf);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void **)&fence);
    CHECK(SUCCEEDED(hr), "objects for the deferred dispatch test (%#lx)", hr);
    if (FAILED(hr)) goto done;

    cpu = XSLOT(heap, 9, SIZE_T)(heap);
    gpu = XSLOT(heap, 10, UINT64)(heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    XSLOT(dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(dev,
            out_buf, NULL, &uav, cpu + inc);

    XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
    XSLOT(list, 33, void, void *)(list, rs);
    XSLOT(list, 25, void, void *)(list, pso);
    XSLOT(list, 35, void, UINT, UINT64)(list, 0, gpu + inc);
    XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
    hr = XSLOT(list, 9, HRESULT)(list);
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
    for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
        Sleep(10);
    if (FAILED(hr = ID3D12Resource_Map(out_buf, 0, NULL, (void **)&out)))
    {
        CHECK(0, "map the deferred shader's output (%#lx)", hr);
        goto done;
    }
    CHECK(out[0] == 0x1234abcd, "the deferred console shader, compiled by AnyPS5 at dispatch, wrote %#x through "
          "its V# (expected 0x1234abcd)", out[0]);

    /* Same code, but s[0:1] comes from two root constants and points into GPU memory from
     * ReboxGpuAlloc holding a copy of the V#: the recompiler has to read that memory too. */
    {
        HRESULT (WINAPI *galloc)(SIZE_T, SIZE_T, void **, UINT64 *) = (void *)GetProcAddress(m, "ReboxGpuAlloc");
        HRESULT (WINAPI *shadow_heap)(SIZE_T, UINT64 *, void **, UINT *, UINT *) =
                (void *)GetProcAddress(m, "ReboxShadowHeap");
        D3D12_ROOT_PARAMETER cparam = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                                        .Constants = { 0, 0, 2 } };
        D3D12_ROOT_SIGNATURE_DESC crsd = { 1, &cparam };
        void *crs = NULL, *cpso = NULL, *mem = NULL;
        ID3D10Blob *crs_blob = NULL;
        BYTE *shadow = NULL;
        UINT64 mem_gpu = 0;
        UINT32 ptr[2];

        out[0] = 0;
        hr = galloc(256, 256, &mem, &mem_gpu);
        if (SUCCEEDED(hr))
            hr = shadow_heap(cpu, NULL, (void **)&shadow, NULL, NULL);
        if (SUCCEEDED(hr) && SUCCEEDED(hr = serialize(&crsd, D3D_ROOT_SIGNATURE_VERSION_1, &crs_blob, NULL)))
            hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                    ID3D10Blob_GetBufferPointer(crs_blob), ID3D10Blob_GetBufferSize(crs_blob), &IID_ID3D12RootSignature,
                    &crs);
        if (SUCCEEDED(hr))
        {
            cd.pRootSignature = crs;
            hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                    &IID_ID3D12PipelineState, &cpso);
        }
        if (SUCCEEDED(hr))
            hr = ID3D12CommandAllocator_Reset(alloc);
        if (SUCCEEDED(hr))
            hr = XSLOT(list, 10, HRESULT, ID3D12CommandAllocator *, void *)(list, alloc, NULL);
        CHECK(SUCCEEDED(hr), "objects for the deferred dispatch through GPU memory (%#lx)", hr);
        if (SUCCEEDED(hr))
        {
            memcpy(mem, shadow + inc, 16);
            ptr[0] = (UINT32)mem_gpu;
            ptr[1] = (UINT32)(mem_gpu >> 32);
            XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
            XSLOT(list, 33, void, void *)(list, crs);
            XSLOT(list, 25, void, void *)(list, cpso);
            XSLOT(list, 39, void, UINT, UINT, const void *, UINT)(list, 0, 2, ptr, 0);
            XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
            hr = XSLOT(list, 9, HRESULT)(list);
            XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
            XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 2);
            for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 2; i++)
                Sleep(10);
            CHECK(out[0] == 0x1234abcd, "a deferred console shader whose V# pointer leads into ReboxGpuAlloc "
                  "memory wrote %#x (expected 0x1234abcd)", out[0]);
        }
        if (cpso) XSLOT(cpso, 2, ULONG)(cpso);
        if (crs) XSLOT(crs, 2, ULONG)(crs);
        if (crs_blob) ID3D10Blob_Release(crs_blob);
    }
    ID3D12Resource_Unmap(out_buf, 0, NULL);

done:
    if (fence) ID3D12Fence_Release(fence);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (out_buf) ID3D12Resource_Release(out_buf);
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (rs_blob) ID3D10Blob_Release(rs_blob);
    if (heap) XSLOT(heap, 2, ULONG)(heap);
}

static void test_xbox(void)
{
    HMODULE m = LoadLibraryA("d3d12_xs.dll");
    xbox_create_fn create = m ? (xbox_create_fn)(void *)GetProcAddress(m, "D3D12XboxCreateDevice") : NULL;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = m ? (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature") : NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_ROOT_PARAMETER param = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                                   .Constants = { 0, 0, 1 } };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param };
    void *dev = NULL, *dev2 = NULL, *dev5 = NULL, *queue = NULL, *list = NULL, *heap = NULL, *rs = NULL, *same;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *blob = NULL;
    UINT64 *fields;
    SIZE_T cpu_start;
    UINT64 gpu_start;
    BYTE *embedded;
    HRESULT hr;
    int i;

    CHECK(create && serialize, "D3D12XboxCreateDevice present");
    if (!create || !serialize) return;
    test_gpu_memory(m);

    hr = create(NULL, NULL, &IID_ID3D12Device, &dev);
    if (FAILED(hr))
    {
        printf("skip D3D12XboxCreateDevice failed %#lx (no usable Vulkan device?)\n", hr);
        return;
    }
    hr = create(NULL, NULL, &IID_ID3D12Device2, &dev2);
    CHECK(SUCCEEDED(hr) && dev2 == dev, "D3D12XboxCreateDevice returns one device (%p, %p)", dev, dev2);
    hr = XSLOT(dev, 0, HRESULT, REFIID, void **)(dev, &IID_ID3D12Device5, &dev5);
    CHECK(SUCCEEDED(hr) && dev5 == dev, "QueryInterface for another device version returns the proxy");

    /* Device slot 8 (PC 8), 9 (PC, forwarded unwrapped), 12, 14. */
    hr = XSLOT(dev, 8, HRESULT, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **)(dev, &qd,
            &IID_ID3D12CommandQueue, &queue);
    CHECK(SUCCEEDED(hr), "Xbox CreateCommandQueue (slot 8) %#lx", hr);
    hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&alloc);
    CHECK(SUCCEEDED(hr), "CreateCommandAllocator through forward thunk (slot 9) %#lx", hr);
    hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
            REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    CHECK(SUCCEEDED(hr), "Xbox CreateCommandList (slot 12) %#lx", hr);
    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    CHECK(SUCCEEDED(hr), "Xbox CreateDescriptorHeap (slot 14) %#lx", hr);
    CHECK(queue && alloc && list && heap, "objects created (queue %p, allocator %p, list %p, heap %p)",
          queue, (void *)alloc, list, heap);
    if (!queue || !alloc || !list || !heap) return;

    /* Inline fast paths read cursor +0x18 and limits +0x28/+0x30: all zero. */
    fields = list;
    CHECK(!fields[3] && !fields[5] && !fields[6], "command list cursor and limits are zero");

    hr = XSLOT(list, 7, HRESULT, REFIID, void **)(list, &IID_ID3D12Device, &same);
    CHECK(SUCCEEDED(hr) && same == dev, "command list GetDevice (slot 7) returns the device proxy");

    cpu_start = XSLOT(heap, 9, SIZE_T)(heap);
    gpu_start = XSLOT(heap, 10, UINT64)(heap);
    CHECK(cpu_start && gpu_start, "descriptor heap starts returned in RAX (slots 9, 10): %#llx %#llx",
          (unsigned long long)cpu_start, (unsigned long long)gpu_start);

    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
    CHECK(SUCCEEDED(hr), "root signature through the device proxy (slot 16) %#lx", hr);

    /* Record through Xbox slots: heaps (32), root signature (33), constant
     * (37), embedded data (148). */
    XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
    if (rs)
    {
        XSLOT(list, 33, void, void *)(list, rs);
        XSLOT(list, 37, void, UINT, UINT, UINT)(list, 0, 42, 0);
    }
    embedded = XSLOT(list, 148, void *, UINT64, UINT64)(list, 1000, 256);
    CHECK(embedded && !((ULONG_PTR)embedded & 255), "embedded allocation (slot 148) %p", embedded);
    if (embedded) memset(embedded, 0, 1000);
    hr = XSLOT(list, 9, HRESULT)(list);
    CHECK(SUCCEEDED(hr), "Close (slot 9) after Xbox-slot recording %#lx", hr);

    hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence);
    CHECK(SUCCEEDED(hr), "CreateFence (slot 36) %#lx", hr);

    /* Unproxied children report the device proxy. (D3D12CreateDevice on the
     * same adapter returns the same vkd3d-proton device, so there is no
     * separate PC-path device to compare against.) */
    if (fence)
    {
        ID3D12Device *got = NULL;

        hr = ID3D12Fence_GetDevice(fence, &IID_ID3D12Device, (void **)&got);
        CHECK(SUCCEEDED(hr) && (void *)got == dev, "raw fence GetDevice returns the device proxy (%p)", (void *)got);
        if (got) ID3D12Device_Release(got);
    }
    XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
    if (fence)
    {
        hr = XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
        for (i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
            Sleep(10);
        CHECK(SUCCEEDED(hr) && ID3D12Fence_GetCompletedValue(fence) == 1,
              "ExecuteCommandLists (queue slot 10) unwraps the list and the GPU finishes it");
    }

    hr = XSLOT(dev, 100, HRESULT, UINT_PTR, UINT_PTR, UINT_PTR)(dev, 1, 2, 3);
    CHECK(hr == E_NOTIMPL, "unidentified device slot traps with E_NOTIMPL (%#lx)", hr);

    test_pacing(dev);
    test_real_heap_va(dev, queue);
    test_console_shader(dev, queue);
    test_console_graphics(dev, queue);
    test_shadow_heap(dev);
    test_console_dispatch(dev, queue);
    test_present(dev, queue);
    test_ffx(dev, queue);
    test_present_format(dev, queue);

    /* Slot 60, graphics PSO with an extension array (TomBox): console VS/PS
     * bytes get fallbacks unless REBOX_SHADER_FALLBACK=0. */
    if (rs)
    {
        static const char vs_bytes[] = "REBOX-FAKE-CONSOLE-VS60", ps_bytes[] = "REBOX-FAKE-CONSOLE-PS60";
        const char *fb = getenv("REBOX_SHADER_FALLBACK");
        BOOL strict = fb && *fb == '0';
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = { 0 };
        BYTE ext[0x40] = { 0 };
        void *pso = NULL;

        gd.pRootSignature = rs;
        gd.VS = (D3D12_SHADER_BYTECODE){ vs_bytes, sizeof(vs_bytes) };
        gd.PS = (D3D12_SHADER_BYTECODE){ ps_bytes, sizeof(ps_bytes) };
        gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        gd.SampleMask = ~0u;
        gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        gd.RasterizerState.DepthClipEnable = TRUE;
        gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gd.NumRenderTargets = 1;
        gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        gd.SampleDesc.Count = 1;
        hr = XSLOT(dev, 60, HRESULT, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, UINT, const void *, REFIID, void **)(
                dev, &gd, 1, ext, &IID_ID3D12PipelineState, &pso);
        CHECK(SUCCEEDED(hr) != strict, "graphics PSO through slot 60 with an extension %s (%#lx)",
              strict ? "rejected in strict mode" : "gets fallbacks", hr);
        if (pso) XSLOT(pso, 2, ULONG)(pso);
    }

    /* Slot 76 as the FidelityFX upscaler calls it (Xbox2Windows): a 0x40-byte
     * desc with Xbox texture layout 0x101 and extension word 3. */
    {
        D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 48, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   { 1, 0 }, (D3D12_TEXTURE_LAYOUT)0x101,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
        D3D12_RESOURCE_ALLOCATION_INFO info = { 0 };
        BYTE desc_x[0x40] = { 0 };
        ID3D12Resource *tex = NULL;

        memcpy(desc_x, &td, 0x34);
        *(UINT *)(desc_x + 0x34) = 3;
        hr = XSLOT(dev, 76, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const void *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, desc_x, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource,
                (void **)&tex);
        CHECK(SUCCEEDED(hr) && tex, "slot 76 texture with Xbox layout 0x101 (%#lx)", hr);
        if (tex) ID3D12Resource_Release(tex);
        XSLOT(dev, 25, D3D12_RESOURCE_ALLOCATION_INFO *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
              const D3D12_RESOURCE_DESC *)(dev, &info, 0, 1, &td);
        CHECK(info.SizeInBytes >= 64 * 48 * 4, "GetResourceAllocationInfo (slot 25) with Xbox layout 0x101 (%llu)",
              (unsigned long long)info.SizeInBytes);
    }

    /* DEFAULT-heap buffers become CPU-visible, so a title can write through
     * their GPU VA as on the console. */
    {
        D3D12_HEAP_PROPERTIES heap_props = { D3D12_HEAP_TYPE_DEFAULT }, got;
        D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 65536, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        D3D12_HEAP_FLAGS flags;
        ID3D12Resource *buf = NULL;

        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &heap_props,
                D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&buf);
        if (SUCCEEDED(hr))
            hr = ID3D12Resource_GetHeapProperties(buf, &got, &flags);
        CHECK(SUCCEEDED(hr) && got.Type == D3D12_HEAP_TYPE_CUSTOM
              && got.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
              "DEFAULT-heap buffer is created CPU-visible (heap type %u)", got.Type);
        if (buf)
        {
            UINT64 va = ID3D12Resource_GetGPUVirtualAddress(buf);
            D3D12_RANGE none = { 0, 0 };
            void *cpu = NULL;

            ID3D12Resource_Map(buf, 0, &none, &cpu);
            if (gpu_identity)
                CHECK((UINT64)(ULONG_PTR)cpu == va, "DEFAULT-heap buffer is identity-mapped too (%p, %#llx)", cpu,
                      (unsigned long long)va);
            else
                printf("info DEFAULT-heap buffer at CPU %p, GPU VA %#llx\n", cpu, (unsigned long long)va);
            ID3D12Resource_Release(buf);
        }
    }

    if (fence) ID3D12Fence_Release(fence);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (blob) ID3D10Blob_Release(blob);
    XSLOT(list, 2, ULONG)(list);
    XSLOT(heap, 2, ULONG)(heap);
    XSLOT(queue, 2, ULONG)(queue);
    ID3D12CommandAllocator_Release(alloc);
}

/* XAudio2 via the xaudio2_9.dll shim: XMA2 voices must be creatable (plain
 * Wine fails them) and play out as silence with callbacks on time. */
#pragma pack(push, 1)
typedef struct
{
    WAVEFORMATEX wfx;
    WORD NumStreams;
    DWORD ChannelMask, SamplesEncoded, BytesPerBlock, PlayBegin, PlayLength, LoopBegin, LoopLength;
    BYTE LoopCount, EncoderVersion;
    WORD BlockCount;
} xma2_format;
#pragma pack(pop)

static LONG xma_buffers_ended;

static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *cb, UINT32 bytes) {}
static void STDMETHODCALLTYPE cb_void(IXAudio2VoiceCallback *cb) {}
static void STDMETHODCALLTYPE cb_ctx(IXAudio2VoiceCallback *cb, void *ctx) {}
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *cb, void *ctx)
{
    if (ctx == (void *)0x1234)
        InterlockedIncrement(&xma_buffers_ended);
}
static void STDMETHODCALLTYPE cb_error(IXAudio2VoiceCallback *cb, void *ctx, HRESULT hr) {}

static IXAudio2VoiceCallbackVtbl cb_vtbl = { cb_pass_start, cb_void, cb_void, cb_ctx, cb_buffer_end, cb_ctx, cb_error };
static IXAudio2VoiceCallback voice_cb = { &cb_vtbl };

/* Wine's CreateMasteringVoice reports success even without an audio device,
 * so ask mmdevapi whether there is a render endpoint. */
static BOOL have_audio_device(void)
{
    HMODULE ole32 = LoadLibraryA("ole32.dll");
    HRESULT (WINAPI *co_init)(void *, DWORD) = (void *)GetProcAddress(ole32, "CoInitializeEx");
    HRESULT (WINAPI *co_create)(REFCLSID, IUnknown *, DWORD, REFIID, void **) =
            (void *)GetProcAddress(ole32, "CoCreateInstance");
    IMMDeviceEnumerator *e = NULL;
    IMMDevice *dev = NULL;
    BOOL ok = FALSE;

    co_init(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(co_create(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator,
                            (void **)&e)))
    {
        ok = SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, eRender, eConsole, &dev));
        if (dev) IMMDevice_Release(dev);
        IMMDeviceEnumerator_Release(e);
    }
    return ok;
}

static void test_xaudio2(void)
{
    HMODULE m = LoadLibraryA("xaudio2_9.dll");
    HRESULT (WINAPI *create)(IXAudio2 **, UINT32, UINT32) = m ? (void *)GetProcAddress(m, MAKEINTRESOURCEA(1)) : NULL;
    xma2_format fmt = { { 0x166, 2, 48000, 48000 * 4, 2048, 16, sizeof(xma2_format) - sizeof(WAVEFORMATEX) },
                        1, 3, 48000, 65536, 0, 0, 0, 0, 0, 2, 1 };
    static BYTE xma_data[2048];
    XAUDIO2_BUFFER buf = { XAUDIO2_END_OF_STREAM, sizeof(xma_data), xma_data, 0, 4800, 0, 0, 0, (void *)0x1234 };
    IXAudio2MasteringVoice *master = NULL;
    IXAudio2SourceVoice *voice = NULL;
    IXAudio2 *xa = NULL;
    HRESULT hr;

    CHECK(create != NULL, "xaudio2_9 exports XAudio2Create at ordinal 1");
    if (!create) return;
    hr = create(&xa, 0, 0);
    CHECK(SUCCEEDED(hr) && xa, "XAudio2Create (%#lx)", hr);
    if (FAILED(hr)) return;

    if (!have_audio_device() || FAILED(hr = IXAudio2_CreateMasteringVoice(xa, &master, 0, 0, 0, NULL, NULL,
                                                                           AudioCategory_GameEffects)))
    {
        /* No audio device (sandbox): FAudio can't create any source voice
         * then (it reads the missing mastering voice), so the XMA2 path is
         * only checked on machines with audio (tests/manual-test.sh). */
        printf("skip XMA2 voice checks: no audio device (%#lx)\n", hr);
        IXAudio2_Release(xa);
        return;
    }
    hr = IXAudio2_CreateSourceVoice(xa, &voice, &fmt.wfx, 0, 2.0f, &voice_cb, NULL, NULL);
    CHECK(SUCCEEDED(hr) && voice, "XMA2 source voice (%#lx)", hr);
    if (voice)
    {
        hr = IXAudio2SourceVoice_SubmitSourceBuffer(voice, &buf, NULL);
        CHECK(SUCCEEDED(hr), "XMA2 buffer submitted (%#lx)", hr);
        IXAudio2SourceVoice_Start(voice, 0, 0);
        for (int i = 0; i < 300 && !xma_buffers_ended; i++)
            Sleep(10);
        CHECK(xma_buffers_ended == 1, "XMA2 buffer played out as silence: OnBufferEnd with its context");
        IXAudio2SourceVoice_DestroyVoice(voice);
    }
    IXAudio2MasteringVoice_DestroyVoice(master);
    IXAudio2_Release(xa);
}

/* smoke.exe --pso-check DIR: create a compute PSO from every compute .scsv
 * in DIR (translated game shaders from tools/scsv_pack.py; never part of the
 * repo) through the d3d12_xs device, with an empty root signature: console
 * shaders use only the push block and the bindless heaps. Reports how many
 * vkd3d-proton and the driver accept. */
static int pso_check(const char *dir)
{
    HMODULE m = LoadLibraryA("d3d12_xs.dll");
    xbox_create_fn create = m ? (xbox_create_fn)(void *)GetProcAddress(m, "D3D12XboxCreateDevice") : NULL;
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = m ? (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature") : NULL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    void *dev = NULL, *rs = NULL;
    ID3D10Blob *blob = NULL;
    int ok = 0, failed = 0, other = 0;
    HANDLE find;
    HRESULT hr;

    if (!create || FAILED(create(NULL, NULL, &IID_ID3D12Device, &dev))
            || FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL))
            || FAILED(XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs)))
    {
        printf("FAIL pso-check: no device or root signature\n");
        return 1;
    }
    cd.pRootSignature = rs;
    snprintf(pattern, sizeof(pattern), "%s\\*.scsv", dir);
    if ((find = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
    {
        printf("FAIL pso-check: no .scsv files in %s\n", dir);
        return 1;
    }
    do
    {
        SIZE_T size;
        BYTE *data;
        void *pso = NULL;

        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (!(data = read_file(path, &size)))
            continue;
        if (size < 44 || *(UINT *)(data + 8) != 0x20 /* compute */)
        {
            other++;
            free(data);
            continue;
        }
        cd.CS.pShaderBytecode = data;
        cd.CS.BytecodeLength = size;
        hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                &IID_ID3D12PipelineState, &pso);
        if (SUCCEEDED(hr))
        {
            ok++;
            XSLOT(pso, 2, ULONG)(pso);
        }
        else if (failed++ < 10)
            printf("info pso-check: %s failed %#lx\n", fd.cFileName, hr);
        free(data);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    printf("%s pso-check: %d of %d compute PSOs created from translated shaders (%d other stages skipped)\n",
           failed ? "FAIL" : "ok  ", ok, ok + failed, other);
    return failed != 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--pso-check"))
        return pso_check(argv[2]);
    test_xmem();
    if (argc > 1)
        test_d3d12(argv[1], !(argc > 2 && !strcmp(argv[2], "nocache")));
    test_xbox();
    test_xaudio2();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
