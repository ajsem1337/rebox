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

struct GameLicense {
  char skuStoreId[18];
  uint8_t isActive, isTrialOwnedByThisUser, isDiscLicense, isTrial;
  uint32_t trialTimeRemainingInSeconds;
  char trialUniqueId[64];
  int64_t expirationDate;
};
struct AddonLicense { char skuStoreId[18]; char inAppOfferToken[64]; uint8_t isActive; int64_t expirationDate; };
struct CanAcquireLicenseResult { char licensableSku[5]; uint32_t status; };
struct ConsumableResult { uint32_t quantity; };
struct PackageUpdate { char packageIdentifier[33]; uint8_t isMandatory; };
struct Price {
  float basePrice, price, recurrencePrice;
  const char* currencyCode;
  char formattedBasePrice[16], formattedPrice[16], formattedRecurrencePrice[16];
  uint8_t isOnSale;
  int64_t saleEndDate;
};
struct Availability { const char* availabilityId; Price price; int64_t endDate; };
static_assert(sizeof(GameLicense) == 104 && sizeof(AddonLicense) == 96 && sizeof(CanAcquireLicenseResult) == 12 && sizeof(PackageUpdate) == 34,
              "XStore struct layouts");

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

constexpr HRESULT kNetworkError = static_cast<HRESULT>(0x89245301);
constexpr HRESULT kContentNotOwned = static_cast<HRESULT>(0x803F8001);

static const GUID kStore6 = {0xb09d803c, 0x2414, 0x4a05, {0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a, 0x44}};
static const GUID kThreading = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
static const GUID kPackage = {0x3720de07, 0xe8e4, 0x44a3, {0xad, 0x32, 0xb3, 0x59, 0xe8, 0xad, 0xbe, 0x55}};

static Api g_threading;
static Api g_store;
static void* g_context = nullptr;
static int g_callbacks = 0;
static void CALLBACK CountingCallback(void*) { g_callbacks++; }

static HRESULT Wait(AsyncBlock* block) {
  return g_threading.At<HRESULT (*)(void*, AsyncBlock*, BOOLEAN)>(3, "XAsyncGetStatus")(g_threading.self, block, TRUE);
}

typedef HRESULT (*ContextAsyncFn)(void*, void*, AsyncBlock*);
typedef HRESULT (*HandleResultFn)(void*, AsyncBlock*, void**);

static void CheckContext() {
  auto create = g_store.At<HRESULT (*)(void*, void*, void**)>(3, "XStoreCreateContext");
  auto close = g_store.At<void (*)(void*, void*)>(4, "XStoreCloseContextHandle");
  void* extra = nullptr;
  Check(create(g_store.self, nullptr, &g_context) == S_OK && g_context, "XStoreCreateContext without a user (allowed on PC)");
  Check(create(g_store.self, nullptr, &extra) == S_OK && extra && extra != g_context, "a second context is a distinct handle");
  close(g_store.self, extra);
  Check(create(g_store.self, nullptr, nullptr) == E_INVALIDARG, "XStoreCreateContext rejects a null output");
  AsyncBlock block{};
  auto gameLicense = g_store.At<ContextAsyncFn>(28, "XStoreQueryGameLicenseAsync");
  Check(gameLicense(g_store.self, extra, &block) == E_INVALIDARG, "a closed context is rejected");
}

static void CheckLicenses() {
  AsyncBlock block{};
  Check(g_store.At<ContextAsyncFn>(28, "XStoreQueryGameLicenseAsync")(g_store.self, g_context, &block) == S_OK && Wait(&block) == S_OK,
        "XStoreQueryGameLicenseAsync completes");
  GameLicense license;
  std::memset(&license, 0xcc, sizeof(license));
  HRESULT hr = g_store.At<HRESULT (*)(void*, AsyncBlock*, GameLicense*)>(29, "XStoreQueryGameLicenseResult")(g_store.self, &block, &license);
  Check(hr == S_OK && license.isActive == 1 && !license.isTrial && !license.isTrialOwnedByThisUser && !license.isDiscLicense &&
            license.trialUniqueId[0] == '\0' && license.expirationDate == 0,
        "the game license is active, full and not a trial");
  Check(std::strlen(license.skuStoreId) == 0 || std::strstr(license.skuStoreId, "/0010") != nullptr, "the license SKU is <store id>/0010");

  block = {};
  Check(g_store.At<ContextAsyncFn>(30, "XStoreQueryAddOnLicensesAsync")(g_store.self, g_context, &block) == S_OK && Wait(&block) == S_OK,
        "XStoreQueryAddOnLicensesAsync completes");
  uint32_t count = 99;
  Check(g_store.At<HRESULT (*)(void*, AsyncBlock*, uint32_t*)>(31, "XStoreQueryAddOnLicensesResultCount")(g_store.self, &block, &count) == S_OK &&
            count == 0,
        "no add-on licenses");
  Check(g_store.At<HRESULT (*)(void*, AsyncBlock*, uint32_t, AddonLicense*)>(32, "XStoreQueryAddOnLicensesResult")(g_store.self, &block, 0,
                                                                                                                  nullptr) == S_OK,
        "XStoreQueryAddOnLicensesResult with no licenses");
}

