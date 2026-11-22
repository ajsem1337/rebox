#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "gtx/log.h"
#include "xgr_internal.h"
#include "xtask.h"

namespace gtx::xgr::network {
namespace {

constexpr uint16_t kDefaultPreferredLocalUdpMultiplayerPort = 3074;

HRESULT XNetworkingQueryPreferredLocalUdpMultiplayerPort_impl(void*, uint16_t* port) {
  if (!port) return E_INVALIDARG;
  *port = kDefaultPreferredLocalUdpMultiplayerPort;
  return S_OK;
}

HRESULT CALLBACK preferred_port_provider(task::AsyncOp op, const task::AsyncProviderData* data) {
  switch (op) {
    case task::AsyncOp::DoWork:
      task::AsyncComplete(data->async, S_OK, sizeof(uint16_t));
      break;
    case task::AsyncOp::GetResult:
      std::memcpy(data->buffer, &kDefaultPreferredLocalUdpMultiplayerPort, sizeof(uint16_t));
      break;
    default:
      break;
  }
  return S_OK;
}

HRESULT XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync_impl(void*, task::AsyncBlock* async) {
  if (!async) return E_INVALIDARG;
  HRESULT hr = task::AsyncBegin(async, nullptr, reinterpret_cast<const void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync_impl),
                                "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", preferred_port_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(async, 0);
}

HRESULT XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult_impl(void*, task::AsyncBlock* async, uint16_t* port) {
  if (!port) return E_INVALIDARG;
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync_impl),
                              sizeof(uint16_t), port, nullptr);
}

using PreferredPortChangedCallback = void(CALLBACK*)(void* context, uint16_t preferredLocalUdpMultiplayerPort);

struct PortRegistration {
  void* context;
  PreferredPortChangedCallback callback;
  task::Queue* queue;
  bool active;
  bool running;
  uint64_t token;
};

std::atomic<uint64_t> g_next_port_token{1};
std::mutex g_port_lock;
std::condition_variable g_port_idle;
std::unordered_map<uint64_t, std::shared_ptr<PortRegistration>> g_port_registrations;

void CALLBACK deliver_initial_preferred_port(void* holder, bool canceled) {
  auto* registration = static_cast<std::shared_ptr<PortRegistration>*>(holder);
  PortRegistration& r = **registration;
  bool deliver;
  {
    std::lock_guard<std::mutex> lock(g_port_lock);
    deliver = r.active && !canceled;
    r.running = deliver;
  }
  if (deliver) {
    r.callback(r.context, kDefaultPreferredLocalUdpMultiplayerPort);
    std::lock_guard<std::mutex> lock(g_port_lock);
    r.running = false;
    if (!r.active) g_port_registrations.erase(r.token);
    g_port_idle.notify_all();
  }
  task::Close(r.queue);
  delete registration;
}

HRESULT XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged_impl(void*, task::Queue* queue, void* context,
                                                                        PreferredPortChangedCallback callback, task::RegistrationToken* token) {
  if (!callback || !token) return E_INVALIDARG;
  task::Queue* target = nullptr;
  if (queue) {
    HRESULT hr = task::Duplicate(queue, &target);
    if (FAILED(hr)) return hr;
  } else if (!task::GetProcessQueue(&target)) {
    return E_INVALIDARG;
  }
  uint64_t value = g_next_port_token++;
  auto registration = std::make_shared<PortRegistration>(PortRegistration{context, callback, target, true, false, value});
  {
    std::lock_guard<std::mutex> lock(g_port_lock);
    g_port_registrations.emplace(value, registration);
  }
  auto* holder = new std::shared_ptr<PortRegistration>(registration);
  HRESULT hr = task::Submit(target, task::PortId::Completion, holder, deliver_initial_preferred_port);
  if (FAILED(hr)) {
    delete holder;
    task::Close(target);
    std::lock_guard<std::mutex> lock(g_port_lock);
    g_port_registrations.erase(value);
    return hr;
  }
  token->token = value;
  return S_OK;
}

uint8_t XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged_impl(void*, task::RegistrationToken token, uint8_t wait) {
  std::unique_lock<std::mutex> lock(g_port_lock);
  auto found = g_port_registrations.find(token.token);
  if (found == g_port_registrations.end()) return 1;
  std::shared_ptr<PortRegistration> registration = found->second;
  registration->active = false;
  if (!registration->running) {
    g_port_registrations.erase(found);
    return 1;
  }
  if (!wait) return 0;
  g_port_idle.wait(lock, [&] { return !registration->running; });
  return 1;
}

constexpr uint32_t kThumbprintTypeCount = 3;

struct Thumbprint {
  uint32_t thumbprintType;
  size_t thumbprintBufferByteCount;
  uint8_t* thumbprintBuffer;
};

struct SecurityInformation {
  uint32_t enabledHttpSecurityProtocolFlags;
  size_t thumbprintCount;
  Thumbprint* thumbprints;
};
static_assert(sizeof(SecurityInformation) == 24, "XNetworkingSecurityInformation layout");
static_assert(sizeof(Thumbprint) == 24, "XNetworkingThumbprint layout");

const HRESULT kTitleNetworkSecurityListUnreachable = HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);

