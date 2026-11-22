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
struct HttpHeader { const char* name; const char* value; };
struct Utf16HttpHeader { const wchar_t* name; const wchar_t* value; };
struct TokenData { size_t tokenSize; size_t signatureSize; const char* token; const char* signature; };
struct Utf16TokenData { size_t tokenCount; size_t signatureCount; const wchar_t* token; const wchar_t* signature; };
typedef void(CALLBACK* RemoteShowFn)(void*, uint32_t, void*, const char*, const char*, size_t, const void*);
typedef void(CALLBACK* RemoteCloseFn)(void*, uint32_t, void*);
typedef void(CALLBACK* SpopFn)(void*, uint32_t, void*, const char*, const char*);
struct RemoteConnectHandlers { RemoteShowFn show; RemoteCloseFn close; void* context; };

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
static std::string TableUuid(const char* iface) {
  std::string key = std::string("\"name\": \"") + iface + "\",";
  size_t at = g_table.find(key);
  if (at == std::string::npos) return "";
  size_t uuid = g_table.find("\"uuid\": \"", at);
  size_t next = g_table.find("\"name\": ", at + key.size());
  if (uuid == std::string::npos || (next != std::string::npos && uuid > next)) return "";
  return g_table.substr(uuid + 9, 36);
}
static bool LoadTable() {
  std::ifstream file("table.json", std::ios::binary);
  if (!file) return false;
  std::ostringstream text;
  text << file.rdbuf();
  g_table = text.str();
  return !g_table.empty();
}

static std::string GuidText(const GUID& g) {
  char text[40];
  std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", static_cast<unsigned long>(g.Data1), g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
  return text;
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
static Api Get(const GUID& iid, const char* iface) {
  std::string expected = TableUuid(iface);
  if (expected != GuidText(iid)) {
    std::printf("FAIL guid pin: %s is {%s} in table.json, the test uses {%s}\n", iface, expected.c_str(), GuidText(iid).c_str());
    std::exit(1);
  }
  void* object = nullptr;
  HRESULT hr = g_query(iid, iid, &object);
  if (FAILED(hr) || !object) {
    std::printf("FAIL QueryApiImpl %s %08lx\n", iface, static_cast<unsigned long>(hr));
    std::exit(1);
  }
  return {object, *static_cast<void***>(object)};
}

constexpr HRESULT kNotFound = static_cast<HRESULT>(0x80070490);
constexpr HRESULT kUserNotFound = static_cast<HRESULT>(0x89245104);
constexpr HRESULT kFailedToGetToken = static_cast<HRESULT>(0x89245111);

static const GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID kUser2 = {0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}};
static const GUID kUser3 = {0x1bf2f8c5, 0xd507, 0x4e52, {0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61}};
static const GUID kUser4 = {0x079415e3, 0x6727, 0x437f, {0x8e, 0x9d, 0x8f, 0x8f, 0x9b, 0x24, 0x39, 0xf7}};
static const GUID kUserGamertag = {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};
static const GUID kUser5 = {0x26f3c674, 0xa2fe, 0x44fa, {0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53}};
static const GUID kUser6 = {0x5131d685, 0x4394, 0x4ee6, {0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff}};
static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

static Api g_threading;
static Api g_user;
static void* g_handle = nullptr;
static int g_foreign_object = 0;
static void* const g_foreign = &g_foreign_object;

static HRESULT WaitAsync(AsyncBlock* block) {
  return g_threading.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(g_threading.self, block, TRUE);
}

static void Label(char* out, size_t size, const char* name, const char* what) { std::snprintf(out, size, "%s %s", name, what); }

typedef HRESULT (*ResultSizeFn)(void*, AsyncBlock*, size_t*);

