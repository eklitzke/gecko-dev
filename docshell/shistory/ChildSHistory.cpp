/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ChildSHistory.h"
#include "mozilla/dom/ChildSHistoryBinding.h"
#include "nsIMessageManager.h"
#include "nsComponentManagerUtils.h"
#include "nsSHistory.h"
#include "nsDocShell.h"
#include "nsISHEntry.h"
#include "nsXULAppAPI.h"

namespace mozilla {
namespace dom {

ChildSHistory::ChildSHistory(nsDocShell* aDocShell)
  : mDocShell(aDocShell)
  , mHistory(new nsSHistory())
{
  mHistory->SetRootDocShell(mDocShell);
}

ChildSHistory::~ChildSHistory()
{
}

int32_t
ChildSHistory::Count()
{
  int32_t count;
  mHistory->GetCount(&count);
  return count;
}

int32_t
ChildSHistory::Index()
{
  int32_t index;
  mHistory->GetIndex(&index);
  return index;
}

void
ChildSHistory::Reload(uint32_t aReloadFlags, ErrorResult& aRv)
{
  aRv = mHistory->Reload(aReloadFlags);
}

bool
ChildSHistory::CanGo(int32_t aOffset)
{
  CheckedInt<int32_t> index = Index();
  index += aOffset;
  if (!index.isValid()) {
    return false;
  }
  return index.value() < Count() && index.value() >= 0;
}

void
ChildSHistory::Go(int32_t aOffset, ErrorResult& aRv)
{
  // XXX(nika): Should we turn Go(-1) and Go(1) to call GoForward and GoBack?
  // They technically fire different change events but I couldn't find anyone
  // who cares, so I'm inclined not to.
  CheckedInt<int32_t> index = Index();
  index += aOffset;
  if (!index.isValid()) {
    aRv.Throw(NS_ERROR_FAILURE);
    return;
  }
  aRv = mHistory->GotoIndex(index.value());
}

void
ChildSHistory::EvictLocalContentViewers()
{
  mHistory->EvictAllContentViewers();
}

nsISHistory*
ChildSHistory::LegacySHistory()
{
  return mHistory;
}

nsISHistoryInternal*
ChildSHistory::LegacySHistoryInternal()
{
  return mHistory;
}

nsIWebNavigation*
ChildSHistory::LegacySHistoryWebNav()
{
  return mHistory;
}

ParentSHistory*
ChildSHistory::GetParentIfSameProcess()
{
  if (XRE_IsContentProcess()) {
    return nullptr;
  }

  MOZ_CRASH("Unimplemented!");
}

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ChildSHistory)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(ChildSHistory)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ChildSHistory)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(ChildSHistory,
                                      mDocShell,
                                      mHistory)

JSObject*
ChildSHistory::WrapObject(JSContext* cx, JS::Handle<JSObject*> aGivenProto)
{
  return ChildSHistoryBinding::Wrap(cx, this, aGivenProto);
}

nsISupports*
ChildSHistory::GetParentObject() const
{
  // We want to get the TabChildGlobal, which is the
  // nsIContentFrameMessageManager on mDocShell.
  nsCOMPtr<nsIContentFrameMessageManager> mm =
    do_GetInterface(static_cast<nsIDocShell*>(mDocShell));
  return mm;
}

} // namespace dom
} // namespace mozilla
