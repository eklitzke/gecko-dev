/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "base/basictypes.h"

#include "XPCModule.h"
#include "mozilla/ModuleUtils.h"
#include "nsImageModule.h"
#include "nsLayoutStatics.h"
#include "nsContentCID.h"
#include "nsContentDLF.h"
#include "nsContentPolicyUtils.h"
#include "nsDataDocumentContentPolicy.h"
#include "nsNoDataProtocolContentPolicy.h"
#include "nsDOMCID.h"
#include "nsFrameMessageManager.h"
#include "nsHTMLContentSerializer.h"
#include "nsHTMLParts.h"
#include "nsIComponentManager.h"
#include "nsIContentIterator.h"
#include "nsIContentSerializer.h"
#include "nsIContentViewer.h"
#include "nsIController.h"
#include "nsIControllers.h"
#include "nsIDocument.h"
#include "nsIDocumentEncoder.h"
#include "nsIFactory.h"
#include "nsIIdleService.h"
#include "nsHTMLStyleSheet.h"
#include "nsILayoutDebugger.h"
#include "nsNameSpaceManager.h"
#include "nsIObserver.h"
#include "nsIObserverService.h"
#include "nsIScriptError.h"
#include "nsCaret.h"
#include "nsPlainTextSerializer.h"
#include "nsXMLContentSerializer.h"
#include "nsXHTMLContentSerializer.h"
#include "nsContentAreaDragDrop.h"
#include "nsBox.h"
#include "nsIFrameTraversal.h"
#include "nsLayoutCID.h"
#include "nsStyleSheetService.h"
#include "nsFocusManager.h"
#include "ThirdPartyUtil.h"
#include "nsStructuredCloneContainer.h"
#include "gfxPlatform.h"
#include "mozilla/gfx/gfxVars.h"

#include "nsIEventListenerService.h"
#include "nsIMessageManager.h"

// Transformiix stuff
#include "mozilla/dom/XPathEvaluator.h"

// view stuff
#include "nsContentCreatorFunctions.h"

#include "nsHostObjectURI.h"
#include "nsGlobalWindowCommands.h"
#include "nsIControllerCommandTable.h"
#include "nsJSProtocolHandler.h"
#include "nsIControllerContext.h"
#include "nsZipArchive.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/DOMRequest.h"
#include "mozilla/dom/LocalStorageManager.h"
#include "mozilla/dom/network/UDPSocketChild.h"
#include "mozilla/dom/quota/QuotaManagerService.h"
#include "mozilla/dom/ServiceWorkerManager.h"
#include "mozilla/dom/SessionStorageManager.h"
#include "mozilla/dom/StorageActivityService.h"
#include "mozilla/dom/WorkerDebuggerManager.h"
#include "mozilla/dom/Notification.h"
#include "mozilla/OSFileConstants.h"
#include "mozilla/Services.h"

#ifdef MOZ_WEBSPEECH_TEST_BACKEND
#include "mozilla/dom/FakeSpeechRecognitionService.h"
#endif
#ifdef MOZ_WEBSPEECH
#include "mozilla/dom/nsSynthVoiceRegistry.h"
#endif

#include "mozilla/dom/PushNotifier.h"
using mozilla::dom::PushNotifier;
#define PUSHNOTIFIER_CID \
{ 0x2fc2d3e3, 0x020f, 0x404e, { 0xb0, 0x6a, 0x6e, 0xcf, 0x3e, 0xa2, 0x33, 0x4a } }

#include "AudioChannelAgent.h"
using mozilla::dom::AudioChannelAgent;

// Editor stuff
#include "nsEditorCID.h"
#include "mozilla/EditorController.h" //CID
#include "mozilla/HTMLEditor.h"

#include "nsScriptSecurityManager.h"
#include "ContentPrincipal.h"
#include "SystemPrincipal.h"
#include "NullPrincipal.h"
#include "nsNetCID.h"
#if defined(MOZ_WIDGET_ANDROID)
#include "nsHapticFeedback.h"
#endif
#include "nsParserUtils.h"

#include "nsHTMLCanvasFrame.h"

class nsIDocumentLoaderFactory;

#define PRODUCT_NAME "Gecko"

/* 0ddf4df8-4dbb-4133-8b79-9afb966514f5 */
#define NS_PLUGINDOCLOADERFACTORY_CID \
{ 0x0ddf4df8, 0x4dbb, 0x4133, { 0x8b, 0x79, 0x9a, 0xfb, 0x96, 0x65, 0x14, 0xf5 } }

#define NS_WINDOWCOMMANDTABLE_CID \
 { /* 0DE2FBFA-6B7F-11D7-BBBA-0003938A9D96 */        \
  0x0DE2FBFA, 0x6B7F, 0x11D7, {0xBB, 0xBA, 0x00, 0x03, 0x93, 0x8A, 0x9D, 0x96} }

#include "nsIBoxObject.h"
#include "inDeepTreeWalker.h"

#ifdef MOZ_XUL
#include "XULDocument.h"
#include "nsIXULSortService.h"
#endif

static void Shutdown();

#include "nsGeolocation.h"
#include "nsDeviceSensors.h"
#include "mozilla/dom/nsContentSecurityManager.h"
#include "mozilla/dom/nsCSPService.h"
#include "mozilla/dom/nsCSPContext.h"
#include "nsIPowerManagerService.h"
#include "nsIMediaManager.h"
#include "mozilla/dom/nsMixedContentBlocker.h"

#include "AudioChannelService.h"
#include "mozilla/net/WebSocketEventService.h"

#include "mozilla/dom/power/PowerManagerService.h"

#include "nsIPresentationService.h"

#include "MediaManager.h"

#include "GMPService.h"

#include "mozilla/dom/PresentationDeviceManager.h"
#include "mozilla/dom/PresentationTCPSessionTransport.h"

#include "nsScriptError.h"

#include "mozilla/TextInputProcessor.h"

using namespace mozilla;
using namespace mozilla::dom;
using mozilla::dom::power::PowerManagerService;
using mozilla::dom::quota::QuotaManagerService;
using mozilla::dom::WorkerDebuggerManager;
using mozilla::dom::UDPSocketChild;
using mozilla::gmp::GeckoMediaPluginService;
using mozilla::dom::NotificationTelemetryService;

#define NS_EDITORCOMMANDTABLE_CID \
{ 0x4f5e62b8, 0xd659, 0x4156, \
  { 0x84, 0xfc, 0x2f, 0x60, 0x99, 0x40, 0x03, 0x69 } }

#define NS_EDITINGCOMMANDTABLE_CID \
{ 0xcb38a746, 0xbeb8, 0x43f3, \
  { 0x94, 0x29, 0x77, 0x96, 0xe1, 0xa9, 0x3f, 0xb4 } }

#define NS_HAPTICFEEDBACK_CID \
{ 0x1f15dbc8, 0xbfaa, 0x45de, \
  { 0x8a, 0x46, 0x08, 0xe2, 0xe2, 0x63, 0x26, 0xb0 } }

NS_GENERIC_FACTORY_CONSTRUCTOR(TextEditor)

NS_GENERIC_FACTORY_CONSTRUCTOR(nsParserUtils)

NS_GENERIC_FACTORY_CONSTRUCTOR(HTMLEditor)

// PresentationDeviceManager
/* e1e79dec-4085-4994-ac5b-744b016697e6 */
#define PRESENTATION_DEVICE_MANAGER_CID \
{ 0xe1e79dec, 0x4085, 0x4994, { 0xac, 0x5b, 0x74, 0x4b, 0x01, 0x66, 0x97, 0xe6 } }

