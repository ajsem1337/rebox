#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);
struct AsyncBlock { void* queue; void* context; void (*callback)(AsyncBlock*); unsigned char internal[32]; };
struct Token { uint64_t value; };

struct BroadcastStatus {
  uint8_t canStartBroadcast, isAnyAppBroadcasting, isCaptureResourceUnavailable, isGameStreamInProgress, isGpuConstrained, isAppInactive,
      isBlockedForApp, isDisabledByUser, isDisabledBySystem;
};
struct ScreenshotFile { char path[260]; size_t fileSize; uint32_t width, height; };
struct DiagnosticScreenshotResult { size_t fileCount; ScreenshotFile files[10]; };
struct RecordClipResult { char path[260]; size_t fileSize; uint64_t startTime; uint32_t durationInMs, width, height; uint8_t encoding; };
struct LocalResult {
  void* clipHandle;
  size_t fileSizeInBytes;
  SYSTEMTIME clipStartTimestamp;
  uint64_t durationInMilliseconds;
  uint32_t width, height;
  uint8_t encoding, colorFormat;
};
struct UserRecordingResult {
  size_t fileSizeInBytes;
  SYSTEMTIME clipStartTimestamp;
  uint64_t durationInMilliseconds;
  uint32_t width, height;
  uint8_t encoding, colorFormat;
};
struct VideoCaptureSettings { uint32_t width, height; uint64_t maxRecordTimespanDurationInMs; uint8_t encoding, colorFormat, isCaptureByGamesAllowed; };
static_assert(sizeof(BroadcastStatus) == 9 && sizeof(DiagnosticScreenshotResult) == 2808 && sizeof(RecordClipResult) == 296 &&
                  sizeof(LocalResult) == 56 && sizeof(UserRecordingResult) == 48 && sizeof(VideoCaptureSettings) == 24,
              "XAppCapture struct layouts");

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
static bool LoadTable() {
  std::ifstream file("table.json", std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  g_table = text.str();
  return !g_table.empty();
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
static Api Get(const GUID& iid) {
  void* object = nullptr;
  HRESULT hr = g_query(iid, iid, &object);
  if (FAILED(hr) || !object) {
    std::printf("FAIL QueryApiImpl %08lx\n", static_cast<unsigned long>(hr));
    std::exit(1);
  }
  return {object, *static_cast<void***>(object)};
}

constexpr HRESULT kBlockedByTitle = static_cast<HRESULT>(0x82323018);
constexpr HRESULT kDisabledByCaptureSetting = static_cast<HRESULT>(0x8232301E);
constexpr HRESULT kStopWithoutStart = static_cast<HRESULT>(0x82323014);
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);

static const GUID kCapture = {0x22e672d7, 0xb4e3, 0x406c, {0xbd, 0x50, 0x8f, 0x0d, 0x25, 0x23, 0x6f, 0x9e}};
static const GUID kMetadata = {0x186d5592, 0xa72d, 0x45fb, {0x95, 0x60, 0x11, 0xae, 0xd0, 0xe6, 0x64, 0x7a}};
static const GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID kFeature = {0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};
static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

static Api g_capture, g_metadata, g_user, g_threading;
static void* g_local_user = nullptr;

typedef HRESULT (*TakeDiagnosticScreenshotFn)(void*, BOOLEAN, uint16_t, const char*, DiagnosticScreenshotResult*);
typedef HRESULT (*RecordDiagnosticClipFn)(void*, uint64_t, uint32_t, const char*, RecordClipResult*);
typedef HRESULT (*TakeScreenshotFn)(void*, void*, void*);
typedef HRESULT (*OpenScreenshotStreamFn)(void*, const char*, uint16_t, void**, uint64_t*);
typedef HRESULT (*ReadScreenshotStreamFn)(void*, void*, uint64_t, uint32_t, uint8_t*, uint32_t*);
typedef HRESULT (*CloseStreamFn)(void*, void*);
typedef HRESULT (*NoArgFn)(void*);
typedef HRESULT (*GetVideoCaptureSettingsFn)(void*, VideoCaptureSettings*);
typedef HRESULT (*RecordTimespanFn)(void*, const SYSTEMTIME*, uint64_t, LocalResult*);
typedef HRESULT (*ReadLocalStreamFn)(void*, void*, size_t, uint32_t, uint8_t*, uint32_t*);
typedef HRESULT (*StartUserRecordFn)(void*, void*, uint32_t, char*);
typedef HRESULT (*StopUserRecordFn)(void*, const char*, UserRecordingResult*);
typedef HRESULT (*CancelUserRecordFn)(void*, const char*);
typedef BOOLEAN (*IsBroadcastingFn)(void*);
typedef HRESULT (*ShowUiFn)(void*, void*);
typedef HRESULT (*GetStatusFn)(void*, void*, BroadcastStatus*);
typedef HRESULT (*RegisterFn)(void*, void*, void*, void*, Token*);
typedef BOOLEAN (*UnregisterFn)(void*, Token, BOOLEAN);
typedef HRESULT (*StringMetadataFn)(void*, const char*, const char*, uint8_t);
typedef HRESULT (*Int32MetadataFn)(void*, const char*, int32_t, uint8_t);
typedef HRESULT (*DoubleMetadataFn)(void*, const char*, double, uint8_t);
typedef HRESULT (*StopStateFn)(void*, const char*);
typedef HRESULT (*RemainingFn)(void*, uint64_t*);

static bool AllZero(const void* data, size_t size) {
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; i++)
    if (bytes[i]) return false;
  return true;
}

