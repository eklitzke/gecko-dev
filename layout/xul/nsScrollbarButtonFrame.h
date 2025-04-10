/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**

  Eric D Vaughan
  This class lays out its children either vertically or horizontally

**/

#ifndef nsScrollbarButtonFrame_h___
#define nsScrollbarButtonFrame_h___

#include "mozilla/Attributes.h"
#include "nsButtonBoxFrame.h"
#include "nsITimer.h"
#include "nsRepeatService.h"

class nsScrollbarButtonFrame final : public nsButtonBoxFrame
{
public:
  NS_DECL_FRAMEARENA_HELPERS(nsScrollbarButtonFrame)

  explicit nsScrollbarButtonFrame(ComputedStyle* aStyle):
    nsButtonBoxFrame(aStyle, kClassID), mCursorOnThis(false) {}

  // Overrides
  virtual void DestroyFrom(nsIFrame* aDestructRoot, PostDestroyData& aPostDestroyData) override;

  friend nsIFrame* NS_NewScrollbarButtonFrame(nsIPresShell* aPresShell, ComputedStyle* aStyle);

  virtual nsresult HandleEvent(nsPresContext* aPresContext,
                               mozilla::WidgetGUIEvent* aEvent,
                               nsEventStatus* aEventStatus) override;

  static nsresult GetChildWithTag(nsAtom* atom, nsIFrame* start, nsIFrame*& result);
  static nsresult GetParentWithTag(nsAtom* atom, nsIFrame* start, nsIFrame*& result);

  bool HandleButtonPress(nsPresContext* aPresContext,
                         mozilla::WidgetGUIEvent* aEvent,
                         nsEventStatus* aEventStatus);

  NS_IMETHOD HandleMultiplePress(nsPresContext* aPresContext,
                                 mozilla::WidgetGUIEvent* aEvent,
                                 nsEventStatus* aEventStatus,
                                 bool aControlHeld) override
 {
   return NS_OK;
 }

  NS_IMETHOD HandleDrag(nsPresContext* aPresContext,
                        mozilla::WidgetGUIEvent* aEvent,
                        nsEventStatus* aEventStatus) override
  {
    return NS_OK;
  }

  NS_IMETHOD HandleRelease(nsPresContext* aPresContext,
                           mozilla::WidgetGUIEvent* aEvent,
                           nsEventStatus* aEventStatus) override;

protected:
  virtual void MouseClicked(mozilla::WidgetGUIEvent* aEvent) override;

  void StartRepeat() {
    nsRepeatService::GetInstance()->Start(Notify, this,
                                          mContent->OwnerDoc(),
                                          NS_LITERAL_CSTRING("nsScrollbarButtonFrame"));
  }
  void StopRepeat() {
    nsRepeatService::GetInstance()->Stop(Notify, this);
  }
  void Notify();
  static void Notify(void* aData) {
    static_cast<nsScrollbarButtonFrame*>(aData)->Notify();
  }

  bool mCursorOnThis;
};

#endif