#define PRESENTATION_TCP_SESSION_TRANSPORT_CID \
{ 0xc9d023f4, 0x6228, 0x4c07, { 0x8b, 0x1d, 0x9c, 0x19, 0x57, 0x3f, 0xaa, 0x27 } }

already_AddRefed<nsIPresentationService> NS_CreatePresentationService();

// Factory Constructor
typedef nsHostObjectURI::Mutator nsHostObjectURIMutator;
NS_GENERIC_FACTORY_CONSTRUCTOR(nsHostObjectURIMutator)
NS_GENERIC_FACTORY_CONSTRUCTOR(LocalStorageManager)
NS_GENERIC_FACTORY_CONSTRUCTOR(SessionStorageManager)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(DOMRequestService,
                                         DOMRequestService::FactoryCreate)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(QuotaManagerService,
                                         QuotaManagerService::FactoryCreate)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(ServiceWorkerManager,
                                         ServiceWorkerManager::GetInstance)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(WorkerDebuggerManager,
                                         WorkerDebuggerManager::GetInstance)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(StorageActivityService,
                                         StorageActivityService::GetOrCreate)

#ifdef MOZ_WEBSPEECH
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(nsSynthVoiceRegistry,
                                         nsSynthVoiceRegistry::GetInstanceForService)
#endif

NS_GENERIC_FACTORY_CONSTRUCTOR(AudioChannelAgent)

NS_GENERIC_FACTORY_CONSTRUCTOR(nsDeviceSensors)

#if defined(ANDROID)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsHapticFeedback)
#endif
NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(ThirdPartyUtil, Init)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(nsIPowerManagerService,
                                         PowerManagerService::GetInstance)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(nsIMediaManagerService,
                                         MediaManager::GetInstance)
NS_GENERIC_FACTORY_CONSTRUCTOR(PresentationDeviceManager)
NS_GENERIC_FACTORY_CONSTRUCTOR(TextInputProcessor)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(nsIPresentationService,
                                         NS_CreatePresentationService)
NS_GENERIC_FACTORY_CONSTRUCTOR(PresentationTCPSessionTransport)
NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(NotificationTelemetryService, Init)
NS_GENERIC_FACTORY_CONSTRUCTOR(PushNotifier)

//-----------------------------------------------------------------------------

static bool gInitialized = false;

// Perform our one-time intialization for this module

static nsresult
Initialize()
{
  if (gInitialized) {
    MOZ_CRASH("Recursive layout module initialization");
    return NS_ERROR_FAILURE;
  }
  if (XRE_GetProcessType() == GeckoProcessType_GPU) {
    // We mark the layout module as being available in the GPU process so that
    // XPCOM's component manager initializes the power manager service, which
    // is needed for nsAppShell. However, we don't actually need anything in
    // the layout module itself.
    return NS_OK;
  }

  static_assert(sizeof(uintptr_t) == sizeof(void*),
                "Eeek! You'll need to adjust the size of uintptr_t to the "
                "size of a pointer on your platform.");

  gInitialized = true;

  nsresult rv;
  rv = xpcModuleCtor();
  if (NS_FAILED(rv))
    return rv;

  rv = nsLayoutStatics::Initialize();
  if (NS_FAILED(rv)) {
    Shutdown();
    return rv;
  }

  return NS_OK;
}

// Shutdown this module, releasing all of the module resources

// static
void
Shutdown()
{
  MOZ_ASSERT(gInitialized, "module not initialized");
  if (!gInitialized)
    return;

  gInitialized = false;

  nsLayoutStatics::Release();
}

#ifdef DEBUG
nsresult NS_NewLayoutDebugger(nsILayoutDebugger** aResult);
#endif

nsresult NS_NewBoxObject(nsIBoxObject** aResult);

#ifdef MOZ_XUL
nsresult NS_NewListBoxObject(nsIBoxObject** aResult);
nsresult NS_NewScrollBoxObject(nsIBoxObject** aResult);
nsresult NS_NewMenuBoxObject(nsIBoxObject** aResult);
nsresult NS_NewTreeBoxObject(nsIBoxObject** aResult);
#endif

nsresult NS_CreateFrameTraversal(nsIFrameTraversal** aResult);

already_AddRefed<nsIContentViewer> NS_NewContentViewer();
nsresult NS_NewContentDocumentLoaderFactory(nsIDocumentLoaderFactory** aResult);
nsresult NS_NewHTMLCopyTextEncoder(nsIDocumentEncoder** aResult);
nsresult NS_NewTextEncoder(nsIDocumentEncoder** aResult);
nsresult NS_NewContentPolicy(nsIContentPolicy** aResult);

nsresult NS_NewEventListenerService(nsIEventListenerService** aResult);
nsresult NS_NewGlobalMessageManager(nsISupports** aResult);
nsresult NS_NewParentProcessMessageManager(nsISupports** aResult);
nsresult NS_NewChildProcessMessageManager(nsISupports** aResult);

nsresult NS_NewXULControllers(nsISupports* aOuter, REFNSIID aIID, void** aResult);

#define MAKE_CTOR(ctor_, iface_, func_)                   \
static nsresult                                           \
ctor_(nsISupports* aOuter, REFNSIID aIID, void** aResult) \
{                                                         \
  *aResult = nullptr;                                      \
  if (aOuter)                                             \
    return NS_ERROR_NO_AGGREGATION;                       \
  iface_* inst;                                           \
  nsresult rv = func_(&inst);                             \
  if (NS_SUCCEEDED(rv)) {                                 \
    rv = inst->QueryInterface(aIID, aResult);             \
    NS_RELEASE(inst);                                     \
  }                                                       \
  return rv;                                              \
}

// As above, but expects
//   already_AddRefed<nsIFoo> NS_NewFoo();
// instead of
//   nsresult NS_NewFoo(nsIFoo**);
#define MAKE_CTOR2(ctor_, iface_, func_)                  \
static nsresult                                           \
ctor_(nsISupports* aOuter, REFNSIID aIID, void** aResult) \
{                                                         \
  *aResult = nullptr;                                      \
  if (aOuter) {                                           \
    return NS_ERROR_NO_AGGREGATION;                       \
  }                                                       \
  nsCOMPtr<iface_> inst = func_();                        \
  inst.forget(aResult);                                   \
  return NS_OK;                                           \
}

#ifdef DEBUG
MAKE_CTOR(CreateNewLayoutDebugger,        nsILayoutDebugger,           NS_NewLayoutDebugger)
#endif

MAKE_CTOR(CreateNewFrameTraversal,      nsIFrameTraversal,      NS_CreateFrameTraversal)
MAKE_CTOR(CreateNewBoxObject,           nsIBoxObject,           NS_NewBoxObject)

#ifdef MOZ_XUL
MAKE_CTOR(CreateNewListBoxObject,       nsIBoxObject,           NS_NewListBoxObject)
MAKE_CTOR(CreateNewMenuBoxObject,       nsIBoxObject,           NS_NewMenuBoxObject)
MAKE_CTOR(CreateNewScrollBoxObject,     nsIBoxObject,           NS_NewScrollBoxObject)
MAKE_CTOR(CreateNewTreeBoxObject,       nsIBoxObject,           NS_NewTreeBoxObject)
#endif // MOZ_XUL

NS_GENERIC_FACTORY_CONSTRUCTOR(inDeepTreeWalker)

