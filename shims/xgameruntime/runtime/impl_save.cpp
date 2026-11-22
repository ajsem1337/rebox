#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xgr_internal.h"
#include "xtask.h"
#include "handles.h"

namespace gtx::xgr::user { uint64_t id_of(void* handle); }

namespace gtx::xgr::save {
namespace {

constexpr HRESULT kGsInvalidContainerName = static_cast<HRESULT>(0x80830001);
constexpr HRESULT kGsOutOfLocalStorage = static_cast<HRESULT>(0x80830003);
constexpr HRESULT kGsUpdateTooBig = static_cast<HRESULT>(0x80830005);
constexpr HRESULT kGsQuotaExceeded = static_cast<HRESULT>(0x80830006);
constexpr HRESULT kGsProvidedBufferTooSmall = static_cast<HRESULT>(0x80830007);
constexpr HRESULT kGsBlobNotFound = static_cast<HRESULT>(0x80830008);
constexpr HRESULT kGsNoServiceConfiguration = static_cast<HRESULT>(0x80830009);
constexpr HRESULT kGsHandleExpired = static_cast<HRESULT>(0x8083000D);
constexpr HRESULT kGsProviderMismatch = static_cast<HRESULT>(0x8083000F);

constexpr size_t kMaxContainerNameSize = 256;
constexpr size_t kMaxContainerDisplayNameSize = 128;
constexpr size_t kMaxBlobNameSize = 65;
constexpr size_t kMaxConfigurationIdSize = 65;
constexpr uint64_t kMaxUpdateBytes = 16ull * 1024 * 1024;
constexpr int64_t kQuotaBytes = 256ll * 1024 * 1024;

struct BlobInfo {
  const char* name;
  uint32_t size;
};
struct Blob {
  BlobInfo info;
  uint8_t* data;
};
struct ContainerInfo {
  const char* name;
  const char* displayName;
  uint32_t blobCount;
  uint64_t totalSize;
  int64_t lastModifiedTime;
  uint8_t needsSync;
};
static_assert(sizeof(Blob) == 24, "XGameSaveBlob layout");
static_assert(sizeof(ContainerInfo) == 48, "XGameSaveContainerInfo layout");

using BlobInfoCallback = uint8_t(CALLBACK*)(const BlobInfo* info, void* context);
using ContainerInfoCallback = uint8_t(CALLBACK*)(const ContainerInfo* info, void* context);

struct StoredBlob {
  uint32_t size = 0;
  uint64_t file = 0;
};

struct StoredContainer {
  std::string name;
  std::string displayName;
  int64_t modified = 0;
  std::wstring dir;
  std::map<std::string, StoredBlob> blobs;
  uint64_t total() const {
    uint64_t sum = 0;
    for (const auto& b : blobs) sum += b.second.size;
    return sum;
  }
};

struct Store {
  std::wstring base;
  std::wstring saveDir;
  std::shared_mutex lock;
  std::map<std::string, StoredContainer> containers;
  uint64_t used() const {
    uint64_t sum = 0;
    for (const auto& c : containers) sum += c.second.total();
    return sum;
  }
};

enum class Mode { None, Containers, Files };

std::mutex g_storesLock;
std::map<std::wstring, std::weak_ptr<Store>> g_stores;
std::map<std::wstring, Mode> g_modes;

HRESULT from_win32(DWORD error) {
  if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) return kGsOutOfLocalStorage;
  return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
HRESULT last_error() { return from_win32(GetLastError()); }

bool is_name_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

bool valid_container_name(const char* name) {
  if (!name) return false;
  size_t length = strnlen(name, kMaxContainerNameSize);
  if (length == 0 || length >= kMaxContainerNameSize) return false;
  const char* lastSlash = std::strrchr(name, '/');
  const char* fileName = lastSlash ? lastSlash + 1 : name;
  for (const char* p = name; p < fileName; ++p)
    if (!is_name_char(*p) && *p != '/') return false;
  if (!*fileName) return false;
  for (const char* p = fileName; *p; ++p) {
    if (!is_name_char(*p) && *p != '.' && *p != '-') return false;
    if (p[0] == '.' && p[1] == '.') return false;
  }
  return name[length - 1] != '.';
}

bool valid_text(const char* text, size_t maxSize, bool allowEmpty) {
  if (!text) return false;
  size_t length = strnlen(text, maxSize);
  if (length >= maxSize || (!allowEmpty && length == 0)) return false;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool valid_blob_name(const char* name) { return valid_text(name, kMaxBlobNameSize, false); }

bool configuration_folder(const char* configurationId, std::wstring* out) {
  size_t length = strnlen(configurationId, kMaxConfigurationIdSize);
  if (length == 0 || length >= kMaxConfigurationIdSize) return false;
  out->clear();
  for (size_t i = 0; i < length; i++) {
    char c = configurationId[i];
    if (!is_name_char(c) && c != '-') return false;
    out->push_back(static_cast<wchar_t>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return true;
}

std::wstring environment(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (!n) return {};
  std::wstring value(n, L'\0');
  n = GetEnvironmentVariableW(name, value.data(), n);
  value.resize(n);
  return value;
}

HRESULT storage_root(std::wstring* out) {
  std::wstring root = environment(L"XGR_SAVE_ROOT");
  if (root.empty()) {
    std::wstring local = environment(L"LOCALAPPDATA");
    if (local.empty()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    root = local + L"\\xgr\\saves";
  }
  DWORD n = GetFullPathNameW(root.c_str(), 0, nullptr, nullptr);
  if (!n) return last_error();
  std::wstring full(n, L'\0');
  n = GetFullPathNameW(root.c_str(), n, full.data(), nullptr);
  if (!n) return last_error();
  full.resize(n);
  while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
  *out = full;
  return S_OK;
}

HRESULT user_folder(void* userHandle, const char* configurationId, std::wstring* out) {
  std::wstring scid;
  if (!configuration_folder(configurationId, &scid)) return kGsNoServiceConfiguration;
  std::wstring root;
  HRESULT hr = storage_root(&root);
  if (FAILED(hr)) return hr;
  *out = root + L"\\" + std::to_wstring(user::id_of(userHandle)) + L"\\" + scid;
  return S_OK;
}

bool is_directory(const std::wstring& path) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

HRESULT ensure_directory(const std::wstring& path) {
  for (size_t i = 3; i <= path.size(); i++) {
    if (i < path.size() && path[i] != L'\\') continue;
    std::wstring part = path.substr(0, i);
    if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return last_error();
  }
  return is_directory(path) ? S_OK : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
}

uint64_t random_id() {
  static std::mutex lock;
  static std::mt19937_64 engine = [] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device(), static_cast<unsigned>(GetTickCount64()), static_cast<unsigned>(GetCurrentProcessId())};
    return std::mt19937_64(seed);
  }();
  std::lock_guard<std::mutex> guard(lock);
  return engine();
}

std::wstring hex_id(uint64_t id) {
  wchar_t text[17];
  swprintf_s(text, L"%016llx", static_cast<unsigned long long>(id));
  return text;
}

bool parse_hex_id(const std::wstring& text, uint64_t* out) {
  if (text.size() != 16) return false;
  uint64_t value = 0;
  for (wchar_t c : text) {
    int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
    if (digit < 0) return false;
    value = value << 4 | static_cast<uint64_t>(digit);
  }
  *out = value;
  return true;
}

std::wstring blob_path(const std::wstring& dir, uint64_t file) { return dir + L"\\" + hex_id(file) + L".blob"; }
std::wstring manifest_path(const std::wstring& dir) { return dir + L"\\manifest"; }
std::wstring manifest_temp_path(const std::wstring& dir) { return dir + L"\\manifest.tmp"; }

HRESULT write_durable(const std::wstring& path, const void* data, size_t size, DWORD disposition) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return last_error();
  HRESULT hr = S_OK;
  const uint8_t* cursor = static_cast<const uint8_t*>(data);
  while (size && SUCCEEDED(hr)) {
    DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 30));
    DWORD written = 0;
    if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) hr = last_error();
    cursor += written;
    size -= written;
  }
  if (SUCCEEDED(hr) && !FlushFileBuffers(file)) hr = last_error();
  CloseHandle(file);
  if (FAILED(hr)) DeleteFileW(path.c_str());
  return hr;
}