static void CheckGamerPicture() {
  auto begin = g_user.At<HRESULT (*)(void*, void*, uint32_t, AsyncBlock*)>(16, "XUserGetGamerPictureAsync");
  auto resultSize = g_user.At<ResultSizeFn>(17, "XUserGetGamerPictureResultSize");
  auto result = g_user.At<HRESULT (*)(void*, AsyncBlock*, size_t, void*, size_t*)>(18, "XUserGetGamerPictureResult");
  bool allSizes = true;
  for (uint32_t pictureSize = 0; pictureSize < 4; pictureSize++) {
    AsyncBlock b{};
    HRESULT hr = begin(g_user.self, g_handle, pictureSize, &b);
    HRESULT status = WaitAsync(&b);
    size_t size = 77;
    HRESULT sizeHr = resultSize(g_user.self, &b, &size);
    unsigned char buffer[16];
    size_t used = 99;
    HRESULT resultHr = result(g_user.self, &b, sizeof(buffer), buffer, &used);
    allSizes = allSizes && hr == S_OK && status == kNotFound && sizeHr == kNotFound && size == 77 && resultHr == kNotFound && used == 99;
  }
  Check(allSizes, "XUserGetGamerPictureAsync completes with ERROR_NOT_FOUND for every size and ResultSize/Result propagate it (no image data)");

  AsyncBlock b{};
  begin(g_user.self, g_handle, 1, &b);
  WaitAsync(&b);
  Check(result(g_user.self, &b, 16, nullptr, nullptr) == E_INVALIDARG, "XUserGetGamerPictureResult rejects a null buffer with a non-zero size");
  Check(result(g_user.self, &b, 0, nullptr, nullptr) == kNotFound, "XUserGetGamerPictureResult with a zero buffer still reports the call's failure");
  Check(resultSize(g_user.self, &b, nullptr) == E_INVALIDARG, "XUserGetGamerPictureResultSize rejects a null size pointer");

  AsyncBlock unused{};
  Check(begin(g_user.self, nullptr, 0, &unused) == E_INVALIDARG, "XUserGetGamerPictureAsync rejects a null user");
  Check(begin(g_user.self, g_foreign, 0, &unused) == E_INVALIDARG, "XUserGetGamerPictureAsync rejects a handle that is not the local user");
  Check(begin(g_user.self, g_handle, 4, &unused) == E_INVALIDARG, "XUserGetGamerPictureAsync rejects an unknown picture size");
  Check(begin(g_user.self, g_handle, 0, nullptr) == E_INVALIDARG, "XUserGetGamerPictureAsync rejects a null async block");
  Check(result(g_user.self, &unused, 0, nullptr, nullptr) == E_INVALIDARG, "XUserGetGamerPictureResult rejects a block that was never started");
}

static void CheckResolvePrivilege() {
  auto check = g_user.At<HRESULT (*)(void*, void*, uint32_t, uint32_t, uint8_t*, uint32_t*)>(20, "XUserCheckPrivilege");
  auto begin = g_user.At<HRESULT (*)(void*, void*, uint32_t, uint32_t, AsyncBlock*)>(21, "XUserResolvePrivilegeWithUiAsync");
  auto result = g_user.At<HRESULT (*)(void*, AsyncBlock*)>(22, "XUserResolvePrivilegeWithUiResult");
  const uint32_t kMultiplayer = 254;
  uint8_t has = 0;
  Check(check(g_user.self, g_handle, 0, kMultiplayer, &has, nullptr) == S_OK && has == 1, "XUserCheckPrivilege grants the privilege the resolve call is asked about");
  for (uint32_t options = 0; options < 2; options++) {
    AsyncBlock b{};
    HRESULT hr = begin(g_user.self, g_handle, options, kMultiplayer, &b);
    HRESULT status = WaitAsync(&b);
    HRESULT resultHr = result(g_user.self, &b);
    char label[160];
    std::snprintf(label, sizeof(label), "XUserResolvePrivilegeWithUiAsync (options %u) succeeds without UI for a privilege the user already has", options);
    Check(hr == S_OK && status == S_OK && resultHr == S_OK, label);
  }
  AsyncBlock unused{};
  Check(begin(g_user.self, nullptr, 0, kMultiplayer, &unused) == E_INVALIDARG, "XUserResolvePrivilegeWithUiAsync rejects a null user");
  Check(begin(g_user.self, g_foreign, 0, kMultiplayer, &unused) == E_INVALIDARG, "XUserResolvePrivilegeWithUiAsync rejects a handle that is not the local user");
  Check(begin(g_user.self, g_handle, 2, kMultiplayer, &unused) == E_INVALIDARG, "XUserResolvePrivilegeWithUiAsync rejects unknown option bits");
  Check(begin(g_user.self, g_handle, 0, kMultiplayer, nullptr) == E_INVALIDARG, "XUserResolvePrivilegeWithUiAsync rejects a null async block");
  Check(result(g_user.self, &unused) == E_INVALIDARG, "XUserResolvePrivilegeWithUiResult rejects a block that was never started");
}

static HANDLE g_completed;
static void CALLBACK SignalCompletion(AsyncBlock* block) { SetEvent(static_cast<HANDLE>(block->context)); }