MAKE_CTOR2(CreateContentViewer,           nsIContentViewer,            NS_NewContentViewer)
MAKE_CTOR(CreateHTMLDocument,             nsIDocument,                 NS_NewHTMLDocument)
MAKE_CTOR(CreateXMLDocument,              nsIDocument,                 NS_NewXMLDocument)
MAKE_CTOR(CreateSVGDocument,              nsIDocument,                 NS_NewSVGDocument)
MAKE_CTOR(CreateImageDocument,            nsIDocument,                 NS_NewImageDocument)
MAKE_CTOR2(CreateContentIterator,         nsIContentIterator,          NS_NewContentIterator)
MAKE_CTOR2(CreatePreContentIterator,      nsIContentIterator,          NS_NewPreContentIterator)
MAKE_CTOR2(CreateSubtreeIterator,         nsIContentIterator,          NS_NewContentSubtreeIterator)
MAKE_CTOR(CreateTextEncoder,              nsIDocumentEncoder,          NS_NewTextEncoder)
MAKE_CTOR(CreateHTMLCopyTextEncoder,      nsIDocumentEncoder,          NS_NewHTMLCopyTextEncoder)
MAKE_CTOR(CreateXMLContentSerializer,     nsIContentSerializer,        NS_NewXMLContentSerializer)
MAKE_CTOR(CreateHTMLContentSerializer,    nsIContentSerializer,        NS_NewHTMLContentSerializer)
MAKE_CTOR(CreateXHTMLContentSerializer,   nsIContentSerializer,        NS_NewXHTMLContentSerializer)
MAKE_CTOR(CreatePlainTextSerializer,      nsIContentSerializer,        NS_NewPlainTextSerializer)
MAKE_CTOR(CreateContentPolicy,            nsIContentPolicy,            NS_NewContentPolicy)
#ifdef MOZ_XUL
MAKE_CTOR(CreateXULSortService,           nsIXULSortService,           NS_NewXULSortService)
MAKE_CTOR(CreateXULDocument,              nsIDocument,                 NS_NewXULDocument)
// NS_NewXULControllers
#endif
MAKE_CTOR(CreateContentDLF,               nsIDocumentLoaderFactory,    NS_NewContentDocumentLoaderFactory)
MAKE_CTOR(CreateEventListenerService,     nsIEventListenerService,     NS_NewEventListenerService)
MAKE_CTOR(CreateGlobalMessageManager,     nsISupports,                 NS_NewGlobalMessageManager)
MAKE_CTOR(CreateParentMessageManager,     nsISupports,                 NS_NewParentProcessMessageManager)
MAKE_CTOR(CreateChildMessageManager,      nsISupports,                 NS_NewChildProcessMessageManager)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsDataDocumentContentPolicy)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsNoDataProtocolContentPolicy)
MAKE_CTOR(CreatePluginDocument,           nsIDocument,                 NS_NewPluginDocument)
MAKE_CTOR(CreateVideoDocument,            nsIDocument,                 NS_NewVideoDocument)
MAKE_CTOR(CreateFocusManager,             nsIFocusManager,      NS_NewFocusManager)

NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(nsStyleSheetService, Init)

typedef nsJSURI::Mutator nsJSURIMutator;
NS_GENERIC_FACTORY_CONSTRUCTOR(nsJSURIMutator)

// views are not refcounted, so this is the same as
// NS_GENERIC_FACTORY_CONSTRUCTOR without the NS_ADDREF/NS_RELEASE
#define NS_GENERIC_FACTORY_CONSTRUCTOR_NOREFS(_InstanceClass)                 \
static nsresult                                                               \
_InstanceClass##Constructor(nsISupports *aOuter, REFNSIID aIID,               \
                            void **aResult)                                   \
{                                                                             \
    nsresult rv;                                                              \
                                                                              \
    *aResult = nullptr;                                                       \
    if (nullptr != aOuter) {                                                  \
        rv = NS_ERROR_NO_AGGREGATION;                                         \
        return rv;                                                            \
    }                                                                         \
                                                                              \
    _InstanceClass * inst = new _InstanceClass();                             \
    if (nullptr == inst) {                                                    \
        rv = NS_ERROR_OUT_OF_MEMORY;                                          \
        return rv;                                                            \
    }                                                                         \
    rv = inst->QueryInterface(aIID, aResult);                                 \
                                                                              \
    return rv;                                                                \
}                                                                             \

#define NS_GEOLOCATION_CID \
  { 0x1E1C3FF, 0x94A, 0xD048, { 0x44, 0xB4, 0x62, 0xD2, 0x9C, 0x7B, 0x4F, 0x39 } }

NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(Geolocation, Init)

#define NS_GEOLOCATION_SERVICE_CID \
  { 0x404d02a, 0x1CA, 0xAAAB, { 0x47, 0x62, 0x94, 0x4b, 0x1b, 0xf2, 0xf7, 0xb5 } }

#define NS_AUDIOCHANNEL_SERVICE_CID \
  { 0xf712e983, 0x048a, 0x443f, { 0x88, 0x02, 0xfc, 0xc3, 0xd9, 0x27, 0xce, 0xac }}

#define NS_WEBSOCKETEVENT_SERVICE_CID \
  { 0x31689828, 0xda66, 0x49a6, { 0x87, 0x0c, 0xdf, 0x62, 0xb8, 0x3f, 0xe7, 0x89 }}

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(nsGeolocationService, nsGeolocationService::GetGeolocationService)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(AudioChannelService, AudioChannelService::GetOrCreate)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(WebSocketEventService, WebSocketEventService::GetOrCreate)

#ifdef MOZ_WEBSPEECH_TEST_BACKEND
NS_GENERIC_FACTORY_CONSTRUCTOR(FakeSpeechRecognitionService)
#endif

NS_GENERIC_FACTORY_CONSTRUCTOR(nsContentSecurityManager)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsCSPContext)
NS_GENERIC_FACTORY_CONSTRUCTOR(CSPService)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsMixedContentBlocker)

NS_GENERIC_FACTORY_CONSTRUCTOR(ContentPrincipal)
NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(SystemPrincipal,
    nsScriptSecurityManager::SystemPrincipalSingletonConstructor)
NS_GENERIC_FACTORY_CONSTRUCTOR_INIT(NullPrincipal, Init)
NS_GENERIC_FACTORY_CONSTRUCTOR(nsStructuredCloneContainer)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(OSFileConstantsService,
                                         OSFileConstantsService::GetOrCreate);

NS_GENERIC_FACTORY_CONSTRUCTOR(UDPSocketChild)

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(GeckoMediaPluginService, GeckoMediaPluginService::GetGeckoMediaPluginService)

NS_GENERIC_FACTORY_CONSTRUCTOR(nsScriptError)

#ifdef ACCESSIBILITY
#include "xpcAccessibilityService.h"

  MAKE_CTOR(CreateA11yService, nsIAccessibilityService, NS_GetAccessibilityService)
#endif

static nsresult
Construct_nsIScriptSecurityManager(nsISupports *aOuter, REFNSIID aIID,
                                   void **aResult)
{
    if (!aResult)
        return NS_ERROR_NULL_POINTER;
    *aResult = nullptr;
    if (aOuter)
        return NS_ERROR_NO_AGGREGATION;
    nsScriptSecurityManager *obj = nsScriptSecurityManager::GetScriptSecurityManager();
    if (!obj)
        return NS_ERROR_OUT_OF_MEMORY;
    if (NS_FAILED(obj->QueryInterface(aIID, aResult)))
        return NS_ERROR_FAILURE;
    return NS_OK;
}

