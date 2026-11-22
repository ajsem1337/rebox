#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "xgr_internal.h"
#include "xtask.h"
#include "handles.h"

namespace gtx::xgr::storage { bool directory(std::string* path); }

namespace gtx::xgr::package {
namespace {

constexpr HRESULT kInvalidSelector = static_cast<HRESULT>(0x89245203L);
constexpr HRESULT kNoPackageIdentifier = static_cast<HRESULT>(0x89245208L);
constexpr size_t kPackageIdentifierMaxLength = 33;
constexpr size_t kMaxIdentifierScan = 4096;
constexpr size_t kMaxConfigBytes = 1024 * 1024;
constexpr uint64_t kWriteIntervalMs = 5ull * 60ull * 1000ull;
constexpr uint64_t kWriteBudgetBytes = 1ull << 30;

enum class SelectorType : uint32_t { Language, Tag, Chunk, Feature };
enum class Availability : uint32_t { Ready, Pending, Installable, Unavailable };
enum class Kind : uint32_t { Game, Content, PublisherContent };
enum class Scope : uint32_t { ThisOnly, ThisAndRelated, ThisPublisher };

struct Selector {
  SelectorType type;
  union {
    const char* text;
    uint32_t chunkId;
  };
};
static_assert(sizeof(Selector) == 16, "XPackageChunkSelector layout");

struct Version {
  uint16_t major, minor, build, revision;
};

struct Details {
  const char* packageIdentifier;
  Version version;
  Kind kind;
  const char* displayName;
  const char* description;
  const char* publisher;
  const char* storeId;
  bool installing;
  uint32_t index;
  uint32_t count;
  bool ageRestricted;
  const char* titleId;
};
static_assert(sizeof(Details) == 80, "XPackageDetails layout");

struct Feature {
  const char* id;
  const char* displayName;
  const char* tags;
  bool hidden;
  uint32_t storeIdCount;
  const char** storeIds;
};

struct Progress {
  uint64_t totalBytes;
  uint64_t installedBytes;
  uint64_t launchBytes;
  bool launchable;
  bool completed;
};
static_assert(sizeof(Progress) == 32, "XPackageInstallationProgress layout");

struct WriteStats {
  uint64_t interval;
  uint64_t budget;
  uint64_t elapsed;
  uint64_t bytesWritten;
};

using EnumerationCallback = bool(CALLBACK*)(void* context, const Details* details);
using ChunkAvailabilityCallback = bool(CALLBACK*)(void* context, const Selector* selector, Availability availability);
using FeatureCallback = bool(CALLBACK*)(void* context, const Feature* feature);
using InstalledCallback = void(CALLBACK*)(void* context, const Details* details);
using ProgressCallback = void(CALLBACK*)(void* context, void* monitor);

struct Monitor {
  std::set<uint64_t> tokens;
};
struct Mount {
  std::string path;
};

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

bool read_env(const wchar_t* name, std::wstring* out) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return false;
  std::wstring v(n, L'\0');
  DWORD got = GetEnvironmentVariableW(name, v.data(), n);
  if (got == 0 || got >= n) return false;
  v.resize(got);
  *out = v;
  return true;
}

bool read_env_utf8(const wchar_t* name, std::string* out) {
  std::wstring w;
  if (!read_env(name, &w)) return false;
  *out = to_utf8(w);
  return true;
}

std::wstring executable_directory() {
  std::wstring path(32768, L'\0');
  DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (n == 0 || n >= path.size()) return {};
  path.resize(n);
  size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  path.resize(slash + 1);
  return path;
}

bool equals_ignore_case(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

class GameConfig {
 public:
  bool load(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    std::string text(kMaxConfigBytes, '\0');
    size_t len = std::fread(text.data(), 1, text.size(), f);
    std::fclose(f);
    text.resize(len);
    xml_ = strip_comments(text);
    return true;
  }

  bool element_text(const char* element, std::string* out) const {
    size_t start = 0, end = 0;
    if (!find_open_tag(element, 0, &start, &end)) return false;
    if (end > 0 && xml_[end - 1] == '/') return false;
    size_t close = find_ignore_case("</" + std::string(element), end + 1);
    if (close == std::string::npos) return false;
    *out = trim(decode(xml_.substr(end + 1, close - end - 1)));
    return true;
  }

  bool attribute(const char* element, const char* name, std::string* out) const {
    std::vector<std::string> all = attributes(element, name);
    if (all.empty()) return false;
    *out = all.front();
    return true;
  }

  std::vector<std::string> attributes(const char* element, const char* name) const {
    std::vector<std::string> values;
    size_t from = 0, start = 0, end = 0;
    while (find_open_tag(element, from, &start, &end)) {
      std::string tag = xml_.substr(start, end - start);
      std::string value;
      if (attribute_in_tag(tag, name, &value)) values.push_back(value);
      from = end;
    }
    return values;
  }

 private:
  static std::string strip_comments(const std::string& in) {
    std::string out;
    size_t pos = 0;
    while (pos < in.size()) {
      size_t open = in.find("<!--", pos);
      if (open == std::string::npos) {
        out.append(in, pos, std::string::npos);
        break;
      }
      out.append(in, pos, open - pos);
      size_t close = in.find("-->", open + 4);
      if (close == std::string::npos) break;
      pos = close + 3;
    }
    return out;
  }

  static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }

  static std::string decode(const std::string& s) {
    static const struct { const char* entity; char ch; } kEntities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    for (size_t i = 0; i < s.size();) {
      bool replaced = false;
      if (s[i] == '&') {
        for (const auto& e : kEntities) {
          size_t n = std::strlen(e.entity);
          if (s.compare(i, n, e.entity) == 0) {
            out.push_back(e.ch);
            i += n;
            replaced = true;
            break;
          }
        }
      }
      if (!replaced) out.push_back(s[i++]);
    }
    return out;
  }

  size_t find_ignore_case(const std::string& needle, size_t from) const {
    if (needle.empty() || xml_.size() < needle.size()) return std::string::npos;
    for (size_t i = from; i + needle.size() <= xml_.size(); ++i)
      if (equals_ignore_case(xml_.substr(i, needle.size()), needle)) return i;
    return std::string::npos;
  }

  bool find_open_tag(const char* element, size_t from, size_t* start, size_t* end) const {
    std::string open = "<" + std::string(element);
    for (size_t pos = find_ignore_case(open, from); pos != std::string::npos; pos = find_ignore_case(open, pos + 1)) {
      size_t after = pos + open.size();
      if (after >= xml_.size()) return false;
      char c = xml_[after];
      if (c != '>' && c != '/' && c != ' ' && c != '\t' && c != '\r' && c != '\n') continue;
      size_t close = xml_.find('>', after);
      if (close == std::string::npos) return false;
      *start = pos;
      *end = close;
      return true;
    }
    return false;
  }

  static bool attribute_in_tag(const std::string& tag, const char* name, std::string* out) {
    std::string key = name;
    for (size_t pos = 0; pos + key.size() < tag.size(); ++pos) {
      char before = pos == 0 ? ' ' : tag[pos - 1];
      if (before != ' ' && before != '\t' && before != '\r' && before != '\n') continue;
      if (!equals_ignore_case(tag.substr(pos, key.size()), key)) continue;
      size_t p = pos + key.size();
      while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t')) ++p;
      if (p >= tag.size() || tag[p] != '=') continue;
      ++p;
      while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t')) ++p;
      if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) continue;
      char quote = tag[p];
      size_t close = tag.find(quote, p + 1);
      if (close == std::string::npos) return false;
      *out = decode(tag.substr(p + 1, close - p - 1));
      return true;
    }
    return false;
  }