static void AddLocalUser() {
  AsyncBlock add{};
  g_user.At<HRESULT (*)(void*, uint32_t, AsyncBlock*)>(7, "XUserAddAsync")(g_user.self, 1, &add);
  g_threading.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(g_threading.self, &add, TRUE);
  HRESULT hr = g_user.At<HRESULT (*)(void*, AsyncBlock*, void**)>(8, "XUserAddResult")(g_user.self, &add, &g_local_user);
  Check(hr == S_OK && g_local_user, "test adds the local user");
}

static HRESULT TakeDiagnostic(uint16_t flags, const char* prefix, DiagnosticScreenshotResult* result) {
  return g_capture.At<TakeDiagnosticScreenshotFn>(3, "XAppCaptureTakeDiagnosticScreenshot")(g_capture.self, TRUE, flags, prefix, result);
}
static HRESULT RecordDiagnostic(uint32_t duration, const char* prefix, RecordClipResult* result) {
  return g_capture.At<RecordDiagnosticClipFn>(4, "XAppCaptureRecordDiagnosticClip")(g_capture.self, 0, duration, prefix, result);
}
static HRESULT StartUserRecord(void* user, uint32_t length, char* buffer) {
  return g_capture.At<StartUserRecordFn>(15, "XAppCaptureStartUserRecord")(g_capture.self, user, length, buffer);
}
static HRESULT GetStatus(void* user, BroadcastStatus* status) {
  return g_metadata.At<GetStatusFn>(5, "XAppBroadcastGetStatus")(g_metadata.self, user, status);
}

