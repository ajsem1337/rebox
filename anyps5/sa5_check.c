/* sa5_check.exe: compiles compute shaders through rebox_anyps5.dll.
 *   sa5_check                                  built-in s_endpgm shader
 *   sa5_check CODE.bin SKIP RSRC1 RSRC2 X,Y,Z  a dumped shader (e.g. a PS09
 *                                              entry, SKIP = 36), compile only,
 *                                              with no guest memory
 *   sa5_check vs PLAN [FIRST [COUNT]]          the vertex shaders in PLAN (format: vertex_sweep);
 *                                              SA5_USER, SA5_MEMORY: a draw's user SGPRs and guest
 *                                              memory (see vertex_sweep)
 *   sa5_check ps PLAN [FIRST [COUNT]]          the pixel shaders in PLAN (format: pixel_sweep) with
 *                                              their PSO register state, compile only, no guest
 *                                              memory; one "ps NAME ok|FAIL ..." line each
 *   sa5_check mesh                             a built-in console mesh program (one triangle),
 *                                              compiled and finalized as d3d12_xs does
 *   sa5_check ms PLAN [FIRST [COUNT]]          the mesh shaders in PLAN (format: mesh_sweep), the
 *                                              same way, no guest memory */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rebox_anyps5.h"

static int compile(const uint32_t *code, uint32_t dwords, uint32_t rsrc1, uint32_t rsrc2, const uint32_t threads[3])
{
    struct sa5_compute_request req = { 0 };
    struct sa5_result *result = NULL;
    uint32_t user_data[16] = { 0 }, words = 0, count, wave;
    char error[512];

    req.code = code;
    req.code_dwords = dwords;
    req.code_address = 0x10000000;
    req.user_data = user_data;
    req.user_data_count = (rsrc2 >> 1) & 0x1f;
    memcpy(req.threads, threads, sizeof(req.threads));
    req.rsrc1 = rsrc1;
    req.rsrc2 = rsrc2;
    wave = 32;
    req.wave = wave;
    if (sa5_compile_compute(&req, &result, error, sizeof(error)))
    {
        printf("FAIL compile: %s\n", error);
        return 1;
    }
    sa5_result_spirv(result, &words);
    count = sa5_result_binding_count(result);
    printf("ok   compiled: %u SPIR-V words, %u bindings, subgroup %u\n", words, count,
           sa5_result_subgroup_size(result));
    for (uint32_t i = 0; i < count; i++)
    {
        struct sa5_binding b;
        sa5_result_binding(result, i, &b);
        printf("     binding kind %u set %u binding %u count %u words %u\n", b.kind, b.set, b.binding, b.count,
               b.word_count);
    }
    sa5_result_free(result);
    return 0;
}