HRESULT read_exact(const std::wstring& path, uint8_t* out, uint64_t size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return last_error();
  HRESULT hr = S_OK;
  LARGE_INTEGER actual{};
  if (!GetFileSizeEx(file, &actual)) hr = last_error();
  else if (static_cast<uint64_t>(actual.QuadPart) != size) hr = HRESULT_FROM_WIN32(ERROR_FILE_CORRUPT);
  while (SUCCEEDED(hr) && size) {
    DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 1u << 30));
    DWORD got = 0;
    if (!ReadFile(file, out, chunk, &got, nullptr) || got != chunk) hr = got == chunk ? last_error() : HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
    out += got;
    size -= got;
  }
  CloseHandle(file);
  return hr;
}

bool read_small_file(const std::wstring& path, std::string* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < (64 << 20);
  if (ok) {
    out->assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
  }
  CloseHandle(file);
  return ok;
}

constexpr char kManifestMagic[8] = {'X', 'G', 'R', 'S', 'A', 'V', 'E', '1'};

void put_u32(std::string* out, uint32_t v) { out->append(reinterpret_cast<const char*>(&v), sizeof(v)); }
void put_u64(std::string* out, uint64_t v) { out->append(reinterpret_cast<const char*>(&v), sizeof(v)); }
void put_text(std::string* out, const std::string& text) {
  put_u32(out, static_cast<uint32_t>(text.size()));
  out->append(text);
}

std::string encode_manifest(const StoredContainer& c) {
  std::string out(kManifestMagic, sizeof(kManifestMagic));
  put_text(&out, c.name);
  put_text(&out, c.displayName);
  put_u64(&out, static_cast<uint64_t>(c.modified));
  put_u32(&out, static_cast<uint32_t>(c.blobs.size()));
  for (const auto& b : c.blobs) {
    put_text(&out, b.first);
    put_u32(&out, b.second.size);
    put_u64(&out, b.second.file);
  }
  return out;
}

struct Reader {
  const std::string& data;
  size_t at = 0;
  bool ok = true;
  template <class T> T get() {
    T v{};
    if (!ok || data.size() - at < sizeof(T)) {
      ok = false;
      return v;
    }
    std::memcpy(&v, data.data() + at, sizeof(T));
    at += sizeof(T);
    return v;
  }
  std::string text(size_t maxSize) {
    uint32_t n = get<uint32_t>();
    if (!ok || n >= maxSize || data.size() - at < n) {
      ok = false;
      return {};
    }
    std::string s = data.substr(at, n);
    at += n;
    return s;
  }
};

bool decode_manifest(const std::string& data, StoredContainer* c) {
  if (data.size() < sizeof(kManifestMagic) || std::memcmp(data.data(), kManifestMagic, sizeof(kManifestMagic))) return false;
  Reader r{data, sizeof(kManifestMagic)};
  c->name = r.text(kMaxContainerNameSize);
  c->displayName = r.text(kMaxContainerDisplayNameSize);
  c->modified = static_cast<int64_t>(r.get<uint64_t>());
  uint32_t count = r.get<uint32_t>();
  if (!r.ok || !valid_container_name(c->name.c_str()) || c->name.size() != std::strlen(c->name.c_str())) return false;
  for (uint32_t i = 0; i < count && r.ok; i++) {
    std::string name = r.text(kMaxBlobNameSize);
    StoredBlob b;
    b.size = r.get<uint32_t>();
    b.file = r.get<uint64_t>();
    if (!r.ok || !valid_blob_name(name.c_str()) || name.size() != std::strlen(name.c_str()) || b.size > kMaxUpdateBytes) return false;
    c->blobs[name] = b;
  }
  return r.ok && r.at == data.size();
}

