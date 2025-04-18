/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=4 sw=4 et tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*

  An implementation for the XUL document. This implementation serves
  as the basis for generating an NGLayout content model.

  Notes
  -----

  1. We do some monkey business in the document observer methods to
     keep the element map in sync for HTML elements. Why don't we just
     do it for _all_ elements? Well, in the case of XUL elements,
     which may be lazily created during frame construction, the
     document observer methods will never be called because we'll be
     adding the XUL nodes into the content model "quietly".

*/

#include "mozilla/ArrayUtils.h"

#include "XULDocument.h"

#include "nsError.h"
#include "nsIBoxObject.h"
#include "nsIChromeRegistry.h"
#include "nsView.h"
#include "nsViewManager.h"
#include "nsIContentViewer.h"
#include "nsIStreamListener.h"
#include "nsITimer.h"
#include "nsDocShell.h"
#include "nsGkAtoms.h"
#include "nsXMLContentSink.h"
#include "nsXULContentSink.h"
#include "nsXULContentUtils.h"
#include "nsIXULOverlayProvider.h"
#include "nsIStringEnumerator.h"
#include "nsDocElementCreatedNotificationRunner.h"
#include "nsNetUtil.h"
#include "nsParserCIID.h"
#include "nsPIBoxObject.h"
#include "mozilla/dom/BoxObject.h"
#include "nsString.h"
#include "nsPIDOMWindow.h"
#include "nsPIWindowRoot.h"
#include "nsXULCommandDispatcher.h"
#include "nsXULElement.h"
#include "nsXULPrototypeCache.h"
#include "mozilla/Logging.h"
#include "nsIFrame.h"
#include "nsXBLService.h"
#include "nsCExternalHandlerService.h"
#include "nsMimeTypes.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "nsContentList.h"
#include "nsISimpleEnumerator.h"
#include "nsIScriptGlobalObject.h"
#include "nsIScriptSecurityManager.h"
#include "nsNodeInfoManager.h"
#include "nsContentCreatorFunctions.h"
#include "nsContentUtils.h"
#include "nsIParser.h"
#include "nsCharsetSource.h"
#include "mozilla/StyleSheetInlines.h"
#include "mozilla/css/Loader.h"
#include "nsIScriptError.h"
#include "nsIStyleSheetLinkingElement.h"
#include "nsIObserverService.h"
#include "nsNodeUtils.h"
#include "nsIDocShellTreeOwner.h"
#include "nsIXULWindow.h"
#include "nsXULPopupManager.h"
#include "nsCCUncollectableMarker.h"
#include "nsURILoader.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/NodeInfoInlines.h"
#include "mozilla/dom/ProcessingInstruction.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/XULDocumentBinding.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/Preferences.h"
#include "nsTextNode.h"
#include "nsJSUtils.h"
#include "mozilla/dom/URL.h"
#include "nsIContentPolicy.h"
#include "mozAutoDocUpdate.h"
#include "xpcpublic.h"
#include "mozilla/StyleSheet.h"
#include "mozilla/StyleSheetInlines.h"
#include "nsIConsoleService.h"

using namespace mozilla;
using namespace mozilla::dom;

//----------------------------------------------------------------------
//
// CIDs
//

static NS_DEFINE_CID(kParserCID,                 NS_PARSER_CID);

static bool IsOverlayAllowed(nsIURI* aURI)
{
    bool canOverlay = false;
    if (NS_SUCCEEDED(aURI->SchemeIs("about", &canOverlay)) && canOverlay)
        return true;
    if (NS_SUCCEEDED(aURI->SchemeIs("chrome", &canOverlay)) && canOverlay)
        return true;
    return false;
}

//----------------------------------------------------------------------
//
// Miscellaneous Constants
//

const nsForwardReference::Phase nsForwardReference::kPasses[] = {
    nsForwardReference::eConstruction,
    nsForwardReference::eHookup,
    nsForwardReference::eDone
};

//----------------------------------------------------------------------
//
// Statics
//

int32_t XULDocument::gRefCnt = 0;

LazyLogModule XULDocument::gXULLog("XULDocument");

//----------------------------------------------------------------------

struct BroadcastListener {
    nsWeakPtr mListener;
    RefPtr<nsAtom> mAttribute;
};

struct BroadcasterMapEntry : public PLDHashEntryHdr
{
    Element* mBroadcaster;  // [WEAK]
    nsTArray<BroadcastListener*> mListeners;  // [OWNING] of BroadcastListener objects
};

//----------------------------------------------------------------------
//
// ctors & dtors
//

namespace mozilla {
namespace dom {

XULDocument::XULDocument(void)
    : XMLDocument("application/vnd.mozilla.xul+xml"),
      mNextSrcLoadWaiter(nullptr),
      mApplyingPersistedAttrs(false),
      mIsWritingFastLoad(false),
      mDocumentLoaded(false),
      mStillWalking(false),
      mRestrictPersistence(false),
      mPendingSheets(0),
      mDocLWTheme(Doc_Theme_Uninitialized),
      mState(eState_Master),
      mCurrentScriptProto(nullptr),
      mOffThreadCompiling(false),
      mOffThreadCompileStringBuf(nullptr),
      mOffThreadCompileStringLength(0),
      mResolutionPhase(nsForwardReference::eStart),
      mBroadcasterMap(nullptr),
      mInitialLayoutComplete(false),
      mHandlingDelayedAttrChange(false),
      mHandlingDelayedBroadcasters(false)
{
    // Override the default in nsDocument
    mCharacterSet = UTF_8_ENCODING;

    mDefaultElementType = kNameSpaceID_XUL;
    mType = eXUL;

    mDelayFrameLoaderInitialization = true;

    mAllowXULXBL = eTriTrue;
}

XULDocument::~XULDocument()
{
    NS_ASSERTION(mNextSrcLoadWaiter == nullptr,
        "unreferenced document still waiting for script source to load?");

    // In case we failed somewhere early on and the forward observer
    // decls never got resolved.
    mForwardReferences.Clear();
    // Likewise for any references we have to IDs where we might
    // look for persisted data:
    mPersistenceIds.Clear();

    // Destroy our broadcaster map.
    delete mBroadcasterMap;

    Preferences::UnregisterCallback(XULDocument::DirectionChanged,
                                    "intl.uidirection", this);

    if (mOffThreadCompileStringBuf) {
      js_free(mOffThreadCompileStringBuf);
    }
}

} // namespace dom
} // namespace mozilla

nsresult
NS_NewXULDocument(nsIDocument** result)
{
    MOZ_ASSERT(result != nullptr, "null ptr");
    if (! result)
        return NS_ERROR_NULL_POINTER;

    RefPtr<XULDocument> doc = new XULDocument();

    nsresult rv;
    if (NS_FAILED(rv = doc->Init())) {
        return rv;
    }

    doc.forget(result);
    return NS_OK;
}


namespace mozilla {
namespace dom {

//----------------------------------------------------------------------
//
// nsISupports interface
//

NS_IMPL_CYCLE_COLLECTION_CLASS(XULDocument)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(XULDocument, XMLDocument)
    NS_ASSERTION(!nsCCUncollectableMarker::InGeneration(cb, tmp->GetMarkedCCGeneration()),
                 "Shouldn't traverse XULDocument!");
    // XXX tmp->mForwardReferences?
    // XXX tmp->mContextStack?

    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mCurrentPrototype)
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mMasterPrototype)
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mCommandDispatcher)
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPrototypes)
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mLocalStore)

    if (tmp->mOverlayLoadObservers) {
        for (auto iter = tmp->mOverlayLoadObservers->Iter();
             !iter.Done();
             iter.Next()) {
            NS_CYCLE_COLLECTION_NOTE_EDGE_NAME(cb, "mOverlayLoadObservers value");
            cb.NoteXPCOMChild(iter.Data());
        }
    }
    if (tmp->mPendingOverlayLoadNotifications) {
        for (auto iter = tmp->mPendingOverlayLoadNotifications->Iter();
             !iter.Done();
             iter.Next()) {
            NS_CYCLE_COLLECTION_NOTE_EDGE_NAME(cb, "mPendingOverlayLoadNotifications value");
            cb.NoteXPCOMChild(iter.Data());
        }
    }
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(XULDocument, XMLDocument)
    NS_IMPL_CYCLE_COLLECTION_UNLINK(mCommandDispatcher)
    NS_IMPL_CYCLE_COLLECTION_UNLINK(mLocalStore)
    //XXX We should probably unlink all the objects we traverse.
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED(XULDocument,
                                             XMLDocument,
                                             nsIStreamLoaderObserver,
                                             nsICSSLoaderObserver,
                                             nsIOffThreadScriptReceiver)


//----------------------------------------------------------------------
//
// nsIDocument interface
//

void
XULDocument::Reset(nsIChannel* aChannel, nsILoadGroup* aLoadGroup)
{
    NS_NOTREACHED("Reset");
}

void
XULDocument::ResetToURI(nsIURI* aURI, nsILoadGroup* aLoadGroup,
                        nsIPrincipal* aPrincipal)
{
    NS_NOTREACHED("ResetToURI");
}

void
XULDocument::SetContentType(const nsAString& aContentType)
{
    NS_ASSERTION(aContentType.EqualsLiteral("application/vnd.mozilla.xul+xml"),
                 "xul-documents always has content-type application/vnd.mozilla.xul+xml");
    // Don't do anything, xul always has the mimetype
    // application/vnd.mozilla.xul+xml
}

// This is called when the master document begins loading, whether it's
// being cached or not.
nsresult
XULDocument::StartDocumentLoad(const char* aCommand, nsIChannel* aChannel,
                               nsILoadGroup* aLoadGroup,
                               nsISupports* aContainer,
                               nsIStreamListener **aDocListener,
                               bool aReset, nsIContentSink* aSink)
{
    if (MOZ_LOG_TEST(gXULLog, LogLevel::Warning)) {

        nsCOMPtr<nsIURI> uri;
        nsresult rv = aChannel->GetOriginalURI(getter_AddRefs(uri));
        if (NS_SUCCEEDED(rv)) {
            nsAutoCString urlspec;
            rv = uri->GetSpec(urlspec);
            if (NS_SUCCEEDED(rv)) {
                MOZ_LOG(gXULLog, LogLevel::Warning,
                       ("xul: load document '%s'", urlspec.get()));
            }
        }
    }
    // NOTE: If this ever starts calling nsDocument::StartDocumentLoad
    // we'll possibly need to reset our content type afterwards.
    mStillWalking = true;
    mMayStartLayout = false;
    mDocumentLoadGroup = do_GetWeakReference(aLoadGroup);

    mChannel = aChannel;

    // Get the URI.  Note that this should match nsDocShell::OnLoadingSite
    nsresult rv =
        NS_GetFinalChannelURI(aChannel, getter_AddRefs(mDocumentURI));
    NS_ENSURE_SUCCESS(rv, rv);

    mOriginalURI = mDocumentURI;

    // Get the document's principal
    nsCOMPtr<nsIPrincipal> principal;
    nsContentUtils::GetSecurityManager()->
        GetChannelResultPrincipal(mChannel, getter_AddRefs(principal));
    principal = MaybeDowngradePrincipal(principal);

    ResetStylesheetsToURI(mDocumentURI);

    RetrieveRelevantHeaders(aChannel);

    // Look in the chrome cache: we've got this puppy loaded
    // already.
    nsXULPrototypeDocument* proto = IsChromeURI(mDocumentURI) ?
            nsXULPrototypeCache::GetInstance()->GetPrototype(mDocumentURI) :
            nullptr;

    // Same comment as nsChromeProtocolHandler::NewChannel and
    // XULDocument::ResumeWalk
    // - Ben Goodger
    //
    // We don't abort on failure here because there are too many valid
    // cases that can return failure, and the null-ness of |proto| is enough
    // to trigger the fail-safe parse-from-disk solution. Example failure cases
    // (for reference) include:
    //
    // NS_ERROR_NOT_AVAILABLE: the URI cannot be found in the startup cache,
    //                         parse from disk
    // other: the startup cache file could not be found, probably
    //        due to being accessed before a profile has been selected (e.g.
    //        loading chrome for the profile manager itself). This must be
    //        parsed from disk.

    if (proto) {
        // If we're racing with another document to load proto, wait till the
        // load has finished loading before trying to add cloned style sheets.
        // XULDocument::EndLoad will call proto->NotifyLoadDone, which will
        // find all racing documents and notify them via OnPrototypeLoadDone,
        // which will add style sheet clones to each document.
        bool loaded;
        rv = proto->AwaitLoadDone(this, &loaded);
        if (NS_FAILED(rv)) return rv;

        mMasterPrototype = mCurrentPrototype = proto;

        // Set up the right principal on ourselves.
        SetPrincipal(proto->DocumentPrincipal());

        // We need a listener, even if proto is not yet loaded, in which
        // event the listener's OnStopRequest method does nothing, and all
        // the interesting work happens below XULDocument::EndLoad, from
        // the call there to mCurrentPrototype->NotifyLoadDone().
        *aDocListener = new CachedChromeStreamListener(this, loaded);
    }
    else {
        bool useXULCache = nsXULPrototypeCache::GetInstance()->IsEnabled();
        bool fillXULCache = (useXULCache && IsChromeURI(mDocumentURI));


        // It's just a vanilla document load. Create a parser to deal
        // with the stream n' stuff.

        nsCOMPtr<nsIParser> parser;
        rv = PrepareToLoadPrototype(mDocumentURI, aCommand, principal,
                                    getter_AddRefs(parser));
        if (NS_FAILED(rv)) return rv;

        // Predicate mIsWritingFastLoad on the XUL cache being enabled,
        // so we don't have to re-check whether the cache is enabled all
        // the time.
        mIsWritingFastLoad = useXULCache;

        nsCOMPtr<nsIStreamListener> listener = do_QueryInterface(parser, &rv);
        NS_ASSERTION(NS_SUCCEEDED(rv), "parser doesn't support nsIStreamListener");
        if (NS_FAILED(rv)) return rv;

        *aDocListener = listener;

        parser->Parse(mDocumentURI);

        // Put the current prototype, created under PrepareToLoad, into the
        // XUL prototype cache now.  We can't do this under PrepareToLoad or
        // overlay loading will break; search for PutPrototype in ResumeWalk
        // and see the comment there.
        if (fillXULCache) {
            nsXULPrototypeCache::GetInstance()->PutPrototype(mCurrentPrototype);
        }
    }

    NS_IF_ADDREF(*aDocListener);
    return NS_OK;
}

// This gets invoked after a prototype for this document or one of
// its overlays is fully built in the content sink.
void
XULDocument::EndLoad()
{
    // This can happen if an overlay fails to load
    if (!mCurrentPrototype)
        return;

    nsresult rv;

    // Whack the prototype document into the cache so that the next
    // time somebody asks for it, they don't need to load it by hand.

    nsCOMPtr<nsIURI> uri = mCurrentPrototype->GetURI();
    bool isChrome = IsChromeURI(uri);

    // Remember if the XUL cache is on
    bool useXULCache = nsXULPrototypeCache::GetInstance()->IsEnabled();

    // If the current prototype is an overlay document (non-master prototype)
    // and we're filling the FastLoad disk cache, tell the cache we're done
    // loading it, and write the prototype. The master prototype is put into
    // the cache earlier in XULDocument::StartDocumentLoad.
    if (useXULCache && mIsWritingFastLoad && isChrome &&
        mMasterPrototype != mCurrentPrototype) {
        nsXULPrototypeCache::GetInstance()->WritePrototype(mCurrentPrototype);
    }

    if (IsOverlayAllowed(uri)) {
        nsCOMPtr<nsIXULOverlayProvider> reg =
            mozilla::services::GetXULOverlayProviderService();

        if (reg) {
            nsCOMPtr<nsISimpleEnumerator> overlays;
            rv = reg->GetStyleOverlays(uri, getter_AddRefs(overlays));
            if (NS_FAILED(rv)) return;

            bool moreSheets;
            nsCOMPtr<nsISupports> next;
            nsCOMPtr<nsIURI> sheetURI;

            while (NS_SUCCEEDED(rv = overlays->HasMoreElements(&moreSheets)) &&
                   moreSheets) {
                overlays->GetNext(getter_AddRefs(next));

                sheetURI = do_QueryInterface(next);
                if (!sheetURI) {
                    NS_ERROR("Chrome registry handed me a non-nsIURI object!");
                    continue;
                }

                if (IsChromeURI(sheetURI)) {
                    mCurrentPrototype->AddStyleSheetReference(sheetURI);
                }
            }
        }

        if (isChrome && useXULCache) {
            // If it's a chrome prototype document, then notify any
            // documents that raced to load the prototype, and awaited
            // its load completion via proto->AwaitLoadDone().
            rv = mCurrentPrototype->NotifyLoadDone();
            if (NS_FAILED(rv)) return;
        }
    }

    OnPrototypeLoadDone(true);
    if (MOZ_LOG_TEST(gXULLog, LogLevel::Warning)) {
        nsAutoCString urlspec;
        rv = uri->GetSpec(urlspec);
        if (NS_SUCCEEDED(rv)) {
            MOZ_LOG(gXULLog, LogLevel::Warning,
                   ("xul: Finished loading document '%s'", urlspec.get()));
        }
    }
}

nsresult
XULDocument::OnPrototypeLoadDone(bool aResumeWalk)
{
    nsresult rv;

    // Add the style overlays from chrome registry, if any.
    rv = AddPrototypeSheets();
    if (NS_FAILED(rv)) return rv;

    rv = PrepareToWalk();
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to prepare for walk");
    if (NS_FAILED(rv)) return rv;

    if (aResumeWalk) {
        rv = ResumeWalk();
    }
    return rv;
}