template <typename Char, typename Header, typename Data>
static void CheckTokenFamily(int asyncSlot, const char* asyncName, int sizeSlot, const char* sizeName, int resultSlot, const char* resultName,
                             const Char* method, const Char* url, const Header* headers) {
  typedef HRESULT (*BeginFn)(void*, void*, uint32_t, const Char*, const Char*, size_t, const Header*, size_t, const void*, AsyncBlock*);
  auto begin = g_user.At<BeginFn>(asyncSlot, asyncName);
  auto resultSize = g_user.At<ResultSizeFn>(sizeSlot, sizeName);
  auto result = g_user.At<HRESULT (*)(void*, AsyncBlock*, size_t, void*, Data**, size_t*)>(resultSlot, resultName);
  char label[200];

  AsyncBlock b{};
  b.context = g_completed;
  b.callback = &SignalCompletion;
  ResetEvent(g_completed);
  HRESULT hr = begin(g_user.self, g_handle, 0, method, url, 1, headers, 0, nullptr, &b);
  bool called = WaitForSingleObject(g_completed, 5000) == WAIT_OBJECT_0;
  HRESULT status = WaitAsync(&b);
  Label(label, sizeof(label), asyncName, "completes (callback fired) with E_GAMEUSER_FAILED_TO_GET_TOKEN: no token is invented");
  Check(hr == S_OK && called && status == kFailedToGetToken, label);
  size_t size = 77;
  Label(label, sizeof(label), sizeName, "propagates E_GAMEUSER_FAILED_TO_GET_TOKEN");
  Check(resultSize(g_user.self, &b, &size) == kFailedToGetToken && size == 77, label);
  Label(label, sizeof(label), sizeName, "rejects a null size pointer");
  Check(resultSize(g_user.self, &b, nullptr) == E_INVALIDARG, label);
  unsigned char buffer[256];
  Data* data = reinterpret_cast<Data*>(buffer);
  size_t used = 99;
  Label(label, sizeof(label), resultName, "rejects a null data pointer");
  Check(result(g_user.self, &b, sizeof(buffer), buffer, nullptr, &used) == E_INVALIDARG, label);
  Label(label, sizeof(label), resultName, "rejects a null buffer with a non-zero size");
  Check(result(g_user.self, &b, sizeof(buffer), nullptr, &data, &used) == E_INVALIDARG, label);
  Label(label, sizeof(label), resultName, "with a zero buffer reports the call's failure");
  Check(result(g_user.self, &b, 0, nullptr, &data, &used) == kFailedToGetToken && data == nullptr, label);
  data = reinterpret_cast<Data*>(buffer);
  Label(label, sizeof(label), resultName, "propagates E_GAMEUSER_FAILED_TO_GET_TOKEN, clears the data pointer and leaves the used count alone");
  Check(result(g_user.self, &b, sizeof(buffer), buffer, &data, &used) == kFailedToGetToken && data == nullptr && used == 99, label);

  AsyncBlock withBody{};
  const char body[4] = {'b', 'o', 'd', 'y'};
  hr = begin(g_user.self, g_handle, 3, method, url, 0, nullptr, sizeof(body), body, &withBody);
  Label(label, sizeof(label), asyncName, "accepts ForceRefresh|AllUsers with a body and still fails to get a token");
  Check(hr == S_OK && WaitAsync(&withBody) == kFailedToGetToken, label);

  AsyncBlock unused{};
  Label(label, sizeof(label), asyncName, "rejects a null user");
  Check(begin(g_user.self, nullptr, 0, method, url, 0, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a handle that is not the local user");
  Check(begin(g_user.self, g_foreign, 0, method, url, 0, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects unknown option bits");
  Check(begin(g_user.self, g_handle, 4, method, url, 0, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a null method");
  Check(begin(g_user.self, g_handle, 0, nullptr, url, 0, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a null url");
  Check(begin(g_user.self, g_handle, 0, method, nullptr, 0, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a header count without headers");
  Check(begin(g_user.self, g_handle, 0, method, url, 2, nullptr, 0, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a body size without a body");
  Check(begin(g_user.self, g_handle, 0, method, url, 0, nullptr, 4, nullptr, &unused) == E_INVALIDARG, label);
  Label(label, sizeof(label), asyncName, "rejects a null async block");
  Check(begin(g_user.self, g_handle, 0, method, url, 0, nullptr, 0, nullptr, nullptr) == E_INVALIDARG, label);
}

static void CheckResolveIssue() {
  auto begin = g_user.At<HRESULT (*)(void*, void*, const char*, AsyncBlock*)>(29, "XUserResolveIssueWithUiAsync");
  auto result = g_user.At<HRESULT (*)(void*, AsyncBlock*)>(30, "XUserResolveIssueWithUiResult");
  auto beginWide = g_user.At<HRESULT (*)(void*, void*, const wchar_t*, AsyncBlock*)>(31, "XUserResolveIssueWithUiUtf16Async");
  auto resultWide = g_user.At<HRESULT (*)(void*, AsyncBlock*)>(32, "XUserResolveIssueWithUiUtf16Result");
  {
    AsyncBlock withUrl{}, withoutUrl{};
    HRESULT hr1 = begin(g_user.self, g_handle, "https://example.invalid/", &withUrl);
    HRESULT hr2 = begin(g_user.self, g_handle, nullptr, &withoutUrl);
    bool ok = hr1 == S_OK && hr2 == S_OK && WaitAsync(&withUrl) == E_ABORT && WaitAsync(&withoutUrl) == E_ABORT &&
              result(g_user.self, &withUrl) == E_ABORT && result(g_user.self, &withoutUrl) == E_ABORT;
    Check(ok, "XUserResolveIssueWithUiAsync (with and without a url) completes with E_ABORT and shows no UI");
  }
  {
    AsyncBlock withUrl{}, withoutUrl{};
    HRESULT hr1 = beginWide(g_user.self, g_handle, L"https://example.invalid/", &withUrl);
    HRESULT hr2 = beginWide(g_user.self, g_handle, nullptr, &withoutUrl);
    bool ok = hr1 == S_OK && hr2 == S_OK && WaitAsync(&withUrl) == E_ABORT && WaitAsync(&withoutUrl) == E_ABORT &&
              resultWide(g_user.self, &withUrl) == E_ABORT && resultWide(g_user.self, &withoutUrl) == E_ABORT;
    Check(ok, "XUserResolveIssueWithUiUtf16Async (with and without a url) completes with E_ABORT and shows no UI");
  }
  AsyncBlock unused{};
  Check(begin(g_user.self, nullptr, nullptr, &unused) == E_INVALIDARG && beginWide(g_user.self, nullptr, nullptr, &unused) == E_INVALIDARG,
        "XUserResolveIssueWithUi(Utf16)Async reject a null user");
  Check(begin(g_user.self, g_foreign, nullptr, &unused) == E_INVALIDARG && beginWide(g_user.self, g_foreign, nullptr, &unused) == E_INVALIDARG,
        "XUserResolveIssueWithUi(Utf16)Async reject a handle that is not the local user");
  Check(begin(g_user.self, g_handle, nullptr, nullptr) == E_INVALIDARG && beginWide(g_user.self, g_handle, nullptr, nullptr) == E_INVALIDARG,
        "XUserResolveIssueWithUi(Utf16)Async reject a null async block");
  Check(result(g_user.self, &unused) == E_INVALIDARG && resultWide(g_user.self, &unused) == E_INVALIDARG,
        "XUserResolveIssueWithUi(Utf16)Result reject a block that was never started");
}

static void CheckAddById(const Api& user2) {
  auto getId = g_user.At<HRESULT (*)(void*, void*, uint64_t*)>(11, "XUserGetId");
  auto closeHandle = g_user.At<void (*)(void*, void*)>(4, "XUserCloseHandle");
  auto begin = user2.At<HRESULT (*)(void*, uint64_t, AsyncBlock*)>(37, "XUserAddByIdWithUiAsync");
  auto result = user2.At<HRESULT (*)(void*, AsyncBlock*, void**)>(38, "XUserAddByIdWithUiResult");
  uint64_t id = 0;
  getId(g_user.self, g_handle, &id);
  {
    AsyncBlock b{};
    HRESULT hr = begin(user2.self, id, &b);
    HRESULT status = WaitAsync(&b);
    Check(result(user2.self, &b, nullptr) == E_INVALIDARG, "XUserAddByIdWithUiResult rejects a null out pointer");
    void* added = nullptr;
    HRESULT resultHr = result(user2.self, &b, &added);
    Check(hr == S_OK && status == S_OK && resultHr == S_OK && added == g_handle, "XUserAddByIdWithUiAsync adds the signed-in local user by its id without UI");
    if (added) closeHandle(g_user.self, added);
  }
  {
    AsyncBlock b{};
    HRESULT hr = begin(user2.self, id + 1, &b);
    HRESULT status = WaitAsync(&b);
    void* added = g_foreign;
    HRESULT resultHr = result(user2.self, &b, &added);
    Check(hr == S_OK && status == kUserNotFound && resultHr == kUserNotFound && added == nullptr,
          "XUserAddByIdWithUiAsync reports E_GAMEUSER_USER_NOT_FOUND for an id that is not signed in");
  }
  Check(begin(user2.self, id, nullptr) == E_INVALIDARG, "XUserAddByIdWithUiAsync rejects a null async block");
}

static void CheckMsaToken(const Api& user3) {
  auto begin = user3.At<HRESULT (*)(void*, void*, uint32_t, const char*, AsyncBlock*)>(39, "XUserGetMsaTokenSilentlyAsync");
  auto result = user3.At<HRESULT (*)(void*, AsyncBlock*, size_t, char*, size_t*)>(40, "XUserGetMsaTokenSilentlyResult");
  auto resultSize = user3.At<ResultSizeFn>(41, "XUserGetMsaTokenSilentlyResultSize");
  AsyncBlock b{};
  HRESULT hr = begin(user3.self, g_handle, 0, "service::example.invalid::MBI_SSL", &b);
  HRESULT status = WaitAsync(&b);
  size_t size = 77;
  HRESULT sizeHr = resultSize(user3.self, &b, &size);
  char token[64] = "unchanged";
  size_t used = 99;
  Check(result(user3.self, &b, sizeof(token), nullptr, &used) == E_INVALIDARG, "XUserGetMsaTokenSilentlyResult rejects a null buffer with a non-zero size");
  HRESULT zeroHr = result(user3.self, &b, 0, nullptr, &used);
  HRESULT resultHr = result(user3.self, &b, sizeof(token), token, &used);
  Check(hr == S_OK && status == kFailedToGetToken && sizeHr == kFailedToGetToken && size == 77 && zeroHr == kFailedToGetToken &&
            resultHr == kFailedToGetToken && used == 99 && !std::strcmp(token, "unchanged"),
        "XUserGetMsaTokenSilentlyAsync fails with E_GAMEUSER_FAILED_TO_GET_TOKEN and ResultSize/Result propagate it (no token is invented)");
  Check(resultSize(user3.self, &b, nullptr) == E_INVALIDARG, "XUserGetMsaTokenSilentlyResultSize rejects a null size pointer");
  AsyncBlock unused{};
  Check(begin(user3.self, nullptr, 0, "scope", &unused) == E_INVALIDARG, "XUserGetMsaTokenSilentlyAsync rejects a null user");
  Check(begin(user3.self, g_foreign, 0, "scope", &unused) == E_INVALIDARG, "XUserGetMsaTokenSilentlyAsync rejects a handle that is not the local user");
  Check(begin(user3.self, g_handle, 1, "scope", &unused) == E_INVALIDARG, "XUserGetMsaTokenSilentlyAsync rejects unknown options");
  Check(begin(user3.self, g_handle, 0, nullptr, &unused) == E_INVALIDARG, "XUserGetMsaTokenSilentlyAsync rejects a null scope");
  Check(begin(user3.self, g_handle, 0, "scope", nullptr) == E_INVALIDARG, "XUserGetMsaTokenSilentlyAsync rejects a null async block");
}

static int g_prompt_calls = 0;
static void CALLBACK RemoteShow(void*, uint32_t, void*, const char*, const char*, size_t, const void*) { g_prompt_calls++; }
static void CALLBACK RemoteClose(void*, uint32_t, void*) { g_prompt_calls++; }
static void CALLBACK SpopPrompt(void*, uint32_t, void*, const char*, const char*) { g_prompt_calls++; }

static void CheckPlatformPrompts(const Api& user5) {
  auto setRemote = user5.At<HRESULT (*)(void*, void*, const RemoteConnectHandlers*)>(43, "XUserPlatformRemoteConnectSetEventHandlers");
  auto cancelRemote = user5.At<HRESULT (*)(void*, void*)>(44, "XUserPlatformRemoteConnectCancelPrompt");
  auto setSpop = user5.At<HRESULT (*)(void*, void*, SpopFn, void*)>(45, "XUserPlatformSpopPromptSetEventHandlers");
  auto completeSpop = user5.At<HRESULT (*)(void*, void*, uint32_t)>(46, "XUserPlatformSpopPromptComplete");
  auto createQueue = g_threading.At<HRESULT (*)(void*, uint32_t, uint32_t, void**)>(12, "XTaskQueueCreate");
  auto dispatch = g_threading.At<BOOLEAN (*)(void*, void*, uint32_t, uint32_t)>(16, "XTaskQueueDispatch");
  auto closeQueue = g_threading.At<void (*)(void*, void*)>(17, "XTaskQueueCloseHandle");

  void* manual = nullptr;
  HRESULT created = createQueue(g_threading.self, 0, 0, &manual);
  RemoteConnectHandlers handlers{&RemoteShow, &RemoteClose, &g_prompt_calls};
  HRESULT first = setRemote(user5.self, nullptr, &handlers);
  HRESULT second = setRemote(user5.self, manual, &handlers);
  RemoteConnectHandlers cleared{nullptr, nullptr, nullptr};
  HRESULT third = setRemote(user5.self, nullptr, &cleared);
  HRESULT fourth = setRemote(user5.self, manual, &handlers);
  Check(created == S_OK && first == S_OK && second == S_OK && third == S_OK && fourth == S_OK,
        "XUserPlatformRemoteConnectSetEventHandlers stores and replaces handlers, with and without a queue");
  Check(setRemote(user5.self, nullptr, nullptr) == E_INVALIDARG, "XUserPlatformRemoteConnectSetEventHandlers rejects null handlers");
  Check(cancelRemote(user5.self, nullptr) == E_INVALIDARG && cancelRemote(user5.self, g_foreign) == E_INVALIDARG,
        "XUserPlatformRemoteConnectCancelPrompt rejects an operation when no prompt is active");

  HRESULT spop1 = setSpop(user5.self, nullptr, &SpopPrompt, &g_prompt_calls);
  HRESULT spop2 = setSpop(user5.self, manual, &SpopPrompt, nullptr);
  Check(spop1 == S_OK && spop2 == S_OK, "XUserPlatformSpopPromptSetEventHandlers stores and replaces the handler, with and without a queue");
  Check(setSpop(user5.self, nullptr, nullptr, nullptr) == E_INVALIDARG, "XUserPlatformSpopPromptSetEventHandlers rejects a null handler");
  bool rejected = true;
  for (uint32_t result = 0; result < 4; result++) rejected = rejected && completeSpop(user5.self, g_foreign, result) == E_INVALIDARG;
  Check(rejected && completeSpop(user5.self, nullptr, 0) == E_INVALIDARG, "XUserPlatformSpopPromptComplete rejects an operation when no prompt is active");
  Check(completeSpop(user5.self, g_foreign, 4) == E_INVALIDARG, "XUserPlatformSpopPromptComplete rejects an unknown result");

  Sleep(50);
  bool work = dispatch(g_threading.self, manual, 0, 0);
  bool completion = dispatch(g_threading.self, manual, 1, 0);
  Check(!work && !completion && g_prompt_calls == 0, "no remote-connect or SPOP handler is ever invoked or queued");
  closeQueue(g_threading.self, manual);
}

static void CheckSignOut(const Api& user6) {
  auto present = user6.At<BOOLEAN (*)(void*)>(47, "XUserIsSignOutPresent");
  auto begin = user6.At<HRESULT (*)(void*, void*, AsyncBlock*)>(48, "XUserSignOutAsync");
  auto result = user6.At<HRESULT (*)(void*, AsyncBlock*)>(49, "XUserSignOutResult");
  auto getState = g_user.At<HRESULT (*)(void*, void*, uint32_t*)>(14, "XUserGetState");
  auto getId = g_user.At<HRESULT (*)(void*, void*, uint64_t*)>(11, "XUserGetId");
  auto findById = g_user.At<HRESULT (*)(void*, uint64_t, void**)>(12, "XUserFindUserById");
  auto closeHandle = g_user.At<void (*)(void*, void*)>(4, "XUserCloseHandle");
  Check(present(user6.self) == 0, "XUserIsSignOutPresent reports that the game cannot sign users out here");
  AsyncBlock b{};
  HRESULT hr = begin(user6.self, g_handle, &b);
  HRESULT status = WaitAsync(&b);
  HRESULT resultHr = result(user6.self, &b);
  Check(hr == S_OK && status == E_NOTIMPL && resultHr == E_NOTIMPL, "XUserSignOutAsync completes with E_NOTIMPL and XUserSignOutResult propagates it");
  uint32_t state = 9;
  uint64_t id = 0;
  void* found = nullptr;
  getId(g_user.self, g_handle, &id);
  HRESULT findHr = findById(g_user.self, id, &found);
  Check(getState(g_user.self, g_handle, &state) == S_OK && state == 0 && findHr == S_OK && found == g_handle,
        "the local user stays signed in and findable after XUserSignOutAsync");
  if (found) closeHandle(g_user.self, found);
  AsyncBlock unused{};
  Check(begin(user6.self, nullptr, &unused) == E_INVALIDARG, "XUserSignOutAsync rejects a null user");
  Check(begin(user6.self, g_foreign, &unused) == E_INVALIDARG, "XUserSignOutAsync rejects a handle that is not the local user");
  Check(begin(user6.self, g_handle, nullptr) == E_INVALIDARG, "XUserSignOutAsync rejects a null async block");
  Check(result(user6.self, &unused) == E_INVALIDARG, "XUserSignOutResult rejects a block that was never started");
}

struct LocalId { uint64_t value; };
struct RegistrationToken { uint64_t token; };
static void CALLBACK NeverFiredChange(void*, LocalId, uint32_t) {}

static void CheckProfileAnswers(const Api& user4) {
  auto closeHandle = g_user.At<void (*)(void*, void*)>(4, "XUserCloseHandle");
  uint32_t maxUsers = 0;
  Check(g_user.At<HRESULT (*)(void*, uint32_t*)>(6, "XUserGetMaxUsers")(g_user.self, &maxUsers) == S_OK && maxUsers == 4,
        "XUserGetMaxUsers reports 4 (a choice; no GDK page gives a number)");
  Check(user4.At<BOOLEAN (*)(void*, void*)>(42, "XUserIsStoreUser")(user4.self, g_handle) == 1,
        "XUserIsStoreUser is true, the documented console answer");
  uint32_t ageGroup = 9;
  Check(g_user.At<HRESULT (*)(void*, void*, uint32_t*)>(19, "XUserGetAgeGroup")(g_user.self, g_handle, &ageGroup) == S_OK && ageGroup == 3,
        "XUserGetAgeGroup reports an adult");
  BOOLEAN guest = 9;
  Check(g_user.At<HRESULT (*)(void*, void*, BOOLEAN*)>(13, "XUserGetIsGuest")(g_user.self, g_handle, &guest) == S_OK && guest == 0,
        "XUserGetIsGuest reports that the local user is not a guest");

  auto compare = g_user.At<int32_t (*)(void*, void*, void*)>(5, "XUserCompare");
  int32_t forward = compare(g_user.self, g_handle, nullptr);
  int32_t backward = compare(g_user.self, nullptr, g_handle);
  Check(compare(g_user.self, g_handle, g_handle) == 0 && forward != 0 && backward != 0 && (forward < 0) != (backward < 0),
        "XUserCompare is 0 for the same user and orders two different handles consistently");

  LocalId localId{};
  void* found = nullptr;
  HRESULT getHr = g_user.At<HRESULT (*)(void*, void*, LocalId*)>(9, "XUserGetLocalId")(g_user.self, g_handle, &localId);
  auto findByLocalId = g_user.At<HRESULT (*)(void*, LocalId, void**)>(10, "XUserFindUserByLocalId");
  HRESULT findHr = findByLocalId(g_user.self, localId, &found);
  Check(getHr == S_OK && findHr == S_OK && found == g_handle, "XUserFindUserByLocalId finds the user XUserGetLocalId named");
  if (found) closeHandle(g_user.self, found);
  void* missing = g_foreign;
  LocalId other{localId.value + 1};
  Check(findByLocalId(g_user.self, other, &missing) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "XUserFindUserByLocalId reports ERROR_NOT_FOUND for another local id");
}

static void CheckGamertag(const Api& gamertag) {
  auto get = gamertag.At<HRESULT (*)(void*, void*, uint32_t, size_t, char*, size_t*)>(3, "XUserGetGamertag");
  char modern[128] = {}, classic[128] = {}, suffix[128] = {'x'}, unique[128] = {};
  size_t modernUsed = 0, classicUsed = 0, suffixUsed = 9, uniqueUsed = 0;
  HRESULT modernHr = get(gamertag.self, g_handle, 1, sizeof(modern), modern, &modernUsed);
  Check(modernHr == S_OK && modern[0] && modernUsed == std::strlen(modern) + 1, "XUserGetGamertag gives the modern gamertag and counts its terminator");
  size_t classicLength = std::strlen(modern) < 15 ? std::strlen(modern) : 15;
  Check(get(gamertag.self, g_handle, 0, sizeof(classic), classic, &classicUsed) == S_OK && classicUsed == classicLength + 1 &&
            std::strncmp(classic, modern, classicLength) == 0 && classic[classicLength] == 0,
        "XUserGetGamertag cuts the classic gamertag to 15 bytes");
  Check(get(gamertag.self, g_handle, 2, sizeof(suffix), suffix, &suffixUsed) == S_OK && suffix[0] == 0 && suffixUsed == 1,
        "XUserGetGamertag gives an empty modern suffix");
  Check(get(gamertag.self, g_handle, 3, sizeof(unique), unique, &uniqueUsed) == S_OK && std::strcmp(unique, modern) == 0 && uniqueUsed == modernUsed,
        "XUserGetGamertag gives the modern gamertag as the unique modern one");
  char exact[128] = {};
  Check(get(gamertag.self, g_handle, 1, modernUsed, exact, nullptr) == S_OK && std::strcmp(exact, modern) == 0,
        "XUserGetGamertag fills a buffer of exactly the reported size, without a used pointer");
  char tooShort[128] = {};
  Check(get(gamertag.self, g_handle, 1, modernUsed - 1, tooShort, nullptr) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
        "XUserGetGamertag reports ERROR_INSUFFICIENT_BUFFER for a buffer one byte short");
  Check(get(gamertag.self, g_handle, 4, sizeof(modern), modern, nullptr) == E_INVALIDARG, "XUserGetGamertag rejects an unknown component");
  Check(get(gamertag.self, nullptr, 1, sizeof(modern), modern, nullptr) == E_INVALIDARG, "XUserGetGamertag rejects a null user");
  Check(get(gamertag.self, g_handle, 1, sizeof(modern), nullptr, nullptr) == E_INVALIDARG, "XUserGetGamertag rejects a null buffer");
}

static void CheckChangeEvent() {
  auto reg = g_user.At<HRESULT (*)(void*, void*, void*, void*, RegistrationToken*)>(33, "XUserRegisterForChangeEvent");
  auto unreg = g_user.At<BOOLEAN (*)(void*, RegistrationToken, BOOLEAN)>(34, "XUserUnregisterForChangeEvent");
  void* callback = reinterpret_cast<void*>(&NeverFiredChange);
  RegistrationToken first{}, second{};
  HRESULT firstHr = reg(g_user.self, nullptr, nullptr, callback, &first);
  HRESULT secondHr = reg(g_user.self, nullptr, nullptr, callback, &second);
  Check(firstHr == S_OK && secondHr == S_OK && first.token && second.token && first.token != second.token,
        "XUserRegisterForChangeEvent gives each registration its own token");
  RegistrationToken untouched{77};
  Check(reg(g_user.self, nullptr, nullptr, nullptr, &untouched) == E_INVALIDARG && untouched.token == 77,
        "XUserRegisterForChangeEvent rejects a null callback and leaves the token alone");
  Check(reg(g_user.self, nullptr, nullptr, callback, nullptr) == E_INVALIDARG, "XUserRegisterForChangeEvent rejects a null token");
  Check(unreg(g_user.self, first, TRUE) == TRUE && unreg(g_user.self, second, FALSE) == TRUE, "XUserUnregisterForChangeEvent succeeds with and without wait");
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
  g_threading = Get(kThreading, "IXThreadingImpl");
  g_user = Get(kUser, "IXUserImpl");
  Api user2 = Get(kUser2, "IXUserImpl2");
  Api user3 = Get(kUser3, "IXUserImpl3");
  Api user4 = Get(kUser4, "IXUserImpl4");
  Api user5 = Get(kUser5, "IXUserImpl5");
  Api user6 = Get(kUser6, "IXUserImpl6");
  Api gamertag = Get(kUserGamertag, "IXUserGamertagImpl");
  g_completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  AsyncBlock add{};
  HRESULT hr = g_user.At<HRESULT (*)(void*, uint32_t, AsyncBlock*)>(7, "XUserAddAsync")(g_user.self, 1, &add);
  WaitAsync(&add);
  HRESULT added = g_user.At<HRESULT (*)(void*, AsyncBlock*, void**)>(8, "XUserAddResult")(g_user.self, &add, &g_handle);
  Check(hr == S_OK && added == S_OK && g_handle, "XUserAddAsync gives the local user");
  if (!g_handle) return 1;

  CheckProfileAnswers(user4);
  CheckGamertag(gamertag);
  CheckChangeEvent();
  CheckGamerPicture();
  CheckResolvePrivilege();
  const HttpHeader headers[] = {{"Accept", "application/json"}};
  CheckTokenFamily<char, HttpHeader, TokenData>(23, "XUserGetTokenAndSignatureAsync", 24, "XUserGetTokenAndSignatureResultSize", 25,
                                                "XUserGetTokenAndSignatureResult", "GET", "https://example.invalid/", headers);
  const Utf16HttpHeader wideHeaders[] = {{L"Accept", L"application/json"}};
  CheckTokenFamily<wchar_t, Utf16HttpHeader, Utf16TokenData>(26, "XUserGetTokenAndSignatureUtf16Async", 27, "XUserGetTokenAndSignatureUtf16ResultSize",
                                                             28, "XUserGetTokenAndSignatureUtf16Result", L"GET", L"https://example.invalid/", wideHeaders);
  CheckResolveIssue();
  CheckAddById(user2);
  CheckMsaToken(user3);
  CheckPlatformPrompts(user5);
  CheckSignOut(user6);

  g_user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(g_user.self, g_handle);
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
