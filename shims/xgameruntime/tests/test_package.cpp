#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef HRESULT (*QueryFn)(const GUID&, const GUID&, void**);

struct AsyncBlock { void* queue; void* context; void* callback; unsigned char internal[32]; };
struct Token { uint64_t token; };
struct Selector { uint32_t type; union { const char* text; uint32_t chunkId; }; };
struct Version { uint16_t major, minor, build, revision; };
struct Details {
  const char* packageIdentifier; Version version; uint32_t kind; const char* displayName; const char* description;
  const char* publisher; const char* storeId; bool installing; uint32_t index; uint32_t count; bool ageRestricted; const char* titleId;
};
struct Progress { uint64_t totalBytes, installedBytes, launchBytes; bool launchable, completed; };
struct WriteStats { uint64_t interval, budget, elapsed, bytesWritten; };

const GUID kPackage4 = {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};
const GUID kAsync = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
const GUID kPersistentLocalStorage3 = {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}};
constexpr int kMountForPackageSlot = 8;

enum PackageSlot {
  GetCurrentProcessPackageIdentifier = 3, IsPackagedProcess, CreateInstallationMonitor, CloseInstallationMonitorHandle,
  GetInstallationProgress, UpdateInstallationMonitor, RegisterInstallationProgressChanged, UnregisterInstallationProgressChanged,
  GetUserLocale, FindChunkAvailability, EnumerateChunkAvailability, ChangeChunkInstallOrder, InstallChunks, InstallChunksAsync,
  InstallChunksResult, EstimateDownloadSize, UninstallChunks, UnregisterPackageInstalled = 22, GetMountPathSize = 24, GetMountPath,
  CloseMountHandle, EnumeratePackages = 28, RegisterPackageInstalled, GetWriteStats, UninstallUWPInstance = 32, EnumerateFeatures,
  UninstallPackage, EnumeratePackages2, RegisterPackageInstalled2, MountWithUiAsync, MountWithUiResult, EnumeratePackages3,
  RegisterPackageInstalled3, GetKind
};
static_assert(UninstallChunks == 19 && GetMountPathSize == 24 && UninstallPackage == 34 && EnumeratePackages2 == 35 &&
              MountWithUiResult == 38 && EnumeratePackages3 == 39 && GetKind == 41, "slots from table.json");

constexpr HRESULT kInvalidSelector = static_cast<HRESULT>(0x89245203L);
constexpr HRESULT kNoPackageIdentifier = static_cast<HRESULT>(0x89245208L);

int g_failures = 0;
void check(bool ok, const char* phase, const char* what) {
  std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", phase, what);
  if (!ok) ++g_failures;
}

struct Runtime {
  HMODULE module = nullptr;
  void* package = nullptr;
  void** pv = nullptr;
  void* async = nullptr;
  void** av = nullptr;

  bool load() {
    module = LoadLibraryA("xgameruntime.dll");
    if (!module) return false;
    auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(module, "QueryApiImpl")));
    if (!query || query(kPackage4, kPackage4, &package) || query(kAsync, kAsync, &async))
      return false;
    pv = *static_cast<void***>(package);
    av = *static_cast<void***>(async);
    return true;
  }
  bool unload() {
    FreeLibrary(module);
    return GetModuleHandleA("xgameruntime.dll") == nullptr;
  }
  template <class R, class... A> R pkg(int slot, A... a) { return reinterpret_cast<R (*)(void*, A...)>(pv[slot])(package, a...); }
  HRESULT wait(AsyncBlock* b) { return reinterpret_cast<HRESULT (*)(void*, AsyncBlock*, bool)>(av[3])(async, b, true); }
};

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n - 1));
  return s;
}
std::wstring g_exe_dir;
std::wstring g_config;
const char kConfigMarker[] = "<!-- xgr test_package -->";

void set_env(const wchar_t* name, const wchar_t* value) { SetEnvironmentVariableW(name, value); }
void clear_package_env() {
  for (const wchar_t* n : {L"XGR_PACKAGE_ID", L"XGR_PACKAGE_STORE_ID", L"XGR_PACKAGE_DISPLAY_NAME", L"XGR_PACKAGE_PUBLISHER",
                           L"XGR_PACKAGE_DESCRIPTION", L"XGR_PACKAGE_VERSION", L"XGR_TITLE_ID"})
    set_env(n, nullptr);
}

bool remove_own_config() {
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_config.c_str(), L"rb") != 0 || !f) return true;
  char head[64] = {};
  std::fread(head, 1, sizeof(head) - 1, f);
  std::fclose(f);
  if (!std::strstr(head, kConfigMarker)) return false;
  return DeleteFileW(g_config.c_str()) != 0;
}

