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
#include <dxgi.h>
#include <xaudio2.h>
#include <mmdeviceapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../generated/test_shaders.h"
#include "../anyps5/rebox_anyps5.h"
#include "../generated/repro_shaders.h"

static int failures;

/* Heap slot 10 reports shadow-heap title addresses unless REBOX_HEAP_VA says real or virtual;
 * the shadow-heap and deferred-shader tests need them. */
static BOOL title_heap_va(void)
{
    const char *mode = getenv("REBOX_HEAP_VA"), *legacy = getenv("REBOX_REAL_HEAP_VA");
    if (mode)
        return !strcmp(mode, "title");
    return !(legacy && legacy[0] == '0');
}

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

/* Xbox-only root signature flags (0x01000000, 0x02000000, 0x80000000; Leonida found the PC
 * deserializer rejects them): serializing, creating and deserializing must all work. */
static void test_xbox_root_flags(void *dev)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER deserialize = (PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER)(void *)
            GetProcAddress(m, "D3D12CreateRootSignatureDeserializer");
    D3D12_ROOT_PARAMETER param = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 1 } };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, 0x83000000 };
    ID3D12RootSignatureDeserializer *des = NULL;
    ID3D10Blob *blob = NULL, *err = NULL;
    void *rs = NULL;
    HRESULT hr;

    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    CHECK(SUCCEEDED(hr), "D3D12SerializeRootSignature takes Xbox-only flags 0x83000000 (%#lx)", hr);
    if (FAILED(hr))
        goto done;
    hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &rs);
    CHECK(SUCCEEDED(hr), "CreateRootSignature (device slot 16) takes a blob with Xbox-only flags (%#lx)", hr);
    hr = deserialize(ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignatureDeserializer, (void **)&des);
    CHECK(SUCCEEDED(hr), "D3D12CreateRootSignatureDeserializer takes a blob with Xbox-only flags (%#lx)", hr);
    if (des)
    {
        const D3D12_ROOT_SIGNATURE_DESC *d = ID3D12RootSignatureDeserializer_GetRootSignatureDesc(des);
        printf("info deserialized root signature flags %#x\n", d ? d->Flags : 0);
        ID3D12RootSignatureDeserializer_Release(des);
    }
done:
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (blob) ID3D10Blob_Release(blob);
    if (err) ID3D10Blob_Release(err);
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
           : title_heap_va() ? "shadow-heap title address" : "real heap address");
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
    if (!title_heap_va())
    {
        printf("skip shadow heap test: REBOX_HEAP_VA turns title addresses off\n");
        return;
    }
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

/* A console DXBC container as the game ships them: a DXIL part holding an XBCH header and a zlib
 * stream (here one stored deflate block) of FSSM, then a NOBC stub. The FSSM header gives the code
 * offset (u32 at 0xC0, plus 0x28), float mode (0x104), RSRC2 (0x138) and thread-group size (0x13C).
 * Returns the container's size; free *out. */
static SIZE_T make_console_container(const UINT32 *code, UINT dwords, UINT32 rsrc2, BYTE **out)
{
    SIZE_T fssm_size = 0x168 + dwords * 4, zsize = 2 + 5 + fssm_size + 4, part = 4 + 0x60 + zsize + 4 + 0x14;
    SIZE_T total = 0x24 + 8 + ((part + 3) & ~(SIZE_T)3);
    BYTE *c = calloc(1, total), *fssm = calloc(1, fssm_size), *p;
    UINT32 a = 1, b = 0;

    memcpy(fssm, "FSSM", 4);
    *(UINT32 *)(fssm + 0xc0) = 0x168 - 0x28;
    *(UINT32 *)(fssm + 0x104) = 0xc0;
    *(UINT32 *)(fssm + 0x138) = rsrc2;
    *(UINT32 *)(fssm + 0x13c) = 1;
    *(UINT32 *)(fssm + 0x140) = 1;
    *(UINT32 *)(fssm + 0x144) = 1;
    memcpy(fssm + 0x168, code, dwords * 4);
    for (SIZE_T i = 0; i < fssm_size; i++)
    {
        a = (a + fssm[i]) % 65521;
        b = (b + a) % 65521;
    }
    memcpy(c, "DXBC", 4);
    *(UINT32 *)(c + 0x14) = 1;
    *(UINT32 *)(c + 0x18) = (UINT32)total;
    *(UINT32 *)(c + 0x1c) = 1;
    *(UINT32 *)(c + 0x20) = 0x24;
    p = c + 0x24;
    memcpy(p, "DXIL", 4);
    *(UINT32 *)(p + 4) = (UINT32)(total - 0x24 - 8);
    p += 8;
    memcpy(p, "XBCH", 4);
    p += 4 + 0x60;
    *p++ = 0x78;
    *p++ = 0x01;
    *p++ = 0x01; /* final stored block */
    *p++ = (BYTE)fssm_size;
    *p++ = (BYTE)(fssm_size >> 8);
    *p++ = (BYTE)~fssm_size;
    *p++ = (BYTE)(~fssm_size >> 8);
    memcpy(p, fssm, fssm_size);
    p += fssm_size;
    *p++ = (BYTE)(b >> 8);
    *p++ = (BYTE)b;
    *p++ = (BYTE)(a >> 8);
    *p++ = (BYTE)a;
    memcpy(p, "NOBC", 4);
    free(fssm);
    *out = c;
    return total;
}

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
    if (!title_heap_va())
    {
        printf("skip deferred console dispatch: REBOX_HEAP_VA turns title addresses off\n");
        return;
    }
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

        /* The same code in a console container, created as an ordinary compute PSO: d3d12_xs
         * turns its FSSM into a deferred console shader (console_fssm.c). */
        {
            void *fpso = NULL;
            BYTE *container = NULL;
            SIZE_T csize = make_console_container(rdna_uav_write, ARRAYSIZE(rdna_uav_write), 2 << 1, &container);

            out[0] = 0;
            cd.pRootSignature = rs;
            cd.CS = (D3D12_SHADER_BYTECODE){ container, csize };
            hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                    &IID_ID3D12PipelineState, &fpso);
            CHECK(SUCCEEDED(hr), "a compute PSO from a console FSSM container (%#lx)", hr);
            if (SUCCEEDED(hr) && SUCCEEDED(ID3D12CommandAllocator_Reset(alloc))
                    && SUCCEEDED(XSLOT(list, 10, HRESULT, ID3D12CommandAllocator *, void *)(list, alloc, NULL)))
            {
                XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
                XSLOT(list, 33, void, void *)(list, rs);
                XSLOT(list, 25, void, void *)(list, fpso);
                XSLOT(list, 35, void, UINT, UINT64)(list, 0, gpu + inc);
                XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
                XSLOT(list, 9, HRESULT)(list);
                XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
                XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 3);
                for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 3; i++)
                    Sleep(10);
                CHECK(out[0] == 0x1234abcd, "the console container's FSSM code ran through AnyPS5 at dispatch "
                      "(wrote %#x)", out[0]);
            }
            if (fpso) XSLOT(fpso, 2, ULONG)(fpso);
            free(container);
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

