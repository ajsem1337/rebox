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
     * it. Here: alignment 8 (256 bytes, matching its rounding), page_size 1, memory_type 5, so GPU-visible
     * memory, which XMemVirtualQuery must report as GPU-accessible. */
    {
        const ULONGLONG amd = 0xa8004000;
        vquery_fn vquery = (vquery_fn)(void *)GetProcAddress(m, "XMemVirtualQuery");
        BYTE *p = xalloc(0x1100, amd);
        xmem_info info;

        CHECK(p && !((ULONG_PTR)p & 255), "XMemAlloc AMD attributes %#llx -> %p", amd, p);
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
    test_present(dev, queue);
    test_ffx(dev, queue);
    test_present_format(dev, queue);

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

int main(int argc, char **argv)
{
    test_xmem();
    if (argc > 1)
        test_d3d12(argv[1], !(argc > 2 && !strcmp(argv[2], "nocache")));
    test_xbox();
    test_xaudio2();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures != 0;
}