  std::string xml_;
};

bool is_safe_name(const std::string& s, size_t max_length) {
  if (s.empty() || s.size() > max_length) return false;
  for (char c : s) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  if (s.front() == '.' || s.back() == '.') return false;
  std::string stem = s.substr(0, s.find('.'));
  static const char* const kReserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
  for (const char* r : kReserved)
    if (equals_ignore_case(stem, r)) return false;
  if (stem.size() == 4 && (equals_ignore_case(stem.substr(0, 3), "COM") || equals_ignore_case(stem.substr(0, 3), "LPT")) &&
      stem[3] >= '0' && stem[3] <= '9')
    return false;
  return true;
}

bool parse_version(const std::string& text, Version* out) {
  unsigned parts[4] = {0, 0, 0, 0};
  int count = 0;
  size_t pos = 0;
  while (count < 4) {
    size_t dot = text.find('.', pos);
    std::string part = text.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    if (part.empty() || part.size() > 5 || part.find_first_not_of("0123456789") != std::string::npos) return false;
    unsigned long v = std::strtoul(part.c_str(), nullptr, 10);
    if (v > 0xFFFF) return false;
    parts[count++] = static_cast<unsigned>(v);
    if (dot == std::string::npos) break;
    pos = dot + 1;
  }
  if (count == 0) return false;
  *out = {static_cast<uint16_t>(parts[0]), static_cast<uint16_t>(parts[1]), static_cast<uint16_t>(parts[2]),
          static_cast<uint16_t>(parts[3])};
  return true;
}

