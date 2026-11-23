/* Missing functions in API sets Wine hosts.
 *
 * A console title imports Xbox-only functions through api-ms-win-* sets. When Wine's apiset
 * schema hosts the set but its host DLL lacks the name, Wine binds a stub that aborts the process
 * when called, and a DLL of the set's name can't take its place: the schema wins. scan_imports
 * only reports these. So at load, after the title's imports are bound, every IAT entry pointing at
 * one of Wine's stubs is pointed at our own, which logs the name once and returns E_NOTIMPL, like
 * the generated stubs. DLLs loaded later (LoadLibrary of the package's Bink, BVH builder or
 * FidelityFX DLLs) are fixed up from a loader notification, before their DllMain runs.
 *
 * Some functions Wine has but only as failing stubs, where a console title stops if they fail, are
 * replaced the same way, by the address Wine's export resolves to (replacements[]). */

#include <windows.h>
#include <psapi.h>
#include <winternl.h>
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

/* Process lifetime and constrained-mode notifications (api-ms-win-core-psm-appnotify): Wine's
 * return ERROR_CALL_NOT_IMPLEMENTED, and a GDK title then stops its game thread and exits
 * with 1 a second after creating its window. Registered here, never called: a title isn't
 * suspended, resumed or constrained under Wine. */
static int app_state_registration;

static ULONG WINAPI app_state_register(void *routine, void *context, void **registration)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("RegisterAppStateChangeNotification: registered (the title is never suspended here)");
    if (registration)
        *registration = &app_state_registration;
    return ERROR_SUCCESS;
}

static void WINAPI app_state_unregister(void *registration)
{
}

static ULONG WINAPI app_constrained_register(void *routine, void *context, void **registration)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("RegisterAppConstrainedChangeNotification: registered (the title is never constrained here)");
    if (registration)
        *registration = &app_state_registration;
    return ERROR_SUCCESS;
}

/* The title's top-level windows are the console's screen: frameless, the client area the whole
 * window (xcl). a GDK title asks for an overlapped 1920x1080 window and sizes its back
 * buffers from the client area, which a frame cut to 1912x1046. Top-level windows with a caption or
 * sizing frame are made popups at the size asked for. REBOX_FRAMELESS=0 leaves them. */
static DWORD frameless_style(DWORD style, HWND parent)
{
    static int enabled = -1;
    if (enabled < 0)
    {
        char v[8];
        DWORD n = GetEnvironmentVariableA("REBOX_FRAMELESS", v, sizeof(v));
        enabled = !(n && n < sizeof(v) && v[0] == '0');
    }
    if (!enabled || parent || (style & WS_CHILD) || !(style & (WS_CAPTION | WS_THICKFRAME)))
        return style;
    {
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            rebox_log("frameless window: style %#lx made a popup (the console's screen)", style);
    }
    return (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
}

/* The real functions, by the pointers captured at startup: never through an import, which may be
 * one of these replacements (our own DLLs load from outside the Windows directory in the smoke
 * test, and would call themselves). */
static void *real_create_window_w, *real_create_window_a;

static HWND WINAPI create_window_w(DWORD ex, const WCHAR *cls, const WCHAR *name, DWORD style, int x, int y, int w,
        int h, HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    return ((HWND (WINAPI *)(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
            void *))real_create_window_w)(ex, cls, name, frameless_style(style, parent), x, y, w, h, parent, menu, inst,
            param);
}

static HWND WINAPI create_window_a(DWORD ex, const char *cls, const char *name, DWORD style, int x, int y, int w,
        int h, HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    return ((HWND (WINAPI *)(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
            void *))real_create_window_a)(ex, cls, name, frameless_style(style, parent), x, y, w, h, parent, menu, inst,
            param);
}

static struct
{
    const char *dll, *name;
    void *ours, *wine;
} replacements[] = {
    { "user32.dll", "CreateWindowExW", create_window_w },
    { "user32.dll", "CreateWindowExA", create_window_a },
    { "twinapi.appcore.dll", "RegisterAppStateChangeNotification", app_state_register },
    { "twinapi.appcore.dll", "UnregisterAppStateChangeNotification", app_state_unregister },
    { "twinapi.appcore.dll", "RegisterAppConstrainedChangeNotification", app_constrained_register },
    { "twinapi.appcore.dll", "UnregisterAppConstrainedChangeNotification", app_state_unregister },
};

static void *replacement(void *target)
{
    for (UINT i = 0; i < ARRAYSIZE(replacements); i++)
        if (replacements[i].wine && replacements[i].wine == target)
            return replacements[i].ours;
    return NULL;
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
    BOOL title;

    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !dir->VirtualAddress)
        return 0;
    GetModuleFileNameA(module, path, sizeof(path));
    file = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
    /* Replacements are for the title's modules only: not Wine's DLLs or ours (all under the
     * Windows directory), which would otherwise call themselves through them. */
    {
        char windir[MAX_PATH];
        UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
        title = !(n && n < sizeof(windir) && !_strnicmp(path, windir, n));
    }
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); desc->Name; desc++)
    {
        void **iat = (void **)(base + desc->FirstThunk);

        for (; *iat; iat++)
        {
            DWORD old;
            void *ours;

            if (title && (ours = replacement(*iat)))
            {
                if (VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old))
                {
                    *iat = ours;
                    VirtualProtect(iat, sizeof(*iat), old, &old);
                    fixed++;
                }
                continue;
            }
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

/* ntdll's DLL load notifications (not in mingw's headers). */
typedef struct
{
    ULONG Flags;
    const UNICODE_STRING *FullDllName;
    const UNICODE_STRING *BaseDllName;
    void *DllBase;
    ULONG SizeOfImage;
} DLL_LOADED_DATA;

#define DLL_NOTIFICATION_LOADED 1

static CRITICAL_SECTION stub_lock;

static void CALLBACK dll_loaded(ULONG reason, const DLL_LOADED_DATA *data, void *context)
{
    if (reason != DLL_NOTIFICATION_LOADED)
        return;
    EnterCriticalSection(&stub_lock);
    fix_module(data->DllBase);
    LeaveCriticalSection(&stub_lock);
}

void import_fixup_run(void)
{
    NTSTATUS (WINAPI *register_notification)(ULONG, void *, void *, void **);
    static void *cookie;

    HMODULE modules[512];
    DWORD bytes;

    InitializeCriticalSection(&stub_lock);
    for (UINT i = 0; i < ARRAYSIZE(replacements); i++)
    {
        HMODULE dll = LoadLibraryA(replacements[i].dll);
        replacements[i].wine = dll ? (void *)GetProcAddress(dll, replacements[i].name) : NULL;
        if (replacements[i].ours == (void *)create_window_w)
            real_create_window_w = replacements[i].wine;
        if (replacements[i].ours == (void *)create_window_a)
            real_create_window_a = replacements[i].wine;
    }
    register_notification = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
    if (!register_notification || register_notification(0, dll_loaded, NULL, &cookie))
        rebox_log("import fixup: no DLL load notifications; DLLs loaded later keep Wine's stubs");
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    EnterCriticalSection(&stub_lock);
    for (DWORD i = 0; i < bytes / sizeof(*modules) && i < ARRAYSIZE(modules); i++)
        fix_module(modules[i]);
    LeaveCriticalSection(&stub_lock);
}
