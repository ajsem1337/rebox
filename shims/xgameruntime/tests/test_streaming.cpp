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
struct Token { uint64_t value; };
struct Version { uint16_t major, minor, build, revision; };
struct DisplayDetails {
  uint32_t preferredWidth, preferredHeight;
  RECT safeArea;
  uint32_t maxPixels, maxWidth, maxHeight, flags;
};
struct StateValue {
  uint32_t valueKind;
  union {
    uint8_t booleanValue;
    int64_t integerValue;
    double doubleValue;
    const char* stringValue;
  };
};
struct StateOperation {
  uint32_t operationKind;
  const char* path;
  StateValue value;
};
static_assert(sizeof(StateOperation) == 32, "XGameStreamingTouchControlsStateOperation layout");
static_assert(sizeof(DisplayDetails) == 40, "XGameStreamingDisplayDetails layout");

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

constexpr HRESULT kNotInitialized = static_cast<HRESULT>(0x89245400);
constexpr HRESULT kClientNotConnected = static_cast<HRESULT>(0x89245401);
constexpr HRESULT kNoData = static_cast<HRESULT>(0x89245402);
constexpr HRESULT kNoDatacenter = static_cast<HRESULT>(0x89245403);
constexpr HRESULT kNotStreamingController = static_cast<HRESULT>(0x89245404);
constexpr uint64_t kSomeClient = 0x1234;

static const GUID kStreaming = {0x8aff07f5, 0xa1bf, 0x4db8, {0x80, 0xa5, 0x31, 0xcc, 0xa0, 0xde, 0x51, 0xb7}};
static const GUID kStreaming2 = {0x5f5e5169, 0x746c, 0x4001, {0xad, 0x1c, 0xda, 0x72, 0x8d, 0x01, 0xc9, 0xeb}};
static const GUID kStreaming3 = {0x57786622, 0x6605, 0x46d0, {0xb9, 0x17, 0x0f, 0x22, 0xbb, 0xcd, 0x9c, 0x52}};

typedef HRESULT (*InitializeFn)(void*);
typedef void (*UninitializeFn)(void*);
typedef BOOLEAN (*IsStreamingFn)(void*);
typedef HRESULT (*RegisterPropertiesFn)(void*, uint64_t, void*, void*, void*, Token*);
typedef BOOLEAN (*UnregisterPropertiesFn)(void*, uint64_t, Token, BOOLEAN);
typedef HRESULT (*PhysicalDimensionsFn)(void*, uint64_t, uint32_t*, uint32_t*);
typedef uint32_t (*ClientCountFn)(void*);
typedef HRESULT (*GetClientsFn)(void*, uint32_t, uint64_t*, uint32_t*);
typedef uint32_t (*ConnectionStateFn)(void*, uint64_t);
typedef HRESULT (*RegisterConnectionFn)(void*, void*, void*, void*, Token*);
typedef BOOLEAN (*UnregisterConnectionFn)(void*, Token, BOOLEAN);
typedef HRESULT (*AddedLatencyFn)(void*, uint64_t, uint32_t*, uint32_t*, uint32_t*);
typedef size_t (*LocationNameSizeFn)(void*);
typedef HRESULT (*LocationNameFn)(void*, size_t, char*);
typedef void (*HideFn)(void*);
typedef void (*ShowLayoutFn)(void*, const char*);
typedef void (*HideOnClientFn)(void*, uint64_t);
typedef void (*ShowLayoutOnClientFn)(void*, uint64_t, const char*);
typedef HRESULT (*TouchEnabledFn)(void*, uint64_t, BOOLEAN*);
typedef HRESULT (*LastFrameFn)(void*, uint64_t, uint64_t*);
typedef HRESULT (*AssociatedFrameFn)(void*, void*, uint64_t*);
typedef HRESULT (*PhysicalityFn)(void*, void*, uint64_t*);
typedef HRESULT (*UpdateStateFn)(void*, size_t, const StateOperation*);
typedef HRESULT (*UpdateStateOnClientFn)(void*, uint64_t, size_t, const StateOperation*);
typedef HRESULT (*ShowWithStateFn)(void*, const char*, size_t, const StateOperation*);
typedef HRESULT (*ShowWithStateOnClientFn)(void*, uint64_t, const char*, size_t, const StateOperation*);
typedef size_t (*BundleNameSizeFn)(void*, uint64_t);
typedef HRESULT (*BundleVersionFn)(void*, uint64_t, Version*, size_t, char*);
typedef HRESULT (*IpAddressFn)(void*, uint64_t, size_t, char*);
typedef HRESULT (*SessionIdFn)(void*, uint64_t, size_t, char*, size_t*);
typedef HRESULT (*DisplayDetailsFn)(void*, uint64_t, uint32_t, float, float, DisplayDetails*);
typedef HRESULT (*SetResolutionFn)(void*, uint32_t, uint32_t);

