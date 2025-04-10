/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim:set ts=4 sw=4 sts=4 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "nsNetUtil.h"

#include "mozilla/Atomics.h"
#include "mozilla/Encoding.h"
#include "mozilla/LoadContext.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/Monitor.h"
#include "mozilla/TaskQueue.h"
#include "mozilla/Telemetry.h"
#include "nsCategoryCache.h"
#include "nsContentUtils.h"
#include "nsHashKeys.h"
#include "nsHttp.h"
#include "nsIAsyncStreamCopier.h"
#include "nsIAuthPrompt.h"
#include "nsIAuthPrompt2.h"
#include "nsIAuthPromptAdapterFactory.h"
#include "nsIBufferedStreams.h"
#include "nsIChannelEventSink.h"
#include "nsIContentSniffer.h"
#include "nsIDocument.h"
#include "nsIDownloader.h"
#include "nsIFileProtocolHandler.h"
#include "nsIFileStreams.h"
#include "nsIFileURL.h"
#include "nsIIDNService.h"
#include "nsIInputStreamChannel.h"
#include "nsIInputStreamPump.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsILoadContext.h"
#include "nsIMIMEHeaderParam.h"
#include "nsIMutable.h"
#include "nsINode.h"
#include "nsIObjectLoadingContent.h"
#include "nsIOfflineCacheUpdate.h"
#include "nsIPersistentProperties2.h"
#include "nsIPrivateBrowsingChannel.h"
#include "nsIPropertyBag2.h"
#include "nsIProtocolProxyService.h"
#include "nsIRedirectChannelRegistrar.h"
#include "nsIRequestObserverProxy.h"
#include "nsIScriptSecurityManager.h"
#include "nsISensitiveInfoHiddenURI.h"
#include "nsISimpleStreamListener.h"
#include "nsISocketProvider.h"
#include "nsISocketProviderService.h"
#include "nsIStandardURL.h"
#include "nsIStreamLoader.h"
#include "nsIIncrementalStreamLoader.h"
#include "nsIStreamTransportService.h"
#include "nsStringStream.h"
#include "nsISyncStreamListener.h"
#include "nsITransport.h"
#include "nsIURIWithPrincipal.h"
#include "nsIURLParser.h"
#include "nsIUUIDGenerator.h"
#include "nsIViewSourceChannel.h"
#include "nsInterfaceRequestorAgg.h"
#include "plstr.h"
#include "nsINestedURI.h"
#include "mozilla/dom/nsCSPUtils.h"
#include "mozilla/net/HttpBaseChannel.h"
#include "nsIScriptError.h"
#include "nsISiteSecurityService.h"
#include "nsHttpHandler.h"
#include "nsNSSComponent.h"
#include "nsIRedirectHistoryEntry.h"
#include "nsICertBlocklist.h"
#include "nsICertOverrideService.h"
#include "nsQueryObject.h"
#include "mozIThirdPartyUtil.h"

#include <limits>

using namespace mozilla;
using namespace mozilla::net;
using mozilla::dom::ClientInfo;
using mozilla::dom::PerformanceStorage;
using mozilla::dom::ServiceWorkerDescriptor;

#define DEFAULT_RP 3
#define DEFAULT_PRIVATE_RP 2

static uint32_t sDefaultRp = DEFAULT_RP;
static uint32_t defaultPrivateRp = DEFAULT_PRIVATE_RP;

already_AddRefed<nsIIOService>
do_GetIOService(nsresult *error /* = 0 */)
{
    nsCOMPtr<nsIIOService> io = mozilla::services::GetIOService();
    if (error)
        *error = io ? NS_OK : NS_ERROR_FAILURE;
    return io.forget();
}

nsresult
NS_NewLocalFileInputStream(nsIInputStream **result,
                           nsIFile         *file,
                           int32_t          ioFlags       /* = -1 */,
                           int32_t          perm          /* = -1 */,
                           int32_t          behaviorFlags /* = 0 */)
{
    nsresult rv;
    nsCOMPtr<nsIFileInputStream> in =
        do_CreateInstance(NS_LOCALFILEINPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = in->Init(file, ioFlags, perm, behaviorFlags);
        if (NS_SUCCEEDED(rv))
            in.forget(result);
    }
    return rv;
}

nsresult
NS_NewLocalFileOutputStream(nsIOutputStream **result,
                            nsIFile          *file,
                            int32_t           ioFlags       /* = -1 */,
                            int32_t           perm          /* = -1 */,
                            int32_t           behaviorFlags /* = 0 */)
{
    nsresult rv;
    nsCOMPtr<nsIFileOutputStream> out =
        do_CreateInstance(NS_LOCALFILEOUTPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = out->Init(file, ioFlags, perm, behaviorFlags);
        if (NS_SUCCEEDED(rv))
            out.forget(result);
    }
    return rv;
}

nsresult
net_EnsureIOService(nsIIOService **ios, nsCOMPtr<nsIIOService> &grip)
{
    nsresult rv = NS_OK;
    if (!*ios) {
        grip = do_GetIOService(&rv);
        *ios = grip;
    }
    return rv;
}

nsresult
NS_NewFileURI(nsIURI **result,
              nsIFile *spec,
              nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    nsresult rv;
    nsCOMPtr<nsIIOService> grip;
    rv = net_EnsureIOService(&ioService, grip);
    if (ioService)
        rv = ioService->NewFileURI(spec, result);
    return rv;
}