bool is_host_character(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

bool is_well_formed_https_url(const char* url) {
  static const char kScheme[] = "https://";
  constexpr size_t kSchemeLength = sizeof(kScheme) - 1;
  if (_strnicmp(url, kScheme, kSchemeLength) != 0) return false;
  for (const char* p = url; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c <= 0x20 || c == 0x7f || c == '\\') return false;
  }
  const char* host = url + kSchemeLength;
  const char* end = host;
  if (*end == '[') {
    ++end;
    while (*end && *end != ']') {
      char c = *end;
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex && c != ':' && c != '.') return false;
      ++end;
    }
    if (*end != ']' || end == host + 1) return false;
    ++end;
  } else {
    while (is_host_character(*end)) ++end;
    if (end == host || *host == '.' || *host == '-') return false;
    for (const char* p = host; p + 1 < end; ++p)
      if (p[0] == '.' && p[1] == '.') return false;
  }
  if (*end == ':') {
    const char* digits = ++end;
    uint32_t port = 0;
    while (*end >= '0' && *end <= '9') {
      port = port * 10 + static_cast<uint32_t>(*end - '0');
      if (port > 65535) return false;
      ++end;
    }
    if (end == digits || port == 0) return false;
  }
  return *end == '\0' || *end == '/' || *end == '?' || *end == '#';
}

HRESULT CALLBACK offline_security_information_provider(task::AsyncOp op, const task::AsyncProviderData*) {
  return op == task::AsyncOp::DoWork ? kTitleNetworkSecurityListUnreachable : S_OK;
}

HRESULT begin_security_information_query(task::AsyncBlock* async, const void* identity, const char* name) {
  HRESULT hr = task::AsyncBegin(async, nullptr, identity, name, offline_security_information_provider);
  if (FAILED(hr)) return hr;
  return task::AsyncSchedule(async, 0);
}

HRESULT security_information_result(task::AsyncBlock* async, const void* identity, size_t size, size_t* used, uint8_t* buffer,
                                    SecurityInformation** information) {
  if (!buffer || !information) return E_INVALIDARG;
  *information = nullptr;
  HRESULT hr = task::AsyncGetResult(async, identity, size, buffer, used);
  if (SUCCEEDED(hr)) *information = reinterpret_cast<SecurityInformation*>(buffer);
  return hr;
}

HRESULT XNetworkingQuerySecurityInformationForUrlAsync_impl(void*, const char* url, task::AsyncBlock* async) {
  if (!url || !async || !is_well_formed_https_url(url)) return E_INVALIDARG;
  return begin_security_information_query(async, reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlAsync_impl),
                                          "XNetworkingQuerySecurityInformationForUrlAsync");
}

HRESULT XNetworkingQuerySecurityInformationForUrlAsyncResultSize_impl(void*, task::AsyncBlock* async, size_t* size) {
  return task::AsyncGetResultSize(async, size);
}

HRESULT XNetworkingQuerySecurityInformationForUrlAsyncResult_impl(void*, task::AsyncBlock* async, size_t size, size_t* used, uint8_t* buffer,
                                                                  SecurityInformation** information) {
  return security_information_result(async, reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlAsync_impl), size, used,
                                     buffer, information);
}

bool utf16_to_utf8(const wchar_t* text, std::string* out) {
  int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 0) return false;
  out->resize(static_cast<size_t>(length));
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out->data(), length, nullptr, nullptr) != length) return false;
  out->pop_back();
  return true;
}

HRESULT XNetworkingQuerySecurityInformationForUrlUtf16Async_impl(void*, const wchar_t* url, task::AsyncBlock* async) {
  if (!url || !async) return E_INVALIDARG;
  std::string narrow;
  if (!utf16_to_utf8(url, &narrow) || !is_well_formed_https_url(narrow.c_str())) return E_INVALIDARG;
  return begin_security_information_query(async, reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlUtf16Async_impl),
                                          "XNetworkingQuerySecurityInformationForUrlUtf16Async");
}

HRESULT XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize_impl(void*, task::AsyncBlock* async, size_t* size) {
  return task::AsyncGetResultSize(async, size);
}

HRESULT XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult_impl(void*, task::AsyncBlock* async, size_t size, size_t* used,
                                                                       uint8_t* buffer, SecurityInformation** information) {
  return security_information_result(async, reinterpret_cast<const void*>(&XNetworkingQuerySecurityInformationForUrlUtf16Async_impl), size,
                                     used, buffer, information);
}

bool is_well_formed_security_information(const SecurityInformation* information) {
  if (information->thumbprintCount && !information->thumbprints) return false;
  for (size_t i = 0; i < information->thumbprintCount; i++) {
    const Thumbprint& thumbprint = information->thumbprints[i];
    if (thumbprint.thumbprintType >= kThumbprintTypeCount) return false;
    if (!thumbprint.thumbprintBufferByteCount || !thumbprint.thumbprintBuffer) return false;
  }
  return true;
}

HRESULT XNetworkingVerifyServerCertificate_impl(void*, void* requestHandle, const SecurityInformation* information) {
  if (!requestHandle || !information || !is_well_formed_security_information(information)) return E_INVALIDARG;
  return E_NOTIMPL;
}

const Override kOverrides[] = {
    {"XNetworkingQueryPreferredLocalUdpMultiplayerPort", reinterpret_cast<void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPort_impl)},
    {"XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", reinterpret_cast<void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync_impl)},
    {"XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", reinterpret_cast<void*>(&XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult_impl)},
    {"XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", reinterpret_cast<void*>(&XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged_impl)},
    {"XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged", reinterpret_cast<void*>(&XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged_impl)},
    {"XNetworkingQuerySecurityInformationForUrlAsync", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlAsync_impl)},
    {"XNetworkingQuerySecurityInformationForUrlAsyncResultSize", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlAsyncResultSize_impl)},
    {"XNetworkingQuerySecurityInformationForUrlAsyncResult", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlAsyncResult_impl)},
    {"XNetworkingQuerySecurityInformationForUrlUtf16Async", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlUtf16Async_impl)},
    {"XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize_impl)},
    {"XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", reinterpret_cast<void*>(&XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult_impl)},
    {"XNetworkingVerifyServerCertificate", reinterpret_cast<void*>(&XNetworkingVerifyServerCertificate_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::network