// called when an error occurs parsing a document
bool
XULDocument::OnDocumentParserError()
{
  // don't report errors that are from overlays
  if (mCurrentPrototype && mMasterPrototype != mCurrentPrototype) {
    nsCOMPtr<nsIURI> uri = mCurrentPrototype->GetURI();
    if (IsChromeURI(uri)) {
      nsCOMPtr<nsIObserverService> os =
        mozilla::services::GetObserverService();
      if (os)
        os->NotifyObservers(uri, "xul-overlay-parsererror",
                            EmptyString().get());
    }

    return false;
  }

  return true;
}

static void
ClearBroadcasterMapEntry(PLDHashTable* aTable, PLDHashEntryHdr* aEntry)
{
    BroadcasterMapEntry* entry =
        static_cast<BroadcasterMapEntry*>(aEntry);
    for (size_t i = entry->mListeners.Length() - 1; i != (size_t)-1; --i) {
        delete entry->mListeners[i];
    }
    entry->mListeners.Clear();

    // N.B. that we need to manually run the dtor because we
    // constructed the nsTArray object in-place.
    entry->mListeners.~nsTArray<BroadcastListener*>();
}

static bool
CanBroadcast(int32_t aNameSpaceID, nsAtom* aAttribute)
{
    // Don't push changes to the |id|, |persist|, |command| or
    // |observes| attribute.
    if (aNameSpaceID == kNameSpaceID_None) {
        if ((aAttribute == nsGkAtoms::id) ||
            (aAttribute == nsGkAtoms::persist) ||
            (aAttribute == nsGkAtoms::command) ||
            (aAttribute == nsGkAtoms::observes)) {
            return false;
        }
    }
    return true;
}

struct nsAttrNameInfo
{
  nsAttrNameInfo(int32_t aNamespaceID, nsAtom* aName, nsAtom* aPrefix) :
    mNamespaceID(aNamespaceID), mName(aName), mPrefix(aPrefix) {}
  nsAttrNameInfo(const nsAttrNameInfo& aOther) :
    mNamespaceID(aOther.mNamespaceID), mName(aOther.mName),
    mPrefix(aOther.mPrefix) {}
  int32_t           mNamespaceID;
  RefPtr<nsAtom> mName;
  RefPtr<nsAtom> mPrefix;
};

void
XULDocument::SynchronizeBroadcastListener(Element *aBroadcaster,
                                          Element *aListener,
                                          const nsAString &aAttr)
{
    if (!nsContentUtils::IsSafeToRunScript()) {
        nsDelayedBroadcastUpdate delayedUpdate(aBroadcaster, aListener,
                                               aAttr);
        mDelayedBroadcasters.AppendElement(delayedUpdate);
        MaybeBroadcast();
        return;
    }
    bool notify = mDocumentLoaded || mHandlingDelayedBroadcasters;

    if (aAttr.EqualsLiteral("*")) {
        uint32_t count = aBroadcaster->GetAttrCount();
        nsTArray<nsAttrNameInfo> attributes(count);
        for (uint32_t i = 0; i < count; ++i) {
            const nsAttrName* attrName = aBroadcaster->GetAttrNameAt(i);
            int32_t nameSpaceID = attrName->NamespaceID();
            nsAtom* name = attrName->LocalName();

            // _Don't_ push the |id|, |ref|, or |persist| attribute's value!
            if (! CanBroadcast(nameSpaceID, name))
                continue;

            attributes.AppendElement(nsAttrNameInfo(nameSpaceID, name,
                                                    attrName->GetPrefix()));
        }

        count = attributes.Length();
        while (count-- > 0) {
            int32_t nameSpaceID = attributes[count].mNamespaceID;
            nsAtom* name = attributes[count].mName;
            nsAutoString value;
            if (aBroadcaster->GetAttr(nameSpaceID, name, value)) {
              aListener->SetAttr(nameSpaceID, name, attributes[count].mPrefix,
                                 value, notify);
            }

#if 0
            // XXX we don't fire the |onbroadcast| handler during
            // initial hookup: doing so would potentially run the
            // |onbroadcast| handler before the |onload| handler,
            // which could define JS properties that mask XBL
            // properties, etc.
            ExecuteOnBroadcastHandlerFor(aBroadcaster, aListener, name);
#endif
        }
    }
    else {
        // Find out if the attribute is even present at all.
        RefPtr<nsAtom> name = NS_Atomize(aAttr);

        nsAutoString value;
        if (aBroadcaster->GetAttr(kNameSpaceID_None, name, value)) {
            aListener->SetAttr(kNameSpaceID_None, name, value, notify);
        } else {
            aListener->UnsetAttr(kNameSpaceID_None, name, notify);
        }

#if 0
        // XXX we don't fire the |onbroadcast| handler during initial
        // hookup: doing so would potentially run the |onbroadcast|
        // handler before the |onload| handler, which could define JS
        // properties that mask XBL properties, etc.
        ExecuteOnBroadcastHandlerFor(aBroadcaster, aListener, name);
#endif
    }
}

void
XULDocument::AddBroadcastListenerFor(Element& aBroadcaster, Element& aListener,
                                     const nsAString& aAttr, ErrorResult& aRv)
{
    nsresult rv =
        nsContentUtils::CheckSameOrigin(this, &aBroadcaster);

    if (NS_FAILED(rv)) {
        aRv.Throw(rv);
        return;
    }

    rv = nsContentUtils::CheckSameOrigin(this, &aListener);

    if (NS_FAILED(rv)) {
        aRv.Throw(rv);
        return;
    }

    static const PLDHashTableOps gOps = {
        PLDHashTable::HashVoidPtrKeyStub,
        PLDHashTable::MatchEntryStub,
        PLDHashTable::MoveEntryStub,
        ClearBroadcasterMapEntry,
        nullptr
    };

    if (! mBroadcasterMap) {
        mBroadcasterMap = new PLDHashTable(&gOps, sizeof(BroadcasterMapEntry));
    }

    auto entry = static_cast<BroadcasterMapEntry*>
                            (mBroadcasterMap->Search(&aBroadcaster));
    if (!entry) {
        entry = static_cast<BroadcasterMapEntry*>
                           (mBroadcasterMap->Add(&aBroadcaster, fallible));

        if (! entry) {
            aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
            return;
        }

        entry->mBroadcaster = &aBroadcaster;

        // N.B. placement new to construct the nsTArray object in-place
        new (&entry->mListeners) nsTArray<BroadcastListener*>();
    }

    // Only add the listener if it's not there already!
    RefPtr<nsAtom> attr = NS_Atomize(aAttr);

    for (size_t i = entry->mListeners.Length() - 1; i != (size_t)-1; --i) {
        BroadcastListener* bl = entry->mListeners[i];
        nsCOMPtr<Element> blListener = do_QueryReferent(bl->mListener);

        if (blListener == &aListener && bl->mAttribute == attr)
            return;
    }

    BroadcastListener* bl = new BroadcastListener;
    bl->mListener  = do_GetWeakReference(&aListener);
    bl->mAttribute = attr;

    entry->mListeners.AppendElement(bl);

    SynchronizeBroadcastListener(&aBroadcaster, &aListener, aAttr);
}

void
XULDocument::RemoveBroadcastListenerFor(Element& aBroadcaster,
                                        Element& aListener,
                                        const nsAString& aAttr)
{
    // If we haven't added any broadcast listeners, then there sure
    // aren't any to remove.
    if (! mBroadcasterMap)
        return;

    auto entry = static_cast<BroadcasterMapEntry*>
                            (mBroadcasterMap->Search(&aBroadcaster));
    if (entry) {
        RefPtr<nsAtom> attr = NS_Atomize(aAttr);
        for (size_t i = entry->mListeners.Length() - 1; i != (size_t)-1; --i) {
            BroadcastListener* bl = entry->mListeners[i];
            nsCOMPtr<Element> blListener = do_QueryReferent(bl->mListener);

            if (blListener == &aListener && bl->mAttribute == attr) {
                entry->mListeners.RemoveElementAt(i);
                delete bl;

                if (entry->mListeners.IsEmpty())
                    mBroadcasterMap->RemoveEntry(entry);

                break;
            }
        }
    }
}

nsresult
XULDocument::ExecuteOnBroadcastHandlerFor(Element* aBroadcaster,
                                          Element* aListener,
                                          nsAtom* aAttr)
{
    // Now we execute the onchange handler in the context of the
    // observer. We need to find the observer in order to
    // execute the handler.

    for (nsIContent* child = aListener->GetFirstChild();
         child;
         child = child->GetNextSibling()) {

        // Look for an <observes> element beneath the listener. This
        // ought to have an |element| attribute that refers to
        // aBroadcaster, and an |attribute| element that tells us what
        // attriubtes we're listening for.
        if (!child->IsXULElement(nsGkAtoms::observes))
            continue;

        // Is this the element that was listening to us?
        nsAutoString listeningToID;
        child->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::element, listeningToID);

        nsAutoString broadcasterID;
        aBroadcaster->GetAttr(kNameSpaceID_None, nsGkAtoms::id, broadcasterID);

        if (listeningToID != broadcasterID)
            continue;

        // We are observing the broadcaster, but is this the right
        // attribute?
        nsAutoString listeningToAttribute;
        child->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::attribute,
                                    listeningToAttribute);

        if (!aAttr->Equals(listeningToAttribute) &&
            !listeningToAttribute.EqualsLiteral("*")) {
            continue;
        }

        // This is the right <observes> element. Execute the
        // |onbroadcast| event handler
        WidgetEvent event(true, eXULBroadcast);

        RefPtr<nsPresContext> presContext = GetPresContext();
        if (presContext) {
          // Handle the DOM event
          nsEventStatus status = nsEventStatus_eIgnore;
          EventDispatcher::Dispatch(child, presContext, &event, nullptr,
                                    &status);
        }
    }

    return NS_OK;
}

static bool
ShouldPersistAttribute(Element* aElement, nsAtom* aAttribute)
{
    if (aElement->IsXULElement(nsGkAtoms::window)) {
        // This is not an element of the top document, its owner is
        // not an nsXULWindow. Persist it.
        if (aElement->OwnerDoc()->GetParentDocument()) {
            return true;
        }
        // The following attributes of xul:window should be handled in
        // nsXULWindow::SavePersistentAttributes instead of here.
        if (aAttribute == nsGkAtoms::screenX ||
            aAttribute == nsGkAtoms::screenY ||
            aAttribute == nsGkAtoms::width ||
            aAttribute == nsGkAtoms::height ||
            aAttribute == nsGkAtoms::sizemode) {
            return false;
        }
    }
    return true;
}

void
XULDocument::AttributeChanged(Element* aElement, int32_t aNameSpaceID,
                              nsAtom* aAttribute, int32_t aModType,
                              const nsAttrValue* aOldValue)
{
    NS_ASSERTION(aElement->OwnerDoc() == this, "unexpected doc");

    // Might not need this, but be safe for now.
    nsCOMPtr<nsIMutationObserver> kungFuDeathGrip(this);

    // Synchronize broadcast listeners
    if (mBroadcasterMap &&
        CanBroadcast(aNameSpaceID, aAttribute)) {
        auto entry = static_cast<BroadcasterMapEntry*>
                                (mBroadcasterMap->Search(aElement));

        if (entry) {
            // We've got listeners: push the value.
            nsAutoString value;
            bool attrSet = aElement->GetAttr(kNameSpaceID_None, aAttribute, value);

            for (size_t i = entry->mListeners.Length() - 1; i != (size_t)-1; --i) {
                BroadcastListener* bl = entry->mListeners[i];
                if ((bl->mAttribute == aAttribute) ||
                    (bl->mAttribute == nsGkAtoms::_asterisk)) {
                    nsCOMPtr<Element> listenerEl
                        = do_QueryReferent(bl->mListener);
                    if (listenerEl) {
                        nsAutoString currentValue;
                        bool hasAttr = listenerEl->GetAttr(kNameSpaceID_None,
                                                           aAttribute,
                                                           currentValue);
                        // We need to update listener only if we're
                        // (1) removing an existing attribute,
                        // (2) adding a new attribute or
                        // (3) changing the value of an attribute.
                        bool needsAttrChange =
                            attrSet != hasAttr || !value.Equals(currentValue);
                        nsDelayedBroadcastUpdate delayedUpdate(aElement,
                                                               listenerEl,
                                                               aAttribute,
                                                               value,
                                                               attrSet,
                                                               needsAttrChange);

                        size_t index =
                            mDelayedAttrChangeBroadcasts.IndexOf(delayedUpdate,
                                0, nsDelayedBroadcastUpdate::Comparator());
                        if (index != mDelayedAttrChangeBroadcasts.NoIndex) {
                            if (mHandlingDelayedAttrChange) {
                                NS_WARNING("Broadcasting loop!");
                                continue;
                            }
                            mDelayedAttrChangeBroadcasts.RemoveElementAt(index);
                        }

                        mDelayedAttrChangeBroadcasts.AppendElement(delayedUpdate);
                    }
                }
            }
        }
    }

    // checks for modifications in broadcasters
    bool listener, resolved;
    CheckBroadcasterHookup(aElement, &listener, &resolved);

    // See if there is anything we need to persist in the localstore.
    //
    // XXX Namespace handling broken :-(
    nsAutoString persist;
    aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::persist, persist);
    // Persistence of attributes of xul:window is handled in nsXULWindow.
    if (ShouldPersistAttribute(aElement, aAttribute) && !persist.IsEmpty() &&
        // XXXldb This should check that it's a token, not just a substring.
        persist.Find(nsDependentAtomString(aAttribute)) >= 0) {
      nsContentUtils::AddScriptRunner(
        NewRunnableMethod<Element*, int32_t, nsAtom*>(
          "dom::XULDocument::DoPersist",
          this,
          &XULDocument::DoPersist,
          aElement,
          kNameSpaceID_None,
          aAttribute));
    }
}

void
XULDocument::ContentAppended(nsIContent* aFirstNewContent)
{
    NS_ASSERTION(aFirstNewContent->OwnerDoc() == this, "unexpected doc");

    // Might not need this, but be safe for now.
    nsCOMPtr<nsIMutationObserver> kungFuDeathGrip(this);

    // Update our element map
    nsresult rv = NS_OK;
    for (nsIContent* cur = aFirstNewContent; cur && NS_SUCCEEDED(rv);
         cur = cur->GetNextSibling()) {
        rv = AddSubtreeToDocument(cur);
    }
}

void
XULDocument::ContentInserted(nsIContent* aChild)
{
    NS_ASSERTION(aChild->OwnerDoc() == this, "unexpected doc");

    // Might not need this, but be safe for now.
    nsCOMPtr<nsIMutationObserver> kungFuDeathGrip(this);

    AddSubtreeToDocument(aChild);
}

void
XULDocument::ContentRemoved(nsIContent* aChild, nsIContent* aPreviousSibling)
{
    NS_ASSERTION(aChild->OwnerDoc() == this, "unexpected doc");

    // Might not need this, but be safe for now.
    nsCOMPtr<nsIMutationObserver> kungFuDeathGrip(this);

    RemoveSubtreeFromDocument(aChild);
}

nsresult
XULDocument::AddForwardReference(nsForwardReference* aRef)
{
    if (mResolutionPhase < aRef->GetPhase()) {
        if (!mForwardReferences.AppendElement(aRef)) {
            delete aRef;
            return NS_ERROR_OUT_OF_MEMORY;
        }
    }
    else {
        NS_ERROR("forward references have already been resolved");
        delete aRef;
    }

    return NS_OK;
}

nsresult
XULDocument::ResolveForwardReferences()
{
    if (mResolutionPhase == nsForwardReference::eDone)
        return NS_OK;

    NS_ASSERTION(mResolutionPhase == nsForwardReference::eStart,
                 "nested ResolveForwardReferences()");

    // Resolve each outstanding 'forward' reference. We iterate
    // through the list of forward references until no more forward
    // references can be resolved. This annealing process is
    // guaranteed to converge because we've "closed the gate" to new
    // forward references.

    const nsForwardReference::Phase* pass = nsForwardReference::kPasses;
    while ((mResolutionPhase = *pass) != nsForwardReference::eDone) {
        uint32_t previous = 0;
        while (mForwardReferences.Length() &&
               mForwardReferences.Length() != previous) {
            previous = mForwardReferences.Length();

            for (uint32_t i = 0; i < mForwardReferences.Length(); ++i) {
                nsForwardReference* fwdref = mForwardReferences[i];

                if (fwdref->GetPhase() == *pass) {
                    nsForwardReference::Result result = fwdref->Resolve();

                    switch (result) {
                    case nsForwardReference::eResolve_Succeeded:
                    case nsForwardReference::eResolve_Error:
                        mForwardReferences.RemoveElementAt(i);

                        // fixup because we removed from list
                        --i;
                        break;

                    case nsForwardReference::eResolve_Later:
                        // do nothing. we'll try again later
                        ;
                    }

                    if (mResolutionPhase == nsForwardReference::eStart) {
                        // Resolve() loaded a dynamic overlay,
                        // (see XULDocument::LoadOverlayInternal()).
                        // Return for now, we will be called again.
                        return NS_OK;
                    }
                }
            }
        }

        ++pass;
    }

    mForwardReferences.Clear();
    return NS_OK;
}

//----------------------------------------------------------------------
//
// nsIDOMDocument interface
//

