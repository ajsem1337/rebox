// Offline local user for the xgameruntime stand-in: one always-signed-in adult local profile, no network and no UI.
// Written from the interface signatures (IXUserImpl* in the Xodus IDLs); the XAsync parts use the vendored GTX task layer (MIT, see GTX_LICENSE.txt).
// Environment: XGR_GAMERTAG, XGR_USER_ID (decimal) override the identity.
#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "gtx/log.h"
#include "xgr_internal.h"
#include "xtask.h"
#include "handles.h"

namespace gtx::xgr::user {
namespace {

struct User {
  std::atomic<long> refs{1};
  uint64_t id;
  uint64_t localId;
  std::atomic<bool> added{false};
};
struct LocalId { uint64_t value; };

User* g_user = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;

User* the_user() {
  AcquireSRWLockExclusive(&g_lock);
  if (!g_user) {
    g_user = new User;
    const char* e = std::getenv("XGR_USER_ID");
    g_user->id = e ? std::strtoull(e, nullptr, 10) : 2535400000000001ull;   // looks like an XUID, obviously synthetic
    g_user->localId = 1;
  }
  User* u = g_user;
  ReleaseSRWLockExclusive(&g_lock);
  return u;
}

User* from(void* h) { return reinterpret_cast<User*>(h); }

void hand_out(User* u) {
  u->refs++;
  handles::opened(u, handles::Type::User);
}

HRESULT XUserGetMaxUsers_impl(void*, uint32_t* n) {
  if (!n) return E_INVALIDARG;
  *n = 4;
  return S_OK;
}

// add: begin + schedule; the provider produces the handle when the result is read (GetResult), so each caller gets its own reference
HRESULT CALLBACK add_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  switch (op) {
    case task::AsyncOp::DoWork:
      task::AsyncComplete(d->async, S_OK, sizeof(void*));
      break;
    case task::AsyncOp::GetResult: {
      User* u = the_user();
      hand_out(u);
      u->added = true;
      void* h = u;
      std::memcpy(d->buffer, &h, sizeof(h));
      break;
    }
    default:
      break;
  }
  return S_OK;
}
HRESULT XUserAddAsync_impl(void*, uint32_t options, task::AsyncBlock* a) {
  (void)options;
  HRESULT hr = task::AsyncBegin(a, nullptr, reinterpret_cast<const void*>(&XUserAddAsync_impl), "XUserAddAsync", add_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(a, 0);
}
void user_handed_over();
HRESULT XUserAddResult_impl(void*, task::AsyncBlock* a, void** out) {
  HRESULT hr = task::AsyncGetResult(a, reinterpret_cast<const void*>(&XUserAddAsync_impl), sizeof(void*), out, nullptr);
  if (SUCCEEDED(hr)) user_handed_over();
  return hr;
}

HRESULT XUserDuplicateHandle_impl(void*, void* h, void** out) {
  if (!h || !out) return E_INVALIDARG;
  hand_out(from(h));
  *out = h;
  return S_OK;
}
void XUserCloseHandle_impl(void*, void* h) {
  if (!h) return;
  from(h)->refs--;
  handles::closed(h);
}
int32_t XUserCompare_impl(void*, void* a, void* b) { return a == b ? 0 : (a < b ? -1 : 1); }
HRESULT XUserGetLocalId_impl(void*, void* h, LocalId* out) {
  if (!h || !out) return E_INVALIDARG;
  out->value = from(h)->localId;
  return S_OK;
}
HRESULT XUserFindUserByLocalId_impl(void*, LocalId id, void** out) {
  if (!out) return E_INVALIDARG;
  User* u = the_user();
  if (id.value != u->localId) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  hand_out(u);
  *out = u;
  return S_OK;
}
HRESULT XUserGetId_impl(void*, void* h, uint64_t* out) {
  if (!h || !out) return E_INVALIDARG;
  *out = from(h)->id;
  return S_OK;
}
HRESULT XUserFindUserById_impl(void*, uint64_t id, void** out) {
  if (!out) return E_INVALIDARG;
  User* u = the_user();
  if (id != u->id) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  hand_out(u);
  *out = u;
  return S_OK;
}
HRESULT XUserGetIsGuest_impl(void*, void* h, uint8_t* out) {
  if (!h || !out) return E_INVALIDARG;
  *out = 0;
  return S_OK;
}
HRESULT XUserGetState_impl(void*, void* h, uint32_t* out) {
  if (!h || !out) return E_INVALIDARG;
  *out = 0;   // XUserState_SignedIn
  return S_OK;
}
HRESULT XUserGetAgeGroup_impl(void*, void* h, uint32_t* out) {
  if (!h || !out) return E_INVALIDARG;
  *out = 3;   // XUserAgeGroup_Adult
  return S_OK;
}
HRESULT XUserCheckPrivilege_impl(void*, void* h, uint32_t, uint32_t, uint8_t* has) {
  if (!h || !has) return E_INVALIDARG;
  *has = 1;   // offline profile: every privilege granted, so no title is blocked on a missing one
  return S_OK;
}
HRESULT XUserGetGamertag_impl(void*, void* h, uint32_t component, size_t size, char* out, size_t* used) {
  if (!h || !out || component > 3) return E_INVALIDARG;
  const char* e = std::getenv("XGR_GAMERTAG");
  const char* tag = e && *e ? e : "Player";
  size_t n = component == 2 ? 0 : std::strlen(tag);
  if (component == 0 && n > 15) n = 15;
  if (size < n + 1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  std::memcpy(out, tag, n);
  out[n] = 0;
  if (used) *used = n + 1;
  return S_OK;
}
uint8_t XUserIsStoreUser_impl(void*, void*) { return 1; }
uint8_t XUserIsSignOutPresent_impl(void*) { return 0; }

// change events never fire (the one user never signs out); registration succeeds so titles that wait for sign-in events proceed
HRESULT XUserRegisterForChangeEvent_impl(void*, void*, void*, void* callback, task::RegistrationToken* tok) {
  static std::atomic<uint64_t> next{1};
  if (!callback || !tok) return E_INVALIDARG;
  tok->token = next++;
  return S_OK;
}
uint8_t XUserUnregisterForChangeEvent_impl(void*, task::RegistrationToken, uint8_t) { return 1; }

constexpr HRESULT kDeferralNotAvailable = static_cast<HRESULT>(0x89245103);
HRESULT XUserGetSignOutDeferral_impl(void*, void** deferral) {
  if (!deferral) return E_INVALIDARG;
  *deferral = nullptr;
  return kDeferralNotAvailable;
}
void XUserCloseSignOutDeferralHandle_impl(void*, void*) {}

struct AppLocalDeviceId { uint8_t value[32]; };
constexpr HRESULT kUserNotFound = static_cast<HRESULT>(0x89245104);

HRESULT XUserFindForDevice_impl(void*, const AppLocalDeviceId* device, void** out) {
  if (!device || !out) return E_INVALIDARG;
  User* u = the_user();
  if (!u->added) return kUserNotFound;
  hand_out(u);
  *out = u;
  return S_OK;
}

HRESULT register_never_fired(const void* callback, task::RegistrationToken* tok) {
  static std::atomic<uint64_t> next{1};
  if (!callback || !tok) return E_INVALIDARG;
  tok->token = next++;
  return S_OK;
}

// Device association: the gamepad (gameinput's device, id "rebox" 1) belongs to the local user.
// A GDK title marks a controller connected only from this callback, and some of its menu
// screens take gamepad input only from a connected controller. As in xcl (GPL-2.0-only), the change
// (that device, from no user to ours) goes to each registered callback, on its queue, once the
// title has the user (XUserAddResult): delivered earlier, a GDK title dropped it. A callback
// registered after that gets it at once.
struct DeviceAssociationChange {
  AppLocalDeviceId deviceId;
  LocalId oldUser;
  LocalId newUser;
};
using DeviceAssociationCallback = void(CALLBACK*)(void* context, const DeviceAssociationChange* change);
struct AssociationRegistration {
  task::Queue* queue;  // our own handle: the title's queue, or the process queue
  void* context;
  DeviceAssociationCallback callback;
};
struct AssociationDelivery {
  task::Queue* queue;
  uint64_t token;
  bool reconnect;  // the gamepad leaves the user first, then comes back
};
std::map<uint64_t, AssociationRegistration> g_associations;
SRWLOCK g_associations_lock = SRWLOCK_INIT;
std::atomic<bool> g_user_handed_over{false};

void CALLBACK deliver_device_association(void* p, bool canceled) {
  auto* d = static_cast<AssociationDelivery*>(p);
  AssociationRegistration r{};
  bool live = false;
  AcquireSRWLockShared(&g_associations_lock);
  auto it = g_associations.find(d->token);
  if (it != g_associations.end()) {
    r = it->second;
    live = true;
  }
  ReleaseSRWLockShared(&g_associations_lock);
  if (!canceled && live) {  // a change queued before the title unregisters is dropped
    DeviceAssociationChange change{};
    static const uint8_t id[] = {'s', 'c', 'a', 'r', 'l', 'e', 't', 't', 1};
    std::memcpy(change.deviceId.value, id, sizeof(id));
    if (d->reconnect) {
      change.oldUser.value = the_user()->localId;
      change.newUser.value = 0;
      gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, "XUserRegisterForDeviceAssociationChanged", nullptr,
                      "the gamepad leaves the local user (callback %llu)", static_cast<unsigned long long>(d->token));
      r.callback(r.context, &change);
    }
    change.oldUser.value = 0;
    change.newUser.value = the_user()->localId;
    gtx::log::write(gtx::log::Kind::Info, GTX_SHIM_DLL, "XUserRegisterForDeviceAssociationChanged", nullptr,
                    "the gamepad is associated with the local user (callback %llu)",
                    static_cast<unsigned long long>(d->token));
    r.callback(r.context, &change);
  }
  task::Close(d->queue);
  delete d;
}

// Queue the association for one registration (the caller holds no lock).
void queue_association(uint64_t token, task::Queue* queue, bool reconnect = false) {
  task::Queue* target = nullptr;
  if (FAILED(task::Duplicate(queue, &target))) return;
  auto* d = new AssociationDelivery{target, token, reconnect};
  if (FAILED(task::Submit(target, task::PortId::Completion, d, deliver_device_association))) {
    task::Close(target);
    delete d;
  }
}

// The title has the user: every registered callback hears of the gamepad, once.
// reconnect: the gamepad leaves the user first, then comes back (each callback gets both, in order, in one call).
void associate_all(bool reconnect = false) {
  std::vector<std::pair<uint64_t, task::Queue*>> due;
  AcquireSRWLockShared(&g_associations_lock);
  for (auto& [token, r] : g_associations) due.emplace_back(token, r.queue);
  ReleaseSRWLockShared(&g_associations_lock);
  for (auto& [token, queue] : due) queue_association(token, queue, reconnect);
}

// XGR_REASSOCIATE_SECONDS=N (diagnostic): the gamepad reconnects every N seconds (it leaves the
// user, then comes back), for a screen that only counts controllers that connect while it is
// open; the same association again changes nothing, so a title may ignore it.
DWORD WINAPI reassociate(void* param) {
  DWORD ms = static_cast<DWORD>(reinterpret_cast<uintptr_t>(param)) * 1000;
  for (;;) {
    Sleep(ms);
    associate_all(true);
  }
}

void user_handed_over() {
  if (g_user_handed_over.exchange(true)) return;
  associate_all();
  if (const char* e = std::getenv("XGR_REASSOCIATE_SECONDS")) {
    uintptr_t seconds = std::strtoul(e, nullptr, 10);
    if (seconds) {
      if (HANDLE t = CreateThread(nullptr, 0, reassociate, reinterpret_cast<void*>(seconds), 0, nullptr)) CloseHandle(t);
    }
  }
}

HRESULT XUserRegisterForDeviceAssociationChanged_impl(void*, task::Queue* queue, void* context, const void* callback,
                                                      task::RegistrationToken* tok) {
  HRESULT hr = register_never_fired(callback, tok);
  if (FAILED(hr)) return hr;
  task::Queue* own = nullptr;
  if (queue) {
    if (FAILED(task::Duplicate(queue, &own))) return S_OK;
  } else if (!task::GetProcessQueue(&own)) {
    return S_OK;
  }
  AcquireSRWLockExclusive(&g_associations_lock);
  g_associations[tok->token] =
      AssociationRegistration{own, context, reinterpret_cast<DeviceAssociationCallback>(const_cast<void*>(callback))};
  ReleaseSRWLockExclusive(&g_associations_lock);
  if (g_user_handed_over) queue_association(tok->token, own);
  return S_OK;
}
uint8_t XUserUnregisterForDeviceAssociationChanged_impl(void*, task::RegistrationToken tok, uint8_t) {
  task::Queue* queue = nullptr;
  AcquireSRWLockExclusive(&g_associations_lock);
  auto it = g_associations.find(tok.token);
  if (it != g_associations.end()) {
    queue = it->second.queue;
    g_associations.erase(it);
  }
  ReleaseSRWLockExclusive(&g_associations_lock);
  if (queue) task::Close(queue);
  return 1;
}

constexpr uint32_t kAudioEndpointKindCount = 2;

HRESULT XUserGetDefaultAudioEndpointUtf16_impl(void*, LocalId, uint32_t kind, size_t count, wchar_t* endpoint, size_t*) {
  if (kind >= kAudioEndpointKindCount || (count && !endpoint)) return E_INVALIDARG;
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT XUserRegisterForDefaultAudioEndpointUtf16Changed_impl(void*, void*, void*, const void* callback, task::RegistrationToken* tok) {
  return register_never_fired(callback, tok);
}
uint8_t XUserUnregisterForDefaultAudioEndpointUtf16Changed_impl(void*, task::RegistrationToken, uint8_t) { return 1; }

HRESULT CALLBACK find_controller_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  switch (op) {
    case task::AsyncOp::DoWork:
      task::AsyncComplete(d->async, S_OK, sizeof(AppLocalDeviceId));
      break;
    case task::AsyncOp::GetResult:
      std::memset(d->buffer, 0, sizeof(AppLocalDeviceId));
      break;
    default:
      break;
  }
  return S_OK;
}
HRESULT XUserFindControllerForUserWithUiAsync_impl(void*, void* h, task::AsyncBlock* a) {
  if (!h || !a) return E_INVALIDARG;
  HRESULT hr = task::AsyncBegin(a, nullptr, reinterpret_cast<const void*>(&XUserFindControllerForUserWithUiAsync_impl),
                                "XUserFindControllerForUserWithUiAsync", find_controller_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(a, 0);
}
HRESULT XUserFindControllerForUserWithUiResult_impl(void*, task::AsyncBlock* a, AppLocalDeviceId* device) {
  if (!device) return E_INVALIDARG;
  return task::AsyncGetResult(a, reinterpret_cast<const void*>(&XUserFindControllerForUserWithUiAsync_impl), sizeof(AppLocalDeviceId),
                              device, nullptr);
}

constexpr HRESULT kFailedToGetToken = static_cast<HRESULT>(0x89245111);
constexpr HRESULT kNoGamerPicture = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kSignOutNotPresent = E_NOTIMPL;
constexpr HRESULT kNoActivePrompt = E_INVALIDARG;
constexpr uint32_t kGamerPictureSizeCount = 4;
constexpr uint32_t kPrivilegeOptionsMask = 0x1;
constexpr uint32_t kTokenAndSignatureOptionsMask = 0x3;
constexpr uint32_t kMsaTokenSilentlyOptionsMask = 0x0;
constexpr uint32_t kSpopOperationResultCount = 4;

bool is_local_user(void* h) {
  AcquireSRWLockShared(&g_lock);
  bool known = h && h == g_user;
  ReleaseSRWLockShared(&g_lock);
  return known;
}

HRESULT CALLBACK complete_with_status_provider(task::AsyncOp op, const task::AsyncProviderData* d) {
  if (op == task::AsyncOp::DoWork) task::AsyncComplete(d->async, static_cast<HRESULT>(reinterpret_cast<intptr_t>(d->context)), 0);
  return S_OK;
}

HRESULT begin_completing_with(task::AsyncBlock* a, const void* identity, const char* name, HRESULT status) {
  HRESULT hr = task::AsyncBegin(a, reinterpret_cast<void*>(static_cast<intptr_t>(status)), identity, name, complete_with_status_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(a, 0);
}

HRESULT status_of(task::AsyncBlock* a, const void* identity) { return task::AsyncGetResult(a, identity, 0, nullptr, nullptr); }

HRESULT XUserGetGamerPictureAsync_impl(void*, void* h, uint32_t pictureSize, task::AsyncBlock* a) {
  if (!is_local_user(h) || pictureSize >= kGamerPictureSizeCount || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserGetGamerPictureAsync_impl), "XUserGetGamerPictureAsync", kNoGamerPicture);
}
HRESULT XUserGetGamerPictureResultSize_impl(void*, task::AsyncBlock* a, size_t* size) { return task::AsyncGetResultSize(a, size); }
HRESULT XUserGetGamerPictureResult_impl(void*, task::AsyncBlock* a, size_t size, void* buffer, size_t* used) {
  if (size && !buffer) return E_INVALIDARG;
  return task::AsyncGetResult(a, reinterpret_cast<const void*>(&XUserGetGamerPictureAsync_impl), size, buffer, used);
}

HRESULT XUserResolvePrivilegeWithUiAsync_impl(void*, void* h, uint32_t options, uint32_t, task::AsyncBlock* a) {
  if (!is_local_user(h) || (options & ~kPrivilegeOptionsMask) || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserResolvePrivilegeWithUiAsync_impl), "XUserResolvePrivilegeWithUiAsync", S_OK);
}
HRESULT XUserResolvePrivilegeWithUiResult_impl(void*, task::AsyncBlock* a) {
  return status_of(a, reinterpret_cast<const void*>(&XUserResolvePrivilegeWithUiAsync_impl));
}

template <typename Char, typename Header>
HRESULT begin_token_request(void* h, uint32_t options, const Char* method, const Char* url, size_t headerCount, const Header* headers,
                            size_t bodySize, const void* body, task::AsyncBlock* a, const void* identity, const char* name) {
  if (!is_local_user(h) || (options & ~kTokenAndSignatureOptionsMask) || !method || !url || (headerCount && !headers) ||
      (bodySize && !body) || !a)
    return E_INVALIDARG;
  return begin_completing_with(a, identity, name, kFailedToGetToken);
}

template <typename Data>
HRESULT token_result(task::AsyncBlock* a, const void* identity, size_t size, void* buffer, Data** data, size_t* used) {
  if (!data || (size && !buffer)) return E_INVALIDARG;
  *data = nullptr;
  HRESULT hr = task::AsyncGetResult(a, identity, size, buffer, used);
  if (SUCCEEDED(hr)) *data = static_cast<Data*>(buffer);
  return hr;
}

struct TokenAndSignatureHttpHeader { const char* name; const char* value; };
struct TokenAndSignatureData { size_t tokenSize; size_t signatureSize; const char* token; const char* signature; };
struct TokenAndSignatureUtf16HttpHeader { const wchar_t* name; const wchar_t* value; };
struct TokenAndSignatureUtf16Data { size_t tokenCount; size_t signatureCount; const wchar_t* token; const wchar_t* signature; };

HRESULT XUserGetTokenAndSignatureAsync_impl(void*, void* h, uint32_t options, const char* method, const char* url, size_t headerCount,
                                            const TokenAndSignatureHttpHeader* headers, size_t bodySize, const void* body, task::AsyncBlock* a) {
  return begin_token_request(h, options, method, url, headerCount, headers, bodySize, body, a,
                             reinterpret_cast<const void*>(&XUserGetTokenAndSignatureAsync_impl), "XUserGetTokenAndSignatureAsync");
}
HRESULT XUserGetTokenAndSignatureResultSize_impl(void*, task::AsyncBlock* a, size_t* size) { return task::AsyncGetResultSize(a, size); }
HRESULT XUserGetTokenAndSignatureResult_impl(void*, task::AsyncBlock* a, size_t size, void* buffer, TokenAndSignatureData** data, size_t* used) {
  return token_result(a, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureAsync_impl), size, buffer, data, used);
}

HRESULT XUserGetTokenAndSignatureUtf16Async_impl(void*, void* h, uint32_t options, const wchar_t* method, const wchar_t* url, size_t headerCount,
                                                 const TokenAndSignatureUtf16HttpHeader* headers, size_t bodySize, const void* body,
                                                 task::AsyncBlock* a) {
  return begin_token_request(h, options, method, url, headerCount, headers, bodySize, body, a,
                             reinterpret_cast<const void*>(&XUserGetTokenAndSignatureUtf16Async_impl), "XUserGetTokenAndSignatureUtf16Async");
}
HRESULT XUserGetTokenAndSignatureUtf16ResultSize_impl(void*, task::AsyncBlock* a, size_t* size) { return task::AsyncGetResultSize(a, size); }
HRESULT XUserGetTokenAndSignatureUtf16Result_impl(void*, task::AsyncBlock* a, size_t size, void* buffer, TokenAndSignatureUtf16Data** data,
                                                  size_t* used) {
  return token_result(a, reinterpret_cast<const void*>(&XUserGetTokenAndSignatureUtf16Async_impl), size, buffer, data, used);
}

HRESULT XUserResolveIssueWithUiAsync_impl(void*, void* h, const char*, task::AsyncBlock* a) {
  if (!is_local_user(h) || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserResolveIssueWithUiAsync_impl), "XUserResolveIssueWithUiAsync", E_ABORT);
}
HRESULT XUserResolveIssueWithUiResult_impl(void*, task::AsyncBlock* a) {
  return status_of(a, reinterpret_cast<const void*>(&XUserResolveIssueWithUiAsync_impl));
}
HRESULT XUserResolveIssueWithUiUtf16Async_impl(void*, void* h, const wchar_t*, task::AsyncBlock* a) {
  if (!is_local_user(h) || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserResolveIssueWithUiUtf16Async_impl), "XUserResolveIssueWithUiUtf16Async",
                               E_ABORT);
}
HRESULT XUserResolveIssueWithUiUtf16Result_impl(void*, task::AsyncBlock* a) {
  return status_of(a, reinterpret_cast<const void*>(&XUserResolveIssueWithUiUtf16Async_impl));
}

