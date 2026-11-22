#include <windows.h>

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
struct Thumbprint { uint32_t type; size_t byteCount; uint8_t* bytes; };
struct SecurityInformation { uint32_t enabledHttpSecurityProtocolFlags; size_t thumbprintCount; Thumbprint* thumbprints; };

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

constexpr HRESULT kNetworkUnreachable = static_cast<HRESULT>(0x800704CF);
constexpr HRESULT kIllegalMethodCall = static_cast<HRESULT>(0x8000000E);

static const GUID kNetworking = {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};
static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

static Api g_threading;
static Api g_net;
static HRESULT WaitAsync(AsyncBlock* block) {
  return g_threading.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(g_threading.self, block, TRUE);
}

typedef HRESULT (*PortFn)(void*, uint16_t*);
typedef HRESULT (*PortAsyncFn)(void*, AsyncBlock*);
typedef HRESULT (*PortResultFn)(void*, AsyncBlock*, uint16_t*);
typedef HRESULT (*RegisterFn)(void*, void*, void*, void*, Token*);
typedef BOOLEAN (*UnregisterFn)(void*, Token, BOOLEAN);
typedef HRESULT (*SecurityAsyncFn)(void*, const char*, AsyncBlock*);
typedef HRESULT (*SecurityUtf16AsyncFn)(void*, const wchar_t*, AsyncBlock*);
typedef HRESULT (*SecuritySizeFn)(void*, AsyncBlock*, size_t*);
typedef HRESULT (*SecurityResultFn)(void*, AsyncBlock*, size_t, size_t*, uint8_t*, SecurityInformation**);
typedef HRESULT (*VerifyFn)(void*, void*, const SecurityInformation*);