already_AddRefed<nsINodeList>
XULDocument::GetElementsByAttribute(const nsAString& aAttribute,
                                    const nsAString& aValue)
{
    RefPtr<nsAtom> attrAtom(NS_Atomize(aAttribute));
    nsAutoPtr<nsString> attrValue(new nsString(aValue));
    RefPtr<nsContentList> list = new nsContentList(this,
                                            MatchAttribute,
                                            nsContentUtils::DestroyMatchString,
                                            attrValue.forget(),
                                            true,
                                            attrAtom,
                                            kNameSpaceID_Unknown);

    return list.forget();
}

already_AddRefed<nsINodeList>
XULDocument::GetElementsByAttributeNS(const nsAString& aNamespaceURI,
                                      const nsAString& aAttribute,
                                      const nsAString& aValue,
                                      ErrorResult& aRv)
{
    RefPtr<nsAtom> attrAtom(NS_Atomize(aAttribute));
    nsAutoPtr<nsString> attrValue(new nsString(aValue));

    int32_t nameSpaceId = kNameSpaceID_Wildcard;
    if (!aNamespaceURI.EqualsLiteral("*")) {
      nsresult rv =
        nsContentUtils::NameSpaceManager()->RegisterNameSpace(aNamespaceURI,
                                                              nameSpaceId);
      if (NS_FAILED(rv)) {
          aRv.Throw(rv);
          return nullptr;
      }
    }

    RefPtr<nsContentList> list = new nsContentList(this,
                                            MatchAttribute,
                                            nsContentUtils::DestroyMatchString,
                                            attrValue.forget(),
                                            true,
                                            attrAtom,
                                            nameSpaceId);
    return list.forget();
}

void
XULDocument::Persist(const nsAString& aID,
                     const nsAString& aAttr,
                     ErrorResult& aRv)
{
    // If we're currently reading persisted attributes out of the
    // localstore, _don't_ re-enter and try to set them again!
    if (mApplyingPersistedAttrs) {
        return;
    }

    Element* element = nsDocument::GetElementById(aID);
    if (!element) {
        return;
    }

    RefPtr<nsAtom> tag;
    int32_t nameSpaceID;

    RefPtr<mozilla::dom::NodeInfo> ni = element->GetExistingAttrNameFromQName(aAttr);
    nsresult rv;
    if (ni) {
        tag = ni->NameAtom();
        nameSpaceID = ni->NamespaceID();
    }
    else {
        // Make sure that this QName is going to be valid.
        const char16_t *colon;
        rv = nsContentUtils::CheckQName(PromiseFlatString(aAttr), true, &colon);

        if (NS_FAILED(rv)) {
            // There was an invalid character or it was malformed.
            aRv.Throw(NS_ERROR_INVALID_ARG);
            return;
        }

        if (colon) {
            // We don't really handle namespace qualifiers in attribute names.
            aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
            return;
        }

        tag = NS_Atomize(aAttr);
        if (NS_WARN_IF(!tag)) {
            aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
            return;
        }

        nameSpaceID = kNameSpaceID_None;
    }

    aRv = Persist(element, nameSpaceID, tag);
}

nsresult
XULDocument::Persist(Element* aElement, int32_t aNameSpaceID,
                     nsAtom* aAttribute)
{
    // For non-chrome documents, persistance is simply broken
    if (!nsContentUtils::IsSystemPrincipal(NodePrincipal()))
        return NS_ERROR_NOT_AVAILABLE;

    if (!mLocalStore) {
        mLocalStore = do_GetService("@mozilla.org/xul/xulstore;1");
        if (NS_WARN_IF(!mLocalStore)) {
            return NS_ERROR_NOT_INITIALIZED;
        }
    }

    nsAutoString id;

    aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::id, id);
    nsAtomString attrstr(aAttribute);

    nsAutoString valuestr;
    aElement->GetAttr(kNameSpaceID_None, aAttribute, valuestr);

    nsAutoCString utf8uri;
    nsresult rv = mDocumentURI->GetSpec(utf8uri);
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }
    NS_ConvertUTF8toUTF16 uri(utf8uri);

    bool hasAttr;
    rv = mLocalStore->HasValue(uri, id, attrstr, &hasAttr);
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }

    if (hasAttr && valuestr.IsEmpty()) {
        return mLocalStore->RemoveValue(uri, id, attrstr);
    }

    // Persisting attributes to windows is handled by nsXULWindow.
    if (aElement->IsXULElement(nsGkAtoms::window)) {
        return NS_OK;
    }

    return mLocalStore->SetValue(uri, id, attrstr, valuestr);
}


nsresult
XULDocument::GetViewportSize(int32_t* aWidth,
                             int32_t* aHeight)
{
    *aWidth = *aHeight = 0;

    FlushPendingNotifications(FlushType::Layout);

    nsIPresShell *shell = GetShell();
    NS_ENSURE_TRUE(shell, NS_ERROR_FAILURE);

    nsIFrame* frame = shell->GetRootFrame();
    NS_ENSURE_TRUE(frame, NS_ERROR_FAILURE);

    nsSize size = frame->GetSize();

    *aWidth = nsPresContext::AppUnitsToIntCSSPixels(size.width);
    *aHeight = nsPresContext::AppUnitsToIntCSSPixels(size.height);

    return NS_OK;
}

int32_t
XULDocument::GetWidth(ErrorResult& aRv)
{
    int32_t width = 0;
    int32_t height = 0;
    aRv = GetViewportSize(&width, &height);
    return width;
}

int32_t
XULDocument::GetHeight(ErrorResult& aRv)
{
    int32_t width = 0;
    int32_t height = 0;
    aRv = GetViewportSize(&width, &height);
    return height;
}

static JSObject*
GetScopeObjectOfNode(nsINode* node)
{
    MOZ_ASSERT(node, "Must not be called with null.");

    // Window root occasionally keeps alive a node of a document whose
    // window is already dead. If in this brief period someone calls
    // GetPopupNode and we return that node, nsNodeSH::PreCreate will throw,
    // because it will not know which scope this node belongs to. Returning
    // an orphan node like that to JS would be a bug anyway, so to avoid
    // this, let's do the same check as nsNodeSH::PreCreate does to
    // determine the scope and if it fails let's just return null in
    // XULDocument::GetPopupNode.
    nsIDocument* doc = node->OwnerDoc();
    MOZ_ASSERT(doc, "This should never happen.");

    nsIGlobalObject* global = doc->GetScopeObject();
    return global ? global->GetGlobalJSObject() : nullptr;
}

already_AddRefed<nsINode>
XULDocument::GetPopupNode()
{
    nsCOMPtr<nsINode> node;
    nsCOMPtr<nsPIWindowRoot> rootWin = GetWindowRoot();
    if (rootWin) {
        node = rootWin->GetPopupNode(); // addref happens here
    }

    if (!node) {
        nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
        if (pm) {
            node = pm->GetLastTriggerPopupNode(this);
        }
    }

    if (node && nsContentUtils::CanCallerAccess(node)
        && GetScopeObjectOfNode(node)) {
        return node.forget();
    }

    return nullptr;
}

void
XULDocument::SetPopupNode(nsINode* aNode)
{
    nsCOMPtr<nsPIWindowRoot> rootWin = GetWindowRoot();
    if (rootWin) {
        rootWin->SetPopupNode(aNode);
    }
}

// Returns the rangeOffset element from the XUL Popup Manager. This is for
// chrome callers only.
nsINode*
XULDocument::GetPopupRangeParent(ErrorResult& aRv)
{
    nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
    if (!pm) {
        aRv.Throw(NS_ERROR_FAILURE);
        return nullptr;
    }

    nsINode* rangeParent = pm->GetMouseLocationParent();
    if (rangeParent && !nsContentUtils::CanCallerAccess(rangeParent)) {
        aRv.Throw(NS_ERROR_DOM_SECURITY_ERR);
        return nullptr;
    }

    return rangeParent;
}

// Returns the rangeOffset element from the XUL Popup Manager. We check the
// rangeParent to determine if the caller has rights to access to the data.
int32_t
XULDocument::GetPopupRangeOffset(ErrorResult& aRv)
{
    nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
    if (!pm) {
        aRv.Throw(NS_ERROR_FAILURE);
        return 0;
    }

    nsINode* rangeParent = pm->GetMouseLocationParent();
    if (rangeParent && !nsContentUtils::CanCallerAccess(rangeParent)) {
        aRv.Throw(NS_ERROR_DOM_SECURITY_ERR);
        return 0;
    }

    return pm->MouseLocationOffset();
}

already_AddRefed<nsINode>
XULDocument::GetTooltipNode()
{
    nsXULPopupManager* pm = nsXULPopupManager::GetInstance();
    if (pm) {
        nsCOMPtr<nsINode> node = pm->GetLastTriggerTooltipNode(this);
        if (node && nsContentUtils::CanCallerAccess(node)) {
            return node.forget();
        }
    }

    return nullptr;
}

nsresult
XULDocument::AddElementToDocumentPre(Element* aElement)
{
    // Do a bunch of work that's necessary when an element gets added
    // to the XUL Document.
    nsresult rv;

    // 1. Add the element to the id map, since it seems this can be
    // called when creating elements from prototypes.
    nsAtom* id = aElement->GetID();
    if (id) {
        // FIXME: Shouldn't BindToTree take care of this?
        nsAutoScriptBlocker scriptBlocker;
        AddToIdTable(aElement, id);
    }

    // 2. If the element is a 'command updater' (i.e., has a
    // "commandupdater='true'" attribute), then add the element to the
    // document's command dispatcher
    if (aElement->AttrValueIs(kNameSpaceID_None, nsGkAtoms::commandupdater,
                              nsGkAtoms::_true, eCaseMatters)) {
        rv = nsXULContentUtils::SetCommandUpdater(this, aElement);
        if (NS_FAILED(rv)) return rv;
    }

    // 3. Check for a broadcaster hookup attribute, in which case
    // we'll hook the node up as a listener on a broadcaster.
    bool listener, resolved;
    rv = CheckBroadcasterHookup(aElement, &listener, &resolved);
    if (NS_FAILED(rv)) return rv;

    // If it's not there yet, we may be able to defer hookup until
    // later.
    if (listener && !resolved && (mResolutionPhase != nsForwardReference::eDone)) {
        BroadcasterHookup* hookup = new BroadcasterHookup(this, aElement);
        rv = AddForwardReference(hookup);
        if (NS_FAILED(rv)) return rv;
    }

    return NS_OK;
}

nsresult
XULDocument::AddElementToDocumentPost(Element* aElement)
{
    if (aElement == GetRootElement()) {
        ResetDocumentDirection();
    }

    // We need to pay special attention to the keyset tag to set up a listener
    if (aElement->NodeInfo()->Equals(nsGkAtoms::keyset, kNameSpaceID_XUL)) {
        // Create our XUL key listener and hook it up.
        nsXBLService::AttachGlobalKeyHandler(aElement);
    }

    return NS_OK;
}

nsresult
XULDocument::AddSubtreeToDocument(nsIContent* aContent)
{
    NS_ASSERTION(aContent->GetUncomposedDoc() == this, "Element not in doc!");
    // From here on we only care about elements.
    if (!aContent->IsElement()) {
        return NS_OK;
    }

    Element* aElement = aContent->AsElement();

    // Do pre-order addition magic
    nsresult rv = AddElementToDocumentPre(aElement);
    if (NS_FAILED(rv)) return rv;

    // Recurse to children
    for (nsIContent* child = aElement->GetLastChild();
         child;
         child = child->GetPreviousSibling()) {

        rv = AddSubtreeToDocument(child);
        if (NS_FAILED(rv))
            return rv;
    }

    // Do post-order addition magic
    return AddElementToDocumentPost(aElement);
}

nsresult
XULDocument::RemoveSubtreeFromDocument(nsIContent* aContent)
{
    // From here on we only care about elements.
    if (!aContent->IsElement()) {
        return NS_OK;
    }

    Element* aElement = aContent->AsElement();

    // Do a bunch of cleanup to remove an element from the XUL
    // document.
    nsresult rv;

    if (aElement->NodeInfo()->Equals(nsGkAtoms::keyset, kNameSpaceID_XUL)) {
        nsXBLService::DetachGlobalKeyHandler(aElement);
    }

    // 1. Remove any children from the document.
    for (nsIContent* child = aElement->GetLastChild();
         child;
         child = child->GetPreviousSibling()) {

        rv = RemoveSubtreeFromDocument(child);
        if (NS_FAILED(rv))
            return rv;
    }

    // Remove the element from the id map, since we added it in
    // AddElementToDocumentPre().
    nsAtom* id = aElement->GetID();
    if (id) {
        // FIXME: Shouldn't UnbindFromTree take care of this?
        nsAutoScriptBlocker scriptBlocker;
        RemoveFromIdTable(aElement, id);
    }

    // 3. If the element is a 'command updater', then remove the
    // element from the document's command dispatcher.
    if (aElement->AttrValueIs(kNameSpaceID_None, nsGkAtoms::commandupdater,
                              nsGkAtoms::_true, eCaseMatters)) {
        rv = mCommandDispatcher->RemoveCommandUpdater(aElement);
        if (NS_FAILED(rv)) return rv;
    }

    // 4. Remove the element from our broadcaster map, since it is no longer
    // in the document.
    nsCOMPtr<Element> broadcaster, listener;
    nsAutoString attribute, broadcasterID;
    rv = FindBroadcaster(aElement, getter_AddRefs(listener),
                         broadcasterID, attribute, getter_AddRefs(broadcaster));
    if (rv == NS_FINDBROADCASTER_FOUND) {
        RemoveBroadcastListenerFor(*broadcaster, *listener, attribute);
    }

    return NS_OK;
}

//----------------------------------------------------------------------
//
// nsIDOMNode interface
//

nsresult
XULDocument::Clone(mozilla::dom::NodeInfo *aNodeInfo, nsINode **aResult,
                   bool aPreallocateChildren) const
{
    // We don't allow cloning of a XUL document
    *aResult = nullptr;
    return NS_ERROR_DOM_NOT_SUPPORTED_ERR;
}


//----------------------------------------------------------------------
//
// Implementation methods
//

nsresult
XULDocument::Init()
{
    nsresult rv = XMLDocument::Init();
    NS_ENSURE_SUCCESS(rv, rv);

    // Create our command dispatcher and hook it up.
    mCommandDispatcher = new nsXULCommandDispatcher(this);

    if (gRefCnt++ == 0) {
        // ensure that the XUL prototype cache is instantiated successfully,
        // so that we can use nsXULPrototypeCache::GetInstance() without
        // null-checks in the rest of the class.
        nsXULPrototypeCache* cache = nsXULPrototypeCache::GetInstance();
        if (!cache) {
          NS_ERROR("Could not instantiate nsXULPrototypeCache");
          return NS_ERROR_FAILURE;
        }
    }

    Preferences::RegisterCallback(XULDocument::DirectionChanged,
                                  "intl.uidirection", this);

    return NS_OK;
}


nsresult
XULDocument::StartLayout(void)
{
    mMayStartLayout = true;
    nsCOMPtr<nsIPresShell> shell = GetShell();
    if (shell) {
        // Resize-reflow this time
        nsPresContext *cx = shell->GetPresContext();
        NS_ASSERTION(cx != nullptr, "no pres context");
        if (! cx)
            return NS_ERROR_UNEXPECTED;

        nsCOMPtr<nsIDocShell> docShell = cx->GetDocShell();
        NS_ASSERTION(docShell != nullptr, "container is not a docshell");
        if (! docShell)
            return NS_ERROR_UNEXPECTED;

        nsresult rv = shell->Initialize();
        NS_ENSURE_SUCCESS(rv, rv);
    }

    return NS_OK;
}

/* static */
bool
XULDocument::MatchAttribute(Element* aElement,
                            int32_t aNamespaceID,
                            nsAtom* aAttrName,
                            void* aData)
{
    MOZ_ASSERT(aElement, "Must have content node to work with!");
    nsString* attrValue = static_cast<nsString*>(aData);
    if (aNamespaceID != kNameSpaceID_Unknown &&
        aNamespaceID != kNameSpaceID_Wildcard) {
        return attrValue->EqualsLiteral("*") ?
            aElement->HasAttr(aNamespaceID, aAttrName) :
            aElement->AttrValueIs(aNamespaceID, aAttrName, *attrValue,
                                  eCaseMatters);
    }

    // Qualified name match. This takes more work.

    uint32_t count = aElement->GetAttrCount();
    for (uint32_t i = 0; i < count; ++i) {
        const nsAttrName* name = aElement->GetAttrNameAt(i);
        bool nameMatch;
        if (name->IsAtom()) {
            nameMatch = name->Atom() == aAttrName;
        } else if (aNamespaceID == kNameSpaceID_Wildcard) {
            nameMatch = name->NodeInfo()->Equals(aAttrName);
        } else {
            nameMatch = name->NodeInfo()->QualifiedNameEquals(aAttrName);
        }

        if (nameMatch) {
            return attrValue->EqualsLiteral("*") ||
                aElement->AttrValueIs(name->NamespaceID(), name->LocalName(),
                                      *attrValue, eCaseMatters);
        }
    }

    return false;
}

