#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "xgr_internal.h"

namespace gtx::xgr::launch {
namespace {

constexpr char kBackgroundPrefix[] = "ms-bgm-";
constexpr DWORD kRestartOnCrashOnly = RESTART_NO_HANG | RESTART_NO_PATCH | RESTART_NO_REBOOT;

bool to_wide(const char* text, std::wstring* out) {
  out->clear();
  if (!*text) return true;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
  if (n <= 0) return false;
  out->resize(static_cast<size_t>(n));
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out->data(), n) != n) return false;
  out->resize(static_cast<size_t>(n - 1));
  return true;
}

bool starts_with_ignore_case(const char* text, const char* prefix) {
  for (; *prefix; ++text, ++prefix)
    if (tolower(static_cast<unsigned char>(*text)) != tolower(static_cast<unsigned char>(*prefix))) return false;
  return true;
}

bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

size_t scheme_length(const char* uri) {
  if (!is_alpha(uri[0])) return 0;
  size_t i = 1;
  while (is_alpha(uri[i]) || (uri[i] >= '0' && uri[i] <= '9') || uri[i] == '+' || uri[i] == '-' || uri[i] == '.') ++i;
  return uri[i] == ':' ? i : 0;
}

HRESULT XLaunchUri_impl(void*, void*, const char* uri) {
  if (!uri) return E_INVALIDARG;
  if (starts_with_ignore_case(uri, kBackgroundPrefix)) uri += sizeof(kBackgroundPrefix) - 1;
  size_t scheme = scheme_length(uri);
  if (scheme < 2) return E_INVALIDARG;
  if (scheme == 4 && starts_with_ignore_case(uri, "file")) return E_ACCESSDENIED;
  std::wstring wide;
  if (!to_wide(uri, &wide)) return E_INVALIDARG;
  HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  SHELLEXECUTEINFOW info{sizeof(info)};
  info.fMask = SEE_MASK_NOASYNC;
  info.lpFile = wide.c_str();
  info.nShow = SW_SHOWNORMAL;
  HRESULT hr = ShellExecuteExW(&info) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
  if (SUCCEEDED(com)) CoUninitialize();
  return hr;
}

void start_new_game(const char* exePath, const char* args) {
  std::wstring path, arguments;
  if (!exePath || !to_wide(exePath, &path) || path.empty()) return;
  if (args && !to_wide(args, &arguments)) return;
  DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (!n) return;
  std::wstring full(n, L'\0');
  n = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
  if (!n || n >= full.size()) return;
  full.resize(n);
  std::wstring commandLine = L"\"" + full + L"\"";
  if (!arguments.empty()) commandLine += L" " + arguments;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(full.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void XLaunchNewGame_impl(void*, const char* exePath, const char* args, void*) {
  start_new_game(exePath, args);
  TerminateProcess(GetCurrentProcess(), 0);
}

HRESULT XLaunchRestartOnCrash_impl(void*, const char* args, uint32_t reserved) {
  if (reserved) return E_INVALIDARG;
  std::wstring arguments;
  if (args && !to_wide(args, &arguments)) return E_INVALIDARG;
  return RegisterApplicationRestart(args ? arguments.c_str() : nullptr, kRestartOnCrashOnly);
}

const Override kOverrides[] = {
    {"XLaunchUri", reinterpret_cast<void*>(&XLaunchUri_impl)},
    {"XLaunchNewGame", reinterpret_cast<void*>(&XLaunchNewGame_impl)},
    {"XLaunchRestartOnCrash", reinterpret_cast<void*>(&XLaunchRestartOnCrash_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::launch