static void CheckPreferredPort() {
  auto query = g_net.At<PortFn>(3, "XNetworkingQueryPreferredLocalUdpMultiplayerPort");
  uint16_t port = 0;
  Check(query(g_net.self, &port) == S_OK && port == 3074, "XNetworkingQueryPreferredLocalUdpMultiplayerPort returns the documented default UDP 3074");
  Check(query(g_net.self, nullptr) == E_INVALIDARG, "XNetworkingQueryPreferredLocalUdpMultiplayerPort rejects null");

  auto begin = g_net.At<PortAsyncFn>(4, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync");
  auto result = g_net.At<PortResultFn>(5, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult");
  AsyncBlock block{};
  HRESULT hr = begin(g_net.self, &block);
  HRESULT status = WaitAsync(&block);
  port = 0;
  HRESULT got = result(g_net.self, &block, &port);
  Check(hr == S_OK && status == S_OK && got == S_OK && port == 3074, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync completes with UDP 3074");
  Check(result(g_net.self, &block, &port) == kIllegalMethodCall, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult refuses a second read");
  Check(begin(g_net.self, nullptr) == E_INVALIDARG, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync rejects a null async block");
  AsyncBlock other{};
  begin(g_net.self, &other);
  WaitAsync(&other);
  Check(result(g_net.self, &other, nullptr) == E_INVALIDARG, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult rejects a null port");
  port = 0;
  Check(result(g_net.self, &other, &port) == S_OK && port == 3074, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult still reads after a rejected call");
}

static HANDLE g_port_event;
static int g_port_calls = 0;
static uint16_t g_delivered_port = 0;
static void CALLBACK PortChanged(void* context, uint16_t port) {
  g_port_calls++;
  g_delivered_port = port;
  if (context) SetEvent(static_cast<HANDLE>(context));
}

static void CheckPortChanged() {
  auto reg = g_net.At<RegisterFn>(6, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged");
  auto unreg = g_net.At<UnregisterFn>(7, "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged");
  g_port_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  Token token{};
  HRESULT hr = reg(g_net.self, nullptr, g_port_event, reinterpret_cast<void*>(&PortChanged), &token);
  bool delivered = hr == S_OK && token.value && WaitForSingleObject(g_port_event, 5000) == WAIT_OBJECT_0;
  Check(delivered && g_delivered_port == 3074, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged sends the documented initial notification with UDP 3074");
  Sleep(50);
  Check(g_port_calls == 1, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged sends nothing after the initial notification");
  Token second{};
  HRESULT hr2 = reg(g_net.self, nullptr, g_port_event, reinterpret_cast<void*>(&PortChanged), &second);
  WaitForSingleObject(g_port_event, 5000);
  Check(hr2 == S_OK && second.value && second.value != token.value, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged hands out distinct tokens");
  Check(unreg(g_net.self, token, TRUE) == 1 && unreg(g_net.self, second, TRUE) == 1, "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged returns true");
  Check(unreg(g_net.self, token, FALSE) == 1, "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged returns true for a removed token");
  Token unused{};
  Check(reg(g_net.self, nullptr, nullptr, nullptr, &unused) == E_INVALIDARG, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged rejects a null callback");
  Check(reg(g_net.self, nullptr, nullptr, reinterpret_cast<void*>(&PortChanged), nullptr) == E_INVALIDARG,
        "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged rejects a null token");

  void* manual = nullptr;
  hr = g_threading.At<HRESULT (*)(void*, uint32_t, uint32_t, void**)>(12, "XTaskQueueCreate")(g_threading.self, 0, 0, &manual);
  Token early{};
  int before = g_port_calls;
  hr2 = reg(g_net.self, manual, nullptr, reinterpret_cast<void*>(&PortChanged), &early);
  BOOLEAN removed = unreg(g_net.self, early, FALSE);
  auto dispatch = g_threading.At<BOOLEAN (*)(void*, void*, uint32_t, uint32_t)>(16, "XTaskQueueDispatch");
  while (dispatch(g_threading.self, manual, 1, 0)) {
  }
  Check(hr == S_OK && hr2 == S_OK && removed == 1 && g_port_calls == before,
        "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged before dispatch suppresses the initial notification");
  Token queued{};
  hr2 = reg(g_net.self, manual, nullptr, reinterpret_cast<void*>(&PortChanged), &queued);
  while (dispatch(g_threading.self, manual, 1, 0)) {
  }
  Check(hr2 == S_OK && g_port_calls == before + 1 && g_delivered_port == 3074,
        "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged delivers on the completion port of the given queue");
  unreg(g_net.self, queued, TRUE);
  g_threading.At<void (*)(void*, void*)>(17, "XTaskQueueCloseHandle")(g_threading.self, manual);
}

struct Blocking { HANDLE entered; HANDLE release; volatile LONG calls; };
static void CALLBACK BlockingPortChanged(void* context, uint16_t) {
  auto* b = static_cast<Blocking*>(context);
  InterlockedIncrement(&b->calls);
  SetEvent(b->entered);
  WaitForSingleObject(b->release, 10000);
}

struct UnregisterCall { UnregisterFn unreg; void* self; Token token; BOOLEAN result; };
static DWORD WINAPI UnregisterAndWait(void* p) {
  auto* call = static_cast<UnregisterCall*>(p);
  call->result = call->unreg(call->self, call->token, TRUE);
  return 0;
}

static void CheckPortUnregisterWhileRunning() {
  auto reg = g_net.At<RegisterFn>(6, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged");
  auto unreg = g_net.At<UnregisterFn>(7, "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged");
  Blocking b{CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr), 0};
  Token token{};
  HRESULT hr = reg(g_net.self, nullptr, &b, reinterpret_cast<void*>(&BlockingPortChanged), &token);
  bool running = hr == S_OK && WaitForSingleObject(b.entered, 5000) == WAIT_OBJECT_0;
  Check(running && unreg(g_net.self, token, FALSE) == 0,
        "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(wait=false) during the callback returns false (documented: pending)");
  Check(unreg(g_net.self, token, FALSE) == 0,
        "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged: polling a pending token keeps returning false while the callback runs");
  SetEvent(b.release);
  BOOLEAN done = 0;
  for (int i = 0; i < 500 && !done; ++i) {
    done = unreg(g_net.self, token, FALSE);
    if (!done) Sleep(10);
  }
  Check(done == 1 && b.calls == 1,
        "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged: polling returns true once the callback has returned");

  ResetEvent(b.release);
  Token second{};
  hr = reg(g_net.self, nullptr, &b, reinterpret_cast<void*>(&BlockingPortChanged), &second);
  running = hr == S_OK && WaitForSingleObject(b.entered, 5000) == WAIT_OBJECT_0;
  running = running && unreg(g_net.self, second, FALSE) == 0;
  UnregisterCall call{unreg, g_net.self, second, 2};
  HANDLE thread = CreateThread(nullptr, 0, &UnregisterAndWait, &call, 0, nullptr);
  bool blocked = thread && WaitForSingleObject(thread, 200) == WAIT_TIMEOUT;
  SetEvent(b.release);
  bool finished = thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
  Check(running && blocked && finished && call.result == 1,
        "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(wait=true) after a pending wait=false still blocks until the callback returns");
  if (thread) CloseHandle(thread);
  CloseHandle(b.entered);
  CloseHandle(b.release);
}

static void CheckOfflineQuery(HRESULT begun, AsyncBlock* block, SecuritySizeFn size, SecurityResultFn result, const char* name) {
  char label[200];
  HRESULT status = WaitAsync(block);
  size_t needed = 12345;
  HRESULT sizeHr = size(g_net.self, block, &needed);
  uint8_t buffer[256];
  size_t used = 777;
  SecurityInformation* info = reinterpret_cast<SecurityInformation*>(buffer);
  HRESULT got = result(g_net.self, block, sizeof(buffer), &used, buffer, &info);
  std::snprintf(label, sizeof(label), "%s completes with HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE) and returns no security information", name);
  Check(begun == S_OK && status == kNetworkUnreachable && sizeHr == kNetworkUnreachable && needed == 12345 && got == kNetworkUnreachable &&
            info == nullptr && used == 777,
        label);
  std::snprintf(label, sizeof(label), "%s ResultSize rejects a null size", name);
  Check(size(g_net.self, block, nullptr) == E_INVALIDARG, label);
  std::snprintf(label, sizeof(label), "%s Result rejects a null buffer or a null information pointer", name);
  Check(result(g_net.self, block, sizeof(buffer), &used, nullptr, &info) == E_INVALIDARG &&
            result(g_net.self, block, sizeof(buffer), &used, buffer, nullptr) == E_INVALIDARG,
        label);
  std::snprintf(label, sizeof(label), "%s Result with a zero-size buffer still reports the query failure", name);
  Check(result(g_net.self, block, 0, nullptr, buffer, &info) == kNetworkUnreachable, label);
}

static void CheckSecurityInformation() {
  auto begin = g_net.At<SecurityAsyncFn>(8, "XNetworkingQuerySecurityInformationForUrlAsync");
  auto size = g_net.At<SecuritySizeFn>(9, "XNetworkingQuerySecurityInformationForUrlAsyncResultSize");
  auto result = g_net.At<SecurityResultFn>(10, "XNetworkingQuerySecurityInformationForUrlAsyncResult");
  auto begin16 = g_net.At<SecurityUtf16AsyncFn>(11, "XNetworkingQuerySecurityInformationForUrlUtf16Async");
  auto size16 = g_net.At<SecuritySizeFn>(12, "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize");
  auto result16 = g_net.At<SecurityResultFn>(13, "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult");

  const char* accepted[] = {"https://example.invalid/titles/1?x=y#top", "HTTPS://Example.Invalid:443", "https://[::1]:8443/", "https://a.b-c.invalid"};
  for (const char* url : accepted) {
    AsyncBlock block{};
    char name[200];
    std::snprintf(name, sizeof(name), "XNetworkingQuerySecurityInformationForUrlAsync(%s)", url);
    CheckOfflineQuery(begin(g_net.self, url, &block), &block, size, result, name);
  }
  {
    AsyncBlock block{};
    CheckOfflineQuery(begin16(g_net.self, L"https://example.invalid/\x00e9t\x00e9", &block), &block, size16, result16,
                      "XNetworkingQuerySecurityInformationForUrlUtf16Async");
  }

  const char* rejected[] = {"",
                            "http://example.invalid/",
                            "ftp://example.invalid/",
                            "https://",
                            "https:///path",
                            "https://exa mple.invalid/",
                            "https://example.invalid/\n",
                            "https://example.invalid\\..\\x",
                            "https://example.invalid:0/",
                            "https://example.invalid:65536/",
                            "https://example.invalid:/",
                            "https://example.invalid:12ab/",
                            "https://user@example.invalid/",
                            "https://[::1/",
                            "https://[]/",
                            "https://..example.invalid/",
                            "https://example..invalid/",
                            "https://-example.invalid/",
                            "example.invalid"};
  bool allRejected = true;
  for (const char* url : rejected) {
    AsyncBlock block{};
    if (begin(g_net.self, url, &block) != E_INVALIDARG) {
      std::printf("INFO accepted malformed url \"%s\"\n", url);
      allRejected = false;
    }
  }
  Check(allRejected, "XNetworkingQuerySecurityInformationForUrlAsync rejects empty, non-https and malformed URLs");
  AsyncBlock unused{};
  Check(begin(g_net.self, nullptr, &unused) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlAsync rejects a null url");
  Check(begin(g_net.self, "https://example.invalid/", nullptr) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlAsync rejects a null async block");
  Check(begin16(g_net.self, nullptr, &unused) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlUtf16Async rejects a null url");
  Check(begin16(g_net.self, L"https://example.invalid/", nullptr) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlUtf16Async rejects a null async block");
  Check(begin16(g_net.self, L"http://example.invalid/", &unused) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlUtf16Async rejects a non-https url");
  Check(begin16(g_net.self, L"https://example.invalid/\xd800x", &unused) == E_INVALIDARG,
        "XNetworkingQuerySecurityInformationForUrlUtf16Async rejects invalid UTF-16 (lone surrogate)");
  Check(size(g_net.self, nullptr, nullptr) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlAsyncResultSize rejects null arguments");
  AsyncBlock never{};
  size_t needed = 0;
  Check(size(g_net.self, &never, &needed) == E_INVALIDARG, "XNetworkingQuerySecurityInformationForUrlAsyncResultSize rejects a block that was never started");
}

static void CheckVerifyServerCertificate() {
  auto verify = g_net.At<VerifyFn>(14, "XNetworkingVerifyServerCertificate");
  int requestStandIn = 0;
  void* request = &requestStandIn;
  uint8_t digest[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  Thumbprint pinned{2, sizeof(digest), digest};
  SecurityInformation none{0, 0, nullptr};
  SecurityInformation one{0, 1, &pinned};
  Check(verify(g_net.self, request, &none) == E_NOTIMPL, "XNetworkingVerifyServerCertificate fails closed without pinned thumbprints (no chain can be inspected offline)");
  Check(verify(g_net.self, request, &one) == E_NOTIMPL, "XNetworkingVerifyServerCertificate fails closed with a pinned root thumbprint");
  Check(verify(g_net.self, nullptr, &none) == E_INVALIDARG, "XNetworkingVerifyServerCertificate rejects a null request handle");
  Check(verify(g_net.self, request, nullptr) == E_INVALIDARG, "XNetworkingVerifyServerCertificate rejects null security information");
  SecurityInformation missing{0, 2, nullptr};
  Check(verify(g_net.self, request, &missing) == E_INVALIDARG, "XNetworkingVerifyServerCertificate rejects a thumbprint count without an array");
  Thumbprint badType{3, sizeof(digest), digest};
  SecurityInformation unknownType{0, 1, &badType};
  Check(verify(g_net.self, request, &unknownType) == E_INVALIDARG, "XNetworkingVerifyServerCertificate rejects an unknown thumbprint type");
  Thumbprint empty{0, 0, digest};
  SecurityInformation emptyThumbprint{0, 1, &empty};
  Check(verify(g_net.self, request, &emptyThumbprint) == E_INVALIDARG, "XNetworkingVerifyServerCertificate rejects an empty thumbprint");
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
  g_net = Get(kNetworking);
  CheckPreferredPort();
  CheckPortChanged();
  CheckPortUnregisterWhileRunning();
  CheckSecurityInformation();
  CheckVerifyServerCertificate();
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