HRESULT XUserAddByIdWithUiAsync_impl(void*, uint64_t userId, task::AsyncBlock* a) {
  if (!a) return E_INVALIDARG;
  const void* identity = reinterpret_cast<const void*>(&XUserAddByIdWithUiAsync_impl);
  if (userId != the_user()->id) return begin_completing_with(a, identity, "XUserAddByIdWithUiAsync", kUserNotFound);
  HRESULT hr = task::AsyncBegin(a, nullptr, identity, "XUserAddByIdWithUiAsync", add_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(a, 0);
}
HRESULT XUserAddByIdWithUiResult_impl(void*, task::AsyncBlock* a, void** out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  return task::AsyncGetResult(a, reinterpret_cast<const void*>(&XUserAddByIdWithUiAsync_impl), sizeof(void*), out, nullptr);
}

HRESULT XUserGetMsaTokenSilentlyAsync_impl(void*, void* h, uint32_t options, const char* scope, task::AsyncBlock* a) {
  if (!is_local_user(h) || (options & ~kMsaTokenSilentlyOptionsMask) || !scope || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserGetMsaTokenSilentlyAsync_impl), "XUserGetMsaTokenSilentlyAsync",
                               kFailedToGetToken);
}
HRESULT XUserGetMsaTokenSilentlyResult_impl(void*, task::AsyncBlock* a, size_t size, char* token, size_t* used) {
  if (size && !token) return E_INVALIDARG;
  return task::AsyncGetResult(a, reinterpret_cast<const void*>(&XUserGetMsaTokenSilentlyAsync_impl), size, token, used);
}
HRESULT XUserGetMsaTokenSilentlyResultSize_impl(void*, task::AsyncBlock* a, size_t* size) { return task::AsyncGetResultSize(a, size); }