static void CheckDiagnostics() {
  DiagnosticScreenshotResult shot;
  std::memset(&shot, 0xcc, sizeof(shot));
  Check(TakeDiagnostic(1, nullptr, &shot) == kDisabledByCaptureSetting && AllZero(&shot, sizeof(shot)),
        "XAppCaptureTakeDiagnosticScreenshot takes no screenshot and reports game captures disabled, result cleared");
  Check(TakeDiagnostic(3, "bug_42", &shot) == kDisabledByCaptureSetting, "XAppCaptureTakeDiagnosticScreenshot accepts SDR|HDR and a plain prefix, still no capture");
  Check(TakeDiagnostic(1, nullptr, nullptr) == E_INVALIDARG, "XAppCaptureTakeDiagnosticScreenshot rejects a null result");
  Check(TakeDiagnostic(0, nullptr, &shot) == E_INVALIDARG, "XAppCaptureTakeDiagnosticScreenshot rejects empty capture flags");
  Check(TakeDiagnostic(4, nullptr, &shot) == E_INVALIDARG, "XAppCaptureTakeDiagnosticScreenshot rejects unknown capture flags");
  const char* unsafe[] = {"..\\up", "a/b", "c:x", "x\\y", "tab\there", "..", "nul?"};
  bool rejected = true;
  for (const char* prefix : unsafe) rejected = rejected && TakeDiagnostic(1, prefix, &shot) == E_INVALIDARG;
  Check(rejected, "XAppCaptureTakeDiagnosticScreenshot rejects prefixes with separators, drive colons, .. or control characters");
  std::string longPrefix(200, 'p');
  Check(TakeDiagnostic(1, longPrefix.c_str(), &shot) == E_INVALIDARG, "XAppCaptureTakeDiagnosticScreenshot rejects an overlong prefix");

  RecordClipResult clip;
  std::memset(&clip, 0xcc, sizeof(clip));
  Check(RecordDiagnostic(5000, nullptr, &clip) == kDisabledByCaptureSetting && AllZero(&clip, sizeof(clip)),
        "XAppCaptureRecordDiagnosticClip records nothing and reports game captures disabled, result cleared");
  Check(RecordDiagnostic(5000, nullptr, nullptr) == E_INVALIDARG, "XAppCaptureRecordDiagnosticClip rejects a null result");
  Check(RecordDiagnostic(0, nullptr, &clip) == E_INVALIDARG, "XAppCaptureRecordDiagnosticClip rejects a zero duration");
  Check(RecordDiagnostic(5000, "..\\..\\evil", &clip) == E_INVALIDARG, "XAppCaptureRecordDiagnosticClip rejects an unsafe prefix");
}

static void CheckScreenshots() {
  auto take = g_capture.At<TakeScreenshotFn>(5, "XAppCaptureTakeScreenshot");
  unsigned char result[252] = {};
  std::memset(result, 0xcc, sizeof(result));
  Check(take(g_capture.self, g_local_user, result) == kDisabledByCaptureSetting && AllZero(result, sizeof(result)),
        "XAppCaptureTakeScreenshot takes nothing and reports game captures disabled, result cleared");
  Check(take(g_capture.self, nullptr, result) == E_INVALIDARG, "XAppCaptureTakeScreenshot rejects a null user");
  Check(take(g_capture.self, g_local_user, nullptr) == E_INVALIDARG, "XAppCaptureTakeScreenshot rejects a null result");

  auto open = g_capture.At<OpenScreenshotStreamFn>(6, "XAppCaptureOpenScreenshotStream");
  void* handle = reinterpret_cast<void*>(1);
  uint64_t total = 77;
  Check(open(g_capture.self, "screenshot-1", 1, &handle, &total) == kNotFound && !handle && total == 0,
        "XAppCaptureOpenScreenshotStream finds no screenshot for any local id");
  Check(open(g_capture.self, "screenshot-1", 2, &handle, nullptr) == kNotFound && !handle, "XAppCaptureOpenScreenshotStream accepts a null totalBytes");
  Check(open(g_capture.self, nullptr, 1, &handle, &total) == E_INVALIDARG, "XAppCaptureOpenScreenshotStream rejects a null local id");
  Check(open(g_capture.self, "", 1, &handle, &total) == E_INVALIDARG, "XAppCaptureOpenScreenshotStream rejects an empty local id");
  std::string longId(260, 'i');
  Check(open(g_capture.self, longId.c_str(), 1, &handle, &total) == E_INVALIDARG, "XAppCaptureOpenScreenshotStream rejects a local id longer than the result field");
  Check(open(g_capture.self, "screenshot-1", 3, &handle, &total) == E_INVALIDARG, "XAppCaptureOpenScreenshotStream rejects a combined format");
  Check(open(g_capture.self, "screenshot-1", 1, nullptr, &total) == E_INVALIDARG, "XAppCaptureOpenScreenshotStream rejects a null handle pointer");

  auto read = g_capture.At<ReadScreenshotStreamFn>(7, "XAppCaptureReadScreenshotStream");
  uint8_t buffer[16];
  uint32_t written = 99;
  void* forged = reinterpret_cast<void*>(0x1234);
  Check(read(g_capture.self, forged, 0, sizeof(buffer), buffer, &written) == E_HANDLE && written == 0,
        "XAppCaptureReadScreenshotStream rejects a handle it never issued and writes nothing");
  Check(read(g_capture.self, nullptr, 0, sizeof(buffer), buffer, &written) == E_INVALIDARG, "XAppCaptureReadScreenshotStream rejects a null handle");
  Check(read(g_capture.self, forged, 0, sizeof(buffer), nullptr, &written) == E_INVALIDARG, "XAppCaptureReadScreenshotStream rejects a null buffer");
  Check(read(g_capture.self, forged, 0, sizeof(buffer), buffer, nullptr) == E_INVALIDARG, "XAppCaptureReadScreenshotStream rejects a null bytesWritten");

  auto close = g_capture.At<CloseStreamFn>(8, "XAppCaptureCloseScreenshotStream");
  Check(close(g_capture.self, forged) == E_HANDLE, "XAppCaptureCloseScreenshotStream rejects a handle it never issued");
  Check(close(g_capture.self, nullptr) == E_INVALIDARG, "XAppCaptureCloseScreenshotStream rejects a null handle");
}