bool write_config(const std::string& body) {
  if (!remove_own_config()) return false;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_config.c_str(), L"wb") != 0 || !f) return false;
  std::string text = std::string(kConfigMarker) + "\n" + body;
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  return true;
}

struct Seen { int calls = 0; Details last{}; std::string id, name, desc, pub, store, title; };
bool CALLBACK record_package(void* ctx, const Details* d) {
  auto* s = static_cast<Seen*>(ctx);
  ++s->calls;
  s->last = *d;
  s->id = d->packageIdentifier;
  s->name = d->displayName;
  s->desc = d->description;
  s->pub = d->publisher;
  s->store = d->storeId;
  s->title = d->titleId;
  return true;
}
struct SeenChunks { int calls = 0; std::vector<std::string> languages; std::vector<uint32_t> availability; };
bool CALLBACK record_chunk(void* ctx, const Selector* s, uint32_t availability) {
  auto* seen = static_cast<SeenChunks*>(ctx);
  ++seen->calls;
  if (s->type == 0) seen->languages.push_back(s->text);
  seen->availability.push_back(availability);
  return true;
}
int g_feature_calls = 0;
bool CALLBACK record_feature(void*, const void*) { return ++g_feature_calls > 0; }
int g_progress_calls = 0;
void CALLBACK record_progress(void*, void*) { ++g_progress_calls; }
int g_installed_calls = 0;
void CALLBACK record_installed(void*, const Details*) { ++g_installed_calls; }

std::string package_id(Runtime& r) {
  char id[33] = {};
  return r.pkg<HRESULT>(GetCurrentProcessPackageIdentifier, sizeof(id), id) == S_OK ? id : "";
}

Seen enumerate(Runtime& r, int slot, uint32_t kind, uint32_t scope, HRESULT* hr) {
  Seen s;
  *hr = r.pkg<HRESULT>(slot, kind, scope, static_cast<void*>(&s), &record_package);
  return s;
}

std::string mount_path(Runtime& r, void* mount, const char* phase) {
  size_t size = 0;
  check(r.pkg<HRESULT>(GetMountPathSize, mount, &size) == S_OK && size > 1, phase, "XPackageGetMountPathSize");
  std::string path(size, '\0');
  check(r.pkg<HRESULT>(GetMountPath, mount, size, path.data()) == S_OK, phase, "XPackageGetMountPath");
  check(r.pkg<HRESULT>(GetMountPath, mount, size - 1, path.data()) == E_NOT_SUFFICIENT_BUFFER, phase, "XPackageGetMountPath: short buffer -> E_NOT_SUFFICIENT_BUFFER");
  path.resize(size - 1);
  return path;
}