#ifdef DEBUG
NS_DEFINE_NAMED_CID(NS_LAYOUT_DEBUGGER_CID);
#endif
NS_DEFINE_NAMED_CID(NS_FRAMETRAVERSAL_CID);
NS_DEFINE_NAMED_CID(NS_BOXOBJECT_CID);
#ifdef MOZ_XUL
NS_DEFINE_NAMED_CID(NS_LISTBOXOBJECT_CID);
NS_DEFINE_NAMED_CID(NS_MENUBOXOBJECT_CID);
NS_DEFINE_NAMED_CID(NS_SCROLLBOXOBJECT_CID);
NS_DEFINE_NAMED_CID(NS_TREEBOXOBJECT_CID);
#endif // MOZ_XUL
NS_DEFINE_NAMED_CID(IN_DEEPTREEWALKER_CID);
NS_DEFINE_NAMED_CID(NS_CONTENT_VIEWER_CID);
NS_DEFINE_NAMED_CID(NS_HTMLDOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_XMLDOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_SVGDOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_IMAGEDOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_CONTENTITERATOR_CID);
NS_DEFINE_NAMED_CID(NS_PRECONTENTITERATOR_CID);
NS_DEFINE_NAMED_CID(NS_SUBTREEITERATOR_CID);
NS_DEFINE_NAMED_CID(NS_TEXT_ENCODER_CID);
NS_DEFINE_NAMED_CID(NS_HTMLCOPY_TEXT_ENCODER_CID);
NS_DEFINE_NAMED_CID(NS_XMLCONTENTSERIALIZER_CID);
NS_DEFINE_NAMED_CID(NS_XHTMLCONTENTSERIALIZER_CID);
NS_DEFINE_NAMED_CID(NS_HTMLCONTENTSERIALIZER_CID);
NS_DEFINE_NAMED_CID(NS_PLAINTEXTSERIALIZER_CID);
NS_DEFINE_NAMED_CID(NS_PARSERUTILS_CID);
NS_DEFINE_NAMED_CID(NS_SCRIPTABLEUNESCAPEHTML_CID);
NS_DEFINE_NAMED_CID(NS_CONTENTPOLICY_CID);
NS_DEFINE_NAMED_CID(NS_DATADOCUMENTCONTENTPOLICY_CID);
NS_DEFINE_NAMED_CID(NS_NODATAPROTOCOLCONTENTPOLICY_CID);
NS_DEFINE_NAMED_CID(NS_XULCONTROLLERS_CID);
#ifdef MOZ_XUL
NS_DEFINE_NAMED_CID(NS_XULSORTSERVICE_CID);
NS_DEFINE_NAMED_CID(NS_XULDOCUMENT_CID);
#endif
NS_DEFINE_NAMED_CID(NS_CONTENT_DOCUMENT_LOADER_FACTORY_CID);
NS_DEFINE_NAMED_CID(NS_JSPROTOCOLHANDLER_CID);
NS_DEFINE_NAMED_CID(NS_JSURI_CID);
NS_DEFINE_NAMED_CID(NS_JSURIMUTATOR_CID);
NS_DEFINE_NAMED_CID(NS_WINDOWCOMMANDTABLE_CID);
NS_DEFINE_NAMED_CID(NS_WINDOWCONTROLLER_CID);
NS_DEFINE_NAMED_CID(NS_PLUGINDOCLOADERFACTORY_CID);
NS_DEFINE_NAMED_CID(NS_PLUGINDOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_VIDEODOCUMENT_CID);
NS_DEFINE_NAMED_CID(NS_STYLESHEETSERVICE_CID);
NS_DEFINE_NAMED_CID(NS_HOSTOBJECTURI_CID);
NS_DEFINE_NAMED_CID(NS_HOSTOBJECTURIMUTATOR_CID);
NS_DEFINE_NAMED_CID(NS_DOMSESSIONSTORAGEMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_DOMLOCALSTORAGEMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_TEXTEDITOR_CID);
NS_DEFINE_NAMED_CID(DOMREQUEST_SERVICE_CID);
NS_DEFINE_NAMED_CID(QUOTAMANAGER_SERVICE_CID);
NS_DEFINE_NAMED_CID(SERVICEWORKERMANAGER_CID);
NS_DEFINE_NAMED_CID(STORAGEACTIVITYSERVICE_CID);
NS_DEFINE_NAMED_CID(NOTIFICATIONTELEMETRYSERVICE_CID);
NS_DEFINE_NAMED_CID(PUSHNOTIFIER_CID);
NS_DEFINE_NAMED_CID(WORKERDEBUGGERMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_AUDIOCHANNELAGENT_CID);
NS_DEFINE_NAMED_CID(NS_HTMLEDITOR_CID);
NS_DEFINE_NAMED_CID(NS_EDITORCONTROLLER_CID);
NS_DEFINE_NAMED_CID(NS_EDITINGCONTROLLER_CID);
NS_DEFINE_NAMED_CID(NS_EDITORCOMMANDTABLE_CID);
NS_DEFINE_NAMED_CID(NS_EDITINGCOMMANDTABLE_CID);
NS_DEFINE_NAMED_CID(NS_GEOLOCATION_SERVICE_CID);
NS_DEFINE_NAMED_CID(NS_GEOLOCATION_CID);
NS_DEFINE_NAMED_CID(NS_AUDIOCHANNEL_SERVICE_CID);
NS_DEFINE_NAMED_CID(NS_WEBSOCKETEVENT_SERVICE_CID);
NS_DEFINE_NAMED_CID(NS_FOCUSMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_CONTENTSECURITYMANAGER_CID);
NS_DEFINE_NAMED_CID(CSPSERVICE_CID);
NS_DEFINE_NAMED_CID(NS_CSPCONTEXT_CID);
NS_DEFINE_NAMED_CID(NS_MIXEDCONTENTBLOCKER_CID);
NS_DEFINE_NAMED_CID(NS_EVENTLISTENERSERVICE_CID);
NS_DEFINE_NAMED_CID(NS_GLOBALMESSAGEMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_PARENTPROCESSMESSAGEMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_CHILDPROCESSMESSAGEMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_SCRIPTSECURITYMANAGER_CID);
NS_DEFINE_NAMED_CID(NS_PRINCIPAL_CID);
NS_DEFINE_NAMED_CID(NS_SYSTEMPRINCIPAL_CID);
NS_DEFINE_NAMED_CID(NS_NULLPRINCIPAL_CID);
NS_DEFINE_NAMED_CID(THIRDPARTYUTIL_CID);
NS_DEFINE_NAMED_CID(NS_STRUCTUREDCLONECONTAINER_CID);
NS_DEFINE_NAMED_CID(NS_DEVICE_SENSORS_CID);
#if defined(ANDROID)
NS_DEFINE_NAMED_CID(NS_HAPTICFEEDBACK_CID);
#endif
NS_DEFINE_NAMED_CID(NS_POWERMANAGERSERVICE_CID);
NS_DEFINE_NAMED_CID(OSFILECONSTANTSSERVICE_CID);
NS_DEFINE_NAMED_CID(UDPSOCKETCHILD_CID);
NS_DEFINE_NAMED_CID(NS_MEDIAMANAGERSERVICE_CID);
#ifdef MOZ_WEBSPEECH_TEST_BACKEND
NS_DEFINE_NAMED_CID(NS_FAKE_SPEECH_RECOGNITION_SERVICE_CID);
#endif
#ifdef MOZ_WEBSPEECH
NS_DEFINE_NAMED_CID(NS_SYNTHVOICEREGISTRY_CID);
#endif

#ifdef ACCESSIBILITY
NS_DEFINE_NAMED_CID(NS_ACCESSIBILITY_SERVICE_CID);
#endif

