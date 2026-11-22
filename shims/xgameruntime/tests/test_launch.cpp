#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);
struct AsyncBlock { void* queue; void* context; void (*callback)(AsyncBlock*); unsigned char internal[32]; };
struct Token { uint64_t value; };

static int g_failures = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_failures++;
}

static QueryApi g_query;
static std::string g_table;
static int TableSlot(const char* method) {
  std::string key = std::string("\"name\": \"") + method + "\",";
  size_t at = g_table.find(key);
  if (at == std::string::npos || g_table.find(key, at + 1) != std::string::npos) return -1;
  size_t slot = g_table.find("\"slot\": ", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (slot == std::string::npos || (next != std::string::npos && slot > next)) return -1;
  return std::atoi(g_table.c_str() + slot + 8);
}
static std::string TableUuid(const char* iface) {
  std::string key = std::string("\"name\": \"") + iface + "\",";
  size_t at = g_table.find(key);
  if (at == std::string::npos) return "";
  size_t uuid = g_table.find("\"uuid\": \"", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (uuid == std::string::npos || (next != std::string::npos && uuid > next)) return "";
  return g_table.substr(uuid + 9, 36);
}
static bool LoadTable() {
  std::ifstream file("table.json", std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  g_table = text.str();
  return !g_table.empty();
}
static std::string GuidText(const GUID& g) {
  char text[40];
  std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return text;
}

struct Api {
  void* self;
  void** slots;
  template <typename Fn> Fn At(int slot, const char* method) const {
    int expected = TableSlot(method);
    if (expected != slot) {
      std::printf("FAIL slot pin: %s is slot %d in table.json, the test uses %d\n", method, expected, slot);
      std::exit(1);
    }
    return reinterpret_cast<Fn>(slots[slot]);
  }
};
static Api Get(const char* iface, const GUID& iid) {
  if (TableUuid(iface) != GuidText(iid)) {
    std::printf("FAIL guid pin: %s is %s in table.json, the test uses %s\n", iface, TableUuid(iface).c_str(), GuidText(iid).c_str());
    std::exit(1);
  }
  void* object = nullptr;
  HRESULT hr = g_query(iid, iid, &object);
  if (FAILED(hr) || !object) {
    std::printf("FAIL QueryApiImpl %s %08lx\n", iface, static_cast<unsigned long>(hr));
    std::exit(1);
  }
  return {object, *static_cast<void***>(object)};
}

static const GUID kLauncher = {0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};
static const GUID kGame2 = {0x50849859, 0x0ad8, 0x4f81, {0x80, 0xe4, 0x5b, 0xc7, 0x86, 0x26, 0xf8, 0x52}};
static const GUID kGame3 = {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};

constexpr DWORD kRestartOnCrashOnly = RESTART_NO_HANG | RESTART_NO_PATCH | RESTART_NO_REBOOT;

static std::wstring g_exe;
static std::wstring g_exe_dir;

static std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), n);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}
static std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}
static std::wstring TempPath(const std::wstring& name) {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  return std::wstring(temp) + name;
}
static void WriteText(const std::wstring& path, const std::string& text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
static bool ReadText(const std::wstring& path, std::string* text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
  char buffer[1024];
  size_t n = std::fread(buffer, 1, sizeof(buffer), f);
  std::fclose(f);
  text->assign(buffer, n);
  return true;
}
static bool WaitForFile(const std::wstring& path, std::string* text, DWORD timeoutMs) {
  for (DWORD waited = 0; waited <= timeoutMs; waited += 50) {
    if (ReadText(path, text) && !text->empty()) return true;
    Sleep(50);
  }
  return false;
}

static HRESULT LaunchUri(const char* uri) {
  Api launcher = Get("IXLauncherImpl", kLauncher);
  return launcher.At<HRESULT (*)(void*, void*, const char*)>(3, "XLaunchUri")(launcher.self, nullptr, uri);
}

static bool RegisterScheme(const std::wstring& scheme) {
  std::wstring key = L"Software\\Classes\\" + scheme;
  std::wstring command = L"\"" + g_exe + L"\" uri-marker \"%1\"";
  HKEY root = nullptr, open = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &root, nullptr) != ERROR_SUCCESS) return false;
  const wchar_t kEmpty[] = L"";
  bool ok = RegSetValueExW(root, L"URL Protocol", 0, REG_SZ, reinterpret_cast<const BYTE*>(kEmpty), sizeof(kEmpty)) == ERROR_SUCCESS &&
            RegCreateKeyExW(root, L"shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &open, nullptr) == ERROR_SUCCESS &&
            RegSetValueExW(open, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  if (open) RegCloseKey(open);
  RegCloseKey(root);
  return ok;
}

static void CheckLaunchUri() {
  std::wstring scheme = L"xgrtest" + std::to_wstring(GetCurrentProcessId());
  std::wstring marker = TempPath(scheme + L".txt");
  DeleteFileW(marker.c_str());
  bool registered = RegisterScheme(scheme);
  Check(registered, "a per-user URL scheme pointing at this test is registered");
  if (registered) {
    std::string uri = Narrow(scheme) + ":open?level=2";
    std::string received;
    Check(LaunchUri(uri.c_str()) == S_OK && WaitForFile(marker, &received, 10000) && received == uri,
          "XLaunchUri hands the URI to the program registered for its scheme");
    DeleteFileW(marker.c_str());
    std::string background = "ms-bgm-" + uri;
    received.clear();
    Check(LaunchUri(background.c_str()) == S_OK && WaitForFile(marker, &received, 10000) && received == uri,
          "XLaunchUri drops the ms-bgm- prefix and launches the rest normally (documented PC behaviour)");
    DeleteFileW(marker.c_str());
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, (L"Software\\Classes\\" + scheme).c_str());
  Check(LaunchUri(nullptr) == E_INVALIDARG, "XLaunchUri rejects a null URI");
  Check(LaunchUri("no scheme here") == E_INVALIDARG && LaunchUri(":nothing") == E_INVALIDARG, "XLaunchUri rejects text without a URI scheme");
  Check(LaunchUri("C:\\Windows\\notepad.exe") == E_INVALIDARG, "XLaunchUri does not run a file path (a drive letter is not a scheme)");
  Check(LaunchUri("file:///C:/Windows/notepad.exe") == E_ACCESSDENIED && LaunchUri("FILE:C:/x") == E_ACCESSDENIED,
        "XLaunchUri refuses file: URIs");
  Check(LaunchUri("xgr:\xff\xfe") == E_INVALIDARG, "XLaunchUri rejects a URI that is not UTF-8");
}

static void CheckRestartOnCrash() {
  Api game3 = Get("IXGameImpl3", kGame3);
  auto restart = game3.At<HRESULT (*)(void*, const char*, uint32_t)>(5, "XLaunchRestartOnCrash");
  wchar_t args[RESTART_MAX_CMD_LINE] = {};
  DWORD size = RESTART_MAX_CMD_LINE;
  DWORD flags = 0;
  Check(restart(game3.self, "--resume slot1", 0) == S_OK, "XLaunchRestartOnCrash registers a restart");
  Check(GetApplicationRestartSettings(GetCurrentProcess(), args, &size, &flags) == S_OK && std::wstring(args) == L"--resume slot1" &&
            flags == kRestartOnCrashOnly,
        "the Windows restart registration carries the title's arguments and restarts on a crash only");
  Check(restart(game3.self, nullptr, 0) == S_OK, "XLaunchRestartOnCrash accepts no arguments");
  Check(restart(game3.self, "--resume", 1) == E_INVALIDARG, "XLaunchRestartOnCrash rejects a non-zero reserved value");
  std::string tooLong(RESTART_MAX_CMD_LINE + 8, 'a');
  Check(FAILED(restart(game3.self, tooLong.c_str(), 0)), "XLaunchRestartOnCrash rejects arguments longer than Windows allows");
  UnregisterApplicationRestart();
}

static DWORD RunSelf(const std::wstring& args, DWORD* exitCode) {
  std::wstring command = L"\"" + g_exe + L"\" " + args;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(g_exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, g_exe_dir.c_str(), &startup, &process)) return WAIT_FAILED;
  DWORD wait = WaitForSingleObject(process.hProcess, 20000);
  GetExitCodeProcess(process.hProcess, exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return wait;
}

static void CheckNewGame() {
  std::wstring tag = std::to_wstring(GetCurrentProcessId());
  std::wstring started = TempPath(L"xgr_new_game_" + tag + L".txt");
  std::wstring returned = TempPath(L"xgr_new_game_returned_" + tag + L".txt");
  DeleteFileW(started.c_str());
  DeleteFileW(returned.c_str());
  DWORD code = 1;
  Check(RunSelf(L"new-game-parent " + tag, &code) == WAIT_OBJECT_0 && code == 0, "XLaunchNewGame ends the calling process (exit code 0)");
  std::string text;
  Check(WaitForFile(started, &text, 10000) && text == "new-game-child " + Narrow(tag) + " --level 3",
        "XLaunchNewGame starts the new game (path relative to the current directory) with its arguments");
  Check(!ReadText(returned, &text), "XLaunchNewGame does not return to the caller");
  DeleteFileW(started.c_str());

  code = 1;
  Check(RunSelf(L"new-game-missing " + tag, &code) == WAIT_OBJECT_0 && code == 0 && !ReadText(returned, &text),
        "XLaunchNewGame ends the calling process even when the new game cannot start (documented)");
  DeleteFileW(returned.c_str());
}

static int ChildMain(int argc, char** argv) {
  std::string mode = argv[1];
  if (mode == "uri-marker" && argc >= 3) {
    std::string uri = argv[2];
    size_t colon = uri.find(':');
    WriteText(TempPath(Widen(uri.substr(0, colon)) + L".txt"), uri);
    return 0;
  }
  if (mode == "new-game-child" && argc >= 5) {
    WriteText(TempPath(L"xgr_new_game_" + Widen(argv[2]) + L".txt"), std::string("new-game-child ") + argv[2] + " " + argv[3] + " " + argv[4]);
    return 0;
  }
  if ((mode == "new-game-parent" || mode == "new-game-missing") && argc >= 3) {
    HMODULE module = LoadLibraryA("xgameruntime.dll");
    g_query = reinterpret_cast<QueryApi>(GetProcAddress(module, "QueryApiImpl"));
    if (!LoadTable()) return 2;
    Api game2 = Get("IXGameImpl2", kGame2);
    std::string exe = Narrow(g_exe.substr(g_exe_dir.size()));
    std::string path = mode == "new-game-parent" ? exe : "xgr_no_such_game.exe";
    std::string args = std::string("new-game-child ") + argv[2] + " --level 3";
    game2.At<void (*)(void*, const char*, const char*, void*)>(4, "XLaunchNewGame")(game2.self, path.c_str(), args.c_str(), nullptr);
    WriteText(TempPath(L"xgr_new_game_returned_" + Widen(argv[2]) + L".txt"), "returned");
    return 3;
  }
  return 4;
}

int main(int argc, char** argv) {
  wchar_t exe[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_exe.assign(exe, n);
  g_exe_dir = g_exe.substr(0, g_exe.find_last_of(L'\\') + 1);
  if (argc >= 2) return ChildMain(argc, argv);
  HMODULE module = LoadLibraryA("xgameruntime.dll");
  if (!module) {
    std::printf("FAIL LoadLibrary\n");
    return 1;
  }
  g_query = reinterpret_cast<QueryApi>(GetProcAddress(module, "QueryApiImpl"));
  if (!LoadTable()) {
    std::printf("FAIL table.json not found next to the test (run it from the build directory)\n");
    return 1;
  }
  CheckLaunchUri();
  CheckRestartOnCrash();
  CheckNewGame();
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