bool parse_title_id(const std::string& text, std::string* out) {
  if (text.empty() || text.size() > 8 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08lX", std::strtoul(text.c_str(), nullptr, 16));
  *out = buf;
  return true;
}

struct Identity {
  std::string packageId;
  std::string storeId;
  std::string displayName;
  std::string publisher;
  std::string description;
  std::string titleId;
  Version version{1, 0, 0, 0};
  std::vector<std::string> languages;
  std::wstring directory;
  std::string directoryUtf8;
};

std::string pick(const GameConfig* config, const char* element, const char* attribute, const wchar_t* env, const char* fallback) {
  std::string v;
  if (config && (attribute ? config->attribute(element, attribute, &v) : config->element_text(element, &v)) && !v.empty()) return v;
  if (read_env_utf8(env, &v)) return v;
  return fallback;
}

const Identity& identity() {
  static Identity id;
  static std::once_flag once;
  std::call_once(once, [] {
    id.directory = executable_directory();
    id.directoryUtf8 = to_utf8(id.directory);
    GameConfig loaded;
    const GameConfig* config = loaded.load(id.directory + L"MicrosoftGame.config") ? &loaded : nullptr;

    std::string env_id;
    id.packageId = read_env_utf8(L"XGR_PACKAGE_ID", &env_id) && is_safe_name(env_id, kPackageIdentifierMaxLength - 1)
                       ? env_id
                       : "XGR.LocalPackage";
    id.storeId = pick(config, "StoreId", nullptr, L"XGR_PACKAGE_STORE_ID", "XGRLOCAL0000");
    id.displayName = pick(config, "ShellVisuals", "DefaultDisplayName", L"XGR_PACKAGE_DISPLAY_NAME", "XGR Local Package");
    id.publisher = pick(config, "ShellVisuals", "PublisherDisplayName", L"XGR_PACKAGE_PUBLISHER", "XGR Local Publisher");
    id.description = pick(config, "ShellVisuals", "Description", L"XGR_PACKAGE_DESCRIPTION", "");

    Version v;
    if (parse_version(pick(config, "Identity", "Version", L"XGR_PACKAGE_VERSION", ""), &v)) id.version = v;
    parse_title_id(pick(config, "TitleId", nullptr, L"XGR_TITLE_ID", ""), &id.titleId);

    if (config) {
      for (const std::string& lang : config->attributes("Resource", "Language"))
        if (!lang.empty()) id.languages.push_back(lang);
    }
  });
  return id;
}

uint64_t directory_bytes(const std::wstring& dir) {
  uint64_t total = 0;
  std::vector<std::wstring> pending{dir};
  while (!pending.empty()) {
    std::wstring current = pending.back();
    pending.pop_back();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((current + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        pending.push_back(current + fd.cFileName + L"\\");
      else
        total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return total;
}

uint64_t package_bytes() {
  static uint64_t bytes = 0;
  static std::once_flag once;
  std::call_once(once, [] { bytes = directory_bytes(identity().directory); });
  return bytes;
}

SRWLOCK g_lock = SRWLOCK_INIT;
std::set<Monitor*> g_monitors;
std::set<Mount*> g_mounts;
std::set<uint64_t> g_installed_tokens;
std::atomic<uint64_t> g_next_token{1};

struct Locked {
  Locked() { AcquireSRWLockExclusive(&g_lock); }
  ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

Monitor* new_monitor() {
  auto* m = new Monitor;
  {
    Locked lock;
    g_monitors.insert(m);
  }
  handles::opened(m, handles::Type::PackageInstallationMonitor);
  return m;
}

Mount* new_mount(const std::string& path) {
  auto* m = new Mount{path};
  {
    Locked lock;
    g_mounts.insert(m);
  }
  handles::opened(m, handles::Type::PackageMount);
  return m;
}

HRESULT resolve_package(const char* packageIdentifier) {
  if (!packageIdentifier) return E_INVALIDARG;
  size_t len = strnlen(packageIdentifier, kMaxIdentifierScan);
  if (len == kMaxIdentifierScan) return kNoPackageIdentifier;
  return identity().packageId == std::string(packageIdentifier, len) ? S_OK : kNoPackageIdentifier;
}

bool declares_language(const char* language) {
  const Identity& id = identity();
  if (id.languages.empty()) return true;
  for (const std::string& l : id.languages)
    if (equals_ignore_case(l, language)) return true;
  return false;
}

HRESULT check_selectors(uint32_t count, const Selector* selectors) {
  if (count > 0 && !selectors) return E_INVALIDARG;
  for (uint32_t i = 0; i < count; ++i) {
    const Selector& s = selectors[i];
    switch (s.type) {
      case SelectorType::Chunk:
        break;
      case SelectorType::Language:
        if (!s.text) return E_INVALIDARG;
        if (!declares_language(s.text)) return kInvalidSelector;
        break;
      case SelectorType::Tag:
      case SelectorType::Feature:
        if (!s.text) return E_INVALIDARG;
        break;
      default:
        return E_INVALIDARG;
    }
  }
  return S_OK;
}

HRESULT check_package_and_selectors(const char* packageIdentifier, uint32_t count, const Selector* selectors) {
  HRESULT hr = resolve_package(packageIdentifier);
  if (FAILED(hr)) return hr;
  return check_selectors(count, selectors);
}

HRESULT copy_string(const std::string& value, size_t size, char* buffer) {
  if (!buffer) return E_INVALIDARG;
  if (size < value.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
  std::memcpy(buffer, value.c_str(), value.size() + 1);
  return S_OK;
}

Details package_details() {
  const Identity& id = identity();
  Details d{};
  d.packageIdentifier = id.packageId.c_str();
  d.version = id.version;
  d.kind = Kind::Game;
  d.displayName = id.displayName.c_str();
  d.description = id.description.c_str();
  d.publisher = id.publisher.c_str();
  d.storeId = id.storeId.c_str();
  d.installing = false;
  d.index = 0;
  d.count = 1;
  d.ageRestricted = false;
  d.titleId = id.titleId.c_str();
  return d;
}

HRESULT XPackageGetCurrentProcessPackageIdentifier_impl(void*, size_t size, char* buffer) {
  return copy_string(identity().packageId, size, buffer);
}

bool XPackageIsPackagedProcess_impl(void*) { return true; }

HRESULT XPackageCreateInstallationMonitor_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors,
                                               uint32_t, void*, void** monitor) {
  if (!monitor) return E_INVALIDARG;
  *monitor = nullptr;
  HRESULT hr = check_package_and_selectors(packageIdentifier, count, selectors);
  if (FAILED(hr)) return hr;
  *monitor = new_monitor();
  return S_OK;
}

void XPackageCloseInstallationMonitorHandle_impl(void*, void* handle) {
  auto* m = static_cast<Monitor*>(handle);
  {
    Locked lock;
    if (!g_monitors.erase(m)) return;
  }
  handles::closed(m);
  delete m;
}

bool is_live_monitor(void* handle) {
  Locked lock;
  return g_monitors.count(static_cast<Monitor*>(handle)) != 0;
}

void XPackageGetInstallationProgress_impl(void*, void* handle, Progress* progress) {
  if (!progress) return;
  *progress = {};
  if (!is_live_monitor(handle)) return;
  uint64_t bytes = package_bytes();
  progress->totalBytes = bytes;
  progress->installedBytes = bytes;
  progress->launchBytes = bytes;
  progress->launchable = true;
  progress->completed = true;
}

bool XPackageUpdateInstallationMonitor_impl(void*, void*) { return false; }

HRESULT XPackageRegisterInstallationProgressChanged_impl(void*, void* handle, void*, ProgressCallback callback,
                                                         task::RegistrationToken* token) {
  if (!handle || !callback || !token) return E_INVALIDARG;
  Locked lock;
  auto it = g_monitors.find(static_cast<Monitor*>(handle));
  if (it == g_monitors.end()) return E_INVALIDARG;
  token->token = g_next_token++;
  (*it)->tokens.insert(token->token);
  return S_OK;
}

bool XPackageUnregisterInstallationProgressChanged_impl(void*, void* handle, task::RegistrationToken token, bool) {
  Locked lock;
  auto it = g_monitors.find(static_cast<Monitor*>(handle));
  if (it == g_monitors.end()) return false;
  return (*it)->tokens.erase(token.token) != 0;
}

std::vector<std::string> user_locales() {
  std::vector<std::string> out;
  wchar_t name[LOCALE_NAME_MAX_LENGTH];
  if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) out.push_back(to_utf8(name));
  ULONG languages = 0, chars = 0;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languages, nullptr, &chars) && chars > 0) {
    std::wstring list(chars, L'\0');
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &languages, list.data(), &chars))
      for (const wchar_t* p = list.c_str(); *p; p += wcslen(p) + 1) out.push_back(to_utf8(p));
  }
  return out;
}