void phase_defaults() {
  const char* P = "defaults";
  clear_package_env();
  set_env(L"XGR_PACKAGE_ID", L"..\\evil");
  Runtime r;
  if (!r.load()) { check(false, P, "load runtime"); return; }

  char id[33] = {};
  check(r.pkg<HRESULT>(GetCurrentProcessPackageIdentifier, sizeof(id), id) == S_OK && !std::strcmp(id, "XGR.LocalPackage"), P,
        "package identifier: unsafe XGR_PACKAGE_ID rejected, synthetic default used");
  check(r.pkg<HRESULT>(GetCurrentProcessPackageIdentifier, static_cast<size_t>(5), id) == E_NOT_SUFFICIENT_BUFFER, P, "package identifier: short buffer");
  check(r.pkg<HRESULT>(GetCurrentProcessPackageIdentifier, sizeof(id), static_cast<char*>(nullptr)) == E_INVALIDARG, P, "package identifier: null buffer");
  check(r.pkg<bool>(IsPackagedProcess), P, "XPackageIsPackagedProcess");

  HRESULT hr;
  Seen s = enumerate(r, EnumeratePackages, 0, 0, &hr);
  check(hr == S_OK && s.calls == 1, P, "XPackageEnumeratePackages(Game, ThisOnly): one package");
  check(s.id == "XGR.LocalPackage" && s.name == "XGR Local Package" && s.pub == "XGR Local Publisher" && s.desc.empty() &&
            s.store == "XGRLOCAL0000" && s.title.empty(), P, "details: synthetic identity");
  check(s.last.version.major == 1 && s.last.version.minor == 0 && s.last.version.build == 0 && s.last.version.revision == 0 &&
            s.last.kind == 0 && !s.last.installing && s.last.index == 0 && s.last.count == 1 && !s.last.ageRestricted, P,
        "details: version 1.0.0.0, Game, installed, index 0 of 1");
  s = enumerate(r, EnumeratePackages, 0, 2, &hr);
  check(hr == S_OK && s.calls == 1, P, "XPackageEnumeratePackages(Game, ThisPublisher): one package");
  s = enumerate(r, EnumeratePackages, 1, 1, &hr);
  check(hr == S_OK && s.calls == 0, P, "XPackageEnumeratePackages(Content): no DLC");
  s = enumerate(r, EnumeratePackages, 2, 0, &hr);
  check(hr == S_OK && s.calls == 0, P, "XPackageEnumeratePackages(PublisherContent): none");
  s = enumerate(r, EnumeratePackages2, 0, 0, &hr);
  check(hr == S_OK && s.calls == 1, P, "IXPackageImpl2 XPackageEnumeratePackages");
  s = enumerate(r, EnumeratePackages3, 0, 1, &hr);
  check(hr == S_OK && s.calls == 1, P, "IXPackageImpl3 XPackageEnumeratePackages");
  s = enumerate(r, EnumeratePackages, 7, 0, &hr);
  check(hr == E_INVALIDARG, P, "XPackageEnumeratePackages: bad kind");
  s = enumerate(r, EnumeratePackages, 0, 9, &hr);
  check(hr == E_INVALIDARG, P, "XPackageEnumeratePackages: bad scope");
  check(r.pkg<HRESULT>(EnumeratePackages, 0u, 0u, static_cast<void*>(nullptr), static_cast<void*>(nullptr)) == E_INVALIDARG, P,
        "XPackageEnumeratePackages: null callback");

  uint32_t kind = 9;
  check(r.pkg<HRESULT>(GetKind, "XGR.LocalPackage", &kind) == S_OK && kind == 0, P, "XPackageGetKind(own) = Game");
  check(r.pkg<HRESULT>(GetKind, "OtherPackage", &kind) == kNoPackageIdentifier, P, "XPackageGetKind(other) -> E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER");
  check(r.pkg<HRESULT>(GetKind, "..\\..\\Windows", &kind) == kNoPackageIdentifier, P, "XPackageGetKind(traversal) rejected");
  check(r.pkg<HRESULT>(GetKind, static_cast<const char*>(nullptr), &kind) == E_INVALIDARG, P, "XPackageGetKind(null)");

  char locale[85] = "unchanged";
  check(r.pkg<HRESULT>(GetUserLocale, sizeof(locale), locale) == S_OK && locale[0] == '\0', P,
        "XPackageGetUserLocale: no package languages -> success, empty");

  uint32_t avail = 9;
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 0u, static_cast<Selector*>(nullptr), &avail) == S_OK && avail == 0, P,
        "XPackageFindChunkAvailability(whole package) = Ready");
  Selector tag{}; tag.type = 1; tag.text = "AnyTag";
  Selector lang{}; lang.type = 0; lang.text = "fr-FR";
  Selector chunk{}; chunk.type = 2; chunk.chunkId = 1000;
  Selector feature{}; feature.type = 3; feature.text = "AnyFeature";
  Selector several[] = {tag, lang, chunk, feature};
  avail = 9;
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 4u, several, &avail) == S_OK && avail == 0, P,
        "XPackageFindChunkAvailability(tag, language, chunk, feature) = Ready");
  Selector bad{}; bad.type = 9; bad.text = "x";
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, &bad, &avail) == E_INVALIDARG, P, "FindChunkAvailability: bad selector type");
  Selector nulltag{}; nulltag.type = 1; nulltag.text = nullptr;
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, &nulltag, &avail) == E_INVALIDARG, P, "FindChunkAvailability: null tag");
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, static_cast<Selector*>(nullptr), &avail) == E_INVALIDARG, P,
        "FindChunkAvailability: count 1, null selectors");
  check(r.pkg<HRESULT>(FindChunkAvailability, "SomeDlc", 0u, static_cast<Selector*>(nullptr), &avail) == kNoPackageIdentifier, P,
        "FindChunkAvailability(content not present) -> E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER");
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 0u, static_cast<Selector*>(nullptr), static_cast<uint32_t*>(nullptr)) == E_INVALIDARG, P,
        "FindChunkAvailability: null out");

  SeenChunks seen;
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "XGR.LocalPackage", 0u, static_cast<void*>(&seen), &record_chunk) == S_OK && seen.calls == 0, P,
        "XPackageEnumerateChunkAvailability(Language): none declared");
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "XGR.LocalPackage", 1u, static_cast<void*>(&seen), &record_chunk) == S_OK && seen.calls == 0, P,
        "XPackageEnumerateChunkAvailability(Tag): no layout known");
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "Other", 0u, static_cast<void*>(&seen), &record_chunk) == kNoPackageIdentifier, P,
        "XPackageEnumerateChunkAvailability(other)");
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "XGR.LocalPackage", 7u, static_cast<void*>(&seen), &record_chunk) == E_INVALIDARG, P,
        "XPackageEnumerateChunkAvailability: bad type");
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "XGR.LocalPackage", 0u, static_cast<void*>(&seen), static_cast<void*>(nullptr)) == E_INVALIDARG, P,
        "XPackageEnumerateChunkAvailability: null callback");

  check(r.pkg<HRESULT>(EnumerateFeatures, "XGR.LocalPackage", static_cast<void*>(nullptr), &record_feature) == S_OK && g_feature_calls == 0, P,
        "XPackageEnumerateFeatures(own): no features");
  check(r.pkg<HRESULT>(EnumerateFeatures, "Other", static_cast<void*>(nullptr), &record_feature) == kNoPackageIdentifier, P, "XPackageEnumerateFeatures(other)");
  check(r.pkg<HRESULT>(EnumerateFeatures, "XGR.LocalPackage", static_cast<void*>(nullptr), static_cast<void*>(nullptr)) == E_INVALIDARG, P,
        "XPackageEnumerateFeatures: null callback");

  check(r.pkg<HRESULT>(ChangeChunkInstallOrder, "XGR.LocalPackage", 1u, &tag) == S_OK, P, "XPackageChangeChunkInstallOrder(own)");
  check(r.pkg<HRESULT>(ChangeChunkInstallOrder, "Other", 1u, &tag) == kNoPackageIdentifier, P, "XPackageChangeChunkInstallOrder(other)");

  uint64_t dl = 77; bool prompt = true;
  check(r.pkg<HRESULT>(EstimateDownloadSize, "XGR.LocalPackage", 1u, &tag, &dl, &prompt) == S_OK && dl == 0 && !prompt, P,
        "XPackageEstimateDownloadSize(own) = 0, no prompt");
  check(r.pkg<HRESULT>(EstimateDownloadSize, "XGR.LocalPackage", 1u, &tag, &dl, static_cast<bool*>(nullptr)) == S_OK, P,
        "XPackageEstimateDownloadSize: optional prompt out");
  check(r.pkg<HRESULT>(EstimateDownloadSize, "SomeDlc", 1u, &tag, &dl, &prompt) == kNoPackageIdentifier, P, "XPackageEstimateDownloadSize(DLC not present)");
  check(r.pkg<HRESULT>(EstimateDownloadSize, "XGR.LocalPackage", 1u, &tag, static_cast<uint64_t*>(nullptr), &prompt) == E_INVALIDARG, P,
        "XPackageEstimateDownloadSize: null size");

  void* monitor = nullptr;
  check(r.pkg<HRESULT>(CreateInstallationMonitor, "XGR.LocalPackage", 0u, static_cast<Selector*>(nullptr), 1000u, static_cast<void*>(nullptr), &monitor) == S_OK &&
            monitor, P, "XPackageCreateInstallationMonitor(own)");
  Progress pr{};
  r.pkg<void>(GetInstallationProgress, monitor, &pr);
  check(pr.completed && pr.launchable && pr.totalBytes > 0 && pr.installedBytes == pr.totalBytes && pr.launchBytes == pr.totalBytes, P,
        "XPackageGetInstallationProgress: 100% installed, launchable");
  check(!r.pkg<bool>(UpdateInstallationMonitor, monitor), P, "XPackageUpdateInstallationMonitor: nothing changed");
  Token t1{}, t2{};
  check(r.pkg<HRESULT>(RegisterInstallationProgressChanged, monitor, static_cast<void*>(nullptr), &record_progress, &t1) == S_OK, P,
        "XPackageRegisterInstallationProgressChanged");
  check(r.pkg<HRESULT>(RegisterInstallationProgressChanged, monitor, static_cast<void*>(nullptr), &record_progress, &t2) == S_OK && t1.token != t2.token, P,
        "second registration gets a distinct token");
  check(r.pkg<HRESULT>(RegisterInstallationProgressChanged, monitor, static_cast<void*>(nullptr), static_cast<void*>(nullptr), &t2) == E_INVALIDARG, P,
        "XPackageRegisterInstallationProgressChanged: null callback");
  Sleep(50);
  check(g_progress_calls == 0, P, "progress callback never fires");
  check(r.pkg<bool>(UnregisterInstallationProgressChanged, monitor, t1, true), P, "XPackageUnregisterInstallationProgressChanged");
  check(!r.pkg<bool>(UnregisterInstallationProgressChanged, monitor, t1, false), P, "unregister twice -> false");
  r.pkg<void>(CloseInstallationMonitorHandle, monitor);
  pr = {};
  pr.completed = true;
  r.pkg<void>(GetInstallationProgress, monitor, &pr);
  check(!pr.completed && pr.totalBytes == 0, P, "closed monitor reports nothing");
  check(r.pkg<HRESULT>(RegisterInstallationProgressChanged, monitor, static_cast<void*>(nullptr), &record_progress, &t1) == E_INVALIDARG, P,
        "register on closed monitor -> E_INVALIDARG");
  check(!r.pkg<bool>(UnregisterInstallationProgressChanged, monitor, t2, false), P, "closing the monitor dropped its registrations");
  r.pkg<void>(CloseInstallationMonitorHandle, monitor);
  monitor = reinterpret_cast<void*>(1);
  check(r.pkg<HRESULT>(CreateInstallationMonitor, "SomeDlc", 0u, static_cast<Selector*>(nullptr), 0u, static_cast<void*>(nullptr), &monitor) == kNoPackageIdentifier &&
            !monitor, P, "XPackageCreateInstallationMonitor(DLC not present)");

  check(r.pkg<HRESULT>(InstallChunks, "XGR.LocalPackage", 1u, &tag, 0u, false, static_cast<void*>(nullptr), &monitor) == S_OK && monitor, P,
        "XPackageInstallChunks(own, already installed)");
  pr = {};
  r.pkg<void>(GetInstallationProgress, monitor, &pr);
  check(pr.completed, P, "installed-chunks monitor is complete");
  r.pkg<void>(CloseInstallationMonitorHandle, monitor);
  check(r.pkg<HRESULT>(InstallChunks, "SomeDlc", 1u, &tag, 0u, false, static_cast<void*>(nullptr), &monitor) == kNoPackageIdentifier, P,
        "XPackageInstallChunks(DLC) -> E_GAMEPACKAGE_NO_PACKAGE_IDENTIFIER");

  Token it{};
  check(r.pkg<HRESULT>(RegisterPackageInstalled, static_cast<void*>(nullptr), static_cast<void*>(nullptr), &record_installed, &it) == S_OK, P,
        "XPackageRegisterPackageInstalled");
  Token it3{};
  check(r.pkg<HRESULT>(RegisterPackageInstalled3, static_cast<void*>(nullptr), static_cast<void*>(nullptr), &record_installed, &it3) == S_OK, P,
        "IXPackageImpl3 XPackageRegisterPackageInstalled");
  check(r.pkg<HRESULT>(RegisterPackageInstalled2, static_cast<void*>(nullptr), static_cast<void*>(nullptr), static_cast<void*>(nullptr), &it3) == E_INVALIDARG, P,
        "XPackageRegisterPackageInstalled: null callback");
  check(r.pkg<bool>(UnregisterPackageInstalled, it, true) && !r.pkg<bool>(UnregisterPackageInstalled, it, false), P,
        "XPackageUnregisterPackageInstalled: once true, then false");
  check(g_installed_calls == 0, P, "package-installed callback never fires");

  WriteStats ws{};
  check(r.pkg<HRESULT>(GetWriteStats, &ws) == S_OK && ws.interval == 300000 && ws.budget == (1ull << 30) && ws.elapsed == 0, P,
        "XPackageGetWriteStats: 5 min interval, 1 GiB budget, first interval untimed");
  check(r.pkg<HRESULT>(GetWriteStats, static_cast<WriteStats*>(nullptr)) == E_INVALIDARG, P, "XPackageGetWriteStats: null");
  check(r.pkg<HRESULT>(UninstallUWPInstance, "Anything") == E_NOTIMPL, P, "XPackageUninstallUWPInstance -> E_NOTIMPL (console)");
  check(!r.pkg<bool>(UninstallPackage, "XGR.LocalPackage") && !r.pkg<bool>(UninstallPackage, "SomeDlc"), P, "XPackageUninstallPackage -> false");

  size_t size = 0;
  check(r.pkg<HRESULT>(GetMountPathSize, static_cast<void*>(nullptr), &size) == E_INVALIDARG, P, "XPackageGetMountPathSize(null handle)");

  check(r.unload(), P, "runtime unloaded");
}