uint64_t file_size(const WIN32_FIND_DATAW& fd) { return (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; }

template <class F> void for_each_entry(const std::wstring& dir, F&& visit) {
  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
    visit(fd);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
}

bool is_own_blob_file(const WIN32_FIND_DATAW& fd, uint64_t* id) {
  std::wstring name = fd.cFileName;
  if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
  if (name.size() != 21 || name.compare(16, 5, L".blob")) return false;
  return parse_hex_id(name.substr(0, 16), id);
}

void remove_container_files(const std::wstring& dir, const StoredContainer* keep) {
  DeleteFileW(manifest_temp_path(dir).c_str());
  for_each_entry(dir, [&](const WIN32_FIND_DATAW& fd) {
    uint64_t id = 0;
    if (!is_own_blob_file(fd, &id)) return;
    if (keep) {
      for (const auto& b : keep->blobs)
        if (b.second.file == id) return;
    }
    DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
  });
  if (!keep) RemoveDirectoryW(dir.c_str());
}

void load_store(Store* s) {
  for_each_entry(s->saveDir, [&](const WIN32_FIND_DATAW& fd) {
    uint64_t id = 0;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    if (!parse_hex_id(fd.cFileName, &id)) return;
    std::wstring dir = s->saveDir + L"\\" + fd.cFileName;
    std::string bytes;
    if (!read_small_file(manifest_path(dir), &bytes)) {
      if (GetFileAttributesW(manifest_path(dir).c_str()) == INVALID_FILE_ATTRIBUTES) remove_container_files(dir, nullptr);
      return;
    }
    StoredContainer c;
    if (!decode_manifest(bytes, &c) || s->containers.count(c.name)) return;
    for (const auto& b : c.blobs) {
      WIN32_FILE_ATTRIBUTE_DATA info;
      if (!GetFileAttributesExW(blob_path(dir, b.second.file).c_str(), GetFileExInfoStandard, &info)) return;
      if (((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != b.second.size) return;
    }
    c.dir = dir;
    remove_container_files(dir, &c);
    s->containers[c.name] = std::move(c);
  });
}

HRESULT claim_mode(const std::wstring& base, Mode mode) {
  std::lock_guard<std::mutex> guard(g_storesLock);
  Mode& current = g_modes[base];
  if (current != Mode::None && current != mode) return kGsProviderMismatch;
  current = mode;
  return S_OK;
}

HRESULT open_store(void* userHandle, const char* configurationId, std::shared_ptr<Store>* out) {
  std::wstring base;
  HRESULT hr = user_folder(userHandle, configurationId, &base);
  if (FAILED(hr)) return hr;
  hr = claim_mode(base, Mode::Containers);
  if (FAILED(hr)) return hr;
  std::lock_guard<std::mutex> guard(g_storesLock);
  std::shared_ptr<Store> s = g_stores[base].lock();
  if (!s) {
    s = std::make_shared<Store>();
    s->base = base;
    s->saveDir = base + L"\\xgamesave";
    hr = ensure_directory(s->saveDir);
    if (FAILED(hr)) return hr;
    load_store(s.get());
    g_stores[base] = s;
  }
  *out = s;
  return S_OK;
}

enum class Kind { Provider, Container, Update };

struct ProviderObject {
  std::shared_ptr<Store> store;
};
struct ContainerObject {
  std::shared_ptr<Store> store;
  std::string name;
};
using BlobOps = std::map<std::string, std::optional<std::vector<uint8_t>>>;
struct UpdateObject {
  std::shared_ptr<Store> store;
  std::string container;
  std::string displayName;
  std::mutex lock;
  BlobOps ops;
  uint64_t bytes = 0;
  bool submitted = false;
};

std::mutex g_handlesLock;
std::unordered_map<const void*, std::pair<Kind, std::shared_ptr<void>>> g_handles;

handles::Type handle_type(Kind kind) {
  switch (kind) {
    case Kind::Provider:
      return handles::Type::GameSaveProvider;
    case Kind::Container:
      return handles::Type::GameSaveContainer;
    default:
      return handles::Type::GameSaveUpdate;
  }
}

template <class T> void* publish(Kind kind, std::shared_ptr<T> object) {
  void* key = object.get();
  {
    std::lock_guard<std::mutex> guard(g_handlesLock);
    g_handles[key] = {kind, std::move(object)};
  }
  handles::opened(key, handle_type(kind));
  return key;
}

template <class T> std::shared_ptr<T> lookup(Kind kind, const void* handle) {
  if (!handle) return nullptr;
  std::lock_guard<std::mutex> guard(g_handlesLock);
  auto it = g_handles.find(handle);
  if (it == g_handles.end() || it->second.first != kind) return nullptr;
  return std::static_pointer_cast<T>(it->second.second);
}

void retire(Kind kind, const void* handle) {
  std::shared_ptr<void> object;
  {
    std::lock_guard<std::mutex> guard(g_handlesLock);
    auto it = g_handles.find(handle);
    if (it == g_handles.end() || it->second.first != kind) return;
    object = std::move(it->second.second);
    g_handles.erase(it);
  }
  handles::closed(handle);
}

HRESULT commit_update(Store& s, const std::string& name, const std::string& displayName, const BlobOps& ops, uint64_t bytes) {
  if (bytes > kMaxUpdateBytes) return kGsUpdateTooBig;
  std::unique_lock<std::shared_mutex> guard(s.lock);
  auto existing = s.containers.find(name);
  bool created = existing == s.containers.end();
  StoredContainer next;
  if (!created) next = existing->second;
  next.name = name;
  next.displayName = displayName;
  for (const auto& op : ops) {
    if (op.second) next.blobs[op.first] = StoredBlob{static_cast<uint32_t>(op.second->size()), 0};
    else next.blobs.erase(op.first);
  }
  uint64_t before = s.used() - (created ? 0 : existing->second.total());
  if (before + next.total() > static_cast<uint64_t>(kQuotaBytes)) return kGsQuotaExceeded;
  if (created) {
    HRESULT hr = ensure_directory(s.saveDir);
    if (FAILED(hr)) return hr;
    for (int attempt = 0;; attempt++) {
      next.dir = s.saveDir + L"\\" + hex_id(random_id());
      if (CreateDirectoryW(next.dir.c_str(), nullptr)) break;
      if (GetLastError() != ERROR_ALREADY_EXISTS || attempt == 8) return last_error();
    }
  }
  std::vector<std::wstring> written;
  auto rollback = [&](HRESULT hr) {
    for (const auto& path : written) DeleteFileW(path.c_str());
    DeleteFileW(manifest_temp_path(next.dir).c_str());
    if (created) RemoveDirectoryW(next.dir.c_str());
    return hr;
  };
  for (const auto& op : ops) {
    if (!op.second) continue;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 8; attempt++) {
      uint64_t id = random_id();
      std::wstring path = blob_path(next.dir, id);
      hr = write_durable(path, op.second->data(), op.second->size(), CREATE_NEW);
      if (SUCCEEDED(hr)) {
        written.push_back(path);
        next.blobs[op.first].file = id;
        break;
      }
      if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS)) break;
    }
    if (FAILED(hr)) return rollback(hr);
  }
  next.modified = static_cast<int64_t>(std::time(nullptr));
  std::string manifest = encode_manifest(next);
  HRESULT hr = write_durable(manifest_temp_path(next.dir), manifest.data(), manifest.size(), CREATE_ALWAYS);
  if (FAILED(hr)) return rollback(hr);
  if (!MoveFileExW(manifest_temp_path(next.dir).c_str(), manifest_path(next.dir).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return rollback(last_error());
  if (!created) {
    for (const auto& old : existing->second.blobs) {
      auto kept = next.blobs.find(old.first);
      if (kept == next.blobs.end() || kept->second.file != old.second.file) DeleteFileW(blob_path(next.dir, old.second.file).c_str());
    }
  }
  s.containers[name] = std::move(next);
  return S_OK;
}

HRESULT delete_container(Store& s, const std::string& name) {
  std::unique_lock<std::shared_mutex> guard(s.lock);
  auto it = s.containers.find(name);
  if (it == s.containers.end()) return S_OK;
  if (!DeleteFileW(manifest_path(it->second.dir).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return last_error();
  for (const auto& b : it->second.blobs) DeleteFileW(blob_path(it->second.dir, b.second.file).c_str());
  remove_container_files(it->second.dir, nullptr);
  s.containers.erase(it);
  return S_OK;
}

int64_t remaining_quota(Store& s) {
  std::shared_lock<std::shared_mutex> guard(s.lock);
  int64_t left = kQuotaBytes - static_cast<int64_t>(s.used());
  return left < 0 ? 0 : left;
}

struct ContainerSummary {
  std::string name;
  std::string displayName;
  uint32_t blobCount;
  uint64_t totalSize;
  int64_t modified;
};

std::vector<ContainerSummary> summarize(Store& s, const char* prefix, const char* exact) {
  std::vector<ContainerSummary> out;
  std::shared_lock<std::shared_mutex> guard(s.lock);
  size_t prefixLength = prefix ? std::strlen(prefix) : 0;
  for (const auto& entry : s.containers) {
    const StoredContainer& c = entry.second;
    if (exact && c.name != exact) continue;
    if (prefixLength && c.name.compare(0, prefixLength, prefix) != 0) continue;
    out.push_back({c.name, c.displayName, static_cast<uint32_t>(c.blobs.size()), c.total(), c.modified});
  }
  return out;
}

HRESULT report_containers(const std::vector<ContainerSummary>& list, void* context, ContainerInfoCallback callback) {
  for (const auto& c : list) {
    ContainerInfo info{c.name.c_str(), c.displayName.c_str(), c.blobCount, c.totalSize, c.modified, 0};
    if (!callback(&info, context)) break;
  }
  return S_OK;
}

struct BlobSummary {
  std::string name;
  uint32_t size;
};

std::vector<BlobSummary> summarize_blobs(Store& s, const std::string& container, const char* prefix) {
  std::vector<BlobSummary> out;
  std::shared_lock<std::shared_mutex> guard(s.lock);
  auto it = s.containers.find(container);
  if (it == s.containers.end()) return out;
  size_t prefixLength = prefix ? std::strlen(prefix) : 0;
  for (const auto& b : it->second.blobs) {
    if (prefixLength && b.first.compare(0, prefixLength, prefix) != 0) continue;
    out.push_back({b.first, b.second.size});
  }
  return out;
}

struct PendingBlob {
  std::string name;
  uint32_t size = 0;
  std::wstring path;
  std::vector<uint8_t> data;
};

HRESULT collect_blobs(const Store& s, const std::string& container, const std::vector<std::string>* names, std::vector<PendingBlob>* out) {
  auto it = s.containers.find(container);
  if (!names) {
    if (it == s.containers.end()) return S_OK;
    for (const auto& b : it->second.blobs) out->push_back({b.first, b.second.size, blob_path(it->second.dir, b.second.file), {}});
    return S_OK;
  }
  for (const auto& name : *names) {
    if (it == s.containers.end()) return kGsBlobNotFound;
    auto b = it->second.blobs.find(name);
    if (b == it->second.blobs.end()) return kGsBlobNotFound;
    out->push_back({name, b->second.size, blob_path(it->second.dir, b->second.file), {}});
  }
  return S_OK;
}

size_t result_bytes(const std::vector<PendingBlob>& blobs) {
  size_t total = blobs.size() * sizeof(Blob);
  for (const auto& b : blobs) total += b.name.size() + 1 + b.size;
  return total;
}

std::vector<uint8_t*> place_blobs(const std::vector<PendingBlob>& blobs, void* buffer) {
  auto* headers = static_cast<Blob*>(buffer);
  auto* cursor = reinterpret_cast<uint8_t*>(headers + blobs.size());
  for (size_t i = 0; i < blobs.size(); i++) {
    std::memcpy(cursor, blobs[i].name.c_str(), blobs[i].name.size() + 1);
    headers[i].info.name = reinterpret_cast<const char*>(cursor);
    headers[i].info.size = blobs[i].size;
    cursor += blobs[i].name.size() + 1;
  }
  std::vector<uint8_t*> slots;
  for (size_t i = 0; i < blobs.size(); i++) {
    headers[i].data = cursor;
    slots.push_back(cursor);
    cursor += blobs[i].size;
  }
  return slots;
}

HRESULT copy_names(const char** blobNames, uint32_t count, std::optional<std::vector<std::string>>* out) {
  if (!blobNames) return S_OK;
  std::vector<std::string> names;
  for (uint32_t i = 0; i < count; i++) {
    if (!blobNames[i]) return E_INVALIDARG;
    names.emplace_back(blobNames[i]);
  }
  *out = std::move(names);
  return S_OK;
}

struct AsyncJob {
  virtual ~AsyncJob() = default;
  virtual HRESULT work(size_t* resultSize) = 0;
  virtual void deliver(void*) {}
};

HRESULT CALLBACK job_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  auto* job = static_cast<AsyncJob*>(d->context);
  switch (op) {
    case task::AsyncOp::Begin:
      return task::AsyncSchedule(d->async, 0);
    case task::AsyncOp::DoWork: {
      size_t size = 0;
      HRESULT hr = job->work(&size);
      task::AsyncComplete(d->async, hr, SUCCEEDED(hr) ? size : 0);
      return S_OK;
    }
    case task::AsyncOp::GetResult:
      job->deliver(d->buffer);
      return S_OK;
    case task::AsyncOp::Cleanup:
      delete job;
      return S_OK;
    default:
      return S_OK;
  }
}

HRESULT start_job(task::AsyncBlock* async, AsyncJob* job, const void* identity, const char* name) {
  if (!async) {
    delete job;
    return E_INVALIDARG;
  }
  HRESULT hr = task::AsyncBegin(async, job, identity, name, job_provider);
  if (FAILED(hr)) delete job;
  return hr;
}

HRESULT result_with_payload(task::AsyncBlock* async, const void* identity, size_t size, void* buffer) {
  size_t needed = 0;
  HRESULT hr = task::AsyncGetResultSize(async, &needed);
  if (FAILED(hr)) return hr;
  if (needed && (size < needed || !buffer)) return kGsProvidedBufferTooSmall;
  return task::AsyncGetResult(async, identity, size, buffer, nullptr);
}

HRESULT XGameSaveInitializeProvider_impl(void*, void* user, const char* configurationId, uint8_t, void** provider) {
  if (!provider) return E_INVALIDARG;
  *provider = nullptr;
  if (!user || !configurationId) return E_INVALIDARG;
  auto object = std::make_shared<ProviderObject>();
  HRESULT hr = open_store(user, configurationId, &object->store);
  if (FAILED(hr)) return hr;
  *provider = publish(Kind::Provider, object);
  return S_OK;
}

struct InitializeJob : AsyncJob {
  void* user;
  std::string configurationId;
  void* provider = nullptr;
  bool delivered = false;
  ~InitializeJob() override {
    if (provider && !delivered) retire(Kind::Provider, provider);
  }
  HRESULT work(size_t* size) override {
    HRESULT hr = XGameSaveInitializeProvider_impl(nullptr, user, configurationId.c_str(), 0, &provider);
    *size = sizeof(void*);
    return hr;
  }
  void deliver(void* buffer) override {
    std::memcpy(buffer, &provider, sizeof(provider));
    delivered = true;
  }
};

HRESULT XGameSaveInitializeProviderAsync_impl(void*, void* user, const char* configurationId, uint8_t, task::AsyncBlock* async) {
  if (!user || !configurationId || !async) return E_INVALIDARG;
  auto* job = new InitializeJob();
  job->user = user;
  job->configurationId = configurationId;
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync_impl), "XGameSaveInitializeProviderAsync");
}

HRESULT XGameSaveInitializeProviderResult_impl(void*, task::AsyncBlock* async, void** provider) {
  if (!provider) return E_INVALIDARG;
  *provider = nullptr;
  return result_with_payload(async, reinterpret_cast<const void*>(&XGameSaveInitializeProviderAsync_impl), sizeof(void*), provider);
}

void XGameSaveCloseProvider_impl(void*, void* provider) { retire(Kind::Provider, provider); }

HRESULT XGameSaveGetRemainingQuota_impl(void*, void* provider, int64_t* remaining) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !remaining) return E_INVALIDARG;
  *remaining = remaining_quota(*p->store);
  return S_OK;
}

