#pragma once
#include <cstdint>

namespace gtx::xgr::handles {

enum class Type : uint32_t {
  AppCaptureScreenshotStream,
  DisplayTimeoutDeferral,
  GameSaveContainer,
  GameSaveProvider,
  GameSaveUpdate,
  PackageInstallationMonitor,
  PackageMount,
  SpeechSynthesizer,
  SpeechSynthesizerStream,
  StoreContext,
  StoreLicense,
  StoreProductQuery,
  TaskQueue,
  User,
  UserSignOutDeferral,
  GameUiTextEntry,
};

void opened(const void* handle, Type type);
void closed(const void* handle);
void report_leaks();

}  // namespace gtx::xgr::handles