void phase_environment() {
  const char* P = "environment";
  clear_package_env();
  set_env(L"XGR_PACKAGE_ID", L"Test.Pkg_01");
  set_env(L"XGR_PACKAGE_STORE_ID", L"TESTSTORE");
  set_env(L"XGR_PACKAGE_DISPLAY_NAME", L"Env Name");
  set_env(L"XGR_PACKAGE_PUBLISHER", L"Env Pub");
  set_env(L"XGR_PACKAGE_DESCRIPTION", L"Env Desc");
  set_env(L"XGR_PACKAGE_VERSION", L"2.3.4.5");
  set_env(L"XGR_TITLE_ID", L"abc");
  Runtime r;
  if (!r.load()) { check(false, P, "load runtime"); return; }
  check(package_id(r) == "Test.Pkg_01", P, "XGR_PACKAGE_ID used");
  HRESULT hr;
  Seen s = enumerate(r, EnumeratePackages, 0, 0, &hr);
  check(hr == S_OK && s.calls == 1 && s.id == "Test.Pkg_01" && s.store == "TESTSTORE" && s.name == "Env Name" && s.pub == "Env Pub" &&
            s.desc == "Env Desc" && s.title == "00000ABC", P, "details from environment (title id normalised to 8 upper hex)");
  check(s.last.version.major == 2 && s.last.version.minor == 3 && s.last.version.build == 4 && s.last.version.revision == 5, P, "XGR_PACKAGE_VERSION");
  uint32_t kind = 9;
  check(r.pkg<HRESULT>(GetKind, "Test.Pkg_01", &kind) == S_OK && kind == 0, P, "own id resolves");
  check(r.pkg<HRESULT>(GetKind, "XGR.LocalPackage", &kind) == kNoPackageIdentifier, P, "default id no longer resolves");
  check(r.unload(), P, "runtime unloaded");
}

