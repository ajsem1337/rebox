#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "handles.h"
#include "xgr_internal.h"

namespace gtx::xgr::system { void report_error(HRESULT hr, const char* message); }

namespace gtx::xgr::handles {
namespace {

enum class Reason : uint32_t { Created, Destroyed };

using HandleCallback = void(CALLBACK*)(void* handle, Type type, Reason reason, void* context);

struct Tracked {
  Type type;
  uint64_t references;
};

std::mutex g_lock;
std::map<const void*, Tracked> g_tracked;
HandleCallback g_callback = nullptr;
void* g_context = nullptr;

const char* type_name(Type type) {
  static const char* const kNames[] = {
      "AppCaptureScreenshotStream", "DisplayTimeoutDeferral", "GameSaveContainer", "GameSaveProvider", "GameSaveUpdate",
      "PackageInstallationMonitor", "PackageMount",           "SpeechSynthesizer", "SpeechSynthesizerStream", "StoreContext",
      "StoreLicense",               "StoreProductQuery",      "TaskQueue",         "User",                    "UserSignOutDeferral",
      "GameUiTextEntry",
  };
  uint32_t index = static_cast<uint32_t>(type);
  return index < sizeof(kNames) / sizeof(kNames[0]) ? kNames[index] : "Unknown";
}

void notify(HandleCallback callback, void* context, const void* handle, Type type, Reason reason) {
  if (callback) callback(const_cast<void*>(handle), type, reason, context);
}

HRESULT XSystemHandleTrack_impl(void*, HandleCallback callback, void* context) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_callback = callback;
  g_context = callback ? context : nullptr;
  return S_OK;
}

uint8_t XSystemIsHandleValid_impl(void*, void* handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lock(g_lock);
  return g_tracked.count(handle) ? 1 : 0;
}

const Override kOverrides[] = {
    {"XSystemHandleTrack", reinterpret_cast<void*>(&XSystemHandleTrack_impl)},
    {"XSystemIsHandleValid", reinterpret_cast<void*>(&XSystemIsHandleValid_impl)},
};

}  // namespace

void opened(const void* handle, Type type) {
  if (!handle) return;
  HandleCallback callback;
  void* context;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto found = g_tracked.find(handle);
    if (found != g_tracked.end()) {
      found->second.references++;
      return;
    }
    g_tracked.emplace(handle, Tracked{type, 1});
    callback = g_callback;
    context = g_context;
  }
  notify(callback, context, handle, type, Reason::Created);
}

void closed(const void* handle) {
  if (!handle) return;
  HandleCallback callback;
  void* context;
  Type type;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto found = g_tracked.find(handle);
    if (found == g_tracked.end()) return;
    if (--found->second.references) return;
    type = found->second.type;
    g_tracked.erase(found);
    callback = g_callback;
    context = g_context;
  }
  notify(callback, context, handle, type, Reason::Destroyed);
}

void report_leaks() {
  std::vector<std::pair<const void*, Type>> leaked;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    for (const auto& entry : g_tracked) leaked.emplace_back(entry.first, entry.second.type);
  }
  for (const auto& entry : leaked) {
    char message[160];
    std::snprintf(message, sizeof(message), "XGameRuntimeUninitialize: %s handle %p was never closed", type_name(entry.second), entry.first);
    system::report_error(E_FAIL, message);
  }
}

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::handles