NS_DEFINE_NAMED_CID(GECKO_MEDIA_PLUGIN_SERVICE_CID);

NS_DEFINE_NAMED_CID(PRESENTATION_SERVICE_CID);
NS_DEFINE_NAMED_CID(PRESENTATION_DEVICE_MANAGER_CID);
NS_DEFINE_NAMED_CID(PRESENTATION_TCP_SESSION_TRANSPORT_CID);

NS_DEFINE_NAMED_CID(TEXT_INPUT_PROCESSOR_CID);

NS_DEFINE_NAMED_CID(NS_SCRIPTERROR_CID);

static nsresult
CreateWindowCommandTableConstructor(nsISupports *aOuter,
                                    REFNSIID aIID, void **aResult)
{
  nsresult rv;
  nsCOMPtr<nsIControllerCommandTable> commandTable =
      do_CreateInstance(NS_CONTROLLERCOMMANDTABLE_CONTRACTID, &rv);
  if (NS_FAILED(rv)) return rv;

  rv = nsWindowCommandRegistration::RegisterWindowCommands(commandTable);
  if (NS_FAILED(rv)) return rv;

  return commandTable->QueryInterface(aIID, aResult);
}

static nsresult
CreateWindowControllerWithSingletonCommandTable(nsISupports *aOuter,
                                      REFNSIID aIID, void **aResult)
{
  nsresult rv;
  nsCOMPtr<nsIController> controller =
       do_CreateInstance("@mozilla.org/embedcomp/base-command-controller;1", &rv);

 if (NS_FAILED(rv)) return rv;

  nsCOMPtr<nsIControllerCommandTable> windowCommandTable = do_GetService(kNS_WINDOWCOMMANDTABLE_CID, &rv);
  if (NS_FAILED(rv)) return rv;

  // this is a singleton; make it immutable
  windowCommandTable->MakeImmutable();

  nsCOMPtr<nsIControllerContext> controllerContext = do_QueryInterface(controller, &rv);
  if (NS_FAILED(rv)) return rv;

  controllerContext->Init(windowCommandTable);
  if (NS_FAILED(rv)) return rv;

  return controller->QueryInterface(aIID, aResult);
}

// Constructor of a controller which is set up to use, internally, a
// singleton command-table pre-filled with editor commands.
static nsresult
EditorControllerConstructor(nsISupports* aOuter, REFNSIID aIID, void** aResult)
{
  nsresult rv;
  nsCOMPtr<nsIController> controller = do_CreateInstance("@mozilla.org/embedcomp/base-command-controller;1", &rv);
  if (NS_FAILED(rv)) return rv;

  nsCOMPtr<nsIControllerCommandTable> editorCommandTable = do_GetService(kNS_EDITORCOMMANDTABLE_CID, &rv);
  if (NS_FAILED(rv)) return rv;

  // this guy is a singleton, so make it immutable
  editorCommandTable->MakeImmutable();

  nsCOMPtr<nsIControllerContext> controllerContext = do_QueryInterface(controller, &rv);
  if (NS_FAILED(rv)) return rv;

  rv = controllerContext->Init(editorCommandTable);
  if (NS_FAILED(rv)) return rv;

  return controller->QueryInterface(aIID, aResult);
}

// Constructor of a controller which is set up to use, internally, a
// singleton command-table pre-filled with editing commands.
static nsresult
nsEditingControllerConstructor(nsISupports *aOuter, REFNSIID aIID,
                                void **aResult)
{
  nsresult rv;
  nsCOMPtr<nsIController> controller = do_CreateInstance("@mozilla.org/embedcomp/base-command-controller;1", &rv);
  if (NS_FAILED(rv)) return rv;

  nsCOMPtr<nsIControllerCommandTable> editingCommandTable = do_GetService(kNS_EDITINGCOMMANDTABLE_CID, &rv);
  if (NS_FAILED(rv)) return rv;

  // this guy is a singleton, so make it immutable
  editingCommandTable->MakeImmutable();

  nsCOMPtr<nsIControllerContext> controllerContext = do_QueryInterface(controller, &rv);
  if (NS_FAILED(rv)) return rv;

  rv = controllerContext->Init(editingCommandTable);
  if (NS_FAILED(rv)) return rv;

  return controller->QueryInterface(aIID, aResult);
}

// Constructor for a command-table pre-filled with editor commands
static nsresult
nsEditorCommandTableConstructor(nsISupports *aOuter, REFNSIID aIID,
                                            void **aResult)
{
  nsresult rv;
  nsCOMPtr<nsIControllerCommandTable> commandTable =
      do_CreateInstance(NS_CONTROLLERCOMMANDTABLE_CONTRACTID, &rv);
  if (NS_FAILED(rv)) return rv;

  rv = EditorController::RegisterEditorCommands(commandTable);
  if (NS_FAILED(rv)) return rv;

  // we don't know here whether we're being created as an instance,
  // or a service, so we can't become immutable

  return commandTable->QueryInterface(aIID, aResult);
}

// Constructor for a command-table pre-filled with editing commands
static nsresult
nsEditingCommandTableConstructor(nsISupports *aOuter, REFNSIID aIID,
                                              void **aResult)
{
  nsresult rv;
  nsCOMPtr<nsIControllerCommandTable> commandTable =
      do_CreateInstance(NS_CONTROLLERCOMMANDTABLE_CONTRACTID, &rv);
  if (NS_FAILED(rv)) return rv;

  rv = EditorController::RegisterEditingCommands(commandTable);
  if (NS_FAILED(rv)) return rv;

  // we don't know here whether we're being created as an instance,
  // or a service, so we can't become immutable

  return commandTable->QueryInterface(aIID, aResult);
}