std::string primary_subtag(const std::string& locale) { return locale.substr(0, locale.find('-')); }

HRESULT XPackageGetUserLocale_impl(void*, size_t size, char* locale) {
  if (!locale) return E_INVALIDARG;
  const Identity& id = identity();
  if (id.languages.empty()) return copy_string("", size, locale);
  std::vector<std::string> user = user_locales();
  for (const std::string& u : user)
    for (const std::string& p : id.languages)
      if (equals_ignore_case(u, p)) return copy_string(u, size, locale);
  for (const std::string& u : user)
    for (const std::string& p : id.languages)
      if (equals_ignore_case(primary_subtag(u), primary_subtag(p))) return copy_string(u, size, locale);
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT XPackageFindChunkAvailability_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors,
                                           Availability* availability) {
  if (!availability) return E_INVALIDARG;
  HRESULT hr = check_package_and_selectors(packageIdentifier, count, selectors);
  if (FAILED(hr)) return hr;
  *availability = Availability::Ready;
  return S_OK;
}

HRESULT XPackageEnumerateChunkAvailability_impl(void*, const char* packageIdentifier, SelectorType type, void* context,
                                                ChunkAvailabilityCallback callback) {
  if (!callback || static_cast<uint32_t>(type) > static_cast<uint32_t>(SelectorType::Feature)) return E_INVALIDARG;
  HRESULT hr = resolve_package(packageIdentifier);
  if (FAILED(hr)) return hr;
  if (type != SelectorType::Language) return S_OK;
  for (const std::string& lang : identity().languages) {
    Selector s{};
    s.type = SelectorType::Language;
    s.text = lang.c_str();
    if (!callback(context, &s, Availability::Ready)) break;
  }
  return S_OK;
}