struct QuotaJob : AsyncJob {
  std::shared_ptr<Store> store;
  int64_t remaining = 0;
  HRESULT work(size_t* size) override {
    remaining = remaining_quota(*store);
    *size = sizeof(remaining);
    return S_OK;
  }
  void deliver(void* buffer) override { std::memcpy(buffer, &remaining, sizeof(remaining)); }
};

HRESULT XGameSaveGetRemainingQuotaAsync_impl(void*, void* provider, task::AsyncBlock* async) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !async) return E_INVALIDARG;
  auto* job = new QuotaJob();
  job->store = p->store;
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync_impl), "XGameSaveGetRemainingQuotaAsync");
}

HRESULT XGameSaveGetRemainingQuotaResult_impl(void*, task::AsyncBlock* async, int64_t* remaining) {
  if (!remaining) return E_INVALIDARG;
  return result_with_payload(async, reinterpret_cast<const void*>(&XGameSaveGetRemainingQuotaAsync_impl), sizeof(int64_t), remaining);
}

HRESULT XGameSaveDeleteContainer_impl(void*, void* provider, const char* containerName) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !containerName) return E_INVALIDARG;
  if (!valid_container_name(containerName)) return kGsInvalidContainerName;
  return delete_container(*p->store, containerName);
}

struct DeleteJob : AsyncJob {
  std::shared_ptr<Store> store;
  std::string name;
  HRESULT work(size_t* size) override {
    *size = 0;
    return delete_container(*store, name);
  }
};

