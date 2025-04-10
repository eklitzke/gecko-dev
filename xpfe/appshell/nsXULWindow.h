/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsXULWindow_h__
#define nsXULWindow_h__

// Local Includes
#include "nsChromeTreeOwner.h"
#include "nsContentTreeOwner.h"

// Helper classes
#include "nsCOMPtr.h"
#include "nsTArray.h"
#include "nsString.h"
#include "nsWeakReference.h"
#include "nsCOMArray.h"
#include "nsRect.h"
#include "Units.h"

// Interfaces needed
#include "nsIBaseWindow.h"
#include "nsIDocShell.h"
#include "nsIDocShellTreeItem.h"
#include "nsIDOMWindow.h"
#include "nsIInterfaceRequestor.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIXULWindow.h"
#include "nsIPrompt.h"
#include "nsIAuthPrompt.h"
#include "nsIXULBrowserWindow.h"
#include "nsIWeakReference.h"
#include "nsIWidgetListener.h"
#include "nsITabParent.h"
#include "nsIXULStore.h"

namespace mozilla {
namespace dom {
class Element;
} // namespace dom
} // namespace mozilla

class nsAtom;

// nsXULWindow

#define NS_XULWINDOW_IMPL_CID                         \
{ /* 8eaec2f3-ed02-4be2-8e0f-342798477298 */          \
     0x8eaec2f3,                                      \
     0xed02,                                          \
     0x4be2,                                          \
   { 0x8e, 0x0f, 0x34, 0x27, 0x98, 0x47, 0x72, 0x98 } \
}

class nsContentShellInfo;