HRESULT XPackageChangeChunkInstallOrder_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors) {
  return check_package_and_selectors(packageIdentifier, count, selectors);
}

HRESULT XPackageInstallChunks_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors, uint32_t,
                                   bool, void*, void** monitor) {
  if (!monitor) return E_INVALIDARG;
  *monitor = nullptr;
  HRESULT hr = check_package_and_selectors(packageIdentifier, count, selectors);
  if (FAILED(hr)) return hr;
  *monitor = new_monitor();
  return S_OK;
}

HRESULT CALLBACK monitor_result_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  switch (op) {
    case task::AsyncOp::DoWork:
      task::AsyncComplete(d->async, static_cast<HRESULT>(reinterpret_cast<intptr_t>(d->context)), sizeof(void*));
      break;
    case task::AsyncOp::GetResult: {
      void* m = new_monitor();
      std::memcpy(d->buffer, &m, sizeof(m));
      break;
    }
    default:
      break;
  }
  return S_OK;
}

HRESULT XPackageInstallChunksAsync_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors, uint32_t,
                                        bool, task::AsyncBlock* async) {
  if (!async || !packageIdentifier) return E_INVALIDARG;
  HRESULT outcome = check_package_and_selectors(packageIdentifier, count, selectors);
  HRESULT hr = task::AsyncBegin(async, reinterpret_cast<void*>(static_cast<intptr_t>(outcome)),
                                reinterpret_cast<const void*>(&XPackageInstallChunksAsync_impl), "XPackageInstallChunksAsync",
                                monitor_result_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(async, 0);
}

HRESULT XPackageInstallChunksResult_impl(void*, task::AsyncBlock* async, void** monitor) {
  if (!monitor) return E_INVALIDARG;
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XPackageInstallChunksAsync_impl), sizeof(void*), monitor, nullptr);
}

