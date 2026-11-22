#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include "xgr_internal.h"

namespace gtx::xgr::misc {
namespace {

HRESULT XGameEventWrite_impl(void*, void* user, const char* serviceConfigId, const char* playSessionId, const char* eventName,
                             const char* dimensionsJson, const char* measurementsJson) {
  (void)dimensionsJson;
  (void)measurementsJson;
  if (!user || !serviceConfigId || !playSessionId || !eventName) return E_INVALIDARG;
  return S_OK;
}

HRESULT accept_pending_invite(const char* inviteUri) {
  if (!inviteUri || !*inviteUri) return E_INVALIDARG;
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT XGameActivationAcceptPendingInvite_impl(void*, const char* inviteUri) { return accept_pending_invite(inviteUri); }
HRESULT XGameInviteAcceptPendingInvite_impl(void*, const char* inviteUri) { return accept_pending_invite(inviteUri); }

const Override kOverrides[] = {
    {"XGameEventWrite", reinterpret_cast<void*>(&XGameEventWrite_impl)},
    {"XGameActivationAcceptPendingInvite", reinterpret_cast<void*>(&XGameActivationAcceptPendingInvite_impl)},
    {"XGameInviteAcceptPendingInvite", reinterpret_cast<void*>(&XGameInviteAcceptPendingInvite_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::misc