nsresult
XULDocument::PrepareToLoadPrototype(nsIURI* aURI, const char* aCommand,
                                    nsIPrincipal* aDocumentPrincipal,
                                    nsIParser** aResult)
{
    nsresult rv;

    // Create a new prototype document.
    rv = NS_NewXULPrototypeDocument(getter_AddRefs(mCurrentPrototype));
    if (NS_FAILED(rv)) return rv;

    rv = mCurrentPrototype->InitPrincipal(aURI, aDocumentPrincipal);
    if (NS_FAILED(rv)) {
        mCurrentPrototype = nullptr;
        return rv;
    }

    // Bootstrap the master document prototype.
    if (! mMasterPrototype) {
        mMasterPrototype = mCurrentPrototype;
        // Set our principal based on the master proto.
        SetPrincipal(aDocumentPrincipal);
    }

    // Create a XUL content sink, a parser, and kick off a load for
    // the overlay.
    RefPtr<XULContentSinkImpl> sink = new XULContentSinkImpl();

    rv = sink->Init(this, mCurrentPrototype);
    NS_ASSERTION(NS_SUCCEEDED(rv), "Unable to initialize datasource sink");
    if (NS_FAILED(rv)) return rv;

    nsCOMPtr<nsIParser> parser = do_CreateInstance(kParserCID, &rv);
    NS_ASSERTION(NS_SUCCEEDED(rv), "unable to create parser");
    if (NS_FAILED(rv)) return rv;

    parser->SetCommand(nsCRT::strcmp(aCommand, "view-source") ? eViewNormal :
                       eViewSource);

    parser->SetDocumentCharset(UTF_8_ENCODING,
                               kCharsetFromDocTypeDefault);
    parser->SetContentSink(sink); // grabs a reference to the parser

    parser.forget(aResult);
    return NS_OK;
}


nsresult
XULDocument::ApplyPersistentAttributes()
{
    // For non-chrome documents, persistance is simply broken
    if (!nsContentUtils::IsSystemPrincipal(NodePrincipal()))
        return NS_ERROR_NOT_AVAILABLE;

    // Add all of the 'persisted' attributes into the content
    // model.
    if (!mLocalStore) {
        mLocalStore = do_GetService("@mozilla.org/xul/xulstore;1");
        if (NS_WARN_IF(!mLocalStore)) {
            return NS_ERROR_NOT_INITIALIZED;
        }
    }

    mApplyingPersistedAttrs = true;
    ApplyPersistentAttributesInternal();
    mApplyingPersistedAttrs = false;

    // After we've applied persistence once, we should only reapply
    // it to nodes created by overlays
    mRestrictPersistence = true;
    mPersistenceIds.Clear();

    return NS_OK;
}


nsresult
XULDocument::ApplyPersistentAttributesInternal()
{
    nsCOMArray<Element> elements;

    nsAutoCString utf8uri;
    nsresult rv = mDocumentURI->GetSpec(utf8uri);
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }
    NS_ConvertUTF8toUTF16 uri(utf8uri);

    // Get a list of element IDs for which persisted values are available
    nsCOMPtr<nsIStringEnumerator> ids;
    rv = mLocalStore->GetIDsEnumerator(uri, getter_AddRefs(ids));
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }

    while (1) {
        bool hasmore = false;
        ids->HasMore(&hasmore);
        if (!hasmore) {
            break;
        }

        nsAutoString id;
        ids->GetNext(id);

        if (mRestrictPersistence && !mPersistenceIds.Contains(id)) {
            continue;
        }

        nsIdentifierMapEntry* entry = mIdentifierMap.GetEntry(id);
        if (!entry) {
            continue;
        }

        // We want to hold strong refs to the elements while applying
        // persistent attributes, just in case.
        elements.Clear();
        elements.SetCapacity(entry->GetIdElements().Length());
        for (Element* element : entry->GetIdElements()) {
            elements.AppendObject(element);
        }
        if (elements.IsEmpty()) {
            continue;
        }

        rv = ApplyPersistentAttributesToElements(id, elements);
        if (NS_WARN_IF(NS_FAILED(rv))) {
            return rv;
        }
    }

    return NS_OK;
}

nsresult
XULDocument::ApplyPersistentAttributesToElements(const nsAString &aID,
                                                 nsCOMArray<Element>& aElements)
{
    nsAutoCString utf8uri;
    nsresult rv = mDocumentURI->GetSpec(utf8uri);
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }
    NS_ConvertUTF8toUTF16 uri(utf8uri);

    // Get a list of attributes for which persisted values are available
    nsCOMPtr<nsIStringEnumerator> attrs;
    rv = mLocalStore->GetAttributeEnumerator(uri, aID, getter_AddRefs(attrs));
    if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
    }

    while (1) {
        bool hasmore = PR_FALSE;
        attrs->HasMore(&hasmore);
        if (!hasmore) {
            break;
        }

        nsAutoString attrstr;
        attrs->GetNext(attrstr);

        nsAutoString value;
        rv = mLocalStore->GetValue(uri, aID, attrstr, value);
        if (NS_WARN_IF(NS_FAILED(rv))) {
            return rv;
        }

        RefPtr<nsAtom> attr = NS_Atomize(attrstr);
        if (NS_WARN_IF(!attr)) {
            return NS_ERROR_OUT_OF_MEMORY;
        }

        uint32_t cnt = aElements.Count();
        for (int32_t i = int32_t(cnt) - 1; i >= 0; --i) {
            RefPtr<Element> element = aElements.SafeObjectAt(i);
            if (!element) {
                 continue;
            }

            // Applying persistent attributes to windows is handled by nsXULWindow.
            if (element->IsXULElement(nsGkAtoms::window)) {
                continue;
            }

            Unused << element->SetAttr(kNameSpaceID_None, attr, value, true);
        }
    }

    return NS_OK;
}

void
XULDocument::TraceProtos(JSTracer* aTrc)
{
    uint32_t i, count = mPrototypes.Length();
    for (i = 0; i < count; ++i) {
        mPrototypes[i]->TraceProtos(aTrc);
    }

    if (mCurrentPrototype) {
        mCurrentPrototype->TraceProtos(aTrc);
    }
}

//----------------------------------------------------------------------
//
// XULDocument::ContextStack
//

XULDocument::ContextStack::ContextStack()
    : mTop(nullptr), mDepth(0)
{
}

XULDocument::ContextStack::~ContextStack()
{
    while (mTop) {
        Entry* doomed = mTop;
        mTop = mTop->mNext;
        NS_IF_RELEASE(doomed->mElement);
        delete doomed;
    }
}

nsresult
XULDocument::ContextStack::Push(nsXULPrototypeElement* aPrototype,
                                nsIContent* aElement)
{
    Entry* entry = new Entry;
    entry->mPrototype = aPrototype;
    entry->mElement   = aElement;
    NS_IF_ADDREF(entry->mElement);
    entry->mIndex     = 0;

    entry->mNext = mTop;
    mTop = entry;

    ++mDepth;
    return NS_OK;
}

nsresult
XULDocument::ContextStack::Pop()
{
    if (mDepth == 0)
        return NS_ERROR_UNEXPECTED;

    Entry* doomed = mTop;
    mTop = mTop->mNext;
    --mDepth;

    NS_IF_RELEASE(doomed->mElement);
    delete doomed;
    return NS_OK;
}

nsresult
XULDocument::ContextStack::Peek(nsXULPrototypeElement** aPrototype,
                                nsIContent** aElement,
                                int32_t* aIndex)
{
    if (mDepth == 0)
        return NS_ERROR_UNEXPECTED;

    *aPrototype = mTop->mPrototype;
    *aElement   = mTop->mElement;
    NS_IF_ADDREF(*aElement);
    *aIndex     = mTop->mIndex;

    return NS_OK;
}


nsresult
XULDocument::ContextStack::SetTopIndex(int32_t aIndex)
{
    if (mDepth == 0)
        return NS_ERROR_UNEXPECTED;

    mTop->mIndex = aIndex;
    return NS_OK;
}


//----------------------------------------------------------------------
//
// Content model walking routines
//

nsresult
XULDocument::PrepareToWalk()
{
    // Prepare to walk the mCurrentPrototype
    nsresult rv;

    // Keep an owning reference to the prototype document so that its
    // elements aren't yanked from beneath us.
    mPrototypes.AppendElement(mCurrentPrototype);

    // Get the prototype's root element and initialize the context
    // stack for the prototype walk.
    nsXULPrototypeElement* proto = mCurrentPrototype->GetRootElement();

    if (! proto) {
        if (MOZ_LOG_TEST(gXULLog, LogLevel::Error)) {
            nsCOMPtr<nsIURI> url = mCurrentPrototype->GetURI();

            nsAutoCString urlspec;
            rv = url->GetSpec(urlspec);
            if (NS_FAILED(rv)) return rv;

            MOZ_LOG(gXULLog, LogLevel::Error,
                   ("xul: error parsing '%s'", urlspec.get()));
        }

        return NS_OK;
    }

    nsINode* nodeToInsertBefore = nsINode::GetFirstChild();
    if (mState != eState_Master) {
        nodeToInsertBefore = GetRootElement();
    }

    const nsTArray<RefPtr<nsXULPrototypePI> >& processingInstructions =
        mCurrentPrototype->GetProcessingInstructions();

    uint32_t total = processingInstructions.Length();
    for (uint32_t i = 0; i < total; ++i) {
        rv = CreateAndInsertPI(processingInstructions[i],
                               this, nodeToInsertBefore);
        if (NS_FAILED(rv)) return rv;
    }

    // Now check the chrome registry for any additional overlays.
    rv = AddChromeOverlays();
    if (NS_FAILED(rv)) return rv;

    // Do one-time initialization if we're preparing to walk the
    // master document's prototype.
    RefPtr<Element> root;

    if (mState == eState_Master) {
        // Add the root element
        rv = CreateElementFromPrototype(proto, getter_AddRefs(root), true);
        if (NS_FAILED(rv)) return rv;

        rv = AppendChildTo(root, false);
        if (NS_FAILED(rv)) return rv;

        // Block onload until we've finished building the complete
        // document content model.
        BlockOnload();

        nsContentUtils::AddScriptRunner(
            new nsDocElementCreatedNotificationRunner(this));
    }

    // There'd better not be anything on the context stack at this
    // point! This is the basis case for our "induction" in
    // ResumeWalk(), below, which'll assume that there's always a
    // content element on the context stack if either 1) we're in the
    // "master" document, or 2) we're in an overlay, and we've got
    // more than one prototype element (the single, root "overlay"
    // element) on the stack.
    NS_ASSERTION(mContextStack.Depth() == 0, "something's on the context stack already");
    if (mContextStack.Depth() != 0)
        return NS_ERROR_UNEXPECTED;

    rv = mContextStack.Push(proto, root);
    if (NS_FAILED(rv)) return rv;

    return NS_OK;
}

nsresult
XULDocument::CreateAndInsertPI(const nsXULPrototypePI* aProtoPI,
                               nsINode* aParent, nsINode* aBeforeThis)
{
    MOZ_ASSERT(aProtoPI, "null ptr");
    MOZ_ASSERT(aParent, "null ptr");

    RefPtr<ProcessingInstruction> node =
        NS_NewXMLProcessingInstruction(mNodeInfoManager, aProtoPI->mTarget,
                                       aProtoPI->mData);

    nsresult rv;
    if (aProtoPI->mTarget.EqualsLiteral("xml-stylesheet")) {
        rv = InsertXMLStylesheetPI(aProtoPI, aParent, aBeforeThis, node);
    } else if (aProtoPI->mTarget.EqualsLiteral("xul-overlay")) {
        rv = InsertXULOverlayPI(aProtoPI, aParent, aBeforeThis, node);
    } else {
        // No special processing, just add the PI to the document.
        rv = aParent->InsertChildBefore(node->AsContent(),
                                        aBeforeThis
                                          ? aBeforeThis->AsContent() : nullptr,
                                        false);
    }

    return rv;
}

nsresult
XULDocument::InsertXMLStylesheetPI(const nsXULPrototypePI* aProtoPI,
                                   nsINode* aParent,
                                   nsINode* aBeforeThis,
                                   nsIContent* aPINode)
{
    nsCOMPtr<nsIStyleSheetLinkingElement> ssle(do_QueryInterface(aPINode));
    NS_ASSERTION(ssle, "passed XML Stylesheet node does not "
                       "implement nsIStyleSheetLinkingElement!");

    nsresult rv;

    ssle->InitStyleLinkElement(false);
    // We want to be notified when the style sheet finishes loading, so
    // disable style sheet loading for now.
    ssle->SetEnableUpdates(false);
    ssle->OverrideBaseURI(mCurrentPrototype->GetURI());

    rv = aParent->InsertChildBefore(aPINode->AsContent(),
                                    aBeforeThis
                                      ? aBeforeThis->AsContent() : nullptr,
                                    false);
    if (NS_FAILED(rv)) return rv;

    ssle->SetEnableUpdates(true);

    // load the stylesheet if necessary, passing ourselves as
    // nsICSSObserver
    auto result = ssle->UpdateStyleSheet(this);
    if (result.isErr()) {
        // Ignore errors from UpdateStyleSheet; we don't want failure to
        // do that to break the XUL document load.  But do propagate out
        // NS_ERROR_OUT_OF_MEMORY.
        if (result.unwrapErr() == NS_ERROR_OUT_OF_MEMORY) {
            return result.unwrapErr();
        }
        return NS_OK;
    }

    auto update = result.unwrap();
    if (update.ShouldBlock()) {
        ++mPendingSheets;
    }

    return NS_OK;
}

nsresult
XULDocument::InsertXULOverlayPI(const nsXULPrototypePI* aProtoPI,
                                nsINode* aParent,
                                nsINode* aBeforeThis,
                                nsIContent* aPINode)
{
    nsresult rv;

    rv = aParent->InsertChildBefore(aPINode->AsContent(),
                                    aBeforeThis
                                      ? aBeforeThis->AsContent() : nullptr,
                                    false);
    if (NS_FAILED(rv)) return rv;

    // xul-overlay PI is special only in prolog
    if (!nsContentUtils::InProlog(aPINode)) {
        return NS_OK;
    }

    nsAutoString href;
    nsContentUtils::GetPseudoAttributeValue(aProtoPI->mData,
                                            nsGkAtoms::href,
                                            href);

    // If there was no href, we can't do anything with this PI
    if (href.IsEmpty()) {
        return NS_OK;
    }

    // Add the overlay to our list of overlays that need to be processed.
    nsCOMPtr<nsIURI> uri;

    rv = NS_NewURI(getter_AddRefs(uri), href, nullptr,
                   mCurrentPrototype->GetURI());
    if (NS_SUCCEEDED(rv)) {
        // We insert overlays into mUnloadedOverlays at the same index in
        // document order, so they end up in the reverse of the document
        // order in mUnloadedOverlays.
        // This is needed because the code in ResumeWalk loads the overlays
        // by processing the last item of mUnloadedOverlays and removing it
        // from the array.
        mUnloadedOverlays.InsertElementAt(0, uri);
        rv = NS_OK;
    } else if (rv == NS_ERROR_MALFORMED_URI) {
        // The URL is bad, move along. Don't propagate for now.
        // XXX report this to the Error Console (bug 359846)
        rv = NS_OK;
    }

    return rv;
}

nsresult
XULDocument::AddChromeOverlays()
{
    nsresult rv;

    nsCOMPtr<nsIURI> docUri = mCurrentPrototype->GetURI();

    /* overlays only apply to chrome or about URIs */
    if (!IsOverlayAllowed(docUri)) return NS_OK;

    nsCOMPtr<nsIXULOverlayProvider> chromeReg =
        mozilla::services::GetXULOverlayProviderService();
    // In embedding situations, the chrome registry may not provide overlays,
    // or even exist at all; that's OK.
    NS_ENSURE_TRUE(chromeReg, NS_OK);

    nsCOMPtr<nsISimpleEnumerator> overlays;
    rv = chromeReg->GetXULOverlays(docUri, getter_AddRefs(overlays));
    NS_ENSURE_SUCCESS(rv, rv);

    bool moreOverlays;
    nsCOMPtr<nsISupports> next;
    nsCOMPtr<nsIURI> uri;

    while (NS_SUCCEEDED(rv = overlays->HasMoreElements(&moreOverlays)) &&
           moreOverlays) {

        rv = overlays->GetNext(getter_AddRefs(next));
        if (NS_FAILED(rv) || !next) break;

        uri = do_QueryInterface(next);
        if (!uri) {
            NS_ERROR("Chrome registry handed me a non-nsIURI object!");
            continue;
        }

        // Same comment as in XULDocument::InsertXULOverlayPI
        mUnloadedOverlays.InsertElementAt(0, uri);
    }

    return rv;
}

void
XULDocument::LoadOverlay(const nsAString& aURL, nsIObserver* aObserver,
                         ErrorResult& aRv)
{
    nsresult rv;

    nsCOMPtr<nsIURI> uri;
    rv = NS_NewURI(getter_AddRefs(uri), aURL, nullptr);
    if (NS_FAILED(rv)) {
        aRv.Throw(rv);
        return;
    }

    if (aObserver) {
        nsIObserver* obs = nullptr;
        if (!mOverlayLoadObservers) {
          mOverlayLoadObservers = new nsInterfaceHashtable<nsURIHashKey,nsIObserver>;
        }
        obs = mOverlayLoadObservers->GetWeak(uri);

        if (obs) {
            // We don't support loading the same overlay twice into the same
            // document - that doesn't make sense anyway.
            aRv.Throw(NS_ERROR_FAILURE);
            return;
        }
        mOverlayLoadObservers->Put(uri, aObserver);
    }
    bool shouldReturn, failureFromContent;
    rv = LoadOverlayInternal(uri, true, &shouldReturn, &failureFromContent);
    if (NS_FAILED(rv)) {
        // remove the observer if LoadOverlayInternal generated an error
        if (mOverlayLoadObservers) {
            mOverlayLoadObservers->Remove(uri);
        }
        aRv.Throw(rv);
        return;
    }
}