HRESULT XPackageEstimateDownloadSize_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors,
                                          uint64_t* downloadSize, bool* shouldPresentUserConfirmation) {
  if (!downloadSize) return E_INVALIDARG;
  HRESULT hr = check_package_and_selectors(packageIdentifier, count, selectors);
  if (FAILED(hr)) return hr;
  *downloadSize = 0;
  if (shouldPresentUserConfirmation) *shouldPresentUserConfirmation = false;
  return S_OK;
}

bool XPackageUnregisterPackageInstalled_impl(void*, task::RegistrationToken token, bool) {
  Locked lock;
  return g_installed_tokens.erase(token.token) != 0;
}

bool live_mount_path(void* handle, std::string* path) {
  Locked lock;
  auto it = g_mounts.find(static_cast<Mount*>(handle));
  if (it == g_mounts.end()) return false;
  *path = (*it)->path;
  return true;
}

HRESULT XPackageGetMountPathSize_impl(void*, void* mount, size_t* pathSize) {
  if (!pathSize) return E_INVALIDARG;
  std::string path;
  if (!live_mount_path(mount, &path)) return E_INVALIDARG;
  *pathSize = path.size() + 1;
  return S_OK;
}

HRESULT XPackageGetMountPath_impl(void*, void* mount, size_t pathSize, char* buffer) {
  std::string path;
  if (!live_mount_path(mount, &path)) return E_INVALIDARG;
  return copy_string(path, pathSize, buffer);
}

void XPackageCloseMountHandle_impl(void*, void* handle) {
  auto* m = static_cast<Mount*>(handle);
  {
    Locked lock;
    if (!g_mounts.erase(m)) return;
  }
  handles::closed(m);
  delete m;
}

HRESULT XPackageEnumeratePackages_impl(void*, Kind kind, Scope scope, void* context, EnumerationCallback callback) {
  if (!callback || static_cast<uint32_t>(kind) > static_cast<uint32_t>(Kind::PublisherContent) ||
      static_cast<uint32_t>(scope) > static_cast<uint32_t>(Scope::ThisPublisher))
    return E_INVALIDARG;
  if (kind != Kind::Game) return S_OK;
  Details d = package_details();
  callback(context, &d);
  return S_OK;
}

HRESULT XPackageRegisterPackageInstalled_impl(void*, void*, void*, InstalledCallback callback, task::RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  Locked lock;
  token->token = g_next_token++;
  g_installed_tokens.insert(token->token);
  return S_OK;
}

uint64_t process_bytes_written() {
  IO_COUNTERS io{};
  return GetProcessIoCounters(GetCurrentProcess(), &io) ? io.WriteTransferCount : 0;
}

