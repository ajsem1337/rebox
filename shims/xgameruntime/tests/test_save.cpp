#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <thread>
#include <vector>

typedef HRESULT (*QueryFn)(const GUID&, const GUID&, void**);
struct AsyncBlock {
  void* queue;
  void* context;
  void* callback;
  unsigned char internal[32];
};
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
using BlobCallback = uint8_t (*)(const BlobInfo*, void*);
using ContainerCallback = uint8_t (*)(const ContainerInfo*, void*);

constexpr HRESULT kInvalidContainerName = static_cast<HRESULT>(0x80830001);
constexpr HRESULT kUpdateTooBig = static_cast<HRESULT>(0x80830005);
constexpr HRESULT kQuotaExceeded = static_cast<HRESULT>(0x80830006);
constexpr HRESULT kBufferTooSmall = static_cast<HRESULT>(0x80830007);
constexpr HRESULT kBlobNotFound = static_cast<HRESULT>(0x80830008);
constexpr HRESULT kNoServiceConfiguration = static_cast<HRESULT>(0x80830009);
constexpr HRESULT kHandleExpired = static_cast<HRESULT>(0x8083000D);
constexpr HRESULT kProviderMismatch = static_cast<HRESULT>(0x8083000F);
constexpr int64_t kQuota = 256ll * 1024 * 1024;
constexpr size_t kMaxBlob = 16u * 1024 * 1024;

const char* kScid = "00000000-0000-0000-0000-0000600dcafe";
const char* kFilesScid = "00000000-0000-0000-0000-00000000f11e";

std::atomic<int> g_failures{0};

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_failures++;
}
void check_hr(HRESULT got, HRESULT want, const char* what) {
  if (got != want) std::printf("  got %08lx want %08lx\n", static_cast<unsigned long>(got), static_cast<unsigned long>(want));
  check(got == want, what);
}

struct Iface {
  void* object = nullptr;
  void** vtable = nullptr;
  template <class R, class... A> R call(int slot, A... args) { return reinterpret_cast<R (*)(void*, A...)>(vtable[slot])(object, args...); }
};

Iface g_save, g_async, g_user;
void* g_userHandle = nullptr;

namespace slot {
constexpr int InitializeProvider = 3, InitializeProviderAsync = 4, InitializeProviderResult = 5, CloseProvider = 6;
constexpr int GetRemainingQuota = 7, GetRemainingQuotaAsync = 8, GetRemainingQuotaResult = 9;
constexpr int DeleteContainer = 10, DeleteContainerAsync = 11, DeleteContainerResult = 12;
constexpr int GetContainerInfo = 13, EnumerateContainerInfo = 14, EnumerateContainerInfoByName = 15;
constexpr int CreateContainer = 16, CloseContainer = 17, EnumerateBlobInfo = 18, EnumerateBlobInfoByName = 19;
constexpr int ReadBlobData = 20, ReadBlobDataAsync = 21, ReadBlobDataResult = 22;
constexpr int CreateUpdate = 23, CloseUpdate = 24, SubmitBlobWrite = 25, SubmitBlobDelete = 26;
constexpr int SubmitUpdate = 27, SubmitUpdateAsync = 28, SubmitUpdateResult = 29;
constexpr int FilesGetFolderWithUiAsync = 30, FilesGetFolderWithUiResult = 31, FilesGetRemainingQuota = 32;
constexpr int AsyncGetStatus = 3, AsyncGetResultSize = 4;
constexpr int UserCloseHandle = 4, UserAddAsync = 7, UserAddResult = 8, UserGetId = 11;
}

HRESULT wait(AsyncBlock* a) { return g_async.call<HRESULT>(slot::AsyncGetStatus, a, static_cast<BOOLEAN>(TRUE)); }