static const mozilla::Module::CIDEntry kLayoutCIDs[] = {
  XPCONNECT_CIDENTRIES
#ifdef DEBUG
  { &kNS_LAYOUT_DEBUGGER_CID, false, nullptr, CreateNewLayoutDebugger },
#endif
  { &kNS_FRAMETRAVERSAL_CID, false, nullptr, CreateNewFrameTraversal },
  { &kNS_BOXOBJECT_CID, false, nullptr, CreateNewBoxObject },
#ifdef MOZ_XUL
  { &kNS_LISTBOXOBJECT_CID, false, nullptr, CreateNewListBoxObject },
  { &kNS_MENUBOXOBJECT_CID, false, nullptr, CreateNewMenuBoxObject },
  { &kNS_SCROLLBOXOBJECT_CID, false, nullptr, CreateNewScrollBoxObject },
  { &kNS_TREEBOXOBJECT_CID, false, nullptr, CreateNewTreeBoxObject },
#endif // MOZ_XUL
  { &kIN_DEEPTREEWALKER_CID, false, nullptr, inDeepTreeWalkerConstructor },
  { &kNS_CONTENT_VIEWER_CID, false, nullptr, CreateContentViewer },
  { &kNS_HTMLDOCUMENT_CID, false, nullptr, CreateHTMLDocument },
  { &kNS_XMLDOCUMENT_CID, false, nullptr, CreateXMLDocument },
  { &kNS_SVGDOCUMENT_CID, false, nullptr, CreateSVGDocument },
  { &kNS_IMAGEDOCUMENT_CID, false, nullptr, CreateImageDocument },
  { &kNS_CONTENTITERATOR_CID, false, nullptr, CreateContentIterator },
  { &kNS_PRECONTENTITERATOR_CID, false, nullptr, CreatePreContentIterator },
  { &kNS_SUBTREEITERATOR_CID, false, nullptr, CreateSubtreeIterator },
  { &kNS_TEXT_ENCODER_CID, false, nullptr, CreateTextEncoder },
  { &kNS_HTMLCOPY_TEXT_ENCODER_CID, false, nullptr, CreateHTMLCopyTextEncoder },
  { &kNS_XMLCONTENTSERIALIZER_CID, false, nullptr, CreateXMLContentSerializer },
  { &kNS_HTMLCONTENTSERIALIZER_CID, false, nullptr, CreateHTMLContentSerializer },
  { &kNS_XHTMLCONTENTSERIALIZER_CID, false, nullptr, CreateXHTMLContentSerializer },
  { &kNS_PLAINTEXTSERIALIZER_CID, false, nullptr, CreatePlainTextSerializer },
  { &kNS_PARSERUTILS_CID, false, nullptr, nsParserUtilsConstructor },
  { &kNS_SCRIPTABLEUNESCAPEHTML_CID, false, nullptr, nsParserUtilsConstructor },
  { &kNS_CONTENTPOLICY_CID, false, nullptr, CreateContentPolicy },
  { &kNS_DATADOCUMENTCONTENTPOLICY_CID, false, nullptr, nsDataDocumentContentPolicyConstructor },
  { &kNS_NODATAPROTOCOLCONTENTPOLICY_CID, false, nullptr, nsNoDataProtocolContentPolicyConstructor },
  { &kNS_XULCONTROLLERS_CID, false, nullptr, NS_NewXULControllers },
#ifdef MOZ_XUL
  { &kNS_XULSORTSERVICE_CID, false, nullptr, CreateXULSortService },
  { &kNS_XULDOCUMENT_CID, false, nullptr, CreateXULDocument },
#endif
  { &kNS_CONTENT_DOCUMENT_LOADER_FACTORY_CID, false, nullptr, CreateContentDLF },
  { &kNS_JSPROTOCOLHANDLER_CID, false, nullptr, nsJSProtocolHandler::Create },
  { &kNS_JSURI_CID, false, nullptr, nsJSURIMutatorConstructor }, // do_CreateInstance returns mutator
  { &kNS_JSURIMUTATOR_CID, false, nullptr, nsJSURIMutatorConstructor },
  { &kNS_WINDOWCOMMANDTABLE_CID, false, nullptr, CreateWindowCommandTableConstructor },
  { &kNS_WINDOWCONTROLLER_CID, false, nullptr, CreateWindowControllerWithSingletonCommandTable },
  { &kNS_PLUGINDOCLOADERFACTORY_CID, false, nullptr, CreateContentDLF },
  { &kNS_PLUGINDOCUMENT_CID, false, nullptr, CreatePluginDocument },
  { &kNS_VIDEODOCUMENT_CID, false, nullptr, CreateVideoDocument },
  { &kNS_STYLESHEETSERVICE_CID, false, nullptr, nsStyleSheetServiceConstructor },
  { &kNS_HOSTOBJECTURI_CID, false, nullptr, nsHostObjectURIMutatorConstructor }, // do_CreateInstance returns mutator
  { &kNS_HOSTOBJECTURIMUTATOR_CID, false, nullptr, nsHostObjectURIMutatorConstructor },
  { &kNS_DOMSESSIONSTORAGEMANAGER_CID, false, nullptr, SessionStorageManagerConstructor },
  { &kNS_DOMLOCALSTORAGEMANAGER_CID, false, nullptr, LocalStorageManagerConstructor },
  { &kNS_TEXTEDITOR_CID, false, nullptr, TextEditorConstructor },
  { &kDOMREQUEST_SERVICE_CID, false, nullptr, DOMRequestServiceConstructor },
  { &kQUOTAMANAGER_SERVICE_CID, false, nullptr, QuotaManagerServiceConstructor },
  { &kSERVICEWORKERMANAGER_CID, false, nullptr, ServiceWorkerManagerConstructor },
  { &kSTORAGEACTIVITYSERVICE_CID, false, nullptr, StorageActivityServiceConstructor },
  { &kNOTIFICATIONTELEMETRYSERVICE_CID, false, nullptr, NotificationTelemetryServiceConstructor },
  { &kPUSHNOTIFIER_CID, false, nullptr, PushNotifierConstructor },
  { &kWORKERDEBUGGERMANAGER_CID, true, nullptr, WorkerDebuggerManagerConstructor },
  { &kNS_AUDIOCHANNELAGENT_CID, true, nullptr, AudioChannelAgentConstructor },
  { &kNS_HTMLEDITOR_CID, false, nullptr, HTMLEditorConstructor },
  { &kNS_EDITORCONTROLLER_CID, false, nullptr, EditorControllerConstructor },
  { &kNS_EDITINGCONTROLLER_CID, false, nullptr, nsEditingControllerConstructor },
  { &kNS_EDITORCOMMANDTABLE_CID, false, nullptr, nsEditorCommandTableConstructor },
  { &kNS_EDITINGCOMMANDTABLE_CID, false, nullptr, nsEditingCommandTableConstructor },
  { &kNS_GEOLOCATION_SERVICE_CID, false, nullptr, nsGeolocationServiceConstructor },
  { &kNS_GEOLOCATION_CID, false, nullptr, GeolocationConstructor },
  { &kNS_AUDIOCHANNEL_SERVICE_CID, false, nullptr, AudioChannelServiceConstructor },
  { &kNS_WEBSOCKETEVENT_SERVICE_CID, false, nullptr, WebSocketEventServiceConstructor },
  { &kNS_FOCUSMANAGER_CID, false, nullptr, CreateFocusManager },
#ifdef MOZ_WEBSPEECH_TEST_BACKEND
  { &kNS_FAKE_SPEECH_RECOGNITION_SERVICE_CID, false, nullptr, FakeSpeechRecognitionServiceConstructor },
#endif
#ifdef MOZ_WEBSPEECH
  { &kNS_SYNTHVOICEREGISTRY_CID, true, nullptr, nsSynthVoiceRegistryConstructor },
#endif
  { &kNS_CONTENTSECURITYMANAGER_CID, false, nullptr, nsContentSecurityManagerConstructor },
  { &kCSPSERVICE_CID, false, nullptr, CSPServiceConstructor },
  { &kNS_CSPCONTEXT_CID, false, nullptr, nsCSPContextConstructor },
  { &kNS_MIXEDCONTENTBLOCKER_CID, false, nullptr, nsMixedContentBlockerConstructor },
  { &kNS_EVENTLISTENERSERVICE_CID, false, nullptr, CreateEventListenerService },
  { &kNS_GLOBALMESSAGEMANAGER_CID, false, nullptr, CreateGlobalMessageManager },
  { &kNS_PARENTPROCESSMESSAGEMANAGER_CID, false, nullptr, CreateParentMessageManager },
  { &kNS_CHILDPROCESSMESSAGEMANAGER_CID, false, nullptr, CreateChildMessageManager },
  { &kNS_SCRIPTSECURITYMANAGER_CID, false, nullptr, Construct_nsIScriptSecurityManager },
  { &kNS_PRINCIPAL_CID, false, nullptr, ContentPrincipalConstructor },
  { &kNS_SYSTEMPRINCIPAL_CID, false, nullptr, SystemPrincipalConstructor },
  { &kNS_NULLPRINCIPAL_CID, false, nullptr, NullPrincipalConstructor },
  { &kNS_DEVICE_SENSORS_CID, false, nullptr, nsDeviceSensorsConstructor },
#if defined(ANDROID)
  { &kNS_HAPTICFEEDBACK_CID, false, nullptr, nsHapticFeedbackConstructor },
#endif
  { &kTHIRDPARTYUTIL_CID, false, nullptr, ThirdPartyUtilConstructor },
  { &kNS_STRUCTUREDCLONECONTAINER_CID, false, nullptr, nsStructuredCloneContainerConstructor },
  { &kNS_POWERMANAGERSERVICE_CID, false, nullptr, nsIPowerManagerServiceConstructor, Module::ALLOW_IN_GPU_PROCESS },
  { &kOSFILECONSTANTSSERVICE_CID, true, nullptr, OSFileConstantsServiceConstructor },
  { &kUDPSOCKETCHILD_CID, false, nullptr, UDPSocketChildConstructor },
  { &kGECKO_MEDIA_PLUGIN_SERVICE_CID, true, nullptr, GeckoMediaPluginServiceConstructor },
  { &kNS_MEDIAMANAGERSERVICE_CID, false, nullptr, nsIMediaManagerServiceConstructor },
#ifdef ACCESSIBILITY
  { &kNS_ACCESSIBILITY_SERVICE_CID, false, nullptr, CreateA11yService },
#endif
  { &kPRESENTATION_SERVICE_CID, false, nullptr, nsIPresentationServiceConstructor },
  { &kPRESENTATION_DEVICE_MANAGER_CID, false, nullptr, PresentationDeviceManagerConstructor },
  { &kPRESENTATION_TCP_SESSION_TRANSPORT_CID, false, nullptr, PresentationTCPSessionTransportConstructor },
  { &kTEXT_INPUT_PROCESSOR_CID, false, nullptr, TextInputProcessorConstructor },
  { &kNS_SCRIPTERROR_CID, false, nullptr, nsScriptErrorConstructor },
  { nullptr }
};

