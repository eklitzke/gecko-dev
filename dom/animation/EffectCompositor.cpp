/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "EffectCompositor.h"

#include <bitset>
#include <initializer_list>

#include "mozilla/dom/Animation.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/KeyframeEffect.h"
#include "mozilla/AnimationComparator.h"
#include "mozilla/AnimationPerformanceWarning.h"
#include "mozilla/AnimationTarget.h"
#include "mozilla/AnimationUtils.h"
#include "mozilla/AutoRestore.h"
#include "mozilla/ComputedStyleInlines.h"
#include "mozilla/EffectSet.h"
#include "mozilla/LayerAnimationInfo.h"
#include "mozilla/RestyleManager.h"
#include "mozilla/ServoBindings.h" // Servo_GetProperties_Overriding_Animation
#include "mozilla/ServoStyleSet.h"
#include "mozilla/StyleAnimationValue.h"
#include "mozilla/TypeTraits.h" // For Forward<>
#include "nsContentUtils.h"
#include "nsCSSPseudoElements.h"
#include "nsCSSPropertyIDSet.h"
#include "nsCSSProps.h"
#include "nsAtom.h"
#include "nsIPresShell.h"
#include "nsIPresShellInlines.h"
#include "nsLayoutUtils.h"
#include "nsTArray.h"
#include "PendingAnimationTracker.h"

using mozilla::dom::Animation;
using mozilla::dom::Element;
using mozilla::dom::KeyframeEffect;