struct RemoteConnectEventHandlers { void* show; void* close; void* context; };

struct PromptHandlers {
  task::Queue* remoteConnectQueue = nullptr;
  RemoteConnectEventHandlers remoteConnect{};
  task::Queue* spopQueue = nullptr;
  void* spopHandler = nullptr;
  void* spopContext = nullptr;
};

SRWLOCK g_prompt_lock = SRWLOCK_INIT;
PromptHandlers g_prompt_handlers;

HRESULT hold_queue(task::Queue* queue, task::Queue** held) {
  *held = nullptr;
  return queue ? task::Duplicate(queue, held) : S_OK;
}

HRESULT XUserPlatformRemoteConnectSetEventHandlers_impl(void*, task::Queue* queue, const RemoteConnectEventHandlers* handlers) {
  if (!handlers) return E_INVALIDARG;
  task::Queue* held = nullptr;
  HRESULT hr = hold_queue(queue, &held);
  if (FAILED(hr)) return hr;
  AcquireSRWLockExclusive(&g_prompt_lock);
  task::Queue* previous = g_prompt_handlers.remoteConnectQueue;
  g_prompt_handlers.remoteConnectQueue = held;
  g_prompt_handlers.remoteConnect = *handlers;
  ReleaseSRWLockExclusive(&g_prompt_lock);
  if (previous) task::Close(previous);
  return S_OK;
}