HRESULT XGameSaveDeleteContainerAsync_impl(void*, void* provider, const char* containerName, task::AsyncBlock* async) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !containerName || !async) return E_INVALIDARG;
  if (!valid_container_name(containerName)) return kGsInvalidContainerName;
  auto* job = new DeleteJob();
  job->store = p->store;
  job->name = containerName;
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync_impl), "XGameSaveDeleteContainerAsync");
}

HRESULT XGameSaveDeleteContainerResult_impl(void*, task::AsyncBlock* async) {
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveDeleteContainerAsync_impl), 0, nullptr, nullptr);
}

HRESULT XGameSaveGetContainerInfo_impl(void*, void* provider, const char* containerName, void* context, ContainerInfoCallback callback) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !containerName || !callback) return E_INVALIDARG;
  if (!valid_container_name(containerName)) return kGsInvalidContainerName;
  return report_containers(summarize(*p->store, nullptr, containerName), context, callback);
}

HRESULT XGameSaveEnumerateContainerInfoByName_impl(void*, void* provider, const char* prefix, void* context, ContainerInfoCallback callback) {
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !callback) return E_INVALIDARG;
  return report_containers(summarize(*p->store, prefix, nullptr), context, callback);
}

HRESULT XGameSaveEnumerateContainerInfo_impl(void* self, void* provider, void* context, ContainerInfoCallback callback) {
  return XGameSaveEnumerateContainerInfoByName_impl(self, provider, nullptr, context, callback);
}