static void CheckLocalRecording() {
  VideoCaptureSettings settings;
  std::memset(&settings, 0xcc, sizeof(settings));
  auto getSettings = g_capture.At<GetVideoCaptureSettingsFn>(11, "XAppCaptureGetVideoCaptureSettings");
  Check(getSettings(g_capture.self, &settings) == S_OK && AllZero(&settings, sizeof(settings)),
        "XAppCaptureGetVideoCaptureSettings reports captures by games not allowed, no resolution, no duration");
  Check(getSettings(g_capture.self, nullptr) == E_INVALIDARG, "XAppCaptureGetVideoCaptureSettings rejects null");

  auto record = g_capture.At<RecordTimespanFn>(12, "XAppCaptureRecordTimespan");
  LocalResult local;
  std::memset(&local, 0xcc, sizeof(local));
  SYSTEMTIME now{};
  GetSystemTime(&now);
  Check(record(g_capture.self, nullptr, 5000, &local) == E_ACCESSDENIED && AllZero(&local, sizeof(local)),
        "XAppCaptureRecordTimespan returns the documented E_ACCESSDENIED (captures not allowed) with no clip handle");
  Check(record(g_capture.self, &now, 5000, &local) == E_ACCESSDENIED, "XAppCaptureRecordTimespan with a start time also returns E_ACCESSDENIED");
  Check(record(g_capture.self, nullptr, 5000, nullptr) == E_INVALIDARG, "XAppCaptureRecordTimespan rejects a null result");
  Check(record(g_capture.self, nullptr, 0, &local) == E_INVALIDARG, "XAppCaptureRecordTimespan rejects a zero duration");

  auto read = g_capture.At<ReadLocalStreamFn>(13, "XAppCaptureReadLocalStream");
  uint8_t buffer[16];
  uint32_t written = 5;
  void* forged = reinterpret_cast<void*>(0x5678);
  Check(read(g_capture.self, forged, 0, sizeof(buffer), buffer, &written) == E_HANDLE && written == 0,
        "XAppCaptureReadLocalStream rejects a handle it never issued and writes nothing");
  Check(read(g_capture.self, nullptr, 0, sizeof(buffer), buffer, &written) == E_INVALIDARG, "XAppCaptureReadLocalStream rejects a null handle");
  Check(read(g_capture.self, forged, 0, 0, nullptr, &written) == E_HANDLE, "XAppCaptureReadLocalStream allows a null buffer for a zero-byte read");
  auto close = g_capture.At<CloseStreamFn>(14, "XAppCaptureCloseLocalStream");
  Check(close(g_capture.self, forged) == E_HANDLE, "XAppCaptureCloseLocalStream rejects a handle it never issued");
  Check(close(g_capture.self, nullptr) == E_INVALIDARG, "XAppCaptureCloseLocalStream rejects a null handle");
}