static int g_callback_calls = 0;
static void CALLBACK ConnectionCallback(void*, uint64_t, uint32_t) { g_callback_calls++; }
static void CALLBACK PropertiesCallback(void*, uint64_t, uint32_t, const uint32_t*) { g_callback_calls++; }

static Api g_s1, g_s2, g_s3;
static int g_reading_stand_in;
static void* Reading() { return &g_reading_stand_in; }

static HRESULT Initialize() { return g_s1.At<InitializeFn>(3, "XGameStreamingInitialize")(g_s1.self); }
static void Uninitialize() { g_s1.At<UninitializeFn>(4, "XGameStreamingUninitialize")(g_s1.self); }

static StateOperation BooleanOperation(const char* path, bool value) {
  StateOperation operation{};
  operation.operationKind = 0;
  operation.path = path;
  operation.value.valueKind = 0;
  operation.value.booleanValue = value ? 1 : 0;
  return operation;
}
static StateOperation StringOperation(const char* path, const char* value) {
  StateOperation operation{};
  operation.path = path;
  operation.value.valueKind = 3;
  operation.value.stringValue = value;
  return operation;
}

static bool EveryHresultCallIs(HRESULT expected) {
  uint32_t a = 0, b = 0, c = 0;
  uint64_t frame = 0, physicality = 0, clients[2] = {};
  BOOLEAN touch = 0;
  Token token{};
  char text[64];
  size_t used = 0;
  Version version{};
  DisplayDetails details{};
  StateOperation operation = BooleanOperation("/Flag", true);
  HRESULT results[] = {
      g_s1.At<RegisterPropertiesFn>(6, "XGameStreamingRegisterClientPropertiesChanged")(g_s1.self, kSomeClient, nullptr, nullptr, reinterpret_cast<void*>(&PropertiesCallback), &token),
      g_s1.At<PhysicalDimensionsFn>(8, "XGameStreamingGetStreamPhysicalDimensions")(g_s1.self, kSomeClient, &a, &b),
      g_s1.At<GetClientsFn>(10, "XGameStreamingGetClients")(g_s1.self, 2, clients, &a),
      g_s1.At<RegisterConnectionFn>(12, "XGameStreamingRegisterConnectionStateChanged")(g_s1.self, nullptr, nullptr, reinterpret_cast<void*>(&ConnectionCallback), &token),
      g_s1.At<AddedLatencyFn>(14, "XGameStreamingGetStreamAddedLatency")(g_s1.self, kSomeClient, &a, &b, &c),
      g_s1.At<LocationNameFn>(16, "XGameStreamingGetServerLocationName")(g_s1.self, sizeof(text), text),
      g_s1.At<TouchEnabledFn>(21, "XGameStreamingIsTouchInputEnabled")(g_s1.self, kSomeClient, &touch),
      g_s1.At<LastFrameFn>(22, "XGameStreamingGetLastFrameDisplayed")(g_s1.self, kSomeClient, &frame),
      g_s1.At<AssociatedFrameFn>(23, "XGameStreamingGetAssociatedFrame")(g_s1.self, Reading(), &frame),
      g_s1.At<PhysicalityFn>(24, "XGameStreamingGetGamepadPhysicality")(g_s1.self, Reading(), &physicality),
      g_s1.At<UpdateStateFn>(25, "XGameStreamingUpdateTouchControlsState")(g_s1.self, 1, &operation),
      g_s1.At<UpdateStateOnClientFn>(26, "XGameStreamingUpdateTouchControlsStateOnClient")(g_s1.self, kSomeClient, 1, &operation),
      g_s1.At<ShowWithStateFn>(27, "XGameStreamingShowTouchControlsWithStateUpdate")(g_s1.self, "layout", 1, &operation),
      g_s1.At<ShowWithStateOnClientFn>(28, "XGameStreamingShowTouchControlsWithStateUpdateOnClient")(g_s1.self, kSomeClient, "layout", 1, &operation),
      g_s1.At<BundleVersionFn>(30, "XGameStreamingGetTouchBundleVersion")(g_s1.self, kSomeClient, &version, sizeof(text), text),
      g_s1.At<IpAddressFn>(31, "XGameStreamingGetClientIPAddress")(g_s1.self, kSomeClient, sizeof(text), text),
      g_s2.At<SessionIdFn>(32, "XGameStreamingGetSessionId")(g_s2.self, kSomeClient, sizeof(text), text, &used),
      g_s3.At<DisplayDetailsFn>(33, "XGameStreamingGetDisplayDetails")(g_s3.self, kSomeClient, 1920 * 1080, 21.0f / 9.0f, 16.0f / 10.0f, &details),
      g_s3.At<SetResolutionFn>(34, "XGameStreamingSetResolution")(g_s3.self, 1920, 1080),
  };
  bool all = true;
  for (HRESULT hr : results) all = all && hr == expected;
  return all;
}