HRESULT XUserPlatformRemoteConnectCancelPrompt_impl(void*, void*) { return kNoActivePrompt; }

HRESULT XUserPlatformSpopPromptSetEventHandlers_impl(void*, task::Queue* queue, void* handler, void* context) {
  if (!handler) return E_INVALIDARG;
  task::Queue* held = nullptr;
  HRESULT hr = hold_queue(queue, &held);
  if (FAILED(hr)) return hr;
  AcquireSRWLockExclusive(&g_prompt_lock);
  task::Queue* previous = g_prompt_handlers.spopQueue;
  g_prompt_handlers.spopQueue = held;
  g_prompt_handlers.spopHandler = handler;
  g_prompt_handlers.spopContext = context;
  ReleaseSRWLockExclusive(&g_prompt_lock);
  if (previous) task::Close(previous);
  return S_OK;
}

HRESULT XUserPlatformSpopPromptComplete_impl(void*, void*, uint32_t result) {
  if (result >= kSpopOperationResultCount) return E_INVALIDARG;
  return kNoActivePrompt;
}

HRESULT XUserSignOutAsync_impl(void*, void* h, task::AsyncBlock* a) {
  if (!is_local_user(h) || !a) return E_INVALIDARG;
  return begin_completing_with(a, reinterpret_cast<const void*>(&XUserSignOutAsync_impl), "XUserSignOutAsync", kSignOutNotPresent);
}
HRESULT XUserSignOutResult_impl(void*, task::AsyncBlock* a) { return status_of(a, reinterpret_cast<const void*>(&XUserSignOutAsync_impl)); }