nsresult
XULDocument::LoadOverlayInternal(nsIURI* aURI, bool aIsDynamic,
                                 bool* aShouldReturn,
                                 bool* aFailureFromContent)
{
    nsresult rv;

    // XUL overlays are in the process of being removed. In a Firefox build,
    // loading an overlay will no longer work and display an error in the
    // console. In automation, doing so will cause a crash.
    // However, overlays are allowed in other applications (e.g. Thunderbird)
    // while they work on removing them. See bug 1448162.
#ifdef MOZ_BREAK_XUL_OVERLAYS
    nsCString docSpec;
    mCurrentPrototype->GetURI()->GetSpec(docSpec);
    nsCString overlaySpec;
    aURI->GetSpec(overlaySpec);
    nsPrintfCString msg("Attempt to load overlay %s into %s\n",
                        overlaySpec.get(),
                        docSpec.get());
    nsCOMPtr<nsIConsoleService> consoleSvc =
        do_GetService("@mozilla.org/consoleservice;1");
    if (consoleSvc) {
        consoleSvc->LogStringMessage(NS_ConvertASCIItoUTF16(msg).get());
    }
    printf("%s", msg.get());
    if (xpc::IsInAutomation()) {
        MOZ_CRASH("Attempt to load overlay.");
    }
    return NS_ERROR_NOT_AVAILABLE;
#endif

    *aShouldReturn = false;
    *aFailureFromContent = false;

    if (MOZ_LOG_TEST(gXULLog, LogLevel::Debug)) {
        nsCOMPtr<nsIURI> uri;
        mChannel->GetOriginalURI(getter_AddRefs(uri));

        MOZ_LOG(gXULLog, LogLevel::Debug,
                ("xul: %s loading overlay %s",
                 uri ? uri->GetSpecOrDefault().get() : "",
                 aURI->GetSpecOrDefault().get()));
    }

    if (aIsDynamic)
        mResolutionPhase = nsForwardReference::eStart;

    // Look in the prototype cache for the prototype document with
    // the specified overlay URI. Only use the cache if the containing
    // document is chrome otherwise it may not have a system principal and
    // the cached document will, see bug 565610.
    bool overlayIsChrome = IsChromeURI(aURI);
    bool documentIsChrome = mDocumentURI ?
        IsChromeURI(mDocumentURI) : false;
    mCurrentPrototype = overlayIsChrome && documentIsChrome ?
        nsXULPrototypeCache::GetInstance()->GetPrototype(aURI) : nullptr;

    // Same comment as nsChromeProtocolHandler::NewChannel and
    // XULDocument::StartDocumentLoad
    // - Ben Goodger
    //
    // We don't abort on failure here because there are too many valid
    // cases that can return failure, and the null-ness of |proto| is
    // enough to trigger the fail-safe parse-from-disk solution.
    // Example failure cases (for reference) include:
    //
    // NS_ERROR_NOT_AVAILABLE: the URI was not found in the FastLoad file,
    //                         parse from disk
    // other: the FastLoad file, XUL.mfl, could not be found, probably
    //        due to being accessed before a profile has been selected
    //        (e.g. loading chrome for the profile manager itself).
    //        The .xul file must be parsed from disk.

    bool useXULCache = nsXULPrototypeCache::GetInstance()->IsEnabled();
    if (useXULCache && mCurrentPrototype) {
        bool loaded;
        rv = mCurrentPrototype->AwaitLoadDone(this, &loaded);
        if (NS_FAILED(rv)) return rv;

        if (! loaded) {
            // Return to the main event loop and eagerly await the
            // prototype overlay load's completion. When the content
            // sink completes, it will trigger an EndLoad(), which'll
            // wind us back up here, in ResumeWalk().
            *aShouldReturn = true;
            return NS_OK;
        }

        MOZ_LOG(gXULLog, LogLevel::Debug, ("xul: overlay was cached"));

        // Found the overlay's prototype in the cache, fully loaded. If
        // this is a dynamic overlay, this will call ResumeWalk.
        // Otherwise, we'll return to ResumeWalk, which called us.
        return OnPrototypeLoadDone(aIsDynamic);
    }
    else {
        // Not there. Initiate a load.
        MOZ_LOG(gXULLog, LogLevel::Debug, ("xul: overlay was not cached"));

        if (mIsGoingAway) {
            MOZ_LOG(gXULLog, LogLevel::Debug, ("xul: ...and document already destroyed"));
            return NS_ERROR_NOT_AVAILABLE;
        }

        // We'll set the right principal on the proto doc when we get
        // OnStartRequest from the parser, so just pass in a null principal for
        // now.
        nsCOMPtr<nsIParser> parser;
        rv = PrepareToLoadPrototype(aURI, "view", nullptr, getter_AddRefs(parser));
        if (NS_FAILED(rv)) return rv;

        // Predicate mIsWritingFastLoad on the XUL cache being enabled,
        // so we don't have to re-check whether the cache is enabled all
        // the time.
        mIsWritingFastLoad = useXULCache;

        nsCOMPtr<nsIStreamListener> listener = do_QueryInterface(parser);
        if (! listener)
            return NS_ERROR_UNEXPECTED;

        // Add an observer to the parser; this'll get called when
        // Necko fires its On[Start|Stop]Request() notifications,
        // and will let us recover from a missing overlay.
        RefPtr<ParserObserver> parserObserver =
            new ParserObserver(this, mCurrentPrototype);
        parser->Parse(aURI, parserObserver);
        parserObserver = nullptr;

        nsCOMPtr<nsILoadGroup> group = do_QueryReferent(mDocumentLoadGroup);
        nsCOMPtr<nsIChannel> channel;
        // Set the owner of the channel to be our principal so
        // that the overlay's JSObjects etc end up being created
        // with the right principal and in the correct
        // compartment.
        rv = NS_NewChannel(getter_AddRefs(channel),
                           aURI,
                           NodePrincipal(),
                           nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_INHERITS |
                           nsILoadInfo::SEC_FORCE_INHERIT_PRINCIPAL,
                           nsIContentPolicy::TYPE_OTHER,
                           nullptr, // PerformanceStorage
                           group);

        if (NS_SUCCEEDED(rv)) {
            rv = channel->AsyncOpen2(listener);
        }

        if (NS_FAILED(rv)) {
            // Abandon this prototype
            mCurrentPrototype = nullptr;

            // The parser won't get an OnStartRequest and
            // OnStopRequest, so it needs a Terminate.
            parser->Terminate();

            // Just move on to the next overlay.
            ReportMissingOverlay(aURI);

            // XXX the error could indicate an internal error as well...
            *aFailureFromContent = true;
            return rv;
        }

        // If it's a 'chrome:' prototype document, then put it into
        // the prototype cache; other XUL documents will be reloaded
        // each time.  We must do this after AsyncOpen,
        // or chrome code will wrongly create a cached chrome channel
        // instead of a real one. Prototypes are only cached when the
        // document to be overlayed is chrome to avoid caching overlay
        // scripts with incorrect principals, see bug 565610.
        if (useXULCache && overlayIsChrome && documentIsChrome) {
            nsXULPrototypeCache::GetInstance()->PutPrototype(mCurrentPrototype);
        }

        // Return to the main event loop and eagerly await the
        // overlay load's completion. When the content sink
        // completes, it will trigger an EndLoad(), which'll wind
        // us back in ResumeWalk().
        if (!aIsDynamic)
            *aShouldReturn = true;
    }
    return NS_OK;
}

nsresult
XULDocument::ResumeWalk()
{
    // Walk the prototype and build the delegate content model. The
    // walk is performed in a top-down, left-to-right fashion. That
    // is, a parent is built before any of its children; a node is
    // only built after all of its siblings to the left are fully
    // constructed.
    //
    // It is interruptable so that transcluded documents (e.g.,
    // <html:script src="..." />) can be properly re-loaded if the
    // cached copy of the document becomes stale.
    nsresult rv;
    nsCOMPtr<nsIURI> overlayURI =
        mCurrentPrototype ? mCurrentPrototype->GetURI() : nullptr;

    while (1) {
        // Begin (or resume) walking the current prototype.

        while (mContextStack.Depth() > 0) {
            // Look at the top of the stack to determine what we're
            // currently working on.
            // This will always be a node already constructed and
            // inserted to the actual document.
            nsXULPrototypeElement* proto;
            nsCOMPtr<nsIContent> element;
            int32_t indx; // all children of proto before indx (not
                          // inclusive) have already been constructed
            rv = mContextStack.Peek(&proto, getter_AddRefs(element), &indx);
            if (NS_FAILED(rv)) return rv;

            if (indx >= (int32_t)proto->mChildren.Length()) {
                if (element) {
                    // We've processed all of the prototype's children. If
                    // we're in the master prototype, do post-order
                    // document-level hookup. (An overlay will get its
                    // document hookup done when it's successfully
                    // resolved.)
                    if (mState == eState_Master) {
                        AddElementToDocumentPost(element->AsElement());

                        if (element->NodeInfo()->Equals(nsGkAtoms::style,
                                                        kNameSpaceID_XHTML) ||
                            element->NodeInfo()->Equals(nsGkAtoms::style,
                                                        kNameSpaceID_SVG)) {
                            // XXX sucks that we have to do this -
                            // see bug 370111
                            nsCOMPtr<nsIStyleSheetLinkingElement> ssle =
                                do_QueryInterface(element);
                            NS_ASSERTION(ssle, "<html:style> doesn't implement "
                                               "nsIStyleSheetLinkingElement?");
                            Unused << ssle->UpdateStyleSheet(nullptr);
                        }
                    }
                }
                // Now pop the context stack back up to the parent
                // element and continue the prototype walk.
                mContextStack.Pop();
                continue;
            }

            // Grab the next child, and advance the current context stack
            // to the next sibling to our right.
            nsXULPrototypeNode* childproto = proto->mChildren[indx];
            mContextStack.SetTopIndex(++indx);

            // Whether we're in the "first ply" of an overlay:
            // the "hookup" nodes. In the case !processingOverlayHookupNodes,
            // we're in the master document -or- we're in an overlay, and far
            // enough down into the overlay's content that we can simply build
            // the delegates and attach them to the parent node.
            bool processingOverlayHookupNodes = (mState == eState_Overlay) &&
                                                  (mContextStack.Depth() == 1);

            NS_ASSERTION(element || processingOverlayHookupNodes,
                         "no element on context stack");

            switch (childproto->mType) {
            case nsXULPrototypeNode::eType_Element: {
                // An 'element', which may contain more content.
                nsXULPrototypeElement* protoele =
                    static_cast<nsXULPrototypeElement*>(childproto);

                RefPtr<Element> child;

                if (!processingOverlayHookupNodes) {
                    rv = CreateElementFromPrototype(protoele,
                                                    getter_AddRefs(child),
                                                    false);
                    if (NS_FAILED(rv)) return rv;

                    // ...and append it to the content model.
                    rv = element->AppendChildTo(child, false);
                    if (NS_FAILED(rv)) return rv;

                    // If we're only restoring persisted things on
                    // some elements, store the ID here to do that.
                    if (mRestrictPersistence) {
                        nsAtom* id = child->GetID();
                        if (id) {
                            mPersistenceIds.PutEntry(nsDependentAtomString(id));
                        }
                    }

                    // do pre-order document-level hookup, but only if
                    // we're in the master document. For an overlay,
                    // this will happen when the overlay is
                    // successfully resolved.
                    if (mState == eState_Master)
                        AddElementToDocumentPre(child);
                }
                else {
                    // We're in the "first ply" of an overlay: the
                    // "hookup" nodes. Create an 'overlay' element so
                    // that we can continue to build content, and
                    // enter a forward reference so we can hook it up
                    // later.
                    rv = CreateOverlayElement(protoele, getter_AddRefs(child));
                    if (NS_FAILED(rv)) return rv;
                }

                // If it has children, push the element onto the context
                // stack and begin to process them.
                if (protoele->mChildren.Length() > 0) {
                    rv = mContextStack.Push(protoele, child);
                    if (NS_FAILED(rv)) return rv;
                }
                else {
                    if (mState == eState_Master) {
                        // If there are no children, and we're in the
                        // master document, do post-order document hookup
                        // immediately.
                        AddElementToDocumentPost(child);
                    }
                }
            }
            break;

            case nsXULPrototypeNode::eType_Script: {
                // A script reference. Execute the script immediately;
                // this may have side effects in the content model.
                nsXULPrototypeScript* scriptproto =
                    static_cast<nsXULPrototypeScript*>(childproto);

                if (scriptproto->mSrcURI) {
                    // A transcluded script reference; this may
                    // "block" our prototype walk if the script isn't
                    // cached, or the cached copy of the script is
                    // stale and must be reloaded.
                    bool blocked;
                    rv = LoadScript(scriptproto, &blocked);
                    // If the script cannot be loaded, just keep going!

                    if (NS_SUCCEEDED(rv) && blocked)
                        return NS_OK;
                }
                else if (scriptproto->HasScriptObject()) {
                    // An inline script
                    rv = ExecuteScript(scriptproto);
                    if (NS_FAILED(rv)) return rv;
                }
            }
            break;

            case nsXULPrototypeNode::eType_Text: {
                // A simple text node.

                if (!processingOverlayHookupNodes) {
                    // This does mean that text nodes that are direct children
                    // of <overlay> get ignored.

                    RefPtr<nsTextNode> text =
                        new nsTextNode(mNodeInfoManager);

                    nsXULPrototypeText* textproto =
                        static_cast<nsXULPrototypeText*>(childproto);
                    text->SetText(textproto->mValue, false);

                    rv = element->AppendChildTo(text, false);
                    NS_ENSURE_SUCCESS(rv, rv);
                }
            }
            break;

            case nsXULPrototypeNode::eType_PI: {
                nsXULPrototypePI* piProto =
                    static_cast<nsXULPrototypePI*>(childproto);

                // <?xul-overlay?> and <?xml-stylesheet?> don't have effect
                // outside the prolog, like they used to. Issue a warning.

                if (piProto->mTarget.EqualsLiteral("xml-stylesheet") ||
                    piProto->mTarget.EqualsLiteral("xul-overlay")) {

                    const char16_t* params[] = { piProto->mTarget.get() };

                    nsContentUtils::ReportToConsole(
                                        nsIScriptError::warningFlag,
                                        NS_LITERAL_CSTRING("XUL Document"), nullptr,
                                        nsContentUtils::eXUL_PROPERTIES,
                                        "PINotInProlog",
                                        params, ArrayLength(params),
                                        overlayURI);
                }

                nsIContent* parent = processingOverlayHookupNodes ?
                    GetRootElement() : element.get();

                if (parent) {
                    // an inline script could have removed the root element
                    rv = CreateAndInsertPI(piProto, parent, nullptr);
                    NS_ENSURE_SUCCESS(rv, rv);
                }
            }
            break;

            default:
                NS_NOTREACHED("Unexpected nsXULPrototypeNode::Type value");
            }
        }

        // Once we get here, the context stack will have been
        // depleted. That means that the entire prototype has been
        // walked and content has been constructed.

        // If we're not already, mark us as now processing overlays.
        mState = eState_Overlay;

        // If there are no overlay URIs, then we're done.
        uint32_t count = mUnloadedOverlays.Length();
        if (! count)
            break;

        nsCOMPtr<nsIURI> uri = mUnloadedOverlays[count-1];
        mUnloadedOverlays.RemoveElementAt(count - 1);

        bool shouldReturn, failureFromContent;
        rv = LoadOverlayInternal(uri, false, &shouldReturn,
                                 &failureFromContent);
        if (failureFromContent)
            // The failure |rv| was the result of a problem in the content
            // rather than an unexpected problem in our implementation, so
            // just continue with the next overlay.
            continue;
        if (NS_FAILED(rv))
            return rv;
        if (mOverlayLoadObservers) {
            nsIObserver *obs = mOverlayLoadObservers->GetWeak(overlayURI);
            if (obs) {
                // This overlay has an unloaded overlay, so it will never
                // notify. The best we can do is to notify for the unloaded
                // overlay instead, assuming nobody is already notifiable
                // for it. Note that this will confuse the observer.
                if (!mOverlayLoadObservers->GetWeak(uri))
                    mOverlayLoadObservers->Put(uri, obs);
                mOverlayLoadObservers->Remove(overlayURI);
            }
        }
        if (shouldReturn)
            return NS_OK;
        overlayURI.swap(uri);
    }

    // If we get here, there is nothing left for us to walk. The content
    // model is built and ready for layout.
    rv = ResolveForwardReferences();
    if (NS_FAILED(rv)) return rv;

    ApplyPersistentAttributes();

    mStillWalking = false;
    if (mPendingSheets == 0) {
        rv = DoneWalking();
    }
    return rv;
}

already_AddRefed<nsIXULWindow>
XULDocument::GetXULWindowIfToplevelChrome() const
{
    nsCOMPtr<nsIDocShellTreeItem> item = GetDocShell();
    if (!item) {
        return nullptr;
    }
    nsCOMPtr<nsIDocShellTreeOwner> owner;
    item->GetTreeOwner(getter_AddRefs(owner));
    nsCOMPtr<nsIXULWindow> xulWin = do_GetInterface(owner);
    if (!xulWin) {
        return nullptr;
    }
    nsCOMPtr<nsIDocShell> xulWinShell;
    xulWin->GetDocShell(getter_AddRefs(xulWinShell));
    if (!SameCOMIdentity(xulWinShell, item)) {
        return nullptr;
    }
    return xulWin.forget();
}