void phase_bogus_language() {
  const char* P = "config-languages";
  clear_package_env();
  set_env(L"XGR_PACKAGE_ID", L"CON.txt");
  if (!write_config("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Game configVersion=\"1\">\n"
                    "  <!-- <TitleId>FFFFFFFF</TitleId> -->\n"
                    "  <Resources><Resource Language=\"zz-ZZ\"/></Resources>\n</Game>\n")) {
    check(false, P, "write config");
    return;
  }
  Runtime r;
  if (!r.load()) { check(false, P, "load runtime"); return; }
  check(package_id(r) == "XGR.LocalPackage", P, "reserved device name in XGR_PACKAGE_ID rejected");
  HRESULT hr;
  Seen s = enumerate(r, EnumeratePackages, 0, 0, &hr);
  check(hr == S_OK && s.title.empty(), P, "commented-out TitleId ignored");
  char locale[85] = {};
  check(r.pkg<HRESULT>(GetUserLocale, sizeof(locale), locale) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), P,
        "XPackageGetUserLocale: no user locale close to the package's -> error");
  Selector lang{}; lang.type = 0; lang.text = "en-US";
  uint32_t avail = 9;
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, &lang, &avail) == kInvalidSelector, P,
        "FindChunkAvailability(undeclared language) -> E_GAMEPACKAGE_INVALID_SELECTOR");
  lang.text = "ZZ-zz";
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, &lang, &avail) == S_OK && avail == 0, P,
        "FindChunkAvailability(declared language) = Ready");
  SeenChunks seen;
  check(r.pkg<HRESULT>(EnumerateChunkAvailability, "XGR.LocalPackage", 0u, static_cast<void*>(&seen), &record_chunk) == S_OK && seen.calls == 1 &&
            seen.languages[0] == "zz-ZZ" && seen.availability[0] == 0, P, "EnumerateChunkAvailability(Language): declared language, Ready");
  check(r.unload(), P, "runtime unloaded");
}

