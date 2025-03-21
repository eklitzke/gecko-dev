/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AutoplayPolicy.h"

#include "mozilla/EventStateManager.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/HTMLMediaElement.h"
#include "mozilla/dom/HTMLMediaElementBinding.h"
#include "nsContentUtils.h"
#include "nsIDocument.h"
#include "MediaManager.h"

namespace mozilla {
namespace dom {

/* static */ bool
AutoplayPolicy::IsMediaElementAllowedToPlay(NotNull<HTMLMediaElement*> aElement)
{
  if (Preferences::GetBool("media.autoplay.enabled")) {
    return true;
  }

  // Pages which have been granted permission to capture WebRTC camera or
  // microphone are assumed to be trusted, and are allowed to autoplay.
  MediaManager* manager = MediaManager::GetIfExists();
  if (manager) {
    nsCOMPtr<nsPIDOMWindowInner> window = aElement->OwnerDoc()->GetInnerWindow();
    if (window && manager->IsActivelyCapturingOrHasAPermission(window->WindowID())) {
      return true;
    }
  }

  // TODO : this old way would be removed when user-gestures-needed becomes
  // as a default option to block autoplay.
  if (!Preferences::GetBool("media.autoplay.enabled.user-gestures-needed", false)) {
    // If elelement is blessed, it would always be allowed to play().
    return aElement->IsBlessed() ||
           EventStateManager::IsHandlingUserInput();
  }

  // Muted content
  if (aElement->Volume() == 0.0 || aElement->Muted()) {
    return true;
  }

  // Media has already loaded metadata and doesn't contain audio track
  if (aElement->IsVideo() &&
      aElement->ReadyState() >= HTMLMediaElementBinding::HAVE_METADATA &&
      !aElement->HasAudio()) {
    return true;
  }

  // Whitelisted.
  if (nsContentUtils::IsExactSitePermAllow(
        aElement->NodePrincipal(), "autoplay-media")) {
    return true;
  }

  // Activated by user gesture.
  if (aElement->OwnerDoc()->HasBeenUserActivated()) {
    return true;
  }

  return false;
}

} // namespace dom
} // namespace mozilla