/* Dispatches gfx1030 code once through a deferred console PSO, with its user SGPRs as root
 * constants and heap bound. Returns d3d12_xs's compile result (message in msg); a GPU that never
 * finishes counts as failure. */
static HRESULT dispatch_rdna(void *dev, void *queue, void *heap, const UINT32 *code, UINT dwords, UINT32 rsrc2,
        UINT threads, const UINT32 *sgprs, UINT nsgprs, char *msg, size_t msg_size)
{
    HMODULE m = GetModuleHandleA("d3d12_xs.dll");
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)
            GetProcAddress(m, "D3D12SerializeRootSignature");
    HRESULT (WINAPI *last_result)(char *, SIZE_T) = (void *)GetProcAddress(m, "ReboxConsoleLastResult");
    D3D12_ROOT_PARAMETER p = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, nsgprs } };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &p };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    struct { UINT32 magic, version, size, stage, wave_size, workgroup_size[3], rsrc1, rsrc2, code_offset, code_dwords;
             UINT64 code_address; } *blob;
    void *rs = NULL, *pso = NULL, *list = NULL;
    ID3D12CommandAllocator *alloc = NULL;
    ID3D12Fence *fence = NULL;
    ID3D10Blob *rs_blob = NULL;
    HRESULT hr;

    snprintf(msg, msg_size, "setup failed");
    if (!(blob = calloc(1, sizeof(*blob) + dwords * 4)))
        return E_OUTOFMEMORY;
    *blob = (typeof(*blob)){ 0x44524353, 1, sizeof(*blob) + dwords * 4, 0x20, 32, { threads, 1, 1 }, 0x00ac0000,
                             rsrc2, sizeof(*blob), dwords, 0x300000 };
    memcpy(blob + 1, code, dwords * 4);
    if (SUCCEEDED(hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, NULL)))
        hr = XSLOT(dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(dev, 0,
                ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature, &rs);
    if (SUCCEEDED(hr))
    {
        cd.pRootSignature = rs;
        cd.CS = (D3D12_SHADER_BYTECODE){ blob, blob->size };
        hr = XSLOT(dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(dev, &cd,
                &IID_ID3D12PipelineState, &pso);
    }
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                &IID_ID3D12CommandAllocator, (void **)&alloc);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *,
                REFIID, void **)(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, &list);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(dev, 0, D3D12_FENCE_FLAG_NONE,
                &IID_ID3D12Fence, (void **)&fence);
    if (SUCCEEDED(hr))
    {
        XSLOT(list, 32, void, UINT, void *const *)(list, 1, &heap);
        XSLOT(list, 33, void, void *)(list, rs);
        XSLOT(list, 25, void, void *)(list, pso);
        XSLOT(list, 39, void, UINT, UINT, const void *, UINT)(list, 0, nsgprs, sgprs, 0);
        XSLOT(list, 14, void, UINT, UINT, UINT)(list, 1, 1, 1);
        XSLOT(list, 9, HRESULT)(list);
        XSLOT(queue, 10, void, UINT, void *const *)(queue, 1, &list);
        XSLOT(queue, 14, HRESULT, ID3D12Fence *, UINT64)(queue, fence, 1);
        for (int i = 0; i < 6000 && ID3D12Fence_GetCompletedValue(fence) < 1; i++)
            Sleep(10);
        hr = last_result(msg, msg_size);
        if (ID3D12Fence_GetCompletedValue(fence) < 1)
        {
            snprintf(msg, msg_size, "the GPU did not finish");
            hr = E_FAIL;
        }
    }
    if (fence) ID3D12Fence_Release(fence);
    if (list) XSLOT(list, 2, ULONG)(list);
    if (alloc) ID3D12CommandAllocator_Release(alloc);
    if (pso) XSLOT(pso, 2, ULONG)(pso);
    if (rs) XSLOT(rs, 2, ULONG)(rs);
    if (rs_blob) ID3D10Blob_Release(rs_blob);
    free(blob);
    return hr;
}