void phase_config() {
  const char* P = "config";
  clear_package_env();
  set_env(L"XGR_PACKAGE_ID", L"ThisIdentifierIsMuchTooLongToBeAPackageId");
  set_env(L"XGR_PACKAGE_STORE_ID", L"ENVSTORE");
  set_env(L"XGR_TITLE_ID", L"12345678");
  wchar_t user_locale[LOCALE_NAME_MAX_LENGTH] = {};
  GetUserDefaultLocaleName(user_locale, LOCALE_NAME_MAX_LENGTH);
  std::string locale_utf8 = to_utf8(user_locale);
  std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Game configVersion=\"1\">\n"
                     "  <Identity Name=\"Xgr.TestTitle\" Publisher=\"CN=XGR\" Version=\"3.1.4.1\"/>\n"
                     "  <ShellVisuals DefaultDisplayName=\"Config &amp; Name\" PublisherDisplayName='Config Pub' Description=\"Config Desc\"/>\n"
                     "  <TitleId>1a2b3c4d</TitleId>\n  <StoreId>CFGSTORE0001</StoreId>\n"
                     "  <Resources><Resource Language=\"zz-ZZ\"/><Resource Language=\"" + locale_utf8 + "\"/></Resources>\n"
                     "</Game>\n";
  if (!write_config(body)) { check(false, P, "write config"); return; }
  Runtime r;
  if (!r.load()) { check(false, P, "load runtime"); return; }
  check(package_id(r) == "XGR.LocalPackage", P, "over-long XGR_PACKAGE_ID rejected");
  HRESULT hr;
  Seen s = enumerate(r, EnumeratePackages, 0, 0, &hr);
  check(hr == S_OK && s.name == "Config & Name" && s.pub == "Config Pub" && s.desc == "Config Desc" && s.store == "CFGSTORE0001" &&
            s.title == "1A2B3C4D", P, "details from MicrosoftGame.config win over the environment");
  check(s.last.version.major == 3 && s.last.version.minor == 1 && s.last.version.build == 4 && s.last.version.revision == 1, P, "Identity Version");
  char locale[85] = {};
  check(r.pkg<HRESULT>(GetUserLocale, sizeof(locale), locale) == S_OK && locale_utf8 == locale, P, "XPackageGetUserLocale = user locale declared by the package");
  check(r.pkg<HRESULT>(GetUserLocale, static_cast<size_t>(2), locale) == E_NOT_SUFFICIENT_BUFFER, P, "XPackageGetUserLocale: short buffer");

  AsyncBlock ab{};
  ab = {};
  void* mount = nullptr;
  check(r.pkg<HRESULT>(MountWithUiAsync, "XGR.LocalPackage", &ab) == S_OK && r.wait(&ab) == S_OK &&
            r.pkg<HRESULT>(MountWithUiResult, &ab, &mount) == S_OK && mount, P, "XPackageMountWithUiAsync(own)");
  check(mount_path(r, mount, P) == to_utf8(g_exe_dir), P, "main package mounts at the executable's directory");
  void* second = nullptr;
  ab = {};
  check(r.pkg<HRESULT>(MountWithUiAsync, "XGR.LocalPackage", &ab) == S_OK && r.wait(&ab) == S_OK &&
            r.pkg<HRESULT>(MountWithUiResult, &ab, &second) == S_OK && second && second != mount, P, "second mount gets its own handle");
  r.pkg<void>(CloseMountHandle, mount);
  size_t size = 0;
  check(r.pkg<HRESULT>(GetMountPathSize, second, &size) == S_OK, P, "closing one handle leaves the other valid");
  r.pkg<void>(CloseMountHandle, second);
  check(r.pkg<HRESULT>(GetMountPathSize, second, &size) == E_INVALIDARG, P, "both handles closed");
  ab = {};
  mount = nullptr;
  check(r.pkg<HRESULT>(MountWithUiAsync, "SomeDlc", &ab) == S_OK && r.wait(&ab) == kNoPackageIdentifier &&
            r.pkg<HRESULT>(MountWithUiResult, &ab, &mount) == kNoPackageIdentifier && !mount, P, "XPackageMountWithUiAsync(DLC not present) fails");
  check(r.pkg<HRESULT>(MountWithUiAsync, static_cast<const char*>(nullptr), &ab) == E_INVALIDARG, P, "XPackageMountWithUiAsync(null)");

  Selector tag{}; tag.type = 1; tag.text = "AnyTag";
  ab = {};
  void* monitor = nullptr;
  check(r.pkg<HRESULT>(InstallChunksAsync, "XGR.LocalPackage", 1u, &tag, 0u, true, &ab) == S_OK && r.wait(&ab) == S_OK &&
            r.pkg<HRESULT>(InstallChunksResult, &ab, &monitor) == S_OK && monitor, P, "XPackageInstallChunksAsync(own)");
  Progress pr{};
  r.pkg<void>(GetInstallationProgress, monitor, &pr);
  check(pr.completed && pr.installedBytes == pr.totalBytes, P, "async install monitor complete");
  r.pkg<void>(CloseInstallationMonitorHandle, monitor);
  ab = {};
  monitor = nullptr;
  check(r.pkg<HRESULT>(InstallChunksAsync, "SomeDlc", 1u, &tag, 0u, true, &ab) == S_OK && r.wait(&ab) == kNoPackageIdentifier &&
            r.pkg<HRESULT>(InstallChunksResult, &ab, &monitor) == kNoPackageIdentifier && !monitor, P, "XPackageInstallChunksAsync(DLC) fails");
  Selector bad{}; bad.type = 1; bad.text = nullptr;
  ab = {};
  check(r.pkg<HRESULT>(InstallChunksAsync, "XGR.LocalPackage", 1u, &bad, 0u, true, &ab) == S_OK && r.wait(&ab) == E_INVALIDARG, P,
        "XPackageInstallChunksAsync: bad selector fails the call");
}