static void CheckPackageLicense() {
  char packageId[64] = {};
  Api package = Get(kPackage);
  HRESULT hr = package.At<HRESULT (*)(void*, size_t, char*)>(3, "XPackageGetCurrentProcessPackageIdentifier")(package.self, sizeof(packageId),
                                                                                                              packageId);
  Check(hr == S_OK && packageId[0], "the title's package identifier is known");

  auto acquire = g_store.At<HRESULT (*)(void*, void*, const char*, AsyncBlock*)>(20, "XStoreAcquireLicenseForPackageAsync");
  auto acquireResult = g_store.At<HandleResultFn>(21, "XStoreAcquireLicenseForPackageResult");
  auto valid = g_store.At<BOOLEAN (*)(void*, void*)>(22, "XStoreIsLicenseValid");
  auto closeLicense = g_store.At<void (*)(void*, void*)>(23, "XStoreCloseLicenseHandle");
  auto registerLost = g_store.At<HRESULT (*)(void*, void*, void*, void*, void*, Token*)>(68, "XStoreRegisterPackageLicenseLost");
  auto unregisterLost = g_store.At<BOOLEAN (*)(void*, void*, Token, BOOLEAN)>(69, "XStoreUnregisterPackageLicenseLost");

  AsyncBlock block{};
  void* license = nullptr;
  Check(acquire(g_store.self, g_context, packageId, &block) == S_OK && Wait(&block) == S_OK &&
            acquireResult(g_store.self, &block, &license) == S_OK && license,
        "XStoreAcquireLicenseForPackage licenses the title's own package");
  Check(valid(g_store.self, license), "the acquired license is valid");
  Token token{};
  Check(registerLost(g_store.self, license, nullptr, nullptr, reinterpret_cast<void*>(&CountingCallback), &token) == S_OK && token.value &&
            unregisterLost(g_store.self, license, token, TRUE),
        "XStoreRegisterPackageLicenseLost registers and unregisters");
  closeLicense(g_store.self, license);
  Check(!valid(g_store.self, license), "a closed license is not valid");

  block = {};
  license = reinterpret_cast<void*>(1);
  Check(acquire(g_store.self, g_context, "Other.Package", &block) == S_OK && Wait(&block) == kContentNotOwned &&
            acquireResult(g_store.self, &block, &license) == kContentNotOwned && !license,
        "another package is not owned (LM_E_CONTENT_NOT_OWNED)");

  char storeId[64] = {};
  hr = g_store.At<HRESULT (*)(void*, const char*, size_t, char*)>(65, "XStoreQueryPackageIdentifier")(g_store.self, "9NOTOURS0000", 64, storeId);
  Check(hr == E_INVALIDARG, "XStoreQueryPackageIdentifier: no installed package for another store id (documented E_INVALIDARG)");
}

static void CheckCanAcquire() {
  auto canPackage = g_store.At<HRESULT (*)(void*, void*, const char*, AsyncBlock*)>(26, "XStoreCanAcquireLicenseForPackageAsync");
  auto canPackageResult = g_store.At<HRESULT (*)(void*, AsyncBlock*, CanAcquireLicenseResult*)>(27, "XStoreCanAcquireLicenseForPackageResult");
  auto canStoreId = g_store.At<HRESULT (*)(void*, void*, const char*, AsyncBlock*)>(24, "XStoreCanAcquireLicenseForStoreIdAsync");
  auto canStoreIdResult = g_store.At<HRESULT (*)(void*, AsyncBlock*, CanAcquireLicenseResult*)>(25, "XStoreCanAcquireLicenseForStoreIdResult");
  char packageId[64] = {};
  Api package = Get(kPackage);
  package.At<HRESULT (*)(void*, size_t, char*)>(3, "XPackageGetCurrentProcessPackageIdentifier")(package.self, sizeof(packageId), packageId);

  AsyncBlock block{};
  CanAcquireLicenseResult result{};
  Check(canPackage(g_store.self, g_context, packageId, &block) == S_OK && Wait(&block) == S_OK &&
            canPackageResult(g_store.self, &block, &result) == S_OK && result.status == 1 && std::strcmp(result.licensableSku, "0010") == 0,
        "the title's own package is licensable");
  block = {};
  Check(canStoreId(g_store.self, g_context, "9NOTOURS0000", &block) == S_OK && Wait(&block) == kNetworkError &&
            canStoreIdResult(g_store.self, &block, &result) == kNetworkError,
        "another store id needs the store (E_GAMESTORE_NETWORK_ERROR)");
}

