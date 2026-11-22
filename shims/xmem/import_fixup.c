/* Missing functions in API sets Wine hosts.
 *
 * A console title imports Xbox-only functions through api-ms-win-* sets. When Wine's apiset
 * schema hosts the set but its host DLL lacks the name, Wine binds a stub that aborts the process
 * when called, and a DLL of the set's name can't take its place: the schema wins. scan_imports
 * only reports these. So at load, after the title's imports are bound, every IAT entry pointing at
 * one of Wine's stubs is pointed at our own, which logs the name once and returns E_NOTIMPL, like
 * the generated stubs. */

#include <windows.h>
#include <psapi.h>
#include <string.h>

#include "../common/rebox_log.h"
#include "import_fixup.h"

/* Wine's loader allocate_stub() on x86_64:
 *   48 b9 <dll>  movabs rcx, dll     48 ba <name>  movabs rdx, name
 *   4c 8b 04 24  mov r8, [rsp]       48 b8 <entry> movabs rax, entry     ff e0  jmp rax */
#pragma pack(push, 1)
struct wine_stub
{
    BYTE movq_rcx[2];
    const char *dll;
    BYTE movq_rdx[2];
    const char *name;
    BYTE movq_r8[4];
    BYTE movq_rax[2];
    void *entry;
    BYTE jmpq_rax[2];
};

struct our_stub
{
    BYTE movq_rcx[2];
    struct stub_info *info;
    BYTE movq_rax[2];
    void *handler;
    BYTE jmpq_rax[2];
};
#pragma pack(pop)

struct stub_info
{
    const char *dll, *name;
    LONG called;
};

static ULONG_PTR __attribute__((ms_abi)) unimplemented(struct stub_info *info)
{
    if (!InterlockedExchange(&info->called, 1))
        rebox_log("unimplemented %s!%s (Wine's host DLL lacks it; returning E_NOTIMPL)", info->dll, info->name);
    return E_NOTIMPL;
}

static BOOL is_wine_stub(const void *p)
{
    const struct wine_stub *s = p;
    MEMORY_BASIC_INFORMATION mbi;

    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT
            || !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
            || (const BYTE *)mbi.BaseAddress + mbi.RegionSize < (const BYTE *)p + sizeof(*s))
        return FALSE;
    return s->movq_rcx[0] == 0x48 && s->movq_rcx[1] == 0xb9 && s->movq_rdx[0] == 0x48 && s->movq_rdx[1] == 0xba
            && !memcmp(s->movq_r8, "\x4c\x8b\x04\x24", 4) && s->movq_rax[0] == 0x48 && s->movq_rax[1] == 0xb8
            && s->jmpq_rax[0] == 0xff && s->jmpq_rax[1] == 0xe0;
}

static void *make_stub(const struct wine_stub *wine)
{
    static struct our_stub *code;
    static struct stub_info *infos;
    static UINT used, capacity;
    struct our_stub *s;
    struct stub_info *info;

    if (used == capacity)
    {
        capacity = 4096 / sizeof(*code);
        used = 0;
        code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        infos = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(*infos));
        if (!code || !infos)
            return NULL;
    }
    info = &infos[used];
    info->dll = wine->dll;
    info->name = wine->name;
    s = &code[used++];
    s->movq_rcx[0] = 0x48, s->movq_rcx[1] = 0xb9;
    s->info = info;
    s->movq_rax[0] = 0x48, s->movq_rax[1] = 0xb8;
    s->handler = unimplemented;
    s->jmpq_rax[0] = 0xff, s->jmpq_rax[1] = 0xe0;
    return s;
}

static UINT fix_module(HMODULE module)
{
    BYTE *base = (BYTE *)module;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc;
    char path[MAX_PATH], *file;
    UINT fixed = 0;

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !dir->VirtualAddress)
        return 0;
    GetModuleFileNameA(module, path, sizeof(path));
    file = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; desc++)
    {
        void **iat = (void **)(base + desc->FirstThunk);

        for (; *iat; iat++)
        {
            DWORD old;
            void *ours;

            if (!is_wine_stub(*iat) || !(ours = make_stub(*iat)))
                continue;
            if (!VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old))
                continue;
            rebox_log("import fixup: %s!%s, imported by %s, has no implementation in Wine; it will return E_NOTIMPL",
                         ((struct wine_stub *)*iat)->dll, ((struct wine_stub *)*iat)->name, file);
            *iat = ours;
            VirtualProtect(iat, sizeof(*iat), old, &old);
            fixed++;
        }
    }
    return fixed;
}

void import_fixup_run(void)
{
    HMODULE modules[512];
    DWORD bytes;

    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    for (DWORD i = 0; i < bytes / sizeof(*modules) && i < ARRAYSIZE(modules); i++)
        fix_module(modules[i]);
}
