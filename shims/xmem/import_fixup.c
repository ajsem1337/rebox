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
 * replaced the same way, by the address Wine's export resolves to (replacements[]).
 *
 * The title's network is off unless REBOX_NETWORK=1: its HTTP connections (WinHTTP, which
 * libHttpClient uses too) and sockets fail as with no network, so it sends nothing out (a title's
 * crash handler uploads a minidump of the process, with the host's details, to its publisher).
 * Loopback addresses, localhost and the machine's own name still work. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
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
 * with 1 a second after creating its window. Registered here; a title is never resumed or
 * constrained under Wine, and it is suspended only when it is closed (below), so it can save. */
typedef void (CALLBACK *app_state_routine)(BOOLEAN quiesced, void *context);
static struct app_state { app_state_routine routine; void *context; } app_states[16];
static int app_state_registration;
static SRWLOCK app_state_lock = SRWLOCK_INIT;

static ULONG WINAPI app_state_register(void *routine, void *context, void **registration)
{
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("RegisterAppStateChangeNotification: registered (the title is suspended only when it's closed)");
    if (!routine)
        return ERROR_INVALID_PARAMETER;
    AcquireSRWLockExclusive(&app_state_lock);
    for (UINT i = 0; i < ARRAYSIZE(app_states); i++)
        if (!app_states[i].routine)
        {
            app_states[i].routine = routine;
            app_states[i].context = context;
            if (registration)
                *registration = &app_states[i];
            ReleaseSRWLockExclusive(&app_state_lock);
            return ERROR_SUCCESS;
        }
    ReleaseSRWLockExclusive(&app_state_lock);
    if (registration)
        *registration = &app_state_registration; /* full: registered, never called */
    return ERROR_SUCCESS;
}

static void WINAPI app_state_unregister(void *registration)
{
    AcquireSRWLockExclusive(&app_state_lock);
    for (UINT i = 0; i < ARRAYSIZE(app_states); i++)
        if (registration == &app_states[i])
            app_states[i].routine = NULL;
    ReleaseSRWLockExclusive(&app_state_lock);
}

/* For tests: register an app state routine as the title would. */
ULONG WINAPI ReboxAppStateRegister(void *routine, void *context, void **registration)
{
    return app_state_register(routine, context, registration);
}

/* Closing the title (its window closed, or Escape): on a console the title is suspended before
 * it ends, and saves then (a GDK title writes its save). Each app state routine is told the title
 * is quiescing, on a thread of its own, for up to REBOX_QUIT_TIMEOUT seconds (default 5);
 * then the process ends. REBOX_QUIT_EXIT=0 only suspends (tests); REBOX_ESCAPE_QUITS=0
 * leaves Escape to the title. */
static DWORD env_number(const char *name, DWORD fallback)
{
    char v[16];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n && n < sizeof(v) ? (DWORD)strtoul(v, NULL, 10) : fallback;
}

static DWORD WINAPI quiesce_title(void *unused)
{
    struct app_state copy[ARRAYSIZE(app_states)];
    AcquireSRWLockShared(&app_state_lock);
    memcpy(copy, app_states, sizeof(copy));
    ReleaseSRWLockShared(&app_state_lock);
    for (UINT i = 0; i < ARRAYSIZE(copy); i++)
        if (copy[i].routine)
            copy[i].routine(TRUE, copy[i].context);
    return 0;
}

static DWORD quit_timeout, quit_exit; /* read when the quit is asked for */

static DWORD WINAPI quit_title(void *why)
{
    DWORD timeout = quit_timeout;
    HANDLE t;

    rebox_log("quit: %s; suspending the title (up to %lu ms) so it can save", (const char *)why, timeout);
    if ((t = CreateThread(NULL, 0, quiesce_title, NULL, 0, NULL)))
    {
        if (WaitForSingleObject(t, timeout) == WAIT_TIMEOUT)
            rebox_log("quit: the title is still suspending after %lu ms; ending it anyway", timeout);
        CloseHandle(t);
    }
    if (!quit_exit)
    {
        rebox_log("quit: suspended; not ending the process (REBOX_QUIT_EXIT=0)");
        return 0;
    }
    rebox_log("quit: ending the process");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static void request_quit(const char *why)
{
    static LONG quitting;
    HANDLE t;
    if (InterlockedExchange(&quitting, 1))
        return;
    quit_timeout = env_number("REBOX_QUIT_TIMEOUT", 5) * 1000;
    quit_exit = env_number("REBOX_QUIT_EXIT", 1);
    if ((t = CreateThread(NULL, 0, quit_title, (void *)why, 0, NULL)))
        CloseHandle(t);
}

/* The title's top-level windows are subclassed for this: their own window procedures, written for
 * the console, ignore closing. */
static const WCHAR old_proc_prop[] = L"rebox.wndproc";

static LRESULT CALLBACK quit_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(hwnd, old_proc_prop);
    if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE))
    {
        request_quit("the window was closed");
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && env_number("REBOX_ESCAPE_QUITS", 1))
    {
        request_quit("Escape was pressed");
        return 0;
    }
    if (!old)
        return IsWindowUnicode(hwnd) ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);
    return IsWindowUnicode(hwnd) ? CallWindowProcW(old, hwnd, msg, wp, lp) : CallWindowProcA(old, hwnd, msg, wp, lp);
}