nsresult
XULDocument::DoneWalking()
{
    MOZ_ASSERT(mPendingSheets == 0, "there are sheets to be loaded");
    MOZ_ASSERT(!mStillWalking, "walk not done");

    // XXXldb This is where we should really be setting the chromehidden
    // attribute.

    {
        mozAutoDocUpdate updateBatch(this, UPDATE_STYLE, true);
        uint32_t count = mOverlaySheets.Length();
        for (uint32_t i = 0; i < count; ++i) {
            AddStyleSheet(mOverlaySheets[i]);
        }
    }

    mOverlaySheets.Clear();

    if (!mDocumentLoaded) {
        // Make sure we don't reenter here from StartLayout().  Note that
        // setting mDocumentLoaded to true here means that if StartLayout()
        // causes ResumeWalk() to be reentered, we'll take the other branch of
        // the |if (!mDocumentLoaded)| check above and since
        // mInitialLayoutComplete will be false will follow the else branch
        // there too.  See the big comment there for how such reentry can
        // happen.
        mDocumentLoaded = true;

        NotifyPossibleTitleChange(false);

        nsContentUtils::DispatchTrustedEvent(
            this,
            static_cast<nsIDocument*>(this),
            NS_LITERAL_STRING("MozBeforeInitialXULLayout"),
            true,
            false);

        // Before starting layout, check whether we're a toplevel chrome
        // window.  If we are, setup some state so that we don't have to restyle
        // the whole tree after StartLayout.
        if (nsCOMPtr<nsIXULWindow> win = GetXULWindowIfToplevelChrome()) {
            // We're the chrome document!
            win->BeforeStartLayout();
        }

        StartLayout();

        if (mIsWritingFastLoad && IsChromeURI(mDocumentURI))
            nsXULPrototypeCache::GetInstance()->WritePrototype(mMasterPrototype);

        NS_ASSERTION(mDelayFrameLoaderInitialization,
                     "mDelayFrameLoaderInitialization should be true!");
        mDelayFrameLoaderInitialization = false;
        NS_WARNING_ASSERTION(
          mUpdateNestLevel == 0,
          "Constructing XUL document in middle of an update?");
        if (mUpdateNestLevel == 0) {
            MaybeInitializeFinalizeFrameLoaders();
        }

        NS_DOCUMENT_NOTIFY_OBSERVERS(EndLoad, (this));

        // DispatchContentLoadedEvents undoes the onload-blocking we
        // did in PrepareToWalk().
        DispatchContentLoadedEvents();

        mInitialLayoutComplete = true;

        // Walk the set of pending load notifications and notify any observers.
        // See below for detail.
        if (mPendingOverlayLoadNotifications) {
            nsInterfaceHashtable<nsURIHashKey,nsIObserver>* observers =
                mOverlayLoadObservers.get();
            for (auto iter = mPendingOverlayLoadNotifications->Iter();
                 !iter.Done();
                 iter.Next()) {
                nsIURI* aKey = iter.Key();
                iter.Data()->Observe(aKey, "xul-overlay-merged",
                                     EmptyString().get());

                if (observers) {
                  observers->Remove(aKey);
                }

                iter.Remove();
            }
        }
    }
    else {
        if (mOverlayLoadObservers) {
            nsCOMPtr<nsIURI> overlayURI = mCurrentPrototype->GetURI();
            nsCOMPtr<nsIObserver> obs;
            if (mInitialLayoutComplete) {
                // We have completed initial layout, so just send the notification.
                mOverlayLoadObservers->Get(overlayURI, getter_AddRefs(obs));
                if (obs)
                    obs->Observe(overlayURI, "xul-overlay-merged", EmptyString().get());
                mOverlayLoadObservers->Remove(overlayURI);
            }
            else {
                // If we have not yet displayed the document for the first time
                // (i.e. we came in here as the result of a dynamic overlay load
                // which was spawned by a binding-attached event caused by
                // StartLayout() on the master prototype - we must remember that
                // this overlay has been merged and tell the listeners after
                // StartLayout() is completely finished rather than doing so
                // immediately - otherwise we may be executing code that needs to
                // access XBL Binding implementations on nodes for which frames
                // have not yet been constructed because their bindings have not
                // yet been attached. This can be a race condition because dynamic
                // overlay loading can take varying amounts of time depending on
                // whether or not the overlay prototype is in the XUL cache. The
                // most likely effect of this bug is odd UI initialization due to
                // methods and properties that do not work.
                // XXXbz really, we shouldn't be firing binding constructors
                // until after StartLayout returns!

                if (!mPendingOverlayLoadNotifications) {
                    mPendingOverlayLoadNotifications =
                        new nsInterfaceHashtable<nsURIHashKey,nsIObserver>;
                }

                mPendingOverlayLoadNotifications->Get(overlayURI, getter_AddRefs(obs));
                if (!obs) {
                    mOverlayLoadObservers->Get(overlayURI, getter_AddRefs(obs));
                    NS_ASSERTION(obs, "null overlay load observer?");
                    mPendingOverlayLoadNotifications->Put(overlayURI, obs);
                }
            }
        }
    }

    return NS_OK;
}

NS_IMETHODIMP
XULDocument::StyleSheetLoaded(StyleSheet* aSheet,
                              bool aWasDeferred,
                              nsresult aStatus)
{
    if (!aWasDeferred) {
        // Don't care about when alternate sheets finish loading
        MOZ_ASSERT(mPendingSheets > 0,
                   "Unexpected StyleSheetLoaded notification");

        --mPendingSheets;

        if (!mStillWalking && mPendingSheets == 0) {
            return DoneWalking();
        }
    }

    return NS_OK;
}

void
XULDocument::MaybeBroadcast()
{
    // Only broadcast when not in an update and when safe to run scripts.
    if (mUpdateNestLevel == 0 &&
        (mDelayedAttrChangeBroadcasts.Length() ||
         mDelayedBroadcasters.Length())) {
        if (!nsContentUtils::IsSafeToRunScript()) {
            if (!mInDestructor) {
              nsContentUtils::AddScriptRunner(
                NewRunnableMethod("dom::XULDocument::MaybeBroadcast",
                                  this,
                                  &XULDocument::MaybeBroadcast));
            }
            return;
        }
        if (!mHandlingDelayedAttrChange) {
            mHandlingDelayedAttrChange = true;
            for (uint32_t i = 0; i < mDelayedAttrChangeBroadcasts.Length(); ++i) {
                nsAtom* attrName = mDelayedAttrChangeBroadcasts[i].mAttrName;
                if (mDelayedAttrChangeBroadcasts[i].mNeedsAttrChange) {
                    nsCOMPtr<Element> listener =
                        do_QueryInterface(mDelayedAttrChangeBroadcasts[i].mListener);
                    const nsString& value = mDelayedAttrChangeBroadcasts[i].mAttr;
                    if (mDelayedAttrChangeBroadcasts[i].mSetAttr) {
                        listener->SetAttr(kNameSpaceID_None, attrName, value,
                                          true);
                    } else {
                        listener->UnsetAttr(kNameSpaceID_None, attrName,
                                            true);
                    }
                }
                ExecuteOnBroadcastHandlerFor(mDelayedAttrChangeBroadcasts[i].mBroadcaster,
                                             mDelayedAttrChangeBroadcasts[i].mListener,
                                             attrName);
            }
            mDelayedAttrChangeBroadcasts.Clear();
            mHandlingDelayedAttrChange = false;
        }

        uint32_t length = mDelayedBroadcasters.Length();
        if (length) {
            bool oldValue = mHandlingDelayedBroadcasters;
            mHandlingDelayedBroadcasters = true;
            nsTArray<nsDelayedBroadcastUpdate> delayedBroadcasters;
            mDelayedBroadcasters.SwapElements(delayedBroadcasters);
            for (uint32_t i = 0; i < length; ++i) {
                SynchronizeBroadcastListener(delayedBroadcasters[i].mBroadcaster,
                                             delayedBroadcasters[i].mListener,
                                             delayedBroadcasters[i].mAttr);
            }
            mHandlingDelayedBroadcasters = oldValue;
        }
    }
}

void
XULDocument::EndUpdate(nsUpdateType aUpdateType)
{
    XMLDocument::EndUpdate(aUpdateType);

    MaybeBroadcast();
}

void
XULDocument::ReportMissingOverlay(nsIURI* aURI)
{
    MOZ_ASSERT(aURI, "Must have a URI");

    NS_ConvertUTF8toUTF16 utfSpec(aURI->GetSpecOrDefault());
    const char16_t* params[] = { utfSpec.get() };
    nsContentUtils::ReportToConsole(nsIScriptError::warningFlag,
                                    NS_LITERAL_CSTRING("XUL Document"), this,
                                    nsContentUtils::eXUL_PROPERTIES,
                                    "MissingOverlay",
                                    params, ArrayLength(params));
}

nsresult
XULDocument::LoadScript(nsXULPrototypeScript* aScriptProto, bool* aBlock)
{
    // Load a transcluded script
    nsresult rv;

    bool isChromeDoc = IsChromeURI(mDocumentURI);

    if (isChromeDoc && aScriptProto->HasScriptObject()) {
        rv = ExecuteScript(aScriptProto);

        // Ignore return value from execution, and don't block
        *aBlock = false;
        return NS_OK;
    }

    // Try the XUL script cache, in case two XUL documents source the same
    // .js file (e.g., strres.js from navigator.xul and utilityOverlay.xul).
    // XXXbe the cache relies on aScriptProto's GC root!
    bool useXULCache = nsXULPrototypeCache::GetInstance()->IsEnabled();

    if (isChromeDoc && useXULCache) {
        JSScript* newScriptObject =
            nsXULPrototypeCache::GetInstance()->GetScript(
                                   aScriptProto->mSrcURI);
        if (newScriptObject) {
            // The script language for a proto must remain constant - we
            // can't just change it for this unexpected language.
            aScriptProto->Set(newScriptObject);
        }

        if (aScriptProto->HasScriptObject()) {
            rv = ExecuteScript(aScriptProto);

            // Ignore return value from execution, and don't block
            *aBlock = false;
            return NS_OK;
        }
    }

    // Release script objects from FastLoad since we decided against using them
    aScriptProto->UnlinkJSObjects();

    // Set the current script prototype so that OnStreamComplete can report
    // the right file if there are errors in the script.
    NS_ASSERTION(!mCurrentScriptProto,
                 "still loading a script when starting another load?");
    mCurrentScriptProto = aScriptProto;

    if (isChromeDoc && aScriptProto->mSrcLoading) {
        // Another XULDocument load has started, which is still in progress.
        // Remember to ResumeWalk this document when the load completes.
        mNextSrcLoadWaiter = aScriptProto->mSrcLoadWaiters;
        aScriptProto->mSrcLoadWaiters = this;
        NS_ADDREF_THIS();
    }
    else {
        nsCOMPtr<nsILoadGroup> group = do_QueryReferent(mDocumentLoadGroup);

        // Note: the loader will keep itself alive while it's loading.
        nsCOMPtr<nsIStreamLoader> loader;
        rv = NS_NewStreamLoader(getter_AddRefs(loader),
                                aScriptProto->mSrcURI,
                                this, // aObserver
                                this, // aRequestingContext
                                nsILoadInfo::SEC_REQUIRE_SAME_ORIGIN_DATA_INHERITS,
                                nsIContentPolicy::TYPE_INTERNAL_SCRIPT,
                                group);

        if (NS_FAILED(rv)) {
            mCurrentScriptProto = nullptr;
            return rv;
        }

        aScriptProto->mSrcLoading = true;
    }

    // Block until OnStreamComplete resumes us.
    *aBlock = true;
    return NS_OK;
}

NS_IMETHODIMP
XULDocument::OnStreamComplete(nsIStreamLoader* aLoader,
                              nsISupports* context,
                              nsresult aStatus,
                              uint32_t stringLen,
                              const uint8_t* string)
{
    nsCOMPtr<nsIRequest> request;
    aLoader->GetRequest(getter_AddRefs(request));
    nsCOMPtr<nsIChannel> channel = do_QueryInterface(request);

#ifdef DEBUG
    // print a load error on bad status
    if (NS_FAILED(aStatus)) {
        if (channel) {
            nsCOMPtr<nsIURI> uri;
            channel->GetURI(getter_AddRefs(uri));
            if (uri) {
                printf("Failed to load %s\n", uri->GetSpecOrDefault().get());
            }
        }
    }
#endif

    // This is the completion routine that will be called when a
    // transcluded script completes. Compile and execute the script
    // if the load was successful, then continue building content
    // from the prototype.
    nsresult rv = aStatus;

    NS_ASSERTION(mCurrentScriptProto && mCurrentScriptProto->mSrcLoading,
                 "script source not loading on unichar stream complete?");
    if (!mCurrentScriptProto) {
        // XXX Wallpaper for bug 270042
        return NS_OK;
    }

    if (NS_SUCCEEDED(aStatus)) {
        // If the including XUL document is a FastLoad document, and we're
        // compiling an out-of-line script (one with src=...), then we must
        // be writing a new FastLoad file.  If we were reading this script
        // from the FastLoad file, XULContentSinkImpl::OpenScript (over in
        // nsXULContentSink.cpp) would have already deserialized a non-null
        // script->mScriptObject, causing control flow at the top of LoadScript
        // not to reach here.
        nsCOMPtr<nsIURI> uri = mCurrentScriptProto->mSrcURI;

        // XXX should also check nsIHttpChannel::requestSucceeded

        MOZ_ASSERT(!mOffThreadCompiling && (mOffThreadCompileStringLength == 0 &&
                                            !mOffThreadCompileStringBuf),
                   "XULDocument can't load multiple scripts at once");

        rv = ScriptLoader::ConvertToUTF16(channel, string, stringLen,
                                          EmptyString(), this,
                                          mOffThreadCompileStringBuf,
                                          mOffThreadCompileStringLength);
        if (NS_SUCCEEDED(rv)) {
            // Attempt to give ownership of the buffer to the JS engine.  If
            // we hit offthread compilation, however, we will have to take it
            // back below in order to keep the memory alive until compilation
            // completes.
            JS::SourceBufferHolder srcBuf(mOffThreadCompileStringBuf,
                                          mOffThreadCompileStringLength,
                                          JS::SourceBufferHolder::GiveOwnership);
            mOffThreadCompileStringBuf = nullptr;
            mOffThreadCompileStringLength = 0;

            rv = mCurrentScriptProto->Compile(srcBuf, uri, 1, this, this);
            if (NS_SUCCEEDED(rv) && !mCurrentScriptProto->HasScriptObject()) {
                // We will be notified via OnOffThreadCompileComplete when the
                // compile finishes. Keep the contents of the compiled script
                // alive until the compilation finishes.
                mOffThreadCompiling = true;
                // If the JS engine did not take the source buffer, then take
                // it back here to ensure it remains alive.
                mOffThreadCompileStringBuf = srcBuf.take();
                if (mOffThreadCompileStringBuf) {
                  mOffThreadCompileStringLength = srcBuf.length();
                }
                BlockOnload();
                return NS_OK;
            }
        }
    }

    return OnScriptCompileComplete(mCurrentScriptProto->GetScriptObject(), rv);
}

NS_IMETHODIMP
XULDocument::OnScriptCompileComplete(JSScript* aScript, nsresult aStatus)
{
    // When compiling off thread the script will not have been attached to the
    // script proto yet.
    if (aScript && !mCurrentScriptProto->HasScriptObject())
        mCurrentScriptProto->Set(aScript);

    // Allow load events to be fired once off thread compilation finishes.
    if (mOffThreadCompiling) {
        mOffThreadCompiling = false;
        UnblockOnload(false);
    }

    // After compilation finishes the script's characters are no longer needed.
    if (mOffThreadCompileStringBuf) {
      js_free(mOffThreadCompileStringBuf);
      mOffThreadCompileStringBuf = nullptr;
      mOffThreadCompileStringLength = 0;
    }

    // Clear mCurrentScriptProto now, but save it first for use below in
    // the execute code, and in the while loop that resumes walks of other
    // documents that raced to load this script.
    nsXULPrototypeScript* scriptProto = mCurrentScriptProto;
    mCurrentScriptProto = nullptr;

    // Clear the prototype's loading flag before executing the script or
    // resuming document walks, in case any of those control flows starts a
    // new script load.
    scriptProto->mSrcLoading = false;

    nsresult rv = aStatus;
    if (NS_SUCCEEDED(rv)) {
        rv = ExecuteScript(scriptProto);

        // If the XUL cache is enabled, save the script object there in
        // case different XUL documents source the same script.
        //
        // But don't save the script in the cache unless the master XUL
        // document URL is a chrome: URL.  It is valid for a URL such as
        // about:config to translate into a master document URL, whose
        // prototype document nodes -- including prototype scripts that
        // hold GC roots protecting their mJSObject pointers -- are not
        // cached in the XUL prototype cache.  See StartDocumentLoad,
        // the fillXULCache logic.
        //
        // A document such as about:config is free to load a script via
        // a URL such as chrome://global/content/config.js, and we must
        // not cache that script object without a prototype cache entry
        // containing a companion nsXULPrototypeScript node that owns a
        // GC root protecting the script object.  Otherwise, the script
        // cache entry will dangle once the uncached prototype document
        // is released when its owning XULDocument is unloaded.
        //
        // (See http://bugzilla.mozilla.org/show_bug.cgi?id=98207 for
        // the true crime story.)
        bool useXULCache = nsXULPrototypeCache::GetInstance()->IsEnabled();

        if (useXULCache && IsChromeURI(mDocumentURI) && scriptProto->HasScriptObject()) {
            JS::Rooted<JSScript*> script(RootingCx(), scriptProto->GetScriptObject());
            nsXULPrototypeCache::GetInstance()->PutScript(
                               scriptProto->mSrcURI, script);
        }

        if (mIsWritingFastLoad && mCurrentPrototype != mMasterPrototype) {
            // If we are loading an overlay script, try to serialize
            // it to the FastLoad file here.  Master scripts will be
            // serialized when the master prototype document gets
            // written, at the bottom of ResumeWalk.  That way, master
            // out-of-line scripts are serialized in the same order that
            // they'll be read, in the FastLoad file, which reduces the
            // number of seeks that dump the underlying stream's buffer.
            //
            // Ignore the return value, as we don't need to propagate
            // a failure to write to the FastLoad file, because this
            // method aborts that whole process on error.
            scriptProto->SerializeOutOfLine(nullptr, mCurrentPrototype);
        }
        // ignore any evaluation errors
    }

    rv = ResumeWalk();

    // Load a pointer to the prototype-script's list of XULDocuments who
    // raced to load the same script
    XULDocument** docp = &scriptProto->mSrcLoadWaiters;

    // Resume walking other documents that waited for this one's load, first
    // executing the script we just compiled, in each doc's script context
    XULDocument* doc;
    while ((doc = *docp) != nullptr) {
        NS_ASSERTION(doc->mCurrentScriptProto == scriptProto,
                     "waiting for wrong script to load?");
        doc->mCurrentScriptProto = nullptr;

        // Unlink doc from scriptProto's list before executing and resuming
        *docp = doc->mNextSrcLoadWaiter;
        doc->mNextSrcLoadWaiter = nullptr;

        if (aStatus == NS_BINDING_ABORTED && !scriptProto->HasScriptObject()) {
            // If the previous doc load was aborted, we want to try loading
            // again for the next doc. Otherwise, one abort would lead to all
            // subsequent waiting docs to abort as well.
            bool block = false;
            doc->LoadScript(scriptProto, &block);
            NS_RELEASE(doc);
            return rv;
        }

        // Execute only if we loaded and compiled successfully, then resume
        if (NS_SUCCEEDED(aStatus) && scriptProto->HasScriptObject()) {
            doc->ExecuteScript(scriptProto);
        }
        doc->ResumeWalk();
        NS_RELEASE(doc);
    }

    return rv;
}