namespace mozilla {

NS_IMPL_CYCLE_COLLECTION_CLASS(EffectCompositor)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(EffectCompositor)
  for (auto& elementSet : tmp->mElementsToRestyle) {
    elementSet.Clear();
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(EffectCompositor)
  for (auto& elementSet : tmp->mElementsToRestyle) {
    for (auto iter = elementSet.Iter(); !iter.Done(); iter.Next()) {
      CycleCollectionNoteChild(cb, iter.Key().mElement,
                               "EffectCompositor::mElementsToRestyle[]",
                               cb.Flags());
    }
  }
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(EffectCompositor, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(EffectCompositor, Release)

namespace {
enum class MatchForCompositor {
  // This animation matches and should run on the compositor if possible.
  Yes,
  // This (not currently playing) animation matches and can be run on the
  // compositor if there are other animations for this property that return
  // 'Yes'.
  IfNeeded,
  // This animation does not match or can't be run on the compositor.
  No,
  // This animation does not match or can't be run on the compositor and,
  // furthermore, its presence means we should not run any animations for this
  // property on the compositor.
  NoAndBlockThisProperty
};
}

static MatchForCompositor
IsMatchForCompositor(const KeyframeEffect& aEffect,
                     nsCSSPropertyID aProperty,
                     const nsIFrame* aFrame)
{
  const Animation* animation = aEffect.GetAnimation();
  MOZ_ASSERT(animation);

  if (!animation->IsRelevant()) {
    return MatchForCompositor::No;
  }

  AnimationPerformanceWarning::Type warningType;
  if (animation->ShouldBeSynchronizedWithMainThread(aProperty, aFrame,
                                                    warningType)) {
    EffectCompositor::SetPerformanceWarning(
      aFrame, aProperty,
      AnimationPerformanceWarning(warningType));
    // For a given |aFrame|, we don't want some animations of |aProperty| to
    // run on the compositor and others to run on the main thread, so if any
    // need to be synchronized with the main thread, run them all there.
    return MatchForCompositor::NoAndBlockThisProperty;
  }

  if (!aEffect.HasEffectiveAnimationOfProperty(aProperty)) {
    return MatchForCompositor::No;
  }

  return animation->IsPlaying()
         ? MatchForCompositor::Yes
         : MatchForCompositor::IfNeeded;
}

// Helper function to factor out the common logic from
// GetAnimationsForCompositor and HasAnimationsForCompositor.
//
// Takes an optional array to fill with eligible animations.
//
// Returns true if there are eligible animations, false otherwise.
bool
FindAnimationsForCompositor(const nsIFrame* aFrame,
                            nsCSSPropertyID aProperty,
                            nsTArray<RefPtr<dom::Animation>>* aMatches /*out*/)
{
  MOZ_ASSERT(!aMatches || aMatches->IsEmpty(),
             "Matches array, if provided, should be empty");

  EffectSet* effects = EffectSet::GetEffectSet(aFrame);
  if (!effects || effects->IsEmpty()) {
    return false;
  }

  // First check for newly-started transform animations that should be
  // synchronized with geometric animations. We need to do this before any
  // other early returns (the one above is ok) since we can only check this
  // state when the animation is newly-started.
  if (aProperty == eCSSProperty_transform) {
    PendingAnimationTracker* tracker =
      aFrame->PresContext()->Document()->GetPendingAnimationTracker();
    if (tracker) {
      tracker->MarkAnimationsThatMightNeedSynchronization();
    }
  }

  // If the property will be added to the animations level of the cascade but
  // there is an !important rule for that property in the cascade then the
  // animation will not be applied since the !important rule overrides it.
  if (effects->PropertiesWithImportantRules().HasProperty(aProperty) &&
      effects->PropertiesForAnimationsLevel().HasProperty(aProperty)) {
    return false;
  }

  if (aFrame->RefusedAsyncAnimation()) {
    return false;
  }

  // The animation cascade will almost always be up-to-date by this point
  // but there are some cases such as when we are restoring the refresh driver
  // from test control after seeking where it might not be the case.
  //
  // Those cases are probably not important but just to be safe, let's make
  // sure the cascade is up to date since if it *is* up to date, this is
  // basically a no-op.
  Maybe<NonOwningAnimationTarget> pseudoElement =
    EffectCompositor::GetAnimationElementAndPseudoForFrame(aFrame);
  MOZ_ASSERT(pseudoElement,
             "We have a valid element for the frame, if we don't we should "
             "have bailed out at above the call to EffectSet::GetEffectSet");
  EffectCompositor::MaybeUpdateCascadeResults(pseudoElement->mElement,
                                              pseudoElement->mPseudoType);

  if (!nsLayoutUtils::AreAsyncAnimationsEnabled()) {
    if (nsLayoutUtils::IsAnimationLoggingEnabled()) {
      nsCString message;
      message.AppendLiteral("Performance warning: Async animations are "
                            "disabled");
      AnimationUtils::LogAsyncAnimationFailure(message);
    }
    return false;
  }

  // Disable async animations if we have a rendering observer that
  // depends on our content (svg masking, -moz-element etc) so that
  // it gets updated correctly.
  nsIContent* content = aFrame->GetContent();
  while (content) {
    if (content->HasRenderingObservers()) {
      EffectCompositor::SetPerformanceWarning(
        aFrame, aProperty,
        AnimationPerformanceWarning(
          AnimationPerformanceWarning::Type::HasRenderingObserver));
      return false;
    }
    content = content->GetParent();
  }

  bool foundRunningAnimations = false;
  for (KeyframeEffect* effect : *effects) {
    MatchForCompositor matchResult =
      IsMatchForCompositor(*effect, aProperty, aFrame);

    if (matchResult == MatchForCompositor::NoAndBlockThisProperty) {
      // For a given |aFrame|, we don't want some animations of |aProperty| to
      // run on the compositor and others to run on the main thread, so if any
      // need to be synchronized with the main thread, run them all there.
      if (aMatches) {
        aMatches->Clear();
      }
      return false;
    }

    if (matchResult == MatchForCompositor::No) {
      continue;
    }

    if (aMatches) {
      aMatches->AppendElement(effect->GetAnimation());
    }

    if (matchResult == MatchForCompositor::Yes) {
      foundRunningAnimations = true;
    }
  }

  // If all animations we added were not currently playing animations, don't
  // send them to the compositor.
  if (aMatches && !foundRunningAnimations) {
    aMatches->Clear();
  }

  MOZ_ASSERT(!foundRunningAnimations || !aMatches || !aMatches->IsEmpty(),
             "If return value is true, matches array should be non-empty");

  if (aMatches && foundRunningAnimations) {
    aMatches->Sort(AnimationPtrComparator<RefPtr<dom::Animation>>());
  }

  return foundRunningAnimations;
}

void
EffectCompositor::RequestRestyle(dom::Element* aElement,
                                 CSSPseudoElementType aPseudoType,
                                 RestyleType aRestyleType,
                                 CascadeLevel aCascadeLevel)
{
  if (!mPresContext) {
    // Pres context will be null after the effect compositor is disconnected.
    return;
  }

  // Ignore animations on orphaned elements and elements in documents without
  // a pres shell (e.g. XMLHttpRequest responseXML documents).
  if (!nsContentUtils::GetPresShellForContent(aElement)) {
    return;
  }

  auto& elementsToRestyle = mElementsToRestyle[aCascadeLevel];
  PseudoElementHashEntry::KeyType key = { aElement, aPseudoType };

  if (aRestyleType == RestyleType::Throttled) {
    elementsToRestyle.LookupForAdd(key).OrInsert([]() { return false; });
    mPresContext->PresShell()->SetNeedThrottledAnimationFlush();
  } else {
    bool skipRestyle;
    // Update hashtable first in case PostRestyleForAnimation mutates it.
    // (It shouldn't, but just to be sure.)
    if (auto p = elementsToRestyle.LookupForAdd(key)) {
      skipRestyle = p.Data();
      p.Data() = true;
    } else {
      skipRestyle = false;
      p.OrInsert([]() { return true; });
    }

    if (!skipRestyle) {
      PostRestyleForAnimation(aElement, aPseudoType, aCascadeLevel);
    }
  }

  if (aRestyleType == RestyleType::Layer) {
    mPresContext->RestyleManager()->IncrementAnimationGeneration();
    EffectSet* effectSet =
      EffectSet::GetEffectSet(aElement, aPseudoType);
    if (effectSet) {
      effectSet->UpdateAnimationGeneration(mPresContext);
    }
  }
}

void
EffectCompositor::PostRestyleForAnimation(dom::Element* aElement,
                                          CSSPseudoElementType aPseudoType,
                                          CascadeLevel aCascadeLevel)
{
  if (!mPresContext) {
    return;
  }

  dom::Element* element = GetElementToRestyle(aElement, aPseudoType);
  if (!element) {
    return;
  }

  nsRestyleHint hint = aCascadeLevel == CascadeLevel::Transitions ?
                                        eRestyle_CSSTransitions :
                                        eRestyle_CSSAnimations;

  MOZ_ASSERT(NS_IsMainThread(),
             "Restyle request during restyling should be requested only on "
             "the main-thread. e.g. after the parallel traversal");
  if (ServoStyleSet::IsInServoTraversal() || mIsInPreTraverse) {
    MOZ_ASSERT(hint == eRestyle_CSSAnimations ||
               hint == eRestyle_CSSTransitions);

    // We can't call Servo_NoteExplicitHints here since AtomicRefCell does not
    // allow us mutate ElementData of the |aElement| in SequentialTask.
    // Instead we call Servo_NoteExplicitHints for the element in PreTraverse()
    // which will be called right before the second traversal that we do for
    // updating CSS animations.
    // In that case PreTraverse() will return true so that we know to do the
    // second traversal so we don't need to post any restyle requests to the
    // PresShell.
    return;
  }

  MOZ_ASSERT(!mPresContext->RestyleManager()->IsInStyleRefresh());

  mPresContext->PresShell()->RestyleForAnimation(element, hint);
}

void
EffectCompositor::PostRestyleForThrottledAnimations()
{
  for (size_t i = 0; i < kCascadeLevelCount; i++) {
    CascadeLevel cascadeLevel = CascadeLevel(i);
    auto& elementSet = mElementsToRestyle[cascadeLevel];

    for (auto iter = elementSet.Iter(); !iter.Done(); iter.Next()) {
      bool& postedRestyle = iter.Data();
      if (postedRestyle) {
        continue;
      }

      PostRestyleForAnimation(iter.Key().mElement,
                              iter.Key().mPseudoType,
                              cascadeLevel);
      postedRestyle = true;
    }
  }
}

void
EffectCompositor::ClearRestyleRequestsFor(Element* aElement)
{
  MOZ_ASSERT(aElement);

  auto& elementsToRestyle = mElementsToRestyle[CascadeLevel::Animations];

  CSSPseudoElementType pseudoType = aElement->GetPseudoElementType();
  if (pseudoType == CSSPseudoElementType::NotPseudo) {
    PseudoElementHashEntry::KeyType notPseudoKey =
      { aElement, CSSPseudoElementType::NotPseudo };
    PseudoElementHashEntry::KeyType beforePseudoKey =
      { aElement, CSSPseudoElementType::before };
    PseudoElementHashEntry::KeyType afterPseudoKey =
      { aElement, CSSPseudoElementType::after };

    elementsToRestyle.Remove(notPseudoKey);
    elementsToRestyle.Remove(beforePseudoKey);
    elementsToRestyle.Remove(afterPseudoKey);
  } else if (pseudoType == CSSPseudoElementType::before ||
             pseudoType == CSSPseudoElementType::after) {
    Element* parentElement = aElement->GetParentElement();
    MOZ_ASSERT(parentElement);
    PseudoElementHashEntry::KeyType key = { parentElement, pseudoType };
    elementsToRestyle.Remove(key);
  }
}

void
EffectCompositor::UpdateEffectProperties(const ComputedStyle* aStyle,
                                         Element* aElement,
                                         CSSPseudoElementType aPseudoType)
{
  EffectSet* effectSet = EffectSet::GetEffectSet(aElement, aPseudoType);
  if (!effectSet) {
    return;
  }

  // Style context (Gecko) or computed values (Stylo) change might cause CSS
  // cascade level, e.g removing !important, so we should update the cascading
  // result.
  effectSet->MarkCascadeNeedsUpdate();

  for (KeyframeEffect* effect : *effectSet) {
    effect->UpdateProperties(aStyle);
  }
}


namespace {
  class EffectCompositeOrderComparator {
  public:
    bool Equals(const KeyframeEffect* a, const KeyframeEffect* b) const
    {
      return a == b;
    }

    bool LessThan(const KeyframeEffect* a, const KeyframeEffect* b) const
    {
      MOZ_ASSERT(a->GetAnimation() && b->GetAnimation());
      MOZ_ASSERT(
        Equals(a, b) ||
        a->GetAnimation()->HasLowerCompositeOrderThan(*b->GetAnimation()) !=
          b->GetAnimation()->HasLowerCompositeOrderThan(*a->GetAnimation()));
      return a->GetAnimation()->HasLowerCompositeOrderThan(*b->GetAnimation());
    }
  };
}

bool
EffectCompositor::GetServoAnimationRule(
  const dom::Element* aElement,
  CSSPseudoElementType aPseudoType,
  CascadeLevel aCascadeLevel,
  RawServoAnimationValueMapBorrowedMut aAnimationValues)
{
  MOZ_ASSERT(aAnimationValues);
  MOZ_ASSERT(mPresContext && mPresContext->IsDynamic(),
             "Should not be in print preview");
  // Gecko_GetAnimationRule should have already checked this
  MOZ_ASSERT(nsContentUtils::GetPresShellForContent(aElement),
             "Should not be trying to run animations on elements in documents"
             " without a pres shell (e.g. XMLHttpRequest documents)");

  EffectSet* effectSet = EffectSet::GetEffectSet(aElement, aPseudoType);
  if (!effectSet) {
    return false;
  }

  // Get a list of effects sorted by composite order.
  nsTArray<KeyframeEffect*> sortedEffectList(effectSet->Count());
  for (KeyframeEffect* effect : *effectSet) {
    sortedEffectList.AppendElement(effect);
  }
  sortedEffectList.Sort(EffectCompositeOrderComparator());

  // If multiple animations affect the same property, animations with higher
  // composite order (priority) override or add or animations with lower
  // priority.
  const nsCSSPropertyIDSet propertiesToSkip =
    aCascadeLevel == CascadeLevel::Animations
      ? effectSet->PropertiesForAnimationsLevel().Inverse()
      : effectSet->PropertiesForAnimationsLevel();
  for (KeyframeEffect* effect : sortedEffectList) {
    effect->GetAnimation()->ComposeStyle(*aAnimationValues, propertiesToSkip);
  }

  MOZ_ASSERT(effectSet == EffectSet::GetEffectSet(aElement, aPseudoType),
             "EffectSet should not change while composing style");

  return true;
}

/* static */ dom::Element*
EffectCompositor::GetElementToRestyle(dom::Element* aElement,
                                      CSSPseudoElementType aPseudoType)
{
  if (aPseudoType == CSSPseudoElementType::NotPseudo) {
    return aElement;
  }

  if (aPseudoType == CSSPseudoElementType::before) {
    return nsLayoutUtils::GetBeforePseudo(aElement);
  }

  if (aPseudoType == CSSPseudoElementType::after) {
    return nsLayoutUtils::GetAfterPseudo(aElement);
  }

  NS_NOTREACHED("Should not try to get the element to restyle for a pseudo "
                "other that :before or :after");
  return nullptr;
}

bool
EffectCompositor::HasPendingStyleUpdates() const
{
  for (auto& elementSet : mElementsToRestyle) {
    if (elementSet.Count()) {
      return true;
    }
  }

  return false;
}


/* static */ bool
EffectCompositor::HasAnimationsForCompositor(const nsIFrame* aFrame,
                                             nsCSSPropertyID aProperty)
{
  return FindAnimationsForCompositor(aFrame, aProperty, nullptr);
}

/* static */ nsTArray<RefPtr<dom::Animation>>
EffectCompositor::GetAnimationsForCompositor(const nsIFrame* aFrame,
                                             nsCSSPropertyID aProperty)
{
  nsTArray<RefPtr<dom::Animation>> result;

#ifdef DEBUG
  bool foundSome =
#endif
    FindAnimationsForCompositor(aFrame, aProperty, &result);
  MOZ_ASSERT(!foundSome || !result.IsEmpty(),
             "If return value is true, matches array should be non-empty");

  return result;
}

/* static */ void
EffectCompositor::ClearIsRunningOnCompositor(const nsIFrame *aFrame,
                                             nsCSSPropertyID aProperty)
{
  EffectSet* effects = EffectSet::GetEffectSet(aFrame);
  if (!effects) {
    return;
  }

  for (KeyframeEffect* effect : *effects) {
    effect->SetIsRunningOnCompositor(aProperty, false);
  }
}

/* static */ void
EffectCompositor::MaybeUpdateCascadeResults(Element* aElement,
                                            CSSPseudoElementType aPseudoType)
{
  EffectSet* effects = EffectSet::GetEffectSet(aElement, aPseudoType);
  if (!effects || !effects->CascadeNeedsUpdate()) {
    return;
  }

  UpdateCascadeResults(*effects, aElement, aPseudoType);

  MOZ_ASSERT(!effects->CascadeNeedsUpdate(), "Failed to update cascade state");
}

/* static */ Maybe<NonOwningAnimationTarget>
EffectCompositor::GetAnimationElementAndPseudoForFrame(const nsIFrame* aFrame)
{
  // Always return the same object to benefit from return-value optimization.
  Maybe<NonOwningAnimationTarget> result;

  CSSPseudoElementType pseudoType =
    aFrame->Style()->GetPseudoType();

  if (pseudoType != CSSPseudoElementType::NotPseudo &&
      pseudoType != CSSPseudoElementType::before &&
      pseudoType != CSSPseudoElementType::after) {
    return result;
  }

  nsIContent* content = aFrame->GetContent();
  if (!content) {
    return result;
  }

  if (pseudoType == CSSPseudoElementType::before ||
      pseudoType == CSSPseudoElementType::after) {
    content = content->GetParent();
    if (!content) {
      return result;
    }
  }

  if (!content->IsElement()) {
    return result;
  }

  result.emplace(content->AsElement(), pseudoType);

  return result;
}


/* static */ nsCSSPropertyIDSet
EffectCompositor::GetOverriddenProperties(EffectSet& aEffectSet,
                                          Element* aElement,
                                          CSSPseudoElementType aPseudoType)
{
  MOZ_ASSERT(aElement, "Should have an element to get style data from");

  nsCSSPropertyIDSet result;

  Element* elementToRestyle = GetElementToRestyle(aElement, aPseudoType);
  if (!elementToRestyle) {
    return result;
  }

  AutoTArray<nsCSSPropertyID, LayerAnimationInfo::kRecords> propertiesToTrack;
  {
    nsCSSPropertyIDSet propertiesToTrackAsSet;
    for (KeyframeEffect* effect : aEffectSet) {
      for (const AnimationProperty& property : effect->Properties()) {
        if (nsCSSProps::PropHasFlags(property.mProperty,
                                     CSSPropFlags::CanAnimateOnCompositor) &&
            !propertiesToTrackAsSet.HasProperty(property.mProperty)) {
          propertiesToTrackAsSet.AddProperty(property.mProperty);
          propertiesToTrack.AppendElement(property.mProperty);
        }
      }
      // Skip iterating over the rest of the effects if we've already
      // found all the compositor-animatable properties.
      if (propertiesToTrack.Length() == LayerAnimationInfo::kRecords) {
        break;
      }
    }
  }

  if (propertiesToTrack.IsEmpty()) {
    return result;
  }

  Servo_GetProperties_Overriding_Animation(elementToRestyle,
                                           &propertiesToTrack,
                                           &result);
  return result;
}

/* static */ void
EffectCompositor::UpdateCascadeResults(EffectSet& aEffectSet,
                                       Element* aElement,
                                       CSSPseudoElementType aPseudoType)
{
  MOZ_ASSERT(EffectSet::GetEffectSet(aElement, aPseudoType) == &aEffectSet,
             "Effect set should correspond to the specified (pseudo-)element");
  if (aEffectSet.IsEmpty()) {
    aEffectSet.MarkCascadeUpdated();
    return;
  }

  // Get a list of effects sorted by composite order.
  nsTArray<KeyframeEffect*> sortedEffectList(aEffectSet.Count());
  for (KeyframeEffect* effect : aEffectSet) {
    sortedEffectList.AppendElement(effect);
  }
  sortedEffectList.Sort(EffectCompositeOrderComparator());

  // Get properties that override the *animations* level of the cascade.
  //
  // We only do this for properties that we can animate on the compositor
  // since we will apply other properties on the main thread where the usual
  // cascade applies.
  nsCSSPropertyIDSet overriddenProperties =
    GetOverriddenProperties(aEffectSet, aElement, aPseudoType);

  // Returns a bitset the represents which properties from
  // LayerAnimationInfo::sRecords are present in |aPropertySet|.
  auto compositorPropertiesInSet =
    [](nsCSSPropertyIDSet& aPropertySet) ->
      std::bitset<LayerAnimationInfo::kRecords> {
        std::bitset<LayerAnimationInfo::kRecords> result;
        for (size_t i = 0; i < LayerAnimationInfo::kRecords; i++) {
          if (aPropertySet.HasProperty(
                LayerAnimationInfo::sRecords[i].mProperty)) {
            result.set(i);
          }
        }
      return result;
    };

  nsCSSPropertyIDSet& propertiesWithImportantRules =
    aEffectSet.PropertiesWithImportantRules();
  nsCSSPropertyIDSet& propertiesForAnimationsLevel =
    aEffectSet.PropertiesForAnimationsLevel();

  // Record which compositor-animatable properties were originally set so we can
  // compare for changes later.
  std::bitset<LayerAnimationInfo::kRecords>
    prevCompositorPropertiesWithImportantRules =
      compositorPropertiesInSet(propertiesWithImportantRules);

  nsCSSPropertyIDSet prevPropertiesForAnimationsLevel =
    propertiesForAnimationsLevel;

  propertiesWithImportantRules.Empty();
  propertiesForAnimationsLevel.Empty();

  nsCSSPropertyIDSet propertiesForTransitionsLevel;

  for (const KeyframeEffect* effect : sortedEffectList) {
    MOZ_ASSERT(effect->GetAnimation(),
               "Effects on a target element should have an Animation");
    CascadeLevel cascadeLevel = effect->GetAnimation()->CascadeLevel();

    for (const AnimationProperty& prop : effect->Properties()) {
      if (overriddenProperties.HasProperty(prop.mProperty)) {
        propertiesWithImportantRules.AddProperty(prop.mProperty);
      }

      switch (cascadeLevel) {
        case EffectCompositor::CascadeLevel::Animations:
          propertiesForAnimationsLevel.AddProperty(prop.mProperty);
          break;
        case EffectCompositor::CascadeLevel::Transitions:
          propertiesForTransitionsLevel.AddProperty(prop.mProperty);
          break;
      }
    }
  }

  aEffectSet.MarkCascadeUpdated();

  nsPresContext* presContext = nsContentUtils::GetContextForContent(aElement);
  if (!presContext) {
    return;
  }

  // If properties for compositor are newly overridden by !important rules, or
  // released from being overridden by !important rules, we need to update
  // layers for animations level because it's a trigger to send animations to
  // the compositor or pull animations back from the compositor.
  if (prevCompositorPropertiesWithImportantRules !=
        compositorPropertiesInSet(propertiesWithImportantRules)) {
    presContext->EffectCompositor()->
      RequestRestyle(aElement, aPseudoType,
                     EffectCompositor::RestyleType::Layer,
                     EffectCompositor::CascadeLevel::Animations);
  }

  // If we have transition properties and if the same propery for animations
  // level is newly added or removed, we need to update the transition level
  // rule since the it will be added/removed from the rule tree.
  nsCSSPropertyIDSet changedPropertiesForAnimationLevel =
    prevPropertiesForAnimationsLevel.Xor(propertiesForAnimationsLevel);
  nsCSSPropertyIDSet commonProperties =
    propertiesForTransitionsLevel.Intersect(
      changedPropertiesForAnimationLevel);
  if (!commonProperties.IsEmpty()) {
    EffectCompositor::RestyleType restyleType =
      compositorPropertiesInSet(changedPropertiesForAnimationLevel).none()
      ? EffectCompositor::RestyleType::Standard
      : EffectCompositor::RestyleType::Layer;
    presContext->EffectCompositor()->
      RequestRestyle(aElement, aPseudoType,
                     restyleType,
                     EffectCompositor::CascadeLevel::Transitions);
  }
}

/* static */ void
EffectCompositor::SetPerformanceWarning(
  const nsIFrame *aFrame,
  nsCSSPropertyID aProperty,
  const AnimationPerformanceWarning& aWarning)
{
  EffectSet* effects = EffectSet::GetEffectSet(aFrame);
  if (!effects) {
    return;
  }

  for (KeyframeEffect* effect : *effects) {
    effect->SetPerformanceWarning(aProperty, aWarning);
  }
}

bool
EffectCompositor::PreTraverse(ServoTraversalFlags aFlags)
{
  return PreTraverseInSubtree(aFlags, nullptr);
}

bool
EffectCompositor::PreTraverseInSubtree(ServoTraversalFlags aFlags,
                                       Element* aRoot)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!aRoot || nsContentUtils::GetPresShellForContent(aRoot),
             "Traversal root, if provided, should be bound to a display "
             "document");

  // Convert the root element to the parent element if the root element is
  // pseudo since we check each element in mElementsToRestyle is in the subtree
  // of the root element later in this function, but for pseudo elements the
  // element in mElementsToRestyle is the parent of the pseudo.
  if (aRoot &&
      (aRoot->IsGeneratedContentContainerForBefore() ||
       aRoot->IsGeneratedContentContainerForAfter())) {
    aRoot = aRoot->GetParentElement();
  }

  AutoRestore<bool> guard(mIsInPreTraverse);
  mIsInPreTraverse = true;

  // We need to force flush all throttled animations if we also have
  // non-animation restyles (since we'll want the up-to-date animation style
  // when we go to process them so we can trigger transitions correctly), and
  // if we are currently flushing all throttled animation restyles.
  bool flushThrottledRestyles =
    (aRoot && aRoot->HasDirtyDescendantsForServo()) ||
    (aFlags & ServoTraversalFlags::FlushThrottledAnimations);

  using ElementsToRestyleIterType =
    nsDataHashtable<PseudoElementHashEntry, bool>::Iterator;
  auto getNeededRestyleTarget = [&](const ElementsToRestyleIterType& aIter)
                                -> NonOwningAnimationTarget {
    NonOwningAnimationTarget returnTarget;

    // If aIter.Data() is false, the element only requested a throttled
    // (skippable) restyle, so we can skip it if flushThrottledRestyles is not
    // true.
    if (!flushThrottledRestyles && !aIter.Data()) {
      return returnTarget;
    }

    const NonOwningAnimationTarget& target = aIter.Key();

    // Skip elements in documents without a pres shell. Normally we filter out
    // such elements in RequestRestyle but it can happen that, after adding
    // them to mElementsToRestyle, they are transferred to a different document.
    //
    // We will drop them from mElementsToRestyle at the end of the next full
    // document restyle (at the end of this function) but for consistency with
    // how we treat such elements in RequestRestyle, we just ignore them here.
    if (!nsContentUtils::GetPresShellForContent(target.mElement)) {
      return returnTarget;
    }

    // Ignore restyles that aren't in the flattened tree subtree rooted at
    // aRoot.
    if (aRoot && !nsContentUtils::ContentIsFlattenedTreeDescendantOfForStyle(
          target.mElement, aRoot)) {
      return returnTarget;
    }

    returnTarget = target;
    return returnTarget;
  };

  bool foundElementsNeedingRestyle = false;

  nsTArray<NonOwningAnimationTarget> elementsWithCascadeUpdates;
  for (size_t i = 0; i < kCascadeLevelCount; ++i) {
    CascadeLevel cascadeLevel = CascadeLevel(i);
    auto& elementSet = mElementsToRestyle[cascadeLevel];
    for (auto iter = elementSet.Iter(); !iter.Done(); iter.Next()) {
      const NonOwningAnimationTarget& target = getNeededRestyleTarget(iter);
      if (!target.mElement) {
        continue;
      }

      EffectSet* effects = EffectSet::GetEffectSet(target.mElement,
                                                   target.mPseudoType);
      if (!effects || !effects->CascadeNeedsUpdate()) {
        continue;
      }

      elementsWithCascadeUpdates.AppendElement(target);
    }
  }

  for (const NonOwningAnimationTarget& target: elementsWithCascadeUpdates) {
      MaybeUpdateCascadeResults(target.mElement, target.mPseudoType);
  }
  elementsWithCascadeUpdates.Clear();

  for (size_t i = 0; i < kCascadeLevelCount; ++i) {
    CascadeLevel cascadeLevel = CascadeLevel(i);
    auto& elementSet = mElementsToRestyle[cascadeLevel];
    for (auto iter = elementSet.Iter(); !iter.Done(); iter.Next()) {
      const NonOwningAnimationTarget& target = getNeededRestyleTarget(iter);
      if (!target.mElement) {
        continue;
      }

      // We need to post restyle hints even if the target is not in EffectSet to
      // ensure the final restyling for removed animations.
      // We can't call PostRestyleEvent directly here since we are still in the
      // middle of the servo traversal.
      mPresContext->RestyleManager()->
        PostRestyleEventForAnimations(target.mElement,
                                      target.mPseudoType,
                                      cascadeLevel == CascadeLevel::Transitions
                                        ? eRestyle_CSSTransitions
                                        : eRestyle_CSSAnimations);

      foundElementsNeedingRestyle = true;

      EffectSet* effects = EffectSet::GetEffectSet(target.mElement,
                                                   target.mPseudoType);
      if (!effects) {
        // Drop EffectSets that have been destroyed.
        iter.Remove();
        continue;
      }

      for (KeyframeEffect* effect : *effects) {
        effect->GetAnimation()->WillComposeStyle();
      }

      // Remove the element from the list of elements to restyle since we are
      // about to restyle it.
      iter.Remove();
    }

    // If this is a full document restyle, then unconditionally clear
    // elementSet in case there are any elements that didn't match above
    // because they were moved to a document without a pres shell after
    // posting an animation restyle.
    if (!aRoot && flushThrottledRestyles) {
      elementSet.Clear();
    }
  }

  return foundElementsNeedingRestyle;
}

bool
EffectCompositor::PreTraverse(dom::Element* aElement,
                              CSSPseudoElementType aPseudoType)
{
  MOZ_ASSERT(NS_IsMainThread());

  // If |aElement|'s document does not have a pres shell, e.g. it is document
  // without a browsing context such as we might get from an XMLHttpRequest, we
  // should not run animations on it.
  if (!nsContentUtils::GetPresShellForContent(aElement)) {
    return false;
  }

  bool found = false;
  if (aPseudoType != CSSPseudoElementType::NotPseudo &&
      aPseudoType != CSSPseudoElementType::before &&
      aPseudoType != CSSPseudoElementType::after) {
    return found;
  }

  AutoRestore<bool> guard(mIsInPreTraverse);
  mIsInPreTraverse = true;

  PseudoElementHashEntry::KeyType key = { aElement, aPseudoType };

  // We need to flush all throttled animation restyles too if we also have
  // non-animation restyles (since we'll want the up-to-date animation style
  // when we go to process them so we can trigger transitions correctly).
  Element* elementToRestyle = GetElementToRestyle(aElement, aPseudoType);
  bool flushThrottledRestyles = elementToRestyle &&
                                elementToRestyle->HasDirtyDescendantsForServo();

  for (size_t i = 0; i < kCascadeLevelCount; ++i) {
    CascadeLevel cascadeLevel = CascadeLevel(i);
    auto& elementSet = mElementsToRestyle[cascadeLevel];

    // Skip if we don't have a restyle, or if we only have a throttled
    // (skippable) restyle and we're not required to flush throttled restyles.
    bool hasUnthrottledRestyle = false;
    if (!elementSet.Get(key, &hasUnthrottledRestyle) ||
        (!flushThrottledRestyles && !hasUnthrottledRestyle)) {
      continue;
    }

    mPresContext->RestyleManager()->
      PostRestyleEventForAnimations(aElement,
                                    aPseudoType,
                                    cascadeLevel == CascadeLevel::Transitions
                                      ? eRestyle_CSSTransitions
                                      : eRestyle_CSSAnimations);

    EffectSet* effects = EffectSet::GetEffectSet(aElement, aPseudoType);
    if (effects) {
      MaybeUpdateCascadeResults(aElement, aPseudoType);

      for (KeyframeEffect* effect : *effects) {
        effect->GetAnimation()->WillComposeStyle();
      }
    }

    elementSet.Remove(key);
    found = true;
  }
  return found;
}


} // namespace mozilla
