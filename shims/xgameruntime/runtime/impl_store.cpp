#define GTX_SHIM_DLL "xgameruntime.dll"
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "xgr_internal.h"
#include "xtask.h"
#include "handles.h"

namespace gtx::xgr::package {
const std::string& current_package_identifier();
const std::string& current_store_id();
}

namespace gtx::xgr::store {
namespace {

constexpr HRESULT kNetworkError = static_cast<HRESULT>(0x89245301);
constexpr HRESULT kContentNotOwned = static_cast<HRESULT>(0x803F8001);
constexpr size_t kPackageIdentifierMaxLength = 33;
constexpr size_t kSkuStoreIdLength = 18;
constexpr char kSkuId[] = "0010";

enum class CanLicenseStatus : uint32_t { NotLicensableToUser, Licensable, LicenseActionNotApplicableToProduct };

struct GameLicense {
  char skuStoreId[kSkuStoreIdLength];
  uint8_t isActive;
  uint8_t isTrialOwnedByThisUser;
  uint8_t isDiscLicense;
  uint8_t isTrial;
  uint32_t trialTimeRemainingInSeconds;
  char trialUniqueId[64];
  int64_t expirationDate;
};
static_assert(sizeof(GameLicense) == 104, "XStoreGameLicense layout");

struct AddonLicense {
  char skuStoreId[kSkuStoreIdLength];
  char inAppOfferToken[64];
  uint8_t isActive;
  int64_t expirationDate;
};
static_assert(sizeof(AddonLicense) == 96, "XStoreAddonLicense layout");

struct CanAcquireLicenseResult {
  char licensableSku[5];
  CanLicenseStatus status;
};
static_assert(sizeof(CanAcquireLicenseResult) == 12, "XStoreCanAcquireLicenseResult layout");

struct ConsumableResult {
  uint32_t quantity;
};

struct PackageUpdate {
  char packageIdentifier[kPackageIdentifierMaxLength];
  uint8_t isMandatory;
};
static_assert(sizeof(PackageUpdate) == 34, "XStorePackageUpdate layout");

struct Price {
  float basePrice;
  float price;
  float recurrencePrice;
  const char* currencyCode;
  char formattedBasePrice[16];
  char formattedPrice[16];
  char formattedRecurrencePrice[16];
  uint8_t isOnSale;
  int64_t saleEndDate;
};

struct Availability {
  const char* availabilityId;
  Price price;
  int64_t endDate;
};

struct Context {};
struct License {};

using LicenseChangedCallback = void(CALLBACK*)(void* context);
using ProductQueryCallback = uint8_t(CALLBACK*)(const void* product, void* context);

std::mutex g_lock;
std::set<const Context*> g_contexts;
std::set<const License*> g_licenses;
std::atomic<uint64_t> g_next_token{1};

bool live_context(const void* handle) {
  std::lock_guard<std::mutex> lock(g_lock);
  return handle && g_contexts.count(static_cast<const Context*>(handle)) != 0;
}

bool live_license(const void* handle) {
  std::lock_guard<std::mutex> lock(g_lock);
  return handle && g_licenses.count(static_cast<const License*>(handle)) != 0;
}

bool is_title_package(const char* packageIdentifier) {
  return packageIdentifier && package::current_package_identifier() == packageIdentifier;
}

bool is_title_store_id(const char* storeId) { return storeId && package::current_store_id() == storeId; }

struct Job {
  HRESULT result;
  std::vector<uint8_t> payload;
};

HRESULT CALLBACK job_provider(task::AsyncOp op, const task::AsyncProviderData* data) {
  Job* job = static_cast<Job*>(data->context);
  switch (op) {
    case task::AsyncOp::DoWork:
      if (FAILED(job->result)) return job->result;
      task::AsyncComplete(data->async, job->result, job->payload.size());
      return S_OK;
    case task::AsyncOp::GetResult:
      if (!job->payload.empty()) std::memcpy(data->buffer, job->payload.data(), job->payload.size());
      return S_OK;
    case task::AsyncOp::Cleanup:
      delete job;
      return S_OK;
    default:
      return S_OK;
  }
}

HRESULT begin(task::AsyncBlock* async, const void* identity, const char* name, HRESULT result, const void* payload = nullptr,
              size_t payloadSize = 0) {
  if (!async) return E_INVALIDARG;
  auto* job = new Job{result, {}};
  if (payloadSize) job->payload.assign(static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + payloadSize);
  HRESULT hr = task::AsyncBegin(async, job, identity, name, job_provider);
  if (FAILED(hr)) {
    delete job;
    return hr;
  }
  return task::AsyncSchedule(async, 0);
}

HRESULT offline(const void* context, task::AsyncBlock* async, const void* identity, const char* name) {
  if (!live_context(context) || !async) return E_INVALIDARG;
  return begin(async, identity, name, kNetworkError);
}

template <typename T> HRESULT result_into(task::AsyncBlock* async, const void* identity, T* out) {
  if (!out) return E_INVALIDARG;
  return task::AsyncGetResult(async, identity, sizeof(T), out, nullptr);
}

HRESULT result_without_payload(task::AsyncBlock* async, const void* identity) {
  return task::AsyncGetResult(async, identity, 0, nullptr, nullptr);
}

HRESULT handle_result(task::AsyncBlock* async, const void* identity, void** handle) {
  if (!handle) return E_INVALIDARG;
  *handle = nullptr;
  return task::AsyncGetResult(async, identity, sizeof(void*), handle, nullptr);
}

HRESULT count_result(task::AsyncBlock* async, size_t elementSize, uint32_t* count) {
  if (!count) return E_INVALIDARG;
  size_t size = 0;
  HRESULT hr = task::AsyncGetResultSize(async, &size);
  if (FAILED(hr)) return hr;
  *count = static_cast<uint32_t>(size / elementSize);
  return S_OK;
}

HRESULT array_result(task::AsyncBlock* async, const void* identity, uint32_t count, size_t elementSize, void* items) {
  if (count && !items) return E_INVALIDARG;
  return task::AsyncGetResult(async, identity, count * elementSize, items, nullptr);
}

HRESULT XStoreCreateContext_impl(void*, void*, Context** handle) {
  if (!handle) return E_INVALIDARG;
  auto* context = new Context();
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_contexts.insert(context);
  }
  handles::opened(context, handles::Type::StoreContext);
  *handle = context;
  return S_OK;
}

void XStoreCloseContextHandle_impl(void*, Context* handle) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_contexts.erase(handle)) return;
  }
  handles::closed(handle);
  delete handle;
}

