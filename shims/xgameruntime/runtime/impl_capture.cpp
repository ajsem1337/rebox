#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "gtx/log.h"
#include "xgr_internal.h"
#include "xtask.h"

namespace gtx::xgr::capture {
namespace {

constexpr HRESULT kGameDvrBlockedByTitle = static_cast<HRESULT>(0x82323018);
constexpr HRESULT kGameDvrDisabledByCaptureSetting = static_cast<HRESULT>(0x8232301E);
constexpr HRESULT kStopRecordWithoutStart = static_cast<HRESULT>(0x82323014);

constexpr uint16_t kScreenshotFormatSdr = 1;
constexpr uint16_t kScreenshotFormatHdr = 2;
constexpr uint8_t kMetadataPriorityCount = 2;
constexpr size_t kLocalIdBytes = 250;
constexpr size_t kFilenamePrefixMaxBytes = 128;
constexpr uint64_t kMetadataStorageBytes = 0;

struct BroadcastStatus {
  uint8_t canStartBroadcast;
  uint8_t isAnyAppBroadcasting;
  uint8_t isCaptureResourceUnavailable;
  uint8_t isGameStreamInProgress;
  uint8_t isGpuConstrained;
  uint8_t isAppInactive;
  uint8_t isBlockedForApp;
  uint8_t isDisabledByUser;
  uint8_t isDisabledBySystem;
};
static_assert(sizeof(BroadcastStatus) == 9, "XAppBroadcastStatus layout");

struct ScreenshotFile {
  char path[260];
  size_t fileSize;
  uint32_t width;
  uint32_t height;
};
struct TakeScreenshotResult {
  char localId[kLocalIdBytes];
  uint16_t availableScreenshotFormats;
};
static_assert(sizeof(TakeScreenshotResult) == 252, "XAppCaptureTakeScreenshotResult layout");

struct DiagnosticScreenshotResult {
  size_t fileCount;
  ScreenshotFile files[10];
};
static_assert(sizeof(DiagnosticScreenshotResult) == 2808, "XAppCaptureDiagnosticScreenshotResult layout");

struct RecordClipResult {
  char path[260];
  size_t fileSize;
  uint64_t startTime;
  uint32_t durationInMs;
  uint32_t width;
  uint32_t height;
  uint8_t encoding;
};
static_assert(sizeof(RecordClipResult) == 296, "XAppCaptureRecordClipResult layout");

struct LocalResult {
  void* clipHandle;
  size_t fileSizeInBytes;
  SYSTEMTIME clipStartTimestamp;
  uint64_t durationInMilliseconds;
  uint32_t width;
  uint32_t height;
  uint8_t encoding;
  uint8_t colorFormat;
};
static_assert(sizeof(LocalResult) == 56, "XAppCaptureLocalResult layout");

struct UserRecordingResult {
  size_t fileSizeInBytes;
  SYSTEMTIME clipStartTimestamp;
  uint64_t durationInMilliseconds;
  uint32_t width;
  uint32_t height;
  uint8_t encoding;
  uint8_t colorFormat;
};
static_assert(sizeof(UserRecordingResult) == 48, "XAppCaptureUserRecordingResult layout");

struct VideoCaptureSettings {
  uint32_t width;
  uint32_t height;
  uint64_t maxRecordTimespanDurationInMs;
  uint8_t encoding;
  uint8_t colorFormat;
  uint8_t isCaptureByGamesAllowed;
};
static_assert(sizeof(VideoCaptureSettings) == 24, "XAppCaptureVideoCaptureSettings layout");

std::atomic<bool> g_record_enabled_by_title{true};
std::atomic<uint64_t> g_next_token{1};

HRESULT capture_refusal() {
  return g_record_enabled_by_title ? kGameDvrDisabledByCaptureSetting : kGameDvrBlockedByTitle;
}

bool is_filename_prefix_safe(const char* prefix) {
  if (!prefix) return true;
  size_t length = strnlen(prefix, kFilenamePrefixMaxBytes + 1);
  if (length > kFilenamePrefixMaxBytes) return false;
  if (std::strstr(prefix, "..")) return false;
  for (size_t i = 0; i < length; i++) {
    unsigned char c = static_cast<unsigned char>(prefix[i]);
    if (c < 0x20 || c == 0x7f || std::strchr("\\/:*?\"<>|", c)) return false;
  }
  return true;
}

bool is_local_id_well_formed(const char* id) {
  if (!id) return false;
  size_t length = strnlen(id, kLocalIdBytes);
  return length > 0 && length < kLocalIdBytes;
}

bool is_single_screenshot_format(uint16_t format) {
  return format == kScreenshotFormatSdr || format == kScreenshotFormatHdr;
}

bool is_screenshot_format_set(uint16_t flags) {
  return flags != 0 && (flags & ~(kScreenshotFormatSdr | kScreenshotFormatHdr)) == 0;
}

bool is_metadata_name_valid(const char* name) { return name && *name; }

bool is_metadata_priority_valid(uint8_t priority) { return priority < kMetadataPriorityCount; }

HRESULT register_silent_event(const void* callback, task::RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  token->token = g_next_token++;
  return S_OK;
}

HRESULT XAppCaptureTakeScreenshot_impl(void*, void* requestingUser, TakeScreenshotResult* result) {
  if (!result) return E_INVALIDARG;
  std::memset(result, 0, sizeof(*result));
  if (!requestingUser) return E_INVALIDARG;
  return capture_refusal();
}

HRESULT XAppCaptureTakeDiagnosticScreenshot_impl(void*, uint8_t, uint16_t captureFlags, const char* filenamePrefix,
                                                 DiagnosticScreenshotResult* result) {
  if (!result) return E_INVALIDARG;
  std::memset(result, 0, sizeof(*result));
  if (!is_screenshot_format_set(captureFlags) || !is_filename_prefix_safe(filenamePrefix)) return E_INVALIDARG;
  return capture_refusal();
}

HRESULT XAppCaptureRecordDiagnosticClip_impl(void*, uint64_t, uint32_t durationInMs, const char* filenamePrefix, RecordClipResult* result) {
  if (!result) return E_INVALIDARG;
  std::memset(result, 0, sizeof(*result));
  if (!durationInMs || !is_filename_prefix_safe(filenamePrefix)) return E_INVALIDARG;
  return capture_refusal();
}

HRESULT XAppCaptureOpenScreenshotStream_impl(void*, const char* localId, uint16_t screenshotFormat, void** handle, uint64_t* totalBytes) {
  if (handle) *handle = nullptr;
  if (totalBytes) *totalBytes = 0;
  if (!handle || !is_local_id_well_formed(localId) || !is_single_screenshot_format(screenshotFormat)) return E_INVALIDARG;
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT read_from_unknown_stream(void* handle, uint32_t bytesToRead, uint8_t* buffer, uint32_t* bytesWritten) {
  if (bytesWritten) *bytesWritten = 0;
  if (!handle || !bytesWritten || (bytesToRead && !buffer)) return E_INVALIDARG;
  return E_HANDLE;
}

HRESULT XAppCaptureReadScreenshotStream_impl(void*, void* handle, uint64_t, uint32_t bytesToRead, uint8_t* buffer, uint32_t* bytesWritten) {
  return read_from_unknown_stream(handle, bytesToRead, buffer, bytesWritten);
}

HRESULT close_unknown_stream(void* handle) { return handle ? E_HANDLE : E_INVALIDARG; }

HRESULT XAppCaptureCloseScreenshotStream_impl(void*, void* handle) { return close_unknown_stream(handle); }

HRESULT XAppCaptureEnableRecord_impl(void*) {
  g_record_enabled_by_title = true;
  return S_OK;
}

HRESULT XAppCaptureDisableRecord_impl(void*) {
  g_record_enabled_by_title = false;
  return S_OK;
}

HRESULT XAppCaptureGetVideoCaptureSettings_impl(void*, VideoCaptureSettings* settings) {
  if (!settings) return E_INVALIDARG;
  std::memset(settings, 0, sizeof(*settings));
  return S_OK;
}

HRESULT XAppCaptureRecordTimespan_impl(void*, const SYSTEMTIME*, uint64_t durationInMilliseconds, LocalResult* result) {
  if (!result) return E_INVALIDARG;
  std::memset(result, 0, sizeof(*result));
  if (!durationInMilliseconds) return E_INVALIDARG;
  return E_ACCESSDENIED;
}

HRESULT XAppCaptureReadLocalStream_impl(void*, void* handle, size_t, uint32_t bytesToRead, uint8_t* buffer, uint32_t* bytesWritten) {
  return read_from_unknown_stream(handle, bytesToRead, buffer, bytesWritten);
}

HRESULT XAppCaptureCloseLocalStream_impl(void*, void* handle) { return close_unknown_stream(handle); }

HRESULT XAppCaptureStartUserRecord_impl(void*, void* requestingUser, uint32_t localIdBufferLength, char* localIdBuffer) {
  if (localIdBuffer && localIdBufferLength) localIdBuffer[0] = '\0';
  if (!requestingUser || !localIdBuffer || !localIdBufferLength) return E_INVALIDARG;
  return capture_refusal();
}

HRESULT XAppCaptureStopUserRecord_impl(void*, const char* localId, UserRecordingResult* result) {
  if (!result) return E_INVALIDARG;
  std::memset(result, 0, sizeof(*result));
  if (!is_local_id_well_formed(localId)) return E_INVALIDARG;
  return kStopRecordWithoutStart;
}

HRESULT XAppCaptureCancelUserRecord_impl(void*, const char* localId) {
  if (!is_local_id_well_formed(localId)) return E_INVALIDARG;
  return kStopRecordWithoutStart;
}

uint8_t XAppBroadcastIsAppBroadcasting_impl(void*) { return 0; }

HRESULT XAppBroadcastShowUI_impl(void*, void* requestingUser) {
  if (!requestingUser) return E_INVALIDARG;
  return E_ABORT;
}

HRESULT XAppBroadcastGetStatus_impl(void*, void* requestingUser, BroadcastStatus* status) {
  if (!requestingUser || !status) return E_INVALIDARG;
  std::memset(status, 0, sizeof(*status));
  status->isCaptureResourceUnavailable = 1;
  status->isBlockedForApp = g_record_enabled_by_title ? 0 : 1;
  return S_OK;
}

HRESULT XAppBroadcastRegisterIsAppBroadcastingChanged_impl(void*, task::Queue*, void*, const void* callback, task::RegistrationToken* token) {
  return register_silent_event(callback, token);
}

uint8_t XAppBroadcastUnregisterIsAppBroadcastingChanged_impl(void*, task::RegistrationToken, uint8_t) { return 1; }

HRESULT accept_metadata(const char* name, uint8_t priority) {
  return is_metadata_name_valid(name) && is_metadata_priority_valid(priority) ? S_OK : E_INVALIDARG;
}

HRESULT XAppCaptureMetadataAddStringEvent_impl(void*, const char* name, const char* value, uint8_t priority) {
  return value ? accept_metadata(name, priority) : E_INVALIDARG;
}
HRESULT XAppCaptureMetadataAddInt32Event_impl(void*, const char* name, int32_t, uint8_t priority) { return accept_metadata(name, priority); }
HRESULT XAppCaptureMetadataAddDoubleEvent_impl(void*, const char* name, double, uint8_t priority) { return accept_metadata(name, priority); }
HRESULT XAppCaptureMetadataStartStringState_impl(void*, const char* name, const char* value, uint8_t priority) {
  return value ? accept_metadata(name, priority) : E_INVALIDARG;
}
HRESULT XAppCaptureMetadataStartInt32State_impl(void*, const char* name, int32_t, uint8_t priority) { return accept_metadata(name, priority); }
HRESULT XAppCaptureMetadataStartDoubleState_impl(void*, const char* name, double, uint8_t priority) { return accept_metadata(name, priority); }
HRESULT XAppCaptureMetadataStopState_impl(void*, const char* name) { return is_metadata_name_valid(name) ? S_OK : E_INVALIDARG; }
HRESULT XAppCaptureMetadataStopAllStates_impl(void*) { return S_OK; }

HRESULT XAppCaptureMetadataRemainingStorageBytesAvailable_impl(void*, uint64_t* value) {
  if (!value) return E_INVALIDARG;
  *value = kMetadataStorageBytes;
  return S_OK;
}

HRESULT XAppCaptureRegisterMetadataPurged_impl(void*, task::Queue*, void*, const void* callback, task::RegistrationToken* token) {
  return register_silent_event(callback, token);
}

uint8_t XAppCaptureUnRegisterMetadataPurged_impl(void*, task::RegistrationToken, uint8_t) { return 1; }

const Override kOverrides[] = {
    {"XAppCaptureTakeScreenshot", reinterpret_cast<void*>(&XAppCaptureTakeScreenshot_impl)},
    {"XAppCaptureTakeDiagnosticScreenshot", reinterpret_cast<void*>(&XAppCaptureTakeDiagnosticScreenshot_impl)},
    {"XAppCaptureRecordDiagnosticClip", reinterpret_cast<void*>(&XAppCaptureRecordDiagnosticClip_impl)},
    {"XAppCaptureOpenScreenshotStream", reinterpret_cast<void*>(&XAppCaptureOpenScreenshotStream_impl)},
    {"XAppCaptureReadScreenshotStream", reinterpret_cast<void*>(&XAppCaptureReadScreenshotStream_impl)},
    {"XAppCaptureCloseScreenshotStream", reinterpret_cast<void*>(&XAppCaptureCloseScreenshotStream_impl)},
    {"XAppCaptureEnableRecord", reinterpret_cast<void*>(&XAppCaptureEnableRecord_impl)},
    {"XAppCaptureDisableRecord", reinterpret_cast<void*>(&XAppCaptureDisableRecord_impl)},
    {"XAppCaptureGetVideoCaptureSettings", reinterpret_cast<void*>(&XAppCaptureGetVideoCaptureSettings_impl)},
    {"XAppCaptureRecordTimespan", reinterpret_cast<void*>(&XAppCaptureRecordTimespan_impl)},
    {"XAppCaptureReadLocalStream", reinterpret_cast<void*>(&XAppCaptureReadLocalStream_impl)},
    {"XAppCaptureCloseLocalStream", reinterpret_cast<void*>(&XAppCaptureCloseLocalStream_impl)},
    {"XAppCaptureStartUserRecord", reinterpret_cast<void*>(&XAppCaptureStartUserRecord_impl)},
    {"XAppCaptureStopUserRecord", reinterpret_cast<void*>(&XAppCaptureStopUserRecord_impl)},
    {"XAppCaptureCancelUserRecord", reinterpret_cast<void*>(&XAppCaptureCancelUserRecord_impl)},
    {"XAppBroadcastIsAppBroadcasting", reinterpret_cast<void*>(&XAppBroadcastIsAppBroadcasting_impl)},
    {"XAppBroadcastShowUI", reinterpret_cast<void*>(&XAppBroadcastShowUI_impl)},
    {"XAppBroadcastGetStatus", reinterpret_cast<void*>(&XAppBroadcastGetStatus_impl)},
    {"XAppBroadcastRegisterIsAppBroadcastingChanged", reinterpret_cast<void*>(&XAppBroadcastRegisterIsAppBroadcastingChanged_impl)},
    {"XAppBroadcastUnregisterIsAppBroadcastingChanged", reinterpret_cast<void*>(&XAppBroadcastUnregisterIsAppBroadcastingChanged_impl)},
    {"XAppCaptureMetadataAddStringEvent", reinterpret_cast<void*>(&XAppCaptureMetadataAddStringEvent_impl)},
    {"XAppCaptureMetadataAddInt32Event", reinterpret_cast<void*>(&XAppCaptureMetadataAddInt32Event_impl)},
    {"XAppCaptureMetadataAddDoubleEvent", reinterpret_cast<void*>(&XAppCaptureMetadataAddDoubleEvent_impl)},
    {"XAppCaptureMetadataStartStringState", reinterpret_cast<void*>(&XAppCaptureMetadataStartStringState_impl)},
    {"XAppCaptureMetadataStartInt32State", reinterpret_cast<void*>(&XAppCaptureMetadataStartInt32State_impl)},
    {"XAppCaptureMetadataStartDoubleState", reinterpret_cast<void*>(&XAppCaptureMetadataStartDoubleState_impl)},
    {"XAppCaptureMetadataStopState", reinterpret_cast<void*>(&XAppCaptureMetadataStopState_impl)},
    {"XAppCaptureMetadataStopAllStates", reinterpret_cast<void*>(&XAppCaptureMetadataStopAllStates_impl)},
    {"XAppCaptureMetadataRemainingStorageBytesAvailable", reinterpret_cast<void*>(&XAppCaptureMetadataRemainingStorageBytesAvailable_impl)},
    {"XAppCaptureRegisterMetadataPurged", reinterpret_cast<void*>(&XAppCaptureRegisterMetadataPurged_impl)},
    {"XAppCaptureUnRegisterMetadataPurged", reinterpret_cast<void*>(&XAppCaptureUnRegisterMetadataPurged_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::capture