HRESULT init_provider(const char* scid, void** provider) {
  return g_save.call<HRESULT>(slot::InitializeProvider, g_userHandle, scid, static_cast<uint8_t>(0), provider);
}
void close_provider(void* p) { g_save.call<void>(slot::CloseProvider, p); }
HRESULT create_container(void* provider, const char* name, void** container) {
  return g_save.call<HRESULT>(slot::CreateContainer, provider, name, container);
}
void close_container(void* c) { g_save.call<void>(slot::CloseContainer, c); }
HRESULT create_update(void* container, const char* display, void** update) {
  return g_save.call<HRESULT>(slot::CreateUpdate, container, display, update);
}
void close_update(void* u) { g_save.call<void>(slot::CloseUpdate, u); }
HRESULT write_blob(void* update, const char* name, const std::string& data) {
  return g_save.call<HRESULT>(slot::SubmitBlobWrite, update, name, reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
HRESULT delete_blob(void* update, const char* name) { return g_save.call<HRESULT>(slot::SubmitBlobDelete, update, name); }
HRESULT submit(void* update) { return g_save.call<HRESULT>(slot::SubmitUpdate, update); }
HRESULT quota(void* provider, int64_t* left) { return g_save.call<HRESULT>(slot::GetRemainingQuota, provider, left); }

HRESULT save_blobs(void* provider, const char* container, const char* display, const std::vector<std::pair<std::string, std::string>>& blobs,
                   const std::vector<std::string>& deletes = {}) {
  void* c = nullptr;
  void* u = nullptr;
  HRESULT hr = create_container(provider, container, &c);
  if (SUCCEEDED(hr)) hr = create_update(c, display, &u);
  for (const auto& b : blobs)
    if (SUCCEEDED(hr)) hr = write_blob(u, b.first.c_str(), b.second);
  for (const auto& d : deletes)
    if (SUCCEEDED(hr)) hr = delete_blob(u, d.c_str());
  if (SUCCEEDED(hr)) hr = submit(u);
  if (u) close_update(u);
  if (c) close_container(c);
  return hr;
}

struct ReadResult {
  HRESULT hr = E_FAIL;
  std::vector<std::pair<std::string, std::string>> blobs;
};

size_t doc_size(const std::vector<std::pair<std::string, size_t>>& blobs) {
  size_t total = 0;
  for (const auto& b : blobs) total += b.first.size() + 1 + b.second + sizeof(Blob);
  return total;
}

ReadResult read_sync(void* container, std::vector<const char*> names, size_t size, bool all = false) {
  ReadResult r;
  std::vector<uint8_t> buffer(size ? size : 1);
  uint32_t count = static_cast<uint32_t>(names.size());
  r.hr = g_save.call<HRESULT>(slot::ReadBlobData, container, all ? static_cast<const char**>(nullptr) : names.data(), &count, size,
                              reinterpret_cast<Blob*>(buffer.data()));
  if (SUCCEEDED(r.hr)) {
    auto* blobs = reinterpret_cast<Blob*>(buffer.data());
    for (uint32_t i = 0; i < count; i++)
      r.blobs.push_back({blobs[i].info.name, std::string(reinterpret_cast<char*>(blobs[i].data), blobs[i].info.size)});
  }
  return r;
}

struct Listing {
  std::vector<std::string> names;
  std::vector<ContainerInfo> infos;
  std::vector<std::string> displays;
  size_t stopAfter = 0;
};
uint8_t collect_container(const ContainerInfo* info, void* context) {
  auto* l = static_cast<Listing*>(context);
  l->names.push_back(info->name);
  l->displays.push_back(info->displayName);
  l->infos.push_back(*info);
  return l->stopAfter == 0 || l->names.size() < l->stopAfter;
}
Listing list_containers(void* provider, const char* prefix, size_t stopAfter = 0, HRESULT* hr = nullptr) {
  Listing l;
  l.stopAfter = stopAfter;
  HRESULT r = prefix ? g_save.call<HRESULT>(slot::EnumerateContainerInfoByName, provider, prefix, static_cast<void*>(&l), static_cast<ContainerCallback>(&collect_container))
                     : g_save.call<HRESULT>(slot::EnumerateContainerInfo, provider, static_cast<void*>(&l), static_cast<ContainerCallback>(&collect_container));
  if (hr) *hr = r;
  return l;
}

struct BlobListing {
  std::vector<std::string> names;
  std::vector<uint32_t> sizes;
};
uint8_t collect_blob(const BlobInfo* info, void* context) {
  auto* l = static_cast<BlobListing*>(context);
  l->names.push_back(info->name);
  l->sizes.push_back(info->size);
  return 1;
}
BlobListing list_blobs(void* container, const char* prefix, HRESULT* hr = nullptr) {
  BlobListing l;
  HRESULT r = prefix ? g_save.call<HRESULT>(slot::EnumerateBlobInfoByName, container, prefix, static_cast<void*>(&l), static_cast<BlobCallback>(&collect_blob))
                     : g_save.call<HRESULT>(slot::EnumerateBlobInfo, container, static_cast<void*>(&l), static_cast<BlobCallback>(&collect_blob));
  if (hr) *hr = r;
  return l;
}

std::wstring g_root;

std::wstring exe_dir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  *wcsrchr(path, L'\\') = 0;
  return path;
}

void remove_tree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      std::wstring child = dir + L"\\" + fd.cFileName;
      if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(child);
      else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child.c_str());
      else DeleteFileW(child.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

void walk(const std::wstring& dir, std::vector<std::wstring>* out) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    std::wstring child = dir + L"\\" + fd.cFileName;
    out->push_back(child);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) walk(child, out);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::set<std::wstring> subdirs(const std::wstring& dir) {
  std::set<std::wstring> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
      out.insert(dir + L"\\" + fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

std::wstring save_dir(const char* scid) {
  uint64_t id = 0;
  g_user.call<HRESULT>(slot::UserGetId, g_userHandle, &id);
  std::wstring s;
  for (const char* p = scid; *p; ++p) s.push_back(static_cast<wchar_t>(*p));
  return g_root + L"\\" + std::to_wstring(id) + L"\\" + s;
}

std::string pattern(size_t size, char seed) {
  std::string s(size, '\0');
  for (size_t i = 0; i < size; i++) s[i] = static_cast<char>(seed + i * 7);
  return s;
}

const std::string kPersistData = pattern(4096, 'p');

void test_provider_arguments() {
  void* p = reinterpret_cast<void*>(1);
  check_hr(g_save.call<HRESULT>(slot::InitializeProvider, g_userHandle, kScid, static_cast<uint8_t>(0), static_cast<void**>(nullptr)), E_INVALIDARG, "InitializeProvider null out");
  check_hr(init_provider(nullptr, &p), E_INVALIDARG, "InitializeProvider null scid");
  check(p == nullptr, "InitializeProvider nulls out on failure");
  check_hr(g_save.call<HRESULT>(slot::InitializeProvider, static_cast<void*>(nullptr), kScid, static_cast<uint8_t>(0), &p), E_INVALIDARG, "InitializeProvider null user");
  check_hr(init_provider("..\\..\\evil", &p), kNoServiceConfiguration, "InitializeProvider rejects path-like scid");
  check_hr(init_provider("", &p), kNoServiceConfiguration, "InitializeProvider rejects empty scid");
  check_hr(init_provider("C:x", &p), kNoServiceConfiguration, "InitializeProvider rejects drive scid");
}

void test_names(void* provider) {
  const char* bad[] = {"", "a/../b", "..", "a..b", "a.", "dir\\file", "C:file", "a/b/", "x\x01y", "sp ace", "a/b.c/d", "/"};
  bool allRejected = true;
  for (const char* name : bad) {
    void* c = reinterpret_cast<void*>(1);
    HRESULT hr = create_container(provider, name, &c);
    if (hr != kInvalidContainerName || c != nullptr) {
      std::printf("  name '%s' -> %08lx\n", name, static_cast<unsigned long>(hr));
      allRejected = false;
    }
  }
  check(allRejected, "CreateContainer rejects invalid names with E_GS_INVALID_CONTAINER_NAME");
  std::string longName(255, 'a');
  std::string tooLong(256, 'a');
  void* c = nullptr;
  check_hr(create_container(provider, longName.c_str(), &c), S_OK, "CreateContainer accepts 255-character name");
  close_container(c);
  check_hr(create_container(provider, tooLong.c_str(), &c), kInvalidContainerName, "CreateContainer rejects 256-character name");
  check_hr(create_container(provider, "dir/sub_2/save-1.dat", &c), S_OK, "CreateContainer accepts path-style name");
  close_container(c);
  check_hr(g_save.call<HRESULT>(slot::DeleteContainer, provider, "../x"), kInvalidContainerName, "DeleteContainer rejects invalid name");
  Listing l;
  check_hr(g_save.call<HRESULT>(slot::GetContainerInfo, provider, "a..b", static_cast<void*>(&l), static_cast<ContainerCallback>(&collect_container)),
           kInvalidContainerName, "GetContainerInfo rejects invalid name");
}

void test_basic(void* provider) {
  int64_t left = 0;
  check_hr(quota(provider, &left), S_OK, "GetRemainingQuota");
  check(left == kQuota, "fresh quota is 256 MB");
  check(list_containers(provider, nullptr).names.empty(), "fresh provider has no containers");

  void* c = nullptr;
  check_hr(create_container(provider, "slot1", &c), S_OK, "CreateContainer slot1");
  check(list_containers(provider, nullptr).names.empty(), "unsubmitted container is not enumerated");
  Listing info;
  check_hr(g_save.call<HRESULT>(slot::GetContainerInfo, provider, "slot1", static_cast<void*>(&info), static_cast<ContainerCallback>(&collect_container)), S_OK,
           "GetContainerInfo on unsubmitted container");
  check(info.names.empty(), "GetContainerInfo reports nothing for unsubmitted container");
  BlobListing empty = list_blobs(c, nullptr);
  check(empty.names.empty(), "unsubmitted container has no blobs");

  void* u = nullptr;
  check_hr(create_update(c, nullptr, &u), E_INVALIDARG, "CreateUpdate null display name");
  check_hr(create_update(c, std::string(128, 'd').c_str(), &u), E_INVALIDARG, "CreateUpdate display name too long");
  check_hr(create_update(c, "Slot One", &u), S_OK, "CreateUpdate");
  std::string world = pattern(1000, 'w'), player = pattern(300, 'q');
  check_hr(write_blob(u, "WorldState", world), S_OK, "SubmitBlobWrite WorldState");
  check_hr(write_blob(u, "PlayerState", player), S_OK, "SubmitBlobWrite PlayerState");
  check_hr(write_blob(u, "..\\..\\escape", "x"), S_OK, "SubmitBlobWrite path-like blob name is stored, not used as a path");
  check_hr(write_blob(u, std::string(64, 'n').c_str(), ""), S_OK, "SubmitBlobWrite 64-byte name, empty blob");
  check_hr(write_blob(u, std::string(65, 'n').c_str(), "x"), E_INVALIDARG, "SubmitBlobWrite 65-byte name rejected");
  check_hr(write_blob(u, "", "x"), E_INVALIDARG, "SubmitBlobWrite empty name rejected");
  check_hr(write_blob(u, "WorldState", "again"), E_INVALIDARG, "second modification of a blob in one update rejected");
  check_hr(delete_blob(u, "PlayerState"), E_INVALIDARG, "delete after write of the same blob rejected");
  check_hr(g_save.call<HRESULT>(slot::SubmitBlobWrite, u, "nullData", static_cast<const uint8_t*>(nullptr), static_cast<size_t>(4)), E_INVALIDARG,
           "SubmitBlobWrite null data rejected");
  check_hr(submit(u), S_OK, "SubmitUpdate");
  check_hr(submit(u), kHandleExpired, "SubmitUpdate twice rejected");
  check_hr(write_blob(u, "Late", "x"), kHandleExpired, "SubmitBlobWrite after submit rejected");
  check_hr(delete_blob(u, "Late"), kHandleExpired, "SubmitBlobDelete after submit rejected");
  close_update(u);
  check_hr(write_blob(u, "Late", "x"), E_INVALIDARG, "closed update handle rejected");

  int64_t now = static_cast<int64_t>(std::time(nullptr));
  Listing all = list_containers(provider, nullptr);
  check(all.names.size() == 1 && all.names[0] == "slot1", "submitted container is enumerated");
  if (all.infos.size() == 1) {
    check(all.displays[0] == "Slot One", "display name");
    check(all.infos[0].blobCount == 4, "blob count");
    check(all.infos[0].totalSize == world.size() + player.size() + 1, "total size");
    check(all.infos[0].lastModifiedTime >= now - 10 && all.infos[0].lastModifiedTime <= now + 10, "last modified time is now");
    check(all.infos[0].needsSync == 0, "needsSync false on a local device");
  }
  check_hr(quota(provider, &left), S_OK, "GetRemainingQuota after save");
  check(left == kQuota - static_cast<int64_t>(world.size() + player.size() + 1), "quota drops by stored bytes");

  BlobListing blobs = list_blobs(c, nullptr);
  check(blobs.names.size() == 4 && blobs.names[1] == "PlayerState" && blobs.names[2] == "WorldState", "EnumerateBlobInfo lists blobs");
  BlobListing players = list_blobs(c, "Player");
  check(players.names.size() == 1 && players.sizes[0] == player.size(), "EnumerateBlobInfoByName filters by prefix");
  check(list_blobs(c, "Nope").names.empty(), "EnumerateBlobInfoByName no match");

  size_t need = doc_size({{"WorldState", world.size()}, {"PlayerState", player.size()}});
  ReadResult r = read_sync(c, {"WorldState", "PlayerState"}, need);
  check_hr(r.hr, S_OK, "ReadBlobData with documented size formula");
  check(r.blobs.size() == 2 && r.blobs[0].first == "WorldState" && r.blobs[0].second == world && r.blobs[1].first == "PlayerState" && r.blobs[1].second == player,
        "ReadBlobData returns requested blobs in order");
  check_hr(read_sync(c, {"WorldState", "PlayerState"}, need - 1).hr, kBufferTooSmall, "ReadBlobData buffer too small");
  check_hr(read_sync(c, {"WorldState", "Missing"}, 1 << 16).hr, kBlobNotFound, "ReadBlobData missing blob");
  ReadResult every = read_sync(c, {}, 1 << 16, true);
  check(SUCCEEDED(every.hr) && every.blobs.size() == 4, "ReadBlobData with null names reads every blob");
  uint32_t count = 1;
  const char* oneName[] = {"WorldState"};
  check_hr(g_save.call<HRESULT>(slot::ReadBlobData, c, oneName, static_cast<uint32_t*>(nullptr), static_cast<size_t>(0), static_cast<Blob*>(nullptr)), E_INVALIDARG,
           "ReadBlobData null count");
  check_hr(g_save.call<HRESULT>(slot::ReadBlobData, c, oneName, &count, static_cast<size_t>(0), static_cast<Blob*>(nullptr)), kBufferTooSmall,
           "ReadBlobData null buffer");

  AsyncBlock ab{};
  check_hr(g_save.call<HRESULT>(slot::ReadBlobDataAsync, c, oneName, static_cast<uint32_t>(1), &ab), S_OK, "ReadBlobDataAsync");
  check_hr(wait(&ab), S_OK, "ReadBlobDataAsync completes");
  size_t resultSize = 0;
  g_async.call<HRESULT>(slot::AsyncGetResultSize, &ab, &resultSize);
  check(resultSize == doc_size({{"WorldState", world.size()}}), "XAsyncGetResultSize matches documented formula");
  std::vector<uint8_t> buf(resultSize);
  uint32_t got = 0;
  check_hr(g_save.call<HRESULT>(slot::ReadBlobDataResult, &ab, resultSize - 1, reinterpret_cast<Blob*>(buf.data()), &got), kBufferTooSmall,
           "ReadBlobDataResult buffer too small");
  check_hr(g_save.call<HRESULT>(slot::ReadBlobDataResult, &ab, resultSize, reinterpret_cast<Blob*>(buf.data()), &got), S_OK,
           "ReadBlobDataResult after a too-small attempt");
  auto* rb = reinterpret_cast<Blob*>(buf.data());
  check(got == 1 && !std::strcmp(rb[0].info.name, "WorldState") && std::string(reinterpret_cast<char*>(rb[0].data), rb[0].info.size) == world,
        "ReadBlobDataResult content");
  check_hr(g_save.call<HRESULT>(slot::ReadBlobDataResult, &ab, resultSize, reinterpret_cast<Blob*>(buf.data()), &got), E_ILLEGAL_METHOD_CALL,
           "ReadBlobDataResult twice");

  AsyncBlock miss{};
  const char* missing[] = {"Missing"};
  g_save.call<HRESULT>(slot::ReadBlobDataAsync, c, missing, static_cast<uint32_t>(1), &miss);
  wait(&miss);
  check_hr(g_save.call<HRESULT>(slot::ReadBlobDataResult, &miss, buf.size(), reinterpret_cast<Blob*>(buf.data()), &got), kBlobNotFound, "ReadBlobDataAsync missing blob");

  void* u2 = nullptr;
  std::string world2 = pattern(2000, 'W');
  create_update(c, "Slot One v2", &u2);
  write_blob(u2, "WorldState", world2);
  delete_blob(u2, "PlayerState");
  AsyncBlock sb{};
  check_hr(g_save.call<HRESULT>(slot::SubmitUpdateAsync, u2, &sb), S_OK, "SubmitUpdateAsync");
  AsyncBlock again{};
  check_hr(g_save.call<HRESULT>(slot::SubmitUpdateAsync, u2, &again), kHandleExpired, "SubmitUpdateAsync twice rejected");
  check_hr(submit(u2), kHandleExpired, "SubmitUpdate after SubmitUpdateAsync rejected");
  close_update(u2);
  check_hr(wait(&sb), S_OK, "SubmitUpdateAsync completes after the handle was closed");
  check_hr(g_save.call<HRESULT>(slot::SubmitUpdateResult, &sb), S_OK, "SubmitUpdateResult");
  r = read_sync(c, {"WorldState"}, 1 << 16);
  check(SUCCEEDED(r.hr) && r.blobs.size() == 1 && r.blobs[0].second == world2, "async update wrote the blob");
  check_hr(read_sync(c, {"PlayerState"}, 1 << 16).hr, kBlobNotFound, "async update deleted the blob");
  Listing v2 = list_containers(provider, "slot");
  check(v2.displays.size() == 1 && v2.displays[0] == "Slot One v2" && v2.infos[0].blobCount == 3, "display name and count updated");

  void* abandoned = nullptr;
  create_update(c, "abandoned", &abandoned);
  write_blob(abandoned, "WorldState", "junk");
  close_update(abandoned);
  r = read_sync(c, {"WorldState"}, 1 << 16);
  check(SUCCEEDED(r.hr) && r.blobs[0].second == world2, "abandoned update leaves the container unchanged");

  close_container(c);
  check(list_blobs(c, nullptr).names.empty(), "closed container handle yields nothing");
  HRESULT hr = S_OK;
  list_blobs(c, nullptr, &hr);
  check_hr(hr, E_INVALIDARG, "closed container handle rejected");
  list_blobs(provider, nullptr, &hr);
  check_hr(hr, E_INVALIDARG, "provider handle passed as container rejected");
}

void test_enumeration(void* provider) {
  check_hr(save_blobs(provider, "slot2", "Slot Two", {{"a", "1"}}), S_OK, "save slot2");
  check_hr(save_blobs(provider, "other", "Other", {{"a", "1"}}), S_OK, "save other");
  check(list_containers(provider, nullptr).names.size() == 3, "EnumerateContainerInfo lists all");
  check(list_containers(provider, "slot").names.size() == 2, "EnumerateContainerInfoByName prefix");
  check(list_containers(provider, "").names.size() == 3, "EnumerateContainerInfoByName empty prefix lists all");
  check(list_containers(provider, "zz").names.empty(), "EnumerateContainerInfoByName no match");
  check(list_containers(provider, nullptr, 1).names.size() == 1, "callback returning false stops enumeration");
  Listing one;
  check_hr(g_save.call<HRESULT>(slot::GetContainerInfo, provider, "slot2", static_cast<void*>(&one), static_cast<ContainerCallback>(&collect_container)), S_OK,
           "GetContainerInfo");
  check(one.names.size() == 1 && one.displays[0] == "Slot Two", "GetContainerInfo reports the container");
  struct Reentry {
    void* provider;
    int inner = 0;
  } re{provider};
  auto reenter = [](const ContainerInfo* info, void* ctx) -> uint8_t {
    auto* r = static_cast<Reentry*>(ctx);
    Listing l;
    g_save.call<HRESULT>(slot::GetContainerInfo, r->provider, info->name, static_cast<void*>(&l), static_cast<ContainerCallback>(&collect_container));
    r->inner += static_cast<int>(l.names.size());
    return 1;
  };
  g_save.call<HRESULT>(slot::EnumerateContainerInfo, provider, static_cast<void*>(&re), static_cast<ContainerCallback>(+reenter));
  check(re.inner == 3, "API calls from inside an enumeration callback work");
  HRESULT hr = S_OK;
  list_containers(nullptr, nullptr, 0, &hr);
  check_hr(hr, E_INVALIDARG, "EnumerateContainerInfo null provider");
  check_hr(g_save.call<HRESULT>(slot::EnumerateContainerInfo, provider, static_cast<void*>(nullptr), static_cast<ContainerCallback>(nullptr)), E_INVALIDARG,
           "EnumerateContainerInfo null callback");
}

void test_atomic_failure(void* provider) {
  std::wstring dir = save_dir(kScid) + L"\\xgamesave";
  std::set<std::wstring> before = subdirs(dir);
  check_hr(save_blobs(provider, "atomic", "Atomic", {{"A", "first-A"}, {"B", "first-B"}}), S_OK, "save atomic v1");
  std::set<std::wstring> after = subdirs(dir);
  std::wstring mine;
  for (const auto& d : after)
    if (!before.count(d)) mine = d;
  check(!mine.empty(), "atomic container got its own directory");
  std::vector<std::wstring> filesBefore;
  walk(mine, &filesBefore);
  CreateDirectoryW((mine + L"\\manifest.tmp").c_str(), nullptr);
  HRESULT hr = save_blobs(provider, "atomic", "Atomic v2", {{"A", "second-A"}, {"C", "second-C"}}, {"B"});
  check(FAILED(hr), "update fails when its manifest cannot be written");
  RemoveDirectoryW((mine + L"\\manifest.tmp").c_str());
  std::vector<std::wstring> filesAfter;
  walk(mine, &filesAfter);
  check(filesAfter.size() == filesBefore.size(), "failed update leaves no stray files");
  void* c = nullptr;
  create_container(provider, "atomic", &c);
  ReadResult r = read_sync(c, {}, 1 << 16, true);
  check(SUCCEEDED(r.hr) && r.blobs.size() == 2 && r.blobs[0].second == "first-A" && r.blobs[1].second == "first-B", "failed update leaves previous contents intact");
  Listing l = list_containers(provider, "atomic");
  check(l.displays.size() == 1 && l.displays[0] == "Atomic", "failed update leaves display name intact");

  void* u = nullptr;
  create_update(c, "big", &u);
  std::string huge(kMaxBlob + 1, 'h');
  check_hr(write_blob(u, "huge", huge), kUpdateTooBig, "SubmitBlobWrite over GS_MAX_BLOB_SIZE");
  std::string ten(10u << 20, 't');
  check_hr(write_blob(u, "ten1", ten), S_OK, "10 MB write accepted");
  check_hr(write_blob(u, "ten2", ten), S_OK, "second 10 MB write accepted");
  check_hr(submit(u), kUpdateTooBig, "update over 16 MB rejected at submit");
  close_update(u);
  r = read_sync(c, {}, 1 << 16, true);
  check(SUCCEEDED(r.hr) && r.blobs.size() == 2, "rejected big update leaves contents intact");
  create_update(c, "max", &u);
  check_hr(write_blob(u, "max", std::string(kMaxBlob, 'm')), S_OK, "exactly 16 MB write accepted");
  check_hr(submit(u), S_OK, "exactly 16 MB update submitted");
  close_update(u);
  create_update(c, "Atomic", &u);
  delete_blob(u, "max");
  check_hr(submit(u), S_OK, "delete of the 16 MB blob");
  close_update(u);
  close_container(c);
}

void test_quota(void* provider) {
  int64_t start = 0;
  quota(provider, &start);
  std::string block(kMaxBlob, 'q');
  int saved = 0;
  HRESULT last = S_OK;
  for (int i = 0; i < 17 && SUCCEEDED(last); i++) {
    std::string name = "q" + std::to_string(i);
    last = save_blobs(provider, name.c_str(), "quota", {{"fill", block}});
    if (SUCCEEDED(last)) saved++;
  }
  check(saved == static_cast<int>(start / kMaxBlob), "quota allows saves up to the remaining quota");
  check_hr(last, kQuotaExceeded, "save past the quota fails with E_GS_QUOTA_EXCEEDED");
  int64_t left = 0;
  quota(provider, &left);
  check(left == start - saved * static_cast<int64_t>(kMaxBlob), "quota accounts for every save");
  AsyncBlock qa{};
  check_hr(g_save.call<HRESULT>(slot::GetRemainingQuotaAsync, provider, &qa), S_OK, "GetRemainingQuotaAsync");
  wait(&qa);
  int64_t asyncLeft = -1;
  check_hr(g_save.call<HRESULT>(slot::GetRemainingQuotaResult, &qa, &asyncLeft), S_OK, "GetRemainingQuotaResult");
  check(asyncLeft == left, "async quota matches");
  for (int i = 0; i < saved; i++) {
    std::string name = "q" + std::to_string(i);
    if (i % 2) {
      AsyncBlock da{};
      g_save.call<HRESULT>(slot::DeleteContainerAsync, provider, name.c_str(), &da);
      wait(&da);
      last = g_save.call<HRESULT>(slot::DeleteContainerResult, &da);
    } else {
      last = g_save.call<HRESULT>(slot::DeleteContainer, provider, name.c_str());
    }
    if (FAILED(last)) break;
  }
  check_hr(last, S_OK, "DeleteContainer and DeleteContainerAsync");
  quota(provider, &left);
  check(left == start, "deleting containers returns their quota");
  check(list_containers(provider, "q").names.empty(), "deleted containers are gone");
  check_hr(g_save.call<HRESULT>(slot::DeleteContainer, provider, "neverExisted"), S_OK, "DeleteContainer of a missing container");
  AsyncBlock bad{};
  check_hr(g_save.call<HRESULT>(slot::DeleteContainerAsync, provider, "a..b", &bad), kInvalidContainerName, "DeleteContainerAsync invalid name");
  AsyncBlock wrong{};
  g_save.call<HRESULT>(slot::GetRemainingQuotaAsync, provider, &wrong);
  wait(&wrong);
  void* p = nullptr;
  check_hr(g_save.call<HRESULT>(slot::InitializeProviderResult, &wrong, &p), E_INVALIDARG, "Result of a different async call rejected");
}

void test_concurrency(void* provider) {
  std::atomic<int> torn{0}, failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; t++) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 25; i++) {
        std::string value = std::to_string(t) + ":" + std::to_string(i);
        if (FAILED(save_blobs(provider, "shared", "Shared", {{"a", value}, {"b", value}}))) failed++;
        std::string own = "own" + std::to_string(t);
        if (FAILED(save_blobs(provider, own.c_str(), "Own", {{"v", value}}))) failed++;
        void* c = nullptr;
        create_container(provider, "shared", &c);
        ReadResult r = read_sync(c, {"a", "b"}, 4096);
        if (FAILED(r.hr) || r.blobs.size() != 2 || r.blobs[0].second != r.blobs[1].second) torn++;
        close_container(c);
      }
    });
  }
  for (auto& th : threads) th.join();
  check(failed == 0, "concurrent saves all succeed");
  check(torn == 0, "concurrent readers never see a partial update");
  for (int t = 0; t < 8; t++) {
    void* c = nullptr;
    create_container(provider, ("own" + std::to_string(t)).c_str(), &c);
    ReadResult r = read_sync(c, {"v"}, 4096);
    if (FAILED(r.hr) || r.blobs[0].second != std::to_string(t) + ":24") failed++;
    close_container(c);
    g_save.call<HRESULT>(slot::DeleteContainer, provider, ("own" + std::to_string(t)).c_str());
  }
  check(failed == 0, "each thread's last save is what is stored");
}

