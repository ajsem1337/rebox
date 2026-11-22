#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>
// rebox local change: mingw's WinRT headers can't be included here (IReference<unsigned
// char> is defined twice), so without MSVC the Windows caption settings aren't read and
// XClosedCaptionGetProperties reports the defaults.
#ifdef _MSC_VER
#include <roapi.h>
#include <winstring.h>
#include <windows.media.closedcaptioning.h>
#define XGR_WINRT_CAPTIONS 1
#endif

#include <atomic>
#include <cstdint>
#include <thread>

#include "gtx/log.h"
#include "xgr_internal.h"

namespace gtx::xgr::accessibility {
namespace {

#ifdef XGR_WINRT_CAPTIONS
namespace cc = ABI::Windows::Media::ClosedCaptioning;
#endif

struct Color {
  uint8_t a, r, g, b;
};
static_assert(sizeof(Color) == 4, "XColor layout");

struct ClosedCaptionProperties {
  Color backgroundColor;
  Color fontColor;
  Color windowColor;
  uint32_t fontEdgeAttribute;
  uint32_t fontStyle;
  float fontScale;
  uint8_t enabled;
};
static_assert(sizeof(ClosedCaptionProperties) == 28, "XClosedCaptionProperties layout");

constexpr uint32_t kFontEdgeAttributeCount = 6;
constexpr uint32_t kFontStyleCount = 8;
constexpr float kDefaultFontScale = 1.0f;

std::atomic<bool> g_captions_enabled{false};

#ifdef XGR_WINRT_CAPTIONS
float font_scale_for(cc::ClosedCaptionSize size) {
  switch (size) {
    case cc::ClosedCaptionSize_FiftyPercent: return 0.5f;
    case cc::ClosedCaptionSize_OneHundredFiftyPercent: return 1.5f;
    case cc::ClosedCaptionSize_TwoHundredPercent: return 2.0f;
    default: return kDefaultFontScale;
  }
}

Color to_color(const ABI::Windows::UI::Color& color) { return {color.A, color.R, color.G, color.B}; }

struct WinRtEntryPoints {
  HMODULE module;
  decltype(&RoInitialize) initialize;
  decltype(&RoUninitialize) uninitialize;
  decltype(&RoGetActivationFactory) getActivationFactory;
  decltype(&WindowsCreateStringReference) createStringReference;
};

template <typename Fn> Fn entry_point(HMODULE module, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

bool load_winrt(WinRtEntryPoints* winrt) {
  winrt->module = LoadLibraryW(L"combase.dll");
  if (!winrt->module) return false;
  winrt->initialize = entry_point<decltype(&RoInitialize)>(winrt->module, "RoInitialize");
  winrt->uninitialize = entry_point<decltype(&RoUninitialize)>(winrt->module, "RoUninitialize");
  winrt->getActivationFactory = entry_point<decltype(&RoGetActivationFactory)>(winrt->module, "RoGetActivationFactory");
  winrt->createStringReference = entry_point<decltype(&WindowsCreateStringReference)>(winrt->module, "WindowsCreateStringReference");
  if (winrt->initialize && winrt->uninitialize && winrt->getActivationFactory && winrt->createStringReference) return true;
  FreeLibrary(winrt->module);
  return false;
}

bool read_statics(cc::IClosedCaptionPropertiesStatics* statics, ClosedCaptionProperties* out) {
  ABI::Windows::UI::Color font{}, background{}, region{};
  cc::ClosedCaptionEdgeEffect effect = cc::ClosedCaptionEdgeEffect_Default;
  cc::ClosedCaptionStyle style = cc::ClosedCaptionStyle_Default;
  cc::ClosedCaptionSize size = cc::ClosedCaptionSize_Default;
  if (FAILED(statics->get_ComputedFontColor(&font)) || FAILED(statics->get_ComputedBackgroundColor(&background)) ||
      FAILED(statics->get_ComputedRegionColor(&region)) || FAILED(statics->get_FontEffect(&effect)) ||
      FAILED(statics->get_FontStyle(&style)) || FAILED(statics->get_FontSize(&size)))
    return false;
  out->backgroundColor = to_color(background);
  out->fontColor = to_color(font);
  out->windowColor = to_color(region);
  out->fontEdgeAttribute = static_cast<uint32_t>(effect) < kFontEdgeAttributeCount ? static_cast<uint32_t>(effect) : 0;
  out->fontStyle = static_cast<uint32_t>(style) < kFontStyleCount ? static_cast<uint32_t>(style) : 0;
  out->fontScale = font_scale_for(size);
  return true;
}

bool query_windows_caption_settings(ClosedCaptionProperties* out) {
  WinRtEntryPoints winrt{};
  if (!load_winrt(&winrt)) return false;
  bool read = false;
  std::thread worker([&] {
    if (FAILED(winrt.initialize(RO_INIT_MULTITHREADED))) return;
    static const wchar_t kClassName[] = L"Windows.Media.ClosedCaptioning.ClosedCaptionProperties";
    HSTRING_HEADER header{};
    HSTRING name = nullptr;
    cc::IClosedCaptionPropertiesStatics* statics = nullptr;
    if (SUCCEEDED(winrt.createStringReference(kClassName, ARRAYSIZE(kClassName) - 1, &header, &name)) &&
        SUCCEEDED(winrt.getActivationFactory(name, __uuidof(cc::IClosedCaptionPropertiesStatics), reinterpret_cast<void**>(&statics)))) {
      read = read_statics(statics, out);
      statics->Release();
    }
    winrt.uninitialize();
  });
  worker.join();
  FreeLibrary(winrt.module);
  return read;
}
#else
bool query_windows_caption_settings(ClosedCaptionProperties*) { return false; }
#endif

HRESULT XClosedCaptionGetProperties_impl(void*, ClosedCaptionProperties* properties) {
  if (!properties) return E_INVALIDARG;
  ClosedCaptionProperties result{};
  result.fontScale = kDefaultFontScale;
  if (!query_windows_caption_settings(&result)) {
    result = {};
    result.fontScale = kDefaultFontScale;
  }
  result.enabled = g_captions_enabled.load() ? 1 : 0;
  *properties = result;
  return S_OK;
}

HRESULT XClosedCaptionSetEnabled_impl(void*, uint8_t enabled) {
  g_captions_enabled.store(enabled != 0);
  return S_OK;
}

constexpr uint32_t kSpeechToTextPositionHintCount = 8;
constexpr uint32_t kSpeechToTextTypeCount = 2;

std::atomic<uint32_t> g_next_hypothesis_id{1};

bool valid_speech_to_text_text(const char* speakerName, const char* content, uint32_t type) {
  return speakerName && content && type < kSpeechToTextTypeCount;
}

HRESULT XSpeechToTextSetPositionHint_impl(void*, uint32_t position) {
  return position < kSpeechToTextPositionHintCount ? S_OK : E_INVALIDARG;
}

HRESULT XSpeechToTextSendString_impl(void*, const char* speakerName, const char* content, uint32_t type) {
  return valid_speech_to_text_text(speakerName, content, type) ? S_OK : E_INVALIDARG;
}

HRESULT XSpeechToTextBeginHypothesisString_impl(void*, const char* speakerName, const char* content, uint32_t type, uint32_t* hypothesisId) {
  if (!hypothesisId || !valid_speech_to_text_text(speakerName, content, type)) return E_INVALIDARG;
  uint32_t id = g_next_hypothesis_id++;
  if (!id) id = g_next_hypothesis_id++;
  *hypothesisId = id;
  return S_OK;
}

HRESULT XSpeechToTextUpdateHypothesisString_impl(void*, uint32_t, const char* content) { return content ? S_OK : E_INVALIDARG; }
HRESULT XSpeechToTextFinalizeHypothesisString_impl(void*, uint32_t, const char* content) { return content ? S_OK : E_INVALIDARG; }
HRESULT XSpeechToTextCancelHypothesisString_impl(void*, uint32_t) { return S_OK; }

struct SpeechSynthesizer;
struct SpeechSynthesizerStream;
struct VoiceInformation;
using InstalledVoicesCallback = uint8_t(__stdcall*)(const VoiceInformation* information, void* context);

const HRESULT kNoSuchHandle = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kNoSpeechSynthesis = E_NOTIMPL;

HRESULT XSpeechSynthesizerEnumerateInstalledVoices_impl(void*, void*, InstalledVoicesCallback callback) {
  return callback ? S_OK : E_INVALIDARG;
}

HRESULT XSpeechSynthesizerCreate_impl(void*, SpeechSynthesizer** speechSynthesizer) {
  if (!speechSynthesizer) return E_INVALIDARG;
  *speechSynthesizer = nullptr;
  return kNoSpeechSynthesis;
}

HRESULT XSpeechSynthesizerCloseHandle_impl(void*, SpeechSynthesizer* speechSynthesizer) {
  return speechSynthesizer ? kNoSuchHandle : S_OK;
}

HRESULT XSpeechSynthesizerSetDefaultVoice_impl(void*, SpeechSynthesizer* speechSynthesizer) {
  return speechSynthesizer ? kNoSuchHandle : E_INVALIDARG;
}

HRESULT XSpeechSynthesizerSetCustomVoice_impl(void*, SpeechSynthesizer* speechSynthesizer, const char* voiceId) {
  if (!speechSynthesizer || !voiceId) return E_INVALIDARG;
  return kNoSuchHandle;
}

HRESULT create_stream(SpeechSynthesizer* speechSynthesizer, const char* input, SpeechSynthesizerStream** stream) {
  if (!stream) return E_INVALIDARG;
  *stream = nullptr;
  if (!speechSynthesizer || !input) return E_INVALIDARG;
  return kNoSuchHandle;
}

HRESULT XSpeechSynthesizerCreateStreamFromText_impl(void*, SpeechSynthesizer* speechSynthesizer, const char* text, SpeechSynthesizerStream** stream) {
  return create_stream(speechSynthesizer, text, stream);
}

HRESULT XSpeechSynthesizerCreateStreamFromSsml_impl(void*, SpeechSynthesizer* speechSynthesizer, const char* ssml, SpeechSynthesizerStream** stream) {
  return create_stream(speechSynthesizer, ssml, stream);
}

HRESULT XSpeechSynthesizerCloseStreamHandle_impl(void*, SpeechSynthesizerStream*) { return kNoSuchHandle; }

HRESULT XSpeechSynthesizerGetStreamDataSize_impl(void*, SpeechSynthesizerStream*, size_t* bufferSize) {
  if (!bufferSize) return E_INVALIDARG;
  *bufferSize = 0;
  return kNoSuchHandle;
}

HRESULT XSpeechSynthesizerGetStreamData_impl(void*, SpeechSynthesizerStream*, size_t bufferSize, void* buffer, size_t* bufferUsed) {
  if (bufferUsed) *bufferUsed = 0;
  if (bufferSize && !buffer) return E_INVALIDARG;
  return kNoSuchHandle;
}

const Override kOverrides[] = {
    {"XClosedCaptionGetProperties", reinterpret_cast<void*>(&XClosedCaptionGetProperties_impl)},
    {"XClosedCaptionSetEnabled", reinterpret_cast<void*>(&XClosedCaptionSetEnabled_impl)},
    {"XSpeechToTextSetPositionHint", reinterpret_cast<void*>(&XSpeechToTextSetPositionHint_impl)},
    {"XSpeechToTextSendString", reinterpret_cast<void*>(&XSpeechToTextSendString_impl)},
    {"XSpeechToTextBeginHypothesisString", reinterpret_cast<void*>(&XSpeechToTextBeginHypothesisString_impl)},
    {"XSpeechToTextUpdateHypothesisString", reinterpret_cast<void*>(&XSpeechToTextUpdateHypothesisString_impl)},
    {"XSpeechToTextFinalizeHypothesisString", reinterpret_cast<void*>(&XSpeechToTextFinalizeHypothesisString_impl)},
    {"XSpeechToTextCancelHypothesisString", reinterpret_cast<void*>(&XSpeechToTextCancelHypothesisString_impl)},
    {"XSpeechSynthesizerEnumerateInstalledVoices", reinterpret_cast<void*>(&XSpeechSynthesizerEnumerateInstalledVoices_impl)},
    {"XSpeechSynthesizerCreate", reinterpret_cast<void*>(&XSpeechSynthesizerCreate_impl)},
    {"XSpeechSynthesizerCloseHandle", reinterpret_cast<void*>(&XSpeechSynthesizerCloseHandle_impl)},
    {"XSpeechSynthesizerSetDefaultVoice", reinterpret_cast<void*>(&XSpeechSynthesizerSetDefaultVoice_impl)},
    {"XSpeechSynthesizerSetCustomVoice", reinterpret_cast<void*>(&XSpeechSynthesizerSetCustomVoice_impl)},
    {"XSpeechSynthesizerCreateStreamFromText", reinterpret_cast<void*>(&XSpeechSynthesizerCreateStreamFromText_impl)},
    {"XSpeechSynthesizerCreateStreamFromSsml", reinterpret_cast<void*>(&XSpeechSynthesizerCreateStreamFromSsml_impl)},
    {"XSpeechSynthesizerCloseStreamHandle", reinterpret_cast<void*>(&XSpeechSynthesizerCloseStreamHandle_impl)},
    {"XSpeechSynthesizerGetStreamDataSize", reinterpret_cast<void*>(&XSpeechSynthesizerGetStreamDataSize_impl)},
    {"XSpeechSynthesizerGetStreamData", reinterpret_cast<void*>(&XSpeechSynthesizerGetStreamData_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::accessibility
