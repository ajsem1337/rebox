/* tests/run-bcpack.sh: one BCPack request through XCRDAPI.dll (DirectStorage, memory source),
 * decoded by the helper; compares the result with the expected blocks.
 *   bcpack_probe.exe CODEC CHUNK EXPECTED */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define SLOT(obj, n, ret, ...) ((ret (WINAPI *)(void *, ##__VA_ARGS__))((*(void ***)(obj))[n]))

static BYTE *load(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    BYTE *data;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(*size ? *size : 1);
    fread(data, 1, *size, f);
    fclose(f);
    return data;
}

int main(int argc, char **argv)
{
    struct { UINT64 source_type; UINT16 capacity; INT8 priority; const char *name; } desc = { 1, 128 };
    struct { UINT64 options; void *dst; UINT64 page; UINT32 dst_size, reserved; void *src; UINT64 offset;
             UINT32 src_size, intermediate; UINT64 tag; const char *name; } req = { 0 };
    HRESULT (WINAPI *get_factory)(REFIID, void **);
    HMODULE m = LoadLibraryA("XCRDAPI.dll");
    void *factory, *queue, *status;
    long raw_size, expected_size;
    BYTE *raw, *expected, *out;
    HRESULT hr;

    if (argc != 4 || !m) { printf("FAIL usage or no XCRDAPI.dll\n"); return 1; }
    raw = load(argv[2], &raw_size);
    expected = load(argv[3], &expected_size);
    if (!raw || !expected) { printf("FAIL can't read %s or %s\n", argv[2], argv[3]); return 1; }
    out = calloc(1, expected_size);
    get_factory = (void *)GetProcAddress(m, "DStorageGetFactory");
    get_factory(&IID_IUnknown, &factory);
    SLOT(factory, 3, HRESULT, const void *, REFIID, void **)(factory, &desc, &IID_IUnknown, &queue);
    SLOT(factory, 5, HRESULT, UINT32, const char *, REFIID, void **)(factory, 1, "probe", &IID_IUnknown, &status);
    req.options = (UINT64)atoi(argv[1]) << 1 | 1ull << 22; /* BCPack codec, memory source */
    req.dst = out;
    req.dst_size = expected_size;
    req.src = raw;
    req.src_size = raw_size;
    SLOT(queue, 3, void, const void *)(queue, &req);
    SLOT(queue, 4, void, void *, UINT32)(queue, status, 0);
    SLOT(queue, 6, void)(queue);
    for (int i = 0; i < 3000 && !SLOT(status, 3, BOOL, UINT32)(status, 0); i++)
        Sleep(10);
    hr = SLOT(status, 4, HRESULT, UINT32)(status, 0);
    if (hr == S_OK && !memcmp(out, expected, expected_size))
    {
        printf("ok   BCPack codec %s: %ld bytes -> %ld bytes, as expected\n", argv[1], raw_size, expected_size);
        return 0;
    }
    printf("FAIL BCPack codec %s: status %#lx, %s\n", argv[1], hr, hr == S_OK ? "wrong blocks" : "no blocks");
    return 1;
}