HRESULT XPackageGetWriteStats_impl(void*, WriteStats* stats) {
  if (!stats) return E_INVALIDARG;
  static std::mutex m;
  static bool timed = false;
  static uint64_t interval_start = 0;
  static uint64_t baseline = 0;
  std::lock_guard<std::mutex> lock(m);
  uint64_t written = process_bytes_written();
  uint64_t now = GetTickCount64();
  if (!timed && written - baseline >= kWriteBudgetBytes) {
    timed = true;
    interval_start = now;
    baseline = written;
  }
  if (timed && now - interval_start >= kWriteIntervalMs) {
    interval_start = now - (now - interval_start) % kWriteIntervalMs;
    baseline = written;
  }
  stats->interval = kWriteIntervalMs;
  stats->budget = kWriteBudgetBytes;
  stats->elapsed = timed ? now - interval_start : 0;
  stats->bytesWritten = written - baseline;
  return S_OK;
}

HRESULT XPackageUninstallUWPInstance_impl(void*, const char*) { return E_NOTIMPL; }

HRESULT XPackageEnumerateFeatures_impl(void*, const char* packageIdentifier, void*, FeatureCallback callback) {
  if (!callback) return E_INVALIDARG;
  return resolve_package(packageIdentifier);
}

bool XPackageUninstallPackage_impl(void*, const char*) { return false; }

HRESULT CALLBACK mount_result_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  switch (op) {
    case task::AsyncOp::DoWork:
      task::AsyncComplete(d->async, static_cast<HRESULT>(reinterpret_cast<intptr_t>(d->context)), sizeof(void*));
      break;
    case task::AsyncOp::GetResult: {
      void* m = new_mount(identity().directoryUtf8);
      std::memcpy(d->buffer, &m, sizeof(m));
      break;
    }
    default:
      break;
  }
  return S_OK;
}

HRESULT XPackageMountWithUiAsync_impl(void*, const char* packageIdentifier, task::AsyncBlock* async) {
  if (!async || !packageIdentifier) return E_INVALIDARG;
  HRESULT outcome = resolve_package(packageIdentifier);
  HRESULT hr = task::AsyncBegin(async, reinterpret_cast<void*>(static_cast<intptr_t>(outcome)),
                                reinterpret_cast<const void*>(&XPackageMountWithUiAsync_impl), "XPackageMountWithUiAsync",
                                mount_result_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(async, 0);
}

HRESULT XPackageMountWithUiResult_impl(void*, task::AsyncBlock* async, void** mount) {
  if (!mount) return E_INVALIDARG;
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XPackageMountWithUiAsync_impl), sizeof(void*), mount, nullptr);
}

HRESULT XPackageUninstallChunks_impl(void*, const char* packageIdentifier, uint32_t count, const Selector* selectors) {
  HRESULT hr = resolve_package(packageIdentifier);
  if (FAILED(hr)) return hr;
  if (!count || !selectors) return E_INVALIDARG;
  for (uint32_t i = 0; i < count; ++i)
    if (selectors[i].type == SelectorType::Chunk) return E_INVALIDARG;
  return check_selectors(count, selectors);
}