HRESULT XGameSaveCreateContainer_impl(void*, void* provider, const char* containerName, void** container) {
  if (!container) return E_INVALIDARG;
  *container = nullptr;
  auto p = lookup<ProviderObject>(Kind::Provider, provider);
  if (!p || !containerName) return E_INVALIDARG;
  if (!valid_container_name(containerName)) return kGsInvalidContainerName;
  auto object = std::make_shared<ContainerObject>();
  object->store = p->store;
  object->name = containerName;
  *container = publish(Kind::Container, object);
  return S_OK;
}

void XGameSaveCloseContainer_impl(void*, void* container) { retire(Kind::Container, container); }

HRESULT XGameSaveEnumerateBlobInfoByName_impl(void*, void* container, const char* prefix, void* context, BlobInfoCallback callback) {
  auto c = lookup<ContainerObject>(Kind::Container, container);
  if (!c || !callback) return E_INVALIDARG;
  for (const auto& b : summarize_blobs(*c->store, c->name, prefix)) {
    BlobInfo info{b.name.c_str(), b.size};
    if (!callback(&info, context)) break;
  }
  return S_OK;
}

HRESULT XGameSaveEnumerateBlobInfo_impl(void* self, void* container, void* context, BlobInfoCallback callback) {
  return XGameSaveEnumerateBlobInfoByName_impl(self, container, nullptr, context, callback);
}

HRESULT XGameSaveReadBlobData_impl(void*, void* container, const char** blobNames, uint32_t* countOfBlobs, size_t blobsSize, Blob* blobData) {
  auto c = lookup<ContainerObject>(Kind::Container, container);
  if (!c || !countOfBlobs) return E_INVALIDARG;
  std::optional<std::vector<std::string>> names;
  HRESULT hr = copy_names(blobNames, *countOfBlobs, &names);
  if (FAILED(hr)) return hr;
  std::shared_lock<std::shared_mutex> guard(c->store->lock);
  std::vector<PendingBlob> blobs;
  hr = collect_blobs(*c->store, c->name, names ? &*names : nullptr, &blobs);
  if (FAILED(hr)) return hr;
  size_t needed = result_bytes(blobs);
  if (needed && (blobsSize < needed || !blobData)) return kGsProvidedBufferTooSmall;
  std::vector<uint8_t*> slots = needed ? place_blobs(blobs, blobData) : std::vector<uint8_t*>();
  for (size_t i = 0; i < blobs.size(); i++) {
    hr = read_exact(blobs[i].path, slots[i], blobs[i].size);
    if (FAILED(hr)) return hr;
  }
  *countOfBlobs = static_cast<uint32_t>(blobs.size());
  return S_OK;
}

thread_local uint32_t t_deliveredBlobCount = 0;