static void CheckBeforeInitialize() {
  Check(EveryHresultCallIs(kNotInitialized), "every HRESULT method returns E_GAMESTREAMING_NOT_INITIALIZED before XGameStreamingInitialize");
  Check(!g_s1.At<IsStreamingFn>(5, "XGameStreamingIsStreaming")(g_s1.self), "XGameStreamingIsStreaming is false before initialization");
  Check(g_s1.At<ClientCountFn>(9, "XGameStreamingGetClientCount")(g_s1.self) == 0, "XGameStreamingGetClientCount is 0 before initialization");
}

static void CheckInitialization() {
  Check(Initialize() == S_OK, "XGameStreamingInitialize succeeds");
  Check(Initialize() == S_OK, "XGameStreamingInitialize succeeds again while initialized");
  Check(g_s3.At<InitializeFn>(3, "XGameStreamingInitialize")(g_s3.self) == S_OK, "IXGameStreamingImpl3 reaches the inherited XGameStreamingInitialize slot");
}

static void CheckConnectionState() {
  Check(!g_s1.At<IsStreamingFn>(5, "XGameStreamingIsStreaming")(g_s1.self), "XGameStreamingIsStreaming reports a local, unstreamed session");
  Check(g_s1.At<ClientCountFn>(9, "XGameStreamingGetClientCount")(g_s1.self) == 0, "XGameStreamingGetClientCount reports no connected clients");

  auto getClients = g_s1.At<GetClientsFn>(10, "XGameStreamingGetClients");
  uint64_t clients[4] = {7, 7, 7, 7};
  uint32_t used = 99;
  HRESULT hr = getClients(g_s1.self, 4, clients, &used);
  Check(hr == S_OK && used == 0 && clients[0] == 7, "XGameStreamingGetClients returns zero clients and writes no ids");
  used = 99;
  Check(getClients(g_s1.self, 0, nullptr, &used) == S_OK && used == 0, "XGameStreamingGetClients accepts an empty buffer");
  Check(getClients(g_s1.self, 4, clients, nullptr) == S_OK, "XGameStreamingGetClients accepts a null used count");
  Check(getClients(g_s1.self, 4, nullptr, &used) == E_INVALIDARG, "XGameStreamingGetClients rejects a null buffer with a non-zero count");

  auto state = g_s1.At<ConnectionStateFn>(11, "XGameStreamingGetConnectionState");
  Check(state(g_s1.self, kSomeClient) == 0 && state(g_s1.self, 0) == 0, "XGameStreamingGetConnectionState reports Disconnected for any client");

  auto reg = g_s1.At<RegisterConnectionFn>(12, "XGameStreamingRegisterConnectionStateChanged");
  auto unreg = g_s1.At<UnregisterConnectionFn>(13, "XGameStreamingUnregisterConnectionStateChanged");
  Token first{}, second{};
  HRESULT hr1 = reg(g_s1.self, nullptr, nullptr, reinterpret_cast<void*>(&ConnectionCallback), &first);
  HRESULT hr2 = reg(g_s1.self, nullptr, &g_failures, reinterpret_cast<void*>(&ConnectionCallback), &second);
  Check(hr1 == S_OK && hr2 == S_OK && first.value && second.value && first.value != second.value,
        "XGameStreamingRegisterConnectionStateChanged registers with distinct tokens");
  Token unused{};
  Check(reg(g_s1.self, nullptr, nullptr, nullptr, &unused) == E_INVALIDARG, "XGameStreamingRegisterConnectionStateChanged rejects a null callback");
  Check(reg(g_s1.self, nullptr, nullptr, reinterpret_cast<void*>(&ConnectionCallback), nullptr) == E_INVALIDARG,
        "XGameStreamingRegisterConnectionStateChanged rejects a null token");
  Sleep(50);
  Check(g_callback_calls == 0, "XGameStreamingRegisterConnectionStateChanged sends no initial callback (no client is connected)");
  Check(unreg(g_s1.self, first, FALSE) && unreg(g_s1.self, second, TRUE), "XGameStreamingUnregisterConnectionStateChanged unregisters");
  Token unknown{};
  unknown.value = 0xdeadbeef;
  Check(!unreg(g_s1.self, unknown, FALSE) && !unreg(g_s1.self, first, TRUE) && !unreg(g_s1.self, second, FALSE),
        "XGameStreamingUnregisterConnectionStateChanged answers false for a token it never issued or already revoked");
}