typedef HRESULT (*MountForPackageFn)(void*, const char*, void**);
MountForPackageFn mount_for_package(Runtime& r, void** self) {
  auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(r.module, "QueryApiImpl")));
  *self = nullptr;
  if (!query || query(kPersistentLocalStorage3, kPersistentLocalStorage3, self) || !*self) return nullptr;
  return reinterpret_cast<MountForPackageFn>((*static_cast<void***>(*self))[kMountForPackageSlot]);
}

void phase_uninstall_and_pls() {
  const char* P = "uninstall-and-pls";
  clear_package_env();
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  std::wstring pls = std::wstring(temp) + L"xgr_test_pls_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  set_env(L"XGR_PLS_DIR", pls.c_str());
  set_env(L"XGR_PLS_SIZE_MB", L"16");
  Runtime r;
  if (!r.load()) { check(false, P, "load runtime"); return; }

  Selector tag{}; tag.type = 1; tag.text = "BigMaps";
  check(r.pkg<HRESULT>(UninstallChunks, "XGR.LocalPackage", 1u, &tag) == S_OK, P, "XPackageUninstallChunks(own, tag) succeeds and removes nothing");
  uint32_t avail = 9;
  check(r.pkg<HRESULT>(FindChunkAvailability, "XGR.LocalPackage", 1u, &tag, &avail) == S_OK && avail == 0, P,
        "chunks stay Ready after XPackageUninstallChunks (every chunk is needed by the one installed package)");
  Selector chunk{}; chunk.type = 2; chunk.chunkId = 1000;
  check(r.pkg<HRESULT>(UninstallChunks, "XGR.LocalPackage", 1u, &chunk) == E_INVALIDARG, P,
        "XPackageUninstallChunks rejects a selector with only a chunk id (documented)");
  check(r.pkg<HRESULT>(UninstallChunks, "XGR.LocalPackage", 0u, static_cast<Selector*>(nullptr)) == E_INVALIDARG, P,
        "XPackageUninstallChunks rejects an empty selector list");
  Selector nullTag{}; nullTag.type = 1; nullTag.text = nullptr;
  check(r.pkg<HRESULT>(UninstallChunks, "XGR.LocalPackage", 1u, &nullTag) == E_INVALIDARG, P, "XPackageUninstallChunks rejects a tag selector without a tag");
  check(r.pkg<HRESULT>(UninstallChunks, "SomeDlc", 1u, &tag) == kNoPackageIdentifier, P, "XPackageUninstallChunks(DLC not present)");
  check(r.pkg<HRESULT>(UninstallChunks, static_cast<const char*>(nullptr), 1u, &tag) == E_INVALIDARG, P, "XPackageUninstallChunks(null)");

  void* storage = nullptr;
  MountForPackageFn mountFor = mount_for_package(r, &storage);
  check(mountFor != nullptr, P, "IXPersistentLocalStorageImpl3 is available");
  if (mountFor) {
    void* mount = nullptr;
    check(mountFor(storage, "XGR.LocalPackage", &mount) == S_OK && mount, P, "XPersistentLocalStorageMountForPackage(own)");
    check(mount && mount_path(r, mount, P) == to_utf8(pls), P, "the mount is the title's persistent local storage directory");
    r.pkg<void>(CloseMountHandle, mount);
    size_t size = 0;
    check(r.pkg<HRESULT>(GetMountPathSize, mount, &size) == E_INVALIDARG, P, "XPackageCloseMountHandle releases the storage mount");
    mount = reinterpret_cast<void*>(1);
    check(mountFor(storage, "OtherTitle", &mount) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !mount, P,
          "XPersistentLocalStorageMountForPackage(another title): no such storage");
    check(mountFor(storage, nullptr, &mount) == E_INVALIDARG && mountFor(storage, "XGR.LocalPackage", nullptr) == E_INVALIDARG, P,
          "XPersistentLocalStorageMountForPackage rejects null arguments");
  }
  check(r.unload(), P, "runtime unloaded");

  set_env(L"XGR_PLS_SIZE_MB", nullptr);
  Runtime none;
  if (!none.load()) { check(false, P, "load runtime"); return; }
  mountFor = mount_for_package(none, &storage);
  void* mount = nullptr;
  check(mountFor && mountFor(storage, "XGR.LocalPackage", &mount) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && !mount, P,
        "a title that declares no persistent local storage has none to mount");
  check(none.unload(), P, "runtime unloaded");
  set_env(L"XGR_PLS_DIR", nullptr);
  RemoveDirectoryW(pls.c_str());
}

void cleanup() {
  check(remove_own_config(), "cleanup", "test MicrosoftGame.config removed");
}

int main() {
  wchar_t exe[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe, n);
  g_exe_dir = path.substr(0, path.find_last_of(L'\\') + 1);
  g_config = g_exe_dir + L"MicrosoftGame.config";
  if (!remove_own_config()) {
    std::printf("FAIL a MicrosoftGame.config not written by this test is next to the executable; refusing to touch it\n");
    return 1;
  }
  phase_defaults();
  phase_environment();
  phase_bogus_language();
  if (!remove_own_config()) check(false, "setup", "remove config between phases");
  phase_uninstall_and_pls();
  phase_config();
  cleanup();
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
  return g_failures ? 1 : 0;
}