nsresult
XULDocument::ExecuteScript(nsXULPrototypeScript *aScript)
{
    MOZ_ASSERT(aScript != nullptr, "null ptr");
    NS_ENSURE_TRUE(aScript, NS_ERROR_NULL_POINTER);
    NS_ENSURE_TRUE(mScriptGlobalObject, NS_ERROR_NOT_INITIALIZED);

    nsresult rv;
    rv = mScriptGlobalObject->EnsureScriptEnvironment();
    NS_ENSURE_SUCCESS(rv, rv);

    // Execute the precompiled script with the given version
    nsAutoMicroTask mt;

    // We're about to run script via JS::CloneAndExecuteScript, so we need an
    // AutoEntryScript. This is Gecko specific and not in any spec.
    AutoEntryScript aes(mScriptGlobalObject, "precompiled XUL <script> element");
    JSContext* cx = aes.cx();

    JS::Rooted<JSScript*> scriptObject(cx, aScript->GetScriptObject());
    NS_ENSURE_TRUE(scriptObject, NS_ERROR_UNEXPECTED);

    JS::Rooted<JSObject*> global(cx, JS::CurrentGlobalOrNull(cx));
    NS_ENSURE_TRUE(xpc::Scriptability::Get(global).Allowed(), NS_OK);

    JS::ExposeObjectToActiveJS(global);
    JSAutoCompartment ac(cx, global);

    // The script is in the compilation scope. Clone it into the target scope
    // and execute it. On failure, ~AutoScriptEntry will handle exceptions, so
    // there is no need to manually check the return value.
    JS::RootedValue rval(cx);
    JS::CloneAndExecuteScript(cx, scriptObject, &rval);

    return NS_OK;
}


nsresult
XULDocument::CreateElementFromPrototype(nsXULPrototypeElement* aPrototype,
                                        Element** aResult,
                                        bool aIsRoot)
{
    // Create a content model element from a prototype element.
    MOZ_ASSERT(aPrototype != nullptr, "null ptr");
    if (! aPrototype)
        return NS_ERROR_NULL_POINTER;

    *aResult = nullptr;
    nsresult rv = NS_OK;

    if (MOZ_LOG_TEST(gXULLog, LogLevel::Debug)) {
        MOZ_LOG(gXULLog, LogLevel::Debug,
               ("xul: creating <%s> from prototype",
                NS_ConvertUTF16toUTF8(aPrototype->mNodeInfo->QualifiedName()).get()));
    }

    RefPtr<Element> result;

    if (aPrototype->mNodeInfo->NamespaceEquals(kNameSpaceID_XUL)) {
        // If it's a XUL element, it'll be lightweight until somebody
        // monkeys with it.
        rv = nsXULElement::CreateFromPrototype(aPrototype, this, true, aIsRoot, getter_AddRefs(result));
        if (NS_FAILED(rv)) return rv;
    }
    else {
        // If it's not a XUL element, it's gonna be heavyweight no matter
        // what. So we need to copy everything out of the prototype
        // into the element.  Get a nodeinfo from our nodeinfo manager
        // for this node.
        RefPtr<mozilla::dom::NodeInfo> newNodeInfo;
        newNodeInfo = mNodeInfoManager->GetNodeInfo(aPrototype->mNodeInfo->NameAtom(),
                                                    aPrototype->mNodeInfo->GetPrefixAtom(),
                                                    aPrototype->mNodeInfo->NamespaceID(),
                                                    ELEMENT_NODE);
        if (!newNodeInfo) return NS_ERROR_OUT_OF_MEMORY;
        RefPtr<mozilla::dom::NodeInfo> xtfNi = newNodeInfo;
        rv = NS_NewElement(getter_AddRefs(result), newNodeInfo.forget(),
                           NOT_FROM_PARSER);
        if (NS_FAILED(rv))
            return rv;

        rv = AddAttributes(aPrototype, result);
        if (NS_FAILED(rv)) return rv;
    }

    result.forget(aResult);

    return NS_OK;
}

nsresult
XULDocument::CreateOverlayElement(nsXULPrototypeElement* aPrototype,
                                  Element** aResult)
{
    nsresult rv;

    RefPtr<Element> element;
    rv = CreateElementFromPrototype(aPrototype, getter_AddRefs(element), false);
    if (NS_FAILED(rv)) return rv;

    OverlayForwardReference* fwdref =
        new OverlayForwardReference(this, element);

    // transferring ownership to ya...
    rv = AddForwardReference(fwdref);
    if (NS_FAILED(rv)) return rv;

    element.forget(aResult);
    return NS_OK;
}

nsresult
XULDocument::AddAttributes(nsXULPrototypeElement* aPrototype,
                           Element* aElement)
{
    nsresult rv;

    for (uint32_t i = 0; i < aPrototype->mNumAttributes; ++i) {
        nsXULPrototypeAttribute* protoattr = &(aPrototype->mAttributes[i]);
        nsAutoString  valueStr;
        protoattr->mValue.ToString(valueStr);

        rv = aElement->SetAttr(protoattr->mName.NamespaceID(),
                               protoattr->mName.LocalName(),
                               protoattr->mName.GetPrefix(),
                               valueStr,
                               false);
        if (NS_FAILED(rv)) return rv;
    }

    return NS_OK;
}


nsresult
XULDocument::AddPrototypeSheets()
{
    nsresult rv;

    const nsCOMArray<nsIURI>& sheets = mCurrentPrototype->GetStyleSheetReferences();

    for (int32_t i = 0; i < sheets.Count(); i++) {
        nsCOMPtr<nsIURI> uri = sheets[i];

        RefPtr<StyleSheet> incompleteSheet;
        rv = CSSLoader()->LoadSheet(
          uri, mCurrentPrototype->DocumentPrincipal(), this, &incompleteSheet);

        // XXXldb We need to prevent bogus sheets from being held in the
        // prototype's list, but until then, don't propagate the failure
        // from LoadSheet (and thus exit the loop).
        if (NS_SUCCEEDED(rv)) {
            ++mPendingSheets;
            if (!mOverlaySheets.AppendElement(incompleteSheet)) {
                return NS_ERROR_OUT_OF_MEMORY;
            }
        }
    }

    return NS_OK;
}


//----------------------------------------------------------------------
//
// XULDocument::OverlayForwardReference
//

nsForwardReference::Result
XULDocument::OverlayForwardReference::Resolve()
{
    // Resolve a forward reference from an overlay element; attempt to
    // hook it up into the main document.
    nsresult rv;
    RefPtr<Element> target;

    nsIPresShell *shell = mDocument->GetShell();
    bool notify = shell && shell->DidInitialize();

    nsAutoString id;
    mOverlay->GetAttr(kNameSpaceID_None, nsGkAtoms::id, id);
    if (id.IsEmpty()) {
        // mOverlay is a direct child of <overlay> and has no id.
        // Insert it under the root element in the base document.
        Element* root = mDocument->GetRootElement();
        if (!root) {
            return eResolve_Error;
        }

        rv = XULDocument::InsertElement(root, mOverlay, notify);
        if (NS_FAILED(rv)) return eResolve_Error;

        target = mOverlay;
    }
    else {
        // The hook-up element has an id, try to match it with an element
        // with the same id in the base document.
        target = mDocument->GetElementById(id);

        // If we can't find the element in the document, defer the hookup
        // until later.
        if (!target)
            return eResolve_Later;

        rv = Merge(target, mOverlay, notify);
        if (NS_FAILED(rv)) return eResolve_Error;
    }

    // Check if 'target' is still in our document --- it might not be!
    if (!notify && target->GetUncomposedDoc() == mDocument) {
        // Add child and any descendants to the element map
        // XXX this is bogus, the content in 'target' might already be
        // in the document
        rv = mDocument->AddSubtreeToDocument(target);
        if (NS_FAILED(rv)) return eResolve_Error;
    }

    if (MOZ_LOG_TEST(gXULLog, LogLevel::Debug)) {
        nsAutoCString idC;
        LossyCopyUTF16toASCII(id, idC);
        MOZ_LOG(gXULLog, LogLevel::Debug,
               ("xul: overlay resolved '%s'",
                idC.get()));
    }

    mResolved = true;
    return eResolve_Succeeded;
}



nsresult
XULDocument::OverlayForwardReference::Merge(Element* aTargetElement,
                                            Element* aOverlayElement,
                                            bool aNotify)
{
    // This function is given:
    // aTargetElement:  the element in the document whose 'id' attribute
    //                  matches a toplevel node in our overlay.
    // aOverlayElement: the element in the overlay document that matches
    //                  an element in the actual document.
    // aNotify:         whether or not content manipulation methods should
    //                  use the aNotify parameter. After the initial
    //                  reflow (i.e. in the dynamic overlay merge case),
    //                  we want all the content manipulation methods we
    //                  call to notify so that frames are constructed
    //                  etc. Otherwise do not, since that's during initial
    //                  document construction before StartLayout has been
    //                  called which will do everything for us.
    //
    // This function merges the tree from the overlay into the tree in
    // the document, overwriting attributes and appending child content
    // nodes appropriately. (See XUL overlay reference for details)

    nsresult rv;

    // Merge attributes from the overlay content node to that of the
    // actual document.
    uint32_t i;
    const nsAttrName* name;
    for (i = 0; (name = aOverlayElement->GetAttrNameAt(i)); ++i) {
        // We don't want to swap IDs, they should be the same.
        if (name->Equals(nsGkAtoms::id))
            continue;

        // In certain cases merging command or observes is unsafe, so don't.
        if (!aNotify) {
            if (aTargetElement->NodeInfo()->Equals(nsGkAtoms::observes,
                                                   kNameSpaceID_XUL))
                continue;

            if (name->Equals(nsGkAtoms::observes) &&
                aTargetElement->HasAttr(kNameSpaceID_None, nsGkAtoms::observes))
                continue;

            if (name->Equals(nsGkAtoms::command) &&
                aTargetElement->HasAttr(kNameSpaceID_None, nsGkAtoms::command) &&
                !aTargetElement->NodeInfo()->Equals(nsGkAtoms::key,
                                                    kNameSpaceID_XUL) &&
                !aTargetElement->NodeInfo()->Equals(nsGkAtoms::menuitem,
                                                   kNameSpaceID_XUL))
                continue;
        }

        int32_t nameSpaceID = name->NamespaceID();
        nsAtom* attr = name->LocalName();
        nsAtom* prefix = name->GetPrefix();

        nsAutoString value;
        aOverlayElement->GetAttr(nameSpaceID, attr, value);

        // Element in the overlay has the 'removeelement' attribute set
        // so remove it from the actual document.
        if (attr == nsGkAtoms::removeelement && value.EqualsLiteral("true")) {
            nsCOMPtr<nsINode> parent = aTargetElement->GetParentNode();
            if (!parent) return NS_ERROR_FAILURE;
            parent->RemoveChildNode(aTargetElement, true);
            return NS_OK;
        }

        rv = aTargetElement->SetAttr(nameSpaceID, attr, prefix, value, aNotify);
        if (!NS_FAILED(rv) && !aNotify) {
            rv = mDocument->BroadcastAttributeChangeFromOverlay(
                    aTargetElement, nameSpaceID, attr, prefix, value);
        }
        if (NS_FAILED(rv)) return rv;
    }


    // Walk our child nodes, looking for elements that have the 'id'
    // attribute set. If we find any, we must do a parent check in the
    // actual document to ensure that the structure matches that of
    // the actual document. If it does, we can call ourselves and attempt
    // to merge inside that subtree. If not, we just append the tree to
    // the parent like any other.

    uint32_t childCount = aOverlayElement->GetChildCount();

    // This must be a strong reference since it will be the only
    // reference to a content object during part of this loop.
    nsCOMPtr<nsIContent> currContent;

    for (i = 0; i < childCount; ++i) {
        currContent = aOverlayElement->GetFirstChild();

        nsAtom *idAtom = currContent->GetID();

        Element* elementInDocument = nullptr;
        if (idAtom) {
            nsDependentAtomString id(idAtom);

            if (!id.IsEmpty()) {
                nsIDocument *doc = aTargetElement->GetUncomposedDoc();
                //XXXsmaug should we use ShadowRoot::GetElementById()
                //         if doc is null?
                if (!doc) return NS_ERROR_FAILURE;

                elementInDocument = doc->GetElementById(id);
            }
        }

        // The item has an 'id' attribute set, and we need to check with
        // the actual document to see if an item with this id exists at
        // this locale. If so, we want to merge the subtree under that
        // node. Otherwise, we just do an append as if the element had
        // no id attribute.
        if (elementInDocument) {
            // Given two parents, aTargetElement and aOverlayElement, we want
            // to call merge on currContent if we find an associated
            // node in the document with the same id as currContent that
            // also has aTargetNode as its parent.

            nsIContent* elementParent = elementInDocument->GetParent();

            nsAtom *parentID = elementParent->GetID();
            if (parentID && aTargetElement->GetID() == parentID) {
                // The element matches. "Go Deep!"
                //
                // Note that currContent is necessarily an element, because
                // elementInDocument can only be non-null when currContent has a
                // non-null ID.
                rv = Merge(elementInDocument, currContent->AsElement(), aNotify);
                if (NS_FAILED(rv)) return rv;
                nsIContent* firstChild = aOverlayElement->GetFirstChild();
                if (firstChild) {
                  aOverlayElement->RemoveChildNode(firstChild, false);
                }

                continue;
            }
        }

        nsIContent* firstChild = aOverlayElement->GetFirstChild();
        if (firstChild) {
          aOverlayElement->RemoveChildNode(firstChild, false);
        }

        rv = InsertElement(aTargetElement, currContent, aNotify);
        if (NS_FAILED(rv)) return rv;
    }

    return NS_OK;
}



XULDocument::OverlayForwardReference::~OverlayForwardReference()
{
    if (MOZ_LOG_TEST(gXULLog, LogLevel::Warning) && !mResolved) {
        nsAutoString id;
        mOverlay->GetAttr(kNameSpaceID_None, nsGkAtoms::id, id);

        nsAutoCString idC;
        LossyCopyUTF16toASCII(id, idC);

        nsIURI *protoURI = mDocument->mCurrentPrototype->GetURI();

        nsCOMPtr<nsIURI> docURI;
        mDocument->mChannel->GetOriginalURI(getter_AddRefs(docURI));

        MOZ_LOG(gXULLog, LogLevel::Warning,
               ("xul: %s overlay failed to resolve '%s' in %s",
                protoURI->GetSpecOrDefault().get(), idC.get(),
                docURI ? docURI->GetSpecOrDefault().get() : ""));
    }
}


//----------------------------------------------------------------------
//
// XULDocument::BroadcasterHookup
//

nsForwardReference::Result
XULDocument::BroadcasterHookup::Resolve()
{
    nsresult rv;

    bool listener;
    rv = mDocument->CheckBroadcasterHookup(mObservesElement, &listener, &mResolved);
    if (NS_FAILED(rv)) return eResolve_Error;

    return mResolved ? eResolve_Succeeded : eResolve_Later;
}