static void CheckOfflineQueries() {
  AsyncBlock block{};
  void* query = reinterpret_cast<void*>(1);
  Check(g_store.At<ContextAsyncFn>(11, "XStoreQueryProductForCurrentGameAsync")(g_store.self, g_context, &block) == S_OK &&
            Wait(&block) == kNetworkError &&
            g_store.At<HandleResultFn>(12, "XStoreQueryProductForCurrentGameResult")(g_store.self, &block, &query) == kNetworkError && !query,
        "XStoreQueryProductForCurrentGame needs the store");

  block = {};
  query = reinterpret_cast<void*>(1);
  Check(g_store.At<HRESULT (*)(void*, void*, uint32_t, uint32_t, AsyncBlock*)>(9, "XStoreQueryEntitledProductsAsync")(g_store.self, g_context, 0x02,
                                                                                                                    25, &block) == S_OK &&
            Wait(&block) == kNetworkError &&
            g_store.At<HandleResultFn>(10, "XStoreQueryEntitledProductsResult")(g_store.self, &block, &query) == kNetworkError && !query,
        "XStoreQueryEntitledProducts needs the store (documented: entitlement queries need a connection)");

  const char* ids[] = {"9NOTOURS0000"};
  block = {};
  Check(g_store.At<HRESULT (*)(void*, void*, uint32_t, const char**, size_t, const char**, size_t, AsyncBlock*)>(7, "XStoreQueryProductsAsync")(
            g_store.self, g_context, 0x02, ids, 1, nullptr, 0, &block) == S_OK &&
            Wait(&block) == kNetworkError,
        "XStoreQueryProducts needs the store");

  block = {};
  Check(g_store.At<ContextAsyncFn>(55, "XStoreQueryGameAndDlcPackageUpdatesAsync")(g_store.self, g_context, &block) == S_OK &&
            Wait(&block) == kNetworkError,
        "XStoreQueryGameAndDlcPackageUpdates needs the store");

  block = {};
  Check(g_store.At<HRESULT (*)(void*, void*, const char*, const char*, const char*, AsyncBlock*)>(49, "XStoreShowPurchaseUIAsync")(
            g_store.self, g_context, "9NOTOURS0000", nullptr, nullptr, &block) == S_OK &&
            Wait(&block) == kNetworkError &&
            g_store.At<HRESULT (*)(void*, AsyncBlock*)>(50, "XStoreShowPurchaseUIResult")(g_store.self, &block) == kNetworkError,
        "XStoreShowPurchaseUI cannot reach the store");

  block = {};
  Check(g_store.At<HRESULT (*)(void*, void*, const char*, AsyncBlock*)>(71, "XStoreAcquireLicenseForDurablesAsync")(g_store.self, g_context,
                                                                                                                  "9NOTOURS0000", &block) == S_OK &&
            Wait(&block) == kNetworkError,
        "XStoreAcquireLicenseForDurables needs the store");

  Check(!g_store.At<BOOLEAN (*)(void*, const void*)>(16, "XStoreProductsQueryHasMorePages")(g_store.self, reinterpret_cast<void*>(1)),
        "a product query handle that was never returned has no more pages");
  Check(g_store.At<HRESULT (*)(void*, const void*, void*, void*)>(15, "XStoreEnumerateProductsQuery")(g_store.self, reinterpret_cast<void*>(1),
                                                                                                    nullptr, reinterpret_cast<void*>(1)) ==
            E_INVALIDARG,
        "XStoreEnumerateProductsQuery rejects a handle that was never returned");
  Availability availability{};
  Check(!g_store.At<BOOLEAN (*)(void*, Availability)>(70, "XStoreIsAvailabilityPurchasable")(g_store.self, availability),
        "nothing is purchasable offline");
}

static void CheckLicenseChanged() {
  auto reg = g_store.At<HRESULT (*)(void*, void*, void*, void*, void*, Token*)>(66, "XStoreRegisterGameLicenseChanged");
  auto unreg = g_store.At<BOOLEAN (*)(void*, void*, Token, BOOLEAN)>(67, "XStoreUnregisterGameLicenseChanged");
  Token a{}, b{};
  Check(reg(g_store.self, g_context, nullptr, nullptr, reinterpret_cast<void*>(&CountingCallback), &a) == S_OK &&
            reg(g_store.self, g_context, nullptr, nullptr, reinterpret_cast<void*>(&CountingCallback), &b) == S_OK && a.value && a.value != b.value,
        "XStoreRegisterGameLicenseChanged registers with distinct tokens");
  Check(reg(g_store.self, g_context, nullptr, nullptr, nullptr, &a) == E_INVALIDARG, "XStoreRegisterGameLicenseChanged rejects a null callback");
  Check(unreg(g_store.self, g_context, a, TRUE) && unreg(g_store.self, g_context, b, FALSE), "XStoreUnregisterGameLicenseChanged unregisters");
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
  g_store = Get(kStore6);
  CheckContext();
  CheckLicenses();
  CheckPackageLicense();
  CheckCanAcquire();
  CheckOfflineQueries();
  CheckLicenseChanged();
  g_store.At<void (*)(void*, void*)>(4, "XStoreCloseContextHandle")(g_store.self, g_context);
  Sleep(50);
  Check(g_callbacks == 0, "no license-changed or license-lost callback ever fired");
  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