HRESULT XStoreQueryAssociatedProductsAsync_impl(void*, Context* context, uint32_t, uint32_t, task::AsyncBlock* async) {
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsAsync_impl), "XStoreQueryAssociatedProductsAsync");
}
HRESULT XStoreQueryAssociatedProductsResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsAsync_impl), query);
}

HRESULT XStoreQueryProductsAsync_impl(void*, Context* context, uint32_t, const char** storeIds, size_t storeIdsCount, const char** actionFilters,
                                      size_t actionFiltersCount, task::AsyncBlock* async) {
  if ((storeIdsCount && !storeIds) || (actionFiltersCount && !actionFilters)) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryProductsAsync_impl), "XStoreQueryProductsAsync");
}
HRESULT XStoreQueryProductsResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryProductsAsync_impl), query);
}

HRESULT XStoreQueryEntitledProductsAsync_impl(void*, Context* context, uint32_t, uint32_t, task::AsyncBlock* async) {
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryEntitledProductsAsync_impl), "XStoreQueryEntitledProductsAsync");
}
HRESULT XStoreQueryEntitledProductsResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryEntitledProductsAsync_impl), query);
}

HRESULT XStoreQueryProductForCurrentGameAsync_impl(void*, Context* context, task::AsyncBlock* async) {
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryProductForCurrentGameAsync_impl), "XStoreQueryProductForCurrentGameAsync");
}
HRESULT XStoreQueryProductForCurrentGameResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryProductForCurrentGameAsync_impl), query);
}

HRESULT XStoreQueryProductForPackageAsync_impl(void*, Context* context, uint32_t, const char* packageIdentifier, task::AsyncBlock* async) {
  if (!packageIdentifier) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryProductForPackageAsync_impl), "XStoreQueryProductForPackageAsync");
}
HRESULT XStoreQueryProductForPackageResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryProductForPackageAsync_impl), query);
}

HRESULT XStoreQueryAssociatedProductsForStoreIdAsync_impl(void*, Context* context, const char* storeId, uint32_t, uint32_t,
                                                          task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsForStoreIdAsync_impl),
                 "XStoreQueryAssociatedProductsForStoreIdAsync");
}
HRESULT XStoreQueryAssociatedProductsForStoreIdResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreQueryAssociatedProductsForStoreIdAsync_impl), query);
}

HRESULT XStoreEnumerateProductsQuery_impl(void*, const void*, void*, ProductQueryCallback) { return E_INVALIDARG; }
uint8_t XStoreProductsQueryHasMorePages_impl(void*, const void*) { return 0; }
HRESULT XStoreProductsQueryNextPageAsync_impl(void*, const void*, task::AsyncBlock*) { return E_INVALIDARG; }
HRESULT XStoreProductsQueryNextPageResult_impl(void*, task::AsyncBlock* async, void** query) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreProductsQueryNextPageAsync_impl), query);
}
void XStoreCloseProductsQueryHandle_impl(void*, void*) {}

