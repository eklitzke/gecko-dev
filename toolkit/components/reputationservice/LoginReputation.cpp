/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LoginReputation.h"
#include "nsThreadUtils.h"
#include "mozilla/ErrorNames.h"
#include "mozilla/Logging.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/HTMLInputElement.h"
#include "mozilla/ipc/URIUtils.h"

using namespace mozilla;
using namespace mozilla::dom;

#define PREF_PP_ENABLED               "browser.safebrowsing.passwords.enabled"
#define PREF_PASSWORD_ALLOW_TABLE     "urlclassifier.passwordAllowTable"

static bool sPasswordProtectionEnabled = false;

// MOZ_LOG=LoginReputation:5
LazyLogModule gLoginReputationLogModule("LoginReputation");
#define LR_LOG(args) MOZ_LOG(gLoginReputationLogModule, mozilla::LogLevel::Debug, args)
#define LR_LOG_ENABLED() MOZ_LOG_TEST(gLoginReputationLogModule, mozilla::LogLevel::Debug)

static Atomic<bool> gShuttingDown(false);

static const char* kObservedPrefs[] = {
  PREF_PASSWORD_ALLOW_TABLE,
};

// -------------------------------------------------------------------------
// ReputationQueryParam
//
// Concrete class for nsILoginReputationQuery to hold query parameters
//
class ReputationQueryParam final : public nsILoginReputationQuery
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSILOGINREPUTATIONQUERY

  explicit ReputationQueryParam(nsIURI* aURI)
    : mURI(aURI)
  {
  };

private:
  ~ReputationQueryParam() = default;

  nsCOMPtr<nsIURI> mURI;
};

NS_IMPL_ISUPPORTS(ReputationQueryParam, nsILoginReputationQuery)

NS_IMETHODIMP
ReputationQueryParam::GetFormURI(nsIURI** aURI)
{
  NS_IF_ADDREF(*aURI = mURI);
  return NS_OK;
}

// -------------------------------------------------------------------------
// LoginWhitelist
//
// This class is a wrapper that encapsulate asynchronous callback API provided
// by DBService into a MozPromise callback.
//
class LoginWhitelist final : public nsIURIClassifierCallback
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIURICLASSIFIERCALLBACK

  RefPtr<ReputationPromise> QueryLoginWhitelist(nsILoginReputationQuery* aParam);

  LoginWhitelist() = default;

  nsresult Init();
  nsresult Uninit();

  void UpdateWhitelistTables();

private:
  ~LoginWhitelist() = default;

  nsCString mTables;

  // Queries that are waiting for callback from ::AsyncClassifyLocalWithTables.
  nsTArray<UniquePtr<MozPromiseHolder<ReputationPromise>>> mQueryPromises;
};

NS_IMPL_ISUPPORTS(LoginWhitelist, nsIURIClassifierCallback)

nsresult
LoginWhitelist::Init()
{
  UpdateWhitelistTables();

  return NS_OK;
}

nsresult
LoginWhitelist::Uninit()
{
  // Reject all query promise before releasing.
  for (uint8_t i = 0; i < mQueryPromises.Length(); i++) {
    mQueryPromises[i]->Reject(NS_ERROR_ABORT, __func__);
  }
  mQueryPromises.Clear();

  return NS_OK;
}

RefPtr<ReputationPromise>
LoginWhitelist::QueryLoginWhitelist(nsILoginReputationQuery* aParam)
{
  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;
  UniquePtr<MozPromiseHolder<ReputationPromise>> holder =
    MakeUnique<MozPromiseHolder<ReputationPromise>>();
  RefPtr<ReputationPromise> p = holder->Ensure(__func__);

  // Return rejected promise while there is an error.
  auto fail = MakeScopeExit([&] () {
    holder->Reject(rv, __func__);
  });

  nsCOMPtr<nsIURI> uri;
  rv = aParam->GetFormURI(getter_AddRefs(uri));
  if (NS_WARN_IF(NS_FAILED(rv) || !uri)) {
    return p;
  }

  nsCOMPtr<nsIURIClassifier> uriClassifier =
    do_GetService(NS_URLCLASSIFIERDBSERVICE_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return p;
  }

  // AsyncClassifyLocalWithTables API won't trigger a gethash request on
  // a full-length match, so this API call should only include local operation.
  rv = uriClassifier->AsyncClassifyLocalWithTables(uri, mTables, this);
  if (NS_FAILED(rv)) {
    return p;
  }

  fail.release();
  mQueryPromises.AppendElement(Move(holder));
  return p;
}

