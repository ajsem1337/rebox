#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.media.closedcaptioning.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);

struct Color { uint8_t a, r, g, b; };
struct CaptionProperties {
  Color background, font, window;
  uint32_t edge, style;
  float scale;
  uint8_t enabled;
};
static_assert(sizeof(CaptionProperties) == 28, "XClosedCaptionProperties layout");

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

static const GUID kAccessibility = {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};
constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);

namespace cc = ABI::Windows::Media::ClosedCaptioning;

struct WindowsCaptions {
  bool read = false;
  ABI::Windows::UI::Color font{}, background{}, region{};
  cc::ClosedCaptionEdgeEffect effect = cc::ClosedCaptionEdgeEffect_Default;
  cc::ClosedCaptionStyle style = cc::ClosedCaptionStyle_Default;
  cc::ClosedCaptionSize size = cc::ClosedCaptionSize_Default;
};

template <typename Fn> static Fn Proc(HMODULE module, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

static WindowsCaptions ReadWindowsCaptions() {
  WindowsCaptions out;
  HMODULE combase = LoadLibraryA("combase.dll");
  if (!combase) return out;
  auto init = Proc<decltype(&RoInitialize)>(combase, "RoInitialize");
  auto uninit = Proc<decltype(&RoUninitialize)>(combase, "RoUninitialize");
  auto factory = Proc<decltype(&RoGetActivationFactory)>(combase, "RoGetActivationFactory");
  auto reference = Proc<decltype(&WindowsCreateStringReference)>(combase, "WindowsCreateStringReference");
  if (init && uninit && factory && reference) {
    std::thread worker([&] {
      if (FAILED(init(RO_INIT_MULTITHREADED))) return;
      static const wchar_t kName[] = L"Windows.Media.ClosedCaptioning.ClosedCaptionProperties";
      HSTRING_HEADER header{};
      HSTRING name = nullptr;
      cc::IClosedCaptionPropertiesStatics* statics = nullptr;
      if (SUCCEEDED(reference(kName, ARRAYSIZE(kName) - 1, &header, &name)) &&
          SUCCEEDED(factory(name, __uuidof(cc::IClosedCaptionPropertiesStatics), reinterpret_cast<void**>(&statics)))) {
        out.read = SUCCEEDED(statics->get_ComputedFontColor(&out.font)) && SUCCEEDED(statics->get_ComputedBackgroundColor(&out.background)) &&
                   SUCCEEDED(statics->get_ComputedRegionColor(&out.region)) && SUCCEEDED(statics->get_FontEffect(&out.effect)) &&
                   SUCCEEDED(statics->get_FontStyle(&out.style)) && SUCCEEDED(statics->get_FontSize(&out.size));
        statics->Release();
      }
      uninit();
    });
    worker.join();
  }
  FreeLibrary(combase);
  return out;
}

static bool SameColor(const Color& mine, const ABI::Windows::UI::Color& windows) {
  return mine.a == windows.A && mine.r == windows.R && mine.g == windows.G && mine.b == windows.B;
}

static float ExpectedScale(cc::ClosedCaptionSize size) {
  switch (size) {
    case cc::ClosedCaptionSize_FiftyPercent: return 0.5f;
    case cc::ClosedCaptionSize_OneHundredFiftyPercent: return 1.5f;
    case cc::ClosedCaptionSize_TwoHundredPercent: return 2.0f;
    default: return 1.0f;
  }
}

static void CheckClosedCaptions(const Api& access) {
  auto getProperties = access.At<HRESULT (*)(void*, CaptionProperties*)>(3, "XClosedCaptionGetProperties");
  auto setEnabled = access.At<HRESULT (*)(void*, BOOLEAN)>(4, "XClosedCaptionSetEnabled");

  CaptionProperties props;
  std::memset(&props, 0xcc, sizeof(props));
  HRESULT hr = getProperties(access.self, &props);
  Check(hr == S_OK && props.enabled == 0, "XClosedCaptionGetProperties reports captions disabled until the title enables them");
  Check(props.edge < 6 && props.style < 8, "XClosedCaptionGetProperties returns documented font edge and style values");
  std::printf("INFO captions: font %02x%02x%02x%02x background %02x%02x%02x%02x window %02x%02x%02x%02x edge %u style %u scale %.2f\n",
              props.font.a, props.font.r, props.font.g, props.font.b, props.background.a, props.background.r, props.background.g,
              props.background.b, props.window.a, props.window.r, props.window.g, props.window.b, props.edge, props.style, props.scale);

  WindowsCaptions windows = ReadWindowsCaptions();
  if (windows.read) {
    bool same = SameColor(props.font, windows.font) && SameColor(props.background, windows.background) && SameColor(props.window, windows.region) &&
                props.edge == static_cast<uint32_t>(windows.effect) && props.style == static_cast<uint32_t>(windows.style) &&
                props.scale == ExpectedScale(windows.size);
    Check(same, "XClosedCaptionGetProperties matches the Windows closed caption settings");
  } else {
    bool defaults = props.font.a == 0 && props.background.a == 0 && props.window.a == 0 && props.edge == 0 && props.style == 0 && props.scale == 1.0f;
    std::printf("INFO the Windows closed caption settings could not be read here\n");
    Check(defaults, "XClosedCaptionGetProperties falls back to the Default styles when Windows settings are unreadable");
  }

  Check(setEnabled(access.self, TRUE) == S_OK, "XClosedCaptionSetEnabled accepts true");
  hr = getProperties(access.self, &props);
  Check(hr == S_OK && props.enabled == 1, "XClosedCaptionGetProperties reflects XClosedCaptionSetEnabled(true)");
  Check(setEnabled(access.self, FALSE) == S_OK, "XClosedCaptionSetEnabled accepts false");
  hr = getProperties(access.self, &props);
  Check(hr == S_OK && props.enabled == 0, "XClosedCaptionGetProperties reflects XClosedCaptionSetEnabled(false)");
  Check(getProperties(access.self, nullptr) == E_INVALIDARG, "XClosedCaptionGetProperties rejects null");
}

static void CheckSpeechToText(const Api& access) {
  auto position = access.At<HRESULT (*)(void*, uint32_t)>(6, "XSpeechToTextSetPositionHint");
  auto send = access.At<HRESULT (*)(void*, const char*, const char*, uint32_t)>(7, "XSpeechToTextSendString");
  auto begin = access.At<HRESULT (*)(void*, const char*, const char*, uint32_t, uint32_t*)>(17, "XSpeechToTextBeginHypothesisString");
  auto update = access.At<HRESULT (*)(void*, uint32_t, const char*)>(18, "XSpeechToTextUpdateHypothesisString");
  auto finalize = access.At<HRESULT (*)(void*, uint32_t, const char*)>(19, "XSpeechToTextFinalizeHypothesisString");
  auto cancel = access.At<HRESULT (*)(void*, uint32_t)>(20, "XSpeechToTextCancelHypothesisString");

  bool all = true;
  for (uint32_t p = 0; p < 8; p++) all = all && position(access.self, p) == S_OK;
  Check(all, "XSpeechToTextSetPositionHint accepts every documented position");
  Check(position(access.self, 8) == E_INVALIDARG, "XSpeechToTextSetPositionHint rejects an unknown position");

  Check(send(access.self, "Speaker", "Text", 0) == S_OK && send(access.self, "Speaker", "Text", 1) == S_OK, "XSpeechToTextSendString accepts voice and text");
  Check(send(access.self, nullptr, "Text", 0) == E_INVALIDARG, "XSpeechToTextSendString rejects a null speaker name");
  Check(send(access.self, "Speaker", nullptr, 0) == E_INVALIDARG, "XSpeechToTextSendString rejects null content");
  Check(send(access.self, "Speaker", "Text", 2) == E_INVALIDARG, "XSpeechToTextSendString rejects an unknown type");

  uint32_t first = 0, second = 0;
  HRESULT hr1 = begin(access.self, "Speaker", "Partial", 0, &first);
  HRESULT hr2 = begin(access.self, "Speaker", "Typed", 1, &second);
  Check(hr1 == S_OK && hr2 == S_OK && first && second && first != second, "XSpeechToTextBeginHypothesisString hands out distinct ids");
  Check(update(access.self, first, "Partial more") == S_OK, "XSpeechToTextUpdateHypothesisString updates a live hypothesis");
  Check(finalize(access.self, first, "Final") == S_OK, "XSpeechToTextFinalizeHypothesisString finalizes a live hypothesis");
  Check(cancel(access.self, second) == S_OK, "XSpeechToTextCancelHypothesisString cancels a live hypothesis");
  Check(update(access.self, first, "Late") == S_OK && finalize(access.self, second, "Late") == S_OK && cancel(access.self, 0xfffffff0u) == S_OK,
        "finished or unknown hypothesis ids return no error, as documented");
  uint32_t untouched = 77;
  Check(begin(access.self, "Speaker", "Text", 0, nullptr) == E_INVALIDARG, "XSpeechToTextBeginHypothesisString rejects a null id pointer");
  Check(begin(access.self, nullptr, "Text", 0, &untouched) == E_INVALIDARG && untouched == 77, "XSpeechToTextBeginHypothesisString rejects a null speaker name");
  Check(begin(access.self, "Speaker", nullptr, 0, &untouched) == E_INVALIDARG, "XSpeechToTextBeginHypothesisString rejects null content");
  Check(begin(access.self, "Speaker", "Text", 5, &untouched) == E_INVALIDARG, "XSpeechToTextBeginHypothesisString rejects an unknown type");
  Check(update(access.self, first, nullptr) == E_INVALIDARG, "XSpeechToTextUpdateHypothesisString rejects null content");
  Check(finalize(access.self, first, nullptr) == E_INVALIDARG, "XSpeechToTextFinalizeHypothesisString rejects null content");
}

static int g_voice_calls = 0;
static BOOLEAN __stdcall CountVoice(const void*, void* context) {
  g_voice_calls++;
  if (context) (*static_cast<int*>(context))++;
  return TRUE;
}

static void CheckSpeechSynthesizer(const Api& access) {
  auto enumerate = access.At<HRESULT (*)(void*, void*, BOOLEAN(__stdcall*)(const void*, void*))>(8, "XSpeechSynthesizerEnumerateInstalledVoices");
  auto create = access.At<HRESULT (*)(void*, void**)>(9, "XSpeechSynthesizerCreate");
  auto close = access.At<HRESULT (*)(void*, void*)>(10, "XSpeechSynthesizerCloseHandle");
  auto setDefault = access.At<HRESULT (*)(void*, void*)>(11, "XSpeechSynthesizerSetDefaultVoice");
  auto setCustom = access.At<HRESULT (*)(void*, void*, const char*)>(12, "XSpeechSynthesizerSetCustomVoice");
  auto fromText = access.At<HRESULT (*)(void*, void*, const char*, void**)>(13, "XSpeechSynthesizerCreateStreamFromText");
  auto closeStream = access.At<HRESULT (*)(void*, void*)>(14, "XSpeechSynthesizerCloseStreamHandle");
  auto dataSize = access.At<HRESULT (*)(void*, void*, size_t*)>(15, "XSpeechSynthesizerGetStreamDataSize");
  auto data = access.At<HRESULT (*)(void*, void*, size_t, void*, size_t*)>(16, "XSpeechSynthesizerGetStreamData");
  auto fromSsml = access.At<HRESULT (*)(void*, void*, const char*, void**)>(21, "XSpeechSynthesizerCreateStreamFromSsml");

  int counted = 0;
  Check(enumerate(access.self, &counted, &CountVoice) == S_OK && counted == 0 && g_voice_calls == 0,
        "XSpeechSynthesizerEnumerateInstalledVoices succeeds and reports no installed voices");
  Check(enumerate(access.self, nullptr, nullptr) == E_INVALIDARG, "XSpeechSynthesizerEnumerateInstalledVoices rejects a null callback");

  void* synthesizer = reinterpret_cast<void*>(1);
  HRESULT hr = create(access.self, &synthesizer);
  Check(FAILED(hr) && !synthesizer, "XSpeechSynthesizerCreate fails without a speech engine and clears the handle");
  Check(create(access.self, nullptr) == E_INVALIDARG, "XSpeechSynthesizerCreate rejects null");

  int fake = 0;
  void* foreign = &fake;
  Check(close(access.self, nullptr) == S_OK, "XSpeechSynthesizerCloseHandle accepts null");
  Check(close(access.self, foreign) == kNotFound, "XSpeechSynthesizerCloseHandle rejects a handle it never issued");
  Check(setDefault(access.self, nullptr) == E_INVALIDARG && setDefault(access.self, foreign) == kNotFound,
        "XSpeechSynthesizerSetDefaultVoice rejects null and unknown handles");
  Check(setCustom(access.self, foreign, nullptr) == E_INVALIDARG && setCustom(access.self, nullptr, "voice") == E_INVALIDARG &&
            setCustom(access.self, foreign, "voice") == kNotFound,
        "XSpeechSynthesizerSetCustomVoice rejects null arguments and unknown handles");

  void* stream = reinterpret_cast<void*>(1);
  Check(fromText(access.self, foreign, "Hello", &stream) == kNotFound && !stream, "XSpeechSynthesizerCreateStreamFromText rejects an unknown synthesizer and clears the stream");
  Check(fromText(access.self, foreign, nullptr, &stream) == E_INVALIDARG && fromText(access.self, nullptr, "Hello", &stream) == E_INVALIDARG &&
            fromText(access.self, foreign, "Hello", nullptr) == E_INVALIDARG,
        "XSpeechSynthesizerCreateStreamFromText rejects null arguments");
  stream = reinterpret_cast<void*>(1);
  Check(fromSsml(access.self, foreign, "<speak version=\"1.0\"/>", &stream) == kNotFound && !stream,
        "XSpeechSynthesizerCreateStreamFromSsml rejects an unknown synthesizer and clears the stream");
  Check(fromSsml(access.self, foreign, nullptr, &stream) == E_INVALIDARG && fromSsml(access.self, nullptr, "<speak/>", &stream) == E_INVALIDARG &&
            fromSsml(access.self, foreign, "<speak/>", nullptr) == E_INVALIDARG,
        "XSpeechSynthesizerCreateStreamFromSsml rejects null arguments");

  Check(closeStream(access.self, foreign) == kNotFound && closeStream(access.self, nullptr) == kNotFound,
        "XSpeechSynthesizerCloseStreamHandle rejects streams it never issued");
  size_t size = 123;
  Check(dataSize(access.self, foreign, &size) == kNotFound && size == 0, "XSpeechSynthesizerGetStreamDataSize returns ERROR_NOT_FOUND for an unknown stream");
  Check(dataSize(access.self, foreign, nullptr) == E_INVALIDARG, "XSpeechSynthesizerGetStreamDataSize rejects null");
  char buffer[16];
  size_t used = 9;
  Check(data(access.self, foreign, sizeof(buffer), buffer, &used) == kNotFound && used == 0, "XSpeechSynthesizerGetStreamData returns the documented ERROR_NOT_FOUND for an invalid stream");
  Check(data(access.self, foreign, sizeof(buffer), nullptr, nullptr) == E_INVALIDARG, "XSpeechSynthesizerGetStreamData rejects a null buffer");
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
  Api access = Get(kAccessibility);
  CheckClosedCaptions(access);
  CheckSpeechToText(access);
  CheckSpeechSynthesizer(access);
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