static void CheckClientProperties() {
  auto reg = g_s1.At<RegisterPropertiesFn>(6, "XGameStreamingRegisterClientPropertiesChanged");
  auto unreg = g_s1.At<UnregisterPropertiesFn>(7, "XGameStreamingUnregisterClientPropertiesChanged");
  Token token{};
  Check(reg(g_s1.self, kSomeClient, nullptr, nullptr, reinterpret_cast<void*>(&PropertiesCallback), &token) == kClientNotConnected && token.value == 0,
        "XGameStreamingRegisterClientPropertiesChanged returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED for an unknown client");
  Check(reg(g_s1.self, kSomeClient, nullptr, nullptr, nullptr, &token) == E_INVALIDARG, "XGameStreamingRegisterClientPropertiesChanged rejects a null callback");
  Check(reg(g_s1.self, kSomeClient, nullptr, nullptr, reinterpret_cast<void*>(&PropertiesCallback), nullptr) == E_INVALIDARG,
        "XGameStreamingRegisterClientPropertiesChanged rejects a null token");
  Check(!unreg(g_s1.self, kSomeClient, Token{1}, TRUE), "XGameStreamingUnregisterClientPropertiesChanged returns false (no registration exists)");

  uint32_t horizontal = 5, vertical = 5;
  auto dimensions = g_s1.At<PhysicalDimensionsFn>(8, "XGameStreamingGetStreamPhysicalDimensions");
  Check(dimensions(g_s1.self, kSomeClient, &horizontal, &vertical) == kClientNotConnected,
        "XGameStreamingGetStreamPhysicalDimensions returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  Check(dimensions(g_s1.self, kSomeClient, nullptr, &vertical) == E_INVALIDARG, "XGameStreamingGetStreamPhysicalDimensions rejects a null output");

  auto touch = g_s1.At<TouchEnabledFn>(21, "XGameStreamingIsTouchInputEnabled");
  BOOLEAN enabled = 1;
  Check(touch(g_s1.self, kSomeClient, &enabled) == kClientNotConnected && enabled == 0,
        "XGameStreamingIsTouchInputEnabled returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED and false");
  Check(touch(g_s1.self, kSomeClient, nullptr) == E_INVALIDARG, "XGameStreamingIsTouchInputEnabled rejects a null output");

  auto nameSize = g_s1.At<BundleNameSizeFn>(29, "XGameStreamingGetTouchBundleVersionNameSize");
  Check(nameSize(g_s1.self, kSomeClient) == 0, "XGameStreamingGetTouchBundleVersionNameSize is 0 for an unknown client");
  auto bundle = g_s1.At<BundleVersionFn>(30, "XGameStreamingGetTouchBundleVersion");
  Version version{};
  char name[16];
  Check(bundle(g_s1.self, kSomeClient, &version, sizeof(name), name) == kClientNotConnected && bundle(g_s1.self, kSomeClient, nullptr, 0, nullptr) == kClientNotConnected,
        "XGameStreamingGetTouchBundleVersion returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED (optional outputs may be null)");
  Check(bundle(g_s1.self, kSomeClient, &version, sizeof(name), nullptr) == E_INVALIDARG, "XGameStreamingGetTouchBundleVersion rejects a sized null name buffer");

  auto ip = g_s1.At<IpAddressFn>(31, "XGameStreamingGetClientIPAddress");
  char address[46];
  Check(ip(g_s1.self, kSomeClient, sizeof(address), address) == kClientNotConnected, "XGameStreamingGetClientIPAddress returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  Check(ip(g_s1.self, kSomeClient, sizeof(address), nullptr) == E_INVALIDARG, "XGameStreamingGetClientIPAddress rejects a null buffer");

  auto session = g_s2.At<SessionIdFn>(32, "XGameStreamingGetSessionId");
  char id[64];
  size_t used = 77;
  Check(session(g_s2.self, kSomeClient, sizeof(id), id, &used) == kClientNotConnected && used == 0,
        "XGameStreamingGetSessionId returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  Check(session(g_s2.self, kSomeClient, sizeof(id), id, nullptr) == kClientNotConnected, "XGameStreamingGetSessionId accepts a null used size");
  Check(session(g_s2.self, kSomeClient, sizeof(id), nullptr, &used) == E_INVALIDARG, "XGameStreamingGetSessionId rejects a null buffer");

  auto display = g_s3.At<DisplayDetailsFn>(33, "XGameStreamingGetDisplayDetails");
  DisplayDetails details{};
  const float wide = 21.0f / 9.0f, tall = 16.0f / 10.0f, standard = 16.0f / 9.0f;
  Check(display(g_s3.self, kSomeClient, 1920 * 1080, wide, tall, &details) == kClientNotConnected &&
            display(g_s3.self, kSomeClient, 1920 * 1080, standard, standard, &details) == kClientNotConnected,
        "XGameStreamingGetDisplayDetails returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED for valid parameters");
  bool rejects = display(g_s3.self, kSomeClient, 0, wide, tall, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, 1.5f, tall, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, INFINITY, tall, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, wide, 2.0f, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, wide, 0.0f, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, wide, NAN, &details) == E_INVALIDARG &&
                 display(g_s3.self, kSomeClient, 1920 * 1080, wide, tall, nullptr) == E_INVALIDARG;
  Check(rejects, "XGameStreamingGetDisplayDetails rejects the documented invalid parameters");
}

static void CheckLatencyAndInput() {
  auto latency = g_s1.At<AddedLatencyFn>(14, "XGameStreamingGetStreamAddedLatency");
  uint32_t input = 0, output = 0, deviation = 0;
  Check(latency(g_s1.self, kSomeClient, &input, &output, &deviation) == kNoData, "XGameStreamingGetStreamAddedLatency returns the documented E_GAMESTREAMING_NO_DATA (its page lists no CLIENT_NOT_CONNECTED)");
  Check(latency(g_s1.self, kSomeClient, &input, nullptr, &deviation) == E_INVALIDARG, "XGameStreamingGetStreamAddedLatency rejects a null output");

  uint64_t frame = 0;
  auto last = g_s1.At<LastFrameFn>(22, "XGameStreamingGetLastFrameDisplayed");
  Check(last(g_s1.self, kSomeClient, &frame) == kClientNotConnected, "XGameStreamingGetLastFrameDisplayed returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  Check(last(g_s1.self, kSomeClient, nullptr) == E_INVALIDARG, "XGameStreamingGetLastFrameDisplayed rejects a null output");

  auto associated = g_s1.At<AssociatedFrameFn>(23, "XGameStreamingGetAssociatedFrame");
  Check(associated(g_s1.self, Reading(), &frame) == kNotStreamingController, "XGameStreamingGetAssociatedFrame returns E_GAMESTREAMING_NOT_STREAMING_CONTROLLER");
  Check(associated(g_s1.self, nullptr, &frame) == E_INVALIDARG && associated(g_s1.self, Reading(), nullptr) == E_INVALIDARG,
        "XGameStreamingGetAssociatedFrame rejects null arguments");

  auto physicality = g_s1.At<PhysicalityFn>(24, "XGameStreamingGetGamepadPhysicality");
  uint64_t mapping = 0;
  Check(physicality(g_s1.self, Reading(), &mapping) == kNotStreamingController, "XGameStreamingGetGamepadPhysicality returns E_GAMESTREAMING_NOT_STREAMING_CONTROLLER");
  Check(physicality(g_s1.self, nullptr, &mapping) == E_INVALIDARG && physicality(g_s1.self, Reading(), nullptr) == E_INVALIDARG,
        "XGameStreamingGetGamepadPhysicality rejects null arguments");
}

static void CheckServerLocation() {
  Check(g_s1.At<LocationNameSizeFn>(15, "XGameStreamingGetServerLocationNameSize")(g_s1.self) == 0, "XGameStreamingGetServerLocationNameSize is 0 outside a datacenter");
  auto name = g_s1.At<LocationNameFn>(16, "XGameStreamingGetServerLocationName");
  char text[32];
  std::memset(text, 'x', sizeof(text));
  Check(name(g_s1.self, sizeof(text), text) == kNoDatacenter && text[0] == '\0', "XGameStreamingGetServerLocationName returns E_GAMESTREAMING_NO_DATACENTER with an empty string");
  Check(name(g_s1.self, 0, text) == kNoDatacenter, "XGameStreamingGetServerLocationName returns E_GAMESTREAMING_NO_DATACENTER for a zero-size buffer");
  Check(name(g_s1.self, sizeof(text), nullptr) == E_INVALIDARG, "XGameStreamingGetServerLocationName rejects a null buffer");
}

static void CheckTouchControls() {
  g_s1.At<HideFn>(17, "XGameStreamingHideTouchControls")(g_s1.self);
  g_s1.At<ShowLayoutFn>(18, "XGameStreamingShowTouchControlLayout")(g_s1.self, "Driving");
  g_s1.At<ShowLayoutFn>(18, "XGameStreamingShowTouchControlLayout")(g_s1.self, nullptr);
  g_s1.At<HideOnClientFn>(19, "XGameStreamingHideTouchControlsOnClient")(g_s1.self, kSomeClient);
  g_s1.At<ShowLayoutOnClientFn>(20, "XGameStreamingShowTouchControlLayoutOnClient")(g_s1.self, kSomeClient, "Driving");
  g_s1.At<ShowLayoutOnClientFn>(20, "XGameStreamingShowTouchControlLayoutOnClient")(g_s1.self, kSomeClient, nullptr);
  Check(true, "touch layout show/hide requests return without effect when nothing is streamed");

  StateOperation good[3] = {BooleanOperation("/ReloadEnabled", true), StringOperation("/WeaponImage", "rifle"), BooleanOperation("/Slot", false)};
  good[2].value.valueKind = 2;
  good[2].value.doubleValue = 0.5;
  StateOperation badKind = BooleanOperation("/Flag", true);
  badKind.operationKind = 1;
  StateOperation badValueKind = BooleanOperation("/Flag", true);
  badValueKind.value.valueKind = 4;
  StateOperation nullPath = BooleanOperation(nullptr, true);
  StateOperation nullString = StringOperation("/Image", nullptr);
  const StateOperation* bad[] = {&badKind, &badValueKind, &nullPath, &nullString};

  auto update = g_s1.At<UpdateStateFn>(25, "XGameStreamingUpdateTouchControlsState");
  auto updateOnClient = g_s1.At<UpdateStateOnClientFn>(26, "XGameStreamingUpdateTouchControlsStateOnClient");
  auto show = g_s1.At<ShowWithStateFn>(27, "XGameStreamingShowTouchControlsWithStateUpdate");
  auto showOnClient = g_s1.At<ShowWithStateOnClientFn>(28, "XGameStreamingShowTouchControlsWithStateUpdateOnClient");

  Check(update(g_s1.self, 3, good) == S_OK && update(g_s1.self, 0, nullptr) == S_OK, "XGameStreamingUpdateTouchControlsState accepts valid operations");
  Check(show(g_s1.self, "building", 3, good) == S_OK && show(g_s1.self, nullptr, 0, nullptr) == S_OK,
        "XGameStreamingShowTouchControlsWithStateUpdate accepts valid operations and a null layout");
  Check(updateOnClient(g_s1.self, kSomeClient, 3, good) == kClientNotConnected, "XGameStreamingUpdateTouchControlsStateOnClient returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  Check(showOnClient(g_s1.self, kSomeClient, "building", 3, good) == kClientNotConnected,
        "XGameStreamingShowTouchControlsWithStateUpdateOnClient returns E_GAMESTREAMING_CLIENT_NOT_CONNECTED");
  bool rejects = update(g_s1.self, 2, nullptr) == E_INVALIDARG && show(g_s1.self, nullptr, 2, nullptr) == E_INVALIDARG &&
                 updateOnClient(g_s1.self, kSomeClient, 2, nullptr) == E_INVALIDARG && showOnClient(g_s1.self, kSomeClient, nullptr, 2, nullptr) == E_INVALIDARG;
  for (const StateOperation* operation : bad) {
    rejects = rejects && update(g_s1.self, 1, operation) == E_INVALIDARG && show(g_s1.self, "x", 1, operation) == E_INVALIDARG &&
              updateOnClient(g_s1.self, kSomeClient, 1, operation) == E_INVALIDARG && showOnClient(g_s1.self, kSomeClient, "x", 1, operation) == E_INVALIDARG;
  }
  Check(rejects, "touch control state updates reject null arrays, unknown kinds, null paths and null strings with E_INVALIDARG");
}

static void CheckResolution() {
  auto set = g_s3.At<SetResolutionFn>(34, "XGameStreamingSetResolution");
  Check(set(g_s3.self, 1920, 1080) == S_OK && set(g_s3.self, 1280, 720) == S_OK && set(g_s3.self, 3840, 2160) == S_OK,
        "XGameStreamingSetResolution accepts the standard 720p, 1080p and 4K resolutions");
  Check(set(g_s3.self, 2560, 1080) == E_INVALIDARG && set(g_s3.self, 2560, 1440) == E_INVALIDARG,
        "XGameStreamingSetResolution rejects a non-standard resolution outside a streaming server");
  bool rejects = set(g_s3.self, 632, 360) == E_INVALIDARG && set(g_s3.self, 640, 352) == E_INVALIDARG && set(g_s3.self, 1924, 1080) == E_INVALIDARG &&
                 set(g_s3.self, 1920, 1084) == E_INVALIDARG && set(g_s3.self, 0, 0) == E_INVALIDARG;
  Check(rejects, "XGameStreamingSetResolution rejects sizes below 640x360 or not divisible by 8");
}

static void CheckUninitialize() {
  Uninitialize();
  Check(EveryHresultCallIs(kNotInitialized), "every HRESULT method returns E_GAMESTREAMING_NOT_INITIALIZED after XGameStreamingUninitialize");
  Check(Initialize() == S_OK && g_s1.At<GetClientsFn>(10, "XGameStreamingGetClients")(g_s1.self, 0, nullptr, nullptr) == S_OK,
        "XGameStreamingInitialize works again after XGameStreamingUninitialize");
  auto reg = g_s1.At<RegisterConnectionFn>(12, "XGameStreamingRegisterConnectionStateChanged");
  auto unreg = g_s1.At<UnregisterConnectionFn>(13, "XGameStreamingUnregisterConnectionStateChanged");
  Token stale{}, fresh{};
  Check(reg(g_s1.self, nullptr, nullptr, reinterpret_cast<void*>(&ConnectionCallback), &stale) == S_OK, "a registration before a second uninitialize is accepted");
  Uninitialize();
  Check(Initialize() == S_OK, "XGameStreamingInitialize works again a second time");
  Check(reg(g_s1.self, nullptr, nullptr, reinterpret_cast<void*>(&ConnectionCallback), &fresh) == S_OK, "a registration after re-initialization is accepted");
  Check(!unreg(g_s1.self, stale, FALSE) && unreg(g_s1.self, fresh, TRUE),
        "XGameStreamingUninitialize drops its registrations: a token from before it answers false, a token issued after answers true");
  Uninitialize();
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
  g_s1 = Get(kStreaming);
  g_s2 = Get(kStreaming2);
  g_s3 = Get(kStreaming3);
  CheckBeforeInitialize();
  CheckInitialization();
  CheckConnectionState();
  CheckClientProperties();
  CheckLatencyAndInput();
  CheckServerLocation();
  CheckTouchControls();
  CheckResolution();
  CheckUninitialize();
  Check(g_callback_calls == 0, "no registered streaming callback ever fired");
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