void test_files() {
  AsyncBlock a{};
  check_hr(g_save.call<HRESULT>(slot::FilesGetFolderWithUiAsync, g_userHandle, kScid, &a), S_OK, "XGameSaveFilesGetFolderWithUiAsync");
  wait(&a);
  char folder[1024] = {};
  check_hr(g_save.call<HRESULT>(slot::FilesGetFolderWithUiResult, &a, sizeof(folder), folder), kProviderMismatch, "XGameSaveFiles after XGameSave: provider mismatch");

  AsyncBlock f{};
  g_save.call<HRESULT>(slot::FilesGetFolderWithUiAsync, g_userHandle, kFilesScid, &f);
  wait(&f);
  size_t need = 0;
  g_async.call<HRESULT>(slot::AsyncGetResultSize, &f, &need);
  check_hr(g_save.call<HRESULT>(slot::FilesGetFolderWithUiResult, &f, need - 1, folder), kBufferTooSmall, "FilesGetFolderWithUiResult buffer too small");
  check_hr(g_save.call<HRESULT>(slot::FilesGetFolderWithUiResult, &f, sizeof(folder), folder), S_OK, "FilesGetFolderWithUiResult");
  std::wstring expected = save_dir(kFilesScid) + L"\\files";
  wchar_t wide[1024] = {};
  MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1024);
  check(expected == wide, "files folder is under the save root");
  DWORD attributes = GetFileAttributesW(wide);
  check(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY), "files folder exists");
  int64_t left = 0;
  check_hr(g_save.call<HRESULT>(slot::FilesGetRemainingQuota, g_userHandle, kFilesScid, &left), S_OK, "XGameSaveFilesGetRemainingQuota");
  check(left == kQuota, "files quota starts at 256 MB");
  std::wstring sub = std::wstring(wide) + L"\\container1";
  CreateDirectoryW(sub.c_str(), nullptr);
  HANDLE h = CreateFileW((sub + L"\\f.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  std::string data(1000, 'f');
  DWORD written = 0;
  WriteFile(h, data.data(), 1000, &written, nullptr);
  CloseHandle(h);
  g_save.call<HRESULT>(slot::FilesGetRemainingQuota, g_userHandle, kFilesScid, &left);
  check(left == kQuota - 1000, "files quota counts files in the folder");
  check_hr(g_save.call<HRESULT>(slot::FilesGetRemainingQuota, g_userHandle, kFilesScid, static_cast<int64_t*>(nullptr)), E_INVALIDARG, "FilesGetRemainingQuota null out");
  check_hr(g_save.call<HRESULT>(slot::FilesGetRemainingQuota, g_userHandle, "..", &left), kNoServiceConfiguration, "FilesGetRemainingQuota bad scid");
  void* p = nullptr;
  check_hr(init_provider(kFilesScid, &p), kProviderMismatch, "XGameSave after XGameSaveFiles: provider mismatch");
}

void test_async_provider() {
  AsyncBlock a{};
  check_hr(g_save.call<HRESULT>(slot::InitializeProviderAsync, g_userHandle, kScid, static_cast<uint8_t>(0), &a), S_OK, "InitializeProviderAsync");
  check_hr(wait(&a), S_OK, "InitializeProviderAsync completes");
  void* p = nullptr;
  check_hr(g_save.call<HRESULT>(slot::InitializeProviderResult, &a, &p), S_OK, "InitializeProviderResult");
  check(p != nullptr && !list_containers(p, "slot1").names.empty(), "async provider sees the stored containers");
  close_provider(p);
  AsyncBlock bad{};
  g_save.call<HRESULT>(slot::InitializeProviderAsync, g_userHandle, "bad/scid", static_cast<uint8_t>(1), &bad);
  wait(&bad);
  check_hr(g_save.call<HRESULT>(slot::InitializeProviderResult, &bad, &p), kNoServiceConfiguration, "InitializeProviderAsync bad scid fails at result");
  check_hr(g_save.call<HRESULT>(slot::InitializeProviderAsync, g_userHandle, kScid, static_cast<uint8_t>(0), static_cast<AsyncBlock*>(nullptr)), E_INVALIDARG,
           "InitializeProviderAsync null block");
}

void test_root_confinement() {
  std::vector<std::wstring> all;
  walk(g_root, &all);
  bool clean = true;
  for (const auto& path : all) {
    std::wstring name = path.substr(path.rfind(L'\\') + 1);
    if (name.find(L"escape") != std::wstring::npos || name.find(L"evil") != std::wstring::npos || name.find(L"WorldState") != std::wstring::npos ||
        name.find(L"slot") != std::wstring::npos)
      clean = false;
  }
  check(clean, "no title-supplied name appears as a file or folder name");
  std::wstring parent = g_root.substr(0, g_root.rfind(L'\\'));
  check(GetFileAttributesW((parent + L"\\escape").c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW((parent + L"\\evil").c_str()) == INVALID_FILE_ATTRIBUTES,
        "nothing was written beside the save root");
}

void verify_persisted(void* provider, const char* phase) {
  void* c = nullptr;
  create_container(provider, "persist", &c);
  ReadResult r = read_sync(c, {"data"}, 1 << 16);
  std::string what = std::string(phase) + ": persisted blob reads back";
  check(SUCCEEDED(r.hr) && r.blobs.size() == 1 && r.blobs[0].second == kPersistData, what.c_str());
  Listing l = list_containers(provider, "persist");
  what = std::string(phase) + ": persisted container info";
  check(l.displays.size() == 1 && l.displays[0] == "Persisted" && l.infos[0].blobCount == 1, what.c_str());
  close_container(c);
  create_container(provider, "atomic", &c);
  r = read_sync(c, {}, 1 << 16, true);
  what = std::string(phase) + ": atomic container intact";
  check(SUCCEEDED(r.hr) && r.blobs.size() == 2 && r.blobs[0].second == "first-A", what.c_str());
  close_container(c);
}

int main(int argc, char** argv) {
  bool reopen = argc > 1 && !std::strcmp(argv[1], "reopen");
  g_root = exe_dir() + L"\\save_test_root";
  if (!reopen) remove_tree(g_root);
  SetEnvironmentVariableW(L"XGR_SAVE_ROOT", g_root.c_str());

  HMODULE m = LoadLibraryA("xgameruntime.dll");
  auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(m, "QueryApiImpl")));
  if (!query) {
    std::printf("FAIL load xgameruntime.dll\n");
    return 1;
  }
  GUID saveId = {0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6e, 0xe2}};
  GUID userId = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
  GUID threadId = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
  for (const auto& pair : {std::make_pair(&saveId, &g_save), std::make_pair(&userId, &g_user), std::make_pair(&threadId, &g_async)}) {
    if (FAILED(query(*pair.first, *pair.first, &pair.second->object))) {
      std::printf("FAIL QueryApiImpl\n");
      return 1;
    }
    pair.second->vtable = *static_cast<void***>(pair.second->object);
  }
  AsyncBlock add{};
  g_user.call<HRESULT>(slot::UserAddAsync, static_cast<uint32_t>(1), &add);
  wait(&add);
  g_user.call<HRESULT>(slot::UserAddResult, &add, &g_userHandle);
  check(g_userHandle != nullptr, "local user");

  void* provider = nullptr;
  if (reopen) {
    check_hr(init_provider(kScid, &provider), S_OK, "reopen: InitializeProvider in a new process");
    verify_persisted(provider, "new process");
    check_hr(g_save.call<HRESULT>(slot::DeleteContainer, provider, "persist"), S_OK, "reopen: delete persisted container");
    close_provider(provider);
  } else {
    test_provider_arguments();
    check_hr(init_provider(kScid, &provider), S_OK, "InitializeProvider");
    test_names(provider);
    test_basic(provider);
    test_enumeration(provider);
    test_atomic_failure(provider);
    test_quota(provider);
    test_concurrency(provider);
    test_async_provider();
    check_hr(save_blobs(provider, "persist", "Persisted", {{"data", kPersistData}}), S_OK, "save persisted container");
    close_provider(provider);
    int64_t left = 0;
    check_hr(quota(provider, &left), E_INVALIDARG, "closed provider rejected");
    check_hr(init_provider(kScid, &provider), S_OK, "InitializeProvider again after closing every handle");
    verify_persisted(provider, "same process");
    close_provider(provider);
    test_files();
    test_root_confinement();
  }
  g_user.call<void>(slot::UserCloseHandle, g_userHandle);
  std::printf("%s: %d failure(s)\n", reopen ? "reopen" : "main", g_failures.load());
  return g_failures ? 1 : 0;
}