License* new_license() {
  auto* license = new License();
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_licenses.insert(license);
  }
  handles::opened(license, handles::Type::StoreLicense);
  return license;
}

void close_license(License* license) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_licenses.erase(license)) return;
  }
  handles::closed(license);
  delete license;
}

HRESULT XStoreAcquireLicenseForPackageAsync_impl(void*, Context* context, const char* packageIdentifier, task::AsyncBlock* async) {
  if (!live_context(context) || !packageIdentifier || !async) return E_INVALIDARG;
  const void* identity = reinterpret_cast<const void*>(&XStoreAcquireLicenseForPackageAsync_impl);
  if (!is_title_package(packageIdentifier)) return begin(async, identity, "XStoreAcquireLicenseForPackageAsync", kContentNotOwned);
  License* license = new_license();
  HRESULT hr = begin(async, identity, "XStoreAcquireLicenseForPackageAsync", S_OK, &license, sizeof(license));
  if (FAILED(hr)) close_license(license);
  return hr;
}
HRESULT XStoreAcquireLicenseForPackageResult_impl(void*, task::AsyncBlock* async, void** license) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreAcquireLicenseForPackageAsync_impl), license);
}

HRESULT XStoreAcquireLicenseForDurablesAsync_impl(void*, Context* context, const char* storeId, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreAcquireLicenseForDurablesAsync_impl), "XStoreAcquireLicenseForDurablesAsync");
}
HRESULT XStoreAcquireLicenseForDurablesResult_impl(void*, task::AsyncBlock* async, void** license) {
  return handle_result(async, reinterpret_cast<const void*>(&XStoreAcquireLicenseForDurablesAsync_impl), license);
}

uint8_t XStoreIsLicenseValid_impl(void*, const License* license) { return live_license(license) ? 1 : 0; }

void XStoreCloseLicenseHandle_impl(void*, License* license) { close_license(license); }

HRESULT begin_can_acquire(task::AsyncBlock* async, const void* identity, const char* name, bool ownProduct) {
  if (!ownProduct) return begin(async, identity, name, kNetworkError);
  CanAcquireLicenseResult result{};
  std::memcpy(result.licensableSku, kSkuId, sizeof(kSkuId));
  result.status = CanLicenseStatus::Licensable;
  return begin(async, identity, name, S_OK, &result, sizeof(result));
}

HRESULT XStoreCanAcquireLicenseForStoreIdAsync_impl(void*, Context* context, const char* storeId, task::AsyncBlock* async) {
  if (!live_context(context) || !storeId || !async) return E_INVALIDARG;
  return begin_can_acquire(async, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForStoreIdAsync_impl),
                           "XStoreCanAcquireLicenseForStoreIdAsync", is_title_store_id(storeId));
}
HRESULT XStoreCanAcquireLicenseForStoreIdResult_impl(void*, task::AsyncBlock* async, CanAcquireLicenseResult* result) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForStoreIdAsync_impl), result);
}

HRESULT XStoreCanAcquireLicenseForPackageAsync_impl(void*, Context* context, const char* packageIdentifier, task::AsyncBlock* async) {
  if (!live_context(context) || !packageIdentifier || !async) return E_INVALIDARG;
  return begin_can_acquire(async, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForPackageAsync_impl),
                           "XStoreCanAcquireLicenseForPackageAsync", is_title_package(packageIdentifier));
}
HRESULT XStoreCanAcquireLicenseForPackageResult_impl(void*, task::AsyncBlock* async, CanAcquireLicenseResult* result) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreCanAcquireLicenseForPackageAsync_impl), result);
}

HRESULT XStoreQueryGameLicenseAsync_impl(void*, Context* context, task::AsyncBlock* async) {
  if (!live_context(context) || !async) return E_INVALIDARG;
  GameLicense license{};
  std::string sku = package::current_store_id() + "/" + kSkuId;
  if (sku.size() < sizeof(license.skuStoreId)) std::memcpy(license.skuStoreId, sku.c_str(), sku.size() + 1);
  license.isActive = 1;
  return begin(async, reinterpret_cast<const void*>(&XStoreQueryGameLicenseAsync_impl), "XStoreQueryGameLicenseAsync", S_OK, &license,
               sizeof(license));
}
HRESULT XStoreQueryGameLicenseResult_impl(void*, task::AsyncBlock* async, GameLicense* license) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreQueryGameLicenseAsync_impl), license);
}