static void CheckUserRecording() {
  char id[250];
  std::memset(id, 'x', sizeof(id));
  Check(StartUserRecord(g_local_user, sizeof(id), id) == kDisabledByCaptureSetting && id[0] == '\0',
        "XAppCaptureStartUserRecord starts no recording, reports game captures disabled and returns an empty id");
  Check(StartUserRecord(nullptr, sizeof(id), id) == E_INVALIDARG, "XAppCaptureStartUserRecord rejects a null user");
  Check(StartUserRecord(g_local_user, sizeof(id), nullptr) == E_INVALIDARG, "XAppCaptureStartUserRecord rejects a null buffer");
  Check(StartUserRecord(g_local_user, 0, id) == E_INVALIDARG, "XAppCaptureStartUserRecord rejects a zero-length buffer");

  auto stop = g_capture.At<StopUserRecordFn>(16, "XAppCaptureStopUserRecord");
  UserRecordingResult recording;
  std::memset(&recording, 0xcc, sizeof(recording));
  Check(stop(g_capture.self, "recording-1", &recording) == kStopWithoutStart && AllZero(&recording, sizeof(recording)),
        "XAppCaptureStopUserRecord reports that no recording was started");
  Check(stop(g_capture.self, nullptr, &recording) == E_INVALIDARG, "XAppCaptureStopUserRecord rejects a null id");
  Check(stop(g_capture.self, "recording-1", nullptr) == E_INVALIDARG, "XAppCaptureStopUserRecord rejects a null result");

  auto cancel = g_capture.At<CancelUserRecordFn>(17, "XAppCaptureCancelUserRecord");
  Check(cancel(g_capture.self, "recording-1") == kStopWithoutStart, "XAppCaptureCancelUserRecord reports that no recording was started");
  Check(cancel(g_capture.self, "") == E_INVALIDARG, "XAppCaptureCancelUserRecord rejects an empty id");
}

static void CheckRecordToggle() {
  auto enable = g_capture.At<NoArgFn>(9, "XAppCaptureEnableRecord");
  auto disable = g_capture.At<NoArgFn>(10, "XAppCaptureDisableRecord");
  BroadcastStatus status;
  Check(GetStatus(g_local_user, &status) == S_OK && !status.isBlockedForApp, "recording starts enabled by default (not blocked for the app)");
  Check(disable(g_capture.self) == S_OK && disable(g_capture.self) == S_OK, "XAppCaptureDisableRecord returns S_OK, also when repeated");
  Check(GetStatus(g_local_user, &status) == S_OK && status.isBlockedForApp && !status.canStartBroadcast, "XAppCaptureDisableRecord is remembered: broadcast status shows it blocked for the app");
  DiagnosticScreenshotResult shot;
  char id[64];
  unsigned char screenshot[252];
  Check(TakeDiagnostic(1, nullptr, &shot) == kBlockedByTitle && StartUserRecord(g_local_user, sizeof(id), id) == kBlockedByTitle &&
            g_capture.At<TakeScreenshotFn>(5, "XAppCaptureTakeScreenshot")(g_capture.self, g_local_user, screenshot) == kBlockedByTitle,
        "with recording disabled by the title, captures report GameDVR blocked by title");
  Check(enable(g_capture.self) == S_OK, "XAppCaptureEnableRecord returns S_OK");
  Check(GetStatus(g_local_user, &status) == S_OK && !status.isBlockedForApp && TakeDiagnostic(1, nullptr, &shot) == kDisabledByCaptureSetting,
        "XAppCaptureEnableRecord is remembered: back to the not-allowed-by-setting answer");
}

static int g_silent_calls = 0;
static void CALLBACK SilentCallback(void*) { g_silent_calls++; }