static const mozilla::Module::ContractIDEntry kLayoutContracts[] = {
  XPCONNECT_CONTRACTS
  { "@mozilla.org/layout/xul-boxobject;1", &kNS_BOXOBJECT_CID },
#ifdef MOZ_XUL
  { "@mozilla.org/layout/xul-boxobject-listbox;1", &kNS_LISTBOXOBJECT_CID },
  { "@mozilla.org/layout/xul-boxobject-menu;1", &kNS_MENUBOXOBJECT_CID },
  { "@mozilla.org/layout/xul-boxobject-scrollbox;1", &kNS_SCROLLBOXOBJECT_CID },
  { "@mozilla.org/layout/xul-boxobject-tree;1", &kNS_TREEBOXOBJECT_CID },
#endif // MOZ_XUL
  { "@mozilla.org/inspector/deep-tree-walker;1", &kIN_DEEPTREEWALKER_CID },
  { "@mozilla.org/xml/xml-document;1", &kNS_XMLDOCUMENT_CID },
  { "@mozilla.org/svg/svg-document;1", &kNS_SVGDOCUMENT_CID },
  { "@mozilla.org/content/post-content-iterator;1", &kNS_CONTENTITERATOR_CID },
  { "@mozilla.org/content/pre-content-iterator;1", &kNS_PRECONTENTITERATOR_CID },
  { "@mozilla.org/content/subtree-content-iterator;1", &kNS_SUBTREEITERATOR_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "text/xml", &kNS_TEXT_ENCODER_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "application/xml", &kNS_TEXT_ENCODER_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "application/xhtml+xml", &kNS_TEXT_ENCODER_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "image/svg+xml", &kNS_TEXT_ENCODER_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "text/html", &kNS_TEXT_ENCODER_CID },
  { NS_DOC_ENCODER_CONTRACTID_BASE "text/plain", &kNS_TEXT_ENCODER_CID },
  { NS_HTMLCOPY_ENCODER_CONTRACTID, &kNS_HTMLCOPY_TEXT_ENCODER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "text/xml", &kNS_XMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "application/xml", &kNS_XMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "application/xhtml+xml", &kNS_XHTMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "image/svg+xml", &kNS_XMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "text/html", &kNS_HTMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "application/vnd.mozilla.xul+xml", &kNS_XMLCONTENTSERIALIZER_CID },
  { NS_CONTENTSERIALIZER_CONTRACTID_PREFIX "text/plain", &kNS_PLAINTEXTSERIALIZER_CID },
  { NS_PARSERUTILS_CONTRACTID, &kNS_PARSERUTILS_CID },
  { NS_SCRIPTABLEUNESCAPEHTML_CONTRACTID, &kNS_SCRIPTABLEUNESCAPEHTML_CID },
  { NS_CONTENTPOLICY_CONTRACTID, &kNS_CONTENTPOLICY_CID },
  { NS_DATADOCUMENTCONTENTPOLICY_CONTRACTID, &kNS_DATADOCUMENTCONTENTPOLICY_CID },
  { NS_NODATAPROTOCOLCONTENTPOLICY_CONTRACTID, &kNS_NODATAPROTOCOLCONTENTPOLICY_CID },
  { "@mozilla.org/xul/xul-controllers;1", &kNS_XULCONTROLLERS_CID },
#ifdef MOZ_XUL
  { "@mozilla.org/xul/xul-sort-service;1", &kNS_XULSORTSERVICE_CID },
#endif
  { CONTENT_DLF_CONTRACTID, &kNS_CONTENT_DOCUMENT_LOADER_FACTORY_CID },
  { NS_JSPROTOCOLHANDLER_CONTRACTID, &kNS_JSPROTOCOLHANDLER_CID },
  { NS_WINDOWCONTROLLER_CONTRACTID, &kNS_WINDOWCONTROLLER_CID },
  { PLUGIN_DLF_CONTRACTID, &kNS_PLUGINDOCLOADERFACTORY_CID },
  { NS_STYLESHEETSERVICE_CONTRACTID, &kNS_STYLESHEETSERVICE_CID },
  { "@mozilla.org/dom/localStorage-manager;1", &kNS_DOMLOCALSTORAGEMANAGER_CID },
  // Keeping the old ContractID for backward compatibility
  { "@mozilla.org/dom/storagemanager;1", &kNS_DOMLOCALSTORAGEMANAGER_CID },
  { "@mozilla.org/dom/sessionStorage-manager;1", &kNS_DOMSESSIONSTORAGEMANAGER_CID },
  { "@mozilla.org/editor/texteditor;1", &kNS_TEXTEDITOR_CID },
  { DOMREQUEST_SERVICE_CONTRACTID, &kDOMREQUEST_SERVICE_CID },
  { QUOTAMANAGER_SERVICE_CONTRACTID, &kQUOTAMANAGER_SERVICE_CID },
  { SERVICEWORKERMANAGER_CONTRACTID, &kSERVICEWORKERMANAGER_CID },
  { STORAGE_ACTIVITY_SERVICE_CONTRACTID, &kSTORAGEACTIVITYSERVICE_CID },
  { NOTIFICATIONTELEMETRYSERVICE_CONTRACTID, &kNOTIFICATIONTELEMETRYSERVICE_CID },
  { PUSHNOTIFIER_CONTRACTID, &kPUSHNOTIFIER_CID },
  { WORKERDEBUGGERMANAGER_CONTRACTID, &kWORKERDEBUGGERMANAGER_CID },
  { NS_AUDIOCHANNELAGENT_CONTRACTID, &kNS_AUDIOCHANNELAGENT_CID },
  { "@mozilla.org/editor/htmleditor;1", &kNS_HTMLEDITOR_CID },
  { "@mozilla.org/editor/editorcontroller;1", &kNS_EDITORCONTROLLER_CID },
  { "@mozilla.org/editor/editingcontroller;1", &kNS_EDITINGCONTROLLER_CID },
  { "@mozilla.org/geolocation/service;1", &kNS_GEOLOCATION_SERVICE_CID },
  { "@mozilla.org/geolocation;1", &kNS_GEOLOCATION_CID },
  { "@mozilla.org/audiochannel/service;1", &kNS_AUDIOCHANNEL_SERVICE_CID },
  { "@mozilla.org/websocketevent/service;1", &kNS_WEBSOCKETEVENT_SERVICE_CID },
  { "@mozilla.org/focus-manager;1", &kNS_FOCUSMANAGER_CID },
#ifdef MOZ_WEBSPEECH_TEST_BACKEND
  { NS_SPEECH_RECOGNITION_SERVICE_CONTRACTID_PREFIX "fake", &kNS_FAKE_SPEECH_RECOGNITION_SERVICE_CID },
#endif
#ifdef MOZ_WEBSPEECH
  { NS_SYNTHVOICEREGISTRY_CONTRACTID, &kNS_SYNTHVOICEREGISTRY_CID },
#endif
  { NS_CONTENTSECURITYMANAGER_CONTRACTID, &kNS_CONTENTSECURITYMANAGER_CID },
  { CSPSERVICE_CONTRACTID, &kCSPSERVICE_CID },
  { NS_CSPCONTEXT_CONTRACTID, &kNS_CSPCONTEXT_CID },
  { NS_MIXEDCONTENTBLOCKER_CONTRACTID, &kNS_MIXEDCONTENTBLOCKER_CID },
  { NS_EVENTLISTENERSERVICE_CONTRACTID, &kNS_EVENTLISTENERSERVICE_CID },
  { NS_GLOBALMESSAGEMANAGER_CONTRACTID, &kNS_GLOBALMESSAGEMANAGER_CID },
  { NS_PARENTPROCESSMESSAGEMANAGER_CONTRACTID, &kNS_PARENTPROCESSMESSAGEMANAGER_CID },
  { NS_CHILDPROCESSMESSAGEMANAGER_CONTRACTID, &kNS_CHILDPROCESSMESSAGEMANAGER_CID },
  { NS_SCRIPTSECURITYMANAGER_CONTRACTID, &kNS_SCRIPTSECURITYMANAGER_CID },
  { NS_PRINCIPAL_CONTRACTID, &kNS_PRINCIPAL_CID },
  { NS_SYSTEMPRINCIPAL_CONTRACTID, &kNS_SYSTEMPRINCIPAL_CID },
  { NS_NULLPRINCIPAL_CONTRACTID, &kNS_NULLPRINCIPAL_CID },
  { NS_DEVICE_SENSORS_CONTRACTID, &kNS_DEVICE_SENSORS_CID },
#if defined(ANDROID)
  { "@mozilla.org/widget/hapticfeedback;1", &kNS_HAPTICFEEDBACK_CID },
#endif
  { THIRDPARTYUTIL_CONTRACTID, &kTHIRDPARTYUTIL_CID },
  { NS_STRUCTUREDCLONECONTAINER_CONTRACTID, &kNS_STRUCTUREDCLONECONTAINER_CID },
  { POWERMANAGERSERVICE_CONTRACTID, &kNS_POWERMANAGERSERVICE_CID, Module::ALLOW_IN_GPU_PROCESS },
  { OSFILECONSTANTSSERVICE_CONTRACTID, &kOSFILECONSTANTSSERVICE_CID },
  { "@mozilla.org/udp-socket-child;1", &kUDPSOCKETCHILD_CID },
  { MEDIAMANAGERSERVICE_CONTRACTID, &kNS_MEDIAMANAGERSERVICE_CID },
#ifdef ACCESSIBILITY
  { "@mozilla.org/accessibilityService;1", &kNS_ACCESSIBILITY_SERVICE_CID },
  { "@mozilla.org/accessibleRetrieval;1", &kNS_ACCESSIBILITY_SERVICE_CID },
#endif
  { "@mozilla.org/gecko-media-plugin-service;1",  &kGECKO_MEDIA_PLUGIN_SERVICE_CID },
  { PRESENTATION_SERVICE_CONTRACTID, &kPRESENTATION_SERVICE_CID },
  { PRESENTATION_DEVICE_MANAGER_CONTRACTID, &kPRESENTATION_DEVICE_MANAGER_CID },
  { PRESENTATION_TCP_SESSION_TRANSPORT_CONTRACTID, &kPRESENTATION_TCP_SESSION_TRANSPORT_CID },
  { "@mozilla.org/text-input-processor;1", &kTEXT_INPUT_PROCESSOR_CID },
  { NS_SCRIPTERROR_CONTRACTID, &kNS_SCRIPTERROR_CID },
  { nullptr }
};