nsresult
LoginWhitelist::OnClassifyComplete(nsresult aErrorCode,
                                   const nsACString& aLists,
                                   const nsACString& aProvider,
                                   const nsACString& aFullHash)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (gShuttingDown) {
    return NS_OK;
  }

  LR_LOG(("OnClassifyComplete : list = %s", aLists.BeginReading()));

  UniquePtr<MozPromiseHolder<ReputationPromise>> holder =
    Move(mQueryPromises.ElementAt(0));
  mQueryPromises.RemoveElementAt(0);

  if (NS_FAILED(aErrorCode)) {
    // This should not happen
    MOZ_ASSERT_UNREACHABLE("unexpected error received in OnClassifyComplete");
    holder->Reject(aErrorCode, __func__);
  } else if (aLists.IsEmpty()) {
    // Reject if we can not find url in white list.
    holder->Reject(NS_OK, __func__);
  } else {
    holder->Resolve(nsILoginReputationVerdictType::SAFE, __func__);
  }

  return NS_OK;
}

void
LoginWhitelist::UpdateWhitelistTables()
{
  Preferences::GetCString(PREF_PASSWORD_ALLOW_TABLE, mTables);
}

// -------------------------------------------------------------------------
// LoginReputationService
//
NS_IMPL_ISUPPORTS(LoginReputationService,
                  nsILoginReputationService,
                  nsIObserver)

LoginReputationService*
  LoginReputationService::gLoginReputationService = nullptr;

// static
already_AddRefed<LoginReputationService>
LoginReputationService::GetSingleton()
{
  if (!gLoginReputationService) {
    gLoginReputationService = new LoginReputationService();
  }
  return do_AddRef(gLoginReputationService);
}

LoginReputationService::LoginReputationService()
{
  LR_LOG(("Login reputation service starting up"));
}

LoginReputationService::~LoginReputationService()
{
  LR_LOG(("Login reputation service shutting down"));

  MOZ_ASSERT(gLoginReputationService == this);

  gLoginReputationService = nullptr;
}

NS_IMETHODIMP
LoginReputationService::Init()
{
  MOZ_ASSERT(NS_IsMainThread());

  Preferences::AddBoolVarCache(&sPasswordProtectionEnabled, PREF_PP_ENABLED, true);

  switch (XRE_GetProcessType()) {
  case GeckoProcessType_Default:
    LR_LOG(("Init login reputation service in parent"));
    break;
  case GeckoProcessType_Content:
    LR_LOG(("Init login reputation service in child"));
    // Login reputation service in child process will only forward request to
    // parent, return here to skip unnecessary initialization.
    return NS_OK;
  default:
    // No other process type is supported!
    return NS_ERROR_NOT_AVAILABLE;
  }

  // The initialization below only happens in parent process.
  Preferences::AddStrongObserver(this, PREF_PP_ENABLED);

  // Init should only be called once.
  MOZ_ASSERT(!mLoginWhitelist);

  mLoginWhitelist = new LoginWhitelist();

  if (sPasswordProtectionEnabled) {
    Enable();
  }

  return NS_OK;
}

nsresult
LoginReputationService::Enable()
{
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(sPasswordProtectionEnabled);

  LR_LOG(("Enable login reputation service"));

  nsresult rv = mLoginWhitelist->Init();
  Unused << NS_WARN_IF(NS_FAILED(rv));

  for (const char* pref : kObservedPrefs) {
    Preferences::AddStrongObserver(this, pref);
  }

  return NS_OK;
}

nsresult
LoginReputationService::Disable()
{
  MOZ_ASSERT(XRE_IsParentProcess());

  LR_LOG(("Disable login reputation service"));

  nsresult rv = mLoginWhitelist->Uninit();
  Unused << NS_WARN_IF(NS_FAILED(rv));

  mQueryRequests.Clear();

  nsCOMPtr<nsIPrefBranch> prefs = do_GetService(NS_PREFSERVICE_CONTRACTID);
  if (prefs) {
    for (const char* pref : kObservedPrefs) {
      prefs->RemoveObserver(pref, this);
    }
  }

  return NS_OK;
}