HRESULT XPersistentLocalStorageMountForPackage_impl(void*, const char* packageIdentifier, void** mount) {
  if (!mount) return E_INVALIDARG;
  *mount = nullptr;
  if (!packageIdentifier) return E_INVALIDARG;
  std::string path;
  if (FAILED(resolve_package(packageIdentifier)) || !storage::directory(&path)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  *mount = new_mount(path);
  return S_OK;
}

HRESULT XPackageGetKind_impl(void*, const char* packageIdentifier, Kind* kind) {
  if (!kind) return E_INVALIDARG;
  HRESULT hr = resolve_package(packageIdentifier);
  if (FAILED(hr)) return hr;
  *kind = Kind::Game;
  return S_OK;
}

const Override kOverrides[] = {
    {"XPackageGetCurrentProcessPackageIdentifier", reinterpret_cast<void*>(&XPackageGetCurrentProcessPackageIdentifier_impl)},
    {"XPackageIsPackagedProcess", reinterpret_cast<void*>(&XPackageIsPackagedProcess_impl)},
    {"XPackageCreateInstallationMonitor", reinterpret_cast<void*>(&XPackageCreateInstallationMonitor_impl)},
    {"XPackageCloseInstallationMonitorHandle", reinterpret_cast<void*>(&XPackageCloseInstallationMonitorHandle_impl)},
    {"XPackageGetInstallationProgress", reinterpret_cast<void*>(&XPackageGetInstallationProgress_impl)},
    {"XPackageUpdateInstallationMonitor", reinterpret_cast<void*>(&XPackageUpdateInstallationMonitor_impl)},
    {"XPackageRegisterInstallationProgressChanged", reinterpret_cast<void*>(&XPackageRegisterInstallationProgressChanged_impl)},
    {"XPackageUnregisterInstallationProgressChanged", reinterpret_cast<void*>(&XPackageUnregisterInstallationProgressChanged_impl)},
    {"XPackageGetUserLocale", reinterpret_cast<void*>(&XPackageGetUserLocale_impl)},
    {"XPackageFindChunkAvailability", reinterpret_cast<void*>(&XPackageFindChunkAvailability_impl)},
    {"XPackageEnumerateChunkAvailability", reinterpret_cast<void*>(&XPackageEnumerateChunkAvailability_impl)},
    {"XPackageChangeChunkInstallOrder", reinterpret_cast<void*>(&XPackageChangeChunkInstallOrder_impl)},
    {"XPackageInstallChunks", reinterpret_cast<void*>(&XPackageInstallChunks_impl)},
    {"XPackageInstallChunksAsync", reinterpret_cast<void*>(&XPackageInstallChunksAsync_impl)},
    {"XPackageInstallChunksResult", reinterpret_cast<void*>(&XPackageInstallChunksResult_impl)},
    {"XPackageEstimateDownloadSize", reinterpret_cast<void*>(&XPackageEstimateDownloadSize_impl)},
    {"XPackageUnregisterPackageInstalled", reinterpret_cast<void*>(&XPackageUnregisterPackageInstalled_impl)},
    {"XPackageGetMountPathSize", reinterpret_cast<void*>(&XPackageGetMountPathSize_impl)},
    {"XPackageGetMountPath", reinterpret_cast<void*>(&XPackageGetMountPath_impl)},
    {"XPackageCloseMountHandle", reinterpret_cast<void*>(&XPackageCloseMountHandle_impl)},
    {"XPackageEnumeratePackages", reinterpret_cast<void*>(&XPackageEnumeratePackages_impl)},
    {"XPackageRegisterPackageInstalled", reinterpret_cast<void*>(&XPackageRegisterPackageInstalled_impl)},
    {"XPackageGetWriteStats", reinterpret_cast<void*>(&XPackageGetWriteStats_impl)},
    {"XPackageUninstallUWPInstance", reinterpret_cast<void*>(&XPackageUninstallUWPInstance_impl)},
    {"XPackageEnumerateFeatures", reinterpret_cast<void*>(&XPackageEnumerateFeatures_impl)},
    {"XPackageUninstallPackage", reinterpret_cast<void*>(&XPackageUninstallPackage_impl)},
    {"XPackageMountWithUiAsync", reinterpret_cast<void*>(&XPackageMountWithUiAsync_impl)},
    {"XPackageMountWithUiResult", reinterpret_cast<void*>(&XPackageMountWithUiResult_impl)},
    {"XPackageGetKind", reinterpret_cast<void*>(&XPackageGetKind_impl)},
    {"XPackageUninstallChunks", reinterpret_cast<void*>(&XPackageUninstallChunks_impl)},
    {"XPersistentLocalStorageMountForPackage", reinterpret_cast<void*>(&XPersistentLocalStorageMountForPackage_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

const std::string& current_package_identifier() { return identity().packageId; }

const std::string& current_store_id() { return identity().storeId; }

}  // namespace gtx::xgr::package