HRESULT XStoreQueryAddOnLicensesAsync_impl(void*, Context* context, task::AsyncBlock* async) {
  if (!live_context(context) || !async) return E_INVALIDARG;
  return begin(async, reinterpret_cast<const void*>(&XStoreQueryAddOnLicensesAsync_impl), "XStoreQueryAddOnLicensesAsync", S_OK);
}
HRESULT XStoreQueryAddOnLicensesResultCount_impl(void*, task::AsyncBlock* async, uint32_t* count) {
  return count_result(async, sizeof(AddonLicense), count);
}
HRESULT XStoreQueryAddOnLicensesResult_impl(void*, task::AsyncBlock* async, uint32_t count, AddonLicense* licenses) {
  return array_result(async, reinterpret_cast<const void*>(&XStoreQueryAddOnLicensesAsync_impl), count, sizeof(AddonLicense), licenses);
}

HRESULT XStoreQueryConsumableBalanceRemainingAsync_impl(void*, Context* context, const char* storeId, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryConsumableBalanceRemainingAsync_impl),
                 "XStoreQueryConsumableBalanceRemainingAsync");
}
HRESULT XStoreQueryConsumableBalanceRemainingResult_impl(void*, task::AsyncBlock* async, ConsumableResult* result) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreQueryConsumableBalanceRemainingAsync_impl), result);
}

HRESULT XStoreReportConsumableFulfillmentAsync_impl(void*, Context* context, const char* storeId, uint32_t, GUID, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreReportConsumableFulfillmentAsync_impl),
                 "XStoreReportConsumableFulfillmentAsync");
}
HRESULT XStoreReportConsumableFulfillmentResult_impl(void*, task::AsyncBlock* async, ConsumableResult* result) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreReportConsumableFulfillmentAsync_impl), result);
}

HRESULT string_result(task::AsyncBlock* async, const void* identity, size_t size, char* text) {
  if (!text) return E_INVALIDARG;
  return task::AsyncGetResult(async, identity, size, text, nullptr);
}

HRESULT XStoreGetUserCollectionsIdAsync_impl(void*, Context* context, const char* serviceTicket, const char* publisherUserId,
                                             task::AsyncBlock* async) {
  if (!serviceTicket || !publisherUserId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreGetUserCollectionsIdAsync_impl), "XStoreGetUserCollectionsIdAsync");
}
HRESULT XStoreGetUserCollectionsIdResultSize_impl(void*, task::AsyncBlock* async, size_t* size) { return task::AsyncGetResultSize(async, size); }
HRESULT XStoreGetUserCollectionsIdResult_impl(void*, task::AsyncBlock* async, size_t size, char* text) {
  return string_result(async, reinterpret_cast<const void*>(&XStoreGetUserCollectionsIdAsync_impl), size, text);
}

HRESULT XStoreGetUserPurchaseIdAsync_impl(void*, Context* context, const char* serviceTicket, const char* publisherUserId,
                                          task::AsyncBlock* async) {
  if (!serviceTicket || !publisherUserId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreGetUserPurchaseIdAsync_impl), "XStoreGetUserPurchaseIdAsync");
}
HRESULT XStoreGetUserPurchaseIdResultSize_impl(void*, task::AsyncBlock* async, size_t* size) { return task::AsyncGetResultSize(async, size); }
HRESULT XStoreGetUserPurchaseIdResult_impl(void*, task::AsyncBlock* async, size_t size, char* text) {
  return string_result(async, reinterpret_cast<const void*>(&XStoreGetUserPurchaseIdAsync_impl), size, text);
}

HRESULT XStoreQueryLicenseTokenAsync_impl(void*, Context* context, const char** productIds, size_t productIdsCount, const char* customDeveloperString,
                                          task::AsyncBlock* async) {
  if ((productIdsCount && !productIds) || !customDeveloperString) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryLicenseTokenAsync_impl), "XStoreQueryLicenseTokenAsync");
}
HRESULT XStoreQueryLicenseTokenResultSize_impl(void*, task::AsyncBlock* async, size_t* size) { return task::AsyncGetResultSize(async, size); }
HRESULT XStoreQueryLicenseTokenResult_impl(void*, task::AsyncBlock* async, size_t size, char* text) {
  return string_result(async, reinterpret_cast<const void*>(&XStoreQueryLicenseTokenAsync_impl), size, text);
}

HRESULT XStoreShowPurchaseUIAsync_impl(void*, Context* context, const char* storeId, const char*, const char*, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowPurchaseUIAsync_impl), "XStoreShowPurchaseUIAsync");
}
HRESULT XStoreShowPurchaseUIResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreShowPurchaseUIAsync_impl));
}

HRESULT XStoreShowRateAndReviewUIAsync_impl(void*, Context* context, task::AsyncBlock* async) {
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowRateAndReviewUIAsync_impl), "XStoreShowRateAndReviewUIAsync");
}
HRESULT XStoreShowRateAndReviewUIResult_impl(void*, task::AsyncBlock* async, uint8_t* wasUpdated) {
  return result_into(async, reinterpret_cast<const void*>(&XStoreShowRateAndReviewUIAsync_impl), wasUpdated);
}