nsresult
LoginReputationService::Shutdown()
{
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(gShuttingDown);

  // Disable will wait until worker threads are shutdown.
  Disable();

  // Disable will only destroy worker thread, it won't null out these classes.
  // So we will null these classes in shutdown.
  mLoginWhitelist = nullptr;

  return NS_OK;
}

// static
already_AddRefed<nsILoginReputationQuery>
LoginReputationService::ConstructQueryParam(nsIURI* aURI)
{
  RefPtr<ReputationQueryParam> param = new ReputationQueryParam(aURI);
  return param.forget();
}

NS_IMETHODIMP
LoginReputationService::QueryReputationAsync(HTMLInputElement* aInput,
                                             nsILoginReputationQueryCallback* aCallback)
{
  NS_ENSURE_ARG_POINTER(aInput);

  LR_LOG(("QueryReputationAsync() [this=%p]", this));

  if (!sPasswordProtectionEnabled) {
    return NS_ERROR_FAILURE;
  }

  nsIURI* documentURI = aInput->OwnerDoc()->GetDocumentURI();
  NS_ENSURE_STATE(documentURI);

  if (XRE_IsContentProcess()) {
    using namespace mozilla::ipc;

    ContentChild* content = ContentChild::GetSingleton();
    if (content->IsShuttingDown()) {
      return NS_ERROR_FAILURE;
    }

    URIParams uri;
    SerializeURI(documentURI, uri);

    if (!content->SendPLoginReputationConstructor(uri)) {
      return NS_ERROR_FAILURE;
    }
  } else {
    nsCOMPtr<nsILoginReputationQuery> query =
      LoginReputationService::ConstructQueryParam(documentURI);

    nsresult rv = QueryReputation(query, aCallback);
    return rv;
  }

  return NS_OK;
}

NS_IMETHODIMP
LoginReputationService::QueryReputation(nsILoginReputationQuery* aQuery,
                                        nsILoginReputationQueryCallback* aCallback)
{
  MOZ_ASSERT(NS_IsMainThread());

  NS_ENSURE_ARG_POINTER(aQuery);
  NS_ENSURE_ARG_POINTER(aCallback);

  LR_LOG(("QueryReputation() [this=%p]", this));

  if (gShuttingDown || !sPasswordProtectionEnabled) {
    LR_LOG(("QueryReputation() abort [this=%p]", this));
    aCallback->OnComplete(NS_ERROR_ABORT, nsILoginReputationVerdictType::UNSPECIFIED);
    return NS_OK;
  }

  // mQueryRequests is an array used to maintain the ownership of |QueryRequest|.
  // We ensure that |QueryRequest| is always valid until Finish() is
  // called or LoginReputationService is shutdown.
  auto* request =
    mQueryRequests.AppendElement(MakeUnique<QueryRequest>(aQuery, aCallback));

  return QueryLoginWhitelist(request->get());
}