static void CheckSilentEvent(int registerSlot, const char* name, int unregisterSlot, const char* unregisterName, void* context) {
  char label[160];
  Token first{}, second{};
  auto reg = g_metadata.At<RegisterFn>(registerSlot, name);
  auto unreg = g_metadata.At<UnregisterFn>(unregisterSlot, unregisterName);
  HRESULT hr1 = reg(g_metadata.self, nullptr, context, reinterpret_cast<void*>(&SilentCallback), &first);
  HRESULT hr2 = reg(g_metadata.self, nullptr, context, reinterpret_cast<void*>(&SilentCallback), &second);
  std::snprintf(label, sizeof(label), "%s registers with distinct tokens", name);
  Check(hr1 == S_OK && hr2 == S_OK && first.value && second.value && first.value != second.value, label);
  Token unused{};
  std::snprintf(label, sizeof(label), "%s rejects a null callback", name);
  Check(reg(g_metadata.self, nullptr, context, nullptr, &unused) == E_INVALIDARG, label);
  std::snprintf(label, sizeof(label), "%s rejects a null token", name);
  Check(reg(g_metadata.self, nullptr, context, reinterpret_cast<void*>(&SilentCallback), nullptr) == E_INVALIDARG, label);
  std::snprintf(label, sizeof(label), "%s unregisters", name);
  Check(unreg(g_metadata.self, first, FALSE) && unreg(g_metadata.self, second, TRUE), label);
}

static void CheckBroadcast() {
  Check(!g_metadata.At<IsBroadcastingFn>(3, "XAppBroadcastIsAppBroadcasting")(g_metadata.self), "XAppBroadcastIsAppBroadcasting reports no broadcast");
  auto showUi = g_metadata.At<ShowUiFn>(4, "XAppBroadcastShowUI");
  Check(showUi(g_metadata.self, g_local_user) == E_ABORT, "XAppBroadcastShowUI shows no UI and returns E_ABORT like the headless game UI");
  Check(showUi(g_metadata.self, nullptr) == E_INVALIDARG, "XAppBroadcastShowUI rejects a null user");
  BroadcastStatus status;
  std::memset(&status, 0xcc, sizeof(status));
  HRESULT hr = GetStatus(g_local_user, &status);
  Check(hr == S_OK && !status.canStartBroadcast && !status.isAnyAppBroadcasting && status.isCaptureResourceUnavailable && !status.isGameStreamInProgress &&
            !status.isGpuConstrained && !status.isAppInactive && !status.isBlockedForApp && !status.isDisabledByUser && !status.isDisabledBySystem,
        "XAppBroadcastGetStatus reports that a broadcast cannot start because capture resources are unavailable");
  Check(GetStatus(nullptr, &status) == E_INVALIDARG, "XAppBroadcastGetStatus rejects a null user");
  Check(GetStatus(g_local_user, nullptr) == E_INVALIDARG, "XAppBroadcastGetStatus rejects a null status");
  CheckSilentEvent(6, "XAppBroadcastRegisterIsAppBroadcastingChanged", 7, "XAppBroadcastUnregisterIsAppBroadcastingChanged", nullptr);
}