HRESULT XStoreShowRedeemTokenUIAsync_impl(void*, Context* context, const char* token, const char** allowedStoreIds, size_t allowedStoreIdsCount,
                                          uint8_t, task::AsyncBlock* async) {
  if (!token || (allowedStoreIdsCount && !allowedStoreIds)) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowRedeemTokenUIAsync_impl), "XStoreShowRedeemTokenUIAsync");
}
HRESULT XStoreShowRedeemTokenUIResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreShowRedeemTokenUIAsync_impl));
}

HRESULT XStoreShowAssociatedProductsUIAsync_impl(void*, Context* context, const char* storeId, uint32_t, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowAssociatedProductsUIAsync_impl), "XStoreShowAssociatedProductsUIAsync");
}
HRESULT XStoreShowAssociatedProductsUIResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreShowAssociatedProductsUIAsync_impl));
}

HRESULT XStoreShowProductPageUIAsync_impl(void*, Context* context, const char* storeId, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowProductPageUIAsync_impl), "XStoreShowProductPageUIAsync");
}
HRESULT XStoreShowProductPageUIResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreShowProductPageUIAsync_impl));
}

HRESULT XStoreShowGiftingUIAsync_impl(void*, Context* context, const char* storeId, const char*, const char*, task::AsyncBlock* async) {
  if (!storeId) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreShowGiftingUIAsync_impl), "XStoreShowGiftingUIAsync");
}
HRESULT XStoreShowGiftingUIResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreShowGiftingUIAsync_impl));
}

HRESULT XStoreQueryGameAndDlcPackageUpdatesAsync_impl(void*, Context* context, task::AsyncBlock* async) {
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryGameAndDlcPackageUpdatesAsync_impl),
                 "XStoreQueryGameAndDlcPackageUpdatesAsync");
}
HRESULT XStoreQueryGameAndDlcPackageUpdatesResultCount_impl(void*, task::AsyncBlock* async, uint32_t* count) {
  return count_result(async, sizeof(PackageUpdate), count);
}
HRESULT XStoreQueryGameAndDlcPackageUpdatesResult_impl(void*, task::AsyncBlock* async, uint32_t count, PackageUpdate* updates) {
  return array_result(async, reinterpret_cast<const void*>(&XStoreQueryGameAndDlcPackageUpdatesAsync_impl), count, sizeof(PackageUpdate),
                      updates);
}

HRESULT XStoreQueryPackageUpdatesAsync_impl(void*, Context* context, const char** packageIdentifiers, size_t packageIdentifiersCount,
                                            task::AsyncBlock* async) {
  if (packageIdentifiersCount && !packageIdentifiers) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreQueryPackageUpdatesAsync_impl), "XStoreQueryPackageUpdatesAsync");
}
HRESULT XStoreQueryPackageUpdatesResultCount_impl(void*, task::AsyncBlock* async, uint32_t* count) {
  return count_result(async, sizeof(PackageUpdate), count);
}
HRESULT XStoreQueryPackageUpdatesResult_impl(void*, task::AsyncBlock* async, uint32_t count, PackageUpdate* updates) {
  return array_result(async, reinterpret_cast<const void*>(&XStoreQueryPackageUpdatesAsync_impl), count, sizeof(PackageUpdate), updates);
}

HRESULT XStoreDownloadPackageUpdatesAsync_impl(void*, Context* context, const char** packageIdentifiers, size_t packageIdentifiersCount,
                                               task::AsyncBlock* async) {
  if (packageIdentifiersCount && !packageIdentifiers) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreDownloadPackageUpdatesAsync_impl), "XStoreDownloadPackageUpdatesAsync");
}
HRESULT XStoreDownloadPackageUpdatesResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreDownloadPackageUpdatesAsync_impl));
}

HRESULT XStoreDownloadAndInstallPackageUpdatesAsync_impl(void*, Context* context, const char** packageIdentifiers, size_t packageIdentifiersCount,
                                                         task::AsyncBlock* async) {
  if (packageIdentifiersCount && !packageIdentifiers) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreDownloadAndInstallPackageUpdatesAsync_impl),
                 "XStoreDownloadAndInstallPackageUpdatesAsync");
}
HRESULT XStoreDownloadAndInstallPackageUpdatesResult_impl(void*, task::AsyncBlock* async) {
  return result_without_payload(async, reinterpret_cast<const void*>(&XStoreDownloadAndInstallPackageUpdatesAsync_impl));
}