XULDocument::BroadcasterHookup::~BroadcasterHookup()
{
    if (MOZ_LOG_TEST(gXULLog, LogLevel::Warning) && !mResolved) {
        // Tell the world we failed

        nsAutoString broadcasterID;
        nsAutoString attribute;

        if (mObservesElement->IsXULElement(nsGkAtoms::observes)) {
            mObservesElement->GetAttr(kNameSpaceID_None, nsGkAtoms::element, broadcasterID);
            mObservesElement->GetAttr(kNameSpaceID_None, nsGkAtoms::attribute, attribute);
        }
        else {
            mObservesElement->GetAttr(kNameSpaceID_None, nsGkAtoms::observes, broadcasterID);
            attribute.Assign('*');
        }

        nsAutoCString attributeC,broadcasteridC;
        LossyCopyUTF16toASCII(attribute, attributeC);
        LossyCopyUTF16toASCII(broadcasterID, broadcasteridC);
        MOZ_LOG(gXULLog, LogLevel::Warning,
               ("xul: broadcaster hookup failed <%s attribute='%s'> to %s",
                nsAtomCString(mObservesElement->NodeInfo()->NameAtom()).get(),
                attributeC.get(),
                broadcasteridC.get()));
    }
}

//----------------------------------------------------------------------

nsresult
XULDocument::BroadcastAttributeChangeFromOverlay(nsIContent* aNode,
                                                 int32_t aNameSpaceID,
                                                 nsAtom* aAttribute,
                                                 nsAtom* aPrefix,
                                                 const nsAString& aValue)
{
    nsresult rv = NS_OK;

    if (!mBroadcasterMap || !CanBroadcast(aNameSpaceID, aAttribute))
        return rv;

    if (!aNode->IsElement())
        return rv;

    auto entry = static_cast<BroadcasterMapEntry*>
                            (mBroadcasterMap->Search(aNode->AsElement()));
    if (!entry)
        return rv;

    // We've got listeners: push the value.
    for (size_t i = entry->mListeners.Length() - 1; i != (size_t)-1; --i) {
        BroadcastListener* bl = entry->mListeners[i];

        if ((bl->mAttribute != aAttribute) &&
            (bl->mAttribute != nsGkAtoms::_asterisk))
            continue;

        nsCOMPtr<Element> l = do_QueryReferent(bl->mListener);
        if (l) {
            rv = l->SetAttr(aNameSpaceID, aAttribute,
                            aPrefix, aValue, false);
            if (NS_FAILED(rv)) return rv;
        }
    }
    return rv;
}

nsresult
XULDocument::FindBroadcaster(Element* aElement,
                             Element** aListener,
                             nsString& aBroadcasterID,
                             nsString& aAttribute,
                             Element** aBroadcaster)
{
    mozilla::dom::NodeInfo *ni = aElement->NodeInfo();
    *aListener = nullptr;
    *aBroadcaster = nullptr;

    if (ni->Equals(nsGkAtoms::observes, kNameSpaceID_XUL)) {
        // It's an <observes> element, which means that the actual
        // listener is the _parent_ node. This element should have an
        // 'element' attribute that specifies the ID of the
        // broadcaster element, and an 'attribute' element, which
        // specifies the name of the attribute to observe.
        nsIContent* parent = aElement->GetParent();
        if (!parent) {
             // <observes> is the root element
            return NS_FINDBROADCASTER_NOT_FOUND;
        }

        // If we're still parented by an 'overlay' tag, then we haven't
        // made it into the real document yet. Defer hookup.
        if (parent->NodeInfo()->Equals(nsGkAtoms::overlay,
                                       kNameSpaceID_XUL)) {
            return NS_FINDBROADCASTER_AWAIT_OVERLAYS;
        }

        *aListener = parent->IsElement() ? parent->AsElement() : nullptr;
        NS_IF_ADDREF(*aListener);

        aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::element, aBroadcasterID);
        if (aBroadcasterID.IsEmpty()) {
            return NS_FINDBROADCASTER_NOT_FOUND;
        }
        aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::attribute, aAttribute);
    }
    else {
        // It's a generic element, which means that we'll use the
        // value of the 'observes' attribute to determine the ID of
        // the broadcaster element, and we'll watch _all_ of its
        // values.
        aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::observes, aBroadcasterID);

        // Bail if there's no aBroadcasterID
        if (aBroadcasterID.IsEmpty()) {
            // Try the command attribute next.
            aElement->GetAttr(kNameSpaceID_None, nsGkAtoms::command, aBroadcasterID);
            if (!aBroadcasterID.IsEmpty()) {
                // We've got something in the command attribute.  We
                // only treat this as a normal broadcaster if we are
                // not a menuitem or a key.

                if (ni->Equals(nsGkAtoms::menuitem, kNameSpaceID_XUL) ||
                    ni->Equals(nsGkAtoms::key, kNameSpaceID_XUL)) {
                return NS_FINDBROADCASTER_NOT_FOUND;
              }
            }
            else {
              return NS_FINDBROADCASTER_NOT_FOUND;
            }
        }

        *aListener = aElement;
        NS_ADDREF(*aListener);

        aAttribute.Assign('*');
    }

    // Make sure we got a valid listener.
    NS_ENSURE_TRUE(*aListener, NS_ERROR_UNEXPECTED);

    // Try to find the broadcaster element in the document.
    *aBroadcaster = GetElementById(aBroadcasterID);

    // If we can't find the broadcaster, then we'll need to defer the
    // hookup. We may need to resolve some of the other overlays
    // first.
    if (! *aBroadcaster) {
        return NS_FINDBROADCASTER_AWAIT_OVERLAYS;
    }

    NS_ADDREF(*aBroadcaster);

    return NS_FINDBROADCASTER_FOUND;
}

nsresult
XULDocument::CheckBroadcasterHookup(Element* aElement,
                                    bool* aNeedsHookup,
                                    bool* aDidResolve)
{
    // Resolve a broadcaster hookup. Look at the element that we're
    // trying to resolve: it could be an '<observes>' element, or just
    // a vanilla element with an 'observes' attribute on it.
    nsresult rv;

    *aDidResolve = false;

    nsCOMPtr<Element> listener;
    nsAutoString broadcasterID;
    nsAutoString attribute;
    nsCOMPtr<Element> broadcaster;

    rv = FindBroadcaster(aElement, getter_AddRefs(listener),
                         broadcasterID, attribute, getter_AddRefs(broadcaster));
    switch (rv) {
        case NS_FINDBROADCASTER_NOT_FOUND:
            *aNeedsHookup = false;
            return NS_OK;
        case NS_FINDBROADCASTER_AWAIT_OVERLAYS:
            *aNeedsHookup = true;
            return NS_OK;
        case NS_FINDBROADCASTER_FOUND:
            break;
        default:
            return rv;
    }

    NS_ENSURE_ARG(broadcaster && listener);
    ErrorResult domRv;
    AddBroadcastListenerFor(*broadcaster, *listener, attribute, domRv);
    if (domRv.Failed()) {
        return domRv.StealNSResult();
    }

    // Tell the world we succeeded
    if (MOZ_LOG_TEST(gXULLog, LogLevel::Debug)) {
        nsCOMPtr<nsIContent> content =
            do_QueryInterface(listener);

        NS_ASSERTION(content != nullptr, "not an nsIContent");
        if (! content)
            return rv;

        nsAutoCString attributeC,broadcasteridC;
        LossyCopyUTF16toASCII(attribute, attributeC);
        LossyCopyUTF16toASCII(broadcasterID, broadcasteridC);
        MOZ_LOG(gXULLog, LogLevel::Debug,
               ("xul: broadcaster hookup <%s attribute='%s'> to %s",
                nsAtomCString(content->NodeInfo()->NameAtom()).get(),
                attributeC.get(),
                broadcasteridC.get()));
    }

    *aNeedsHookup = false;
    *aDidResolve = true;
    return NS_OK;
}

nsresult
XULDocument::InsertElement(nsINode* aParent, nsIContent* aChild, bool aNotify)
{
    // Insert aChild appropriately into aParent, accounting for a
    // 'pos' attribute set on aChild.

    nsAutoString posStr;
    bool wasInserted = false;

    // insert after an element of a given id
    if (aChild->IsElement()) {
        aChild->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::insertafter, posStr);
    }

    bool isInsertAfter = true;
    if (posStr.IsEmpty()) {
        if (aChild->IsElement()) {
            aChild->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::insertbefore, posStr);
        }
        isInsertAfter = false;
    }

    if (!posStr.IsEmpty()) {
        nsIDocument *document = aParent->OwnerDoc();

        nsIContent *content = nullptr;

        char* str = ToNewCString(posStr);
        char* rest;
        char* token = nsCRT::strtok(str, ", ", &rest);

        while (token) {
            content = document->GetElementById(NS_ConvertASCIItoUTF16(token));
            if (content)
                break;

            token = nsCRT::strtok(rest, ", ", &rest);
        }
        free(str);

        if (content) {
            if (content->GetParent() == aParent) {
                nsIContent* nodeToInsertBefore =
                  isInsertAfter ? content->GetNextSibling() : content;
                nsresult rv = aParent->InsertChildBefore(aChild,
                                                         nodeToInsertBefore,
                                                         aNotify);
                if (NS_FAILED(rv)) {
                    return rv;
                }

                wasInserted = true;
            }
        }
    }

    if (!wasInserted) {
        if (aChild->IsElement() &&
            aChild->AsElement()->GetAttr(kNameSpaceID_None, nsGkAtoms::position, posStr) &&
            !posStr.IsEmpty()) {
            nsresult rv;
            // Positions are one-indexed.
            int32_t pos = posStr.ToInteger(&rv);
            // Note: if the insertion index (which is |pos - 1|) would be less
            // than 0 or greater than the number of children aParent has, then
            // don't insert, since the position is bogus.  Just skip on to
            // appending.
            if (NS_SUCCEEDED(rv) && pos > 0 &&
                uint32_t(pos - 1) <= aParent->GetChildCount()) {
                nsIContent* nodeToInsertBefore =
                    aParent->GetChildAt_Deprecated(pos - 1);
                rv = aParent->InsertChildBefore(aChild, nodeToInsertBefore,
                                                aNotify);
                if (NS_SUCCEEDED(rv))
                    wasInserted = true;
                // If the insertion fails, then we should still
                // attempt an append.  Thus, rather than returning rv
                // immediately, we fall through to the final
                // "catch-all" case that just does an AppendChildTo.
            }
        }
    }

    if (!wasInserted) {
        return aParent->AppendChildTo(aChild, aNotify);
    }
    return NS_OK;
}

//----------------------------------------------------------------------
//
// CachedChromeStreamListener
//

XULDocument::CachedChromeStreamListener::CachedChromeStreamListener(XULDocument* aDocument, bool aProtoLoaded)
    : mDocument(aDocument),
      mProtoLoaded(aProtoLoaded)
{
}


XULDocument::CachedChromeStreamListener::~CachedChromeStreamListener()
{
}


NS_IMPL_ISUPPORTS(XULDocument::CachedChromeStreamListener,
                  nsIRequestObserver, nsIStreamListener)

NS_IMETHODIMP
XULDocument::CachedChromeStreamListener::OnStartRequest(nsIRequest *request,
                                                        nsISupports* acontext)
{
    return NS_ERROR_PARSED_DATA_CACHED;
}


NS_IMETHODIMP
XULDocument::CachedChromeStreamListener::OnStopRequest(nsIRequest *request,
                                                       nsISupports* aContext,
                                                       nsresult aStatus)
{
    if (! mProtoLoaded)
        return NS_OK;

    return mDocument->OnPrototypeLoadDone(true);
}


NS_IMETHODIMP
XULDocument::CachedChromeStreamListener::OnDataAvailable(nsIRequest *request,
                                                         nsISupports* aContext,
                                                         nsIInputStream* aInStr,
                                                         uint64_t aSourceOffset,
                                                         uint32_t aCount)
{
    NS_NOTREACHED("CachedChromeStream doesn't receive data");
    return NS_ERROR_UNEXPECTED;
}

//----------------------------------------------------------------------
//
// ParserObserver
//

XULDocument::ParserObserver::ParserObserver(XULDocument* aDocument,
                                            nsXULPrototypeDocument* aPrototype)
    : mDocument(aDocument), mPrototype(aPrototype)
{
}

XULDocument::ParserObserver::~ParserObserver()
{
}

NS_IMPL_ISUPPORTS(XULDocument::ParserObserver, nsIRequestObserver)

NS_IMETHODIMP
XULDocument::ParserObserver::OnStartRequest(nsIRequest *request,
                                            nsISupports* aContext)
{
    // Guard against buggy channels calling OnStartRequest multiple times.
    if (mPrototype) {
        nsCOMPtr<nsIChannel> channel = do_QueryInterface(request);
        nsIScriptSecurityManager* secMan = nsContentUtils::GetSecurityManager();
        if (channel && secMan) {
            nsCOMPtr<nsIPrincipal> principal;
            secMan->GetChannelResultPrincipal(channel, getter_AddRefs(principal));

            principal = mDocument->MaybeDowngradePrincipal(principal);
            // Failure there is ok -- it'll just set a (safe) null principal
            mPrototype->SetDocumentPrincipal(principal);
        }

        // Make sure to avoid cycles
        mPrototype = nullptr;
    }

    return NS_OK;
}

NS_IMETHODIMP
XULDocument::ParserObserver::OnStopRequest(nsIRequest *request,
                                           nsISupports* aContext,
                                           nsresult aStatus)
{
    nsresult rv = NS_OK;

    if (NS_FAILED(aStatus)) {
        // If an overlay load fails, we need to nudge the prototype
        // walk along.
        nsCOMPtr<nsIChannel> aChannel = do_QueryInterface(request);
        if (aChannel) {
            nsCOMPtr<nsIURI> uri;
            aChannel->GetOriginalURI(getter_AddRefs(uri));
            if (uri) {
                mDocument->ReportMissingOverlay(uri);
            }
        }

        rv = mDocument->ResumeWalk();
    }

    // Drop the reference to the document to break cycle between the
    // document, the parser, the content sink, and the parser
    // observer.
    mDocument = nullptr;

    return rv;
}

already_AddRefed<nsPIWindowRoot>
XULDocument::GetWindowRoot()
{
  if (!mDocumentContainer) {
    return nullptr;
  }

    nsCOMPtr<nsPIDOMWindowOuter> piWin = mDocumentContainer->GetWindow();
    return piWin ? piWin->GetTopWindowRoot() : nullptr;
}

bool
XULDocument::IsDocumentRightToLeft()
{
    // setting the localedir attribute on the root element forces a
    // specific direction for the document.
    Element* element = GetRootElement();
    if (element) {
        static Element::AttrValuesArray strings[] =
            {&nsGkAtoms::ltr, &nsGkAtoms::rtl, nullptr};
        switch (element->FindAttrValueIn(kNameSpaceID_None, nsGkAtoms::localedir,
                                         strings, eCaseMatters)) {
            case 0: return false;
            case 1: return true;
            default: break; // otherwise, not a valid value, so fall through
        }
    }

    // otherwise, get the locale from the chrome registry and
    // look up the intl.uidirection.<locale> preference
    nsCOMPtr<nsIXULChromeRegistry> reg =
        mozilla::services::GetXULChromeRegistryService();
    if (!reg)
        return false;

    nsAutoCString package;
    bool isChrome;
    if (NS_SUCCEEDED(mDocumentURI->SchemeIs("chrome", &isChrome)) &&
        isChrome) {
        mDocumentURI->GetHostPort(package);
    }
    else {
        // use the 'global' package for about and resource uris.
        // otherwise, just default to left-to-right.
        bool isAbout, isResource;
        if (NS_SUCCEEDED(mDocumentURI->SchemeIs("about", &isAbout)) &&
            isAbout) {
            package.AssignLiteral("global");
        }
        else if (NS_SUCCEEDED(mDocumentURI->SchemeIs("resource", &isResource)) &&
            isResource) {
            package.AssignLiteral("global");
        }
        else {
            return false;
        }
    }

    bool isRTL = false;
    reg->IsLocaleRTL(package, &isRTL);
    return isRTL;
}

void
XULDocument::ResetDocumentDirection()
{
    DocumentStatesChanged(NS_DOCUMENT_STATE_RTL_LOCALE);
}

void
XULDocument::DirectionChanged(const char* aPrefName, void* aData)
{
  // Reset the direction and restyle the document if necessary.
  XULDocument* doc = (XULDocument *)aData;
  if (doc) {
      doc->ResetDocumentDirection();
  }
}

nsIDocument::DocumentTheme
XULDocument::GetDocumentLWTheme()
{
    if (mDocLWTheme == Doc_Theme_Uninitialized) {
        mDocLWTheme = ThreadSafeGetDocumentLWTheme();
    }
    return mDocLWTheme;
}

nsIDocument::DocumentTheme
XULDocument::ThreadSafeGetDocumentLWTheme() const
{
    if (mDocLWTheme != Doc_Theme_Uninitialized) {
        return mDocLWTheme;
    }

    DocumentTheme theme = Doc_Theme_None; // No lightweight theme by default
    Element* element = GetRootElement();
    nsAutoString hasLWTheme;
    if (element &&
        element->GetAttr(kNameSpaceID_None, nsGkAtoms::lwtheme, hasLWTheme) &&
        !(hasLWTheme.IsEmpty()) &&
        hasLWTheme.EqualsLiteral("true")) {
        theme = Doc_Theme_Neutral;
        nsAutoString lwTheme;
        element->GetAttr(kNameSpaceID_None, nsGkAtoms::lwthemetextcolor, lwTheme);
        if (!(lwTheme.IsEmpty())) {
            if (lwTheme.EqualsLiteral("dark"))
                theme = Doc_Theme_Dark;
            else if (lwTheme.EqualsLiteral("bright"))
                theme = Doc_Theme_Bright;
        }
    }
    return theme;
}

JSObject*
XULDocument::WrapNode(JSContext *aCx, JS::Handle<JSObject*> aGivenProto)
{
  return XULDocumentBinding::Wrap(aCx, this, aGivenProto);
}

} // namespace dom
} // namespace mozilla
