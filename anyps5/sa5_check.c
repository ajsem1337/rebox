/* sa5_check.exe: compiles compute shaders through rebox_anyps5.dll.
 *   sa5_check                                  built-in s_endpgm shader
 *   sa5_check CODE.bin SKIP RSRC1 RSRC2 X,Y,Z  a dumped shader (e.g. a PS09
 *                                              entry, SKIP = 36), compile only,
 *                                              with no guest memory */
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

int main(int argc, char **argv)
{
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