HRESULT XStoreDownloadAndInstallPackagesAsync_impl(void*, Context* context, const char** storeIds, size_t storeIdsCount, task::AsyncBlock* async) {
  if (storeIdsCount && !storeIds) return E_INVALIDARG;
  return offline(context, async, reinterpret_cast<const void*>(&XStoreDownloadAndInstallPackagesAsync_impl), "XStoreDownloadAndInstallPackagesAsync");
}
HRESULT XStoreDownloadAndInstallPackagesResultCount_impl(void*, task::AsyncBlock* async, uint32_t* count) {
  return count_result(async, kPackageIdentifierMaxLength, count);
}
HRESULT XStoreDownloadAndInstallPackagesResult_impl(void*, task::AsyncBlock* async, uint32_t count, char** packageIdentifiers) {
  if (count && !packageIdentifiers) return E_INVALIDARG;
  return task::AsyncGetResult(async, reinterpret_cast<const void*>(&XStoreDownloadAndInstallPackagesAsync_impl), 0, nullptr, nullptr);
}

HRESULT XStoreQueryPackageIdentifier_impl(void*, const char* storeId, size_t size, char* packageIdentifier) {
  if (!storeId || !packageIdentifier || size < kPackageIdentifierMaxLength || !is_title_store_id(storeId)) return E_INVALIDARG;
  const std::string& id = package::current_package_identifier();
  if (id.size() >= size) return E_INVALIDARG;
  std::memcpy(packageIdentifier, id.c_str(), id.size() + 1);
  return S_OK;
}

HRESULT register_silent(bool liveHandle, const void* callback, task::RegistrationToken* token) {
  if (!liveHandle || !callback || !token) return E_INVALIDARG;
  token->token = g_next_token++;
  return S_OK;
}

HRESULT XStoreRegisterGameLicenseChanged_impl(void*, Context* context, task::Queue*, void*, LicenseChangedCallback callback,
                                              task::RegistrationToken* token) {
  return register_silent(live_context(context), reinterpret_cast<const void*>(callback), token);
}
uint8_t XStoreUnregisterGameLicenseChanged_impl(void*, Context*, task::RegistrationToken, uint8_t) { return 1; }

HRESULT XStoreRegisterPackageLicenseLost_impl(void*, License* license, task::Queue*, void*, LicenseChangedCallback callback,
                                              task::RegistrationToken* token) {
  return register_silent(live_license(license), reinterpret_cast<const void*>(callback), token);
}
uint8_t XStoreUnregisterPackageLicenseLost_impl(void*, License*, task::RegistrationToken, uint8_t) { return 1; }

uint8_t XStoreIsAvailabilityPurchasable_impl(void*, Availability) { return 0; }