struct ReadJob : AsyncJob {
  std::shared_ptr<Store> store;
  std::string container;
  std::optional<std::vector<std::string>> names;
  std::vector<PendingBlob> blobs;
  HRESULT work(size_t* size) override {
    std::shared_lock<std::shared_mutex> guard(store->lock);
    HRESULT hr = collect_blobs(*store, container, names ? &*names : nullptr, &blobs);
    for (size_t i = 0; SUCCEEDED(hr) && i < blobs.size(); i++) {
      blobs[i].data.resize(blobs[i].size);
      hr = read_exact(blobs[i].path, blobs[i].data.data(), blobs[i].size);
    }
    *size = result_bytes(blobs);
    return hr;
  }
  void deliver(void* buffer) override {
    std::vector<uint8_t*> slots = place_blobs(blobs, buffer);
    for (size_t i = 0; i < blobs.size(); i++)
      if (blobs[i].size) std::memcpy(slots[i], blobs[i].data.data(), blobs[i].size);
    t_deliveredBlobCount = static_cast<uint32_t>(blobs.size());
  }
};

HRESULT XGameSaveReadBlobDataAsync_impl(void*, void* container, const char** blobNames, uint32_t countOfBlobs, task::AsyncBlock* async) {
  auto c = lookup<ContainerObject>(Kind::Container, container);
  if (!c || !async) return E_INVALIDARG;
  auto* job = new ReadJob();
  job->store = c->store;
  job->container = c->name;
  HRESULT hr = copy_names(blobNames, countOfBlobs, &job->names);
  if (FAILED(hr)) {
    delete job;
    return hr;
  }
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync_impl), "XGameSaveReadBlobDataAsync");
}

HRESULT XGameSaveReadBlobDataResult_impl(void*, task::AsyncBlock* async, size_t blobsSize, Blob* blobData, uint32_t* countOfBlobs) {
  if (!countOfBlobs) return E_INVALIDARG;
  t_deliveredBlobCount = 0;
  HRESULT hr = result_with_payload(async, reinterpret_cast<const void*>(&XGameSaveReadBlobDataAsync_impl), blobsSize, blobData);
  if (SUCCEEDED(hr)) *countOfBlobs = t_deliveredBlobCount;
  return hr;
}

HRESULT XGameSaveCreateUpdate_impl(void*, void* container, const char* displayName, void** update) {
  if (!update) return E_INVALIDARG;
  *update = nullptr;
  auto c = lookup<ContainerObject>(Kind::Container, container);
  if (!c || !valid_text(displayName, kMaxContainerDisplayNameSize, true)) return E_INVALIDARG;
  auto object = std::make_shared<UpdateObject>();
  object->store = c->store;
  object->container = c->name;
  object->displayName = displayName;
  *update = publish(Kind::Update, object);
  return S_OK;
}

void XGameSaveCloseUpdate_impl(void*, void* update) { retire(Kind::Update, update); }

HRESULT add_operation(void* update, const char* blobName, std::optional<std::vector<uint8_t>> data) {
  auto u = lookup<UpdateObject>(Kind::Update, update);
  if (!u || !valid_blob_name(blobName)) return E_INVALIDARG;
  std::lock_guard<std::mutex> guard(u->lock);
  if (u->submitted) return kGsHandleExpired;
  if (u->ops.count(blobName)) return E_INVALIDARG;
  if (data) u->bytes += data->size();
  u->ops.emplace(blobName, std::move(data));
  return S_OK;
}

HRESULT XGameSaveSubmitBlobWrite_impl(void*, void* update, const char* blobName, const uint8_t* data, size_t byteCount) {
  if (byteCount && !data) return E_INVALIDARG;
  if (byteCount > kMaxUpdateBytes) return kGsUpdateTooBig;
  return add_operation(update, blobName, std::vector<uint8_t>(data, data + byteCount));
}

HRESULT XGameSaveSubmitBlobDelete_impl(void*, void* update, const char* blobName) { return add_operation(update, blobName, std::nullopt); }

struct SubmitJob : AsyncJob {
  std::shared_ptr<Store> store;
  std::string container;
  std::string displayName;
  BlobOps ops;
  uint64_t bytes = 0;
  HRESULT work(size_t* size) override {
    *size = 0;
    return commit_update(*store, container, displayName, ops, bytes);
  }
};

HRESULT take_update(void* update, SubmitJob* job) {
  auto u = lookup<UpdateObject>(Kind::Update, update);
  if (!u) return E_INVALIDARG;
  std::lock_guard<std::mutex> guard(u->lock);
  if (u->submitted) return kGsHandleExpired;
  u->submitted = true;
  job->store = u->store;
  job->container = u->container;
  job->displayName = u->displayName;
  job->ops = std::move(u->ops);
  job->bytes = u->bytes;
  return S_OK;
}

HRESULT XGameSaveSubmitUpdate_impl(void*, void* update) {
  SubmitJob job;
  HRESULT hr = take_update(update, &job);
  if (FAILED(hr)) return hr;
  size_t unused = 0;
  return job.work(&unused);
}

HRESULT XGameSaveSubmitUpdateAsync_impl(void*, void* update, task::AsyncBlock* async) {
  if (!async) return E_INVALIDARG;
  auto* job = new SubmitJob();
  HRESULT hr = take_update(update, job);
  if (FAILED(hr)) {
    delete job;
    return hr;
  }
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync_impl), "XGameSaveSubmitUpdateAsync");
}

HRESULT XGameSaveSubmitUpdateResult_impl(void*, task::AsyncBlock* async) {
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XGameSaveSubmitUpdateAsync_impl), 0, nullptr, nullptr);
}

std::string to_utf8(const std::wstring& text) {
  int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

struct FolderJob : AsyncJob {
  void* user;
  std::string configurationId;
  std::string folder;
  HRESULT work(size_t* size) override {
    std::wstring base;
    HRESULT hr = user_folder(user, configurationId.c_str(), &base);
    if (SUCCEEDED(hr)) hr = claim_mode(base, Mode::Files);
    if (SUCCEEDED(hr)) hr = ensure_directory(base + L"\\files");
    if (FAILED(hr)) return hr;
    folder = to_utf8(base + L"\\files");
    *size = folder.size() + 1;
    return S_OK;
  }
  void deliver(void* buffer) override { std::memcpy(buffer, folder.c_str(), folder.size() + 1); }
};

HRESULT XGameSaveFilesGetFolderWithUiAsync_impl(void*, void* user, const char* configurationId, task::AsyncBlock* async) {
  if (!user || !configurationId || !async) return E_INVALIDARG;
  auto* job = new FolderJob();
  job->user = user;
  job->configurationId = configurationId;
  return start_job(async, job, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync_impl), "XGameSaveFilesGetFolderWithUiAsync");
}