static void CheckMetadata() {
  auto remaining = g_metadata.At<RemainingFn>(16, "XAppCaptureMetadataRemainingStorageBytesAvailable");
  uint64_t before = 1;
  Check(remaining(g_metadata.self, &before) == S_OK && before == 0, "XAppCaptureMetadataRemainingStorageBytesAvailable reports no storage, since nothing is kept");
  Check(remaining(g_metadata.self, nullptr) == E_INVALIDARG, "XAppCaptureMetadataRemainingStorageBytesAvailable rejects null");

  auto addString = g_metadata.At<StringMetadataFn>(8, "XAppCaptureMetadataAddStringEvent");
  auto addInt = g_metadata.At<Int32MetadataFn>(9, "XAppCaptureMetadataAddInt32Event");
  auto addDouble = g_metadata.At<DoubleMetadataFn>(10, "XAppCaptureMetadataAddDoubleEvent");
  auto startString = g_metadata.At<StringMetadataFn>(11, "XAppCaptureMetadataStartStringState");
  auto startInt = g_metadata.At<Int32MetadataFn>(12, "XAppCaptureMetadataStartInt32State");
  auto startDouble = g_metadata.At<DoubleMetadataFn>(13, "XAppCaptureMetadataStartDoubleState");
  auto stopState = g_metadata.At<StopStateFn>(14, "XAppCaptureMetadataStopState");
  auto stopAll = g_metadata.At<NoArgFn>(15, "XAppCaptureMetadataStopAllStates");

  bool accepted = addString(g_metadata.self, "map", "harbor", 0) == S_OK && addInt(g_metadata.self, "score", -12, 1) == S_OK &&
                  addDouble(g_metadata.self, "speed", 3.5, 0) == S_OK && startString(g_metadata.self, "mode", "race", 1) == S_OK &&
                  startInt(g_metadata.self, "lap", 2, 0) == S_OK && startDouble(g_metadata.self, "fuel", std::nan(""), 1) == S_OK &&
                  stopState(g_metadata.self, "lap") == S_OK && stopState(g_metadata.self, "never-started") == S_OK && stopAll(g_metadata.self) == S_OK &&
                  addString(g_metadata.self, "empty-value", "", 0) == S_OK;
  Check(accepted, "metadata events and states are accepted without a running capture, as documented");
  std::string big(1 << 20, 'v');
  bool flood = true;
  for (int i = 0; i < 2000 && flood; i++) flood = addString(g_metadata.self, "flood", big.c_str(), 0) == S_OK;
  uint64_t after = 0;
  Check(flood && remaining(g_metadata.self, &after) == S_OK && after == before,
        "a 2 GB metadata flood is accepted and dropped: nothing is retained, remaining storage is unchanged");

  bool invalid = addString(g_metadata.self, nullptr, "v", 0) == E_INVALIDARG && addString(g_metadata.self, "", "v", 0) == E_INVALIDARG &&
                 addString(g_metadata.self, "n", nullptr, 0) == E_INVALIDARG && addString(g_metadata.self, "n", "v", 2) == E_INVALIDARG &&
                 addInt(g_metadata.self, nullptr, 1, 0) == E_INVALIDARG && addInt(g_metadata.self, "n", 1, 9) == E_INVALIDARG &&
                 addDouble(g_metadata.self, "", 1.0, 0) == E_INVALIDARG && addDouble(g_metadata.self, "n", 1.0, 255) == E_INVALIDARG &&
                 startString(g_metadata.self, "n", nullptr, 0) == E_INVALIDARG && startString(g_metadata.self, nullptr, "v", 0) == E_INVALIDARG &&
                 startInt(g_metadata.self, "", 1, 0) == E_INVALIDARG && startInt(g_metadata.self, "n", 1, 2) == E_INVALIDARG &&
                 startDouble(g_metadata.self, nullptr, 1.0, 0) == E_INVALIDARG && startDouble(g_metadata.self, "n", 1.0, 3) == E_INVALIDARG &&
                 stopState(g_metadata.self, nullptr) == E_INVALIDARG && stopState(g_metadata.self, "") == E_INVALIDARG;
  Check(invalid, "metadata calls reject null or empty names, null string values and unknown priorities");
  CheckSilentEvent(17, "XAppCaptureRegisterMetadataPurged", 18, "XAppCaptureUnRegisterMetadataPurged", &g_failures);
}

static void CheckFeature() {
  Api feature = Get(kFeature);
  Check(!feature.At<BOOLEAN (*)(void*, uint32_t)>(3, "XGameRuntimeIsFeatureAvailable")(feature.self, 1),
        "XGameRuntimeIsFeatureAvailable reports XAppCapture unavailable (no capture service)");
}

int main() {
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
  g_threading = Get(kThreading);
  g_user = Get(kUser);
  g_capture = Get(kCapture);
  g_metadata = Get(kMetadata);
  AddLocalUser();
  CheckDiagnostics();
  CheckScreenshots();
  CheckLocalRecording();
  CheckUserRecording();
  CheckRecordToggle();
  CheckBroadcast();
  CheckMetadata();
  CheckFeature();
  Sleep(50);
  Check(g_silent_calls == 0, "no registered broadcast or metadata-purged callback ever fired");
  g_user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(g_user.self, g_local_user);
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