/* Whether the first DXGI adapter is lavapipe (llvmpipe), whose 8-wide subgroups split a guest wave. */
static BOOL on_lavapipe(void)
{
    HRESULT (WINAPI *create)(REFIID, void **) = (void *)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory1");
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL;
    DXGI_ADAPTER_DESC1 desc;
    BOOL lavapipe = FALSE;

    if (create && SUCCEEDED(create(&IID_IDXGIFactory1, (void **)&factory)))
    {
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, 0, &adapter)))
        {
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc)))
                lavapipe = wcsstr(desc.Description, L"llvmpipe") != NULL;
            IDXGIAdapter1_Release(adapter);
        }
        IDXGIFactory1_Release(factory);
    }
    return lavapipe;
}

/* Hand-written gfx1030 reproductions of recompiler bugs (tests/rdna, from xsx-shader-lab's repro/),
 * through the whole deferred path: the XOR-swapped table pointer of patches/anyps5/0003, the
 * speculated pointer and descriptor of 0004, and two loops with exec save/restore whose output is
 * checked. */
static void test_rdna_repros(void *dev, void *queue)
{
    static const struct { const char *name; const UINT32 *code; UINT dwords; } image_cases[] = {
        { "xor-swap (patch 0003)", repro_xor_swap, ARRAYSIZE(repro_xor_swap) },
        { "speculation-pointer (patch 0004)", repro_speculation_pointer, ARRAYSIZE(repro_speculation_pointer) },
        { "speculation-pointer-taken (patch 0004)", repro_speculation_pointer_taken, ARRAYSIZE(repro_speculation_pointer_taken) },
        { "speculation-descriptor (patch 0004)", repro_speculation_descriptor, ARRAYSIZE(repro_speculation_descriptor) },
    };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE1D, 0, 64, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    D3D12_UNORDERED_ACCESS_VIEW_DESC tuav = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_UAV_DIMENSION_TEXTURE1D };
    D3D12_UNORDERED_ACCESS_VIEW_DESC buav = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                              .Buffer = { 0, 1024, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW } };
    ID3D12Resource *tex = NULL, *buf = NULL;
    void *heap = NULL;
    UINT32 *out = NULL, sgprs[7];
    UINT64 gpu;
    SIZE_T cpu;
    UINT inc;
    char msg[512];
    HRESULT hr;

    if (!title_heap_va() || !LoadLibraryA("rebox_anyps5.dll"))
    {
        printf("skip RDNA repros: no title addresses or rebox_anyps5.dll\n");
        return;
    }
    hr = XSLOT(dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(dev, &hd,
            &IID_ID3D12DescriptorHeap, &heap);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&tex);
    if (SUCCEEDED(hr))
        hr = XSLOT(dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(dev, &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&buf);
    if (SUCCEEDED(hr))
        hr = ID3D12Resource_Map(buf, 0, NULL, (void **)&out);
    CHECK(SUCCEEDED(hr), "objects for the RDNA repros (%#lx)", hr);
    if (FAILED(hr))
        goto done;
    cpu = XSLOT(heap, 9, SIZE_T)(heap);
    gpu = XSLOT(heap, 10, UINT64)(heap);
    inc = XSLOT(dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    XSLOT(dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(dev,
            tex, NULL, &tuav, cpu);
    XSLOT(dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(dev,
            buf, NULL, &buav, cpu + inc);

    /* s[0:1] = the table (a 1D storage image at slot 0); s2 = the workgroup id. pointer-taken runs
     * the path that clobbers the speculated pointer, so its run-time check must report a fault. */
    for (UINT i = 0; i < ARRAYSIZE(image_cases); i++)
    {
        UINT (WINAPI *faults)(void) = (void *)GetProcAddress(GetModuleHandleA("d3d12_xs.dll"), "ReboxConsoleFaults");
        UINT before = faults ? faults() : 0;
        sgprs[0] = (UINT32)gpu;
        sgprs[1] = 0;
        hr = dispatch_rdna(dev, queue, heap, image_cases[i].code, image_cases[i].dwords, 2 << 1 | 1 << 7, 32, sgprs, 2,
                           msg, sizeof(msg));
        CHECK(SUCCEEDED(hr), "RDNA repro %s compiles and runs: %s", image_cases[i].name, msg);
        if (faults && strstr(image_cases[i].name, "pointer-taken"))
            CHECK(faults() > before, "a failed speculation is read back from AnyPS5's fault buffer (%u fault(s))",
                  faults() - before);
        else if (faults && strstr(image_cases[i].name, "speculation"))
            CHECK(faults() == before, "%s reports no fault", image_cases[i].name);
    }

    /* Two exec-masked loops through LDS: s4 = a table with the output V# (slot 1), s5 = n1,
     * s6 = n2. out[x] = (x & 63) + 101 if (x & 63) < n1, else 0, for x < n2. */
    memset(out, 0xcc, 4096);
    memset(sgprs, 0, sizeof(sgprs));
    sgprs[4] = (UINT32)(gpu + inc);
    sgprs[5] = 40;
    sgprs[6] = 100;
    hr = dispatch_rdna(dev, queue, heap, repro_wrexec_loops, ARRAYSIZE(repro_wrexec_loops), 7 << 1 | 8 << 15, 32, sgprs, 7,
                       msg, sizeof(msg));
    {
        UINT good = 0, first_bad = ~0u;
        for (UINT x = 0; x < 100; x++)
        {
            UINT32 expect = (x & 63) < 40 ? (x & 63) + 101 : 0;
            if (out[x] == expect) good++;
            else if (first_bad == ~0u) first_bad = x;
        }
        /* Known lavapipe-only miscompare (xsx-shader-lab): its 8-wide subgroups split the guest
         * wave, and loops ending on a partial wave lose lanes. Exact on RADV. */
        if (SUCCEEDED(hr) && good != 100 && on_lavapipe())
            printf("info RDNA repro wrexec-loops: %u of 100 outputs right on lavapipe (known miscompare: 8-wide "
                   "subgroups; exact on RADV)\n", good);
        else
            CHECK(SUCCEEDED(hr) && good == 100, "RDNA repro wrexec-loops: %u of 100 outputs right (%s%s)", good, msg,
                  first_bad != ~0u ? "; first wrong is shown below" : "");
        if (first_bad != ~0u)
            printf("     out[%u] = %#x\n", first_bad, out[first_bad]);
    }
    ID3D12Resource_Unmap(buf, 0, NULL);

done:
    if (tex) ID3D12Resource_Release(tex);
    if (buf) ID3D12Resource_Release(buf);
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
    test_xbox_root_flags(dev);
    test_real_heap_va(dev, queue);
    test_console_shader(dev, queue);
    test_console_graphics(dev, queue);
    test_shadow_heap(dev);
    test_console_dispatch(dev, queue);
    test_rdna_repros(dev, queue);
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

/* smoke.exe --console-cs PLAN [FIRST [COUNT]]: runs game compute shaders through the deferred
 * console path (tools/console-cs-plan.py writes PLAN from an xsx-shader-lab checkout; game data is
 * never in the repo). For each shader the descriptor tables rdna-run inferred are rebuilt in real
 * D3D12 heaps with synthetic resources, every user SGPR is passed as a root constant (table
 * pointers as the tables' title addresses: they are 32-bit, and some are adjacent, which root-order
 * packing can't express), and the code is dispatched once through an SCRD PSO. Each shader gets a
 * "cs NAME STATUS: message" line: dispatched (compiled or cached, and the GPU finished), skipped
 * (the compile failed; d3d12_xs's message follows) or hung (no result in REBOX_CONSOLE_CS_TIMEOUT seconds, default 300; the sweep stops,
 * since the queue is stuck; REBOX_CONSOLE_CS_SKIP=NAME,... leaves shaders out). */
/* sa5_analyse from rebox_anyps5.dll. */
static int (*cs_analyse)(const UINT32 *, UINT32, UINT32, struct sa5_layout *, struct sa5_slot *, UINT32, char *, size_t);

enum cs_resource { CS_TEX1D, CS_TEX2D, CS_TEX2DARRAY, CS_TEXCUBE, CS_TEX3D, CS_UAV1D, CS_UAV2D, CS_UAV2DARRAY,
                   CS_UAV3D, CS_BUFFER, CS_CBUFFER, CS_RESOURCE_COUNT };

struct console_cs
{
    void *dev, *queue, *list, *heap, *sampler_heap, *rs[33];
    ID3D12CommandAllocator *alloc;
    ID3D12Fence *fence;
    ID3D12Resource *res[CS_RESOURCE_COUNT];
    PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize;
    HRESULT (WINAPI *last_result)(char *, SIZE_T);
    UINT (WINAPI *faults)(void);
    UINT faults_seen;
    SIZE_T cpu, sampler_cpu;
    UINT64 gpu, sampler_gpu, fence_value;
    UINT inc, sampler_inc;
};

static ID3D12Resource *cs_create(struct console_cs *c, D3D12_RESOURCE_DIMENSION dim, UINT64 width, UINT height,
        UINT16 depth, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { dim, 0, width, height, depth, 1,
                               dim == D3D12_RESOURCE_DIMENSION_BUFFER ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R8G8B8A8_UNORM,
                               { 1, 0 }, dim == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                               : D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    ID3D12Resource *r = NULL;

    XSLOT(c->dev, 27, HRESULT, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
          D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **)(c->dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
          D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
    return r;
}

static BOOL cs_setup(struct console_cs *c)
{
    HMODULE m = LoadLibraryA("d3d12_xs.dll");
    xbox_create_fn create = m ? (xbox_create_fn)(void *)GetProcAddress(m, "D3D12XboxCreateDevice") : NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 * 512,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC sd = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (!create || !LoadLibraryA("rebox_anyps5.dll"))
        return FALSE;
    c->serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(void *)GetProcAddress(m, "D3D12SerializeRootSignature");
    c->last_result = (void *)GetProcAddress(m, "ReboxConsoleLastResult");
    c->faults = (void *)GetProcAddress(m, "ReboxConsoleFaults");
    cs_analyse = (void *)GetProcAddress(GetModuleHandleA("rebox_anyps5.dll"), "sa5_analyse");
    if (!cs_analyse)
        return FALSE;
    if (!c->last_result || FAILED(create(NULL, NULL, &IID_ID3D12Device, &c->dev))
            || FAILED(XSLOT(c->dev, 8, HRESULT, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **)(c->dev, &qd,
                &IID_ID3D12CommandQueue, &c->queue))
            || FAILED(XSLOT(c->dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(c->dev, &hd,
                &IID_ID3D12DescriptorHeap, &c->heap))
            || FAILED(XSLOT(c->dev, 14, HRESULT, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **)(c->dev, &sd,
                &IID_ID3D12DescriptorHeap, &c->sampler_heap))
            || FAILED(XSLOT(c->dev, 9, HRESULT, D3D12_COMMAND_LIST_TYPE, REFIID, void **)(c->dev,
                D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&c->alloc))
            || FAILED(XSLOT(c->dev, 12, HRESULT, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *,
                ID3D12PipelineState *, REFIID, void **)(c->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->alloc, NULL,
                &IID_ID3D12GraphicsCommandList, &c->list))
            || FAILED(XSLOT(c->dev, 36, HRESULT, UINT64, D3D12_FENCE_FLAGS, REFIID, void **)(c->dev, 0,
                D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&c->fence)))
        return FALSE;
    XSLOT(c->list, 9, HRESULT)(c->list);
    c->cpu = XSLOT(c->heap, 9, SIZE_T)(c->heap);
    c->gpu = XSLOT(c->heap, 10, UINT64)(c->heap);
    c->sampler_cpu = XSLOT(c->sampler_heap, 9, SIZE_T)(c->sampler_heap);
    c->sampler_gpu = XSLOT(c->sampler_heap, 10, UINT64)(c->sampler_heap);
    c->inc = XSLOT(c->dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(c->dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    c->sampler_inc = XSLOT(c->dev, 15, UINT, D3D12_DESCRIPTOR_HEAP_TYPE)(c->dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    if (c->inc != 32 || c->sampler_inc < 16 || c->sampler_inc > 32 || c->gpu >> 32 || c->sampler_gpu >> 32)
    {
        printf("FAIL console-cs: needs 32-byte resource slots, 16- or 32-byte sampler slots and title addresses "
               "(increments %u/%u, heaps at %#llx/%#llx)\n", c->inc, c->sampler_inc, (unsigned long long)c->gpu,
               (unsigned long long)c->sampler_gpu);
        return FALSE;
    }
    c->res[CS_TEX1D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 64, 1, 1, 0);
    c->res[CS_TEX2D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, 0);
    c->res[CS_TEX2DARRAY] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 2, 0);
    c->res[CS_TEXCUBE] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 32, 32, 6, 0);
    c->res[CS_TEX3D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 16, 16, 16, 0);
    c->res[CS_UAV1D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 64, 1, 1, uav);
    c->res[CS_UAV2D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 1, uav);
    c->res[CS_UAV2DARRAY] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, 64, 2, uav);
    c->res[CS_UAV3D] = cs_create(c, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 16, 16, 16, uav);
    c->res[CS_BUFFER] = cs_create(c, D3D12_RESOURCE_DIMENSION_BUFFER, 1 << 20, 1, 1, uav);
    c->res[CS_CBUFFER] = cs_create(c, D3D12_RESOURCE_DIMENSION_BUFFER, 1 << 16, 1, 1, 0);
    for (int i = 0; i < CS_RESOURCE_COUNT; i++)
        if (!c->res[i])
        {
            printf("FAIL console-cs: synthetic resource %d\n", i);
            return FALSE;
        }
    return TRUE;
}

/* The descriptor a slot needs, written at cpu. */
static BOOL cs_write(struct console_cs *c, const char *kind, const char *dim, SIZE_T cpu)
{
    BOOL is3d = !strcmp(dim, "3D"), isarray = !strcmp(dim, "2D_ARRAY"), is1d = !strcmp(dim, "1D");

    if (!strcmp(kind, "texture"))
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D,
                                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
        ID3D12Resource *r = c->res[CS_TEX2D];
        if (is3d) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; d.Texture3D.MipLevels = 1; r = c->res[CS_TEX3D]; }
        else if (is1d) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; d.Texture1D.MipLevels = 1; r = c->res[CS_TEX1D]; }
        else if (!strcmp(dim, "CUBE")) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; d.TextureCube.MipLevels = 1; r = c->res[CS_TEXCUBE]; }
        else if (isarray) { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipLevels = 1;
                            d.Texture2DArray.ArraySize = 2; r = c->res[CS_TEX2DARRAY]; }
        else d.Texture2D.MipLevels = 1;
        XSLOT(c->dev, 18, void, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, SIZE_T)(c->dev, r, &d, cpu);
    }
    else if (!strcmp(kind, "storage"))
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_UAV_DIMENSION_TEXTURE2D };
        ID3D12Resource *r = c->res[CS_UAV2D];
        if (is3d) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D; d.Texture3D.WSize = 16; r = c->res[CS_UAV3D]; }
        else if (is1d) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; r = c->res[CS_UAV1D]; }
        else if (isarray) { d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.ArraySize = 2;
                            r = c->res[CS_UAV2DARRAY]; }
        XSLOT(c->dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, SIZE_T)(
              c->dev, r, NULL, &d, cpu);
    }
    else if (!strncmp(kind, "buf", 3) || !strncmp(kind, "ubuf", 4))
    {
        BOOL typed = kind[strlen(kind) - 1] == 'f';
        if (kind[0] == 'u')
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d = { typed ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_TYPELESS,
                    D3D12_UAV_DIMENSION_BUFFER, .Buffer = { 0, (1 << 20) / 4, 0, 0,
                    typed ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW } };
            XSLOT(c->dev, 19, void, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *,
                  SIZE_T)(c->dev, c->res[CS_BUFFER], NULL, &d, cpu);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = { typed ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_TYPELESS,
                    D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Buffer = { 0, (1 << 20) / 4,
                    0, typed ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW } };
            XSLOT(c->dev, 18, void, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, SIZE_T)(c->dev,
                  c->res[CS_BUFFER], &d, cpu);
        }
    }
    else if (!strcmp(kind, "cbuffer"))
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC d = { ID3D12Resource_GetGPUVirtualAddress(c->res[CS_CBUFFER]), 1 << 16 };
        XSLOT(c->dev, 17, void, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, SIZE_T)(c->dev, &d, cpu);
    }
    else if (!strcmp(kind, "sampler"))
    {
        D3D12_SAMPLER_DESC d = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1,
                                 D3D12_COMPARISON_FUNC_NEVER, { 0 }, 0, 16 };
        XSLOT(c->dev, 22, void, const D3D12_SAMPLER_DESC *, SIZE_T)(c->dev, &d, cpu);
    }
    else
        return FALSE;
    return TRUE;
}