const Override kOverrides[] = {
    {"XUserGetMaxUsers", reinterpret_cast<void*>(&XUserGetMaxUsers_impl)},
    {"XUserAddAsync", reinterpret_cast<void*>(&XUserAddAsync_impl)},
    {"XUserAddResult", reinterpret_cast<void*>(&XUserAddResult_impl)},
    {"XUserDuplicateHandle", reinterpret_cast<void*>(&XUserDuplicateHandle_impl)},
    {"XUserCloseHandle", reinterpret_cast<void*>(&XUserCloseHandle_impl)},
    {"XUserCompare", reinterpret_cast<void*>(&XUserCompare_impl)},
    {"XUserGetLocalId", reinterpret_cast<void*>(&XUserGetLocalId_impl)},
    {"XUserFindUserByLocalId", reinterpret_cast<void*>(&XUserFindUserByLocalId_impl)},
    {"XUserGetId", reinterpret_cast<void*>(&XUserGetId_impl)},
    {"XUserFindUserById", reinterpret_cast<void*>(&XUserFindUserById_impl)},
    {"XUserGetIsGuest", reinterpret_cast<void*>(&XUserGetIsGuest_impl)},
    {"XUserGetState", reinterpret_cast<void*>(&XUserGetState_impl)},
    {"XUserGetAgeGroup", reinterpret_cast<void*>(&XUserGetAgeGroup_impl)},
    {"XUserCheckPrivilege", reinterpret_cast<void*>(&XUserCheckPrivilege_impl)},
    {"XUserGetGamertag", reinterpret_cast<void*>(&XUserGetGamertag_impl)},
    {"XUserIsStoreUser", reinterpret_cast<void*>(&XUserIsStoreUser_impl)},
    {"XUserIsSignOutPresent", reinterpret_cast<void*>(&XUserIsSignOutPresent_impl)},
    {"XUserRegisterForChangeEvent", reinterpret_cast<void*>(&XUserRegisterForChangeEvent_impl)},
    {"XUserUnregisterForChangeEvent", reinterpret_cast<void*>(&XUserUnregisterForChangeEvent_impl)},
    {"XUserGetSignOutDeferral", reinterpret_cast<void*>(&XUserGetSignOutDeferral_impl)},
    {"XUserCloseSignOutDeferralHandle", reinterpret_cast<void*>(&XUserCloseSignOutDeferralHandle_impl)},
    {"XUserFindForDevice", reinterpret_cast<void*>(&XUserFindForDevice_impl)},
    {"XUserRegisterForDeviceAssociationChanged", reinterpret_cast<void*>(&XUserRegisterForDeviceAssociationChanged_impl)},
    {"XUserUnregisterForDeviceAssociationChanged", reinterpret_cast<void*>(&XUserUnregisterForDeviceAssociationChanged_impl)},
    {"XUserGetDefaultAudioEndpointUtf16", reinterpret_cast<void*>(&XUserGetDefaultAudioEndpointUtf16_impl)},
    {"XUserRegisterForDefaultAudioEndpointUtf16Changed", reinterpret_cast<void*>(&XUserRegisterForDefaultAudioEndpointUtf16Changed_impl)},
    {"XUserUnregisterForDefaultAudioEndpointUtf16Changed", reinterpret_cast<void*>(&XUserUnregisterForDefaultAudioEndpointUtf16Changed_impl)},
    {"XUserFindControllerForUserWithUiAsync", reinterpret_cast<void*>(&XUserFindControllerForUserWithUiAsync_impl)},
    {"XUserFindControllerForUserWithUiResult", reinterpret_cast<void*>(&XUserFindControllerForUserWithUiResult_impl)},
    {"XUserGetGamerPictureAsync", reinterpret_cast<void*>(&XUserGetGamerPictureAsync_impl)},
    {"XUserGetGamerPictureResultSize", reinterpret_cast<void*>(&XUserGetGamerPictureResultSize_impl)},
    {"XUserGetGamerPictureResult", reinterpret_cast<void*>(&XUserGetGamerPictureResult_impl)},
    {"XUserResolvePrivilegeWithUiAsync", reinterpret_cast<void*>(&XUserResolvePrivilegeWithUiAsync_impl)},
    {"XUserResolvePrivilegeWithUiResult", reinterpret_cast<void*>(&XUserResolvePrivilegeWithUiResult_impl)},
    {"XUserGetTokenAndSignatureAsync", reinterpret_cast<void*>(&XUserGetTokenAndSignatureAsync_impl)},
    {"XUserGetTokenAndSignatureResultSize", reinterpret_cast<void*>(&XUserGetTokenAndSignatureResultSize_impl)},
    {"XUserGetTokenAndSignatureResult", reinterpret_cast<void*>(&XUserGetTokenAndSignatureResult_impl)},
    {"XUserGetTokenAndSignatureUtf16Async", reinterpret_cast<void*>(&XUserGetTokenAndSignatureUtf16Async_impl)},
    {"XUserGetTokenAndSignatureUtf16ResultSize", reinterpret_cast<void*>(&XUserGetTokenAndSignatureUtf16ResultSize_impl)},
    {"XUserGetTokenAndSignatureUtf16Result", reinterpret_cast<void*>(&XUserGetTokenAndSignatureUtf16Result_impl)},
    {"XUserResolveIssueWithUiAsync", reinterpret_cast<void*>(&XUserResolveIssueWithUiAsync_impl)},
    {"XUserResolveIssueWithUiResult", reinterpret_cast<void*>(&XUserResolveIssueWithUiResult_impl)},
    {"XUserResolveIssueWithUiUtf16Async", reinterpret_cast<void*>(&XUserResolveIssueWithUiUtf16Async_impl)},
    {"XUserResolveIssueWithUiUtf16Result", reinterpret_cast<void*>(&XUserResolveIssueWithUiUtf16Result_impl)},
    {"XUserAddByIdWithUiAsync", reinterpret_cast<void*>(&XUserAddByIdWithUiAsync_impl)},
    {"XUserAddByIdWithUiResult", reinterpret_cast<void*>(&XUserAddByIdWithUiResult_impl)},
    {"XUserGetMsaTokenSilentlyAsync", reinterpret_cast<void*>(&XUserGetMsaTokenSilentlyAsync_impl)},
    {"XUserGetMsaTokenSilentlyResult", reinterpret_cast<void*>(&XUserGetMsaTokenSilentlyResult_impl)},
    {"XUserGetMsaTokenSilentlyResultSize", reinterpret_cast<void*>(&XUserGetMsaTokenSilentlyResultSize_impl)},
    {"XUserPlatformRemoteConnectSetEventHandlers", reinterpret_cast<void*>(&XUserPlatformRemoteConnectSetEventHandlers_impl)},
    {"XUserPlatformRemoteConnectCancelPrompt", reinterpret_cast<void*>(&XUserPlatformRemoteConnectCancelPrompt_impl)},
    {"XUserPlatformSpopPromptSetEventHandlers", reinterpret_cast<void*>(&XUserPlatformSpopPromptSetEventHandlers_impl)},
    {"XUserPlatformSpopPromptComplete", reinterpret_cast<void*>(&XUserPlatformSpopPromptComplete_impl)},
    {"XUserSignOutAsync", reinterpret_cast<void*>(&XUserSignOutAsync_impl)},
    {"XUserSignOutResult", reinterpret_cast<void*>(&XUserSignOutResult_impl)},
};

}  // namespace

uint64_t id_of(void* h) { return h ? from(h)->id : 0; }

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::user