nsresult
NS_NewChannelInternal(nsIChannel           **outChannel,
                      nsIURI                *aUri,
                      nsILoadInfo           *aLoadInfo,
                      PerformanceStorage    *aPerformanceStorage /* = nullptr */,
                      nsILoadGroup          *aLoadGroup /* = nullptr */,
                      nsIInterfaceRequestor *aCallbacks /* = nullptr */,
                      nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                      nsIIOService          *aIoService /* = nullptr */)
{
  // NS_NewChannelInternal is mostly called for channel redirects. We should allow
  // the creation of a channel even if the original channel did not have a loadinfo
  // attached.
  NS_ENSURE_ARG_POINTER(outChannel);

  nsCOMPtr<nsIIOService> grip;
  nsresult rv = net_EnsureIOService(&aIoService, grip);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIChannel> channel;
  rv = aIoService->NewChannelFromURIWithLoadInfo(
         aUri,
         aLoadInfo,
         getter_AddRefs(channel));
  NS_ENSURE_SUCCESS(rv, rv);

  if (aLoadGroup) {
    rv = channel->SetLoadGroup(aLoadGroup);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (aCallbacks) {
    rv = channel->SetNotificationCallbacks(aCallbacks);
    NS_ENSURE_SUCCESS(rv, rv);
  }

#ifdef DEBUG
  nsLoadFlags channelLoadFlags = 0;
  channel->GetLoadFlags(&channelLoadFlags);
  // Will be removed when we remove LOAD_REPLACE altogether
  // This check is trying to catch protocol handlers that still
  // try to set the LOAD_REPLACE flag.
  MOZ_DIAGNOSTIC_ASSERT(!(channelLoadFlags & nsIChannel::LOAD_REPLACE));
#endif

  if (aLoadFlags != nsIRequest::LOAD_NORMAL) {
    rv = channel->SetLoadFlags(aLoadFlags);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (aPerformanceStorage) {
    nsCOMPtr<nsILoadInfo> loadInfo;
    rv = channel->GetLoadInfo(getter_AddRefs(loadInfo));
    NS_ENSURE_SUCCESS(rv, rv);

    loadInfo->SetPerformanceStorage(aPerformanceStorage);
  }

  channel.forget(outChannel);
  return NS_OK;
}

namespace {

void
AssertLoadingPrincipalAndClientInfoMatch(nsIPrincipal* aLoadingPrincipal,
                                         const ClientInfo& aLoadingClientInfo,
                                         nsContentPolicyType aType)
{
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
  // Verify that the provided loading ClientInfo matches the loading
  // principal.  Unfortunately we can't just use nsIPrincipal::Equals() here
  // because of some corner cases:
  //
  //  1. Worker debugger scripts want to use a system loading principal for
  //     worker scripts with a content principal.  We exempt these from this
  //     check.
  //  2. Null principals currently require exact object identity for
  //     nsIPrincipal::Equals() to return true.  This doesn't work here because
  //     ClientInfo::GetPrincipal() uses PrincipalInfoToPrincipal() to allocate
  //     a new object.  To work around this we compare the principal origin
  //     string itself.  If bug 1431771 is fixed then we could switch to
  //     Equals().

  // Allow worker debugger to load with a system principal.
  if (aLoadingPrincipal->GetIsSystemPrincipal() &&
      (aType == nsIContentPolicy::TYPE_INTERNAL_WORKER ||
       aType == nsIContentPolicy::TYPE_INTERNAL_SHARED_WORKER ||
       aType == nsIContentPolicy::TYPE_INTERNAL_SERVICE_WORKER ||
       aType == nsIContentPolicy::TYPE_INTERNAL_WORKER_IMPORT_SCRIPTS)) {
    return;
  }

  // Perform a fast comparison for most principal checks.
  nsCOMPtr<nsIPrincipal> clientPrincipal(aLoadingClientInfo.GetPrincipal());
  if (aLoadingPrincipal->Equals(clientPrincipal)) {
    return;
  }

  // Fall back to a slower origin equality test to support null principals.
  nsAutoCString loadingOrigin;
  MOZ_ALWAYS_SUCCEEDS(aLoadingPrincipal->GetOrigin(loadingOrigin));

  nsAutoCString clientOrigin;
  MOZ_ALWAYS_SUCCEEDS(clientPrincipal->GetOrigin(clientOrigin));

  MOZ_DIAGNOSTIC_ASSERT(loadingOrigin == clientOrigin);
#endif
}

}

nsresult
NS_NewChannel(nsIChannel           **outChannel,
              nsIURI                *aUri,
              nsIPrincipal          *aLoadingPrincipal,
              nsSecurityFlags        aSecurityFlags,
              nsContentPolicyType    aContentPolicyType,
              PerformanceStorage    *aPerformanceStorage /* nullptr */,
              nsILoadGroup          *aLoadGroup /* = nullptr */,
              nsIInterfaceRequestor *aCallbacks /* = nullptr */,
              nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
              nsIIOService          *aIoService /* = nullptr */)
{
  return NS_NewChannelInternal(outChannel,
                               aUri,
                               nullptr, // aLoadingNode,
                               aLoadingPrincipal,
                               nullptr, // aTriggeringPrincipal
                               Maybe<ClientInfo>(),
                               Maybe<ServiceWorkerDescriptor>(),
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

nsresult
NS_NewChannel(nsIChannel           **outChannel,
              nsIURI                *aUri,
              nsIPrincipal          *aLoadingPrincipal,
              const ClientInfo      &aLoadingClientInfo,
              const Maybe<ServiceWorkerDescriptor>& aController,
              nsSecurityFlags        aSecurityFlags,
              nsContentPolicyType    aContentPolicyType,
              PerformanceStorage    *aPerformanceStorage /* nullptr */,
              nsILoadGroup          *aLoadGroup /* = nullptr */,
              nsIInterfaceRequestor *aCallbacks /* = nullptr */,
              nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
              nsIIOService          *aIoService /* = nullptr */)
{
  AssertLoadingPrincipalAndClientInfoMatch(aLoadingPrincipal,
                                           aLoadingClientInfo,
                                           aContentPolicyType);

  Maybe<ClientInfo> loadingClientInfo;
  loadingClientInfo.emplace(aLoadingClientInfo);

  return NS_NewChannelInternal(outChannel,
                               aUri,
                               nullptr, // aLoadingNode,
                               aLoadingPrincipal,
                               nullptr, // aTriggeringPrincipal
                               loadingClientInfo,
                               aController,
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

nsresult
NS_NewChannelInternal(nsIChannel           **outChannel,
                      nsIURI                *aUri,
                      nsINode               *aLoadingNode,
                      nsIPrincipal          *aLoadingPrincipal,
                      nsIPrincipal          *aTriggeringPrincipal,
                      const Maybe<ClientInfo>& aLoadingClientInfo,
                      const Maybe<ServiceWorkerDescriptor>& aController,
                      nsSecurityFlags        aSecurityFlags,
                      nsContentPolicyType    aContentPolicyType,
                      PerformanceStorage    *aPerformanceStorage /* nullptr */,
                      nsILoadGroup          *aLoadGroup /* = nullptr */,
                      nsIInterfaceRequestor *aCallbacks /* = nullptr */,
                      nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                      nsIIOService          *aIoService /* = nullptr */)
{
  NS_ENSURE_ARG_POINTER(outChannel);

  nsCOMPtr<nsIIOService> grip;
  nsresult rv = net_EnsureIOService(&aIoService, grip);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIChannel> channel;
  rv = aIoService->NewChannelFromURIWithClientAndController(
         aUri,
         aLoadingNode ?
           aLoadingNode->AsDOMNode() : nullptr,
         aLoadingPrincipal,
         aTriggeringPrincipal,
         aLoadingClientInfo,
         aController,
         aSecurityFlags,
         aContentPolicyType,
         getter_AddRefs(channel));
  if (NS_FAILED(rv)) {
    return rv;
  }

  if (aLoadGroup) {
    rv = channel->SetLoadGroup(aLoadGroup);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (aCallbacks) {
    rv = channel->SetNotificationCallbacks(aCallbacks);
    NS_ENSURE_SUCCESS(rv, rv);
  }

#ifdef DEBUG
  nsLoadFlags channelLoadFlags = 0;
  channel->GetLoadFlags(&channelLoadFlags);
  // Will be removed when we remove LOAD_REPLACE altogether
  // This check is trying to catch protocol handlers that still
  // try to set the LOAD_REPLACE flag.
  MOZ_DIAGNOSTIC_ASSERT(!(channelLoadFlags & nsIChannel::LOAD_REPLACE));
#endif

  if (aLoadFlags != nsIRequest::LOAD_NORMAL) {
    rv = channel->SetLoadFlags(aLoadFlags);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (aPerformanceStorage) {
    nsCOMPtr<nsILoadInfo> loadInfo;
    rv = channel->GetLoadInfo(getter_AddRefs(loadInfo));
    NS_ENSURE_SUCCESS(rv, rv);

    loadInfo->SetPerformanceStorage(aPerformanceStorage);
  }

  channel.forget(outChannel);
  return NS_OK;
}

nsresult /*NS_NewChannelWithNodeAndTriggeringPrincipal */
NS_NewChannelWithTriggeringPrincipal(nsIChannel           **outChannel,
                                     nsIURI                *aUri,
                                     nsINode               *aLoadingNode,
                                     nsIPrincipal          *aTriggeringPrincipal,
                                     nsSecurityFlags        aSecurityFlags,
                                     nsContentPolicyType    aContentPolicyType,
                                     PerformanceStorage    *aPerformanceStorage /* = nullptr */,
                                     nsILoadGroup          *aLoadGroup /* = nullptr */,
                                     nsIInterfaceRequestor *aCallbacks /* = nullptr */,
                                     nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                                     nsIIOService          *aIoService /* = nullptr */)
{
  MOZ_ASSERT(aLoadingNode);
  NS_ASSERTION(aTriggeringPrincipal, "Can not create channel without a triggering Principal!");
  return NS_NewChannelInternal(outChannel,
                               aUri,
                               aLoadingNode,
                               aLoadingNode->NodePrincipal(),
                               aTriggeringPrincipal,
                               Maybe<ClientInfo>(),
                               Maybe<ServiceWorkerDescriptor>(),
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

// See NS_NewChannelInternal for usage and argument description
nsresult
NS_NewChannelWithTriggeringPrincipal(nsIChannel           **outChannel,
                                     nsIURI                *aUri,
                                     nsIPrincipal          *aLoadingPrincipal,
                                     nsIPrincipal          *aTriggeringPrincipal,
                                     nsSecurityFlags        aSecurityFlags,
                                     nsContentPolicyType    aContentPolicyType,
                                     PerformanceStorage    *aPerformanceStorage /* = nullptr */,
                                     nsILoadGroup          *aLoadGroup /* = nullptr */,
                                     nsIInterfaceRequestor *aCallbacks /* = nullptr */,
                                     nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                                     nsIIOService          *aIoService /* = nullptr */)
{
  NS_ASSERTION(aLoadingPrincipal, "Can not create channel without a loading Principal!");
  return NS_NewChannelInternal(outChannel,
                               aUri,
                               nullptr, // aLoadingNode
                               aLoadingPrincipal,
                               aTriggeringPrincipal,
                               Maybe<ClientInfo>(),
                               Maybe<ServiceWorkerDescriptor>(),
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

// See NS_NewChannelInternal for usage and argument description
nsresult
NS_NewChannelWithTriggeringPrincipal(nsIChannel           **outChannel,
                                     nsIURI                *aUri,
                                     nsIPrincipal          *aLoadingPrincipal,
                                     nsIPrincipal          *aTriggeringPrincipal,
                                     const ClientInfo      &aLoadingClientInfo,
                                     const Maybe<ServiceWorkerDescriptor>& aController,
                                     nsSecurityFlags        aSecurityFlags,
                                     nsContentPolicyType    aContentPolicyType,
                                     PerformanceStorage    *aPerformanceStorage /* = nullptr */,
                                     nsILoadGroup          *aLoadGroup /* = nullptr */,
                                     nsIInterfaceRequestor *aCallbacks /* = nullptr */,
                                     nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                                     nsIIOService          *aIoService /* = nullptr */)
{
  AssertLoadingPrincipalAndClientInfoMatch(aLoadingPrincipal,
                                           aLoadingClientInfo,
                                           aContentPolicyType);

  Maybe<ClientInfo> loadingClientInfo;
  loadingClientInfo.emplace(aLoadingClientInfo);

  return NS_NewChannelInternal(outChannel,
                               aUri,
                               nullptr, // aLoadingNode
                               aLoadingPrincipal,
                               aTriggeringPrincipal,
                               loadingClientInfo,
                               aController,
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

nsresult
NS_NewChannel(nsIChannel           **outChannel,
              nsIURI                *aUri,
              nsINode               *aLoadingNode,
              nsSecurityFlags        aSecurityFlags,
              nsContentPolicyType    aContentPolicyType,
              PerformanceStorage    *aPerformanceStorage /* = nullptr */,
              nsILoadGroup          *aLoadGroup /* = nullptr */,
              nsIInterfaceRequestor *aCallbacks /* = nullptr */,
              nsLoadFlags            aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
              nsIIOService          *aIoService /* = nullptr */)
{
  NS_ASSERTION(aLoadingNode, "Can not create channel without a loading Node!");
  return NS_NewChannelInternal(outChannel,
                               aUri,
                               aLoadingNode,
                               aLoadingNode->NodePrincipal(),
                               nullptr, // aTriggeringPrincipal
                               Maybe<ClientInfo>(),
                               Maybe<ServiceWorkerDescriptor>(),
                               aSecurityFlags,
                               aContentPolicyType,
                               aPerformanceStorage,
                               aLoadGroup,
                               aCallbacks,
                               aLoadFlags,
                               aIoService);
}

nsresult
NS_GetIsDocumentChannel(nsIChannel * aChannel, bool *aIsDocument)
{
  // Check if this channel is going to be used to create a document. If it has
  // LOAD_DOCUMENT_URI set it is trivially creating a document. If
  // LOAD_HTML_OBJECT_DATA is set it may or may not be used to create a
  // document, depending on its MIME type.

  if (!aChannel || !aIsDocument) {
      return NS_ERROR_NULL_POINTER;
  }
  *aIsDocument = false;
  nsLoadFlags loadFlags;
  nsresult rv = aChannel->GetLoadFlags(&loadFlags);
  if (NS_FAILED(rv)) {
      return rv;
  }
  if (loadFlags & nsIChannel::LOAD_DOCUMENT_URI) {
      *aIsDocument = true;
      return NS_OK;
  }
  if (!(loadFlags & nsIRequest::LOAD_HTML_OBJECT_DATA)) {
      *aIsDocument = false;
      return NS_OK;
  }
  nsAutoCString mimeType;
  rv = aChannel->GetContentType(mimeType);
  if (NS_FAILED(rv)) {
      return rv;
  }
  if (nsContentUtils::HtmlObjectContentTypeForMIMEType(mimeType, false, nullptr) ==
      nsIObjectLoadingContent::TYPE_DOCUMENT) {
      *aIsDocument = true;
      return NS_OK;
  }
  *aIsDocument = false;
  return NS_OK;
}

nsresult
NS_MakeAbsoluteURI(nsACString       &result,
                   const nsACString &spec,
                   nsIURI           *baseURI)
{
    nsresult rv;
    if (!baseURI) {
        NS_WARNING("It doesn't make sense to not supply a base URI");
        result = spec;
        rv = NS_OK;
    }
    else if (spec.IsEmpty())
        rv = baseURI->GetSpec(result);
    else
        rv = baseURI->Resolve(spec, result);
    return rv;
}

nsresult
NS_MakeAbsoluteURI(char        **result,
                   const char   *spec,
                   nsIURI       *baseURI)
{
    nsresult rv;
    nsAutoCString resultBuf;
    rv = NS_MakeAbsoluteURI(resultBuf, nsDependentCString(spec), baseURI);
    if (NS_SUCCEEDED(rv)) {
        *result = ToNewCString(resultBuf);
        if (!*result)
            rv = NS_ERROR_OUT_OF_MEMORY;
    }
    return rv;
}

nsresult
NS_MakeAbsoluteURI(nsAString       &result,
                   const nsAString &spec,
                   nsIURI          *baseURI)
{
    nsresult rv;
    if (!baseURI) {
        NS_WARNING("It doesn't make sense to not supply a base URI");
        result = spec;
        rv = NS_OK;
    }
    else {
        nsAutoCString resultBuf;
        if (spec.IsEmpty())
            rv = baseURI->GetSpec(resultBuf);
        else
            rv = baseURI->Resolve(NS_ConvertUTF16toUTF8(spec), resultBuf);
        if (NS_SUCCEEDED(rv))
            CopyUTF8toUTF16(resultBuf, result);
    }
    return rv;
}

int32_t
NS_GetDefaultPort(const char *scheme,
                  nsIIOService *ioService /* = nullptr */)
{
  nsresult rv;

  // Getting the default port through the protocol handler has a lot of XPCOM
  // overhead involved.  We optimize the protocols that matter for Web pages
  // (HTTP and HTTPS) by hardcoding their default ports here.
  if (strncmp(scheme, "http", 4) == 0) {
    if (scheme[4] == 's' && scheme[5] == '\0') {
      return 443;
    }
    if (scheme[4] == '\0') {
      return 80;
    }
  }

  nsCOMPtr<nsIIOService> grip;
  net_EnsureIOService(&ioService, grip);
  if (!ioService)
      return -1;

  nsCOMPtr<nsIProtocolHandler> handler;
  rv = ioService->GetProtocolHandler(scheme, getter_AddRefs(handler));
  if (NS_FAILED(rv))
    return -1;
  int32_t port;
  rv = handler->GetDefaultPort(&port);
  return NS_SUCCEEDED(rv) ? port : -1;
}

/**
 * This function is a helper function to apply the ToAscii conversion
 * to a string
 */
bool
NS_StringToACE(const nsACString &idn, nsACString &result)
{
  nsCOMPtr<nsIIDNService> idnSrv = do_GetService(NS_IDNSERVICE_CONTRACTID);
  if (!idnSrv)
    return false;
  nsresult rv = idnSrv->ConvertUTF8toACE(idn, result);
  if (NS_FAILED(rv))
    return false;

  return true;
}

int32_t
NS_GetRealPort(nsIURI *aURI)
{
    int32_t port;
    nsresult rv = aURI->GetPort(&port);
    if (NS_FAILED(rv))
        return -1;

    if (port != -1)
        return port; // explicitly specified

    // Otherwise, we have to get the default port from the protocol handler

    // Need the scheme first
    nsAutoCString scheme;
    rv = aURI->GetScheme(scheme);
    if (NS_FAILED(rv))
        return -1;

    return NS_GetDefaultPort(scheme.get());
}

nsresult
NS_NewInputStreamChannelInternal(nsIChannel** outChannel,
                                 nsIURI* aUri,
                                 already_AddRefed<nsIInputStream> aStream,
                                 const nsACString& aContentType,
                                 const nsACString& aContentCharset,
                                 nsILoadInfo* aLoadInfo)
{
  nsresult rv;
  nsCOMPtr<nsIInputStreamChannel> isc =
    do_CreateInstance(NS_INPUTSTREAMCHANNEL_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  rv = isc->SetURI(aUri);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIInputStream> stream = Move(aStream);
  rv = isc->SetContentStream(stream);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIChannel> channel = do_QueryInterface(isc, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!aContentType.IsEmpty()) {
    rv = channel->SetContentType(aContentType);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (!aContentCharset.IsEmpty()) {
    rv = channel->SetContentCharset(aContentCharset);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  channel->SetLoadInfo(aLoadInfo);

  // If we're sandboxed, make sure to clear any owner the channel
  // might already have.
  if (aLoadInfo && aLoadInfo->GetLoadingSandboxed()) {
    channel->SetOwner(nullptr);
  }

  channel.forget(outChannel);
  return NS_OK;
}

nsresult
NS_NewInputStreamChannelInternal(nsIChannel** outChannel,
                                 nsIURI* aUri,
                                 already_AddRefed<nsIInputStream> aStream,
                                 const nsACString& aContentType,
                                 const nsACString& aContentCharset,
                                 nsINode* aLoadingNode,
                                 nsIPrincipal* aLoadingPrincipal,
                                 nsIPrincipal* aTriggeringPrincipal,
                                 nsSecurityFlags aSecurityFlags,
                                 nsContentPolicyType aContentPolicyType)
{
  nsCOMPtr<nsILoadInfo> loadInfo =
    new mozilla::LoadInfo(aLoadingPrincipal,
                          aTriggeringPrincipal,
                          aLoadingNode,
                          aSecurityFlags,
                          aContentPolicyType);
  if (!loadInfo) {
    return NS_ERROR_UNEXPECTED;
  }

  nsCOMPtr<nsIInputStream> stream = Move(aStream);

  return NS_NewInputStreamChannelInternal(outChannel,
                                          aUri,
                                          stream.forget(),
                                          aContentType,
                                          aContentCharset,
                                          loadInfo);
}

nsresult
NS_NewInputStreamChannel(nsIChannel** outChannel,
                         nsIURI* aUri,
                         already_AddRefed<nsIInputStream> aStream,
                         nsIPrincipal* aLoadingPrincipal,
                         nsSecurityFlags aSecurityFlags,
                         nsContentPolicyType aContentPolicyType,
                         const nsACString& aContentType    /* = EmptyCString() */,
                         const nsACString& aContentCharset /* = EmptyCString() */)
{
  nsCOMPtr<nsIInputStream> stream = aStream;
  return NS_NewInputStreamChannelInternal(outChannel,
                                          aUri,
                                          stream.forget(),
                                          aContentType,
                                          aContentCharset,
                                          nullptr, // aLoadingNode
                                          aLoadingPrincipal,
                                          nullptr, // aTriggeringPrincipal
                                          aSecurityFlags,
                                          aContentPolicyType);
}

nsresult
NS_NewInputStreamChannelInternal(nsIChannel        **outChannel,
                                 nsIURI             *aUri,
                                 const nsAString    &aData,
                                 const nsACString   &aContentType,
                                 nsILoadInfo        *aLoadInfo,
                                 bool                aIsSrcdocChannel /* = false */)
{
  nsresult rv;
  nsCOMPtr<nsIStringInputStream> stream;
  stream = do_CreateInstance(NS_STRINGINPUTSTREAM_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

    uint32_t len;
    char* utf8Bytes = ToNewUTF8String(aData, &len);
    rv = stream->AdoptData(utf8Bytes, len);

  nsCOMPtr<nsIChannel> channel;
  rv = NS_NewInputStreamChannelInternal(getter_AddRefs(channel),
                                        aUri,
                                        stream.forget(),
                                        aContentType,
                                        NS_LITERAL_CSTRING("UTF-8"),
                                        aLoadInfo);

  NS_ENSURE_SUCCESS(rv, rv);

  if (aIsSrcdocChannel) {
    nsCOMPtr<nsIInputStreamChannel> inStrmChan = do_QueryInterface(channel);
    NS_ENSURE_TRUE(inStrmChan, NS_ERROR_FAILURE);
    inStrmChan->SetSrcdocData(aData);
  }
  channel.forget(outChannel);
  return NS_OK;
}

nsresult
NS_NewInputStreamChannelInternal(nsIChannel        **outChannel,
                                 nsIURI             *aUri,
                                 const nsAString    &aData,
                                 const nsACString   &aContentType,
                                 nsINode            *aLoadingNode,
                                 nsIPrincipal       *aLoadingPrincipal,
                                 nsIPrincipal       *aTriggeringPrincipal,
                                 nsSecurityFlags     aSecurityFlags,
                                 nsContentPolicyType aContentPolicyType,
                                 bool                aIsSrcdocChannel /* = false */)
{
  nsCOMPtr<nsILoadInfo> loadInfo =
      new mozilla::LoadInfo(aLoadingPrincipal, aTriggeringPrincipal,
                            aLoadingNode, aSecurityFlags, aContentPolicyType);
  return NS_NewInputStreamChannelInternal(outChannel, aUri, aData, aContentType,
                                          loadInfo, aIsSrcdocChannel);
}

nsresult
NS_NewInputStreamChannel(nsIChannel        **outChannel,
                         nsIURI             *aUri,
                         const nsAString    &aData,
                         const nsACString   &aContentType,
                         nsIPrincipal       *aLoadingPrincipal,
                         nsSecurityFlags     aSecurityFlags,
                         nsContentPolicyType aContentPolicyType,
                         bool                aIsSrcdocChannel /* = false */)
{
  return NS_NewInputStreamChannelInternal(outChannel,
                                          aUri,
                                          aData,
                                          aContentType,
                                          nullptr, // aLoadingNode
                                          aLoadingPrincipal,
                                          nullptr, // aTriggeringPrincipal
                                          aSecurityFlags,
                                          aContentPolicyType,
                                          aIsSrcdocChannel);
}

nsresult
NS_NewInputStreamPump(nsIInputStreamPump** aResult,
                      already_AddRefed<nsIInputStream> aStream,
                      uint32_t aSegsize /* = 0 */,
                      uint32_t aSegcount /* = 0 */,
                      bool aCloseWhenDone /* = false */,
                      nsIEventTarget* aMainThreadTarget /* = nullptr */)
{
    nsCOMPtr<nsIInputStream> stream = Move(aStream);

    nsresult rv;
    nsCOMPtr<nsIInputStreamPump> pump =
        do_CreateInstance(NS_INPUTSTREAMPUMP_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = pump->Init(stream, aSegsize, aSegcount, aCloseWhenDone,
                        aMainThreadTarget);
        if (NS_SUCCEEDED(rv)) {
            *aResult = nullptr;
            pump.swap(*aResult);
        }
    }
    return rv;
}

nsresult
NS_NewLoadGroup(nsILoadGroup      **result,
                nsIRequestObserver *obs)
{
    nsresult rv;
    nsCOMPtr<nsILoadGroup> group =
        do_CreateInstance(NS_LOADGROUP_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = group->SetGroupObserver(obs);
        if (NS_SUCCEEDED(rv)) {
            *result = nullptr;
            group.swap(*result);
        }
    }
    return rv;
}

bool NS_IsReasonableHTTPHeaderValue(const nsACString &aValue)
{
  return mozilla::net::nsHttp::IsReasonableHeaderValue(aValue);
}

bool NS_IsValidHTTPToken(const nsACString &aToken)
{
  return mozilla::net::nsHttp::IsValidToken(aToken);
}

void
NS_TrimHTTPWhitespace(const nsACString& aSource, nsACString& aDest)
{
  mozilla::net::nsHttp::TrimHTTPWhitespace(aSource, aDest);
}

nsresult
NS_NewLoadGroup(nsILoadGroup **aResult, nsIPrincipal *aPrincipal)
{
    using mozilla::LoadContext;
    nsresult rv;

    nsCOMPtr<nsILoadGroup> group =
        do_CreateInstance(NS_LOADGROUP_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);

    RefPtr<LoadContext> loadContext = new LoadContext(aPrincipal);
    rv = group->SetNotificationCallbacks(loadContext);
    NS_ENSURE_SUCCESS(rv, rv);

    group.forget(aResult);
    return rv;
}

bool
NS_LoadGroupMatchesPrincipal(nsILoadGroup *aLoadGroup,
                             nsIPrincipal *aPrincipal)
{
    if (!aPrincipal) {
      return false;
    }

    // If this is a null principal then the load group doesn't really matter.
    // The principal will not be allowed to perform any actions that actually
    // use the load group.  Unconditionally treat null principals as a match.
    if (aPrincipal->GetIsNullPrincipal()) {
      return true;
    }

    if (!aLoadGroup) {
        return false;
    }

    nsCOMPtr<nsILoadContext> loadContext;
    NS_QueryNotificationCallbacks(nullptr, aLoadGroup, NS_GET_IID(nsILoadContext),
                                  getter_AddRefs(loadContext));
    NS_ENSURE_TRUE(loadContext, false);

    // Verify load context browser flag match the principal
    bool contextInIsolatedBrowser;
    nsresult rv = loadContext->GetIsInIsolatedMozBrowserElement(&contextInIsolatedBrowser);
    NS_ENSURE_SUCCESS(rv, false);

    return contextInIsolatedBrowser == aPrincipal->GetIsInIsolatedMozBrowserElement();
}

nsresult
NS_NewDownloader(nsIStreamListener   **result,
                 nsIDownloadObserver  *observer,
                 nsIFile              *downloadLocation /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIDownloader> downloader =
        do_CreateInstance(NS_DOWNLOADER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = downloader->Init(observer, downloadLocation);
        if (NS_SUCCEEDED(rv)) {
            downloader.forget(result);
        }
    }
    return rv;
}

nsresult
NS_NewIncrementalStreamLoader(nsIIncrementalStreamLoader        **result,
                              nsIIncrementalStreamLoaderObserver *observer)
{
    nsresult rv;
    nsCOMPtr<nsIIncrementalStreamLoader> loader =
        do_CreateInstance(NS_INCREMENTALSTREAMLOADER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = loader->Init(observer);
        if (NS_SUCCEEDED(rv)) {
            *result = nullptr;
            loader.swap(*result);
        }
    }
    return rv;
}

nsresult
NS_NewStreamLoader(nsIStreamLoader        **result,
                   nsIStreamLoaderObserver *observer,
                   nsIRequestObserver      *requestObserver /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIStreamLoader> loader =
        do_CreateInstance(NS_STREAMLOADER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = loader->Init(observer, requestObserver);
        if (NS_SUCCEEDED(rv)) {
            *result = nullptr;
            loader.swap(*result);
        }
    }
    return rv;
}

nsresult
NS_NewStreamLoaderInternal(nsIStreamLoader        **outStream,
                           nsIURI                  *aUri,
                           nsIStreamLoaderObserver *aObserver,
                           nsINode                 *aLoadingNode,
                           nsIPrincipal            *aLoadingPrincipal,
                           nsSecurityFlags          aSecurityFlags,
                           nsContentPolicyType      aContentPolicyType,
                           nsILoadGroup            *aLoadGroup /* = nullptr */,
                           nsIInterfaceRequestor   *aCallbacks /* = nullptr */,
                           nsLoadFlags              aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                           nsIURI                  *aReferrer /* = nullptr */)
{
   nsCOMPtr<nsIChannel> channel;
   nsresult rv = NS_NewChannelInternal(getter_AddRefs(channel),
                                       aUri,
                                       aLoadingNode,
                                       aLoadingPrincipal,
                                       nullptr, // aTriggeringPrincipal
                                       Maybe<ClientInfo>(),
                                       Maybe<ServiceWorkerDescriptor>(),
                                       aSecurityFlags,
                                       aContentPolicyType,
                                       nullptr, // PerformanceStorage
                                       aLoadGroup,
                                       aCallbacks,
                                       aLoadFlags);

  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIHttpChannel> httpChannel(do_QueryInterface(channel));
  if (httpChannel) {
    rv = httpChannel->SetReferrer(aReferrer);
    MOZ_ASSERT(NS_SUCCEEDED(rv));
  }
  rv = NS_NewStreamLoader(outStream, aObserver);
  NS_ENSURE_SUCCESS(rv, rv);
  return channel->AsyncOpen2(*outStream);
}


nsresult
NS_NewStreamLoader(nsIStreamLoader        **outStream,
                   nsIURI                  *aUri,
                   nsIStreamLoaderObserver *aObserver,
                   nsINode                 *aLoadingNode,
                   nsSecurityFlags          aSecurityFlags,
                   nsContentPolicyType      aContentPolicyType,
                   nsILoadGroup            *aLoadGroup /* = nullptr */,
                   nsIInterfaceRequestor   *aCallbacks /* = nullptr */,
                   nsLoadFlags              aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                   nsIURI                  *aReferrer /* = nullptr */)
{
  NS_ASSERTION(aLoadingNode, "Can not create stream loader without a loading Node!");
  return NS_NewStreamLoaderInternal(outStream,
                                    aUri,
                                    aObserver,
                                    aLoadingNode,
                                    aLoadingNode->NodePrincipal(),
                                    aSecurityFlags,
                                    aContentPolicyType,
                                    aLoadGroup,
                                    aCallbacks,
                                    aLoadFlags,
                                    aReferrer);
}

nsresult
NS_NewStreamLoader(nsIStreamLoader        **outStream,
                   nsIURI                  *aUri,
                   nsIStreamLoaderObserver *aObserver,
                   nsIPrincipal            *aLoadingPrincipal,
                   nsSecurityFlags          aSecurityFlags,
                   nsContentPolicyType      aContentPolicyType,
                   nsILoadGroup            *aLoadGroup /* = nullptr */,
                   nsIInterfaceRequestor   *aCallbacks /* = nullptr */,
                   nsLoadFlags              aLoadFlags /* = nsIRequest::LOAD_NORMAL */,
                   nsIURI                  *aReferrer /* = nullptr */)
{
  return NS_NewStreamLoaderInternal(outStream,
                                    aUri,
                                    aObserver,
                                    nullptr, // aLoadingNode
                                    aLoadingPrincipal,
                                    aSecurityFlags,
                                    aContentPolicyType,
                                    aLoadGroup,
                                    aCallbacks,
                                    aLoadFlags,
                                    aReferrer);
}

nsresult
NS_NewSyncStreamListener(nsIStreamListener **result,
                         nsIInputStream    **stream)
{
    nsresult rv;
    nsCOMPtr<nsISyncStreamListener> listener =
        do_CreateInstance(NS_SYNCSTREAMLISTENER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = listener->GetInputStream(stream);
        if (NS_SUCCEEDED(rv)) {
            listener.forget(result);
        }
    }
    return rv;
}

nsresult
NS_ImplementChannelOpen(nsIChannel      *channel,
                        nsIInputStream **result)
{
    nsCOMPtr<nsIStreamListener> listener;
    nsCOMPtr<nsIInputStream> stream;
    nsresult rv = NS_NewSyncStreamListener(getter_AddRefs(listener),
                                           getter_AddRefs(stream));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_MaybeOpenChannelUsingAsyncOpen2(channel, listener);
    NS_ENSURE_SUCCESS(rv, rv);

    uint64_t n;
    // block until the initial response is received or an error occurs.
    rv = stream->Available(&n);
    NS_ENSURE_SUCCESS(rv, rv);

    *result = nullptr;
    stream.swap(*result);

    return NS_OK;
 }

nsresult
NS_NewRequestObserverProxy(nsIRequestObserver **result,
                           nsIRequestObserver  *observer,
                           nsISupports         *context)
{
    nsresult rv;
    nsCOMPtr<nsIRequestObserverProxy> proxy =
        do_CreateInstance(NS_REQUESTOBSERVERPROXY_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = proxy->Init(observer, context);
        if (NS_SUCCEEDED(rv)) {
            proxy.forget(result);
        }
    }
    return rv;
}

nsresult
NS_NewSimpleStreamListener(nsIStreamListener **result,
                           nsIOutputStream    *sink,
                           nsIRequestObserver *observer /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsISimpleStreamListener> listener =
        do_CreateInstance(NS_SIMPLESTREAMLISTENER_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = listener->Init(sink, observer);
        if (NS_SUCCEEDED(rv)) {
            listener.forget(result);
        }
    }
    return rv;
}

nsresult
NS_CheckPortSafety(int32_t       port,
                   const char   *scheme,
                   nsIIOService *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIIOService> grip;
    rv = net_EnsureIOService(&ioService, grip);
    if (ioService) {
        bool allow;
        rv = ioService->AllowPort(port, scheme, &allow);
        if (NS_SUCCEEDED(rv) && !allow) {
            NS_WARNING("port blocked");
            rv = NS_ERROR_PORT_ACCESS_NOT_ALLOWED;
        }
    }
    return rv;
}

nsresult
NS_CheckPortSafety(nsIURI *uri)
{
    int32_t port;
    nsresult rv = uri->GetPort(&port);
    if (NS_FAILED(rv) || port == -1)  // port undefined or default-valued
        return NS_OK;
    nsAutoCString scheme;
    uri->GetScheme(scheme);
    return NS_CheckPortSafety(port, scheme.get());
}

nsresult
NS_NewProxyInfo(const nsACString &type,
                const nsACString &host,
                int32_t           port,
                uint32_t          flags,
                nsIProxyInfo    **result)
{
    nsresult rv;
    nsCOMPtr<nsIProtocolProxyService> pps =
            do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv))
        rv = pps->NewProxyInfo(type, host, port, flags, UINT32_MAX, nullptr,
                               result);
    return rv;
}

nsresult
NS_GetFileProtocolHandler(nsIFileProtocolHandler **result,
                          nsIIOService            *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIIOService> grip;
    rv = net_EnsureIOService(&ioService, grip);
    if (ioService) {
        nsCOMPtr<nsIProtocolHandler> handler;
        rv = ioService->GetProtocolHandler("file", getter_AddRefs(handler));
        if (NS_SUCCEEDED(rv))
            rv = CallQueryInterface(handler, result);
    }
    return rv;
}

nsresult
NS_GetFileFromURLSpec(const nsACString  &inURL,
                      nsIFile          **result,
                      nsIIOService      *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIFileProtocolHandler> fileHandler;
    rv = NS_GetFileProtocolHandler(getter_AddRefs(fileHandler), ioService);
    if (NS_SUCCEEDED(rv))
        rv = fileHandler->GetFileFromURLSpec(inURL, result);
    return rv;
}

nsresult
NS_GetURLSpecFromFile(nsIFile      *file,
                      nsACString   &url,
                      nsIIOService *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIFileProtocolHandler> fileHandler;
    rv = NS_GetFileProtocolHandler(getter_AddRefs(fileHandler), ioService);
    if (NS_SUCCEEDED(rv))
        rv = fileHandler->GetURLSpecFromFile(file, url);
    return rv;
}

nsresult
NS_GetURLSpecFromActualFile(nsIFile      *file,
                            nsACString   &url,
                            nsIIOService *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIFileProtocolHandler> fileHandler;
    rv = NS_GetFileProtocolHandler(getter_AddRefs(fileHandler), ioService);
    if (NS_SUCCEEDED(rv))
        rv = fileHandler->GetURLSpecFromActualFile(file, url);
    return rv;
}

nsresult
NS_GetURLSpecFromDir(nsIFile      *file,
                     nsACString   &url,
                     nsIIOService *ioService /* = nullptr */)
{
    nsresult rv;
    nsCOMPtr<nsIFileProtocolHandler> fileHandler;
    rv = NS_GetFileProtocolHandler(getter_AddRefs(fileHandler), ioService);
    if (NS_SUCCEEDED(rv))
        rv = fileHandler->GetURLSpecFromDir(file, url);
    return rv;
}

nsresult
NS_GetReferrerFromChannel(nsIChannel *channel,
                          nsIURI **referrer)
{
    nsresult rv = NS_ERROR_NOT_AVAILABLE;
    *referrer = nullptr;

    nsCOMPtr<nsIPropertyBag2> props(do_QueryInterface(channel));
    if (props) {
      // We have to check for a property on a property bag because the
      // referrer may be empty for security reasons (for example, when loading
      // an http page with an https referrer).
      rv = props->GetPropertyAsInterface(NS_LITERAL_STRING("docshell.internalReferrer"),
                                         NS_GET_IID(nsIURI),
                                         reinterpret_cast<void **>(referrer));
      if (NS_FAILED(rv))
        *referrer = nullptr;
    }

    // if that didn't work, we can still try to get the referrer from the
    // nsIHttpChannel (if we can QI to it)
    if (!(*referrer)) {
      nsCOMPtr<nsIHttpChannel> chan(do_QueryInterface(channel));
      if (chan) {
        rv = chan->GetReferrer(referrer);
        if (NS_FAILED(rv))
          *referrer = nullptr;
      }
    }
    return rv;
}

already_AddRefed<nsINetUtil>
do_GetNetUtil(nsresult *error /* = 0 */)
{
    nsCOMPtr<nsIIOService> io = mozilla::services::GetIOService();
    nsCOMPtr<nsINetUtil> util;
    if (io)
        util = do_QueryInterface(io);

    if (error)
        *error = !!util ? NS_OK : NS_ERROR_FAILURE;
    return util.forget();
}

nsresult
NS_ParseRequestContentType(const nsACString &rawContentType,
                           nsCString        &contentType,
                           nsCString        &contentCharset)
{
    // contentCharset is left untouched if not present in rawContentType
    nsresult rv;
    nsCOMPtr<nsINetUtil> util = do_GetNetUtil(&rv);
    NS_ENSURE_SUCCESS(rv, rv);
    nsCString charset;
    bool hadCharset;
    rv = util->ParseRequestContentType(rawContentType, charset, &hadCharset,
                                       contentType);
    if (NS_SUCCEEDED(rv) && hadCharset)
        contentCharset = charset;
    return rv;
}

nsresult
NS_ParseResponseContentType(const nsACString &rawContentType,
                            nsCString        &contentType,
                            nsCString        &contentCharset)
{
    // contentCharset is left untouched if not present in rawContentType
    nsresult rv;
    nsCOMPtr<nsINetUtil> util = do_GetNetUtil(&rv);
    NS_ENSURE_SUCCESS(rv, rv);
    nsCString charset;
    bool hadCharset;
    rv = util->ParseResponseContentType(rawContentType, charset, &hadCharset,
                                        contentType);
    if (NS_SUCCEEDED(rv) && hadCharset)
        contentCharset = charset;
    return rv;
}

nsresult
NS_ExtractCharsetFromContentType(const nsACString &rawContentType,
                                 nsCString        &contentCharset,
                                 bool             *hadCharset,
                                 int32_t          *charsetStart,
                                 int32_t          *charsetEnd)
{
    // contentCharset is left untouched if not present in rawContentType
    nsresult rv;
    nsCOMPtr<nsINetUtil> util = do_GetNetUtil(&rv);
    NS_ENSURE_SUCCESS(rv, rv);

    return util->ExtractCharsetFromContentType(rawContentType,
                                               contentCharset,
                                               charsetStart,
                                               charsetEnd,
                                               hadCharset);
}

nsresult
NS_NewAtomicFileOutputStream(nsIOutputStream **result,
                                nsIFile       *file,
                                int32_t        ioFlags       /* = -1 */,
                                int32_t        perm          /* = -1 */,
                                int32_t        behaviorFlags /* = 0 */)
{
    nsresult rv;
    nsCOMPtr<nsIFileOutputStream> out =
        do_CreateInstance(NS_ATOMICLOCALFILEOUTPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = out->Init(file, ioFlags, perm, behaviorFlags);
        if (NS_SUCCEEDED(rv))
            out.forget(result);
    }
    return rv;
}

nsresult
NS_NewSafeLocalFileOutputStream(nsIOutputStream **result,
                                nsIFile          *file,
                                int32_t           ioFlags       /* = -1 */,
                                int32_t           perm          /* = -1 */,
                                int32_t           behaviorFlags /* = 0 */)
{
    nsresult rv;
    nsCOMPtr<nsIFileOutputStream> out =
        do_CreateInstance(NS_SAFELOCALFILEOUTPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = out->Init(file, ioFlags, perm, behaviorFlags);
        if (NS_SUCCEEDED(rv))
            out.forget(result);
    }
    return rv;
}

nsresult
NS_NewLocalFileStream(nsIFileStream **result,
                      nsIFile        *file,
                      int32_t         ioFlags       /* = -1 */,
                      int32_t         perm          /* = -1 */,
                      int32_t         behaviorFlags /* = 0 */)
{
    nsresult rv;
    nsCOMPtr<nsIFileStream> stream =
        do_CreateInstance(NS_LOCALFILESTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = stream->Init(file, ioFlags, perm, behaviorFlags);
        if (NS_SUCCEEDED(rv))
            stream.forget(result);
    }
    return rv;
}

nsresult
NS_NewBufferedOutputStream(nsIOutputStream** aResult,
                           already_AddRefed<nsIOutputStream> aOutputStream,
                           uint32_t aBufferSize)
{
    nsCOMPtr<nsIOutputStream> outputStream = Move(aOutputStream);

    nsresult rv;
    nsCOMPtr<nsIBufferedOutputStream> out =
        do_CreateInstance(NS_BUFFEREDOUTPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = out->Init(outputStream, aBufferSize);
        if (NS_SUCCEEDED(rv)) {
            out.forget(aResult);
        }
    }
    return rv;
}

MOZ_MUST_USE nsresult
NS_NewBufferedInputStream(nsIInputStream** aResult,
                          already_AddRefed<nsIInputStream> aInputStream,
                          uint32_t aBufferSize)
{
    nsCOMPtr<nsIInputStream> inputStream = Move(aInputStream);

    nsresult rv;
    nsCOMPtr<nsIBufferedInputStream> in =
        do_CreateInstance(NS_BUFFEREDINPUTSTREAM_CONTRACTID, &rv);
    if (NS_SUCCEEDED(rv)) {
        rv = in->Init(inputStream, aBufferSize);
        if (NS_SUCCEEDED(rv)) {
            in.forget(aResult);
        }
    }
    return rv;
}

namespace {

#define BUFFER_SIZE 4096

class BufferWriter final : public Runnable
                         , public nsIInputStreamCallback
{
public:
    NS_DECL_ISUPPORTS_INHERITED

    BufferWriter(nsIInputStream* aInputStream,
                 void* aBuffer, int64_t aCount)
        : Runnable("BufferWriterRunnable")
        , mMonitor("BufferWriter.mMonitor")
        , mInputStream(aInputStream)
        , mBuffer(aBuffer)
        , mCount(aCount)
        , mWrittenData(0)
        , mBufferType(aBuffer ? eExternal : eInternal)
        , mAsyncResult(NS_OK)
        , mBufferSize(0)
        , mCompleted(false)
    {
        MOZ_ASSERT(aInputStream);
        MOZ_ASSERT(aCount == -1 || aCount > 0);
        MOZ_ASSERT_IF(mBuffer, aCount > 0);
    }

    nsresult
    Write()
    {
        // Let's make the inputStream buffered if it's not.
        if (!NS_InputStreamIsBuffered(mInputStream)) {
            nsCOMPtr<nsIInputStream> bufferedStream;
            nsresult rv =
                NS_NewBufferedInputStream(getter_AddRefs(bufferedStream),
                                          mInputStream.forget(), BUFFER_SIZE);
            NS_ENSURE_SUCCESS(rv, rv);

            mInputStream = bufferedStream;
        }

        mAsyncInputStream = do_QueryInterface(mInputStream);

        if (!mAsyncInputStream) {
            return WriteSync();
        }

        // Let's use mAsyncInputStream only.
        mInputStream = nullptr;

        return WriteAsync();
    }

    uint64_t
    WrittenData() const
    {
        return mWrittenData;
    }

    void*
    StealBuffer()
    {
        MOZ_ASSERT(mBufferType == eInternal);

        MonitorAutoLock lock(mMonitor);

        void* buffer = mBuffer;

        mBuffer = nullptr;
        mBufferSize = 0;

        return buffer;
    }

private:
    ~BufferWriter()
    {
        if (mBuffer && mBufferType == eInternal) {
            free(mBuffer);
        }

        if (mTaskQueue) {
            mTaskQueue->BeginShutdown();
        }
    }

    nsresult
    WriteSync()
    {
        uint64_t length = (uint64_t)mCount;

        if (mCount == -1) {
            nsresult rv = mInputStream->Available(&length);
            NS_ENSURE_SUCCESS(rv, rv);

            if (length == 0) {
                // nothing to read.
                return NS_OK;
            }
        }

        if (mBufferType == eInternal) {
            mBuffer = malloc(length);
            if (NS_WARN_IF(!mBuffer)) {
                return NS_ERROR_OUT_OF_MEMORY;
            }
        }

        uint32_t writtenData;
        nsresult rv = mInputStream->ReadSegments(NS_CopySegmentToBuffer,
                                                 mBuffer, length,
                                                 &writtenData);
        NS_ENSURE_SUCCESS(rv, rv);

        mWrittenData = writtenData;
        return NS_OK;
    }

    nsresult
    WriteAsync()
    {
        if (mCount > 0 && mBufferType == eInternal) {
            mBuffer = malloc(mCount);
            if (NS_WARN_IF(!mBuffer)) {
                return NS_ERROR_OUT_OF_MEMORY;
            }
        }

        nsCOMPtr<nsIEventTarget> target =
            do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
        if (!target) {
            return NS_ERROR_FAILURE;
        }

        mTaskQueue = new TaskQueue(target.forget());

        MonitorAutoLock lock(mMonitor);

        nsCOMPtr<nsIRunnable> runnable = this;
        nsresult rv = mTaskQueue->Dispatch(runnable.forget());
        NS_ENSURE_SUCCESS(rv, rv);

        lock.Wait();

        mCompleted = true;
        return mAsyncResult;
    }

    // This method runs on the I/O Thread when the owning thread is blocked by
    // the mMonitor. It is called multiple times until mCount is greater than 0
    // or until there is something to read in the stream.
    NS_IMETHOD
    Run() override
    {
        MOZ_ASSERT(mAsyncInputStream);
        MOZ_ASSERT(!mInputStream);

        MonitorAutoLock lock(mMonitor);

        if (mCompleted) {
            return NS_OK;
        }

        if (mCount == 0) {
            OperationCompleted(lock, NS_OK);
            return NS_OK;
        }

        if (mCount == -1 && !MaybeExpandBufferSize()) {
            OperationCompleted(lock, NS_ERROR_OUT_OF_MEMORY);
            return NS_OK;
        }

        uint64_t offset = mWrittenData;
        uint64_t length = mCount == -1 ? BUFFER_SIZE : mCount;

        // Let's try to read it directly.
        uint32_t writtenData;
        nsresult rv = mAsyncInputStream->ReadSegments(NS_CopySegmentToBuffer,
                                                     static_cast<char*>(mBuffer) + offset,
                                                     length, &writtenData);

        // Operation completed.
        if (NS_SUCCEEDED(rv) && writtenData == 0) {
            OperationCompleted(lock, NS_OK);
            return NS_OK;
        }

        // If we succeeded, let's try to read again.
        if (NS_SUCCEEDED(rv)) {
            mWrittenData += writtenData;
            if (mCount != -1) {
                MOZ_ASSERT(mCount >= writtenData);
                mCount -= writtenData;
            }

            nsCOMPtr<nsIRunnable> runnable = this;
            rv = mTaskQueue->Dispatch(runnable.forget());
            if (NS_WARN_IF(NS_FAILED(rv))) {
                OperationCompleted(lock, rv);
            }

            return NS_OK;
        }

        // Async wait...
        if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
            rv = mAsyncInputStream->AsyncWait(this, 0, length, mTaskQueue);
            if (NS_WARN_IF(NS_FAILED(rv))) {
                OperationCompleted(lock, rv);
            }
            return NS_OK;
        }

        // Error.
        OperationCompleted(lock, rv);
        return NS_OK;
    }

    NS_IMETHOD
    OnInputStreamReady(nsIAsyncInputStream* aStream) override
    {
        MOZ_ASSERT(aStream == mAsyncInputStream);
        // The stream is ready, let's read it again.
        return Run();
    }

    // This function is called from the I/O thread and it will unblock the
    // owning thread.
    void
    OperationCompleted(MonitorAutoLock& aLock, nsresult aRv)
    {
        mAsyncResult = aRv;

        // This will unlock the owning thread.
        aLock.Notify();
    }

    bool
    MaybeExpandBufferSize()
    {
        MOZ_ASSERT(mCount == -1);

        if (mBufferSize >= mWrittenData + BUFFER_SIZE) {
            // The buffer is big enough.
            return true;
        }

        CheckedUint32 bufferSize =
            std::max<uint32_t>(static_cast<uint32_t>(mWrittenData),
                               BUFFER_SIZE);
        while (bufferSize.isValid() &&
               bufferSize.value() < mWrittenData + BUFFER_SIZE) {
            bufferSize *= 2;
        }

        if (!bufferSize.isValid()) {
            return false;
        }

        void* buffer = realloc(mBuffer, bufferSize.value());
        if (!buffer) {
            return false;
        }

        mBuffer = buffer;
        mBufferSize = bufferSize.value();
        return true;
    }

    Monitor mMonitor;

    nsCOMPtr<nsIInputStream> mInputStream;
    nsCOMPtr<nsIAsyncInputStream> mAsyncInputStream;

    RefPtr<TaskQueue> mTaskQueue;

    void* mBuffer;
    int64_t mCount;
    uint64_t mWrittenData;

    enum {
        // The buffer is allocated internally and this object must release it
        // in the DTOR if not stolen. The buffer can be reallocated.
        eInternal,

        // The buffer is not owned by this object and it cannot be reallocated.
        eExternal,
    } mBufferType;

    // The following set if needed for the async read.
    nsresult mAsyncResult;
    uint64_t mBufferSize;
    Atomic<bool> mCompleted;
};

NS_IMPL_ISUPPORTS_INHERITED(BufferWriter, Runnable, nsIInputStreamCallback)

} // anonymous namespace

nsresult
NS_ReadInputStreamToBuffer(nsIInputStream* aInputStream,
                           void** aDest,
                           int64_t aCount,
                           uint64_t* aWritten)
{
    MOZ_ASSERT(aInputStream);
    MOZ_ASSERT(aCount >= -1);

    uint64_t dummyWritten;
    if (!aWritten) {
        aWritten = &dummyWritten;
    }

    if (aCount == 0) {
        *aWritten = 0;
        return NS_OK;
    }

    // This will take care of allocating and reallocating aDest.
    RefPtr<BufferWriter> writer =
       new BufferWriter(aInputStream, *aDest, aCount);

    nsresult rv = writer->Write();
    NS_ENSURE_SUCCESS(rv, rv);

    *aWritten = writer->WrittenData();

    if (!*aDest) {
        *aDest = writer->StealBuffer();
    }

    return NS_OK;
}

nsresult
NS_ReadInputStreamToString(nsIInputStream* aInputStream,
                           nsACString& aDest,
                           int64_t aCount,
                           uint64_t* aWritten)
{
    uint64_t dummyWritten;
    if (!aWritten) {
        aWritten = &dummyWritten;
    }

    // Nothing to do if aCount is 0.
    if (aCount == 0) {
        aDest.Truncate();
        *aWritten = 0;
        return NS_OK;
    }

    // If we have the size, we can pre-allocate the buffer.
    if (aCount > 0) {
        if (NS_WARN_IF(aCount  >= INT32_MAX) ||
            NS_WARN_IF(!aDest.SetLength(aCount, mozilla::fallible))) {
            return NS_ERROR_OUT_OF_MEMORY;
        }

        void* dest = aDest.BeginWriting();
        nsresult rv = NS_ReadInputStreamToBuffer(aInputStream, &dest, aCount,
                                                 aWritten);
        NS_ENSURE_SUCCESS(rv, rv);

       if ((uint64_t)aCount > *aWritten) {
           aDest.Truncate(*aWritten);
       }

       return NS_OK;
    }

    // If the size is unknown, BufferWriter will allocate the buffer.
    void* dest = nullptr;
    nsresult rv = NS_ReadInputStreamToBuffer(aInputStream, &dest, aCount,
                                             aWritten);
    MOZ_ASSERT_IF(NS_FAILED(rv), dest == nullptr);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!dest) {
      MOZ_ASSERT(*aWritten == 0);
      aDest.Truncate();
      return NS_OK;
    }

    aDest.Adopt(reinterpret_cast<char*>(dest), *aWritten);
    return NS_OK;
}

nsresult
NS_NewURI(nsIURI **result,
          const nsACString &spec,
          const char *charset /* = nullptr */,
          nsIURI *baseURI /* = nullptr */,
          nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    nsresult rv;
    nsCOMPtr<nsIIOService> grip;
    rv = net_EnsureIOService(&ioService, grip);
    if (ioService)
        rv = ioService->NewURI(spec, charset, baseURI, result);
    return rv;
}

nsresult
NS_NewURI(nsIURI **result,
          const nsACString &spec,
          NotNull<const Encoding*> encoding,
          nsIURI *baseURI /* = nullptr */,
          nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    nsAutoCString charset;
    encoding->Name(charset);
    return NS_NewURI(result, spec, charset.get(), baseURI, ioService);
}

nsresult
NS_NewURI(nsIURI **result,
          const nsAString &spec,
          const char *charset /* = nullptr */,
          nsIURI *baseURI /* = nullptr */,
          nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    return NS_NewURI(result, NS_ConvertUTF16toUTF8(spec), charset, baseURI, ioService);
}

nsresult
NS_NewURI(nsIURI **result,
          const nsAString &spec,
          NotNull<const Encoding*> encoding,
          nsIURI *baseURI /* = nullptr */,
          nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    return NS_NewURI(result, NS_ConvertUTF16toUTF8(spec), encoding, baseURI, ioService);
}

nsresult
NS_NewURI(nsIURI **result,
          const char *spec,
          nsIURI *baseURI /* = nullptr */,
          nsIIOService *ioService /* = nullptr */)     // pass in nsIIOService to optimize callers
{
    return NS_NewURI(result, nsDependentCString(spec), nullptr, baseURI, ioService);
}

nsresult
NS_GetSanitizedURIStringFromURI(nsIURI *aUri, nsAString &aSanitizedSpec)
{
    aSanitizedSpec.Truncate();

    nsCOMPtr<nsISensitiveInfoHiddenURI> safeUri = do_QueryInterface(aUri);
    nsAutoCString cSpec;
    nsresult rv;
    if (safeUri) {
        rv = safeUri->GetSensitiveInfoHiddenSpec(cSpec);
    } else {
        rv = aUri->GetSpec(cSpec);
    }

    if (NS_SUCCEEDED(rv)) {
        aSanitizedSpec.Assign(NS_ConvertUTF8toUTF16(cSpec));
    }
    return rv;
}

nsresult
NS_LoadPersistentPropertiesFromURISpec(nsIPersistentProperties **outResult,
                                       const nsACString         &aSpec)
{
    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), aSpec);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIChannel> channel;
    rv = NS_NewChannel(getter_AddRefs(channel),
                       uri,
                       nsContentUtils::GetSystemPrincipal(),
                       nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                       nsIContentPolicy::TYPE_OTHER);
    NS_ENSURE_SUCCESS(rv, rv);
    nsCOMPtr<nsIInputStream> in;
    rv = channel->Open2(getter_AddRefs(in));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIPersistentProperties> properties =
      do_CreateInstance(NS_PERSISTENTPROPERTIES_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
    rv = properties->Load(in);
    NS_ENSURE_SUCCESS(rv, rv);

    properties.swap(*outResult);
    return NS_OK;
}

bool
NS_UsePrivateBrowsing(nsIChannel *channel)
{
    OriginAttributes attrs;
    bool result = NS_GetOriginAttributes(channel, attrs);
    NS_ENSURE_TRUE(result, result);
    return attrs.mPrivateBrowsingId > 0;
}

bool
NS_GetOriginAttributes(nsIChannel *aChannel,
                       mozilla::OriginAttributes &aAttributes)
{
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
    // For some channels, they might not have loadInfo, like ExternalHelperAppParent..
    if (loadInfo) {
        loadInfo->GetOriginAttributes(&aAttributes);
    }

    bool isPrivate = false;
    nsCOMPtr<nsIPrivateBrowsingChannel> pbChannel = do_QueryInterface(aChannel);
    if (pbChannel) {
        nsresult rv = pbChannel->GetIsChannelPrivate(&isPrivate);
        NS_ENSURE_SUCCESS(rv, false);
    } else {
        // Some channels may not implement nsIPrivateBrowsingChannel
        nsCOMPtr<nsILoadContext> loadContext;
        NS_QueryNotificationCallbacks(aChannel, loadContext);
        isPrivate = loadContext && loadContext->UsePrivateBrowsing();
    }
    aAttributes.SyncAttributesWithPrivateBrowsing(isPrivate);
    return true;
}

bool
NS_HasBeenCrossOrigin(nsIChannel* aChannel, bool aReport)
{
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  MOZ_RELEASE_ASSERT(loadInfo, "Origin tracking only works for channels created with a loadinfo");

  if (!loadInfo) {
    return false;
  }

  // TYPE_DOCUMENT loads have a null LoadingPrincipal and can not be cross origin.
  if (!loadInfo->LoadingPrincipal()) {
    return false;
  }

  // Always treat tainted channels as cross-origin.
  if (loadInfo->GetTainting() != LoadTainting::Basic) {
    return true;
  }

  nsCOMPtr<nsIPrincipal> loadingPrincipal = loadInfo->LoadingPrincipal();
  uint32_t mode = loadInfo->GetSecurityMode();
  bool dataInherits =
    mode == nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_INHERITS ||
    mode == nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_INHERITS ||
    mode == nsILoadInfo::SEC_REQUIRE_CORS_DATA_INHERITS;

  bool aboutBlankInherits = dataInherits && loadInfo->GetAboutBlankInherits();

  for (nsIRedirectHistoryEntry* redirectHistoryEntry : loadInfo->RedirectChain()) {
    nsCOMPtr<nsIPrincipal> principal;
    redirectHistoryEntry->GetPrincipal(getter_AddRefs(principal));
    if (!principal) {
      return true;
    }

    nsCOMPtr<nsIURI> uri;
    principal->GetURI(getter_AddRefs(uri));
    if (!uri) {
      return true;
    }

    if (aboutBlankInherits && NS_IsAboutBlank(uri)) {
      continue;
    }

    if (NS_FAILED(loadingPrincipal->CheckMayLoad(uri, aReport, dataInherits))) {
      return true;
    }
  }

  nsCOMPtr<nsIURI> uri;
  NS_GetFinalChannelURI(aChannel, getter_AddRefs(uri));
  if (!uri) {
    return true;
  }

  if (aboutBlankInherits && NS_IsAboutBlank(uri)) {
    return false;
  }

  return NS_FAILED(loadingPrincipal->CheckMayLoad(uri, aReport, dataInherits));
}

bool
NS_IsSafeTopLevelNav(nsIChannel* aChannel)
{
  if (!aChannel) {
    return false;
  }
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (!loadInfo) {
    return false;
  }
  if (loadInfo->GetExternalContentPolicyType() != nsIContentPolicy::TYPE_DOCUMENT) {
    return false;
  }
  RefPtr<HttpBaseChannel> baseChan = do_QueryObject(aChannel);
  if (!baseChan) {
    return false;
  }
  nsHttpRequestHead *requestHead = baseChan->GetRequestHead();
  if (!requestHead) {
    return false;
  }
  return requestHead->IsSafeMethod();
}

bool NS_IsSameSiteForeign(nsIChannel* aChannel, nsIURI* aHostURI)
{
  if (!aChannel) {
    return false;
  }
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (!loadInfo) {
    return false;
  }

  // Do not treat loads triggered by web extensions as foreign
  nsCOMPtr<nsIURI> channelURI;
  NS_GetFinalChannelURI(aChannel, getter_AddRefs(channelURI));
  if (BasePrincipal::Cast(loadInfo->TriggeringPrincipal())->
        AddonAllowsLoad(channelURI)) {
    return false;
  }

  nsCOMPtr<nsIURI> uri;
  if (loadInfo->GetExternalContentPolicyType() == nsIContentPolicy::TYPE_DOCUMENT) {
    // for loads of TYPE_DOCUMENT we query the hostURI from the triggeringPricnipal
    // which returns the URI of the document that caused the navigation.
    loadInfo->TriggeringPrincipal()->GetURI(getter_AddRefs(uri));
  }
  else {
    uri = aHostURI;
  }

  nsCOMPtr<mozIThirdPartyUtil> thirdPartyUtil =
    do_GetService(THIRDPARTYUTIL_CONTRACTID);
  if (!thirdPartyUtil) {
    return false;
  }

  bool isForeign = true;
  nsresult rv = thirdPartyUtil->IsThirdPartyChannel(aChannel, uri, &isForeign);
  // if we are dealing with a cross origin request, we can return here
  // because we already know the request is 'foreign'.
  if (NS_FAILED(rv) || isForeign) {
    return true;
  }

  // for loads of TYPE_SUBDOCUMENT we have to perform an additional test, because
  // a cross-origin iframe might perform a navigation to a same-origin iframe which
  // would send same-site cookies. Hence, if the iframe navigation was triggered
  // by a cross-origin triggeringPrincipal, we treat the load as foreign.
  if (loadInfo->GetExternalContentPolicyType() == nsIContentPolicy::TYPE_SUBDOCUMENT) {
    nsCOMPtr<nsIURI> triggeringPrincipalURI;
    loadInfo->TriggeringPrincipal()->GetURI(getter_AddRefs(triggeringPrincipalURI));
    rv = thirdPartyUtil->IsThirdPartyChannel(aChannel, triggeringPrincipalURI, &isForeign);
    if (NS_FAILED(rv) || isForeign) {
      return true;
    }
  }

  // for the purpose of same-site cookies we have to treat any cross-origin
  // redirects as foreign. E.g. cross-site to same-site redirect is a problem
  // with regards to CSRF.

  nsCOMPtr<nsIPrincipal> redirectPrincipal;
  nsCOMPtr<nsIURI> redirectURI;
  for (nsIRedirectHistoryEntry* entry : loadInfo->RedirectChain()) {
    entry->GetPrincipal(getter_AddRefs(redirectPrincipal));
    if (redirectPrincipal) {
      redirectPrincipal->GetURI(getter_AddRefs(redirectURI));
      rv = thirdPartyUtil->IsThirdPartyChannel(aChannel, redirectURI, &isForeign);
      // if at any point we encounter a cross-origin redirect we can return.
      if (NS_FAILED(rv) || isForeign) {
        return true;
      }
    }
  }
  return isForeign;
}

bool
NS_ShouldCheckAppCache(nsIPrincipal *aPrincipal)
{
    uint32_t privateBrowsingId = 0;
    nsresult rv = aPrincipal->GetPrivateBrowsingId(&privateBrowsingId);
    if (NS_SUCCEEDED(rv) && (privateBrowsingId > 0)) {
        return false;
    }

    nsCOMPtr<nsIOfflineCacheUpdateService> offlineService =
        do_GetService("@mozilla.org/offlinecacheupdate-service;1");
    if (!offlineService) {
        return false;
    }

    bool allowed;
    rv = offlineService->OfflineAppAllowed(aPrincipal, nullptr, &allowed);
    return NS_SUCCEEDED(rv) && allowed;
}

void
NS_WrapAuthPrompt(nsIAuthPrompt   *aAuthPrompt,
                  nsIAuthPrompt2 **aAuthPrompt2)
{
    nsCOMPtr<nsIAuthPromptAdapterFactory> factory =
        do_GetService(NS_AUTHPROMPT_ADAPTER_FACTORY_CONTRACTID);
    if (!factory)
        return;

    NS_WARNING("Using deprecated nsIAuthPrompt");
    factory->CreateAdapter(aAuthPrompt, aAuthPrompt2);
}

void
NS_QueryAuthPrompt2(nsIInterfaceRequestor  *aCallbacks,
                    nsIAuthPrompt2        **aAuthPrompt)
{
    CallGetInterface(aCallbacks, aAuthPrompt);
    if (*aAuthPrompt)
        return;

    // Maybe only nsIAuthPrompt is provided and we have to wrap it.
    nsCOMPtr<nsIAuthPrompt> prompt(do_GetInterface(aCallbacks));
    if (!prompt)
        return;

    NS_WrapAuthPrompt(prompt, aAuthPrompt);
}

void
NS_QueryAuthPrompt2(nsIChannel      *aChannel,
                    nsIAuthPrompt2 **aAuthPrompt)
{
    *aAuthPrompt = nullptr;

    // We want to use any auth prompt we can find on the channel's callbacks,
    // and if that fails use the loadgroup's prompt (if any)
    // Therefore, we can't just use NS_QueryNotificationCallbacks, because
    // that would prefer a loadgroup's nsIAuthPrompt2 over a channel's
    // nsIAuthPrompt.
    nsCOMPtr<nsIInterfaceRequestor> callbacks;
    aChannel->GetNotificationCallbacks(getter_AddRefs(callbacks));
    if (callbacks) {
        NS_QueryAuthPrompt2(callbacks, aAuthPrompt);
        if (*aAuthPrompt)
            return;
    }

    nsCOMPtr<nsILoadGroup> group;
    aChannel->GetLoadGroup(getter_AddRefs(group));
    if (!group)
        return;

    group->GetNotificationCallbacks(getter_AddRefs(callbacks));
    if (!callbacks)
        return;
    NS_QueryAuthPrompt2(callbacks, aAuthPrompt);
}

nsresult
NS_NewNotificationCallbacksAggregation(nsIInterfaceRequestor  *callbacks,
                                       nsILoadGroup           *loadGroup,
                                       nsIEventTarget         *target,
                                       nsIInterfaceRequestor **result)
{
    nsCOMPtr<nsIInterfaceRequestor> cbs;
    if (loadGroup)
        loadGroup->GetNotificationCallbacks(getter_AddRefs(cbs));
    return NS_NewInterfaceRequestorAggregation(callbacks, cbs, target, result);
}

nsresult
NS_NewNotificationCallbacksAggregation(nsIInterfaceRequestor  *callbacks,
                                       nsILoadGroup           *loadGroup,
                                       nsIInterfaceRequestor **result)
{
    return NS_NewNotificationCallbacksAggregation(callbacks, loadGroup, nullptr, result);
}

nsresult
NS_DoImplGetInnermostURI(nsINestedURI *nestedURI, nsIURI **result)
{
    MOZ_ASSERT(nestedURI, "Must have a nested URI!");
    MOZ_ASSERT(!*result, "Must have null *result");

    nsCOMPtr<nsIURI> inner;
    nsresult rv = nestedURI->GetInnerURI(getter_AddRefs(inner));
    NS_ENSURE_SUCCESS(rv, rv);

    // We may need to loop here until we reach the innermost
    // URI.
    nsCOMPtr<nsINestedURI> nestedInner(do_QueryInterface(inner));
    while (nestedInner) {
        rv = nestedInner->GetInnerURI(getter_AddRefs(inner));
        NS_ENSURE_SUCCESS(rv, rv);
        nestedInner = do_QueryInterface(inner);
    }

    // Found the innermost one if we reach here.
    inner.swap(*result);

    return rv;
}

nsresult
NS_ImplGetInnermostURI(nsINestedURI *nestedURI, nsIURI **result)
{
    // Make it safe to use swap()
    *result = nullptr;

    return NS_DoImplGetInnermostURI(nestedURI, result);
}

already_AddRefed<nsIURI>
NS_GetInnermostURI(nsIURI *aURI)
{
    MOZ_ASSERT(aURI, "Must have URI");

    nsCOMPtr<nsIURI> uri = aURI;

    nsCOMPtr<nsINestedURI> nestedURI(do_QueryInterface(uri));
    if (!nestedURI) {
        return uri.forget();
    }

    nsresult rv = nestedURI->GetInnermostURI(getter_AddRefs(uri));
    if (NS_FAILED(rv)) {
        return nullptr;
    }

    return uri.forget();
}

nsresult
NS_GetFinalChannelURI(nsIChannel *channel, nsIURI **uri)
{
    *uri = nullptr;

    nsCOMPtr<nsILoadInfo> loadInfo = channel->GetLoadInfo();
    if (loadInfo) {
        nsCOMPtr<nsIURI> resultPrincipalURI;
        loadInfo->GetResultPrincipalURI(getter_AddRefs(resultPrincipalURI));
        if (resultPrincipalURI) {
            resultPrincipalURI.forget(uri);
            return NS_OK;
        }
    }

    return channel->GetOriginalURI(uri);
}

nsresult
NS_URIChainHasFlags(nsIURI   *uri,
                    uint32_t  flags,
                    bool     *result)
{
    nsresult rv;
    nsCOMPtr<nsINetUtil> util = do_GetNetUtil(&rv);
    NS_ENSURE_SUCCESS(rv, rv);

    return util->URIChainHasFlags(uri, flags, result);
}

uint32_t
NS_SecurityHashURI(nsIURI *aURI)
{
    nsCOMPtr<nsIURI> baseURI = NS_GetInnermostURI(aURI);

    nsAutoCString scheme;
    uint32_t schemeHash = 0;
    if (NS_SUCCEEDED(baseURI->GetScheme(scheme)))
        schemeHash = mozilla::HashString(scheme);

    // TODO figure out how to hash file:// URIs
    if (scheme.EqualsLiteral("file"))
        return schemeHash; // sad face

#if IS_ORIGIN_IS_FULL_SPEC_DEFINED
    bool hasFlag;
    if (NS_FAILED(NS_URIChainHasFlags(baseURI,
        nsIProtocolHandler::ORIGIN_IS_FULL_SPEC, &hasFlag)) ||
        hasFlag)
    {
        nsAutoCString spec;
        uint32_t specHash;
        nsresult res = baseURI->GetSpec(spec);
        if (NS_SUCCEEDED(res))
            specHash = mozilla::HashString(spec);
        else
            specHash = static_cast<uint32_t>(res);
        return specHash;
    }
#endif

    nsAutoCString host;
    uint32_t hostHash = 0;
    if (NS_SUCCEEDED(baseURI->GetAsciiHost(host)))
        hostHash = mozilla::HashString(host);

    return mozilla::AddToHash(schemeHash, hostHash, NS_GetRealPort(baseURI));
}

bool
NS_SecurityCompareURIs(nsIURI *aSourceURI,
                       nsIURI *aTargetURI,
                       bool    aStrictFileOriginPolicy)
{
    // Note that this is not an Equals() test on purpose -- for URIs that don't
    // support host/port, we want equality to basically be object identity, for
    // security purposes.  Otherwise, for example, two javascript: URIs that
    // are otherwise unrelated could end up "same origin", which would be
    // unfortunate.
    if (aSourceURI && aSourceURI == aTargetURI)
    {
        return true;
    }

    if (!aTargetURI || !aSourceURI)
    {
        return false;
    }

    // If either URI is a nested URI, get the base URI
    nsCOMPtr<nsIURI> sourceBaseURI = NS_GetInnermostURI(aSourceURI);
    nsCOMPtr<nsIURI> targetBaseURI = NS_GetInnermostURI(aTargetURI);

    // If either uri is an nsIURIWithPrincipal
    nsCOMPtr<nsIURIWithPrincipal> uriPrinc = do_QueryInterface(sourceBaseURI);
    if (uriPrinc) {
        uriPrinc->GetPrincipalUri(getter_AddRefs(sourceBaseURI));
    }

    uriPrinc = do_QueryInterface(targetBaseURI);
    if (uriPrinc) {
        uriPrinc->GetPrincipalUri(getter_AddRefs(targetBaseURI));
    }

    if (!sourceBaseURI || !targetBaseURI)
        return false;

    // Compare schemes
    nsAutoCString targetScheme;
    bool sameScheme = false;
    if (NS_FAILED( targetBaseURI->GetScheme(targetScheme) ) ||
        NS_FAILED( sourceBaseURI->SchemeIs(targetScheme.get(), &sameScheme) ) ||
        !sameScheme)
    {
        // Not same-origin if schemes differ
        return false;
    }

    // For file scheme, reject unless the files are identical. See
    // NS_RelaxStrictFileOriginPolicy for enforcing file same-origin checking
    if (targetScheme.EqualsLiteral("file"))
    {
        // in traditional unsafe behavior all files are the same origin
        if (!aStrictFileOriginPolicy)
            return true;

        nsCOMPtr<nsIFileURL> sourceFileURL(do_QueryInterface(sourceBaseURI));
        nsCOMPtr<nsIFileURL> targetFileURL(do_QueryInterface(targetBaseURI));

        if (!sourceFileURL || !targetFileURL)
            return false;

        nsCOMPtr<nsIFile> sourceFile, targetFile;

        sourceFileURL->GetFile(getter_AddRefs(sourceFile));
        targetFileURL->GetFile(getter_AddRefs(targetFile));

        if (!sourceFile || !targetFile)
            return false;

        // Otherwise they had better match
        bool filesAreEqual = false;
        nsresult rv = sourceFile->Equals(targetFile, &filesAreEqual);
        return NS_SUCCEEDED(rv) && filesAreEqual;
    }

#if IS_ORIGIN_IS_FULL_SPEC_DEFINED
    bool hasFlag;
    if (NS_FAILED(NS_URIChainHasFlags(targetBaseURI,
        nsIProtocolHandler::ORIGIN_IS_FULL_SPEC, &hasFlag)) ||
        hasFlag)
    {
        // URIs with this flag have the whole spec as a distinct trust
        // domain; use the whole spec for comparison
        nsAutoCString targetSpec;
        nsAutoCString sourceSpec;
        return ( NS_SUCCEEDED( targetBaseURI->GetSpec(targetSpec) ) &&
                 NS_SUCCEEDED( sourceBaseURI->GetSpec(sourceSpec) ) &&
                 targetSpec.Equals(sourceSpec) );
    }
#endif

    // Compare hosts
    nsAutoCString targetHost;
    nsAutoCString sourceHost;
    if (NS_FAILED( targetBaseURI->GetAsciiHost(targetHost) ) ||
        NS_FAILED( sourceBaseURI->GetAsciiHost(sourceHost) ))
    {
        return false;
    }

    nsCOMPtr<nsIStandardURL> targetURL(do_QueryInterface(targetBaseURI));
    nsCOMPtr<nsIStandardURL> sourceURL(do_QueryInterface(sourceBaseURI));
    if (!targetURL || !sourceURL)
    {
        return false;
    }

    if (!targetHost.Equals(sourceHost, nsCaseInsensitiveCStringComparator() ))
    {
        return false;
    }

    return NS_GetRealPort(targetBaseURI) == NS_GetRealPort(sourceBaseURI);
}

bool
NS_URIIsLocalFile(nsIURI *aURI)
{
  nsCOMPtr<nsINetUtil> util = do_GetNetUtil();

  bool isFile;
  return util && NS_SUCCEEDED(util->ProtocolHasFlags(aURI,
                                nsIProtocolHandler::URI_IS_LOCAL_FILE,
                                &isFile)) &&
         isFile;
}

bool
NS_RelaxStrictFileOriginPolicy(nsIURI *aTargetURI,
                               nsIURI *aSourceURI,
                               bool aAllowDirectoryTarget /* = false */)
{
  if (!NS_URIIsLocalFile(aTargetURI)) {
    // This is probably not what the caller intended
    NS_NOTREACHED("NS_RelaxStrictFileOriginPolicy called with non-file URI");
    return false;
  }

  if (!NS_URIIsLocalFile(aSourceURI)) {
    // If the source is not also a file: uri then forget it
    // (don't want resource: principals in a file: doc)
    //
    // note: we're not de-nesting jar: uris here, we want to
    // keep archive content bottled up in its own little island
    return false;
  }

  //
  // pull out the internal files
  //
  nsCOMPtr<nsIFileURL> targetFileURL(do_QueryInterface(aTargetURI));
  nsCOMPtr<nsIFileURL> sourceFileURL(do_QueryInterface(aSourceURI));
  nsCOMPtr<nsIFile> targetFile;
  nsCOMPtr<nsIFile> sourceFile;
  bool targetIsDir;

  // Make sure targetFile is not a directory (bug 209234)
  // and that it exists w/out unescaping (bug 395343)
  if (!sourceFileURL || !targetFileURL ||
      NS_FAILED(targetFileURL->GetFile(getter_AddRefs(targetFile))) ||
      NS_FAILED(sourceFileURL->GetFile(getter_AddRefs(sourceFile))) ||
      !targetFile || !sourceFile ||
      NS_FAILED(targetFile->Normalize()) ||
#ifndef MOZ_WIDGET_ANDROID
      NS_FAILED(sourceFile->Normalize()) ||
#endif
      (!aAllowDirectoryTarget &&
       (NS_FAILED(targetFile->IsDirectory(&targetIsDir)) || targetIsDir))) {
    return false;
  }

  //
  // If the file to be loaded is in a subdirectory of the source
  // (or same-dir if source is not a directory) then it will
  // inherit its source principal and be scriptable by that source.
  //
  bool sourceIsDir;
  bool allowed = false;
  nsresult rv = sourceFile->IsDirectory(&sourceIsDir);
  if (NS_SUCCEEDED(rv) && sourceIsDir) {
    rv = sourceFile->Contains(targetFile, &allowed);
  } else {
    nsCOMPtr<nsIFile> sourceParent;
    rv = sourceFile->GetParent(getter_AddRefs(sourceParent));
    if (NS_SUCCEEDED(rv) && sourceParent) {
      rv = sourceParent->Equals(targetFile, &allowed);
      if (NS_FAILED(rv) || !allowed) {
        rv = sourceParent->Contains(targetFile, &allowed);
      } else {
        MOZ_ASSERT(aAllowDirectoryTarget,
                   "sourceFile->Parent == targetFile, but targetFile "
                   "should've been disallowed if it is a directory");
      }
    }
  }

  if (NS_SUCCEEDED(rv) && allowed) {
    return true;
  }

  return false;
}

bool
NS_IsInternalSameURIRedirect(nsIChannel *aOldChannel,
                             nsIChannel *aNewChannel,
                             uint32_t aFlags)
{
  if (!(aFlags & nsIChannelEventSink::REDIRECT_INTERNAL)) {
    return false;
  }

  nsCOMPtr<nsIURI> oldURI, newURI;
  aOldChannel->GetURI(getter_AddRefs(oldURI));
  aNewChannel->GetURI(getter_AddRefs(newURI));

  if (!oldURI || !newURI) {
    return false;
  }

  bool res;
  return NS_SUCCEEDED(oldURI->Equals(newURI, &res)) && res;
}

bool
NS_IsHSTSUpgradeRedirect(nsIChannel *aOldChannel,
                         nsIChannel *aNewChannel,
                         uint32_t aFlags)
{
  if (!(aFlags & nsIChannelEventSink::REDIRECT_STS_UPGRADE)) {
    return false;
  }

  nsCOMPtr<nsIURI> oldURI, newURI;
  aOldChannel->GetURI(getter_AddRefs(oldURI));
  aNewChannel->GetURI(getter_AddRefs(newURI));

  if (!oldURI || !newURI) {
    return false;
  }

  bool isHttp;
  if (NS_FAILED(oldURI->SchemeIs("http", &isHttp)) || !isHttp) {
    return false;
  }

  nsCOMPtr<nsIURI> upgradedURI;
  nsresult rv = NS_GetSecureUpgradedURI(oldURI, getter_AddRefs(upgradedURI));
  if (NS_FAILED(rv)) {
    return false;
  }

  bool res;
  return NS_SUCCEEDED(upgradedURI->Equals(newURI, &res)) && res;
}

nsresult
NS_LinkRedirectChannels(uint32_t channelId,
                        nsIParentChannel *parentChannel,
                        nsIChannel **_result)
{
  nsresult rv;

  nsCOMPtr<nsIRedirectChannelRegistrar> registrar =
      do_GetService("@mozilla.org/redirectchannelregistrar;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  return registrar->LinkChannels(channelId,
                                 parentChannel,
                                 _result);
}

#define NS_FAKE_SCHEME "http://"
#define NS_FAKE_TLD ".invalid"
nsresult NS_MakeRandomInvalidURLString(nsCString &result)
{
  nsresult rv;
  nsCOMPtr<nsIUUIDGenerator> uuidgen =
    do_GetService("@mozilla.org/uuid-generator;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsID idee;
  rv = uuidgen->GenerateUUIDInPlace(&idee);
  NS_ENSURE_SUCCESS(rv, rv);

  char chars[NSID_LENGTH];
  idee.ToProvidedString(chars);

  result.AssignLiteral(NS_FAKE_SCHEME);
  // Strip off the '{' and '}' at the beginning and end of the UUID
  result.Append(chars + 1, NSID_LENGTH - 3);
  result.AppendLiteral(NS_FAKE_TLD);

  return NS_OK;
}
#undef NS_FAKE_SCHEME
#undef NS_FAKE_TLD

nsresult NS_MaybeOpenChannelUsingOpen2(nsIChannel* aChannel,
                                       nsIInputStream **aStream)
{
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (loadInfo && loadInfo->GetSecurityMode() != 0) {
    return aChannel->Open2(aStream);
  }
  return aChannel->Open(aStream);
}

nsresult NS_MaybeOpenChannelUsingAsyncOpen2(nsIChannel* aChannel,
                                            nsIStreamListener *aListener)
{
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->GetLoadInfo();
  if (loadInfo && loadInfo->GetSecurityMode() != 0) {
    return aChannel->AsyncOpen2(aListener);
  }
  return aChannel->AsyncOpen(aListener, nullptr);
}

nsresult
NS_CheckIsJavaCompatibleURLString(nsCString &urlString, bool *result)
{
  *result = false; // Default to "no"

  nsresult rv = NS_OK;
  nsCOMPtr<nsIURLParser> urlParser =
    do_GetService(NS_STDURLPARSER_CONTRACTID, &rv);
  if (NS_FAILED(rv) || !urlParser)
    return NS_ERROR_FAILURE;

  bool compatible = true;
  uint32_t schemePos = 0;
  int32_t schemeLen = 0;
  urlParser->ParseURL(urlString.get(), -1, &schemePos, &schemeLen,
                      nullptr, nullptr, nullptr, nullptr);
  if (schemeLen != -1) {
    nsCString scheme;
    scheme.Assign(urlString.get() + schemePos, schemeLen);
    // By default Java only understands a small number of URL schemes, and of
    // these only some can legitimately represent a browser page's "origin"
    // (and be something we can legitimately expect Java to handle ... or not
    // to mishandle).
    //
    // Besides those listed below, the OJI plugin understands the "jar",
    // "mailto", "netdoc", "javascript" and "rmi" schemes, and Java Plugin2
    // also understands the "about" scheme.  We actually pass "about" URLs
    // to Java ("about:blank" when processing a javascript: URL (one that
    // calls Java) from the location bar of a blank page, and (in FF4 and up)
    // "about:home" when processing a javascript: URL from the home page).
    // And Java doesn't appear to mishandle them (for example it doesn't allow
    // connections to "about" URLs).  But it doesn't make any sense to do
    // same-origin checks on "about" URLs, so we don't include them in our
    // scheme whitelist.
    //
    // The OJI plugin doesn't understand "chrome" URLs (only Java Plugin2
    // does) -- so we mustn't pass them to the OJI plugin.  But we do need to
    // pass "chrome" URLs to Java Plugin2:  Java Plugin2 grants additional
    // privileges to chrome "origins", and some extensions take advantage of
    // this.  For more information see bug 620773.
    //
    // As of FF4, we no longer support the OJI plugin.
    if (PL_strcasecmp(scheme.get(), "http") &&
        PL_strcasecmp(scheme.get(), "https") &&
        PL_strcasecmp(scheme.get(), "file") &&
        PL_strcasecmp(scheme.get(), "ftp") &&
        PL_strcasecmp(scheme.get(), "gopher") &&
        PL_strcasecmp(scheme.get(), "chrome"))
      compatible = false;
  } else {
    compatible = false;
  }

  *result = compatible;

  return NS_OK;
}

/** Given the first (disposition) token from a Content-Disposition header,
 * tell whether it indicates the content is inline or attachment
 * @param aDispToken the disposition token from the content-disposition header
 */
uint32_t
NS_GetContentDispositionFromToken(const nsAString &aDispToken)
{
  // RFC 2183, section 2.8 says that an unknown disposition
  // value should be treated as "attachment"
  // If all of these tests eval to false, then we have a content-disposition of
  // "attachment" or unknown
  if (aDispToken.IsEmpty() ||
      aDispToken.LowerCaseEqualsLiteral("inline") ||
      // Broken sites just send
      // Content-Disposition: filename="file"
      // without a disposition token... screen those out.
      StringHead(aDispToken, 8).LowerCaseEqualsLiteral("filename"))
    return nsIChannel::DISPOSITION_INLINE;

  return nsIChannel::DISPOSITION_ATTACHMENT;
}

uint32_t
NS_GetContentDispositionFromHeader(const nsACString &aHeader,
                                   nsIChannel *aChan /* = nullptr */)
{
  nsresult rv;
  nsCOMPtr<nsIMIMEHeaderParam> mimehdrpar = do_GetService(NS_MIMEHEADERPARAM_CONTRACTID, &rv);
  if (NS_FAILED(rv))
    return nsIChannel::DISPOSITION_ATTACHMENT;

  nsAutoString dispToken;
  rv = mimehdrpar->GetParameterHTTP(aHeader, "", EmptyCString(), true, nullptr,
                                    dispToken);

  if (NS_FAILED(rv)) {
    // special case (see bug 272541): empty disposition type handled as "inline"
    if (rv == NS_ERROR_FIRST_HEADER_FIELD_COMPONENT_EMPTY)
        return nsIChannel::DISPOSITION_INLINE;
    return nsIChannel::DISPOSITION_ATTACHMENT;
  }

  return NS_GetContentDispositionFromToken(dispToken);
}

nsresult
NS_GetFilenameFromDisposition(nsAString &aFilename,
                              const nsACString &aDisposition,
                              nsIURI *aURI /* = nullptr */)
{
  aFilename.Truncate();

  nsresult rv;
  nsCOMPtr<nsIMIMEHeaderParam> mimehdrpar =
      do_GetService(NS_MIMEHEADERPARAM_CONTRACTID, &rv);
  if (NS_FAILED(rv))
    return rv;

  // Get the value of 'filename' parameter
  rv = mimehdrpar->GetParameterHTTP(aDisposition, "filename",
                                    EmptyCString(), true, nullptr,
                                    aFilename);

  if (NS_FAILED(rv)) {
    aFilename.Truncate();
    return rv;
  }

  if (aFilename.IsEmpty())
    return NS_ERROR_NOT_AVAILABLE;

  return NS_OK;
}

void net_EnsurePSMInit()
{
    nsresult rv;
    nsCOMPtr<nsISupports> psm = do_GetService(PSM_COMPONENT_CONTRACTID, &rv);
    MOZ_ASSERT(NS_SUCCEEDED(rv));

    nsCOMPtr<nsISupports> sss = do_GetService(NS_SSSERVICE_CONTRACTID);
    nsCOMPtr<nsISupports> cbl = do_GetService(NS_CERTBLOCKLIST_CONTRACTID);
    nsCOMPtr<nsISupports> cos = do_GetService(NS_CERTOVERRIDE_CONTRACTID);
}

bool NS_IsAboutBlank(nsIURI *uri)
{
    // GetSpec can be expensive for some URIs, so check the scheme first.
    bool isAbout = false;
    if (NS_FAILED(uri->SchemeIs("about", &isAbout)) || !isAbout) {
        return false;
    }

    return uri->GetSpecOrDefault().EqualsLiteral("about:blank");
}

nsresult
NS_GenerateHostPort(const nsCString& host, int32_t port,
                    nsACString &hostLine)
{
    if (strchr(host.get(), ':')) {
        // host is an IPv6 address literal and must be encapsulated in []'s
        hostLine.Assign('[');
        // scope id is not needed for Host header.
        int scopeIdPos = host.FindChar('%');
        if (scopeIdPos == -1)
            hostLine.Append(host);
        else if (scopeIdPos > 0)
            hostLine.Append(Substring(host, 0, scopeIdPos));
        else
          return NS_ERROR_MALFORMED_URI;
        hostLine.Append(']');
    }
    else
        hostLine.Assign(host);
    if (port != -1) {
        hostLine.Append(':');
        hostLine.AppendInt(port);
    }
    return NS_OK;
}

void
NS_SniffContent(const char *aSnifferType, nsIRequest *aRequest,
                const uint8_t *aData, uint32_t aLength,
                nsACString &aSniffedType)
{
  typedef nsCategoryCache<nsIContentSniffer> ContentSnifferCache;
  extern ContentSnifferCache* gNetSniffers;
  extern ContentSnifferCache* gDataSniffers;
  ContentSnifferCache* cache = nullptr;
  if (!strcmp(aSnifferType, NS_CONTENT_SNIFFER_CATEGORY)) {
    if (!gNetSniffers) {
      gNetSniffers = new ContentSnifferCache(NS_CONTENT_SNIFFER_CATEGORY);
    }
    cache = gNetSniffers;
  } else if (!strcmp(aSnifferType, NS_DATA_SNIFFER_CATEGORY)) {
    if (!gDataSniffers) {
      gDataSniffers = new ContentSnifferCache(NS_DATA_SNIFFER_CATEGORY);
    }
    cache = gDataSniffers;
  } else {
    // Invalid content sniffer type was requested
    MOZ_ASSERT(false);
    return;
  }

  nsCOMArray<nsIContentSniffer> sniffers;
  cache->GetEntries(sniffers);
  for (int32_t i = 0; i < sniffers.Count(); ++i) {
    nsresult rv = sniffers[i]->GetMIMETypeFromContent(aRequest, aData, aLength, aSniffedType);
    if (NS_SUCCEEDED(rv) && !aSniffedType.IsEmpty()) {
      return;
    }
  }

  aSniffedType.Truncate();
}

bool
NS_IsSrcdocChannel(nsIChannel *aChannel)
{
  bool isSrcdoc;
  nsCOMPtr<nsIInputStreamChannel> isr = do_QueryInterface(aChannel);
  if (isr) {
    isr->GetIsSrcdocChannel(&isSrcdoc);
    return isSrcdoc;
  }
  nsCOMPtr<nsIViewSourceChannel> vsc = do_QueryInterface(aChannel);
  if (vsc) {
    nsresult rv = vsc->GetIsSrcdocChannel(&isSrcdoc);
    if (NS_SUCCEEDED(rv)) {
      return isSrcdoc;
    }
  }
  return false;
}

nsresult
NS_ShouldSecureUpgrade(nsIURI* aURI,
                       nsILoadInfo* aLoadInfo,
                       nsIPrincipal* aChannelResultPrincipal,
                       bool aPrivateBrowsing,
                       bool aAllowSTS,
                       const OriginAttributes& aOriginAttributes,
                       bool& aShouldUpgrade)
{
  // Even if we're in private browsing mode, we still enforce existing STS
  // data (it is read-only).
  // if the connection is not using SSL and either the exact host matches or
  // a superdomain wants to force HTTPS, do it.
  bool isHttps = false;
  nsresult rv = aURI->SchemeIs("https", &isHttps);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!isHttps) {
    if (aLoadInfo) {
      // If any of the documents up the chain to the root document makes use of
      // the CSP directive 'upgrade-insecure-requests', then it's time to fulfill
      // the promise to CSP and mixed content blocking to upgrade the channel
      // from http to https.
      if (aLoadInfo->GetUpgradeInsecureRequests() ||
          aLoadInfo->GetBrowserUpgradeInsecureRequests()) {
        // let's log a message to the console that we are upgrading a request
        nsAutoCString scheme;
        aURI->GetScheme(scheme);
        // append the additional 's' for security to the scheme :-)
        scheme.AppendASCII("s");
        NS_ConvertUTF8toUTF16 reportSpec(aURI->GetSpecOrDefault());
        NS_ConvertUTF8toUTF16 reportScheme(scheme);

        if (aLoadInfo->GetUpgradeInsecureRequests()) {
          const char16_t* params[] = { reportSpec.get(), reportScheme.get() };
          uint32_t innerWindowId = aLoadInfo->GetInnerWindowID();
          CSP_LogLocalizedStr("upgradeInsecureRequest",
                              params, ArrayLength(params),
                              EmptyString(), // aSourceFile
                              EmptyString(), // aScriptSample
                              0, // aLineNumber
                              0, // aColumnNumber
                              nsIScriptError::warningFlag, "CSP",
                              innerWindowId,
                              !!aLoadInfo->GetOriginAttributes().mPrivateBrowsingId);
          Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::CSP);
        } else {
          nsCOMPtr<nsIDocument> doc;
          nsINode* node = aLoadInfo->LoadingNode();
          if (node) {
            doc = node->OwnerDoc();
          }

          nsAutoString brandName;
          nsresult rv =
            nsContentUtils::GetLocalizedString(nsContentUtils::eBRAND_PROPERTIES,
                                               "brandShortName", brandName);
          if (NS_SUCCEEDED(rv)) {
            const char16_t* params[] = { brandName.get(), reportSpec.get(), reportScheme.get() };
            nsContentUtils::ReportToConsole(nsIScriptError::warningFlag,
                                            NS_LITERAL_CSTRING("DATA_URI_BLOCKED"),
                                            doc,
                                            nsContentUtils::eSECURITY_PROPERTIES,
                                            "BrowserUpgradeInsecureDisplayRequest",
                                            params, ArrayLength(params));
          }
          Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::BrowserDisplay);
        }

        aShouldUpgrade = true;
        return NS_OK;
      }
    }

    // enforce Strict-Transport-Security
    nsISiteSecurityService* sss = gHttpHandler->GetSSService();
    NS_ENSURE_TRUE(sss, NS_ERROR_OUT_OF_MEMORY);

    bool isStsHost = false;
    uint32_t hstsSource = 0;
    uint32_t flags = aPrivateBrowsing ? nsISocketProvider::NO_PERMANENT_STORAGE : 0;
    rv = sss->IsSecureURI(nsISiteSecurityService::HEADER_HSTS, aURI, flags,
                          aOriginAttributes, nullptr, &hstsSource, &isStsHost);

    // if the SSS check fails, it's likely because this load is on a
    // malformed URI or something else in the setup is wrong, so any error
    // should be reported.
    NS_ENSURE_SUCCESS(rv, rv);

    if (isStsHost) {
      LOG(("nsHttpChannel::Connect() STS permissions found\n"));
      if (aAllowSTS) {
        Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::STS);
        aShouldUpgrade = true;
        switch (hstsSource) {
          case nsISiteSecurityService::SOURCE_PRELOAD_LIST:
              Telemetry::Accumulate(Telemetry::HSTS_UPGRADE_SOURCE, 0);
              break;
          case nsISiteSecurityService::SOURCE_ORGANIC_REQUEST:
              Telemetry::Accumulate(Telemetry::HSTS_UPGRADE_SOURCE, 1);
              break;
          case nsISiteSecurityService::SOURCE_UNKNOWN:
          default:
              // record this as an organic request
              Telemetry::Accumulate(Telemetry::HSTS_UPGRADE_SOURCE, 1);
              break;
        }
        return NS_OK;
      }
      Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::PrefBlockedSTS);
    } else {
      Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::NoReasonToUpgrade);
    }
  } else {
    Telemetry::AccumulateCategorical(Telemetry::LABELS_HTTP_SCHEME_UPGRADE_TYPE::AlreadyHTTPS);
  }
  aShouldUpgrade = false;
  return NS_OK;
}

nsresult
NS_GetSecureUpgradedURI(nsIURI* aURI, nsIURI** aUpgradedURI)
{
  NS_MutateURI mutator(aURI);
  mutator.SetScheme(NS_LITERAL_CSTRING("https")); // Change the scheme to HTTPS:

  // Change the default port to 443:
  nsCOMPtr<nsIStandardURL> stdURL = do_QueryInterface(aURI);
  if (stdURL) {
    mutator.Apply(NS_MutatorMethod(&nsIStandardURLMutator::SetDefaultPort, 443, nullptr));
  } else {
    // If we don't have a nsStandardURL, fall back to using GetPort/SetPort.
    // XXXdholbert Is this function even called with a non-nsStandardURL arg,
    // in practice?
    NS_WARNING("Calling NS_GetSecureUpgradedURI for non nsStandardURL");
    int32_t oldPort = -1;
    nsresult rv = aURI->GetPort(&oldPort);
    if (NS_FAILED(rv)) return rv;

    // Keep any nonstandard ports so only the scheme is changed.
    // For example:
    //  http://foo.com:80 -> https://foo.com:443
    //  http://foo.com:81 -> https://foo.com:81

    if (oldPort == 80 || oldPort == -1) {
        mutator.SetPort(-1);
    } else {
        mutator.SetPort(oldPort);
    }
  }

  return mutator.Finalize(aUpgradedURI);
}

nsresult
NS_CompareLoadInfoAndLoadContext(nsIChannel *aChannel)
{
  nsCOMPtr<nsILoadInfo> loadInfo;
  aChannel->GetLoadInfo(getter_AddRefs(loadInfo));

  nsCOMPtr<nsILoadContext> loadContext;
  NS_QueryNotificationCallbacks(aChannel, loadContext);
  if (!loadInfo || !loadContext) {
    return NS_OK;
  }

  // We try to skip about:newtab.
  // about:newtab will use SystemPrincipal to download thumbnails through
  // https:// and blob URLs.
  bool isAboutPage = false;
  nsINode* node = loadInfo->LoadingNode();
  if (node) {
    nsIDocument* doc = node->OwnerDoc();
    if (doc) {
      nsIURI* uri = doc->GetDocumentURI();
      nsresult rv = uri->SchemeIs("about", &isAboutPage);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  if (isAboutPage) {
    return NS_OK;
  }

  // We skip the favicon loading here. The favicon loading might be
  // triggered by the XUL image. For that case, the loadContext will have
  // default originAttributes since the XUL image uses SystemPrincipal, but
  // the loadInfo will use originAttributes from the content. Thus, the
  // originAttributes between loadInfo and loadContext will be different.
  // That's why we have to skip the comparison for the favicon loading.
  if (nsContentUtils::IsSystemPrincipal(loadInfo->LoadingPrincipal()) &&
      loadInfo->InternalContentPolicyType() ==
        nsIContentPolicy::TYPE_INTERNAL_IMAGE_FAVICON) {
    return NS_OK;
  }

  bool loadContextIsInBE = false;
  nsresult rv = loadContext->GetIsInIsolatedMozBrowserElement(&loadContextIsInBE);
  if (NS_FAILED(rv)) {
    return NS_ERROR_UNEXPECTED;
  }

  OriginAttributes originAttrsLoadInfo = loadInfo->GetOriginAttributes();
  OriginAttributes originAttrsLoadContext;
  loadContext->GetOriginAttributes(originAttrsLoadContext);

  LOG(("NS_CompareLoadInfoAndLoadContext - loadInfo: %d, %d, %d; "
       "loadContext: %d %d, %d. [channel=%p]",
       originAttrsLoadInfo.mInIsolatedMozBrowser, originAttrsLoadInfo.mUserContextId,
       originAttrsLoadInfo.mPrivateBrowsingId, loadContextIsInBE,
       originAttrsLoadContext.mUserContextId, originAttrsLoadContext.mPrivateBrowsingId,
       aChannel));

  MOZ_ASSERT(originAttrsLoadInfo.mInIsolatedMozBrowser ==
             loadContextIsInBE,
             "The value of InIsolatedMozBrowser in the loadContext and in "
             "the loadInfo are not the same!");

  MOZ_ASSERT(originAttrsLoadInfo.mUserContextId ==
             originAttrsLoadContext.mUserContextId,
             "The value of mUserContextId in the loadContext and in the "
             "loadInfo are not the same!");

  MOZ_ASSERT(originAttrsLoadInfo.mPrivateBrowsingId ==
             originAttrsLoadContext.mPrivateBrowsingId,
             "The value of mPrivateBrowsingId in the loadContext and in the "
             "loadInfo are not the same!");

  return NS_OK;
}

uint32_t
NS_GetDefaultReferrerPolicy(bool privateBrowsing)
{
  static bool preferencesInitialized = false;

  if (!preferencesInitialized) {
    mozilla::Preferences::AddUintVarCache(&sDefaultRp,
                                          "network.http.referer.defaultPolicy",
                                          DEFAULT_RP);
    mozilla::Preferences::AddUintVarCache(&defaultPrivateRp,
                                          "network.http.referer.defaultPolicy.pbmode",
                                          DEFAULT_PRIVATE_RP);
    preferencesInitialized = true;
  }

  uint32_t defaultToUse;
  if (privateBrowsing) {
      defaultToUse = defaultPrivateRp;
  } else {
      defaultToUse = sDefaultRp;
  }

  switch (defaultToUse) {
    case 0:
      return nsIHttpChannel::REFERRER_POLICY_NO_REFERRER;
    case 1:
      return nsIHttpChannel::REFERRER_POLICY_SAME_ORIGIN;
    case 2:
      return nsIHttpChannel::REFERRER_POLICY_STRICT_ORIGIN_WHEN_XORIGIN;
  }

  return nsIHttpChannel::REFERRER_POLICY_NO_REFERRER_WHEN_DOWNGRADE;
}

bool
NS_IsOffline()
{
    bool offline = true;
    bool connectivity = true;
    nsCOMPtr<nsIIOService> ios = do_GetIOService();
    if (ios) {
        ios->GetOffline(&offline);
        ios->GetConnectivity(&connectivity);
    }
    return offline || !connectivity;
}

namespace mozilla {
namespace net {

bool
InScriptableRange(int64_t val)
{
    return (val <= kJS_MAX_SAFE_INTEGER) && (val >= kJS_MIN_SAFE_INTEGER);
}

bool
InScriptableRange(uint64_t val)
{
    return val <= kJS_MAX_SAFE_UINTEGER;
}

} // namespace net
} // namespace mozilla