static uint8_t *read_all(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(*size);
    if (data && fread(data, 1, *size, f) != (size_t)*size)
    {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static int pixel_sweep(const char *plan, int first, int count)
{
    FILE *f = fopen(plan, "r");
    char line[4096];
    int index = 0, ok = 0, failed = 0, unreadable = 0, valid_count = 0;
    static struct sa5_slot slots[1];

    if (!f)
    {
        printf("FAIL open %s\n", plan);
        return 1;
    }
    while (fgets(line, sizeof(line), f) && (count <= 0 || ok + failed + unreadable < count))
    {
        char name[64], path[1024], winpath[1100];
        unsigned skip, rsrc1, rsrc2, ena, ninterp, col, depth;
        char cntl[1024];
        struct sa5_pixel_request req = { 0 };
        struct sa5_result *result = NULL;
        struct sa5_layout layout;
        uint32_t user_data[32] = { 0 }, words = 0;
        char error[600], ignored[64];
        uint8_t *data;
        long size;

        if (line[0] == '#' || sscanf(line, "%63s %1023s %u %x %x %x %u %x %u %1023s", name, path, &skip, &rsrc1, &rsrc2,
                                     &ena, &ninterp, &col, &depth, cntl) != 10)
            continue;
        if (index++ < first)
            continue;
        snprintf(winpath, sizeof(winpath), "%s%s", path[0] == '/' ? "Z:" : "", path);
        for (char *p = winpath; *p; p++)
            if (*p == '/') *p = '\\';
        if (!(data = read_all(winpath, &size)) || size <= (long)skip)
        {
            printf("ps %s FAIL unreadable\n", name);
            unreadable++;
            free(data);
            continue;
        }
        req.code = (const uint32_t *)(data + skip);
        req.code_dwords = (uint32_t)((size - skip) / 4);
        req.code_address = 0x10000000;
        req.user_data = user_data;
        req.user_data_count = (rsrc2 >> 1) & 0x1f;
        req.rsrc1 = rsrc1;
        req.rsrc2 = rsrc2;
        req.input_ena = ena;
        req.interpolator_count = ninterp;
        /* SPI_PS_INPUT_CNTL_n from the PSO, comma-separated hex ("-" for none). */
        {
            char *p = cntl;
            for (unsigned i = 0; i < 32 && *p && *p != '-'; i++)
            {
                req.interpolator_settings[i] = (uint32_t)strtoul(p, &p, 16);
                if (*p == ',')
                    p++;
            }
        }
        req.col_format = col;
        req.depth_export = depth;
        req.wave = !sa5_analyse(req.code, req.code_dwords, req.user_data_count, &layout, slots, 0, ignored,
                                sizeof(ignored)) ? layout.wave : 64;
        if (sa5_compile_pixel(&req, &result, error, sizeof(error)))
        {
            for (char *p = error; *p; p++)
                if (*p == '\n') *p = ' ';
            printf("ps %s FAIL %.400s\n", name, error);
            failed++;
        }
        else
        {
            const uint32_t *spirv = sa5_result_spirv(result, &words);
            int valid = !sa5_validate(spirv, words, error, sizeof(error));
            for (char *p = error; *p; p++)
                if (*p == '\n') *p = ' ';
            printf("ps %s ok %u words, wave%u, %u bindings, %s%.300s\n", name, words, req.wave,
                   sa5_result_binding_count(result), valid ? "valid" : "INVALID: ", valid ? "" : error);
            valid_count += valid;
            sa5_result_free(result);
            ok++;
        }
        fflush(stdout);
        free(data);
    }
    fclose(f);
    printf("%s ps-check: %d of %d pixel shaders compile with their PSO register state, %d of those pass spirv-val "
           "(%d unreadable)\n", failed || unreadable ? "info" : "ok  ", ok, ok + failed + unreadable, valid_count,
           unreadable);
    return 0;
}

/* Vertex shaders: plan lines NAME CODE SKIP RSRC1 USER_SGPRS INPUTS, one shader per line. Each
 * input r gets a synthetic RGBA32F vertex buffer (stride 16) and v[4+4r..7+4r]. */
static uint32_t memory[64], memory_words;
static uint64_t memory_address;
static struct sa5_region region;

static int vertex_sweep(const char *plan, int first, int count)
{
    FILE *f = fopen(plan, "r");
    char line[4096];
    int index = 0, ok = 0, failed = 0, valid_count = 0;

    if (!f)
    {
        printf("FAIL open %s\n", plan);
        return 1;
    }
    while (fgets(line, sizeof(line), f) && (count <= 0 || ok + failed < count))
    {
        char name[64], path[1024], winpath[1100], error[600];
        unsigned skip, rsrc1, user, inputs;
        struct sa5_vertex_request req = { 0 };
        struct sa5_result *result = NULL;
        uint32_t user_data[32] = { 0 }, words = 0;
        uint8_t *data;
        long size;

        if (line[0] == '#' || sscanf(line, "%63s %1023s %u %x %u %u", name, path, &skip, &rsrc1, &user, &inputs) != 6)
            continue;
        if (index++ < first)
            continue;
        snprintf(winpath, sizeof(winpath), "%s%s", path[0] == '/' ? "Z:" : "", path);
        for (char *p = winpath; *p; p++)
            if (*p == '/') *p = '\\';
        if (!(data = read_all(winpath, &size)) || size <= (long)skip)
        {
            printf("vs %s FAIL unreadable\n", name);
            failed++;
            free(data);
            continue;
        }
        req.code = (const uint32_t *)(data + skip);
        req.code_dwords = (uint32_t)((size - skip) / 4);
        req.code_address = 0x10000000;
        req.wave = 64;
        req.user_data = user_data;
        req.user_data_count = user > 32 ? 32 : user;
        /* s[4:5]: the fetch shader's address, outside the program (AnyPS5 then takes the call for
         * the fetch it stands in for). */
        user_data[4] = 0x30000000u;
        req.rsrc1 = rsrc1;
        req.input_count = inputs > 32 ? 32 : inputs;
        for (unsigned r = 0; r < req.input_count; r++)
        {
            req.inputs[r].vsharp[0] = 0x20000000u + r * 0x100000u;
            req.inputs[r].vsharp[1] = 16u << 16;
            req.inputs[r].vsharp[2] = 1024;
            req.inputs[r].vsharp[3] = (77u << 12) | 0xfacu;
            req.inputs[r].register_start = 4 + 4 * r;
            req.inputs[r].register_count = 4;
        }
        req.push_size = getenv("SA5_PUSH_SIZE") ? (uint32_t)atoi(getenv("SA5_PUSH_SIZE")) : 0;
        /* SA5_USER=v0,v1,... (hex, from s0; s[4:5] stays the fetch address) and
         * SA5_MEMORY=ADDRESS:w0,w1,... (hex): a draw's user SGPRs and guest memory, as d3d12_xs logs
         * them, to follow the descriptors a shader resolves. */
        if (getenv("SA5_USER"))
        {
            const char *p = getenv("SA5_USER");
            for (unsigned i = 0; i < 32 && *p; i++)
            {
                char *end;
                uint32_t v = (uint32_t)strtoul(p, &end, 16);
                if (i != 4 && i != 5)
                    user_data[i] = v;
                p = *end == ',' ? end + 1 : end;
                if (end == p && *end != ',')
                    break;
            }
        }
        if (getenv("SA5_MEMORY"))
        {
            const char *p = getenv("SA5_MEMORY");
            char *end;
            memory_address = strtoull(p, &end, 16);
            memory_words = 0;
            for (p = *end == ':' ? end + 1 : end; *p && memory_words < 64; p = *end == ',' ? end + 1 : end)
            {
                memory[memory_words++] = (uint32_t)strtoul(p, &end, 16);
                if (end == p)
                    break;
            }
            region = (struct sa5_region){ memory_address, memory, memory_words * 4u };
            req.regions = &region;
            req.region_count = 1;
        }
        if (sa5_compile_vertex(&req, &result, error, sizeof(error)))
        {
            for (char *p = error; *p; p++)
                if (*p == '\n') *p = ' ';
            printf("vs %s FAIL %.400s\n", name, error);
            failed++;
        }
        else
        {
            const uint32_t *spirv = sa5_result_spirv(result, &words);
            int valid = !sa5_validate(spirv, words, error, sizeof(error));
            for (char *p = error; *p; p++)
                if (*p == '\n') *p = ' ';
            printf("vs %s ok %u words, %u vertex inputs, %u bindings, %s%.300s\n", name, words,
                   sa5_result_vertex_input_count(result), sa5_result_binding_count(result),
                   valid ? "valid" : "INVALID: ", valid ? "" : error);
            for (uint32_t i = 0; i < sa5_result_binding_count(result) && getenv("SA5_MEMORY"); i++)
            {
                struct sa5_binding b;
                sa5_result_binding(result, i, &b);
                printf("     binding role %u kind %u count %u words", b.role, b.kind, b.count);
                for (uint32_t w = 0; w < b.word_count && w < 8; w++)
                    printf(" %08x", b.words[w]);
                printf("\n");
            }
            /* SA5_PUSH_SIZE set: finalize as a graphics stage does, host buffers' addresses pushed
             * after the stage's push data (the guest buffer table) or baked (the rest). */
            if (getenv("SA5_PUSH_SIZE"))
            {
                struct sa5_host_buffer hosts[8];
                uint32_t host_count = 0, *out = NULL, out_count = 0, pushed = 0;
                for (uint32_t i = 0; i < sa5_result_binding_count(result) && host_count < 8; i++)
                {
                    struct sa5_binding b;
                    sa5_result_binding(result, i, &b);
                    if (b.role == 0 && b.kind == 1)
                        hosts[host_count++] = (struct sa5_host_buffer){ b.set, b.binding, pushed++, 0, 1,
                                req.push_size, 0 };
                    else if (b.role > 2)
                        hosts[host_count++] = (struct sa5_host_buffer){ b.set, b.binding, host_count, b.word_count, 0,
                                0, 0x123450000ull };
                }
                if (sa5_finalize(result, NULL, 0, hosts, host_count, &out, &out_count, error, sizeof(error)))
                {
                    for (char *p = error; *p; p++)
                        if (*p == '\n') *p = ' ';
                    printf("vs %s finalize FAIL (%u host buffers, %u pushed): %.300s\n", name, host_count, pushed, error);
                }
                else
                {
                    printf("vs %s finalize ok (%u host buffers, %u pushed)\n", name, host_count, pushed);
                    sa5_free_words(out);
                }
            }
            valid_count += valid;
            sa5_result_free(result);
            ok++;
        }
        fflush(stdout);
        free(data);
    }
    fclose(f);
    printf("%s vs-check: %d of %d vertex shaders compile, %d of those pass spirv-val\n",
           failed ? "info" : "ok  ", ok, ok + failed, valid_count);
    return 0;
}

/* Mesh shaders: compiled with d3d12_xs's settings (wave and limits from the FSSM, 32 user SGPRs,
 * push data 0-95), then finalized with a draw block at byte 96: the guest buffer table pushed, the
 * other host buffers baked. Returns 0 and a "ok ..." detail, or -1 and the error. */
static int mesh_compile(const struct sa5_mesh_request *req, char *detail, size_t size)
{
    struct sa5_result *result = NULL;
    struct sa5_host_buffer hosts[8];
    uint32_t host_count = 0, pushed = 0, words = 0, *out = NULL, out_count = 0;
    char error[600];
    const uint32_t *spirv;

    if (sa5_compile_mesh(req, &result, error, sizeof(error)))
    {
        snprintf(detail, size, "FAIL %.400s", error);
        return -1;
    }
    spirv = sa5_result_spirv(result, &words);
    if (sa5_validate(spirv, words, error, sizeof(error)))
    {
        snprintf(detail, size, "INVALID %u words: %.300s", words, error);
        sa5_result_free(result);
        return -1;
    }
    for (uint32_t i = 0; i < sa5_result_binding_count(result) && host_count < 8; i++)
    {
        struct sa5_binding b;
        sa5_result_binding(result, i, &b);
        if (b.role == 0 && b.kind == 1)
            hosts[host_count++] = (struct sa5_host_buffer){ b.set, b.binding, pushed++, 0, 1, 0, 0 };
        else if (b.role > 2)
            hosts[host_count++] = (struct sa5_host_buffer){ b.set, b.binding, 0, b.word_count, 0, 0, 0x123450000ull };
    }
    if (sa5_finalize_draw(result, NULL, 0, hosts, host_count, 96, &out, &out_count, error, sizeof(error)))
    {
        snprintf(detail, size, "finalize FAIL (%u bindings): %.300s", sa5_result_binding_count(result), error);
        sa5_result_free(result);
        return -1;
    }
    snprintf(detail, size, "ok %u words, wave%u, %u bindings, subgroup %u; finalized %u words", words, req->wave,
             sa5_result_binding_count(result), sa5_result_subgroup_size(result), out_count);
    sa5_free_words(out);
    sa5_result_free(result);
    return 0;
}

static int mesh_builtin(void)
{
    /* Wave64: three vertices and one triangle (m0 = vertices | primitives << 12), lane n at
     * (n, 0, 0, 1), the triangle from lane 0. */
    static const uint32_t code[] = {
        0xd7650000, 0x000100c1,             /* v_mbcnt_lo_u32_b32 v0, -1, 0 */
        0xd7660000, 0x000200c1,             /* v_mbcnt_hi_u32_b32 v0, -1, v0 */
        0xb07c1003,                         /* s_movk_i32 m0, 0x1003 */
        0xbf900009,                         /* s_sendmsg GS_ALLOC_REQ */
        0x7d880083,                         /* v_cmp_gt_u32 vcc, 3, v0 */
        0xbefe046a,                         /* s_mov_b64 exec, vcc */
        0x7e020d00,                         /* v_cvt_f32_u32 v1, v0 */
        0x7e040280,                         /* v_mov_b32 v2, 0 */
        0x7e0602f2,                         /* v_mov_b32 v3, 1.0 */
        0xf80008cf, 0x03020201,             /* exp pos0 v1, v2, v2, v3 done */
        0xf800020f, 0x03030303,             /* exp param0 v3, v3, v3, v3 */
        0x7d840080,                         /* v_cmp_eq_u32 vcc, 0, v0 */
        0xbefe046a,                         /* s_mov_b64 exec, vcc */
        0x7e0802ff, 0x00200400,             /* v_mov_b32 v4, 0 | 1 << 10 | 2 << 20 */
        0xf8000941, 0x00000004,             /* exp prim v4 done */
        0xbf810000,                         /* s_endpgm */
    };
    uint32_t user[32] = { 0 };
    struct sa5_mesh_request req = { 0 };
    char detail[700];
    int failed;

    req.code = code;
    req.code_dwords = sizeof(code) / 4;
    req.code_address = 0x10000000;
    req.wave = 64;
    req.user_data = user;
    req.user_data_count = 32;
    req.rsrc1 = 0x40000000u | 1u << 21 | 0xc0u << 12;
    req.threads = 128;
    req.max_vertices = 128;
    req.max_primitives = 128;
    req.primitive_vertices = 3;
    req.push_size = 96;
    req.use_cache = 1;
    failed = mesh_compile(&req, detail, sizeof(detail));
    printf("%s mesh: %s\n", failed ? "FAIL" : "ok  ", detail);
    return failed;
}

/* Plan lines: NAME CODE SKIP DWORDS WAVE THREADS MAX_VERTICES MAX_PRIMITIVES PRIMITIVE_VERTICES LDS
 * CLIP_CNTL RSRC1 (hex for the last two), the code at byte SKIP of the file CODE. */
static int mesh_sweep(const char *plan, int first, int count)
{
    FILE *f = fopen(plan, "r");
    char line[4096];
    int index = 0, ok = 0, failed = 0;

    if (!f)
    {
        printf("FAIL open %s\n", plan);
        return 1;
    }
    while (fgets(line, sizeof(line), f) && (count <= 0 || ok + failed < count))
    {
        char name[64], path[1024], winpath[1100], detail[700];
        unsigned skip, dwords, wave, threads, maxv, maxp, verts, lds, clip, rsrc1;
        struct sa5_mesh_request req = { 0 };
        uint32_t user[32] = { 0 };
        uint8_t *data;
        long size;

        if (line[0] == '#' || sscanf(line, "%63s %1023s %u %u %u %u %u %u %u %u %x %x", name, path, &skip, &dwords, &wave,
                                     &threads, &maxv, &maxp, &verts, &lds, &clip, &rsrc1) != 12)
            continue;
        if (index++ < first)
            continue;
        snprintf(winpath, sizeof(winpath), "%s%s", path[0] == '/' ? "Z:" : "", path);
        for (char *p = winpath; *p; p++)
            if (*p == '/') *p = '\\';
        if (!(data = read_all(winpath, &size)) || size < (long)(skip + 4ull * dwords))
        {
            printf("ms %s FAIL unreadable\n", name);
            failed++;
            free(data);
            continue;
        }
        req.code = (const uint32_t *)(data + skip);
        req.code_dwords = dwords;
        req.code_address = 0x10000000;
        req.wave = wave;
        req.user_data = user;
        req.user_data_count = 32;
        req.rsrc1 = rsrc1;
        req.threads = threads;
        req.max_vertices = maxv;
        req.max_primitives = maxp;
        req.primitive_vertices = verts;
        req.lds_bytes = lds;
        req.clip_cntl = clip;
        req.push_size = 96;
        req.use_cache = 1;
        if (mesh_compile(&req, detail, sizeof(detail)))
            failed++;
        else
            ok++;
        for (char *p = detail; *p; p++)
            if (*p == '\n') *p = ' ';
        printf("ms %s %s\n", name, detail);
        fflush(stdout);
        free(data);
    }
    fclose(f);
    printf("%s ms-check: %d of %d mesh shaders compile, validate and finalize\n", failed ? "info" : "ok  ", ok,
           ok + failed);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "mesh"))
        return mesh_builtin();
    if (argc > 2 && !strcmp(argv[1], "ms"))
        return mesh_sweep(argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0);
    if (argc > 2 && !strcmp(argv[1], "ps"))
        return pixel_sweep(argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0);
    if (argc > 2 && !strcmp(argv[1], "vs"))
        return vertex_sweep(argv[2], argc > 3 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0);

    static const uint32_t endpgm[] = { 0xbf810000u };
    uint32_t threads[3] = { 1, 1, 1 };

    if (argc < 6)
        return compile(endpgm, 1, 0, 0, threads);
    {
        FILE *f = fopen(argv[1], "rb");
        long size, skip = strtol(argv[2], NULL, 0);
        uint8_t *data;

        if (!f)
        {
            printf("FAIL open %s\n", argv[1]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = malloc(size);
        fread(data, 1, size, f);
        fclose(f);
        sscanf(argv[5], "%u,%u,%u", &threads[0], &threads[1], &threads[2]);
        return compile((const uint32_t *)(data + skip), (uint32_t)((size - skip) / 4),
                       (uint32_t)strtoul(argv[3], NULL, 0), (uint32_t)strtoul(argv[4], NULL, 0), threads);
    }
}