nsresult
LoginReputationService::QueryLoginWhitelist(QueryRequest* aRequest)
{
  MOZ_ASSERT(NS_IsMainThread());

  NS_ENSURE_ARG_POINTER(aRequest);

  if (gShuttingDown) {
    return NS_ERROR_ABORT;
  }

  using namespace mozilla::Telemetry;
  TimeStamp startTimeMs = TimeStamp::Now();

  RefPtr<LoginReputationService> self = this;

  mLoginWhitelist->QueryLoginWhitelist(aRequest->mParam)->Then(
    GetCurrentThreadSerialEventTarget(), __func__,
    [self, aRequest, startTimeMs](VerdictType aResolveValue) -> void {
      // Promise is resolved if url is found in google-provided whitelist.
      MOZ_ASSERT(NS_IsMainThread());
      MOZ_ASSERT(aResolveValue == nsILoginReputationVerdictType::SAFE);

      LR_LOG(("Query login whitelist [request = %p, result = SAFE]",
              aRequest));

      AccumulateTimeDelta(LOGIN_REPUTATION_LOGIN_WHITELIST_LOOKUP_TIME,
                          startTimeMs);

      Accumulate(LOGIN_REPUTATION_LOGIN_WHITELIST_RESULT,
        nsILoginReputationVerdictType::SAFE);

      self->Finish(aRequest, NS_OK, nsILoginReputationVerdictType::SAFE);
    },
    [self, aRequest, startTimeMs](nsresult rv) -> void {
      // Promise is rejected if url cannot be found in google-provided whitelist.
      // or there is an error.
      if (NS_FAILED(rv)) {
        if (LR_LOG_ENABLED()) {
          nsAutoCString errorName;
          mozilla::GetErrorName(rv, errorName);
          LR_LOG(("Error in QueryLoginWhitelist() [request = %p, rv = %s]",
                  aRequest, errorName.get()));
        }

        // Don't record the lookup time when there is an error, only record the
        // result here.
        Accumulate(LOGIN_REPUTATION_LOGIN_WHITELIST_RESULT, 2); // 2 is error
      } else {
        AccumulateTimeDelta(LOGIN_REPUTATION_LOGIN_WHITELIST_LOOKUP_TIME,
                            startTimeMs);

        Accumulate(LOGIN_REPUTATION_LOGIN_WHITELIST_RESULT,
          nsILoginReputationVerdictType::UNSPECIFIED);

        LR_LOG(("Query login whitelist cannot find the URL [request = %p]",
                aRequest));
      }

      // Check trust-based whitelisting if we can't find the url in login whitelist
      self->Finish(aRequest, rv, nsILoginReputationVerdictType::UNSPECIFIED);
    });

  return NS_OK;
}

nsresult
LoginReputationService::Finish(const QueryRequest* aRequest,
                               nsresult aStatus,
                               VerdictType aVerdict)
{
  MOZ_ASSERT(NS_IsMainThread());

  NS_ENSURE_ARG_POINTER(aRequest);

  LR_LOG(("Query login reputation end [request = %p, result = %s]",
          aRequest, VerdictTypeToString(aVerdict).get()));

  // Since we are shutting down, don't bother call back to child process.
  if (gShuttingDown) {
    return NS_OK;
  }

  aRequest->mCallback->OnComplete(aStatus, aVerdict);

  // QueryRequest may not follow the same order when we queued it in ::QueryReputation
  // because one query request may be finished earlier than the other.
  uint32_t idx = 0;
  for (; idx < mQueryRequests.Length(); idx++) {
    if (mQueryRequests[idx].get() == aRequest) {
      break;
    }
  }

  if (NS_WARN_IF(idx >= mQueryRequests.Length())) {
    return NS_ERROR_FAILURE;
  }
  mQueryRequests.RemoveElementAt(idx);

  return NS_OK;
}


NS_IMETHODIMP
LoginReputationService::Observe(nsISupports *aSubject,
                                const char *aTopic,
                                const char16_t *aData)
{
  if (!strcmp(aTopic, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID)) {
    nsDependentString data(aData);

    if (data.EqualsLiteral(PREF_PP_ENABLED)) {
      nsresult rv = sPasswordProtectionEnabled ? Enable() : Disable();
      Unused << NS_WARN_IF(NS_FAILED(rv));
    } else if (data.EqualsLiteral(PREF_PASSWORD_ALLOW_TABLE)) {
      mLoginWhitelist->UpdateWhitelistTables();
    }
  } else if (!strcmp(aTopic, "quit-application")) {
    // Prepare to shutdown, won't allow any query request after 'gShuttingDown'
    // is set.
    gShuttingDown = true;
  } else if (!strcmp(aTopic, "profile-before-change")) {
    gShuttingDown = true;
    Shutdown();
  } else {
    return NS_ERROR_UNEXPECTED;
  }

  return NS_OK;
}

// static
nsCString
LoginReputationService::VerdictTypeToString(VerdictType aVerdict)
{
  switch(aVerdict) {
    case nsILoginReputationVerdictType::UNSPECIFIED:
      return nsCString("Unspecified");
    case nsILoginReputationVerdictType::LOW_REPUTATION:
      return nsCString("Low Reputation");
    case nsILoginReputationVerdictType::SAFE:
      return nsCString("Safe");
    case nsILoginReputationVerdictType::PHISHING:
      return nsCString("Phishing");
    default:
      return nsCString("Invalid");
  }
}