static HWND subclass_for_quit(HWND hwnd, HWND parent, DWORD style)
{
    LONG_PTR old;
    if (!hwnd || parent || (style & WS_CHILD))
        return hwnd;
    old = IsWindowUnicode(hwnd) ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)quit_wndproc)
                                : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)quit_wndproc);
    SetPropW(hwnd, old_proc_prop, (HANDLE)old);
    return hwnd;
}

/* For tests (the smoke test has no display, so no top-level window): what a title's top-level
 * window gets. */
HWND WINAPI ReboxQuitSubclass(HWND hwnd)
{
    return subclass_for_quit(hwnd, NULL, 0);
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
 * window (xcl). A GDK title asks for an overlapped 1920x1080 window and sizes its back
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
    return subclass_for_quit(((HWND (WINAPI *)(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HWND,
            HMENU, HINSTANCE, void *))real_create_window_w)(ex, cls, name, frameless_style(style, parent), x, y, w, h,
            parent, menu, inst, param), parent, style);
}

static HWND WINAPI create_window_a(DWORD ex, const char *cls, const char *name, DWORD style, int x, int y, int w,
        int h, HWND parent, HMENU menu, HINSTANCE inst, void *param)
{
    return subclass_for_quit(((HWND (WINAPI *)(DWORD, const char *, const char *, DWORD, int, int, int, int, HWND,
            HMENU, HINSTANCE, void *))real_create_window_a)(ex, cls, name, frameless_style(style, parent), x, y, w, h,
            parent, menu, inst, param), parent, style);
}

/* Audio state monitors (windows.media.mediacontrol): Wine's are stubs that abort the process, and
 * A UE5 sample's GameChat2.dll asks for one at startup. A console title's audio is never
 * ducked or muted by the system here, so the monitor says full volume and never calls back.
 * IAudioStateMonitor: IUnknown, RegisterCallback, UnregisterCallback, GetSoundLevel. */
static HRESULT WINAPI monitor_query(void *self, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = self;
    return S_OK;
}
static ULONG WINAPI monitor_ref(void *self) { return 1; }
static HRESULT WINAPI monitor_register(void *self, void *callback, void *context, INT64 *handle)
{
    if (handle)
        *handle = 1;
    return S_OK;
}
static void WINAPI monitor_unregister(void *self, INT64 handle) {}
static UINT WINAPI monitor_level(void *self) { return 2; /* AudioStateMonitorSoundLevel: full volume */ }
static void *const monitor_vtbl[] = { monitor_query, monitor_ref, monitor_ref, monitor_register, monitor_unregister,
                                      monitor_level };
static void *const audio_state_monitor = (void *)monitor_vtbl;

static HRESULT give_monitor(void **monitor)
{
    static LONG logged;
    if (!monitor)
        return E_POINTER;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("CreateRenderAudioStateMonitor*: a monitor at full volume that never calls back");
    *monitor = (void *)&audio_state_monitor;
    return S_OK;
}
static HRESULT WINAPI render_monitor(void **monitor) { return give_monitor(monitor); }
static HRESULT WINAPI render_monitor_category(UINT category, void **monitor) { return give_monitor(monitor); }
static HRESULT WINAPI render_monitor_category_role(UINT category, UINT role, void **monitor) { return give_monitor(monitor); }
static HRESULT WINAPI render_monitor_category_id(UINT category, const WCHAR *id, void **monitor) { return give_monitor(monitor); }

/* For tests: what a title gets for CreateRenderAudioStateMonitorForCategory. */
HRESULT WINAPI ReboxRenderAudioStateMonitor(UINT category, void **monitor)
{
    return render_monitor_category(category, monitor);
}