class nsXULWindow : public nsIBaseWindow,
                    public nsIInterfaceRequestor,
                    public nsIXULWindow,
                    public nsSupportsWeakReference
{
friend class nsChromeTreeOwner;
friend class nsContentTreeOwner;

public:
   NS_DECL_THREADSAFE_ISUPPORTS

   NS_DECL_NSIINTERFACEREQUESTOR
   NS_DECL_NSIXULWINDOW
   NS_DECL_NSIBASEWINDOW

   NS_DECLARE_STATIC_IID_ACCESSOR(NS_XULWINDOW_IMPL_CID)

   void LockUntilChromeLoad() { mLockedUntilChromeLoad = true; }
   bool IsLocked() const { return mLockedUntilChromeLoad; }
   void IgnoreXULSizeMode(bool aEnable) { mIgnoreXULSizeMode = aEnable; }
   void WasRegistered() { mRegistered = true; }

protected:
   enum persistentAttributes {
     PAD_MISC =         0x1,
     PAD_POSITION =     0x2,
     PAD_SIZE =         0x4
   };

   explicit nsXULWindow(uint32_t aChromeFlags);
   virtual ~nsXULWindow();

   NS_IMETHOD EnsureChromeTreeOwner();
   NS_IMETHOD EnsureContentTreeOwner();
   NS_IMETHOD EnsurePrimaryContentTreeOwner();
   NS_IMETHOD EnsurePrompter();
   NS_IMETHOD EnsureAuthPrompter();
   NS_IMETHOD ForceRoundedDimensions();
   NS_IMETHOD GetAvailScreenSize(int32_t* aAvailWidth, int32_t* aAvailHeight);

   void ApplyChromeFlags();
   void SizeShell();
   void OnChromeLoaded();
   void StaggerPosition(int32_t &aRequestedX, int32_t &aRequestedY,
                        int32_t aSpecWidth, int32_t aSpecHeight);
   bool       LoadPositionFromXUL(int32_t aSpecWidth, int32_t aSpecHeight);
   bool       LoadSizeFromXUL(int32_t& aSpecWidth, int32_t& aSpecHeight);
   void       SetSpecifiedSize(int32_t aSpecWidth, int32_t aSpecHeight);
   bool       LoadMiscPersistentAttributesFromXUL();
   void       SyncAttributesToWidget();
   NS_IMETHOD SavePersistentAttributes();

   NS_IMETHOD GetWindowDOMWindow(mozIDOMWindowProxy** aDOMWindow);
   mozilla::dom::Element* GetWindowDOMElement() const;

   // See nsIDocShellTreeOwner for docs on next two methods
   nsresult ContentShellAdded(nsIDocShellTreeItem* aContentShell,
                              bool aPrimary);
   nsresult ContentShellRemoved(nsIDocShellTreeItem* aContentShell);
   NS_IMETHOD GetPrimaryContentSize(int32_t* aWidth,
                                    int32_t* aHeight);
   NS_IMETHOD SetPrimaryContentSize(int32_t aWidth,
                                    int32_t aHeight);
   nsresult GetRootShellSize(int32_t* aWidth,
                             int32_t* aHeight);
   nsresult SetRootShellSize(int32_t aWidth,
                             int32_t aHeight);

   NS_IMETHOD SizeShellTo(nsIDocShellTreeItem* aShellItem, int32_t aCX,
      int32_t aCY);
   NS_IMETHOD ExitModalLoop(nsresult aStatus);
   NS_IMETHOD CreateNewChromeWindow(int32_t aChromeFlags, nsITabParent* aOpeningTab, mozIDOMWindowProxy* aOpenerWindow, nsIXULWindow **_retval);
   NS_IMETHOD CreateNewContentWindow(int32_t aChromeFlags,
                                     nsITabParent* aOpeningTab,
                                     mozIDOMWindowProxy* aOpenerWindow,
                                     uint64_t aNextTabParentId,
                                     nsIXULWindow **_retval);
   NS_IMETHOD GetHasPrimaryContent(bool* aResult);

   void       EnableParent(bool aEnable);
   bool       ConstrainToZLevel(bool aImmediate, nsWindowZ *aPlacement,
                                nsIWidget *aReqBelow, nsIWidget **aActualBelow);
   void       PlaceWindowLayersBehind(uint32_t aLowLevel, uint32_t aHighLevel,
                                      nsIXULWindow *aBehind);
   void       SetContentScrollbarVisibility(bool aVisible);
   bool       GetContentScrollbarVisibility();
   void       PersistentAttributesDirty(uint32_t aDirtyFlags);
   nsresult   GetTabCount(uint32_t* aResult);

   void       LoadPersistentWindowState();
   nsresult   GetPersistentValue(const nsAtom* aAttr,
                                 nsAString& aValue);
   nsresult   SetPersistentValue(const nsAtom* aAttr,
                                 const nsAString& aValue);

   nsChromeTreeOwner*      mChromeTreeOwner;
   nsContentTreeOwner*     mContentTreeOwner;
   nsContentTreeOwner*     mPrimaryContentTreeOwner;
   nsCOMPtr<nsIWidget>     mWindow;
   nsCOMPtr<nsIDocShell>   mDocShell;
   nsCOMPtr<nsPIDOMWindowOuter>  mDOMWindow;
   nsCOMPtr<nsIWeakReference> mParentWindow;
   nsCOMPtr<nsIPrompt>     mPrompter;
   nsCOMPtr<nsIAuthPrompt> mAuthPrompter;
   nsCOMPtr<nsIXULBrowserWindow> mXULBrowserWindow;
   nsCOMPtr<nsIDocShellTreeItem> mPrimaryContentShell;
   nsresult                mModalStatus;
   bool                    mContinueModalLoop;
   bool                    mDebuting;       // being made visible right now
   bool                    mChromeLoaded; // True when chrome has loaded
   bool                    mPersistentWindowStateLoaded;
   bool                    mSizingShellFromXUL; // true when in SizeShell()
   bool                    mShowAfterLoad;
   bool                    mIntrinsicallySized;
   bool                    mCenterAfterLoad;
   bool                    mIsHiddenWindow;
   bool                    mLockedUntilChromeLoad;
   bool                    mIgnoreXULSize;
   bool                    mIgnoreXULPosition;
   bool                    mChromeFlagsFrozen;
   bool                    mIgnoreXULSizeMode;
   // mDestroying is used to prevent reentry into into Destroy(), which can
   // otherwise happen due to script running as we tear down various things.
   bool                    mDestroying;
   bool                    mRegistered;
   uint32_t                mPersistentAttributesDirty; // persistentAttributes
   uint32_t                mPersistentAttributesMask;
   uint32_t                mChromeFlags;
   uint64_t                mNextTabParentId;
   nsString                mTitle;
   nsIntRect               mOpenerScreenRect; // the screen rect of the opener

   nsCOMPtr<nsITabParent> mPrimaryTabParent;
private:
   // GetPrimaryTabParentSize is called from xpidl methods and we don't have a
   // good way to annotate those with MOZ_CAN_RUN_SCRIPT yet.  It takes no
   // refcounted args other than "this", and the "this" uses seem ok.
   MOZ_CAN_RUN_SCRIPT_BOUNDARY nsresult GetPrimaryTabParentSize(int32_t* aWidth, int32_t* aHeight);
   nsresult GetPrimaryContentShellSize(int32_t* aWidth, int32_t* aHeight);
   nsresult SetPrimaryTabParentSize(int32_t aWidth, int32_t aHeight);
   nsCOMPtr<nsIXULStore> mLocalStore;
};

NS_DEFINE_STATIC_IID_ACCESSOR(nsXULWindow, NS_XULWINDOW_IMPL_CID)
#endif /* nsXULWindow_h__ */