HRESULT XGameSaveFilesGetFolderWithUiResult_impl(void*, task::AsyncBlock* async, size_t folderSize, char* folderResult) {
  return result_with_payload(async, reinterpret_cast<const void*>(&XGameSaveFilesGetFolderWithUiAsync_impl), folderSize, folderResult);
}

uint64_t folder_bytes(const std::wstring& dir) {
  uint64_t total = 0;
  for_each_entry(dir, [&](const WIN32_FIND_DATAW& fd) {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += folder_bytes(dir + L"\\" + fd.cFileName);
    else total += file_size(fd);
  });
  return total;
}

HRESULT XGameSaveFilesGetRemainingQuota_impl(void*, void* user, const char* configurationId, int64_t* remaining) {
  if (!user || !configurationId || !remaining) return E_INVALIDARG;
  std::wstring base;
  HRESULT hr = user_folder(user, configurationId, &base);
  if (FAILED(hr)) return hr;
  int64_t left = kQuotaBytes - static_cast<int64_t>(folder_bytes(base + L"\\files"));
  *remaining = left < 0 ? 0 : left;
  return S_OK;
}

const Override kOverrides[] = {
    {"XGameSaveInitializeProvider", reinterpret_cast<void*>(&XGameSaveInitializeProvider_impl)},
    {"XGameSaveInitializeProviderAsync", reinterpret_cast<void*>(&XGameSaveInitializeProviderAsync_impl)},
    {"XGameSaveInitializeProviderResult", reinterpret_cast<void*>(&XGameSaveInitializeProviderResult_impl)},
    {"XGameSaveCloseProvider", reinterpret_cast<void*>(&XGameSaveCloseProvider_impl)},
    {"XGameSaveGetRemainingQuota", reinterpret_cast<void*>(&XGameSaveGetRemainingQuota_impl)},
    {"XGameSaveGetRemainingQuotaAsync", reinterpret_cast<void*>(&XGameSaveGetRemainingQuotaAsync_impl)},
    {"XGameSaveGetRemainingQuotaResult", reinterpret_cast<void*>(&XGameSaveGetRemainingQuotaResult_impl)},
    {"XGameSaveDeleteContainer", reinterpret_cast<void*>(&XGameSaveDeleteContainer_impl)},
    {"XGameSaveDeleteContainerAsync", reinterpret_cast<void*>(&XGameSaveDeleteContainerAsync_impl)},
    {"XGameSaveDeleteContainerResult", reinterpret_cast<void*>(&XGameSaveDeleteContainerResult_impl)},
    {"XGameSaveGetContainerInfo", reinterpret_cast<void*>(&XGameSaveGetContainerInfo_impl)},
    {"XGameSaveEnumerateContainerInfo", reinterpret_cast<void*>(&XGameSaveEnumerateContainerInfo_impl)},
    {"XGameSaveEnumerateContainerInfoByName", reinterpret_cast<void*>(&XGameSaveEnumerateContainerInfoByName_impl)},
    {"XGameSaveCreateContainer", reinterpret_cast<void*>(&XGameSaveCreateContainer_impl)},
    {"XGameSaveCloseContainer", reinterpret_cast<void*>(&XGameSaveCloseContainer_impl)},
    {"XGameSaveEnumerateBlobInfo", reinterpret_cast<void*>(&XGameSaveEnumerateBlobInfo_impl)},
    {"XGameSaveEnumerateBlobInfoByName", reinterpret_cast<void*>(&XGameSaveEnumerateBlobInfoByName_impl)},
    {"XGameSaveReadBlobData", reinterpret_cast<void*>(&XGameSaveReadBlobData_impl)},
    {"XGameSaveReadBlobDataAsync", reinterpret_cast<void*>(&XGameSaveReadBlobDataAsync_impl)},
    {"XGameSaveReadBlobDataResult", reinterpret_cast<void*>(&XGameSaveReadBlobDataResult_impl)},
    {"XGameSaveCreateUpdate", reinterpret_cast<void*>(&XGameSaveCreateUpdate_impl)},
    {"XGameSaveCloseUpdate", reinterpret_cast<void*>(&XGameSaveCloseUpdate_impl)},
    {"XGameSaveSubmitBlobWrite", reinterpret_cast<void*>(&XGameSaveSubmitBlobWrite_impl)},
    {"XGameSaveSubmitBlobDelete", reinterpret_cast<void*>(&XGameSaveSubmitBlobDelete_impl)},
    {"XGameSaveSubmitUpdate", reinterpret_cast<void*>(&XGameSaveSubmitUpdate_impl)},
    {"XGameSaveSubmitUpdateAsync", reinterpret_cast<void*>(&XGameSaveSubmitUpdateAsync_impl)},
    {"XGameSaveSubmitUpdateResult", reinterpret_cast<void*>(&XGameSaveSubmitUpdateResult_impl)},
    {"XGameSaveFilesGetFolderWithUiAsync", reinterpret_cast<void*>(&XGameSaveFilesGetFolderWithUiAsync_impl)},
    {"XGameSaveFilesGetFolderWithUiResult", reinterpret_cast<void*>(&XGameSaveFilesGetFolderWithUiResult_impl)},
    {"XGameSaveFilesGetRemainingQuota", reinterpret_cast<void*>(&XGameSaveFilesGetRemainingQuota_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::save