static FARPROC WINAPI get_proc_address(HMODULE module, const char *name);

/* The title's OutputDebugString text (its own boot log; Leonida's runtime traces it too) goes to
 * the log as "title: ...", one line per line of text, then on to Wine's (a debugger's). */
static void *real_output_debug_string_a, *real_output_debug_string_w;

static void log_title_text(const char *text)
{
    static LONG lines;
    char line[512];
    while (text && *text)
    {
        size_t n = strcspn(text, "\r\n");
        if (n && InterlockedIncrement(&lines) <= 20000)
        {
            snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), text);
            rebox_log("title: %s", line);
        }
        text += n;
        while (*text == '\r' || *text == '\n')
            text++;
    }
}

static void WINAPI output_debug_string_a(const char *text)
{
    if (text && !IsBadStringPtrA(text, 4096))
        log_title_text(text);
    if (real_output_debug_string_a)
        ((void (WINAPI *)(const char *))real_output_debug_string_a)(text);
}

static void WINAPI output_debug_string_w(const WCHAR *text)
{
    char narrow[1024];
    if (text && !IsBadStringPtrW(text, 4096)
            && WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow), NULL, NULL))
        log_title_text(narrow);
    if (real_output_debug_string_w)
        ((void (WINAPI *)(const WCHAR *))real_output_debug_string_w)(text);
}

/* The network, off (see the top of the file). Each kind of attempt is logged, the first few. */
static void *real_connect, *real_wsa_connect, *real_sendto, *real_wsa_sendto, *real_getaddrinfo, *real_getaddrinfo_w;

static void network_blocked(const char *what, const char *where)
{
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 16)
        rebox_log("network: %s %s blocked (REBOX_NETWORK=1 allows the title's network)", what, where ? where : "");
}

static BOOL loopback(const struct sockaddr *address, int length)
{
    static const BYTE v6_loopback[16] = { [15] = 1 };
    if (!address)
        return FALSE;
    if (address->sa_family == AF_INET && length >= (int)sizeof(struct sockaddr_in))
        return ((const struct sockaddr_in *)address)->sin_addr.S_un.S_un_b.s_b1 == 127;
    if (address->sa_family == AF_INET6 && length >= (int)sizeof(struct sockaddr_in6))
        return !memcmp(&((const struct sockaddr_in6 *)address)->sin6_addr, v6_loopback, 16);
    return FALSE;
}

static void describe_address(const struct sockaddr *address, int length, char *out, size_t size)
{
    const BYTE *b;
    *out = 0;
    if (address && address->sa_family == AF_INET && length >= (int)sizeof(struct sockaddr_in))
    {
        b = (const BYTE *)&((const struct sockaddr_in *)address)->sin_addr;
        snprintf(out, size, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
                 (unsigned)((BYTE *)&((const struct sockaddr_in *)address)->sin_port)[0] << 8 | ((BYTE *)&((const struct sockaddr_in *)address)->sin_port)[1]);
    }
    else if (address)
        snprintf(out, size, "(family %u)", address->sa_family);
}

