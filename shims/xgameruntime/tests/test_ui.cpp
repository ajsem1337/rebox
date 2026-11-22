#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

typedef HRESULT (*QueryApi)(const GUID&, const GUID&, void**);
struct AsyncBlock { void* queue; void* context; void (*callback)(AsyncBlock*); unsigned char internal[32]; };
struct Token { uint64_t value; };

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
static Api Get(const char* iface, const GUID& iid) {
  if (TableUuid(iface) != GuidText(iid)) {
    std::printf("FAIL guid pin: %s is %s in table.json, the test uses %s\n", iface, TableUuid(iface).c_str(), GuidText(iid).c_str());
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

static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
static const GUID kUser = {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID kGameUi = {0x6eeaa73e, 0x9669, 0x43ad, {0xa2, 0xc7, 0xd0, 0xda, 0x4e, 0x1f, 0x50, 0xa1}};
static const GUID kGameUi4 = {0xeaf669df, 0x5542, 0x4590, {0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37, 0xcc}};

constexpr uint32_t kManual = 0;
constexpr uint32_t kWork = 0;

struct PlayerPickerInfo {
  void* requestingUser;
  const char* promptText;
  uint32_t selectFromPlayersCount;
  const uint64_t* selectFromPlayers;
  uint32_t preSelectedPlayersCount;
  const uint64_t* preSelectedPlayers;
  uint32_t minSelectionCount;
  uint32_t maxSelectionCount;
};

struct UiCallbacks {
  void* context;
  void(CALLBACK* showPlayerProfileCard)(void*, void*, void*, uint64_t, void*);
  void(CALLBACK* showPlayerPicker)(void*, void*, PlayerPickerInfo*, void*);
  void(CALLBACK* showSendGameInvite)(void*, void*, void*, const char*, const char*, const char*, const char*, const char*, void*);
  void(CALLBACK* showAchievements)(void*, void*, void*, uint32_t, void*);
  void(CALLBACK* showMultiplayerActivityGameInvite)(void*, void*, void*, void*);
  void(CALLBACK* showMessageDialog)(void*, void*, const char*, const char*, const char*, const char*, const char*, uint32_t, uint32_t, void*);
  void(CALLBACK* showErrorDialog)(void*, void*, HRESULT, const char*, void*);
  void(CALLBACK* showTextEntry)(void*, void*, const char*, const char*, const char*, uint32_t, uint32_t, void*);
};

struct WebAuthenticationResultData {
  HRESULT responseStatus;
  size_t responseCompletionUriSize;
  const char* responseCompletionUri;
};

struct Seen {
  void* handle = nullptr;
  void* queue = nullptr;
  void* context = nullptr;
  void* user = nullptr;
  std::string text[5];
  bool null[5] = {};
  uint64_t number = 0;
  uint32_t values[2] = {};
  std::vector<uint64_t> players;
};
static Seen g_seen;
static std::atomic<int> g_calls{0};
static int g_context_marker = 0;
static Api g_ui;
static Api g_ui4;
static Api g_threading;
static void* g_user = nullptr;

static void Keep(int index, const char* text) {
  g_seen.null[index] = text == nullptr;
  g_seen.text[index] = text ? text : "";
}
static void Reset() {
  g_seen = Seen{};
  g_calls = 0;
}
static void Saw(void* handle, void* queue, void* context) {
  g_seen.handle = handle;
  g_seen.queue = queue;
  g_seen.context = context;
}

static void CALLBACK OnProfileCard(void* handle, void* queue, void* user, uint64_t target, void* context) {
  Saw(handle, queue, context);
  g_seen.user = user;
  g_seen.number = target;
  g_calls++;
}
static void CALLBACK OnPlayerPicker(void* handle, void* queue, PlayerPickerInfo* info, void* context) {
  Saw(handle, queue, context);
  g_seen.user = info->requestingUser;
  Keep(0, info->promptText);
  g_seen.players.assign(info->selectFromPlayers, info->selectFromPlayers + info->selectFromPlayersCount);
  g_seen.number = info->preSelectedPlayersCount ? info->preSelectedPlayers[0] : 0;
  g_seen.values[0] = info->minSelectionCount;
  g_seen.values[1] = info->maxSelectionCount;
  g_calls++;
}
static void CALLBACK OnSendGameInvite(void* handle, void* queue, void* user, const char* scid, const char* templateName, const char* sessionId,
                                      const char* invitationText, const char* customActivationContext, void* context) {
  Saw(handle, queue, context);
  g_seen.user = user;
  Keep(0, scid);
  Keep(1, templateName);
  Keep(2, sessionId);
  Keep(3, invitationText);
  Keep(4, customActivationContext);
  g_calls++;
}
static void CALLBACK OnAchievements(void* handle, void* queue, void* user, uint32_t titleId, void* context) {
  Saw(handle, queue, context);
  g_seen.user = user;
  g_seen.number = titleId;
  g_calls++;
}
static void CALLBACK OnMessageDialog(void* handle, void* queue, const char* title, const char* content, const char* first, const char* second,
                                     const char* third, uint32_t defaultButton, uint32_t cancelButton, void* context) {
  Saw(handle, queue, context);
  Keep(0, title);
  Keep(1, content);
  Keep(2, first);
  Keep(3, second);
  Keep(4, third);
  g_seen.values[0] = defaultButton;
  g_seen.values[1] = cancelButton;
  g_calls++;
}
static HRESULT ErrorDialogResponse(void* handle);
static HRESULT g_answer_in_callback = E_PENDING;
static void CALLBACK OnErrorDialog(void* handle, void* queue, HRESULT error, const char* serviceContext, void* context) {
  Saw(handle, queue, context);
  g_seen.number = static_cast<uint32_t>(error);
  Keep(0, serviceContext);
  g_answer_in_callback = ErrorDialogResponse(handle);
  g_calls++;
}
static void CALLBACK OnTextEntry(void* handle, void* queue, const char* title, const char* description, const char* defaultText, uint32_t scope,
                                 uint32_t maxLength, void* context) {
  Saw(handle, queue, context);
  Keep(0, title);
  Keep(1, description);
  Keep(2, defaultText);
  g_seen.values[0] = scope;
  g_seen.values[1] = maxLength;
  g_calls++;
}

static HRESULT SetCallbacks(const UiCallbacks* callbacks) {
  return g_ui4.At<HRESULT (*)(void*, const UiCallbacks*, BOOLEAN)>(42, "XGameUiSetUiCallbacks")(g_ui4.self, callbacks, TRUE);
}
static HRESULT MessageDialogResponse(void* handle, uint32_t button) {
  return g_ui4.At<HRESULT (*)(void*, void*, uint32_t)>(43, "XGameUiSetMessageDialogUiResponse")(g_ui4.self, handle, button);
}
static HRESULT PlayerPickerResponse(void* handle, uint32_t count, const uint64_t* players) {
  return g_ui4.At<HRESULT (*)(void*, void*, uint32_t, const uint64_t*)>(44, "XGameUiSetPlayerPickerUiResponse")(g_ui4.self, handle, count, players);
}
static HRESULT TextEntryResponse(void* handle, const char* text) {
  return g_ui4.At<HRESULT (*)(void*, void*, const char*)>(45, "XGameUiSetTextEntryUiResponse")(g_ui4.self, handle, text);
}
static HRESULT HandleOnlyResponse(int slot, const char* name, void* handle) {
  return g_ui4.At<HRESULT (*)(void*, void*)>(slot, name)(g_ui4.self, handle);
}
static HRESULT ErrorDialogResponse(void* handle) { return HandleOnlyResponse(50, "XGameUiSetErrorDialogUiResponse", handle); }

static HRESULT Wait(AsyncBlock* block) {
  return g_threading.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(g_threading.self, block, TRUE);
}
static void Cancel(AsyncBlock* block) { g_threading.At<void (*)(void*, AsyncBlock*)>(5, "XAsyncCancel")(g_threading.self, block); }
static bool WaitForCall(int calls) {
  for (int i = 0; i < 500 && g_calls < calls; i++) Sleep(10);
  return g_calls >= calls;
}

static HRESULT ShowMessageDialog(AsyncBlock* block, const char* title) {
  return g_ui.At<HRESULT (*)(void*, AsyncBlock*, const char*, const char*, const char*, const char*, const char*, uint32_t, uint32_t)>(
      3, "XGameUiShowMessageDialogAsync")(g_ui.self, block, title, "Content", "Yes", nullptr, "Cancel", 0, 2);
}
static HRESULT MessageDialogResult(AsyncBlock* block, uint32_t* button) {
  return g_ui.At<HRESULT (*)(void*, AsyncBlock*, uint32_t*)>(4, "XGameUiShowMessageDialogResult")(g_ui.self, block, button);
}
static HRESULT ShowTextEntry(AsyncBlock* block) {
  return g_ui.At<HRESULT (*)(void*, AsyncBlock*, const char*, const char*, const char*, uint32_t, uint32_t)>(17, "XGameUiShowTextEntryAsync")(
      g_ui.self, block, "Name", "Pick a name", "Player", 3, 16);
}

static void CheckWebAuthentication() {
  auto show = g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, const char*, const char*)>(24, "XGameUiShowWebAuthenticationAsync");
  auto resultSize = g_ui.At<HRESULT (*)(void*, AsyncBlock*, size_t*)>(25, "XGameUiShowWebAuthenticationResultSize");
  auto result = g_ui.At<HRESULT (*)(void*, AsyncBlock*, size_t, void*, WebAuthenticationResultData**, size_t*)>(26, "XGameUiShowWebAuthenticationResult");
  auto showWithOptions = g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, const char*, const char*, uint32_t)>(27, "XGameUiShowWebAuthenticationWithOptionsAsync");
  const char* request = "https://login.example.invalid/authorize?client_id=1";
  const char* completion = "https://login.example.invalid/done";

  AsyncBlock block{};
  Check(show(g_ui.self, &block, g_user, request, completion) == S_OK && Wait(&block) == E_ABORT,
        "XGameUiShowWebAuthenticationAsync completes with E_ABORT (no web view; nothing is fetched)");
  size_t size = 77;
  unsigned char buffer[256];
  WebAuthenticationResultData* data = reinterpret_cast<WebAuthenticationResultData*>(buffer);
  size_t used = 0;
  Check(resultSize(g_ui.self, &block, &size) == E_ABORT && result(g_ui.self, &block, sizeof(buffer), buffer, &data, &used) == E_ABORT && !data,
        "XGameUiShowWebAuthenticationResultSize and Result report E_ABORT and hand out no data");
  AsyncBlock options{};
  Check(showWithOptions(g_ui.self, &options, g_user, request, completion, 1) == S_OK && Wait(&options) == E_ABORT,
        "XGameUiShowWebAuthenticationWithOptionsAsync (PreferFullscreen, ignored on PC) completes with E_ABORT");
  AsyncBlock bad{};
  Check(show(g_ui.self, &bad, g_user, "http://login.example.invalid/", completion) == E_INVALIDARG,
        "XGameUiShowWebAuthenticationAsync rejects a request URI that is not HTTPS (documented)");
  Check(show(g_ui.self, &bad, nullptr, request, completion) == E_INVALIDARG && show(g_ui.self, &bad, g_user, nullptr, completion) == E_INVALIDARG &&
            show(g_ui.self, &bad, g_user, request, nullptr) == E_INVALIDARG && show(g_ui.self, nullptr, g_user, request, completion) == E_INVALIDARG,
        "XGameUiShowWebAuthenticationAsync rejects null arguments");
  Check(showWithOptions(g_ui.self, &bad, g_user, request, completion, 2) == E_INVALIDARG, "XGameUiShowWebAuthenticationWithOptionsAsync rejects unknown options");
}

static void CheckMessageDialogCallback(const UiCallbacks& callbacks) {
  auto create = g_threading.At<HRESULT (*)(void*, uint32_t, uint32_t, void**)>(12, "XTaskQueueCreate");
  auto dispatch = g_threading.At<bool (*)(void*, void*, uint32_t, uint32_t)>(16, "XTaskQueueDispatch");
  auto close = g_threading.At<void (*)(void*, void*)>(17, "XTaskQueueCloseHandle");
  Check(SetCallbacks(&callbacks) == S_OK, "XGameUiSetUiCallbacks accepts the title's callbacks");

  void* queue = nullptr;
  create(g_threading.self, kManual, kManual, &queue);
  AsyncBlock block{};
  block.queue = queue;
  char title[16] = "Quit?";
  Reset();
  Check(ShowMessageDialog(&block, title) == S_OK, "XGameUiShowMessageDialogAsync starts with a title callback set");
  std::strcpy(title, "changed");
  Check(g_calls == 0, "the callback does not run before the async work is dispatched");
  dispatch(g_threading.self, queue, kWork, 0);
  Check(g_calls == 1 && g_seen.handle && g_seen.queue == queue && g_seen.context == &g_context_marker,
        "the message dialog callback runs on the async block's queue with a handle and the title's context");
  Check(g_seen.text[0] == "Quit?" && g_seen.text[1] == "Content" && g_seen.text[2] == "Yes" && g_seen.null[3] && g_seen.text[4] == "Cancel" &&
            g_seen.values[0] == 0 && g_seen.values[1] == 2,
        "the callback gets copies of the dialog's texts and buttons (null stays null)");
  uint32_t button = 9;
  Check(MessageDialogResult(&block, &button) == E_PENDING, "the dialog stays pending until the title answers");
  Check(TextEntryResponse(g_seen.handle, "x") == E_INVALIDARG, "a message dialog handle is refused by another UI's response call");
  Check(MessageDialogResponse(g_seen.handle, 3) == E_INVALIDARG, "XGameUiSetMessageDialogUiResponse rejects an unknown button");
  Check(MessageDialogResponse(g_seen.handle, 1) == S_OK, "XGameUiSetMessageDialogUiResponse answers the dialog");
  Check(Wait(&block) == S_OK && MessageDialogResult(&block, &button) == S_OK && button == 1,
        "XGameUiShowMessageDialogResult returns the button the title answered with");
  Check(MessageDialogResponse(g_seen.handle, 0) == E_INVALIDARG, "a callback handle can be answered only once");
  close(g_threading.self, queue);

  AsyncBlock canceled{};
  Reset();
  Check(ShowMessageDialog(&canceled, "Quit?") == S_OK && WaitForCall(1), "a dialog on the process queue reaches the callback");
  Check(g_seen.queue != nullptr, "the callback gets the process task queue when the block has none");
  Cancel(&canceled);
  Check(Wait(&canceled) == E_ABORT, "XAsyncCancel ends a dialog the title has not answered with E_ABORT");
  Check(MessageDialogResponse(g_seen.handle, 0) == E_INVALIDARG, "the handle of a canceled dialog is refused");
}

static void CheckOtherCallbacks() {
  AsyncBlock picker{};
  const uint64_t candidates[] = {11, 22, 33};
  const uint64_t preselected[] = {22};
  Reset();
  HRESULT begin = g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, const char*, uint32_t, const uint64_t*, uint32_t, const uint64_t*, uint32_t, uint32_t)>(
      11, "XGameUiShowPlayerPickerAsync")(g_ui.self, &picker, g_user, "Invite", 3, candidates, 1, preselected, 1, 2);
  Check(begin == S_OK && WaitForCall(1) && g_seen.user == g_user && g_seen.text[0] == "Invite" && g_seen.players == std::vector<uint64_t>{11, 22, 33} &&
            g_seen.number == 22 && g_seen.values[0] == 1 && g_seen.values[1] == 2,
        "the player picker callback gets the requesting user, prompt, candidates, preselection and limits");
  const uint64_t chosen[] = {33, 11};
  Check(PlayerPickerResponse(g_seen.handle, 2, nullptr) == E_INVALIDARG, "XGameUiSetPlayerPickerUiResponse rejects a count without players");
  Check(PlayerPickerResponse(g_seen.handle, 2, chosen) == S_OK && Wait(&picker) == S_OK, "XGameUiSetPlayerPickerUiResponse completes the picker");
  uint32_t count = 0;
  uint64_t out[4] = {};
  uint32_t used = 0;
  HRESULT countHr = g_ui.At<HRESULT (*)(void*, AsyncBlock*, uint32_t*)>(12, "XGameUiShowPlayerPickerResultCount")(g_ui.self, &picker, &count);
  HRESULT resultHr = g_ui.At<HRESULT (*)(void*, AsyncBlock*, uint32_t, uint64_t*, uint32_t*)>(13, "XGameUiShowPlayerPickerResult")(g_ui.self, &picker, 4, out, &used);
  Check(countHr == S_OK && count == 2 && resultHr == S_OK && used == 2 && out[0] == 33 && out[1] == 11,
        "XGameUiShowPlayerPickerResultCount and Result return the players the title chose");

  AsyncBlock text{};
  Reset();
  Check(ShowTextEntry(&text) == S_OK && WaitForCall(1) && g_seen.text[0] == "Name" && g_seen.text[1] == "Pick a name" && g_seen.text[2] == "Player" &&
            g_seen.values[0] == 3 && g_seen.values[1] == 16,
        "the text entry callback gets the texts, input scope and maximum length");
  Check(TextEntryResponse(g_seen.handle, nullptr) == E_INVALIDARG, "XGameUiSetTextEntryUiResponse rejects a null string");
  Check(TextEntryResponse(g_seen.handle, "Ada") == S_OK && Wait(&text) == S_OK, "XGameUiSetTextEntryUiResponse completes the text entry");
  uint32_t size = 0;
  char entered[16] = {};
  HRESULT sizeHr = g_ui.At<HRESULT (*)(void*, AsyncBlock*, uint32_t*)>(18, "XGameUiShowTextEntryResultSize")(g_ui.self, &text, &size);
  HRESULT textHr = g_ui.At<HRESULT (*)(void*, AsyncBlock*, uint32_t, char*, uint32_t*)>(19, "XGameUiShowTextEntryResult")(g_ui.self, &text, sizeof(entered), entered, &used);
  Check(sizeHr == S_OK && size == 4 && textHr == S_OK && used == 4 && !std::strcmp(entered, "Ada"),
        "XGameUiShowTextEntryResultSize counts the terminator and Result returns the title's text");

  AsyncBlock card{};
  Reset();
  Check(g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, uint64_t)>(7, "XGameUiShowPlayerProfileCardAsync")(g_ui.self, &card, g_user, 2535400000000042ull) == S_OK &&
            WaitForCall(1) && g_seen.user == g_user && g_seen.number == 2535400000000042ull,
        "the profile card callback gets the user and the target player");
  Check(HandleOnlyResponse(46, "XGameUiSetPlayerProfileCardUiResponse", g_seen.handle) == S_OK && Wait(&card) == S_OK &&
            g_ui.At<HRESULT (*)(void*, AsyncBlock*)>(8, "XGameUiShowPlayerProfileCardResult")(g_ui.self, &card) == S_OK,
        "XGameUiSetPlayerProfileCardUiResponse completes the profile card with S_OK");

  AsyncBlock invite{};
  Reset();
  HRESULT inviteHr = g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, const char*, const char*, const char*, const char*, const char*)>(5, "XGameUiShowSendGameInviteAsync")(
      g_ui.self, &invite, g_user, "scid", "template", "session", nullptr, "context");
  Check(inviteHr == S_OK && WaitForCall(1) && g_seen.text[0] == "scid" && g_seen.text[1] == "template" && g_seen.text[2] == "session" && g_seen.null[3] &&
            g_seen.text[4] == "context",
        "the send-invite callback gets the session fields (missing invitation text stays null)");
  Check(HandleOnlyResponse(47, "XGameUiSetSendGameInviteUiResponse", g_seen.handle) == S_OK && Wait(&invite) == S_OK &&
            g_ui.At<HRESULT (*)(void*, AsyncBlock*)>(6, "XGameUiShowSendGameInviteResult")(g_ui.self, &invite) == S_OK,
        "XGameUiSetSendGameInviteUiResponse completes the invite UI with S_OK");

  AsyncBlock achievements{};
  Reset();
  Check(g_ui.At<HRESULT (*)(void*, AsyncBlock*, void*, uint32_t)>(9, "XGameUiShowAchievementsAsync")(g_ui.self, &achievements, g_user, 0x1234) == S_OK &&
            WaitForCall(1) && g_seen.number == 0x1234,
        "the achievements callback gets the title id");
  Check(HandleOnlyResponse(48, "XGameUiSetAchievementsUiResponse", g_seen.handle) == S_OK && Wait(&achievements) == S_OK &&
            g_ui.At<HRESULT (*)(void*, AsyncBlock*)>(10, "XGameUiShowAchievementsResult")(g_ui.self, &achievements) == S_OK,
        "XGameUiSetAchievementsUiResponse completes the achievements UI with S_OK");

  AsyncBlock error{};
  Reset();
  g_answer_in_callback = E_PENDING;
  Check(g_ui.At<HRESULT (*)(void*, AsyncBlock*, HRESULT, const char*)>(14, "XGameUiShowErrorDialogAsync")(g_ui.self, &error, E_OUTOFMEMORY, "store") == S_OK &&
            Wait(&error) == S_OK && WaitForCall(1) && g_answer_in_callback == S_OK && g_seen.number == static_cast<uint32_t>(E_OUTOFMEMORY) && g_seen.text[0] == "store",
        "XGameUiSetErrorDialogUiResponse can answer from inside the callback");
  Check(g_ui.At<HRESULT (*)(void*, AsyncBlock*)>(15, "XGameUiShowErrorDialogResult")(g_ui.self, &error) == S_OK, "XGameUiShowErrorDialogResult reports S_OK");

  int notAHandle = 0;
  Check(HandleOnlyResponse(49, "XGameUiSetMultiplayerActivityGameInviteUiResponse", &notAHandle) == E_INVALIDARG &&
            HandleOnlyResponse(50, "XGameUiSetErrorDialogUiResponse", nullptr) == E_INVALIDARG && MessageDialogResponse(&notAHandle, 0) == E_INVALIDARG,
        "response calls reject handles that no callback was given");
}

