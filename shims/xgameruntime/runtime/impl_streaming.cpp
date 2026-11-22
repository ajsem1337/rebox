#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "xgr_internal.h"
#include "xtask.h"

namespace gtx::xgr::streaming {
namespace {

constexpr HRESULT kNotInitialized = static_cast<HRESULT>(0x89245400);
constexpr HRESULT kClientNotConnected = static_cast<HRESULT>(0x89245401);
constexpr HRESULT kNoData = static_cast<HRESULT>(0x89245402);
constexpr HRESULT kNoDatacenter = static_cast<HRESULT>(0x89245403);
constexpr HRESULT kNotStreamingController = static_cast<HRESULT>(0x89245404);

using ClientId = uint64_t;
using FramePipelineToken = uint64_t;
using GamepadPhysicality = uint64_t;

enum class ConnectionState : uint32_t { Disconnected, Connected };

struct Version {
  uint16_t major, minor, build, revision;
};

struct DisplayDetails {
  uint32_t preferredWidth;
  uint32_t preferredHeight;
  RECT safeArea;
  uint32_t maxPixels;
  uint32_t maxWidth;
  uint32_t maxHeight;
  uint32_t flags;
};
static_assert(sizeof(DisplayDetails) == 40, "XGameStreamingDisplayDetails layout");

constexpr uint32_t kOperationReplace = 0;
constexpr uint32_t kValueString = 3;
constexpr uint32_t kValueKindCount = 4;

struct TouchControlsStateValue {
  uint32_t valueKind;
  union {
    uint8_t booleanValue;
    int64_t integerValue;
    double doubleValue;
    const char* stringValue;
  };
};

struct TouchControlsStateOperation {
  uint32_t operationKind;
  const char* path;
  TouchControlsStateValue value;
};
static_assert(sizeof(TouchControlsStateOperation) == 32, "XGameStreamingTouchControlsStateOperation layout");

std::atomic<bool> g_initialized{false};
std::atomic<uint64_t> g_next_token{1};
std::mutex g_tokens_mutex;
std::unordered_set<uint64_t> g_live_tokens;

bool initialized() { return g_initialized.load(); }

HRESULT validate_operations(size_t count, const TouchControlsStateOperation* operations) {
  if (count && !operations) return E_INVALIDARG;
  for (size_t i = 0; i < count; i++) {
    const TouchControlsStateOperation& operation = operations[i];
    if (operation.operationKind != kOperationReplace || !operation.path || operation.value.valueKind >= kValueKindCount) return E_INVALIDARG;
    if (operation.value.valueKind == kValueString && !operation.value.stringValue) return E_INVALIDARG;
  }
  return S_OK;
}

HRESULT XGameStreamingInitialize_impl(void*) {
  g_initialized = true;
  return S_OK;
}

void XGameStreamingUninitialize_impl(void*) {
  g_initialized = false;
  std::lock_guard<std::mutex> lock(g_tokens_mutex);
  g_live_tokens.clear();
}

uint8_t XGameStreamingIsStreaming_impl(void*) { return 0; }

HRESULT XGameStreamingRegisterClientPropertiesChanged_impl(void*, ClientId, task::Queue*, void*, const void* callback, task::RegistrationToken* token) {
  if (!initialized()) return kNotInitialized;
  if (!callback || !token) return E_INVALIDARG;
  return kClientNotConnected;
}

uint8_t XGameStreamingUnregisterClientPropertiesChanged_impl(void*, ClientId, task::RegistrationToken, uint8_t) { return 0; }

HRESULT XGameStreamingGetStreamPhysicalDimensions_impl(void*, ClientId, uint32_t* horizontalMm, uint32_t* verticalMm) {
  if (!initialized()) return kNotInitialized;
  if (!horizontalMm || !verticalMm) return E_INVALIDARG;
  return kClientNotConnected;
}

uint32_t XGameStreamingGetClientCount_impl(void*) { return 0; }

HRESULT XGameStreamingGetClients_impl(void*, uint32_t clientCount, ClientId* clients, uint32_t* clientsUsed) {
  if (!initialized()) return kNotInitialized;
  if (clientCount && !clients) return E_INVALIDARG;
  if (clientsUsed) *clientsUsed = 0;
  return S_OK;
}

ConnectionState XGameStreamingGetConnectionState_impl(void*, ClientId) { return ConnectionState::Disconnected; }

HRESULT XGameStreamingRegisterConnectionStateChanged_impl(void*, task::Queue*, void*, const void* callback, task::RegistrationToken* token) {
  if (!initialized()) return kNotInitialized;
  if (!callback || !token) return E_INVALIDARG;
  token->token = g_next_token++;
  {
    std::lock_guard<std::mutex> lock(g_tokens_mutex);
    g_live_tokens.insert(token->token);
  }
  return S_OK;
}

uint8_t XGameStreamingUnregisterConnectionStateChanged_impl(void*, task::RegistrationToken token, uint8_t) {
  std::lock_guard<std::mutex> lock(g_tokens_mutex);
  return g_live_tokens.erase(token.token) ? 1 : 0;
}

HRESULT XGameStreamingGetStreamAddedLatency_impl(void*, ClientId, uint32_t* averageInputLatencyUs, uint32_t* averageOutputLatencyUs,
                                                 uint32_t* standardDeviationUs) {
  if (!initialized()) return kNotInitialized;
  if (!averageInputLatencyUs || !averageOutputLatencyUs || !standardDeviationUs) return E_INVALIDARG;
  return kNoData;
}

size_t XGameStreamingGetServerLocationNameSize_impl(void*) { return 0; }

HRESULT XGameStreamingGetServerLocationName_impl(void*, size_t serverLocationNameSize, char* serverLocationName) {
  if (!initialized()) return kNotInitialized;
  if (!serverLocationName) return E_INVALIDARG;
  if (serverLocationNameSize) serverLocationName[0] = '\0';
  return kNoDatacenter;
}

void XGameStreamingHideTouchControls_impl(void*) {}
void XGameStreamingShowTouchControlLayout_impl(void*, const char*) {}
void XGameStreamingHideTouchControlsOnClient_impl(void*, ClientId) {}
void XGameStreamingShowTouchControlLayoutOnClient_impl(void*, ClientId, const char*) {}

HRESULT XGameStreamingIsTouchInputEnabled_impl(void*, ClientId, uint8_t* touchInputEnabled) {
  if (!initialized()) return kNotInitialized;
  if (!touchInputEnabled) return E_INVALIDARG;
  *touchInputEnabled = 0;
  return kClientNotConnected;
}

HRESULT XGameStreamingGetLastFrameDisplayed_impl(void*, ClientId, FramePipelineToken* framePipelineToken) {
  if (!initialized()) return kNotInitialized;
  if (!framePipelineToken) return E_INVALIDARG;
  return kClientNotConnected;
}

HRESULT XGameStreamingGetAssociatedFrame_impl(void*, void* gamepadReading, FramePipelineToken* framePipelineToken) {
  if (!initialized()) return kNotInitialized;
  if (!gamepadReading || !framePipelineToken) return E_INVALIDARG;
  return kNotStreamingController;
}

HRESULT XGameStreamingGetGamepadPhysicality_impl(void*, void* gamepadReading, GamepadPhysicality* gamepadPhysicality) {
  if (!initialized()) return kNotInitialized;
  if (!gamepadReading || !gamepadPhysicality) return E_INVALIDARG;
  return kNotStreamingController;
}

HRESULT XGameStreamingUpdateTouchControlsState_impl(void*, size_t operationCount, const TouchControlsStateOperation* operations) {
  if (!initialized()) return kNotInitialized;
  return validate_operations(operationCount, operations);
}

HRESULT XGameStreamingUpdateTouchControlsStateOnClient_impl(void*, ClientId, size_t operationCount, const TouchControlsStateOperation* operations) {
  if (!initialized()) return kNotInitialized;
  HRESULT hr = validate_operations(operationCount, operations);
  return FAILED(hr) ? hr : kClientNotConnected;
}

HRESULT XGameStreamingShowTouchControlsWithStateUpdate_impl(void*, const char*, size_t operationCount, const TouchControlsStateOperation* operations) {
  if (!initialized()) return kNotInitialized;
  return validate_operations(operationCount, operations);
}

HRESULT XGameStreamingShowTouchControlsWithStateUpdateOnClient_impl(void*, ClientId, const char*, size_t operationCount,
                                                                    const TouchControlsStateOperation* operations) {
  if (!initialized()) return kNotInitialized;
  HRESULT hr = validate_operations(operationCount, operations);
  return FAILED(hr) ? hr : kClientNotConnected;
}

size_t XGameStreamingGetTouchBundleVersionNameSize_impl(void*, ClientId) { return 0; }

HRESULT XGameStreamingGetTouchBundleVersion_impl(void*, ClientId, Version*, size_t versionNameSize, char* versionName) {
  if (!initialized()) return kNotInitialized;
  if (versionNameSize && !versionName) return E_INVALIDARG;
  return kClientNotConnected;
}

HRESULT XGameStreamingGetClientIPAddress_impl(void*, ClientId, size_t, char* ipAddress) {
  if (!initialized()) return kNotInitialized;
  if (!ipAddress) return E_INVALIDARG;
  return kClientNotConnected;
}

HRESULT XGameStreamingGetSessionId_impl(void*, ClientId, size_t, char* sessionId, size_t* sessionIdUsed) {
  if (!initialized()) return kNotInitialized;
  if (!sessionId) return E_INVALIDARG;
  if (sessionIdUsed) *sessionIdUsed = 0;
  return kClientNotConnected;
}

constexpr float kStandardAspectRatio = 16.0f / 9.0f;

HRESULT XGameStreamingGetDisplayDetails_impl(void*, ClientId, uint32_t maxSupportedPixels, float widestSupportedAspectRatio,
                                             float tallestSupportedAspectRatio, DisplayDetails* displayDetails) {
  if (!initialized()) return kNotInitialized;
  bool widestValid = std::isfinite(widestSupportedAspectRatio) && widestSupportedAspectRatio >= kStandardAspectRatio;
  bool tallestValid = std::isfinite(tallestSupportedAspectRatio) && tallestSupportedAspectRatio > 0.0f && tallestSupportedAspectRatio <= kStandardAspectRatio;
  if (!displayDetails || !maxSupportedPixels || !widestValid || !tallestValid) return E_INVALIDARG;
  return kClientNotConnected;
}

constexpr uint32_t kMinimumStreamWidth = 640;
constexpr uint32_t kMinimumStreamHeight = 360;
constexpr uint32_t kStreamDimensionAlignment = 8;

bool is_standard_resolution(uint32_t width, uint32_t height) {
  return (width == 1280 && height == 720) || (width == 1920 && height == 1080) || (width == 3840 && height == 2160);
}

HRESULT XGameStreamingSetResolution_impl(void*, uint32_t width, uint32_t height) {
  if (!initialized()) return kNotInitialized;
  if (width < kMinimumStreamWidth || height < kMinimumStreamHeight || width % kStreamDimensionAlignment || height % kStreamDimensionAlignment)
    return E_INVALIDARG;
  return is_standard_resolution(width, height) ? S_OK : E_INVALIDARG;
}

const Override kOverrides[] = {
    {"XGameStreamingInitialize", reinterpret_cast<void*>(&XGameStreamingInitialize_impl)},
    {"XGameStreamingUninitialize", reinterpret_cast<void*>(&XGameStreamingUninitialize_impl)},
    {"XGameStreamingIsStreaming", reinterpret_cast<void*>(&XGameStreamingIsStreaming_impl)},
    {"XGameStreamingRegisterClientPropertiesChanged", reinterpret_cast<void*>(&XGameStreamingRegisterClientPropertiesChanged_impl)},
    {"XGameStreamingUnregisterClientPropertiesChanged", reinterpret_cast<void*>(&XGameStreamingUnregisterClientPropertiesChanged_impl)},
    {"XGameStreamingGetStreamPhysicalDimensions", reinterpret_cast<void*>(&XGameStreamingGetStreamPhysicalDimensions_impl)},
    {"XGameStreamingGetClientCount", reinterpret_cast<void*>(&XGameStreamingGetClientCount_impl)},
    {"XGameStreamingGetClients", reinterpret_cast<void*>(&XGameStreamingGetClients_impl)},
    {"XGameStreamingGetConnectionState", reinterpret_cast<void*>(&XGameStreamingGetConnectionState_impl)},
    {"XGameStreamingRegisterConnectionStateChanged", reinterpret_cast<void*>(&XGameStreamingRegisterConnectionStateChanged_impl)},
    {"XGameStreamingUnregisterConnectionStateChanged", reinterpret_cast<void*>(&XGameStreamingUnregisterConnectionStateChanged_impl)},
    {"XGameStreamingGetStreamAddedLatency", reinterpret_cast<void*>(&XGameStreamingGetStreamAddedLatency_impl)},
    {"XGameStreamingGetServerLocationNameSize", reinterpret_cast<void*>(&XGameStreamingGetServerLocationNameSize_impl)},
    {"XGameStreamingGetServerLocationName", reinterpret_cast<void*>(&XGameStreamingGetServerLocationName_impl)},
    {"XGameStreamingHideTouchControls", reinterpret_cast<void*>(&XGameStreamingHideTouchControls_impl)},
    {"XGameStreamingShowTouchControlLayout", reinterpret_cast<void*>(&XGameStreamingShowTouchControlLayout_impl)},
    {"XGameStreamingHideTouchControlsOnClient", reinterpret_cast<void*>(&XGameStreamingHideTouchControlsOnClient_impl)},
    {"XGameStreamingShowTouchControlLayoutOnClient", reinterpret_cast<void*>(&XGameStreamingShowTouchControlLayoutOnClient_impl)},
    {"XGameStreamingIsTouchInputEnabled", reinterpret_cast<void*>(&XGameStreamingIsTouchInputEnabled_impl)},
    {"XGameStreamingGetLastFrameDisplayed", reinterpret_cast<void*>(&XGameStreamingGetLastFrameDisplayed_impl)},
    {"XGameStreamingGetAssociatedFrame", reinterpret_cast<void*>(&XGameStreamingGetAssociatedFrame_impl)},
    {"XGameStreamingGetGamepadPhysicality", reinterpret_cast<void*>(&XGameStreamingGetGamepadPhysicality_impl)},
    {"XGameStreamingUpdateTouchControlsState", reinterpret_cast<void*>(&XGameStreamingUpdateTouchControlsState_impl)},
    {"XGameStreamingUpdateTouchControlsStateOnClient", reinterpret_cast<void*>(&XGameStreamingUpdateTouchControlsStateOnClient_impl)},
    {"XGameStreamingShowTouchControlsWithStateUpdate", reinterpret_cast<void*>(&XGameStreamingShowTouchControlsWithStateUpdate_impl)},
    {"XGameStreamingShowTouchControlsWithStateUpdateOnClient", reinterpret_cast<void*>(&XGameStreamingShowTouchControlsWithStateUpdateOnClient_impl)},
    {"XGameStreamingGetTouchBundleVersionNameSize", reinterpret_cast<void*>(&XGameStreamingGetTouchBundleVersionNameSize_impl)},
    {"XGameStreamingGetTouchBundleVersion", reinterpret_cast<void*>(&XGameStreamingGetTouchBundleVersion_impl)},
    {"XGameStreamingGetClientIPAddress", reinterpret_cast<void*>(&XGameStreamingGetClientIPAddress_impl)},
    {"XGameStreamingGetSessionId", reinterpret_cast<void*>(&XGameStreamingGetSessionId_impl)},
    {"XGameStreamingGetDisplayDetails", reinterpret_cast<void*>(&XGameStreamingGetDisplayDetails_impl)},
    {"XGameStreamingSetResolution", reinterpret_cast<void*>(&XGameStreamingSetResolution_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::streaming