static int WSAAPI net_connect(SOCKET s, const struct sockaddr *address, int length)
{
    char where[64];
    if (loopback(address, length))
        return ((int (WSAAPI *)(SOCKET, const struct sockaddr *, int))real_connect)(s, address, length);
    describe_address(address, length, where, sizeof(where));
    network_blocked("connect to", where);
    SetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

static int WSAAPI net_wsa_connect(SOCKET s, const struct sockaddr *address, int length, WSABUF *caller, WSABUF *callee,
        QOS *sqos, QOS *gqos)
{
    char where[64];
    if (loopback(address, length))
        return ((int (WSAAPI *)(SOCKET, const struct sockaddr *, int, WSABUF *, WSABUF *, QOS *, QOS *))real_wsa_connect)(
                s, address, length, caller, callee, sqos, gqos);
    describe_address(address, length, where, sizeof(where));
    network_blocked("WSAConnect to", where);
    SetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

static int WSAAPI net_sendto(SOCKET s, const char *buffer, int length, int flags, const struct sockaddr *to, int to_length)
{
    char where[64];
    /* No address: a connected socket, which only loopback ones are. */
    if (!to || loopback(to, to_length))
        return ((int (WSAAPI *)(SOCKET, const char *, int, int, const struct sockaddr *, int))real_sendto)(
                s, buffer, length, flags, to, to_length);
    describe_address(to, to_length, where, sizeof(where));
    network_blocked("sendto", where);
    SetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

static int WSAAPI net_wsa_sendto(SOCKET s, WSABUF *buffers, DWORD count, DWORD *sent, DWORD flags,
        const struct sockaddr *to, int to_length, WSAOVERLAPPED *overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE done)
{
    char where[64];
    if (!to || loopback(to, to_length))
        return ((int (WSAAPI *)(SOCKET, WSABUF *, DWORD, DWORD *, DWORD, const struct sockaddr *, int, WSAOVERLAPPED *,
                                LPWSAOVERLAPPED_COMPLETION_ROUTINE))real_wsa_sendto)(
                s, buffers, count, sent, flags, to, to_length, overlapped, done);
    describe_address(to, to_length, where, sizeof(where));
    network_blocked("WSASendTo", where);
    SetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

/* Names that stay on this machine: none (the host itself), localhost, loopback addresses, and the
 * machine's own name, which a retail title looks up at start (xcl allows the same). */
static BOOL local_name(const char *name)
{
    char own[256];
    DWORD size = sizeof(own);

    if (!name || !*name || !_stricmp(name, "localhost") || !strncmp(name, "127.", 4) || !strcmp(name, "::1"))
        return TRUE;
    if (GetComputerNameExA(ComputerNameDnsHostname, own, &size) && !_stricmp(name, own))
        return TRUE;
    size = sizeof(own);
    return GetComputerNameExA(ComputerNameDnsFullyQualified, own, &size) && !_stricmp(name, own);
}

static int WSAAPI net_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
        struct addrinfo **result)
{
    if (local_name(node))
        return ((int (WSAAPI *)(const char *, const char *, const struct addrinfo *, struct addrinfo **))real_getaddrinfo)(
                node, service, hints, result);
    network_blocked("looking up", node);
    if (result)
        *result = NULL;
    SetLastError(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
}

static int WSAAPI net_getaddrinfo_w(const WCHAR *node, const WCHAR *service, const ADDRINFOW *hints, ADDRINFOW **result)
{
    char name[256] = "";
    if (node)
        WideCharToMultiByte(CP_UTF8, 0, node, -1, name, sizeof(name), NULL, NULL);
    if (!node || local_name(name))
        return ((int (WSAAPI *)(const WCHAR *, const WCHAR *, const ADDRINFOW *, ADDRINFOW **))real_getaddrinfo_w)(
                node, service, hints, result);
    network_blocked("looking up", name);
    if (result)
        *result = NULL;
    SetLastError(WSAHOST_NOT_FOUND);
    return WSAHOST_NOT_FOUND;
}

/* WinHTTP: a session opens, its connections don't (ERROR_WINHTTP_CANNOT_CONNECT). */
static void *WINAPI net_winhttp_connect(void *session, const WCHAR *server, WORD port, DWORD reserved)
{
    char name[256] = "";
    if (server)
        WideCharToMultiByte(CP_UTF8, 0, server, -1, name, sizeof(name), NULL, NULL);
    network_blocked("HTTP connection to", name);
    SetLastError(12029);
    return NULL;
}

static BOOL WINAPI net_winhttp_send_request(void *request, const WCHAR *headers, DWORD headers_length, void *optional,
        DWORD optional_length, DWORD total_length, DWORD_PTR context)
{
    network_blocked("HTTP request", NULL);
    SetLastError(12029);
    return FALSE;
}

/* The title's thread priorities and affinities, logged (the first 64 calls of each): a console
 * title can rely on a higher-priority thread on the same core keeping a lower one from running,
 * which Wine's scheduling on Linux doesn't give. */
static void *real_set_thread_priority, *real_set_thread_affinity_mask, *real_set_thread_ideal_processor_ex;

static BOOL WINAPI set_thread_priority(HANDLE thread, int priority)
{
    static LONG logged;
    BOOL ok = real_set_thread_priority && ((BOOL (WINAPI *)(HANDLE, int))real_set_thread_priority)(thread, priority);
    if (InterlockedIncrement(&logged) <= 64)
        rebox_log("SetThreadPriority(thread %#lx, %d) -> %d", GetThreadId(thread), priority, ok);
    return ok;
}

static DWORD_PTR WINAPI set_thread_affinity_mask(HANDLE thread, DWORD_PTR mask)
{
    static LONG logged;
    DWORD_PTR old = real_set_thread_affinity_mask
            ? ((DWORD_PTR (WINAPI *)(HANDLE, DWORD_PTR))real_set_thread_affinity_mask)(thread, mask) : 0;
    if (InterlockedIncrement(&logged) <= 64)
        rebox_log("SetThreadAffinityMask(thread %#lx, %#llx) -> %#llx", GetThreadId(thread),
                  (unsigned long long)mask, (unsigned long long)old);
    return old;
}

static BOOL WINAPI set_thread_ideal_processor_ex(HANDLE thread, PROCESSOR_NUMBER *ideal, PROCESSOR_NUMBER *previous)
{
    static LONG logged;
    BOOL ok = real_set_thread_ideal_processor_ex
            && ((BOOL (WINAPI *)(HANDLE, PROCESSOR_NUMBER *, PROCESSOR_NUMBER *))real_set_thread_ideal_processor_ex)(
                    thread, ideal, previous);
    if (InterlockedIncrement(&logged) <= 64)
        rebox_log("SetThreadIdealProcessorEx(thread %#lx, %u) -> %d", GetThreadId(thread),
                  ideal ? ideal->Number : 0xffu, ok);
    return ok;
}

static struct
{
    const char *dll, *name;
    void *ours, *wine;
} replacements[] = {
    /* The network (off unless REBOX_NETWORK=1). WS2_32's are imported by ordinal too, which the
     * address match covers. */
    { "ws2_32.dll", "connect", net_connect },
    { "ws2_32.dll", "WSAConnect", net_wsa_connect },
    { "ws2_32.dll", "sendto", net_sendto },
    { "ws2_32.dll", "WSASendTo", net_wsa_sendto },
    { "ws2_32.dll", "getaddrinfo", net_getaddrinfo },
    { "ws2_32.dll", "GetAddrInfoW", net_getaddrinfo_w },
    { "winhttp.dll", "WinHttpConnect", net_winhttp_connect },
    { "winhttp.dll", "WinHttpSendRequest", net_winhttp_send_request },
    /* A replaced function the title looks up by name (GameChat2.dll loads its audio state monitor
     * so) is replaced as well. */
    /* The title's reservations below 2^46, the console's address width (xmem.c). */
    { "kernelbase.dll", "VirtualAlloc", xmem_low_virtual_alloc },
    { "kernel32.dll", "VirtualAlloc", xmem_low_virtual_alloc },
    { "kernelbase.dll", "SetThreadPriority", set_thread_priority },
    { "kernel32.dll", "SetThreadPriority", set_thread_priority },
    { "kernelbase.dll", "SetThreadAffinityMask", set_thread_affinity_mask },
    { "kernel32.dll", "SetThreadAffinityMask", set_thread_affinity_mask },
    { "kernelbase.dll", "SetThreadIdealProcessorEx", set_thread_ideal_processor_ex },
    { "kernel32.dll", "SetThreadIdealProcessorEx", set_thread_ideal_processor_ex },
    { "kernelbase.dll", "OutputDebugStringA", output_debug_string_a },
    { "kernel32.dll", "OutputDebugStringA", output_debug_string_a },
    { "kernelbase.dll", "OutputDebugStringW", output_debug_string_w },
    { "kernel32.dll", "OutputDebugStringW", output_debug_string_w },
    { "kernelbase.dll", "GetProcAddress", get_proc_address },
    { "kernel32.dll", "GetProcAddress", get_proc_address },
    { "user32.dll", "CreateWindowExW", create_window_w },
    { "user32.dll", "CreateWindowExA", create_window_a },
    { "twinapi.appcore.dll", "RegisterAppStateChangeNotification", app_state_register },
    { "twinapi.appcore.dll", "UnregisterAppStateChangeNotification", app_state_unregister },
    { "twinapi.appcore.dll", "RegisterAppConstrainedChangeNotification", app_constrained_register },
    { "twinapi.appcore.dll", "UnregisterAppConstrainedChangeNotification", app_state_unregister },
    { "windows.media.mediacontrol.dll", "CreateRenderAudioStateMonitor", render_monitor },
    { "windows.media.mediacontrol.dll", "CreateRenderAudioStateMonitorForCategory", render_monitor_category },
    { "windows.media.mediacontrol.dll", "CreateRenderAudioStateMonitorForCategoryAndDeviceRole",
      render_monitor_category_role },
    { "windows.media.mediacontrol.dll", "CreateRenderAudioStateMonitorForCategoryAndDeviceId",
      render_monitor_category_id },
};

static void *replacement(void *target);
static void *real_get_proc_address;

static FARPROC WINAPI get_proc_address(HMODULE module, const char *name)
{
    FARPROC p = ((FARPROC (WINAPI *)(HMODULE, const char *))real_get_proc_address)(module, name);
    void *ours;
    if (p && (ours = replacement((void *)p)) && ours != (void *)get_proc_address)
        return (FARPROC)ours;
    return p;
}

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

/* xg_xs!XGSuggestSwizzleMode(desc, what, layout *out), a newer XG library's: the dump's xg_xs.dll
 * doesn't export it, and a retail title calls it while building texture descs, passing the desc's
 * own Layout field as out. Layout 0 is the library choosing the swizzle mode itself, which the real
 * xg_xs.dll then does whenever the title asks it about that texture (xcl answers S_OK with the mode
 * it computes; the same choice, made by the library instead of here). */
static HRESULT WINAPI xg_suggest_swizzle_mode(const void *desc, UINT what, UINT *out)
{
    static LONG logged;
    if (!desc || !out)
        return E_POINTER;
    *out = 0;
    if (!InterlockedExchange(&logged, 1))
        rebox_log("xg_xs!XGSuggestSwizzleMode: layout 0 (the library chooses the swizzle mode)");
    return S_OK;
}

static void *make_stub(const struct wine_stub *wine)
{
    if (wine->name && !IsBadStringPtrA(wine->name, 64) && !strcmp(wine->name, "XGSuggestSwizzleMode"))
        return (void *)xg_suggest_swizzle_mode;
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
            rebox_log("import fixup: %s!%s, imported by %s, has no implementation in Wine; %s",
                         ((struct wine_stub *)*iat)->dll, ((struct wine_stub *)*iat)->name, file,
                         ours == (void *)xg_suggest_swizzle_mode ? "answered here" : "it will return E_NOTIMPL");
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

    char *network = rebox_getenv("REBOX_NETWORK");
    BOOL network_on = network && atoi(network);

    free(network);
    if (network_on)
        rebox_log("network: on (REBOX_NETWORK=1)");
    InitializeCriticalSection(&stub_lock);
    for (UINT i = 0; i < ARRAYSIZE(replacements); i++)
    {
        BOOL net = !_stricmp(replacements[i].dll, "ws2_32.dll") || !_stricmp(replacements[i].dll, "winhttp.dll");
        HMODULE dll = net && network_on ? NULL : LoadLibraryA(replacements[i].dll);
        replacements[i].wine = dll ? (void *)GetProcAddress(dll, replacements[i].name) : NULL;
        if (replacements[i].ours == (void *)create_window_w)
            real_create_window_w = replacements[i].wine;
        if (replacements[i].ours == (void *)get_proc_address && replacements[i].wine && !real_get_proc_address)
            real_get_proc_address = replacements[i].wine;
        if (replacements[i].ours == (void *)create_window_a)
            real_create_window_a = replacements[i].wine;
        if (replacements[i].ours == (void *)net_connect) real_connect = replacements[i].wine;
        if (replacements[i].ours == (void *)set_thread_priority && replacements[i].wine) real_set_thread_priority = replacements[i].wine;
        if (replacements[i].ours == (void *)set_thread_affinity_mask && replacements[i].wine) real_set_thread_affinity_mask = replacements[i].wine;
        if (replacements[i].ours == (void *)set_thread_ideal_processor_ex && replacements[i].wine) real_set_thread_ideal_processor_ex = replacements[i].wine;
        if (replacements[i].ours == (void *)net_wsa_connect) real_wsa_connect = replacements[i].wine;
        if (replacements[i].ours == (void *)net_sendto) real_sendto = replacements[i].wine;
        if (replacements[i].ours == (void *)net_wsa_sendto) real_wsa_sendto = replacements[i].wine;
        if (replacements[i].ours == (void *)net_getaddrinfo) real_getaddrinfo = replacements[i].wine;
        if (replacements[i].ours == (void *)net_getaddrinfo_w) real_getaddrinfo_w = replacements[i].wine;
        if (replacements[i].ours == (void *)output_debug_string_a && replacements[i].wine && !real_output_debug_string_a)
            real_output_debug_string_a = replacements[i].wine;
        if (replacements[i].ours == (void *)output_debug_string_w && replacements[i].wine && !real_output_debug_string_w)
            real_output_debug_string_w = replacements[i].wine;
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