const Override kOverrides[] = {
    {"XStoreCreateContext", reinterpret_cast<void*>(&XStoreCreateContext_impl)},
    {"XStoreCloseContextHandle", reinterpret_cast<void*>(&XStoreCloseContextHandle_impl)},
    {"XStoreQueryAssociatedProductsAsync", reinterpret_cast<void*>(&XStoreQueryAssociatedProductsAsync_impl)},
    {"XStoreQueryAssociatedProductsResult", reinterpret_cast<void*>(&XStoreQueryAssociatedProductsResult_impl)},
    {"XStoreQueryProductsAsync", reinterpret_cast<void*>(&XStoreQueryProductsAsync_impl)},
    {"XStoreQueryProductsResult", reinterpret_cast<void*>(&XStoreQueryProductsResult_impl)},
    {"XStoreQueryEntitledProductsAsync", reinterpret_cast<void*>(&XStoreQueryEntitledProductsAsync_impl)},
    {"XStoreQueryEntitledProductsResult", reinterpret_cast<void*>(&XStoreQueryEntitledProductsResult_impl)},
    {"XStoreQueryProductForCurrentGameAsync", reinterpret_cast<void*>(&XStoreQueryProductForCurrentGameAsync_impl)},
    {"XStoreQueryProductForCurrentGameResult", reinterpret_cast<void*>(&XStoreQueryProductForCurrentGameResult_impl)},
    {"XStoreQueryProductForPackageAsync", reinterpret_cast<void*>(&XStoreQueryProductForPackageAsync_impl)},
    {"XStoreQueryProductForPackageResult", reinterpret_cast<void*>(&XStoreQueryProductForPackageResult_impl)},
    {"XStoreEnumerateProductsQuery", reinterpret_cast<void*>(&XStoreEnumerateProductsQuery_impl)},
    {"XStoreProductsQueryHasMorePages", reinterpret_cast<void*>(&XStoreProductsQueryHasMorePages_impl)},
    {"XStoreProductsQueryNextPageAsync", reinterpret_cast<void*>(&XStoreProductsQueryNextPageAsync_impl)},
    {"XStoreProductsQueryNextPageResult", reinterpret_cast<void*>(&XStoreProductsQueryNextPageResult_impl)},
    {"XStoreCloseProductsQueryHandle", reinterpret_cast<void*>(&XStoreCloseProductsQueryHandle_impl)},
    {"XStoreAcquireLicenseForPackageAsync", reinterpret_cast<void*>(&XStoreAcquireLicenseForPackageAsync_impl)},
    {"XStoreAcquireLicenseForPackageResult", reinterpret_cast<void*>(&XStoreAcquireLicenseForPackageResult_impl)},
    {"XStoreIsLicenseValid", reinterpret_cast<void*>(&XStoreIsLicenseValid_impl)},
    {"XStoreCloseLicenseHandle", reinterpret_cast<void*>(&XStoreCloseLicenseHandle_impl)},
    {"XStoreCanAcquireLicenseForStoreIdAsync", reinterpret_cast<void*>(&XStoreCanAcquireLicenseForStoreIdAsync_impl)},
    {"XStoreCanAcquireLicenseForStoreIdResult", reinterpret_cast<void*>(&XStoreCanAcquireLicenseForStoreIdResult_impl)},
    {"XStoreCanAcquireLicenseForPackageAsync", reinterpret_cast<void*>(&XStoreCanAcquireLicenseForPackageAsync_impl)},
    {"XStoreCanAcquireLicenseForPackageResult", reinterpret_cast<void*>(&XStoreCanAcquireLicenseForPackageResult_impl)},
    {"XStoreQueryGameLicenseAsync", reinterpret_cast<void*>(&XStoreQueryGameLicenseAsync_impl)},
    {"XStoreQueryGameLicenseResult", reinterpret_cast<void*>(&XStoreQueryGameLicenseResult_impl)},
    {"XStoreQueryAddOnLicensesAsync", reinterpret_cast<void*>(&XStoreQueryAddOnLicensesAsync_impl)},
    {"XStoreQueryAddOnLicensesResultCount", reinterpret_cast<void*>(&XStoreQueryAddOnLicensesResultCount_impl)},
    {"XStoreQueryAddOnLicensesResult", reinterpret_cast<void*>(&XStoreQueryAddOnLicensesResult_impl)},
    {"XStoreQueryConsumableBalanceRemainingAsync", reinterpret_cast<void*>(&XStoreQueryConsumableBalanceRemainingAsync_impl)},
    {"XStoreQueryConsumableBalanceRemainingResult", reinterpret_cast<void*>(&XStoreQueryConsumableBalanceRemainingResult_impl)},
    {"XStoreReportConsumableFulfillmentAsync", reinterpret_cast<void*>(&XStoreReportConsumableFulfillmentAsync_impl)},
    {"XStoreReportConsumableFulfillmentResult", reinterpret_cast<void*>(&XStoreReportConsumableFulfillmentResult_impl)},
    {"XStoreGetUserCollectionsIdAsync", reinterpret_cast<void*>(&XStoreGetUserCollectionsIdAsync_impl)},
    {"XStoreGetUserCollectionsIdResultSize", reinterpret_cast<void*>(&XStoreGetUserCollectionsIdResultSize_impl)},
    {"XStoreGetUserCollectionsIdResult", reinterpret_cast<void*>(&XStoreGetUserCollectionsIdResult_impl)},
    {"XStoreGetUserPurchaseIdAsync", reinterpret_cast<void*>(&XStoreGetUserPurchaseIdAsync_impl)},
    {"XStoreGetUserPurchaseIdResultSize", reinterpret_cast<void*>(&XStoreGetUserPurchaseIdResultSize_impl)},
    {"XStoreGetUserPurchaseIdResult", reinterpret_cast<void*>(&XStoreGetUserPurchaseIdResult_impl)},
    {"XStoreQueryLicenseTokenAsync", reinterpret_cast<void*>(&XStoreQueryLicenseTokenAsync_impl)},
    {"XStoreQueryLicenseTokenResultSize", reinterpret_cast<void*>(&XStoreQueryLicenseTokenResultSize_impl)},
    {"XStoreQueryLicenseTokenResult", reinterpret_cast<void*>(&XStoreQueryLicenseTokenResult_impl)},
    {"XStoreShowPurchaseUIAsync", reinterpret_cast<void*>(&XStoreShowPurchaseUIAsync_impl)},
    {"XStoreShowPurchaseUIResult", reinterpret_cast<void*>(&XStoreShowPurchaseUIResult_impl)},
    {"XStoreShowRateAndReviewUIAsync", reinterpret_cast<void*>(&XStoreShowRateAndReviewUIAsync_impl)},
    {"XStoreShowRateAndReviewUIResult", reinterpret_cast<void*>(&XStoreShowRateAndReviewUIResult_impl)},
    {"XStoreShowRedeemTokenUIAsync", reinterpret_cast<void*>(&XStoreShowRedeemTokenUIAsync_impl)},
    {"XStoreShowRedeemTokenUIResult", reinterpret_cast<void*>(&XStoreShowRedeemTokenUIResult_impl)},
    {"XStoreQueryGameAndDlcPackageUpdatesAsync", reinterpret_cast<void*>(&XStoreQueryGameAndDlcPackageUpdatesAsync_impl)},
    {"XStoreQueryGameAndDlcPackageUpdatesResultCount", reinterpret_cast<void*>(&XStoreQueryGameAndDlcPackageUpdatesResultCount_impl)},
    {"XStoreQueryGameAndDlcPackageUpdatesResult", reinterpret_cast<void*>(&XStoreQueryGameAndDlcPackageUpdatesResult_impl)},
    {"XStoreDownloadPackageUpdatesAsync", reinterpret_cast<void*>(&XStoreDownloadPackageUpdatesAsync_impl)},
    {"XStoreDownloadPackageUpdatesResult", reinterpret_cast<void*>(&XStoreDownloadPackageUpdatesResult_impl)},
    {"XStoreDownloadAndInstallPackageUpdatesAsync", reinterpret_cast<void*>(&XStoreDownloadAndInstallPackageUpdatesAsync_impl)},
    {"XStoreDownloadAndInstallPackageUpdatesResult", reinterpret_cast<void*>(&XStoreDownloadAndInstallPackageUpdatesResult_impl)},
    {"XStoreDownloadAndInstallPackagesAsync", reinterpret_cast<void*>(&XStoreDownloadAndInstallPackagesAsync_impl)},
    {"XStoreDownloadAndInstallPackagesResultCount", reinterpret_cast<void*>(&XStoreDownloadAndInstallPackagesResultCount_impl)},
    {"XStoreDownloadAndInstallPackagesResult", reinterpret_cast<void*>(&XStoreDownloadAndInstallPackagesResult_impl)},
    {"XStoreQueryPackageIdentifier", reinterpret_cast<void*>(&XStoreQueryPackageIdentifier_impl)},
    {"XStoreRegisterGameLicenseChanged", reinterpret_cast<void*>(&XStoreRegisterGameLicenseChanged_impl)},
    {"XStoreUnregisterGameLicenseChanged", reinterpret_cast<void*>(&XStoreUnregisterGameLicenseChanged_impl)},
    {"XStoreRegisterPackageLicenseLost", reinterpret_cast<void*>(&XStoreRegisterPackageLicenseLost_impl)},
    {"XStoreUnregisterPackageLicenseLost", reinterpret_cast<void*>(&XStoreUnregisterPackageLicenseLost_impl)},
    {"XStoreIsAvailabilityPurchasable", reinterpret_cast<void*>(&XStoreIsAvailabilityPurchasable_impl)},
    {"XStoreAcquireLicenseForDurablesAsync", reinterpret_cast<void*>(&XStoreAcquireLicenseForDurablesAsync_impl)},
    {"XStoreAcquireLicenseForDurablesResult", reinterpret_cast<void*>(&XStoreAcquireLicenseForDurablesResult_impl)},
    {"XStoreShowAssociatedProductsUIAsync", reinterpret_cast<void*>(&XStoreShowAssociatedProductsUIAsync_impl)},
    {"XStoreShowAssociatedProductsUIResult", reinterpret_cast<void*>(&XStoreShowAssociatedProductsUIResult_impl)},
    {"XStoreShowProductPageUIAsync", reinterpret_cast<void*>(&XStoreShowProductPageUIAsync_impl)},
    {"XStoreShowProductPageUIResult", reinterpret_cast<void*>(&XStoreShowProductPageUIResult_impl)},
    {"XStoreQueryAssociatedProductsForStoreIdAsync", reinterpret_cast<void*>(&XStoreQueryAssociatedProductsForStoreIdAsync_impl)},
    {"XStoreQueryAssociatedProductsForStoreIdResult", reinterpret_cast<void*>(&XStoreQueryAssociatedProductsForStoreIdResult_impl)},
    {"XStoreQueryPackageUpdatesAsync", reinterpret_cast<void*>(&XStoreQueryPackageUpdatesAsync_impl)},
    {"XStoreQueryPackageUpdatesResultCount", reinterpret_cast<void*>(&XStoreQueryPackageUpdatesResultCount_impl)},
    {"XStoreQueryPackageUpdatesResult", reinterpret_cast<void*>(&XStoreQueryPackageUpdatesResult_impl)},
    {"XStoreShowGiftingUIAsync", reinterpret_cast<void*>(&XStoreShowGiftingUIAsync_impl)},
    {"XStoreShowGiftingUIResult", reinterpret_cast<void*>(&XStoreShowGiftingUIResult_impl)},
};

}  // namespace

const Override* overrides(size_t* count) {
  *count = sizeof(kOverrides) / sizeof(kOverrides[0]);
  return kOverrides;
}

}  // namespace gtx::xgr::store