static void CheckPartialAndCleared(const UiCallbacks& callbacks) {
  UiCallbacks textOnly{};
  textOnly.context = callbacks.context;
  textOnly.showTextEntry = callbacks.showTextEntry;
  Check(SetCallbacks(&textOnly) == S_OK, "XGameUiSetUiCallbacks replaces every callback at once");
  AsyncBlock dialog{};
  uint32_t button = 0;
  Check(ShowMessageDialog(&dialog, "Quit?") == S_OK && Wait(&dialog) == E_ABORT && MessageDialogResult(&dialog, &button) == E_ABORT,
        "a UI whose callback is null is headless again (E_ABORT)");
  AsyncBlock text{};
  Reset();
  Check(ShowTextEntry(&text) == S_OK && WaitForCall(1) && TextEntryResponse(g_seen.handle, "") == S_OK && Wait(&text) == S_OK,
        "a UI with a callback still goes to the title");
  Check(SetCallbacks(nullptr) == S_OK, "XGameUiSetUiCallbacks(null) clears the callbacks");
  AsyncBlock headless{};
  Reset();
  Check(ShowTextEntry(&headless) == S_OK && Wait(&headless) == E_ABORT && g_calls == 0, "with no callbacks every UI completes with E_ABORT");
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
  g_threading = Get("IXThreadingImpl", kThreading);
  g_ui = Get("IXGameUiImpl", kGameUi);
  g_ui4 = Get("IXGameUiImpl4", kGameUi4);
  Api user = Get("IXUserImpl", kUser);
  AsyncBlock add{};
  user.At<HRESULT (*)(void*, uint32_t, AsyncBlock*)>(7, "XUserAddAsync")(user.self, 1, &add);
  Wait(&add);
  user.At<HRESULT (*)(void*, AsyncBlock*, void**)>(8, "XUserAddResult")(user.self, &add, &g_user);
  Check(g_user != nullptr, "XUserAddAsync gives the local user");

  UiCallbacks callbacks{};
  callbacks.context = &g_context_marker;
  callbacks.showPlayerProfileCard = OnProfileCard;
  callbacks.showPlayerPicker = OnPlayerPicker;
  callbacks.showSendGameInvite = OnSendGameInvite;
  callbacks.showAchievements = OnAchievements;
  callbacks.showMessageDialog = OnMessageDialog;
  callbacks.showErrorDialog = OnErrorDialog;
  callbacks.showTextEntry = OnTextEntry;

  CheckWebAuthentication();
  CheckMessageDialogCallback(callbacks);
  CheckOtherCallbacks();
  CheckPartialAndCleared(callbacks);
  user.At<void (*)(void*, void*)>(4, "XUserCloseHandle")(user.self, g_user);
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