static const mozilla::Module::CategoryEntry kLayoutCategories[] = {
  XPCONNECT_CATEGORIES
  { "content-policy", NS_DATADOCUMENTCONTENTPOLICY_CONTRACTID, NS_DATADOCUMENTCONTENTPOLICY_CONTRACTID },
  { "content-policy", NS_NODATAPROTOCOLCONTENTPOLICY_CONTRACTID, NS_NODATAPROTOCOLCONTENTPOLICY_CONTRACTID },
  { "content-policy", "CSPService", CSPSERVICE_CONTRACTID },
  { "content-policy", NS_MIXEDCONTENTBLOCKER_CONTRACTID, NS_MIXEDCONTENTBLOCKER_CONTRACTID },
  { "net-channel-event-sinks", "CSPService", CSPSERVICE_CONTRACTID },
  { "net-channel-event-sinks", NS_MIXEDCONTENTBLOCKER_CONTRACTID, NS_MIXEDCONTENTBLOCKER_CONTRACTID },
  { "app-startup", "Script Security Manager", "service," NS_SCRIPTSECURITYMANAGER_CONTRACTID },
  { "app-startup", "Push Notifier", "service," PUSHNOTIFIER_CONTRACTID },
  { "clear-origin-attributes-data", "QuotaManagerService", "service," QUOTAMANAGER_SERVICE_CONTRACTID },
  { OBSERVER_TOPIC_IDLE_DAILY, "QuotaManagerService", QUOTAMANAGER_SERVICE_CONTRACTID },
  CONTENTDLF_CATEGORIES
  { "profile-after-change", "PresentationDeviceManager", PRESENTATION_DEVICE_MANAGER_CONTRACTID },
  { "profile-after-change", "PresentationService", PRESENTATION_SERVICE_CONTRACTID },
  { "profile-after-change", "Notification Telemetry Service", NOTIFICATIONTELEMETRYSERVICE_CONTRACTID },
  { nullptr }
};

static void
LayoutModuleDtor()
{
  if (XRE_GetProcessType() == GeckoProcessType_GPU) {
    return;
  }

  Shutdown();
  nsContentUtils::XPCOMShutdown();

  // Layout depends heavily on gfx and imagelib, so we want to make sure that
  // these modules are shut down after all the layout cleanup runs.
  mozilla::image::ShutdownModule();
  gfxPlatform::Shutdown();
  gfx::gfxVars::Shutdown();

  nsScriptSecurityManager::Shutdown();
  xpcModuleDtor();
}

static const mozilla::Module kLayoutModule = {
  mozilla::Module::kVersion,
  kLayoutCIDs,
  kLayoutContracts,
  kLayoutCategories,
  nullptr,
  Initialize,
  LayoutModuleDtor,
  Module::ALLOW_IN_GPU_PROCESS
};

NSMODULE_DEFN(nsLayoutModule) = &kLayoutModule;