static void *cs_root_signature(struct console_cs *c, UINT count)
{
    D3D12_ROOT_PARAMETER p = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, count } };
    D3D12_ROOT_SIGNATURE_DESC d = { count ? 1 : 0, &p };
    ID3D10Blob *blob = NULL;

    if (!c->rs[count] && SUCCEEDED(c->serialize(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL)))
    {
        XSLOT(c->dev, 16, HRESULT, UINT, const void *, SIZE_T, REFIID, void **)(c->dev, 0,
              ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, &c->rs[count]);
        ID3D10Blob_Release(blob);
    }
    return c->rs[count];
}

/* One plan line; returns 0 dispatched, 1 skipped (compile failed), 2 error, 3 hung. */
/* Prints why a shader's inferred tables can't be D3D12 heaps here, and returns 4 (not run). */
static int cs_unplannable(const char *name, const char *why, void *blob)
{
    printf("cs %s not run: %s\n", name, why);
    free(blob);
    return 4;
}

/* One plan line; returns 0 dispatched, 1 skipped (compile failed), 2 error, 3 hung, 4 not run (its
 * inferred tables can't be D3D12 heaps). */
static int cs_run(struct console_cs *c, char *line)
{
    static const char *const kinds[] = { "texture", "storage", "sampler", "buffer", "cbuffer" };
    static const char *const dims[] = { "-", "1D", "2D", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2D_MSAA", "2D_MSAA_ARRAY" };
    char *name = strtok(line, " "), *path = strtok(NULL, " "), *skip = strtok(NULL, " "), *r1 = strtok(NULL, " ");
    char *r2 = strtok(NULL, " "), *threads = strtok(NULL, " \r\n"), winpath[MAX_PATH], message[512], why[128];
    struct { UINT32 magic, version, size, stage, wave_size, workgroup_size[3], rsrc1, rsrc2, code_offset, code_dwords;
             UINT64 code_address; } *blob;
    static struct sa5_slot slots[4096];
    struct sa5_layout layout;
    UINT32 sgprs[32] = { 0 }, nsgprs, table_heap_slot[16];
    BOOL table_sampler[16];
    UINT next_slot[2] = { 0, 0 };
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { 0 };
    void *heaps[2] = { c->heap, c->sampler_heap }, *pso = NULL;
    SIZE_T size, code_bytes;
    BYTE *data;
    HRESULT hr;
    int status;

    if (!name || !threads)
        return 2;
    snprintf(winpath, sizeof(winpath), "%s%s", path[0] == '/' ? "Z:" : "", path);
    for (char *p = winpath; *p; p++)
        if (*p == '/') *p = '\\';
    if (!(data = read_file(winpath, &size)) || size <= (SIZE_T)atoi(skip))
    {
        printf("cs %s error: can't read %s\n", name, winpath);
        free(data);
        return 2;
    }
    code_bytes = (size - atoi(skip)) & ~(SIZE_T)3;
    blob = calloc(1, sizeof(*blob) + code_bytes);
    *blob = (typeof(*blob)){ 0x44524353, 1, sizeof(*blob) + code_bytes, 0x20, 32, { 1, 1, 1 },
                             strtoul(r1, NULL, 0), strtoul(r2, NULL, 0), sizeof(*blob), code_bytes / 4, 0x10000000000ull };
    sscanf(threads, "%u,%u,%u", &blob->workgroup_size[0], &blob->workgroup_size[1], &blob->workgroup_size[2]);
    memcpy(blob + 1, data + atoi(skip), code_bytes);
    free(data);
    nsgprs = (blob->rsrc2 >> 1) & 0x1f;

    /* The descriptor tables the code reads (sa5_analyse), each placed in its own run of slots in
     * the heap of its kind; its SGPR holds that run's title address. */
    if (cs_analyse((const UINT32 *)(blob + 1), blob->code_dwords, nsgprs, &layout, slots, ARRAYSIZE(slots), message,
                   sizeof(message)))
    {
        snprintf(why, sizeof(why), "analysis failed: %.100s", message);
        return cs_unplannable(name, why, blob);
    }
    blob->wave_size = layout.wave;
    if (!layout.table_count)
        return cs_unplannable(name, "no descriptor table", blob);
    for (UINT t = 0; t < layout.table_count; t++)
    {
        UINT samplers = 0, others = 0, span = 1, inc;
        for (UINT i = 0; i < layout.slot_count; i++)
            if (slots[i].table == t)
                slots[i].kind == SA5_SLOT_SAMPLER ? samplers++ : others++;
        if (samplers && others)
        {
            snprintf(why, sizeof(why), "samplers and resources share table %u", t);
            return cs_unplannable(name, why, blob);
        }
        table_sampler[t] = samplers != 0;
        inc = table_sampler[t] ? c->sampler_inc : c->inc;
        for (UINT i = 0; i < layout.slot_count; i++)
            if (slots[i].table == t)
            {
                if ((slots[i].dword * 4) % inc)
                {
                    /* Console S#s are 16 bytes apart; a driver's sampler slots may be wider. */
                    snprintf(why, sizeof(why), "%s at table %u dword %u is not on a %u-byte slot boundary",
                             kinds[slots[i].kind], t, slots[i].dword, inc);
                    return cs_unplannable(name, why, blob);
                }
                span = max(span, slots[i].dword * 4 / inc + 1);
            }
        table_heap_slot[t] = next_slot[table_sampler[t]];
        if (layout.table_sgprs[t] < 32)
            sgprs[layout.table_sgprs[t]] = (UINT32)((table_sampler[t] ? c->sampler_gpu : c->gpu)
                    + (UINT64)next_slot[table_sampler[t]] * inc);
        next_slot[table_sampler[t]] += span;
    }
    if (next_slot[0] > 4 * 512 || next_slot[1] > 2048)
    {
        snprintf(why, sizeof(why), "tables need %u resource and %u sampler slots", next_slot[0], next_slot[1]);
        return cs_unplannable(name, why, blob);
    }
    for (UINT i = 0; i < layout.slot_count; i++)
    {
        const struct sa5_slot *sl = &slots[i];
        BOOL smp = table_sampler[sl->table];
        UINT inc = smp ? c->sampler_inc : c->inc;
        char kind[8];
        if (sl->kind == SA5_SLOT_BUFFER)
            snprintf(kind, sizeof(kind), "%sbuf%s", sl->stored ? "u" : "", sl->formatted ? "f" : "");
        else
            snprintf(kind, sizeof(kind), "%s", kinds[sl->kind]);
        cs_write(c, kind, dims[sl->dim <= 8 ? sl->dim : 0], (smp ? c->sampler_cpu : c->cpu)
                 + (SIZE_T)table_heap_slot[sl->table] * inc + (SIZE_T)sl->dword * 4);
    }

    cd.pRootSignature = cs_root_signature(c, nsgprs);
    cd.CS = (D3D12_SHADER_BYTECODE){ blob, blob->size };
    hr = XSLOT(c->dev, 11, HRESULT, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **)(c->dev, &cd,
            &IID_ID3D12PipelineState, &pso);
    free(blob);
    if (FAILED(hr))
    {
        printf("cs %s error: PSO creation failed %#lx\n", name, hr);
        return 2;
    }
    ID3D12CommandAllocator_Reset(c->alloc);
    XSLOT(c->list, 10, HRESULT, ID3D12CommandAllocator *, void *)(c->list, c->alloc, NULL);
    XSLOT(c->list, 32, void, UINT, void *const *)(c->list, 2, heaps);
    XSLOT(c->list, 33, void, void *)(c->list, cd.pRootSignature);
    XSLOT(c->list, 25, void, void *)(c->list, pso);
    if (nsgprs)
        XSLOT(c->list, 39, void, UINT, UINT, const void *, UINT)(c->list, 0, nsgprs, sgprs, 0);
    XSLOT(c->list, 14, void, UINT, UINT, UINT)(c->list, 1, 1, 1);
    XSLOT(c->list, 9, HRESULT)(c->list);
    XSLOT(c->queue, 10, void, UINT, void *const *)(c->queue, 1, &c->list);
    XSLOT(c->queue, 14, HRESULT, ID3D12Fence *, UINT64)(c->queue, c->fence, ++c->fence_value);
    {
        /* REBOX_CONSOLE_CS_TIMEOUT: seconds to wait for the GPU (default 300: llvmpipe takes
         * about a minute over the largest shaders). */
        const char *t = getenv("REBOX_CONSOLE_CS_TIMEOUT");
        int limit = (t ? atoi(t) : 300) * 100;
        for (int i = 0; i < limit && ID3D12Fence_GetCompletedValue(c->fence) < c->fence_value; i++)
            Sleep(10);
    }
    hr = c->last_result(message, sizeof(message));
    if (ID3D12Fence_GetCompletedValue(c->fence) < c->fence_value)
        status = 3;
    else
        status = SUCCEEDED(hr) ? 0 : 1;
    {
        /* AnyPS5 fault records (unmapped addresses, failed speculation) this dispatch left. */
        UINT faults = c->faults ? c->faults() : 0;
        printf("cs %s %s: %s%s\n", name, status == 0 ? "dispatched" : status == 1 ? "skipped" : "hung", message,
               faults > c->faults_seen ? " [AnyPS5 reported a fault]" : "");
        c->faults_seen = faults;
    }
    fflush(stdout);
    XSLOT(pso, 2, ULONG)(pso);
    return status;
}

static int console_cs(const char *plan, int first, int count)
{
    struct console_cs c = { 0 };
    int totals[5] = { 0 }, n = 0, index = 0;
    char line[16384];
    FILE *f = fopen(plan, "r");

    if (!f || !cs_setup(&c))
    {
        printf("FAIL console-cs: no plan, device, rebox_anyps5.dll or heaps\n");
        return 1;
    }
    while (fgets(line, sizeof(line), f) && (count <= 0 || n < count))
    {
        int status;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (index++ < first)
            continue;
        {
            /* REBOX_CONSOLE_CS_SKIP=NAME,NAME...: shaders known to hang (synthetic inputs can
             * make a loop run forever, and a hung queue stops the sweep). */
            const char *skip = getenv("REBOX_CONSOLE_CS_SKIP");
            char name[64];
            size_t len = strcspn(line, " ");
            snprintf(name, sizeof(name), "%.*s", (int)min(len, sizeof(name) - 1), line);
            if (skip && strstr(skip, name))
            {
                printf("cs %s not run: REBOX_CONSOLE_CS_SKIP\n", name);
                continue;
            }
        }
        n++;
        status = cs_run(&c, line);
        totals[status]++;
        if (status == 3)
        {
            printf("FAIL console-cs: the GPU did not finish; stopping\n");
            break;
        }
    }
    fclose(f);
    printf("%s console-cs: %d of %d game compute shaders dispatched, %d skipped by the compile, %d not run "
           "(tables not expressible as D3D12 heaps), %d errors, %d hung\n", totals[2] || totals[3] ? "FAIL" : "ok  ",
           totals[0], n, totals[1], totals[4], totals[2], totals[3]);
    return totals[2] || totals[3];
}

/* smoke.exe --fssm-check DIR: turns every compute container (*_CS_*.dxbc) in DIR into a deferred
 * console shader and prints one "fssm NAME rsrc1 rsrc2 x,y,z dwords" line each, for comparing
 * with the PS09 cache (game data, never in the repo). */
static int fssm_check(const char *dir)
{
    HMODULE m = LoadLibraryA("d3d12_xs.dll");
    void *(WINAPI *convert)(const void *, SIZE_T, SIZE_T *) = m ? (void *)GetProcAddress(m, "ReboxConsoleFromContainer") : NULL;
    void (WINAPI *release)(void *) = m ? (void *)GetProcAddress(m, "ReboxConsoleFree") : NULL;
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int ok = 0, failed = 0;
    HANDLE find;

    if (!convert || !release)
        return 1;
    snprintf(pattern, sizeof(pattern), "%s\\*_CS_*.dxbc", dir);
    if ((find = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return 1;
    do
    {
        SIZE_T size, blob_size;
        BYTE *data;
        const UINT32 *h;

        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        if (!(data = read_file(path, &size)))
            continue;
        if ((h = convert(data, size, &blob_size)))
        {
            printf("fssm %s %#x %#x %u,%u,%u %u\n", fd.cFileName, h[8], h[9], h[5], h[6], h[7], h[11]);
            release((void *)h);
            ok++;
        }
        else
        {
            printf("fssm %s failed\n", fd.cFileName);
            failed++;
        }
        free(data);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    printf("%s fssm-check: %d of %d compute containers converted\n", failed ? "FAIL" : "ok  ", ok, ok + failed);
    return failed != 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--fssm-check"))
        return fssm_check(argv[2]);
    if (argc > 2 && !strcmp(argv[1], "--pso-check"))
        return pso_check(argv[2]);
    if (argc > 2 && !strcmp(argv[1], "--console-cs"))
        return console_cs(argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0);
    test_xmem();
    if (argc > 1)
        test_d3d12(argv[1], !(argc > 2 && !strcmp(argv[2], "nocache")));
    test_xbox();
    test_xaudio2();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
