/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/DebugOnly.h"

#include "FrameLayerBuilder.h"

#include "gfxContext.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/ProfileTimelineMarkerBinding.h"
#include "mozilla/gfx/Matrix.h"
#include "ActiveLayerTracker.h"
#include "BasicLayers.h"
#include "ImageContainer.h"
#include "ImageLayers.h"
#include "LayerTreeInvalidation.h"
#include "Layers.h"
#include "LayerUserData.h"
#include "MaskLayerImageCache.h"
#include "UnitTransforms.h"
#include "Units.h"
#include "gfx2DGlue.h"
#include "gfxEnv.h"
#include "gfxUtils.h"
#include "nsAutoPtr.h"
#include "nsAnimationManager.h"
#include "nsDisplayList.h"
#include "nsDocShell.h"
#include "nsIScrollableFrame.h"
#include "nsImageFrame.h"
#include "nsLayoutUtils.h"
#include "nsPresContext.h"
#include "nsPrintfCString.h"
#include "nsSVGIntegrationUtils.h"
#include "nsTransitionManager.h"
#include "mozilla/LayerTimelineMarker.h"

#include "mozilla/EffectCompositor.h"
#include "mozilla/Move.h"
#include "mozilla/ReverseIterator.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Tools.h"
#include "mozilla/layers/ShadowLayers.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/layers/TextureWrapperImage.h"
#include "mozilla/layers/WebRenderUserData.h"
#include "mozilla/Unused.h"
#include "GeckoProfiler.h"
#include "LayersLogging.h"
#include "gfxPrefs.h"

#include <algorithm>
#include <functional>
#include <list>

using namespace mozilla::layers;
using namespace mozilla::gfx;

namespace mozilla {

class PaintedDisplayItemLayerUserData;

static nsTHashtable<nsPtrHashKey<DisplayItemData>>* sAliveDisplayItemDatas;

/**
 * The address of gPaintedDisplayItemLayerUserData is used as the user
 * data key for PaintedLayers created by FrameLayerBuilder.
 * It identifies PaintedLayers used to draw non-layer content, which are
 * therefore eligible for recycling. We want display items to be able to
 * create their own dedicated PaintedLayers in BuildLayer, if necessary,
 * and we wouldn't want to accidentally recycle those.
 * The user data is a PaintedDisplayItemLayerUserData.
 */
uint8_t gPaintedDisplayItemLayerUserData;
/**
 * The address of gColorLayerUserData is used as the user
 * data key for ColorLayers created by FrameLayerBuilder.
 * The user data is null.
 */
uint8_t gColorLayerUserData;
/**
 * The address of gImageLayerUserData is used as the user
 * data key for ImageLayers created by FrameLayerBuilder.
 * The user data is null.
 */
uint8_t gImageLayerUserData;
/**
 * The address of gLayerManagerUserData is used as the user
 * data key for retained LayerManagers managed by FrameLayerBuilder.
 * The user data is a LayerManagerData.
 */
uint8_t gLayerManagerUserData;
/**
 * The address of gMaskLayerUserData is used as the user
 * data key for mask layers managed by FrameLayerBuilder.
 * The user data is a MaskLayerUserData.
 */
uint8_t gMaskLayerUserData;
/**
 * The address of gCSSMaskLayerUserData is used as the user
 * data key for mask layers of css masking managed by FrameLayerBuilder.
 * The user data is a CSSMaskLayerUserData.
 */
uint8_t gCSSMaskLayerUserData;

// a global cache of image containers used for mask layers
static MaskLayerImageCache* gMaskLayerImageCache = nullptr;

static inline MaskLayerImageCache* GetMaskLayerImageCache()
{
  if (!gMaskLayerImageCache) {
    gMaskLayerImageCache = new MaskLayerImageCache();
  }

  return gMaskLayerImageCache;
}

struct DisplayItemEntry {
  DisplayItemEntry(nsDisplayItem* aItem,
                   DisplayItemEntryType aType)
    : mItem(aItem)
    , mType(aType)
  {}

  nsDisplayItem* mItem;
  DisplayItemEntryType mType;
};

class FLBDisplayItemIterator : protected FlattenedDisplayItemIterator
{
public:
  FLBDisplayItemIterator(nsDisplayListBuilder* aBuilder,
                         nsDisplayList* aList,
                         ContainerState* aState)
    : FlattenedDisplayItemIterator(aBuilder, aList, false)
    , mState(aState)
  {
    MOZ_ASSERT(mState);
    ResolveFlattening();
  }

  DisplayItemEntry GetNextEntry()
  {
    if (!mMarkers.empty()) {
      DisplayItemEntry entry = mMarkers.front();
      mMarkers.pop_front();
      return entry;
    }

    nsDisplayItem* next = GetNext();
    return DisplayItemEntry { next, DisplayItemEntryType::ITEM };
  }

  nsDisplayItem* GetNext()
  {
    // This function is only supposed to be called if there are no markers set.
    // Breaking this invariant can potentially break effect flattening and/or
    // display item merging.
    MOZ_ASSERT(mMarkers.empty());

    return FlattenedDisplayItemIterator::GetNext();
  }

  bool HasNext() const
  {
    return FlattenedDisplayItemIterator::HasNext() || !mMarkers.empty();
  }

  nsDisplayItem* PeekNext()
  {
    return mNext;
  }

private:
  bool ShouldFlattenNextItem() const override;

  void StartNested(nsDisplayItem* aItem) override
  {
    if (aItem->GetType() == DisplayItemType::TYPE_OPACITY) {
      nsDisplayOpacity* opacity = static_cast<nsDisplayOpacity*> (aItem);

      if (opacity->OpacityAppliedToChildren()) {
        // If the opacity was already applied to children, there is no need to
        // emit opacity markers.
        return;
      }

      mMarkers.emplace_back(aItem, DisplayItemEntryType::PUSH_OPACITY);
      mActiveMarkers.AppendElement(aItem);
    }
  }

  void EndNested(nsDisplayItem* aItem) override
  {
    if (mActiveMarkers.IsEmpty() || mActiveMarkers.LastElement() != aItem) {
      // Do not emit an end marker if this item did not emit a start marker.
      return;
    }

    if (aItem->GetType() == DisplayItemType::TYPE_OPACITY) {
      mMarkers.emplace_back(aItem, DisplayItemEntryType::POP_OPACITY);
      mActiveMarkers.RemoveLastElement();
    }
  }

  std::list<DisplayItemEntry> mMarkers;
  AutoTArray<nsDisplayItem*, 4> mActiveMarkers;
  ContainerState* mState;
};

DisplayItemData::DisplayItemData(LayerManagerData* aParent, uint32_t aKey,
                                 Layer* aLayer, nsIFrame* aFrame)

  : mRefCnt(0)
  , mParent(aParent)
  , mLayer(aLayer)
  , mDisplayItemKey(aKey)
  , mItem(nullptr)
  , mUsed(true)
  , mIsInvalid(false)
  , mReusedItem(false)
  , mDisconnected(false)
{
  MOZ_COUNT_CTOR(DisplayItemData);

  if (!sAliveDisplayItemDatas) {
    sAliveDisplayItemDatas = new nsTHashtable<nsPtrHashKey<DisplayItemData>>();
  }
  MOZ_RELEASE_ASSERT(!sAliveDisplayItemDatas->Contains(this));
  sAliveDisplayItemDatas->PutEntry(this);

  MOZ_RELEASE_ASSERT(mLayer);
  if (aFrame) {
    AddFrame(aFrame);
  }

}

void
DisplayItemData::AddFrame(nsIFrame* aFrame)
{
  MOZ_RELEASE_ASSERT(mLayer);
  mFrameList.AppendElement(aFrame);

  SmallPointerArray<DisplayItemData>& array = aFrame->DisplayItemData();
  array.AppendElement(this);
}

void
DisplayItemData::RemoveFrame(nsIFrame* aFrame)
{
  MOZ_RELEASE_ASSERT(mLayer);
  bool result = mFrameList.RemoveElement(aFrame);
  MOZ_RELEASE_ASSERT(result, "Can't remove a frame that wasn't added!");

  SmallPointerArray<DisplayItemData>& array = aFrame->DisplayItemData();
  array.RemoveElement(this);
}

void
DisplayItemData::EndUpdate()
{
  MOZ_RELEASE_ASSERT(mLayer);
  mItem = nullptr;
  mIsInvalid = false;
  mUsed = false;
  mReusedItem = false;
}

void
DisplayItemData::EndUpdate(nsAutoPtr<nsDisplayItemGeometry> aGeometry)
{
  MOZ_RELEASE_ASSERT(mLayer);
  MOZ_ASSERT(mItem);
  MOZ_ASSERT(mGeometry || aGeometry);

  if (aGeometry) {
    mGeometry = aGeometry;
  }
  mClip = mItem->GetClip();
  mChangedFrameInvalidations.SetEmpty();

  mItem = nullptr;
  EndUpdate();
}

void
DisplayItemData::BeginUpdate(Layer* aLayer, LayerState aState,
                             nsDisplayItem* aItem /* = nullptr */)
{
  BeginUpdate(aLayer, aState, aItem,
              aItem ? aItem->IsReused() : false,
              aItem ? aItem->HasMergedFrames() : false);
}

void
DisplayItemData::BeginUpdate(Layer* aLayer, LayerState aState,
                             nsDisplayItem* aItem, bool aIsReused,
                             bool aIsMerged)
{
  MOZ_RELEASE_ASSERT(mLayer);
  MOZ_RELEASE_ASSERT(aLayer);
  mLayer = aLayer;
  mOptLayer = nullptr;
  mInactiveManager = nullptr;
  mLayerState = aState;
  mUsed = true;

  if (aLayer->AsPaintedLayer()) {
    mItem = aItem;
    mReusedItem = aIsReused;
  }

  if (!aItem) {
    return;
  }

  if (!aIsMerged && mFrameList.Length() == 1) {
    MOZ_ASSERT(mFrameList[0] == aItem->Frame());
    return;
  }

  // We avoid adding or removing element unnecessarily
  // since we have to modify userdata each time
  AutoTArray<nsIFrame*, 4> copy(mFrameList);
  if (!copy.RemoveElement(aItem->Frame())) {
    AddFrame(aItem->Frame());
    mChangedFrameInvalidations.Or(mChangedFrameInvalidations,
                                  aItem->Frame()->GetVisualOverflowRect());
  }

  AutoTArray<nsIFrame*,4> mergedFrames;
  aItem->GetMergedFrames(&mergedFrames);
  for (uint32_t i = 0; i < mergedFrames.Length(); ++i) {
    if (!copy.RemoveElement(mergedFrames[i])) {
      AddFrame(mergedFrames[i]);
      mChangedFrameInvalidations.Or(mChangedFrameInvalidations,
                                    mergedFrames[i]->GetVisualOverflowRect());
    }
  }

  for (uint32_t i = 0; i < copy.Length(); i++) {
    RemoveFrame(copy[i]);
    mChangedFrameInvalidations.Or(mChangedFrameInvalidations,
                                  copy[i]->GetVisualOverflowRect());
  }
}

static const nsIFrame* sDestroyedFrame = nullptr;
DisplayItemData::~DisplayItemData()
{
  MOZ_COUNT_DTOR(DisplayItemData);
  Disconnect();

  MOZ_RELEASE_ASSERT(sAliveDisplayItemDatas);
  nsPtrHashKey<mozilla::DisplayItemData>* entry
    = sAliveDisplayItemDatas->GetEntry(this);
  MOZ_RELEASE_ASSERT(entry);

  sAliveDisplayItemDatas->RemoveEntry(entry);

  if (sAliveDisplayItemDatas->Count() == 0) {
    delete sAliveDisplayItemDatas;
    sAliveDisplayItemDatas = nullptr;
  }
}

void
DisplayItemData::Disconnect()
{
  if (mDisconnected) {
    return;
  }
  mDisconnected = true;

  for (uint32_t i = 0; i < mFrameList.Length(); i++) {
    nsIFrame* frame = mFrameList[i];
    if (frame == sDestroyedFrame) {
      continue;
    }
    SmallPointerArray<DisplayItemData>& array = frame->DisplayItemData();
    array.RemoveElement(this);
  }

  mLayer = nullptr;
  mOptLayer = nullptr;
}

void
DisplayItemData::ClearAnimationCompositorState()
{
  if (mDisplayItemKey != static_cast<uint32_t>(DisplayItemType::TYPE_TRANSFORM) &&
      mDisplayItemKey != static_cast<uint32_t>(DisplayItemType::TYPE_OPACITY)) {
    return;
  }

  for (nsIFrame* frame : mFrameList) {
    nsCSSPropertyID prop = mDisplayItemKey == static_cast<uint32_t>(DisplayItemType::TYPE_TRANSFORM) ?
      eCSSProperty_transform : eCSSProperty_opacity;
    EffectCompositor::ClearIsRunningOnCompositor(frame, prop);
  }
}

const nsRegion&
DisplayItemData::GetChangedFrameInvalidations()
{
  return mChangedFrameInvalidations;
}

DisplayItemData*
DisplayItemData::AssertDisplayItemData(DisplayItemData* aData)
{
  MOZ_RELEASE_ASSERT(aData);
  MOZ_RELEASE_ASSERT(sAliveDisplayItemDatas && sAliveDisplayItemDatas->Contains(aData));
  MOZ_RELEASE_ASSERT(aData->mLayer);
  return aData;
}

/**
 * This is the userdata we associate with a layer manager.
 */
class LayerManagerData : public LayerUserData {
public:
  explicit LayerManagerData(LayerManager *aManager)
    : mLayerManager(aManager)
#ifdef DEBUG_DISPLAY_ITEM_DATA
    , mParent(nullptr)
#endif
    , mInvalidateAllLayers(false)
  {
    MOZ_COUNT_CTOR(LayerManagerData);
  }
  ~LayerManagerData() {
    MOZ_COUNT_DTOR(LayerManagerData);

    for (auto iter = mDisplayItems.Iter(); !iter.Done(); iter.Next()) {
      iter.Get()->GetKey()->Disconnect();
    }
  }

#ifdef DEBUG_DISPLAY_ITEM_DATA
  void Dump(const char *aPrefix = "") {
    printf_stderr("%sLayerManagerData %p\n", aPrefix, this);

    for (auto iter = mDisplayItems.Iter(); !iter.Done(); iter.Next()) {
      FrameLayerBuilder::DisplayItemData* data = iter.Get()->GetKey();

      nsAutoCString prefix;
      prefix += aPrefix;
      prefix += "  ";

      const char* layerState;
      switch (data->mLayerState) {
      case LAYER_NONE:
        layerState = "LAYER_NONE"; break;
      case LAYER_INACTIVE:
        layerState = "LAYER_INACTIVE"; break;
      case LAYER_ACTIVE:
        layerState = "LAYER_ACTIVE"; break;
      case LAYER_ACTIVE_FORCE:
        layerState = "LAYER_ACTIVE_FORCE"; break;
      case LAYER_ACTIVE_EMPTY:
        layerState = "LAYER_ACTIVE_EMPTY"; break;
      case LAYER_SVG_EFFECTS:
        layerState = "LAYER_SVG_EFFECTS"; break;
      }
      uint32_t mask = (1 << TYPE_BITS) - 1;

      nsAutoCString str;
      str += prefix;
      str += nsPrintfCString("Frame %p ", data->mFrameList[0]);
      str += nsDisplayItem::DisplayItemTypeName(static_cast<nsDisplayItem::Type>(data->mDisplayItemKey & mask));
      if ((data->mDisplayItemKey >> TYPE_BITS)) {
        str += nsPrintfCString("(%i)", data->mDisplayItemKey >> TYPE_BITS);
      }
      str += nsPrintfCString(", %s, Layer %p", layerState, data->mLayer.get());
      if (data->mOptLayer) {
        str += nsPrintfCString(", OptLayer %p", data->mOptLayer.get());
      }
      if (data->mInactiveManager) {
        str += nsPrintfCString(", InactiveLayerManager %p", data->mInactiveManager.get());
      }
      str += "\n";

      printf_stderr("%s", str.get());

      if (data->mInactiveManager) {
        prefix += "  ";
        printf_stderr("%sDumping inactive layer info:\n", prefix.get());
        LayerManagerData* lmd = static_cast<LayerManagerData*>
          (data->mInactiveManager->GetUserData(&gLayerManagerUserData));
        lmd->Dump(prefix.get());
      }
    }
  }
#endif

  /**
   * Tracks which frames have layers associated with them.
   */
  LayerManager *mLayerManager;
#ifdef DEBUG_DISPLAY_ITEM_DATA
  LayerManagerData *mParent;
#endif
  nsTHashtable<nsRefPtrHashKey<DisplayItemData> > mDisplayItems;
  bool mInvalidateAllLayers;
};

/* static */ void
FrameLayerBuilder::DestroyDisplayItemDataFor(nsIFrame* aFrame)
{
  RemoveFrameFromLayerManager(aFrame, aFrame->DisplayItemData());
  aFrame->DisplayItemData().Clear();
  aFrame->DeleteProperty(WebRenderUserDataProperty::Key());
}

/**
 * We keep a stack of these to represent the PaintedLayers that are
 * currently available to have display items added to.
 * We use a stack here because as much as possible we want to
 * assign display items to existing PaintedLayers, and to the lowest
 * PaintedLayer in z-order. This reduces the number of layers and
 * makes it more likely a display item will be rendered to an opaque
 * layer, giving us the best chance of getting subpixel AA.
 */
class PaintedLayerData {
public:
  PaintedLayerData() :
    mAnimatedGeometryRoot(nullptr),
    mASR(nullptr),
    mReferenceFrame(nullptr),
    mLayer(nullptr),
    mSolidColor(NS_RGBA(0, 0, 0, 0)),
    mIsSolidColorInVisibleRegion(false),
    mNeedComponentAlpha(false),
    mForceTransparentSurface(false),
    mHideAllLayersBelow(false),
    mOpaqueForAnimatedGeometryRootParent(false),
    mDisableFlattening(false),
    mBackfaceHidden(false),
    mShouldPaintOnContentSide(false),
    mDTCRequiresTargetConfirmation(false),
    mImage(nullptr),
    mItemClip(nullptr),
    mNewChildLayersIndex(-1)
  {}

#ifdef MOZ_DUMP_PAINTING
  /**
   * Keep track of important decisions for debugging.
   */
  nsCString mLog;

  #define FLB_LOG_PAINTED_LAYER_DECISION(pld, ...) \
          if (gfxPrefs::LayersDumpDecision()) { \
            pld->mLog.AppendPrintf("\t\t\t\t"); \
            pld->mLog.AppendPrintf(__VA_ARGS__); \
          }
#else
  #define FLB_LOG_PAINTED_LAYER_DECISION(...)
#endif

  /**
   * Record that an item has been added to the PaintedLayer, so we
   * need to update our regions.
   * @param aVisibleRect the area of the item that's visible
   * @param aSolidColor if non-null, the visible area of the item is
   * a constant color given by *aSolidColor
   */
  void Accumulate(ContainerState* aState,
                  nsDisplayItem* aItem,
                  const nsIntRect& aVisibleRect,
                  const DisplayItemClip& aClip,
                  LayerState aLayerState,
                  nsDisplayList *aList,
                  DisplayItemEntryType aType);
  AnimatedGeometryRoot* GetAnimatedGeometryRoot() { return mAnimatedGeometryRoot; }

  /**
   * A region including the horizontal pan, vertical pan, and no action regions.
   */
  nsRegion CombinedTouchActionRegion();

  /**
   * Add the given hit regions to the hit regions for this PaintedLayer.
   */
  void AccumulateEventRegions(ContainerState* aState,
                              nsDisplayLayerEventRegions* aEventRegions);

  /**
   * Similar to AccumulateEventRegions() but uses different display item to
   * track hit regions.
   */
  void AccumulateHitTestInfo(ContainerState* aState,
                             nsDisplayCompositorHitTestInfo* aItem);

  /**
   * If this represents only a nsDisplayImage, and the image type supports being
   * optimized to an ImageLayer, returns true.
   */
  bool CanOptimizeToImageLayer(nsDisplayListBuilder* aBuilder);

  /**
   * If this represents only a nsDisplayImage, and the image type supports being
   * optimized to an ImageLayer, returns an ImageContainer for the underlying
   * image if one is available.
   */
  already_AddRefed<ImageContainer> GetContainerForImageLayer(nsDisplayListBuilder* aBuilder);

  bool VisibleAboveRegionIntersects(const nsIntRegion& aRegion) const
  { return !mVisibleAboveRegion.Intersect(aRegion).IsEmpty(); }
  bool VisibleRegionIntersects(const nsIntRegion& aRegion) const
  { return !mVisibleRegion.Intersect(aRegion).IsEmpty(); }

  /**
   * The region of visible content in the layer, relative to the
   * container layer (which is at the snapped top-left of the display
   * list reference frame).
   */
  nsIntRegion  mVisibleRegion;
  /**
   * The region of visible content in the layer that is opaque.
   * Same coordinate system as mVisibleRegion.
   */
  nsIntRegion  mOpaqueRegion;
  /**
   * The definitely-hit region for this PaintedLayer.
   */
  nsRegion  mHitRegion;
  /**
   * The maybe-hit region for this PaintedLayer.
   */
  nsRegion  mMaybeHitRegion;
  /**
   * The dispatch-to-content hit region for this PaintedLayer.
   */
  nsRegion  mDispatchToContentHitRegion;
  /**
   * The region for this PaintedLayer that is sensitive to events
   * but disallows panning and zooming. This is an approximation
   * and any deviation from the true region will be part of the
   * mDispatchToContentHitRegion.
   */
  nsRegion mNoActionRegion;
  /**
   * The region for this PaintedLayer that is sensitive to events and
   * allows horizontal panning but not zooming. This is an approximation
   * and any deviation from the true region will be part of the
   * mDispatchToContentHitRegion.
   */
  nsRegion mHorizontalPanRegion;
  /**
   * The region for this PaintedLayer that is sensitive to events and
   * allows vertical panning but not zooming. This is an approximation
   * and any deviation from the true region will be part of the
   * mDispatchToContentHitRegion.
   */
  nsRegion mVerticalPanRegion;
  /**
   * Scaled versions of the bounds of mHitRegion and mMaybeHitRegion.
   * We store these because FindPaintedLayerFor() needs to consume them
   * in this form, and it's a hot code path so we don't want to scale
   * them inside that function.
   */
  nsIntRect mScaledHitRegionBounds;
  nsIntRect mScaledMaybeHitRegionBounds;
  /**
   * The "active scrolled root" for all content in the layer. Must
   * be non-null; all content in a PaintedLayer must have the same
   * active scrolled root.
   */
  AnimatedGeometryRoot* mAnimatedGeometryRoot;
  const ActiveScrolledRoot* mASR;
  /**
   * The chain of clips that should apply to this layer.
   */
  const DisplayItemClipChain* mClipChain;
  /**
   * The offset between mAnimatedGeometryRoot and the reference frame.
   */
  nsPoint mAnimatedGeometryRootOffset;
  /**
   * If non-null, the frame from which we'll extract "fixed positioning"
   * metadata for this layer. This can be a position:fixed frame or a viewport
   * frame; the latter case is used for background-attachment:fixed content.
   */
  const nsIFrame* mReferenceFrame;
  PaintedLayer* mLayer;
  /**
   * If mIsSolidColorInVisibleRegion is true, this is the color of the visible
   * region.
   */
  nscolor      mSolidColor;
  /**
   * True if every pixel in mVisibleRegion will have color mSolidColor.
   */
  bool mIsSolidColorInVisibleRegion;
  /**
   * True if there is any text visible in the layer that's over
   * transparent pixels in the layer.
   */
  bool mNeedComponentAlpha;
  /**
   * Set if the layer should be treated as transparent, even if its entire
   * area is covered by opaque display items. For example, this needs to
   * be set if something is going to "punch holes" in the layer by clearing
   * part of its surface.
   */
  bool mForceTransparentSurface;
  /**
   * Set if all layers below this PaintedLayer should be hidden.
   */
  bool mHideAllLayersBelow;
  /**
   * Set if the opaque region for this layer can be applied to the parent
   * animated geometry root of this layer's animated geometry root.
   * We set this when a PaintedLayer's animated geometry root is a scrollframe
   * and the PaintedLayer completely fills the displayport of the scrollframe.
   */
  bool mOpaqueForAnimatedGeometryRootParent;
  /**
   * Set if there is content in the layer that must avoid being flattened.
   */
  bool mDisableFlattening;
  /**
   * Set if the backface of this region is hidden to the user.
   * Content that backface is hidden should not be draw on the layer
   * with visible backface.
   */
  bool mBackfaceHidden;
  /**
   * Set if it is better to render this layer on the content process, for
   * example if it contains native theme widgets.
   */
  bool mShouldPaintOnContentSide;
  /**
   * Set to true if events targeting the dispatch-to-content region
   * require target confirmation.
   * See CompositorHitTestFlags::eRequiresTargetConfirmation and
   * EventRegions::mDTCRequiresTargetConfirmation.
   */
  bool mDTCRequiresTargetConfirmation;
  /**
   * Stores the pointer to the nsDisplayImage if we want to
   * convert this to an ImageLayer.
   */
  nsDisplayImageContainer* mImage;
  /**
   * Stores the clip that we need to apply to the image or, if there is no
   * image, a clip for SOME item in the layer. There is no guarantee which
   * item's clip will be stored here and mItemClip should not be used to clip
   * the whole layer - only some part of the clip should be used, as determined
   * by PaintedDisplayItemLayerUserData::GetCommonClipCount() - which may even be
   * no part at all.
   */
  const DisplayItemClip* mItemClip;
  /**
   * Index of this layer in mNewChildLayers.
   */
  int32_t mNewChildLayersIndex;
  /**
   * The region of visible content above the layer and below the
   * next PaintedLayerData currently in the stack, if any.
   * This is a conservative approximation: it contains the true region.
   */
  nsIntRegion mVisibleAboveRegion;
  /**
   * All the display items that have been assigned to this painted layer.
   * These items get added by Accumulate().
   */
  nsTArray<AssignedDisplayItem> mAssignedDisplayItems;
  /**
   * Tracks the active opacity markers by holding the indices to PUSH_OPACITY
   * items in |mAssignedDisplayItems|.
   */
  nsTArray<size_t> mOpacityIndices;
};

struct NewLayerEntry {
  NewLayerEntry()
    : mAnimatedGeometryRoot(nullptr)
    , mASR(nullptr)
    , mClipChain(nullptr)
    , mScrollMetadataASR(nullptr)
    , mLayerContentsVisibleRect(0, 0, -1, -1)
    , mLayerState(LAYER_INACTIVE)
    , mHideAllLayersBelow(false)
    , mOpaqueForAnimatedGeometryRootParent(false)
    , mPropagateComponentAlphaFlattening(true)
    , mUntransformedVisibleRegion(false)
    , mIsFixedToRootScrollFrame(false)
  {}
  // mLayer is null if the previous entry is for a PaintedLayer that hasn't
  // been optimized to some other form (yet).
  RefPtr<Layer> mLayer;
  AnimatedGeometryRoot* mAnimatedGeometryRoot;
  const ActiveScrolledRoot* mASR;
  const DisplayItemClipChain* mClipChain;
  const ActiveScrolledRoot* mScrollMetadataASR;
  // If non-null, this ScrollMetadata is set to the be the first ScrollMetadata
  // on the layer.
  UniquePtr<ScrollMetadata> mBaseScrollMetadata;
  // The following are only used for retained layers (for occlusion
  // culling of those layers). These regions are all relative to the
  // container reference frame.
  nsIntRegion mVisibleRegion;
  nsIntRegion mOpaqueRegion;
  // This rect is in the layer's own coordinate space. The computed visible
  // region for the layer cannot extend beyond this rect.
  nsIntRect mLayerContentsVisibleRect;
  LayerState mLayerState;
  bool mHideAllLayersBelow;
  // When mOpaqueForAnimatedGeometryRootParent is true, the opaque region of
  // this layer is opaque in the same position even subject to the animation of
  // geometry of mAnimatedGeometryRoot. For example when mAnimatedGeometryRoot
  // is a scrolled frame and the scrolled content is opaque everywhere in the
  // displayport, we can set this flag.
  // When this flag is set, we can treat this opaque region as covering
  // content whose animated geometry root is the animated geometry root for
  // mAnimatedGeometryRoot->GetParent().
  bool mOpaqueForAnimatedGeometryRootParent;

  // If true, then the content flags for this layer should contribute
  // to our decision to flatten component alpha layers, false otherwise.
  bool mPropagateComponentAlphaFlattening;
  // mVisibleRegion is relative to the associated frame before
  // transform.
  bool mUntransformedVisibleRegion;
  bool mIsFixedToRootScrollFrame;
};

class PaintedLayerDataTree;

/**
 * This is tree node type for PaintedLayerDataTree.
 * Each node corresponds to a different animated geometry root, and contains
 * a stack of PaintedLayerDatas, in bottom-to-top order.
 * There is at most one node per animated geometry root. The ancestor and
 * descendant relations in PaintedLayerDataTree tree mirror those in the frame
 * tree.
 * Each node can have clip that describes the potential extents that items in
 * this node can cover. If mHasClip is false, it means that the node's contents
 * can move anywhere.
 * Testing against the clip instead of the node's actual contents has the
 * advantage that the node's contents can move or animate without affecting
 * content in other nodes. So we don't need to re-layerize during animations
 * (sync or async), and during async animations everything is guaranteed to
 * look correct.
 * The contents of a node's PaintedLayerData stack all share the node's
 * animated geometry root. The child nodes are on top of the PaintedLayerData
 * stack, in z-order, and the clip rects of the child nodes are allowed to
 * intersect with the visible region or visible above region of their parent
 * node's PaintedLayerDatas.
 */
class PaintedLayerDataNode {
public:
  PaintedLayerDataNode(PaintedLayerDataTree& aTree,
                       PaintedLayerDataNode* aParent,
                       AnimatedGeometryRoot* aAnimatedGeometryRoot);
  ~PaintedLayerDataNode();

  AnimatedGeometryRoot* GetAnimatedGeometryRoot() const { return mAnimatedGeometryRoot; }

  /**
   * Whether this node's contents can potentially intersect aRect.
   * aRect is in our tree's ContainerState's coordinate space.
   */
  bool Intersects(const nsIntRect& aRect) const
    { return !mHasClip || mClipRect.Intersects(aRect); }

  /**
   * Create a PaintedLayerDataNode for aAnimatedGeometryRoot, add it to our
   * children, and return it.
   */
  PaintedLayerDataNode* AddChildNodeFor(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  /**
   * Find a PaintedLayerData in our mPaintedLayerDataStack that aItem can be
   * added to. Creates a new PaintedLayerData by calling
   * aNewPaintedLayerCallback if necessary.
   */
  template<typename NewPaintedLayerCallbackType>
  PaintedLayerData* FindPaintedLayerFor(const nsIntRect& aVisibleRect,
                                        bool aBackfaceHidden,
                                        const ActiveScrolledRoot* aASR,
                                        const DisplayItemClipChain* aClipChain,
                                        NewPaintedLayerCallbackType aNewPaintedLayerCallback);

  /**
   * Find an opaque background color for aRegion. Pulls a color from the parent
   * geometry root if appropriate, but only if that color is present underneath
   * the whole clip of this node, so that this node's contents can animate or
   * move (possibly async) without having to change the background color.
   * @param aUnderIndex Searching will start in mPaintedLayerDataStack right
   *                    below aUnderIndex.
   */
  enum { ABOVE_TOP = -1 };
  nscolor FindOpaqueBackgroundColor(const nsIntRegion& aRegion,
                                    int32_t aUnderIndex = ABOVE_TOP) const;
  /**
   * Same as FindOpaqueBackgroundColor, but only returns a color if absolutely
   * nothing is in between, so that it can be used for a layer that can move
   * anywhere inside our clip.
   */
  nscolor FindOpaqueBackgroundColorCoveringEverything() const;

  /**
   * Adds aRect to this node's top PaintedLayerData's mVisibleAboveRegion,
   * or mVisibleAboveBackgroundRegion if mPaintedLayerDataStack is empty.
   */
  void AddToVisibleAboveRegion(const nsIntRect& aRect);
  /**
   * Call this if all of our existing content can potentially be covered, so
   * nothing can merge with it and all new content needs to create new items
   * on top. This will finish all of our children and pop our whole
   * mPaintedLayerDataStack.
   */
  void SetAllDrawingAbove();

  /**
   * Finish this node: Finish all children, finish our PaintedLayer contents,
   * and (if requested) adjust our parent's visible above region to include
   * our clip.
   */
  void Finish(bool aParentNeedsAccurateVisibleAboveRegion);

  /**
   * Finish any children that intersect aRect.
   */
  void FinishChildrenIntersecting(const nsIntRect& aRect);

  /**
   * Finish all children.
   */
  void FinishAllChildren() { FinishAllChildren(true); }

protected:
  /**
   * Finish all items in mPaintedLayerDataStack and clear the stack.
   */
  void PopAllPaintedLayerData();
  /**
   * Finish all of our child nodes, but don't touch mPaintedLayerDataStack.
   */
  void FinishAllChildren(bool aThisNodeNeedsAccurateVisibleAboveRegion);
  /**
   * Pass off opaque background color searching to our parent node, if we have
   * one.
   */
  nscolor FindOpaqueBackgroundColorInParentNode() const;

  PaintedLayerDataTree& mTree;
  PaintedLayerDataNode* mParent;
  AnimatedGeometryRoot* mAnimatedGeometryRoot;

  /**
   * Our contents: a PaintedLayerData stack and our child nodes.
   */
  AutoTArray<PaintedLayerData, 3> mPaintedLayerDataStack;

  /**
   * UniquePtr is used here in the sense of "unique ownership", i.e. there is
   * only one owner. Not in the sense of "this is the only pointer to the
   * node": There are two other, non-owning, pointers to our child nodes: The
   * node's respective children point to their parent node with their mParent
   * pointer, and the tree keeps a map of animated geometry root to node in its
   * mNodes member. These outside pointers are the reason that mChildren isn't
   * just an nsTArray<PaintedLayerDataNode> (since the pointers would become
   * invalid whenever the array expands its capacity).
   */
  nsTArray<UniquePtr<PaintedLayerDataNode>> mChildren;

  /**
   * The region that's covered between our "background" and the bottom of
   * mPaintedLayerDataStack. This is used to indicate whether we can pull
   * a background color from our parent node. If mVisibleAboveBackgroundRegion
   * should be considered infinite, mAllDrawingAboveBackground will be true and
   * the value of mVisibleAboveBackgroundRegion will be meaningless.
   */
  nsIntRegion mVisibleAboveBackgroundRegion;

  /**
   * Our clip, if we have any. If not, that means we can move anywhere, and
   * mHasClip will be false and mClipRect will be meaningless.
   */
  nsIntRect mClipRect;
  bool mHasClip;

  /**
   * Whether mVisibleAboveBackgroundRegion should be considered infinite.
   */
  bool mAllDrawingAboveBackground;
};

class ContainerState;

/**
 * A tree of PaintedLayerDataNodes. At any point in time, the tree only
 * contains nodes for animated geometry roots that new items can potentially
 * merge into. Any time content is added on top that overlaps existing things
 * in such a way that we no longer want to merge new items with some existing
 * content, that existing content gets "finished".
 * The public-facing methods of this class are FindPaintedLayerFor,
 * AddingOwnLayer, and Finish. The other public methods are for
 * PaintedLayerDataNode.
 * The tree calls out to its containing ContainerState for some things.
 * All coordinates / rects in the tree or the tree nodes are in the
 * ContainerState's coordinate space, i.e. relative to the reference frame and
 * in layer pixels.
 * The clip rects of sibling nodes never overlap. This is ensured by finishing
 * existing nodes before adding new ones, if this property were to be violated.
 * The root tree node doesn't get finished until the ContainerState is
 * finished.
 * The tree's root node is always the root reference frame of the builder. We
 * don't stop at the container state's mContainerAnimatedGeometryRoot because
 * some of our contents can have animated geometry roots that are not
 * descendants of the container's animated geometry root. Every animated
 * geometry root we encounter for our contents needs to have a defined place in
 * the tree.
 */
class PaintedLayerDataTree {
public:
  PaintedLayerDataTree(ContainerState& aContainerState,
                       nscolor& aBackgroundColor)
    : mContainerState(aContainerState)
    , mContainerUniformBackgroundColor(aBackgroundColor)
    , mForInactiveLayer(false)
  {}

  ~PaintedLayerDataTree()
  {
    MOZ_ASSERT(!mRoot);
    MOZ_ASSERT(mNodes.Count() == 0);
  }

  void InitializeForInactiveLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  /**
   * Notify our contents that some non-PaintedLayer content has been added.
   * *aRect needs to be a rectangle that doesn't move with respect to
   * aAnimatedGeometryRoot and that contains the added item.
   * If aRect is null, the extents will be considered infinite.
   * If aOutUniformBackgroundColor is non-null, it will be set to an opaque
   * color that can be pulled into the background of the added content, or
   * transparent if that is not possible.
   */
  void AddingOwnLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                      const nsIntRect* aRect,
                      nscolor* aOutUniformBackgroundColor);

  /**
   * Find a PaintedLayerData for aItem. This can either be an existing
   * PaintedLayerData from inside a node in our tree, or a new one that gets
   * created by a call out to aNewPaintedLayerCallback.
   */
  template<typename NewPaintedLayerCallbackType>
  PaintedLayerData* FindPaintedLayerFor(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                        const ActiveScrolledRoot* aASR,
                                        const DisplayItemClipChain* aClipChain,
                                        const nsIntRect& aVisibleRect,
                                        const bool aBackfaceHidden,
                                        NewPaintedLayerCallbackType aNewPaintedLayerCallback);

  /**
   * Finish everything.
   */
  void Finish();

  /**
   * Get the parent animated geometry root of aAnimatedGeometryRoot.
   * That's either aAnimatedGeometryRoot's animated geometry root, or, if
   * that's aAnimatedGeometryRoot itself, then it's the animated geometry
   * root for aAnimatedGeometryRoot's cross-doc parent frame.
   */
  AnimatedGeometryRoot* GetParentAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  /**
   * Whether aAnimatedGeometryRoot has an intrinsic clip that doesn't move with
   * respect to aAnimatedGeometryRoot's parent animated geometry root.
   * If aAnimatedGeometryRoot is a scroll frame, this will be the scroll frame's
   * scroll port, otherwise there is no clip.
   * This method doesn't have much to do with PaintedLayerDataTree, but this is
   * where we have easy access to a display list builder, which we use to get
   * the clip rect result into the right coordinate space.
   */
  bool IsClippedWithRespectToParentAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                        nsIntRect* aOutClip);

  /**
   * Called by PaintedLayerDataNode when it is finished, so that we can drop
   * our pointers to it.
   */
  void NodeWasFinished(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  nsDisplayListBuilder* Builder() const;
  ContainerState& ContState() const { return mContainerState; }
  nscolor UniformBackgroundColor() const { return mContainerUniformBackgroundColor; }

protected:
  /**
   * Finish all nodes that potentially intersect *aRect, where *aRect is a rect
   * that doesn't move with respect to aAnimatedGeometryRoot.
   * If aRect is null, *aRect will be considered infinite.
   */
  void FinishPotentiallyIntersectingNodes(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                          const nsIntRect* aRect);

  /**
   * Make sure that there is a node for aAnimatedGeometryRoot and all of its
   * ancestor geometry roots. Return the node for aAnimatedGeometryRoot.
   */
  PaintedLayerDataNode* EnsureNodeFor(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  /**
   * Find an existing node in the tree for an ancestor of aAnimatedGeometryRoot.
   * *aOutAncestorChild will be set to the last ancestor that was encountered
   * in the search up from aAnimatedGeometryRoot; it will be a child animated
   * geometry root of the result, if neither are null.
   */
  PaintedLayerDataNode*
    FindNodeForAncestorAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                            AnimatedGeometryRoot** aOutAncestorChild);

  ContainerState& mContainerState;
  Maybe<PaintedLayerDataNode> mRoot;

  /**
   * The uniform opaque color from behind this container layer, or
   * NS_RGBA(0,0,0,0) if the background behind this container layer is not
   * uniform and opaque. This color can be pulled into PaintedLayers that are
   * directly above the background.
   */
  nscolor mContainerUniformBackgroundColor;

  /**
   * A hash map for quick access the node belonging to a particular animated
   * geometry root.
   */
  nsDataHashtable<nsPtrHashKey<AnimatedGeometryRoot>, PaintedLayerDataNode*> mNodes;

  bool mForInactiveLayer;
};

/**
 * This is a helper object used to build up the layer children for
 * a ContainerLayer.
 */
class ContainerState {
public:
  ContainerState(nsDisplayListBuilder* aBuilder,
                 LayerManager* aManager,
                 FrameLayerBuilder* aLayerBuilder,
                 nsIFrame* aContainerFrame,
                 nsDisplayItem* aContainerItem,
                 const nsRect& aContainerBounds,
                 ContainerLayer* aContainerLayer,
                 const ContainerLayerParameters& aParameters,
                 nscolor aBackgroundColor,
                 const ActiveScrolledRoot* aContainerASR,
                 const ActiveScrolledRoot* aContainerScrollMetadataASR,
                 const ActiveScrolledRoot* aContainerCompositorASR) :
    mBuilder(aBuilder), mManager(aManager),
    mLayerBuilder(aLayerBuilder),
    mContainerFrame(aContainerFrame),
    mContainerLayer(aContainerLayer),
    mContainerBounds(aContainerBounds),
    mContainerASR(aContainerASR),
    mContainerScrollMetadataASR(aContainerScrollMetadataASR),
    mContainerCompositorASR(aContainerCompositorASR),
    mParameters(aParameters),
    mPaintedLayerDataTree(*this, aBackgroundColor),
    mLastDisplayPortAGR(nullptr)
  {
    nsPresContext* presContext = aContainerFrame->PresContext();
    mAppUnitsPerDevPixel = presContext->AppUnitsPerDevPixel();
    mContainerReferenceFrame =
      const_cast<nsIFrame*>(aContainerItem ? aContainerItem->ReferenceFrameForChildren() :
                                             mBuilder->FindReferenceFrameFor(mContainerFrame));
    bool isAtRoot = !aContainerItem || (aContainerItem->Frame() == mBuilder->RootReferenceFrame());
    MOZ_ASSERT(!isAtRoot || mContainerReferenceFrame == mBuilder->RootReferenceFrame());
    mContainerAnimatedGeometryRoot = isAtRoot
      ? aBuilder->GetRootAnimatedGeometryRoot()
      : aContainerItem->GetAnimatedGeometryRoot();
    MOZ_ASSERT(!mBuilder->IsPaintingToWindow() ||
      nsLayoutUtils::IsAncestorFrameCrossDoc(mBuilder->RootReferenceFrame(),
                                             *mContainerAnimatedGeometryRoot));
    // When AllowResidualTranslation is false, display items will be drawn
    // scaled with a translation by integer pixels, so we know how the snapping
    // will work.
    mSnappingEnabled = aManager->IsSnappingEffectiveTransforms() &&
      !mParameters.AllowResidualTranslation();
    CollectOldLayers();
  }

  /**
   * This is the method that actually walks a display list and builds
   * the child layers.
   */
  void ProcessDisplayItems(nsDisplayList* aList);
  /**
   * This finalizes all the open PaintedLayers by popping every element off
   * mPaintedLayerDataStack, then sets the children of the container layer
   * to be all the layers in mNewChildLayers in that order and removes any
   * layers as children of the container that aren't in mNewChildLayers.
   * @param aTextContentFlags if any child layer has CONTENT_COMPONENT_ALPHA,
   * set *aTextContentFlags to CONTENT_COMPONENT_ALPHA
   */
  void Finish(uint32_t *aTextContentFlags,
              const nsIntRect& aContainerPixelBounds,
              nsDisplayList* aChildItems);

  nscoord GetAppUnitsPerDevPixel() { return mAppUnitsPerDevPixel; }

  nsIntRect ScaleToNearestPixels(const nsRect& aRect) const
  {
    return aRect.ScaleToNearestPixels(mParameters.mXScale, mParameters.mYScale,
                                      mAppUnitsPerDevPixel);
  }
  nsIntRegion ScaleRegionToNearestPixels(const nsRegion& aRegion) const
  {
    return aRegion.ScaleToNearestPixels(mParameters.mXScale, mParameters.mYScale,
                                        mAppUnitsPerDevPixel);
  }
  nsIntRect ScaleToOutsidePixels(const nsRect& aRect, bool aSnap = false) const
  {
    if (aSnap && mSnappingEnabled) {
      return ScaleToNearestPixels(aRect);
    }
    return aRect.ScaleToOutsidePixels(mParameters.mXScale, mParameters.mYScale,
                                      mAppUnitsPerDevPixel);
  }
  nsIntRegion ScaleToOutsidePixels(const nsRegion& aRegion, bool aSnap = false) const
  {
    if (aSnap && mSnappingEnabled) {
      return ScaleRegionToNearestPixels(aRegion);
    }
    return aRegion.ScaleToOutsidePixels(mParameters.mXScale, mParameters.mYScale,
                                        mAppUnitsPerDevPixel);
  }
  nsIntRect ScaleToInsidePixels(const nsRect& aRect, bool aSnap = false) const
  {
    if (aSnap && mSnappingEnabled) {
      return ScaleToNearestPixels(aRect);
    }
    return aRect.ScaleToInsidePixels(mParameters.mXScale, mParameters.mYScale,
                                     mAppUnitsPerDevPixel);
  }

  nsIntRegion ScaleRegionToInsidePixels(const nsRegion& aRegion, bool aSnap = false) const
  {
    if (aSnap && mSnappingEnabled) {
      return ScaleRegionToNearestPixels(aRegion);
    }
    return aRegion.ScaleToInsidePixels(mParameters.mXScale, mParameters.mYScale,
                                        mAppUnitsPerDevPixel);
  }

  nsIntRegion ScaleRegionToOutsidePixels(const nsRegion& aRegion, bool aSnap = false) const
  {
    if (aSnap && mSnappingEnabled) {
      return ScaleRegionToNearestPixels(aRegion);
    }
    return aRegion.ScaleToOutsidePixels(mParameters.mXScale, mParameters.mYScale,
                                        mAppUnitsPerDevPixel);
  }

  nsIFrame* GetContainerFrame() const { return mContainerFrame; }
  nsDisplayListBuilder* Builder() const { return mBuilder; }

  /**
   * Check if we are currently inside an inactive layer.
   */
  bool IsInInactiveLayer() const {
    return mLayerBuilder->GetContainingPaintedLayerData();
  }

  /**
   * Sets aOuterVisibleRegion as aLayer's visible region.
   * @param aOuterVisibleRegion
   *   is in the coordinate space of the container reference frame.
   * @param aLayerContentsVisibleRect, if non-null, is in the layer's own
   *   coordinate system.
   * @param aOuterUntransformed is true if the given aOuterVisibleRegion
   *   is already untransformed with the matrix of the layer.
   */
  void SetOuterVisibleRegionForLayer(Layer* aLayer,
                                     const nsIntRegion& aOuterVisibleRegion,
                                     const nsIntRect* aLayerContentsVisibleRect = nullptr,
                                     bool aOuterUntransformed = false) const;

  /**
   * Try to determine whether the PaintedLayer aData has a single opaque color
   * covering aRect. If successful, return that color, otherwise return
   * NS_RGBA(0,0,0,0).
   * If aRect turns out not to intersect any content in the layer,
   * *aOutIntersectsLayer will be set to false.
   */
  nscolor FindOpaqueBackgroundColorInLayer(const PaintedLayerData* aData,
                                           const nsIntRect& aRect,
                                           bool* aOutIntersectsLayer) const;

  /**
   * Indicate that we are done adding items to the PaintedLayer represented by
   * aData. Make sure that a real PaintedLayer exists for it, and set the final
   * visible region and opaque-content.
   */
  template<typename FindOpaqueBackgroundColorCallbackType>
  void FinishPaintedLayerData(PaintedLayerData& aData, FindOpaqueBackgroundColorCallbackType aFindOpaqueBackgroundColor);

protected:
  friend class PaintedLayerData;
  friend class FLBDisplayItemIterator;

  LayerManager::PaintedLayerCreationHint
    GetLayerCreationHint(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  /**
   * Creates a new PaintedLayer and sets up the transform on the PaintedLayer
   * to account for scrolling.
   */
  already_AddRefed<PaintedLayer> CreatePaintedLayer(PaintedLayerData* aData);

  /**
   * Find a PaintedLayer for recycling, recycle it and prepare it for use, or
   * return null if no suitable layer was found.
   */
  already_AddRefed<PaintedLayer> AttemptToRecyclePaintedLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                              nsDisplayItem* aItem,
                                                              const nsPoint& aTopLeft,
                                                              const nsIFrame* aReferenceFrame);
  /**
   * Recycle aLayer and do any necessary invalidation.
   */
  PaintedDisplayItemLayerUserData* RecyclePaintedLayer(PaintedLayer* aLayer,
                                                       AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                       bool& didResetScrollPositionForLayerPixelAlignment);

  /**
   * Perform the last step of CreatePaintedLayer / AttemptToRecyclePaintedLayer:
   * Initialize aData, set up the layer's transform for scrolling, and
   * invalidate the layer for layer pixel alignment changes if necessary.
   */
  void PreparePaintedLayerForUse(PaintedLayer* aLayer,
                                 PaintedDisplayItemLayerUserData* aData,
                                 AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                 const nsIFrame* aReferenceFrame,
                                 const nsPoint& aTopLeft,
                                 bool aDidResetScrollPositionForLayerPixelAlignment);

  /**
   * Attempt to prepare an ImageLayer based upon the provided PaintedLayerData.
   * Returns nullptr on failure.
   */
  already_AddRefed<Layer> PrepareImageLayer(PaintedLayerData* aData);

  /**
   * Attempt to prepare a ColorLayer based upon the provided PaintedLayerData.
   * Returns nullptr on failure.
   */
  already_AddRefed<Layer> PrepareColorLayer(PaintedLayerData* aData);

  /**
   * Grab the next recyclable ColorLayer, or create one if there are no
   * more recyclable ColorLayers.
   */
  already_AddRefed<ColorLayer> CreateOrRecycleColorLayer(PaintedLayer* aPainted);
  /**
   * Grab the next recyclable ImageLayer, or create one if there are no
   * more recyclable ImageLayers.
   */
  already_AddRefed<ImageLayer> CreateOrRecycleImageLayer(PaintedLayer* aPainted);
  /**
   * Grab a recyclable ImageLayer for use as a mask layer for aLayer (that is a
   * mask layer which has been used for aLayer before), or create one if such
   * a layer doesn't exist.
   *
   * Since mask layers can exist either on the layer directly, or as a side-
   * attachment to FrameMetrics (for ancestor scrollframe clips), we key the
   * recycle operation on both the originating layer and the mask layer's
   * index in the layer, if any.
   */
  struct MaskLayerKey;
  template<typename UserData>
  already_AddRefed<ImageLayer> CreateOrRecycleMaskImageLayerFor(
      const MaskLayerKey& aKey,
      UserData* (*aGetUserData)(Layer* aLayer),
      void (*aSetDefaultUserData)(Layer* aLayer));
  /**
   * Grabs all PaintedLayers and ColorLayers from the ContainerLayer and makes them
   * available for recycling.
   */
  void CollectOldLayers();
  /**
   * If aItem used to belong to a PaintedLayer, invalidates the area of
   * aItem in that layer. If aNewLayer is a PaintedLayer, invalidates the area of
   * aItem in that layer.
   */
  void InvalidateForLayerChange(nsDisplayItem* aItem,
                                PaintedLayer* aNewLayer,
                                DisplayItemData* aData);
  /**
   * Returns true if aItem's opaque area (in aOpaque) covers the entire
   * scrollable area of its presshell.
   */
  bool ItemCoversScrollableArea(nsDisplayItem* aItem, const nsRegion& aOpaque);

  /**
   * Set ScrollMetadata and scroll-induced clipping on aEntry's layer.
   */
  void SetupScrollingMetadata(NewLayerEntry* aEntry);

  /**
   * Applies occlusion culling.
   * For each layer in mNewChildLayers, remove from its visible region the
   * opaque regions of the layers at higher z-index, but only if they have
   * the same animated geometry root and fixed-pos frame ancestor.
   * The opaque region for the child layers that share the same animated
   * geometry root as the container frame is returned in
   * *aOpaqueRegionForContainer.
   *
   * Also sets scroll metadata on the layers.
   */
  void PostprocessRetainedLayers(nsIntRegion* aOpaqueRegionForContainer);

  /**
   * Computes the snapped opaque area of aItem. Sets aList's opaque flag
   * if it covers the entire list bounds. Sets *aHideAllLayersBelow to true
   * this item covers the entire viewport so that all layers below are
   * permanently invisible.
   */
  nsIntRegion ComputeOpaqueRect(nsDisplayItem* aItem,
                                AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                const ActiveScrolledRoot* aASR,
                                const DisplayItemClip& aClip,
                                nsDisplayList* aList,
                                bool* aHideAllLayersBelow,
                                bool* aOpaqueForAnimatedGeometryRootParent);

  /**
   * Fills a PaintedLayerData object that is initialized for a layer that the
   * current item will be assigned to. Also creates mNewChildLayers entries.
   * @param  aData                 The PaintedLayerData that will be filled.
   * @param  aVisibleRect          The visible rect of the item.
   * @param  aAnimatedGeometryRoot The item's animated geometry root.
   * @param  aASR                  The active scrolled root that moves this PaintedLayer.
   * @param  aClipChain            The clip chain that the compositor needs to
   *                               apply to this layer.
   * @param  aScrollMetadataASR    The leaf ASR for which scroll metadata needs to be
   *                               set on the layer, because either the layer itself
   *                               or its scrolled clip need to move with that ASR.
   * @param  aTopLeft              The offset between aAnimatedGeometryRoot and
   *                               the reference frame.
   * @param  aReferenceFrame       The reference frame for the item.
   * @param  aBackfaceHidden       The backface visibility for the item frame.
   */
  void NewPaintedLayerData(PaintedLayerData* aData,
                           AnimatedGeometryRoot* aAnimatedGeometryRoot,
                           const ActiveScrolledRoot* aASR,
                           const DisplayItemClipChain* aClipChain,
                           const ActiveScrolledRoot* aScrollMetadataASR,
                           const nsPoint& aTopLeft,
                           const nsIFrame* aReferenceFrame,
                           const bool aBackfaceHidden);

  /* Build a mask layer to represent the clipping region. Will return null if
   * there is no clipping specified or a mask layer cannot be built.
   * Builds an ImageLayer for the appropriate backend; the mask is relative to
   * aLayer's visible region.
   * aLayer is the layer to be clipped.
   * relative to the container reference frame
   * aRoundedRectClipCount is used when building mask layers for PaintedLayers,
   */
  void SetupMaskLayer(Layer *aLayer, const DisplayItemClip& aClip);

  /**
   * If |aClip| has rounded corners, create a mask layer for them, and
   * add it to |aLayer|'s ancestor mask layers, returning an index into
   * the array of ancestor mask layers. Returns an empty Maybe if
   * |aClip| does not have rounded corners, or if no mask layer could
   * be created.
   */
  Maybe<size_t> SetupMaskLayerForScrolledClip(Layer* aLayer,
                                              const DisplayItemClip& aClip);

  /*
   * Create/find a mask layer with suitable size for aMaskItem to paint
   * css-positioned-masking onto.
   */
  void SetupMaskLayerForCSSMask(Layer* aLayer, nsDisplayMask* aMaskItem);

  already_AddRefed<Layer> CreateMaskLayer(
    Layer *aLayer, const DisplayItemClip& aClip,
    const Maybe<size_t>& aForAncestorMaskLayer);

  /**
   * Get the display port for an AGR.
   * The result would be cached for later reusing.
   */
  nsRect GetDisplayPortForAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot);

  nsDisplayListBuilder*            mBuilder;
  LayerManager*                    mManager;
  FrameLayerBuilder*               mLayerBuilder;
  nsIFrame*                        mContainerFrame;
  nsIFrame*                        mContainerReferenceFrame;
  AnimatedGeometryRoot*            mContainerAnimatedGeometryRoot;
  ContainerLayer*                  mContainerLayer;
  nsRect                           mContainerBounds;

  // Due to the way we store scroll annotations in the layer tree, we need to
  // keep track of three (possibly different) ASRs here.
  // mContainerASR is the ASR of the container display item that this
  // ContainerState was created for.
  // mContainerScrollMetadataASR is the ASR of the leafmost scroll metadata
  // that's in effect on mContainerLayer.
  // mContainerCompositorASR is the ASR that mContainerLayer moves with on
  // the compositor / APZ side, taking into account both the scroll meta data
  // and the fixed position annotation on itself and its ancestors.
  const ActiveScrolledRoot*        mContainerASR;
  const ActiveScrolledRoot*        mContainerScrollMetadataASR;
  const ActiveScrolledRoot*        mContainerCompositorASR;
#ifdef DEBUG
  nsRect                           mAccumulatedChildBounds;
#endif
  ContainerLayerParameters         mParameters;
  /**
   * The region of PaintedLayers that should be invalidated every time
   * we recycle one.
   */
  nsIntRegion                      mInvalidPaintedContent;
  PaintedLayerDataTree             mPaintedLayerDataTree;
  /**
   * We collect the list of children in here. During ProcessDisplayItems,
   * the layers in this array either have mContainerLayer as their parent,
   * or no parent.
   * PaintedLayers have two entries in this array: the second one is used only if
   * the PaintedLayer is optimized away to a ColorLayer or ImageLayer.
   * It's essential that this array is only appended to, since PaintedLayerData
   * records the index of its PaintedLayer in this array.
   */
  typedef AutoTArray<NewLayerEntry,1> AutoLayersArray;
  AutoLayersArray                  mNewChildLayers;
  nsTHashtable<nsRefPtrHashKey<PaintedLayer>> mPaintedLayersAvailableForRecycling;
  nscoord                          mAppUnitsPerDevPixel;
  bool                             mSnappingEnabled;

  struct MaskLayerKey {
    MaskLayerKey() : mLayer(nullptr) {}
    MaskLayerKey(Layer* aLayer, const Maybe<size_t>& aAncestorIndex)
      : mLayer(aLayer),
        mAncestorIndex(aAncestorIndex)
    {}

    PLDHashNumber Hash() const {
      // Hash the layer and add the layer index to the hash.
      return (NS_PTR_TO_UINT32(mLayer) >> 2)
             + (mAncestorIndex ? (*mAncestorIndex + 1) : 0);
    }
    bool operator ==(const MaskLayerKey& aOther) const {
      return mLayer == aOther.mLayer &&
             mAncestorIndex == aOther.mAncestorIndex;
    }

    Layer* mLayer;
    Maybe<size_t> mAncestorIndex;
  };

  nsDataHashtable<nsGenericHashKey<MaskLayerKey>, RefPtr<ImageLayer>>
    mRecycledMaskImageLayers;
  // Keep display port of AGR to avoid wasting time on doing the same
  // thing repeatly.
  AnimatedGeometryRoot* mLastDisplayPortAGR;
  nsRect mLastDisplayPortRect;

  // Cache ScrollMetadata so it doesn't need recomputed if the ASR and clip are unchanged.
  // If mASR == nullptr then mMetadata is not valid.
  struct CachedScrollMetadata {
    const ActiveScrolledRoot* mASR;
    const DisplayItemClip* mClip;
    Maybe<ScrollMetadata> mMetadata;

    CachedScrollMetadata()
      : mASR(nullptr), mClip(nullptr)
    {}
  };
  CachedScrollMetadata mCachedScrollMetadata;
};

bool
FLBDisplayItemIterator::ShouldFlattenNextItem() const
{
  if (!mNext) {
    return false;
  }

  if (!mNext->ShouldFlattenAway(mBuilder)) {
    return false;
  }

  if (mNext->GetType() == DisplayItemType::TYPE_OPACITY) {
    nsDisplayOpacity* opacity = static_cast<nsDisplayOpacity*>(mNext);

    if (opacity->OpacityAppliedToChildren()) {
      // This is the previous opacity flattening path, where the opacity has
      // been applied to children.
      return true;
    }

    if (!mState->mManager->IsWidgetLayerManager()) {
      // Do not flatten opacity inside an inactive layer tree.
      return false;
    }

    LayerState layerState = mNext->GetLayerState(mState->mBuilder,
                                                 mState->mManager,
                                                 mState->mParameters);

    // Do not flatten opacity if child display items require an active layer.
    return (layerState == LayerState::LAYER_NONE ||
            layerState == LayerState::LAYER_INACTIVE);
  }

  return true;
}

class PaintedDisplayItemLayerUserData : public LayerUserData
{
public:
  PaintedDisplayItemLayerUserData() :
    mForcedBackgroundColor(NS_RGBA(0,0,0,0)),
    mXScale(1.f), mYScale(1.f),
    mAppUnitsPerDevPixel(0),
    mTranslation(0, 0),
    mAnimatedGeometryRootPosition(0, 0),
    mLastItemCount(0),
    mContainerLayerFrame(nullptr),
    mHasExplicitLastPaintOffset(false) {}

  NS_INLINE_DECL_REFCOUNTING(PaintedDisplayItemLayerUserData);

  /**
   * A color that should be painted over the bounds of the layer's visible
   * region before any other content is painted.
   */
  nscolor mForcedBackgroundColor;

  /**
   * The resolution scale used.
   */
  float mXScale, mYScale;

  /**
   * The appunits per dev pixel for the items in this layer.
   */
  nscoord mAppUnitsPerDevPixel;

  /**
   * The offset from the PaintedLayer's 0,0 to the
   * reference frame. This isn't necessarily the same as the transform
   * set on the PaintedLayer since we might also be applying an extra
   * offset specified by the parent ContainerLayer/
   */
  nsIntPoint mTranslation;

  /**
   * We try to make 0,0 of the PaintedLayer be the top-left of the
   * border-box of the "active scrolled root" frame (i.e. the nearest ancestor
   * frame for the display items that is being actively scrolled). But
   * we force the PaintedLayer transform to be an integer translation, and we may
   * have a resolution scale, so we have to snap the PaintedLayer transform, so
   * 0,0 may not be exactly the top-left of the active scrolled root. Here we
   * store the coordinates in PaintedLayer space of the top-left of the
   * active scrolled root.
   */
  gfxPoint mAnimatedGeometryRootPosition;

  nsIntRegion mRegionToInvalidate;

  // The offset between the active scrolled root of this layer
  // and the root of the container for the previous and current
  // paints respectively.
  nsPoint mLastAnimatedGeometryRootOrigin;
  nsPoint mAnimatedGeometryRootOrigin;

  RefPtr<ColorLayer> mColorLayer;
  RefPtr<ImageLayer> mImageLayer;

  // The region for which display item visibility for this layer has already
  // been calculated. Used to reduce the number of calls to
  // RecomputeVisibilityForItems if it is known in advance that a larger
  // region will be painted during a transaction than in a single call to
  // DrawPaintedLayer, for example when progressive paint is enabled.
  nsIntRegion mVisibilityComputedRegion;

  // The number of items assigned to this layer on the previous paint.
  size_t mLastItemCount;

  // The translation set on this PaintedLayer before we started updating the
  // layer tree.
  nsIntPoint mLastPaintOffset;

  // Temporary state only valid during the FrameLayerBuilder's lifetime.
  // FLB's mPaintedLayerItems is responsible for cleaning these up when
  // we finish painting to avoid dangling pointers.
  nsTArray<AssignedDisplayItem> mItems;
  nsIFrame* mContainerLayerFrame;

  bool mHasExplicitLastPaintOffset;

  /**
   * This is set when the painted layer has no component alpha.
   */
  bool mDisabledAlpha;

protected:
  ~PaintedDisplayItemLayerUserData() = default;
};

FrameLayerBuilder::FrameLayerBuilder()
  : mRetainingManager(nullptr)
  , mContainingPaintedLayer(nullptr)
  , mInactiveLayerClip(nullptr)
  , mInvalidateAllLayers(false)
  , mInLayerTreeCompressionMode(false)
  , mIsInactiveLayerManager(false)
{
  MOZ_COUNT_CTOR(FrameLayerBuilder);
}

FrameLayerBuilder::~FrameLayerBuilder()
{
  GetMaskLayerImageCache()->Sweep();
  for (PaintedDisplayItemLayerUserData* userData : mPaintedLayerItems) {
    userData->mItems.Clear();
    userData->mContainerLayerFrame = nullptr;
  }
  MOZ_COUNT_DTOR(FrameLayerBuilder);
}

void
FrameLayerBuilder::AddPaintedLayerItemsEntry(PaintedDisplayItemLayerUserData* aData)
{
  mPaintedLayerItems.AppendElement(aData);
}

/*
 * User data for layers which will be used as masks.
 */
struct MaskLayerUserData : public LayerUserData
{
  MaskLayerUserData()
    : mScaleX(-1.0f)
    , mScaleY(-1.0f)
    , mAppUnitsPerDevPixel(-1)
  { }
  MaskLayerUserData(const DisplayItemClip& aClip,
                    int32_t aAppUnitsPerDevPixel,
                    const ContainerLayerParameters& aParams)
    : mScaleX(aParams.mXScale)
    , mScaleY(aParams.mYScale)
    , mOffset(aParams.mOffset)
    , mAppUnitsPerDevPixel(aAppUnitsPerDevPixel)
  {
    aClip.AppendRoundedRects(&mRoundedClipRects);
  }

  void operator=(MaskLayerUserData&& aOther)
  {
    mScaleX = aOther.mScaleX;
    mScaleY = aOther.mScaleY;
    mOffset = aOther.mOffset;
    mAppUnitsPerDevPixel = aOther.mAppUnitsPerDevPixel;
    mRoundedClipRects.SwapElements(aOther.mRoundedClipRects);
  }

  bool
  operator== (const MaskLayerUserData& aOther) const
  {
    return mRoundedClipRects == aOther.mRoundedClipRects &&
           mScaleX == aOther.mScaleX &&
           mScaleY == aOther.mScaleY &&
           mOffset == aOther.mOffset &&
           mAppUnitsPerDevPixel == aOther.mAppUnitsPerDevPixel;
  }

  // Keeps a MaskLayerImageKey alive by managing its mLayerCount member-var
  MaskLayerImageCache::MaskLayerImageKeyRef mImageKey;
  // properties of the mask layer; the mask layer may be re-used if these
  // remain unchanged.
  nsTArray<DisplayItemClip::RoundedRect> mRoundedClipRects;
  // scale from the masked layer which is applied to the mask
  float mScaleX, mScaleY;
  // The ContainerLayerParameters offset which is applied to the mask's transform.
  nsIntPoint mOffset;
  int32_t mAppUnitsPerDevPixel;
};

/*
 * User data for layers which will be used as masks for css positioned mask.
 */
struct CSSMaskLayerUserData : public LayerUserData
{
  CSSMaskLayerUserData()
    : mMaskStyle(nsStyleImageLayers::LayerType::Mask)
  { }

  CSSMaskLayerUserData(nsIFrame* aFrame, const nsIntRect& aMaskBounds,
                       const nsPoint& aMaskLayerOffset)
    : mMaskBounds(aMaskBounds),
      mMaskStyle(aFrame->StyleSVGReset()->mMask),
      mMaskLayerOffset(aMaskLayerOffset)
  {
  }

  void operator=(CSSMaskLayerUserData&& aOther)
  {
    mMaskBounds = aOther.mMaskBounds;
    mMaskStyle = Move(aOther.mMaskStyle);
    mMaskLayerOffset = aOther.mMaskLayerOffset;
  }

  bool
  operator==(const CSSMaskLayerUserData& aOther) const
  {
    if (!mMaskBounds.IsEqualInterior(aOther.mMaskBounds)) {
      return false;
    }

    // Make sure we draw the same portion of the mask onto mask layer.
    if (mMaskLayerOffset != aOther.mMaskLayerOffset) {
      return false;
    }

    return mMaskStyle == aOther.mMaskStyle;
  }

private:
  nsIntRect mMaskBounds;
  nsStyleImageLayers mMaskStyle;
  nsPoint mMaskLayerOffset; // The offset from the origin of mask bounds to
                            // the origin of mask layer.
};

/*
 * A helper object to create a draw target for painting mask and create a
 * image container to hold the drawing result. The caller can then bind this
 * image container with a image mask layer via ImageLayer::SetContainer.
 */
class MaskImageData
{
public:
  MaskImageData(const gfx::IntSize& aSize, LayerManager* aLayerManager)
    : mTextureClientLocked(false)
    , mSize(aSize)
    , mLayerManager(aLayerManager)
  {
    MOZ_ASSERT(!mSize.IsEmpty());
    MOZ_ASSERT(mLayerManager);
  }

  ~MaskImageData()
  {
    if (mTextureClientLocked) {
      MOZ_ASSERT(mTextureClient);
      // Clear DrawTarget before Unlock.
      mDrawTarget = nullptr;
      mTextureClient->Unlock();
    }
  }

  gfx::DrawTarget* CreateDrawTarget()
  {
    if (mDrawTarget) {
      return mDrawTarget;
    }

    if (mLayerManager->GetBackendType() == LayersBackend::LAYERS_BASIC) {
      mDrawTarget = mLayerManager->CreateOptimalMaskDrawTarget(mSize);
      return mDrawTarget;
    }

    MOZ_ASSERT(mLayerManager->GetBackendType() == LayersBackend::LAYERS_CLIENT ||
               mLayerManager->GetBackendType() == LayersBackend::LAYERS_WR);

    KnowsCompositor* knowsCompositor = mLayerManager->AsKnowsCompositor();
    if (!knowsCompositor) {
      return nullptr;
    }
    mTextureClient =
      TextureClient::CreateForDrawing(knowsCompositor,
                                      SurfaceFormat::A8,
                                      mSize,
                                      BackendSelector::Content,
                                      TextureFlags::DISALLOW_BIGIMAGE,
                                      TextureAllocationFlags::ALLOC_CLEAR_BUFFER);
    if (!mTextureClient) {
      return nullptr;
    }

    mTextureClientLocked = mTextureClient->Lock(OpenMode::OPEN_READ_WRITE);
    if (!mTextureClientLocked) {
      return nullptr;
    }

    mDrawTarget = mTextureClient->BorrowDrawTarget();
    return mDrawTarget;
  }

  already_AddRefed<ImageContainer> CreateImageAndImageContainer()
  {
    RefPtr<ImageContainer> container = LayerManager::CreateImageContainer();
    RefPtr<Image> image = CreateImage();

    if (!image) {
      return nullptr;
    }
    container->SetCurrentImageInTransaction(image);

    return container.forget();
  }

private:
  already_AddRefed<Image> CreateImage()
  {
    if (mLayerManager->GetBackendType() == LayersBackend::LAYERS_BASIC &&
        mDrawTarget) {
      RefPtr<SourceSurface> surface = mDrawTarget->Snapshot();
      RefPtr<SourceSurfaceImage> image = new SourceSurfaceImage(mSize, surface);
      // Disallow BIGIMAGE (splitting into multiple textures) for mask
      // layer images
      image->SetTextureFlags(TextureFlags::DISALLOW_BIGIMAGE);
      return image.forget();
    }

    if ((mLayerManager->GetBackendType() == LayersBackend::LAYERS_CLIENT ||
         mLayerManager->GetBackendType() == LayersBackend::LAYERS_WR) &&
        mTextureClient &&
        mDrawTarget) {
      RefPtr<TextureWrapperImage> image =
          new TextureWrapperImage(mTextureClient, gfx::IntRect(gfx::IntPoint(0, 0), mSize));
      return image.forget();
    }

    return nullptr;
  }

  bool mTextureClientLocked;
  gfx::IntSize mSize;
  LayerManager* mLayerManager;
  RefPtr<gfx::DrawTarget> mDrawTarget;
  RefPtr<TextureClient> mTextureClient;
};

static PaintedDisplayItemLayerUserData*
GetPaintedDisplayItemLayerUserData(Layer* aLayer)
{
  return static_cast<PaintedDisplayItemLayerUserData*>(
    aLayer->GetUserData(&gPaintedDisplayItemLayerUserData));
}

/* static */ void
FrameLayerBuilder::Shutdown()
{
  if (gMaskLayerImageCache) {
    delete gMaskLayerImageCache;
    gMaskLayerImageCache = nullptr;
  }
}

void
FrameLayerBuilder::Init(nsDisplayListBuilder* aBuilder, LayerManager* aManager,
                        PaintedLayerData* aLayerData,
                        bool aIsInactiveLayerManager,
                        const DisplayItemClip* aInactiveLayerClip)
{
  mDisplayListBuilder = aBuilder;
  mRootPresContext = aBuilder->RootReferenceFrame()->PresContext()->GetRootPresContext();
  mContainingPaintedLayer = aLayerData;
  mIsInactiveLayerManager = aIsInactiveLayerManager;
  mInactiveLayerClip = aInactiveLayerClip;
  aManager->SetUserData(&gLayerManagerLayerBuilder, this);
}

void
FrameLayerBuilder::FlashPaint(gfxContext *aContext)
{
  float r = float(rand()) / RAND_MAX;
  float g = float(rand()) / RAND_MAX;
  float b = float(rand()) / RAND_MAX;
  aContext->SetColor(Color(r, g, b, 0.4f));
  aContext->Paint();
}

DisplayItemData*
FrameLayerBuilder::GetDisplayItemData(nsIFrame* aFrame, uint32_t aKey)
{
  const SmallPointerArray<DisplayItemData>& array = aFrame->DisplayItemData();
  for (uint32_t i = 0; i < array.Length(); i++) {
    DisplayItemData* item = DisplayItemData::AssertDisplayItemData(array.ElementAt(i));
    if (item->mDisplayItemKey == aKey &&
        item->mLayer->Manager() == mRetainingManager) {
      return item;
    }
  }
  return nullptr;
}

#ifdef MOZ_DUMP_PAINTING
static nsACString&
AppendToString(nsACString& s, const nsIntRect& r,
               const char* pfx="", const char* sfx="")
{
  s += pfx;
  s += nsPrintfCString(
    "(x=%d, y=%d, w=%d, h=%d)",
    r.x, r.y, r.width, r.height);
  return s += sfx;
}

static nsACString&
AppendToString(nsACString& s, const nsIntRegion& r,
               const char* pfx="", const char* sfx="")
{
  s += pfx;

  s += "< ";
  for (auto iter = r.RectIter(); !iter.Done(); iter.Next()) {
    AppendToString(s, iter.Get()) += "; ";
  }
  s += ">";

  return s += sfx;
}
#endif // MOZ_DUMP_PAINTING

/**
 * Invalidate aRegion in aLayer. aLayer is in the coordinate system
 * *after* aTranslation has been applied, so we need to
 * apply the inverse of that transform before calling InvalidateRegion.
 */
static void
InvalidatePostTransformRegion(PaintedLayer* aLayer, const nsIntRegion& aRegion,
                              const nsIntPoint& aTranslation)
{
  // Convert the region from the coordinates of the container layer
  // (relative to the snapped top-left of the display list reference frame)
  // to the PaintedLayer's own coordinates
  nsIntRegion rgn = aRegion;
  rgn.MoveBy(-aTranslation);
  aLayer->InvalidateRegion(rgn);
#ifdef MOZ_DUMP_PAINTING
  if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
    nsAutoCString str;
    AppendToString(str, rgn);
    printf_stderr("Invalidating layer %p: %s\n", aLayer, str.get());
  }
#endif
}

static void
InvalidatePostTransformRegion(PaintedLayer* aLayer, const nsRect& aRect,
                              const DisplayItemClip& aClip,
                              const nsIntPoint& aTranslation)
{
  PaintedDisplayItemLayerUserData* data =
      static_cast<PaintedDisplayItemLayerUserData*>(aLayer->GetUserData(&gPaintedDisplayItemLayerUserData));

  nsRect rect = aClip.ApplyNonRoundedIntersection(aRect);

  nsIntRect pixelRect = rect.ScaleToOutsidePixels(data->mXScale, data->mYScale, data->mAppUnitsPerDevPixel);
  InvalidatePostTransformRegion(aLayer, pixelRect, aTranslation);
}


static nsIntPoint
GetTranslationForPaintedLayer(PaintedLayer* aLayer)
{
  PaintedDisplayItemLayerUserData* data =
    static_cast<PaintedDisplayItemLayerUserData*>
      (aLayer->GetUserData(&gPaintedDisplayItemLayerUserData));
  NS_ASSERTION(data, "Must be a tracked painted layer!");

  return data->mTranslation;
}

/**
 * Some frames can have multiple, nested, retaining layer managers
 * associated with them (normal manager, inactive managers, SVG effects).
 * In these cases we store the 'outermost' LayerManager data property
 * on the frame since we can walk down the chain from there.
 *
 * If one of these frames has just been destroyed, we will free the inner
 * layer manager when removing the entry from mFramesWithLayers. Destroying
 * the layer manager destroys the LayerManagerData and calls into
 * the DisplayItemData destructor. If the inner layer manager had any
 * items with the same frame, then we attempt to retrieve properties
 * from the deleted frame.
 *
 * Cache the destroyed frame pointer here so we can avoid crashing in this case.
 */

/* static */ void
FrameLayerBuilder::RemoveFrameFromLayerManager(const nsIFrame* aFrame,
                                               SmallPointerArray<DisplayItemData>& aArray)
{
  MOZ_RELEASE_ASSERT(!sDestroyedFrame);
  sDestroyedFrame = aFrame;

  // Hold a reference to all the items so that they don't get
  // deleted from under us.
  nsTArray<RefPtr<DisplayItemData> > arrayCopy;
  for (DisplayItemData* data : aArray) {
    arrayCopy.AppendElement(data);
  }

#ifdef DEBUG_DISPLAY_ITEM_DATA
  if (aArray->Length()) {
    LayerManagerData *rootData = aArray->ElementAt(0)->mParent;
    while (rootData->mParent) {
      rootData = rootData->mParent;
    }
    printf_stderr("Removing frame %p - dumping display data\n", aFrame);
    rootData->Dump();
  }
#endif

  for (DisplayItemData* data : aArray) {
    PaintedLayer* t = data->mLayer ? data->mLayer->AsPaintedLayer() : nullptr;
    if (t) {
      PaintedDisplayItemLayerUserData* paintedData =
          static_cast<PaintedDisplayItemLayerUserData*>(t->GetUserData(&gPaintedDisplayItemLayerUserData));
      if (paintedData && data->mGeometry) {
        nsRegion old = data->mGeometry->ComputeInvalidationRegion();
        nsIntRegion rgn = old.ScaleToOutsidePixels(paintedData->mXScale, paintedData->mYScale, paintedData->mAppUnitsPerDevPixel);
        rgn.MoveBy(-GetTranslationForPaintedLayer(t));
        paintedData->mRegionToInvalidate.Or(paintedData->mRegionToInvalidate, rgn);
        paintedData->mRegionToInvalidate.SimplifyOutward(8);
      }
    }

    data->Disconnect();
    data->mParent->mDisplayItems.RemoveEntry(data);
  }

  arrayCopy.Clear();
  sDestroyedFrame = nullptr;
}

void
FrameLayerBuilder::DidBeginRetainedLayerTransaction(LayerManager* aManager)
{
  mRetainingManager = aManager;
  LayerManagerData* data = static_cast<LayerManagerData*>
    (aManager->GetUserData(&gLayerManagerUserData));
  if (data) {
    mInvalidateAllLayers = data->mInvalidateAllLayers;
  } else {
    data = new LayerManagerData(aManager);
    aManager->SetUserData(&gLayerManagerUserData, data);
  }
}

void
FrameLayerBuilder::DidEndTransaction()
{
  GetMaskLayerImageCache()->Sweep();
}

void
FrameLayerBuilder::WillEndTransaction()
{
  if (!mRetainingManager) {
    return;
  }

  // We need to save the data we'll need to support retaining.
  LayerManagerData* data = static_cast<LayerManagerData*>
    (mRetainingManager->GetUserData(&gLayerManagerUserData));
  NS_ASSERTION(data, "Must have data!");

  // Update all the frames that used to have layers.
  for (auto iter = data->mDisplayItems.Iter(); !iter.Done(); iter.Next()) {
    DisplayItemData* data = iter.Get()->GetKey();
    if (!data->mUsed) {
      // This item was visible, but isn't anymore.
      PaintedLayer* t = data->mLayer->AsPaintedLayer();
      if (t && data->mGeometry) {
#ifdef MOZ_DUMP_PAINTING
        if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
          printf_stderr("Invalidating unused display item (%i) belonging to frame %p from layer %p\n", data->mDisplayItemKey, data->mFrameList[0], t);
        }
#endif
        InvalidatePostTransformRegion(t,
                                      data->mGeometry->ComputeInvalidationRegion(),
                                      data->mClip,
                                      GetLastPaintOffset(t));
      }

      data->ClearAnimationCompositorState();
      data->Disconnect();
      iter.Remove();
    } else {
      ComputeGeometryChangeForItem(data);
    }
  }

  data->mInvalidateAllLayers = false;
}

/* static */ DisplayItemData*
FrameLayerBuilder::GetDisplayItemDataForManager(nsDisplayItem* aItem,
                                                LayerManager* aManager)
{
  const SmallPointerArray<DisplayItemData>& array =
    aItem->Frame()->DisplayItemData();
  for (uint32_t i = 0; i < array.Length(); i++) {
    DisplayItemData* item = DisplayItemData::AssertDisplayItemData(array.ElementAt(i));
    if (item->mDisplayItemKey == aItem->GetPerFrameKey() &&
        item->mLayer->Manager() == aManager) {
      return item;
    }
  }
  return nullptr;
}

bool
FrameLayerBuilder::HasRetainedDataFor(nsIFrame* aFrame, uint32_t aDisplayItemKey)
{
  const SmallPointerArray<DisplayItemData>& array =
    aFrame->DisplayItemData();
  for (uint32_t i = 0; i < array.Length(); i++) {
    if (DisplayItemData::AssertDisplayItemData(array.ElementAt(i))->mDisplayItemKey == aDisplayItemKey) {
      return true;
    }
  }
  if (RefPtr<WebRenderUserData> data = GetWebRenderUserData<WebRenderFallbackData>(aFrame, aDisplayItemKey)) {
    return true;
  }
  return false;
}

DisplayItemData*
FrameLayerBuilder::GetOldLayerForFrame(nsIFrame* aFrame, uint32_t aDisplayItemKey, DisplayItemData* aOldData /* = nullptr */)
{
  // If we need to build a new layer tree, then just refuse to recycle
  // anything.
  if (!mRetainingManager || mInvalidateAllLayers)
    return nullptr;

  DisplayItemData* data = aOldData;
  if (!data || data->Disconnected() || data->mLayer->Manager() != mRetainingManager) {
    data = GetDisplayItemData(aFrame, aDisplayItemKey);
  }
  MOZ_ASSERT(data == GetDisplayItemData(aFrame, aDisplayItemKey));

  if (data && data->mLayer->Manager() == mRetainingManager) {
    return data;
  }
  return nullptr;
}

Layer*
FrameLayerBuilder::GetOldLayerFor(nsDisplayItem* aItem,
                                  nsDisplayItemGeometry** aOldGeometry,
                                  DisplayItemClip** aOldClip)
{
  uint32_t key = aItem->GetPerFrameKey();
  nsIFrame* frame = aItem->Frame();

  DisplayItemData* oldData = GetOldLayerForFrame(frame, key);
  if (oldData) {
    if (aOldGeometry) {
      *aOldGeometry = oldData->mGeometry.get();
    }
    if (aOldClip) {
      *aOldClip = &oldData->mClip;
    }
    return oldData->mLayer;
  }

  return nullptr;
}

/* static */ DisplayItemData*
FrameLayerBuilder::GetOldDataFor(nsDisplayItem* aItem)
{
  const SmallPointerArray<DisplayItemData>& array = aItem->Frame()->DisplayItemData();

  for (uint32_t i = 0; i < array.Length(); i++) {
    DisplayItemData *data = DisplayItemData::AssertDisplayItemData(array.ElementAt(i));

    if (data->mDisplayItemKey == aItem->GetPerFrameKey()) {
      return data;
    }
  }
  return nullptr;
}

// Reset state that should not persist when a layer is recycled.
static void
ResetLayerStateForRecycling(Layer* aLayer) {
  // Currently, this clears the mask layer and ancestor mask layers.
  // Other cleanup may be added here.
  aLayer->SetMaskLayer(nullptr);
  aLayer->SetAncestorMaskLayers({});
}

already_AddRefed<ColorLayer>
ContainerState::CreateOrRecycleColorLayer(PaintedLayer *aPainted)
{
  PaintedDisplayItemLayerUserData* data =
      static_cast<PaintedDisplayItemLayerUserData*>(aPainted->GetUserData(&gPaintedDisplayItemLayerUserData));
  RefPtr<ColorLayer> layer = data->mColorLayer;
  if (layer) {
    ResetLayerStateForRecycling(layer);
    layer->ClearExtraDumpInfo();
  } else {
    // Create a new layer
    layer = mManager->CreateColorLayer();
    if (!layer)
      return nullptr;
    // Mark this layer as being used for painting display items
    data->mColorLayer = layer;
    layer->SetUserData(&gColorLayerUserData, nullptr);

    // Remove other layer types we might have stored for this PaintedLayer
    data->mImageLayer = nullptr;
  }
  return layer.forget();
}

already_AddRefed<ImageLayer>
ContainerState::CreateOrRecycleImageLayer(PaintedLayer *aPainted)
{
  PaintedDisplayItemLayerUserData* data =
      static_cast<PaintedDisplayItemLayerUserData*>(aPainted->GetUserData(&gPaintedDisplayItemLayerUserData));
  RefPtr<ImageLayer> layer = data->mImageLayer;
  if (layer) {
    ResetLayerStateForRecycling(layer);
    layer->ClearExtraDumpInfo();
  } else {
    // Create a new layer
    layer = mManager->CreateImageLayer();
    if (!layer)
      return nullptr;
    // Mark this layer as being used for painting display items
    data->mImageLayer = layer;
    layer->SetUserData(&gImageLayerUserData, nullptr);

    // Remove other layer types we might have stored for this PaintedLayer
    data->mColorLayer = nullptr;
  }
  return layer.forget();
}

template<typename UserData>
already_AddRefed<ImageLayer>
ContainerState::CreateOrRecycleMaskImageLayerFor(
    const MaskLayerKey& aKey,
    UserData* (*aGetUserData)(Layer* aLayer),
    void (*aSetDefaultUserData)(Layer* aLayer))
{
  RefPtr<ImageLayer> result = mRecycledMaskImageLayers.Get(aKey);

  if (result && aGetUserData(result.get())) {
    mRecycledMaskImageLayers.Remove(aKey);
    aKey.mLayer->ClearExtraDumpInfo();
    // XXX if we use clip on mask layers, null it out here
  } else {
    // Create a new layer
    result = mManager->CreateImageLayer();
    if (!result) {
      return nullptr;
    }
    aSetDefaultUserData(result);
  }

  return result.forget();
}

static const double SUBPIXEL_OFFSET_EPSILON = 0.02;

/**
 * This normally computes NSToIntRoundUp(aValue). However, if that would
 * give a residual near 0.5 while aOldResidual is near -0.5, or
 * it would give a residual near -0.5 while aOldResidual is near 0.5, then
 * instead we return the integer in the other direction so that the residual
 * is close to aOldResidual.
 */
static int32_t
RoundToMatchResidual(double aValue, double aOldResidual)
{
  int32_t v = NSToIntRoundUp(aValue);
  double residual = aValue - v;
  if (aOldResidual < 0) {
    if (residual > 0 && fabs(residual - 1.0 - aOldResidual) < SUBPIXEL_OFFSET_EPSILON) {
      // Round up instead
      return int32_t(ceil(aValue));
    }
  } else if (aOldResidual > 0) {
    if (residual < 0 && fabs(residual + 1.0 - aOldResidual) < SUBPIXEL_OFFSET_EPSILON) {
      // Round down instead
      return int32_t(floor(aValue));
    }
  }
  return v;
}

static void
ResetScrollPositionForLayerPixelAlignment(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  nsIScrollableFrame* sf = nsLayoutUtils::GetScrollableFrameFor(*aAnimatedGeometryRoot);
  if (sf) {
    sf->ResetScrollPositionForLayerPixelAlignment();
  }
}

static void
InvalidateEntirePaintedLayer(PaintedLayer* aLayer, AnimatedGeometryRoot* aAnimatedGeometryRoot, const char *aReason)
{
#ifdef MOZ_DUMP_PAINTING
  if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
    printf_stderr("Invalidating entire layer %p: %s\n", aLayer, aReason);
  }
#endif
  aLayer->InvalidateWholeLayer();
  aLayer->SetInvalidRectToVisibleRegion();
  ResetScrollPositionForLayerPixelAlignment(aAnimatedGeometryRoot);
}

LayerManager::PaintedLayerCreationHint
ContainerState::GetLayerCreationHint(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  // Check whether the layer will be scrollable. This is used as a hint to
  // influence whether tiled layers are used or not.

  // Check creation hint inherited from our parent.
  if (mParameters.mLayerCreationHint == LayerManager::SCROLLABLE) {
    return LayerManager::SCROLLABLE;
  }

  // Check whether there's any active scroll frame on the animated geometry
  // root chain.
  for (AnimatedGeometryRoot* agr = aAnimatedGeometryRoot;
       agr && agr != mContainerAnimatedGeometryRoot;
       agr = agr->mParentAGR) {
    nsIFrame* fParent = nsLayoutUtils::GetCrossDocParentFrame(*agr);
    if (!fParent) {
      break;
    }
    nsIScrollableFrame* scrollable = do_QueryFrame(fParent);
    if (scrollable) {
      return LayerManager::SCROLLABLE;
    }
  }
  return LayerManager::NONE;
}

already_AddRefed<PaintedLayer>
ContainerState::AttemptToRecyclePaintedLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                             nsDisplayItem* aItem,
                                             const nsPoint& aTopLeft,
                                             const nsIFrame* aReferenceFrame)
{
  Layer* oldLayer = mLayerBuilder->GetOldLayerFor(aItem);
  if (!oldLayer || !oldLayer->AsPaintedLayer()) {
    return nullptr;
  }

  if (!mPaintedLayersAvailableForRecycling.EnsureRemoved(oldLayer->AsPaintedLayer())) {
    // Not found.
    return nullptr;
  }

  // Try to recycle the layer.
  RefPtr<PaintedLayer> layer = oldLayer->AsPaintedLayer();

  // Check if the layer hint has changed and whether or not the layer should
  // be recreated because of it.
  if (!layer->IsOptimizedFor(GetLayerCreationHint(aAnimatedGeometryRoot))) {
    return nullptr;
  }

  bool didResetScrollPositionForLayerPixelAlignment = false;
  PaintedDisplayItemLayerUserData* data =
    RecyclePaintedLayer(layer, aAnimatedGeometryRoot,
                        didResetScrollPositionForLayerPixelAlignment);
  PreparePaintedLayerForUse(layer, data, aAnimatedGeometryRoot,
                            aReferenceFrame, aTopLeft,
                            didResetScrollPositionForLayerPixelAlignment);

  return layer.forget();
}

void ReleaseLayerUserData(void* aData)
{
  PaintedDisplayItemLayerUserData* userData =
    static_cast<PaintedDisplayItemLayerUserData*>(aData);
  userData->Release();
}

already_AddRefed<PaintedLayer>
ContainerState::CreatePaintedLayer(PaintedLayerData* aData)
{
  LayerManager::PaintedLayerCreationHint creationHint =
    GetLayerCreationHint(aData->mAnimatedGeometryRoot);

  // Create a new painted layer
  RefPtr<PaintedLayer> layer = mManager->CreatePaintedLayerWithHint(creationHint);
  if (!layer) {
    return nullptr;
  }

  // Mark this layer as being used for painting display items
  RefPtr<PaintedDisplayItemLayerUserData> userData = new PaintedDisplayItemLayerUserData();
  userData->mDisabledAlpha =
    mParameters.mDisableSubpixelAntialiasingInDescendants;
  userData.get()->AddRef();
  layer->SetUserData(&gPaintedDisplayItemLayerUserData, userData, ReleaseLayerUserData);
  ResetScrollPositionForLayerPixelAlignment(aData->mAnimatedGeometryRoot);

  PreparePaintedLayerForUse(layer, userData, aData->mAnimatedGeometryRoot,
                            aData->mReferenceFrame,
                            aData->mAnimatedGeometryRootOffset, true);

  return layer.forget();
}

PaintedDisplayItemLayerUserData*
ContainerState::RecyclePaintedLayer(PaintedLayer* aLayer,
                                    AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                    bool& didResetScrollPositionForLayerPixelAlignment)
{
  // Clear clip rect and mask layer so we don't accidentally stay clipped.
  // We will reapply any necessary clipping.
  ResetLayerStateForRecycling(aLayer);
  aLayer->ClearExtraDumpInfo();

  PaintedDisplayItemLayerUserData* data =
    static_cast<PaintedDisplayItemLayerUserData*>(
      aLayer->GetUserData(&gPaintedDisplayItemLayerUserData));
  NS_ASSERTION(data, "Recycled PaintedLayers must have user data");

  // This gets called on recycled PaintedLayers that are going to be in the
  // final layer tree, so it's a convenient time to invalidate the
  // content that changed where we don't know what PaintedLayer it belonged
  // to, or if we need to invalidate the entire layer, we can do that.
  // This needs to be done before we update the PaintedLayer to its new
  // transform. See nsGfxScrollFrame::InvalidateInternal, where
  // we ensure that mInvalidPaintedContent is updated according to the
  // scroll position as of the most recent paint.
  if (!FuzzyEqual(data->mXScale, mParameters.mXScale, 0.00001f) ||
      !FuzzyEqual(data->mYScale, mParameters.mYScale, 0.00001f) ||
      data->mAppUnitsPerDevPixel != mAppUnitsPerDevPixel) {
#ifdef MOZ_DUMP_PAINTING
  if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
    printf_stderr("Recycled layer %p changed scale\n", aLayer);
  }
#endif
    InvalidateEntirePaintedLayer(aLayer, aAnimatedGeometryRoot, "recycled layer changed state");
    didResetScrollPositionForLayerPixelAlignment = true;
  }
  if (!data->mRegionToInvalidate.IsEmpty()) {
#ifdef MOZ_DUMP_PAINTING
    if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
      printf_stderr("Invalidating deleted frame content from layer %p\n", aLayer);
    }
#endif
    aLayer->InvalidateRegion(data->mRegionToInvalidate);
#ifdef MOZ_DUMP_PAINTING
    if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
      nsAutoCString str;
      AppendToString(str, data->mRegionToInvalidate);
      printf_stderr("Invalidating layer %p: %s\n", aLayer, str.get());
    }
#endif
    data->mRegionToInvalidate.SetEmpty();
  }
  return data;
}

void
ContainerState::PreparePaintedLayerForUse(PaintedLayer* aLayer,
                                          PaintedDisplayItemLayerUserData* aData,
                                          AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                          const nsIFrame* aReferenceFrame,
                                          const nsPoint& aTopLeft,
                                          bool didResetScrollPositionForLayerPixelAlignment)
{
  aData->mXScale = mParameters.mXScale;
  aData->mYScale = mParameters.mYScale;
  aData->mLastAnimatedGeometryRootOrigin = aData->mAnimatedGeometryRootOrigin;
  aData->mAnimatedGeometryRootOrigin = aTopLeft;
  aData->mAppUnitsPerDevPixel = mAppUnitsPerDevPixel;
  aLayer->SetAllowResidualTranslation(mParameters.AllowResidualTranslation());

  aData->mLastPaintOffset = GetTranslationForPaintedLayer(aLayer);
  aData->mHasExplicitLastPaintOffset = true;

  // Set up transform so that 0,0 in the PaintedLayer corresponds to the
  // (pixel-snapped) top-left of the aAnimatedGeometryRoot.
  nsPoint offset = (*aAnimatedGeometryRoot)->GetOffsetToCrossDoc(aReferenceFrame);
  nscoord appUnitsPerDevPixel = (*aAnimatedGeometryRoot)->PresContext()->AppUnitsPerDevPixel();
  gfxPoint scaledOffset(
      NSAppUnitsToDoublePixels(offset.x, appUnitsPerDevPixel)*mParameters.mXScale,
      NSAppUnitsToDoublePixels(offset.y, appUnitsPerDevPixel)*mParameters.mYScale);
  // We call RoundToMatchResidual here so that the residual after rounding
  // is close to aData->mAnimatedGeometryRootPosition if possible.
  nsIntPoint pixOffset(RoundToMatchResidual(scaledOffset.x, aData->mAnimatedGeometryRootPosition.x),
                       RoundToMatchResidual(scaledOffset.y, aData->mAnimatedGeometryRootPosition.y));
  aData->mTranslation = pixOffset;
  pixOffset += mParameters.mOffset;
  Matrix matrix = Matrix::Translation(pixOffset.x, pixOffset.y);
  aLayer->SetBaseTransform(Matrix4x4::From2D(matrix));

  aData->mVisibilityComputedRegion.SetEmpty();

  // Calculate exact position of the top-left of the active scrolled root.
  // This might not be 0,0 due to the snapping in ScaleToNearestPixels.
  gfxPoint animatedGeometryRootTopLeft = scaledOffset - ThebesPoint(matrix.GetTranslation()) + mParameters.mOffset;
  const bool disableAlpha =
    mParameters.mDisableSubpixelAntialiasingInDescendants;
  if (aData->mDisabledAlpha != disableAlpha) {
    aData->mAnimatedGeometryRootPosition = animatedGeometryRootTopLeft;
    InvalidateEntirePaintedLayer(aLayer, aAnimatedGeometryRoot, "change of subpixel-AA");
    aData->mDisabledAlpha = disableAlpha;
    return;
  }

  // FIXME: Temporary workaround for bug 681192 and bug 724786.
#ifndef MOZ_WIDGET_ANDROID
  // If it has changed, then we need to invalidate the entire layer since the
  // pixels in the layer buffer have the content at a (subpixel) offset
  // from what we need.
  if (!animatedGeometryRootTopLeft.WithinEpsilonOf(aData->mAnimatedGeometryRootPosition, SUBPIXEL_OFFSET_EPSILON)) {
    aData->mAnimatedGeometryRootPosition = animatedGeometryRootTopLeft;
    InvalidateEntirePaintedLayer(aLayer, aAnimatedGeometryRoot, "subpixel offset");
  } else if (didResetScrollPositionForLayerPixelAlignment) {
    aData->mAnimatedGeometryRootPosition = animatedGeometryRootTopLeft;
  }
#else
  Unused << didResetScrollPositionForLayerPixelAlignment;
#endif
}

#if defined(DEBUG) || defined(MOZ_DUMP_PAINTING)
/**
 * Returns the appunits per dev pixel for the item's frame
 */
static int32_t
AppUnitsPerDevPixel(nsDisplayItem* aItem)
{
  // The underlying frame for zoom items is the root frame of the subdocument.
  // But zoom display items report their bounds etc using the parent document's
  // APD because zoom items act as a conversion layer between the two different
  // APDs.
  if (aItem->GetType() == DisplayItemType::TYPE_ZOOM) {
    return static_cast<nsDisplayZoom*>(aItem)->GetParentAppUnitsPerDevPixel();
  }
  return aItem->Frame()->PresContext()->AppUnitsPerDevPixel();
}
#endif

/**
 * Set the visible region for aLayer.
 * aOuterVisibleRegion is the visible region relative to the parent layer.
 * aLayerContentsVisibleRect, if non-null, is a rectangle in the layer's
 * own coordinate system to which the layer's visible region is restricted.
 * Consumes *aOuterVisibleRegion.
 */
static void
SetOuterVisibleRegion(Layer* aLayer, nsIntRegion* aOuterVisibleRegion,
                      const nsIntRect* aLayerContentsVisibleRect = nullptr,
                      bool aOuterUntransformed = false)
{
  Matrix4x4 transform = aLayer->GetTransform();
  Matrix transform2D;
  if (aOuterUntransformed) {
    if (aLayerContentsVisibleRect) {
      aOuterVisibleRegion->And(*aOuterVisibleRegion,
                               *aLayerContentsVisibleRect);
    }
  } else if (transform.Is2D(&transform2D) && !transform2D.HasNonIntegerTranslation()) {
    aOuterVisibleRegion->MoveBy(-int(transform2D._31), -int(transform2D._32));
    if (aLayerContentsVisibleRect) {
      aOuterVisibleRegion->And(*aOuterVisibleRegion, *aLayerContentsVisibleRect);
    }
  } else {
    nsIntRect outerRect = aOuterVisibleRegion->GetBounds();
    // if 'transform' is not invertible, then nothing will be displayed
    // for the layer, so it doesn't really matter what we do here
    Rect outerVisible(outerRect.x, outerRect.y, outerRect.width, outerRect.height);
    transform.Invert();

    Rect layerContentsVisible(-float(INT32_MAX) / 2, -float(INT32_MAX) / 2,
                              float(INT32_MAX), float(INT32_MAX));
    if (aLayerContentsVisibleRect) {
      NS_ASSERTION(aLayerContentsVisibleRect->width >= 0 &&
                   aLayerContentsVisibleRect->height >= 0,
                   "Bad layer contents rectangle");
      // restrict to aLayerContentsVisibleRect before call GfxRectToIntRect,
      // in case layerVisible is extremely large (as it can be when
      // projecting through the inverse of a 3D transform)
      layerContentsVisible = Rect(
          aLayerContentsVisibleRect->x, aLayerContentsVisibleRect->y,
          aLayerContentsVisibleRect->width, aLayerContentsVisibleRect->height);
    }
    gfxRect layerVisible = ThebesRect(transform.ProjectRectBounds(outerVisible, layerContentsVisible));
    layerVisible.RoundOut();
    nsIntRect visRect;
    if (gfxUtils::GfxRectToIntRect(layerVisible, &visRect)) {
      *aOuterVisibleRegion = visRect;
    } else  {
      aOuterVisibleRegion->SetEmpty();
    }
  }

  aLayer->SetVisibleRegion(LayerIntRegion::FromUnknownRegion(*aOuterVisibleRegion));
}

void
ContainerState::SetOuterVisibleRegionForLayer(Layer* aLayer,
                                              const nsIntRegion& aOuterVisibleRegion,
                                              const nsIntRect* aLayerContentsVisibleRect,
                                              bool aOuterUntransformed) const
{
  nsIntRegion visRegion = aOuterVisibleRegion;
  if (!aOuterUntransformed) {
    visRegion.MoveBy(mParameters.mOffset);
  }
  SetOuterVisibleRegion(aLayer, &visRegion, aLayerContentsVisibleRect,
                        aOuterUntransformed);
}

nscolor
ContainerState::FindOpaqueBackgroundColorInLayer(const PaintedLayerData* aData,
                                                 const nsIntRect& aRect,
                                                 bool* aOutIntersectsLayer) const
{
  *aOutIntersectsLayer = true;

  // Scan the candidate's display items.
  nsIntRect deviceRect = aRect;
  nsRect appUnitRect = ToAppUnits(deviceRect, mAppUnitsPerDevPixel);
  appUnitRect.ScaleInverseRoundOut(mParameters.mXScale, mParameters.mYScale);

  for (auto& assignedItem : Reversed(aData->mAssignedDisplayItems)) {
    if (assignedItem.mType != DisplayItemEntryType::ITEM) {
      // |assignedItem| is an effect marker.
      continue;
    }

    nsDisplayItem* item = assignedItem.mItem;
    bool snap;
    nsRect bounds = item->GetBounds(mBuilder, &snap);
    if (snap && mSnappingEnabled) {
      nsIntRect snappedBounds = ScaleToNearestPixels(bounds);
      if (!snappedBounds.Intersects(deviceRect))
        continue;

      if (!snappedBounds.Contains(deviceRect))
        return NS_RGBA(0,0,0,0);

    } else {
      // The layer's visible rect is already (close enough to) pixel
      // aligned, so no need to round out and in here.
      if (!bounds.Intersects(appUnitRect))
        continue;

      if (!bounds.Contains(appUnitRect))
        return NS_RGBA(0,0,0,0);
    }

    if (item->IsInvisibleInRect(appUnitRect)) {
      continue;
    }

    if (item->GetClip().IsRectAffectedByClip(deviceRect,
                                             mParameters.mXScale,
                                             mParameters.mYScale,
                                             mAppUnitsPerDevPixel)) {
      return NS_RGBA(0,0,0,0);
    }

    if (!assignedItem.mHasOpacity) {
      Maybe<nscolor> color = item->IsUniform(mBuilder);

      if (color && NS_GET_A(*color) == 255) {
        return *color;
      }
    }

    return NS_RGBA(0,0,0,0);
  }

  *aOutIntersectsLayer = false;
  return NS_RGBA(0,0,0,0);
}

nscolor
PaintedLayerDataNode::FindOpaqueBackgroundColor(const nsIntRegion& aTargetVisibleRegion,
                                                int32_t aUnderIndex) const
{
  if (aUnderIndex == ABOVE_TOP) {
    aUnderIndex = mPaintedLayerDataStack.Length();
  }
  for (int32_t i = aUnderIndex - 1; i >= 0; --i) {
    const PaintedLayerData* candidate = &mPaintedLayerDataStack[i];
    if (candidate->VisibleAboveRegionIntersects(aTargetVisibleRegion)) {
      // Some non-PaintedLayer content between target and candidate; this is
      // hopeless
      return NS_RGBA(0,0,0,0);
    }

    if (!candidate->VisibleRegionIntersects(aTargetVisibleRegion)) {
      // The layer doesn't intersect our target, ignore it and move on
      continue;
    }

    bool intersectsLayer = true;
    nsIntRect rect = aTargetVisibleRegion.GetBounds();
    nscolor color = mTree.ContState().FindOpaqueBackgroundColorInLayer(
                                        candidate, rect, &intersectsLayer);
    if (!intersectsLayer) {
      continue;
    }
    return color;
  }
  if (mAllDrawingAboveBackground ||
      !mVisibleAboveBackgroundRegion.Intersect(aTargetVisibleRegion).IsEmpty()) {
    // Some non-PaintedLayer content is between this node's background and target.
    return NS_RGBA(0,0,0,0);
  }
  return FindOpaqueBackgroundColorInParentNode();
}

nscolor
PaintedLayerDataNode::FindOpaqueBackgroundColorCoveringEverything() const
{
  if (!mPaintedLayerDataStack.IsEmpty() ||
      mAllDrawingAboveBackground ||
      !mVisibleAboveBackgroundRegion.IsEmpty()) {
    return NS_RGBA(0,0,0,0);
  }
  return FindOpaqueBackgroundColorInParentNode();
}

nscolor
PaintedLayerDataNode::FindOpaqueBackgroundColorInParentNode() const
{
  if (mParent) {
    if (mHasClip) {
      // Check whether our parent node has uniform content behind our whole
      // clip.
      // There's one tricky case here: If our parent node is also a scrollable,
      // and is currently scrolled in such a way that this inner one is
      // clipped by it, then it's not really clear how we should determine
      // whether we have a uniform background in the parent: There might be
      // non-uniform content in the parts that our scroll port covers in the
      // parent and that are currently outside the parent's clip.
      // For now, we'll fail to pull a background color in that case.
      return mParent->FindOpaqueBackgroundColor(mClipRect);
    }
    return mParent->FindOpaqueBackgroundColorCoveringEverything();
  }
  // We are the root.
  return mTree.UniformBackgroundColor();
}

bool
PaintedLayerData::CanOptimizeToImageLayer(nsDisplayListBuilder* aBuilder)
{
  if (!mImage) {
    return false;
  }

  return mImage->CanOptimizeToImageLayer(mLayer->Manager(), aBuilder);
}

already_AddRefed<ImageContainer>
PaintedLayerData::GetContainerForImageLayer(nsDisplayListBuilder* aBuilder)
{
  if (!mImage) {
    return nullptr;
  }

  return mImage->GetContainer(mLayer->Manager(), aBuilder);
}

PaintedLayerDataNode::PaintedLayerDataNode(PaintedLayerDataTree& aTree,
                                           PaintedLayerDataNode* aParent,
                                           AnimatedGeometryRoot* aAnimatedGeometryRoot)
  : mTree(aTree)
  , mParent(aParent)
  , mAnimatedGeometryRoot(aAnimatedGeometryRoot)
  , mAllDrawingAboveBackground(false)
{
  MOZ_ASSERT(nsLayoutUtils::IsAncestorFrameCrossDoc(mTree.Builder()->RootReferenceFrame(), *mAnimatedGeometryRoot));
  mHasClip = mTree.IsClippedWithRespectToParentAnimatedGeometryRoot(mAnimatedGeometryRoot, &mClipRect);
}

PaintedLayerDataNode::~PaintedLayerDataNode()
{
  MOZ_ASSERT(mPaintedLayerDataStack.IsEmpty());
  MOZ_ASSERT(mChildren.IsEmpty());
}

PaintedLayerDataNode*
PaintedLayerDataNode::AddChildNodeFor(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  MOZ_ASSERT(aAnimatedGeometryRoot->mParentAGR == mAnimatedGeometryRoot);
  UniquePtr<PaintedLayerDataNode> child =
    MakeUnique<PaintedLayerDataNode>(mTree, this, aAnimatedGeometryRoot);
  mChildren.AppendElement(Move(child));
  return mChildren.LastElement().get();
}

template<typename NewPaintedLayerCallbackType>
PaintedLayerData*
PaintedLayerDataNode::FindPaintedLayerFor(const nsIntRect& aVisibleRect,
                                          const bool aBackfaceHidden,
                                          const ActiveScrolledRoot* aASR,
                                          const DisplayItemClipChain* aClipChain,
                                          NewPaintedLayerCallbackType aNewPaintedLayerCallback)
{
  if (!mPaintedLayerDataStack.IsEmpty()) {
    PaintedLayerData* lowestUsableLayer = nullptr;
    for (auto& data : Reversed(mPaintedLayerDataStack)) {
      if (data.mVisibleAboveRegion.Intersects(aVisibleRect)) {
        break;
      }
      if (data.mBackfaceHidden == aBackfaceHidden &&
          data.mASR == aASR &&
          data.mClipChain == aClipChain) {
        lowestUsableLayer = &data;
      }
      // Also check whether the event-regions intersect the visible rect,
      // unless we're in an inactive layer, in which case the event-regions
      // will be hoisted out into their own layer.
      // For performance reasons, we check the intersection with the bounds
      // of the event-regions.
      if (!mTree.ContState().IsInInactiveLayer() &&
          (data.mScaledHitRegionBounds.Intersects(aVisibleRect) ||
           data.mScaledMaybeHitRegionBounds.Intersects(aVisibleRect))) {
        break;
      }
      // If the visible region intersects with the current layer then we
      // can't possibly use any of the layers below it, so stop the search
      // now.
      //
      // If we're trying to minimize painted layer size and we don't
      // intersect the current visible region, then make sure we don't
      // use this painted layer.
      if (data.mVisibleRegion.Intersects(aVisibleRect)) {
        break;
      } else if (gfxPrefs::LayoutSmallerPaintedLayers()) {
        lowestUsableLayer = nullptr;
      }
    }
    if (lowestUsableLayer) {
      return lowestUsableLayer;
    }
  }
  PaintedLayerData* data = mPaintedLayerDataStack.AppendElement();
  aNewPaintedLayerCallback(data);

  return data;
}

void
PaintedLayerDataNode::FinishChildrenIntersecting(const nsIntRect& aRect)
{
  for (int32_t i = mChildren.Length() - 1; i >= 0; i--) {
    if (mChildren[i]->Intersects(aRect)) {
      mChildren[i]->Finish(true);
      mChildren.RemoveElementAt(i);
    }
  }
}

void
PaintedLayerDataNode::FinishAllChildren(bool aThisNodeNeedsAccurateVisibleAboveRegion)
{
  for (int32_t i = mChildren.Length() - 1; i >= 0; i--) {
    mChildren[i]->Finish(aThisNodeNeedsAccurateVisibleAboveRegion);
  }
  mChildren.Clear();
}

void
PaintedLayerDataNode::Finish(bool aParentNeedsAccurateVisibleAboveRegion)
{
  // Skip "visible above region" maintenance, because this node is going away.
  FinishAllChildren(false);

  PopAllPaintedLayerData();

  if (mParent && aParentNeedsAccurateVisibleAboveRegion) {
    if (mHasClip) {
      mParent->AddToVisibleAboveRegion(mClipRect);
    } else {
      mParent->SetAllDrawingAbove();
    }
  }
  mTree.NodeWasFinished(mAnimatedGeometryRoot);
}

void
PaintedLayerDataNode::AddToVisibleAboveRegion(const nsIntRect& aRect)
{
  nsIntRegion& visibleAboveRegion = mPaintedLayerDataStack.IsEmpty()
    ? mVisibleAboveBackgroundRegion
    : mPaintedLayerDataStack.LastElement().mVisibleAboveRegion;
  visibleAboveRegion.Or(visibleAboveRegion, aRect);
  visibleAboveRegion.SimplifyOutward(8);
}

void
PaintedLayerDataNode::SetAllDrawingAbove()
{
  PopAllPaintedLayerData();
  mAllDrawingAboveBackground = true;
  mVisibleAboveBackgroundRegion.SetEmpty();
}

void
PaintedLayerDataNode::PopAllPaintedLayerData()
{
  for (int32_t index = mPaintedLayerDataStack.Length() - 1; index >= 0; index--) {
    PaintedLayerData& data = mPaintedLayerDataStack[index];
    mTree.ContState().FinishPaintedLayerData(data, [this, &data, index]() {
      return this->FindOpaqueBackgroundColor(data.mVisibleRegion, index);
    });
  }
  mPaintedLayerDataStack.Clear();
}

void
PaintedLayerDataTree::InitializeForInactiveLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  mForInactiveLayer = true;
  mRoot.emplace(*this, nullptr, aAnimatedGeometryRoot);

}

nsDisplayListBuilder*
PaintedLayerDataTree::Builder() const
{
  return mContainerState.Builder();
}

void
PaintedLayerDataTree::Finish()
{
  if (mRoot) {
    mRoot->Finish(false);
  }
  MOZ_ASSERT(mNodes.Count() == 0);
  mRoot.reset();
}

void
PaintedLayerDataTree::NodeWasFinished(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  mNodes.Remove(aAnimatedGeometryRoot);
}

void
PaintedLayerDataTree::AddingOwnLayer(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                     const nsIntRect* aRect,
                                     nscolor* aOutUniformBackgroundColor)
{
  PaintedLayerDataNode* node = nullptr;
  if (mForInactiveLayer) {
    node = mRoot.ptr();
  } else {
    FinishPotentiallyIntersectingNodes(aAnimatedGeometryRoot, aRect);
    node = EnsureNodeFor(aAnimatedGeometryRoot);
  }
  if (aRect) {
    if (aOutUniformBackgroundColor) {
      *aOutUniformBackgroundColor = node->FindOpaqueBackgroundColor(*aRect);
    }
    node->AddToVisibleAboveRegion(*aRect);
  } else {
    if (aOutUniformBackgroundColor) {
      *aOutUniformBackgroundColor = node->FindOpaqueBackgroundColorCoveringEverything();
    }
    node->SetAllDrawingAbove();
  }
}

template<typename NewPaintedLayerCallbackType>
PaintedLayerData*
PaintedLayerDataTree::FindPaintedLayerFor(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                          const ActiveScrolledRoot* aASR,
                                          const DisplayItemClipChain* aClipChain,
                                          const nsIntRect& aVisibleRect,
                                          const bool aBackfaceHidden,
                                          NewPaintedLayerCallbackType aNewPaintedLayerCallback)
{
  const nsIntRect* bounds = &aVisibleRect;
  PaintedLayerDataNode* node = nullptr;
  if (mForInactiveLayer) {
    node = mRoot.ptr();
  } else {
    FinishPotentiallyIntersectingNodes(aAnimatedGeometryRoot, bounds);
    node = EnsureNodeFor(aAnimatedGeometryRoot);
  }

  PaintedLayerData* data =
    node->FindPaintedLayerFor(aVisibleRect, aBackfaceHidden, aASR, aClipChain,
                              aNewPaintedLayerCallback);
  return data;
}

void
PaintedLayerDataTree::FinishPotentiallyIntersectingNodes(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                         const nsIntRect* aRect)
{
  AnimatedGeometryRoot* ancestorThatIsChildOfCommonAncestor = nullptr;
  PaintedLayerDataNode* ancestorNode =
    FindNodeForAncestorAnimatedGeometryRoot(aAnimatedGeometryRoot,
                                            &ancestorThatIsChildOfCommonAncestor);
  if (!ancestorNode) {
    // None of our ancestors are in the tree. This should only happen if this
    // is the very first item we're looking at.
    MOZ_ASSERT(!mRoot);
    return;
  }

  if (ancestorNode->GetAnimatedGeometryRoot() == aAnimatedGeometryRoot) {
    // aAnimatedGeometryRoot already has a node in the tree.
    // This is the common case.
    MOZ_ASSERT(!ancestorThatIsChildOfCommonAncestor);
    if (aRect) {
      ancestorNode->FinishChildrenIntersecting(*aRect);
    } else {
      ancestorNode->FinishAllChildren();
    }
    return;
  }

  // We have found an existing ancestor, but it's a proper ancestor of our
  // animated geometry root.
  // ancestorThatIsChildOfCommonAncestor is the last animated geometry root
  // encountered on the way up from aAnimatedGeometryRoot to ancestorNode.
  MOZ_ASSERT(ancestorThatIsChildOfCommonAncestor);
  MOZ_ASSERT(nsLayoutUtils::IsAncestorFrameCrossDoc(*ancestorThatIsChildOfCommonAncestor, *aAnimatedGeometryRoot));
  MOZ_ASSERT(ancestorThatIsChildOfCommonAncestor->mParentAGR == ancestorNode->GetAnimatedGeometryRoot());

  // ancestorThatIsChildOfCommonAncestor is not in the tree yet!
  MOZ_ASSERT(!mNodes.Get(ancestorThatIsChildOfCommonAncestor));

  // We're about to add a node for ancestorThatIsChildOfCommonAncestor, so we
  // finish all intersecting siblings.
  nsIntRect clip;
  if (IsClippedWithRespectToParentAnimatedGeometryRoot(ancestorThatIsChildOfCommonAncestor, &clip)) {
    ancestorNode->FinishChildrenIntersecting(clip);
  } else {
    ancestorNode->FinishAllChildren();
  }
}

PaintedLayerDataNode*
PaintedLayerDataTree::EnsureNodeFor(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  MOZ_ASSERT(aAnimatedGeometryRoot);
  PaintedLayerDataNode* node = mNodes.Get(aAnimatedGeometryRoot);
  if (node) {
    return node;
  }

  AnimatedGeometryRoot* parentAnimatedGeometryRoot = aAnimatedGeometryRoot->mParentAGR;
  if (!parentAnimatedGeometryRoot) {
    MOZ_ASSERT(!mRoot);
    MOZ_ASSERT(*aAnimatedGeometryRoot == Builder()->RootReferenceFrame());
    mRoot.emplace(*this, nullptr, aAnimatedGeometryRoot);
    node = mRoot.ptr();
  } else {
    PaintedLayerDataNode* parentNode = EnsureNodeFor(parentAnimatedGeometryRoot);
    MOZ_ASSERT(parentNode);
    node = parentNode->AddChildNodeFor(aAnimatedGeometryRoot);
  }
  MOZ_ASSERT(node);
  mNodes.Put(aAnimatedGeometryRoot, node);
  return node;
}

bool
PaintedLayerDataTree::IsClippedWithRespectToParentAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                                       nsIntRect* aOutClip)
{
  if (mForInactiveLayer) {
    return false;
  }
  nsIScrollableFrame* scrollableFrame = nsLayoutUtils::GetScrollableFrameFor(*aAnimatedGeometryRoot);
  if (!scrollableFrame) {
    return false;
  }
  nsIFrame* scrollFrame = do_QueryFrame(scrollableFrame);
  nsRect scrollPort = scrollableFrame->GetScrollPortRect() + Builder()->ToReferenceFrame(scrollFrame);
  *aOutClip = mContainerState.ScaleToNearestPixels(scrollPort);
  return true;
}

PaintedLayerDataNode*
PaintedLayerDataTree::FindNodeForAncestorAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                                              AnimatedGeometryRoot** aOutAncestorChild)
{
  if (!aAnimatedGeometryRoot) {
    return nullptr;
  }
  PaintedLayerDataNode* node = mNodes.Get(aAnimatedGeometryRoot);
  if (node) {
    return node;
  }
  *aOutAncestorChild = aAnimatedGeometryRoot;
  return FindNodeForAncestorAnimatedGeometryRoot(aAnimatedGeometryRoot->mParentAGR, aOutAncestorChild);
}

static bool
CanOptimizeAwayPaintedLayer(PaintedLayerData* aData,
                           FrameLayerBuilder* aLayerBuilder)
{
  if (!aLayerBuilder->IsBuildingRetainedLayers()) {
    return false;
  }

  // If there's no painted layer with valid content in it that we can reuse,
  // always create a color or image layer (and potentially throw away an
  // existing completely invalid painted layer).
  if (aData->mLayer->GetValidRegion().IsEmpty()) {
    return true;
  }

  // There is an existing painted layer we can reuse. Throwing it away can make
  // compositing cheaper (see bug 946952), but it might cause us to re-allocate
  // the painted layer frequently due to an animation. So we only discard it if
  // we're in tree compression mode, which is triggered at a low frequency.
  return aLayerBuilder->CheckInLayerTreeCompressionMode();
}

#ifdef DEBUG
static int32_t FindIndexOfLayerIn(nsTArray<NewLayerEntry>& aArray,
                                  Layer* aLayer)
{
  for (uint32_t i = 0; i < aArray.Length(); ++i) {
    if (aArray[i].mLayer == aLayer) {
      return i;
    }
  }
  return -1;
}
#endif

already_AddRefed<Layer>
ContainerState::PrepareImageLayer(PaintedLayerData* aData)
{
  RefPtr<ImageContainer> imageContainer =
    aData->GetContainerForImageLayer(mBuilder);
  if (!imageContainer) {
    return nullptr;
  }

  RefPtr<ImageLayer> imageLayer = CreateOrRecycleImageLayer(aData->mLayer);
  imageLayer->SetContainer(imageContainer);
  aData->mImage->ConfigureLayer(imageLayer, mParameters);
  imageLayer->SetPostScale(mParameters.mXScale,
                           mParameters.mYScale);

  if (aData->mItemClip->HasClip()) {
    ParentLayerIntRect clip =
      ViewAs<ParentLayerPixel>(ScaleToNearestPixels(aData->mItemClip->GetClipRect()));
    clip.MoveBy(ViewAs<ParentLayerPixel>(mParameters.mOffset));
    imageLayer->SetClipRect(Some(clip));
  } else {
    imageLayer->SetClipRect(Nothing());
  }

  FLB_LOG_PAINTED_LAYER_DECISION(aData,
                                 "  Selected image layer=%p\n", imageLayer.get());

  return imageLayer.forget();
}

already_AddRefed<Layer>
ContainerState::PrepareColorLayer(PaintedLayerData* aData)
{
  RefPtr<ColorLayer> colorLayer = CreateOrRecycleColorLayer(aData->mLayer);
  colorLayer->SetColor(Color::FromABGR(aData->mSolidColor));

  // Copy transform
  colorLayer->SetBaseTransform(aData->mLayer->GetBaseTransform());
  colorLayer->SetPostScale(aData->mLayer->GetPostXScale(),
                           aData->mLayer->GetPostYScale());

  nsIntRect visibleRect = aData->mVisibleRegion.GetBounds();
  visibleRect.MoveBy(-GetTranslationForPaintedLayer(aData->mLayer));
  colorLayer->SetBounds(visibleRect);
  colorLayer->SetClipRect(Nothing());

  FLB_LOG_PAINTED_LAYER_DECISION(aData,
                                 "  Selected color layer=%p\n", colorLayer.get());

  return colorLayer.forget();
}

static void
SetBackfaceHiddenForLayer(bool aBackfaceHidden, Layer* aLayer)
{
  if (aBackfaceHidden) {
    aLayer->SetContentFlags(aLayer->GetContentFlags() |
                            Layer::CONTENT_BACKFACE_HIDDEN);
  } else {
    aLayer->SetContentFlags(aLayer->GetContentFlags() &
                            ~Layer::CONTENT_BACKFACE_HIDDEN);
  }
}

template<typename FindOpaqueBackgroundColorCallbackType>
void ContainerState::FinishPaintedLayerData(PaintedLayerData& aData, FindOpaqueBackgroundColorCallbackType aFindOpaqueBackgroundColor)
{
  PaintedLayerData* data = &aData;

  MOZ_ASSERT(data->mOpacityIndices.IsEmpty());

  if (!data->mLayer) {
    // No layer was recycled, so we create a new one.
    RefPtr<PaintedLayer> paintedLayer = CreatePaintedLayer(data);
    data->mLayer = paintedLayer;

    NS_ASSERTION(FindIndexOfLayerIn(mNewChildLayers, paintedLayer) < 0,
                 "Layer already in list???");
    mNewChildLayers[data->mNewChildLayersIndex].mLayer = paintedLayer.forget();
  }

  PaintedDisplayItemLayerUserData* userData = GetPaintedDisplayItemLayerUserData(data->mLayer);
  NS_ASSERTION(userData, "where did our user data go?");
  userData->mLastItemCount = data->mAssignedDisplayItems.Length();

  NewLayerEntry* newLayerEntry = &mNewChildLayers[data->mNewChildLayersIndex];

  RefPtr<Layer> layer;
  bool canOptimizeToImageLayer = data->CanOptimizeToImageLayer(mBuilder);

  FLB_LOG_PAINTED_LAYER_DECISION(data, "Selecting layer for pld=%p\n", data);
  FLB_LOG_PAINTED_LAYER_DECISION(data, "  Solid=%i, hasImage=%c, canOptimizeAwayPaintedLayer=%i\n",
          data->mIsSolidColorInVisibleRegion, canOptimizeToImageLayer ? 'y' : 'n',
          CanOptimizeAwayPaintedLayer(data, mLayerBuilder));

  if ((data->mIsSolidColorInVisibleRegion || canOptimizeToImageLayer) &&
      CanOptimizeAwayPaintedLayer(data, mLayerBuilder)) {
    NS_ASSERTION(!(data->mIsSolidColorInVisibleRegion && canOptimizeToImageLayer),
                 "Can't be a solid color as well as an image!");

    layer = canOptimizeToImageLayer ? PrepareImageLayer(data)
                                    : PrepareColorLayer(data);

    if (layer) {
      NS_ASSERTION(FindIndexOfLayerIn(mNewChildLayers, layer) < 0,
                   "Layer already in list???");
      NS_ASSERTION(newLayerEntry->mLayer == data->mLayer,
                   "Painted layer at wrong index");
      // Store optimized layer in reserved slot
      NewLayerEntry* paintedLayerEntry = newLayerEntry;
      newLayerEntry = &mNewChildLayers[data->mNewChildLayersIndex + 1];
      NS_ASSERTION(!newLayerEntry->mLayer, "Slot already occupied?");
      newLayerEntry->mLayer = layer;
      newLayerEntry->mAnimatedGeometryRoot = data->mAnimatedGeometryRoot;
      newLayerEntry->mASR = paintedLayerEntry->mASR;
      newLayerEntry->mClipChain = paintedLayerEntry->mClipChain;
      newLayerEntry->mScrollMetadataASR = paintedLayerEntry->mScrollMetadataASR;

      // Hide the PaintedLayer. We leave it in the layer tree so that we
      // can find and recycle it later.
      ParentLayerIntRect emptyRect;
      data->mLayer->SetClipRect(Some(emptyRect));
      data->mLayer->SetVisibleRegion(LayerIntRegion());
      data->mLayer->InvalidateWholeLayer();
      data->mLayer->SetEventRegions(EventRegions());
    }
  }

  if (!layer) {
    // We couldn't optimize to an image layer or a color layer above.
    layer = data->mLayer;
    layer->SetClipRect(Nothing());
    FLB_LOG_PAINTED_LAYER_DECISION(data, "  Selected painted layer=%p\n", layer.get());
  }

  for (auto& item : data->mAssignedDisplayItems) {
    MOZ_ASSERT(item.mItem->GetType() != DisplayItemType::TYPE_LAYER_EVENT_REGIONS);
    MOZ_ASSERT(item.mItem->GetType() != DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO);

    if (item.mType == DisplayItemEntryType::POP_OPACITY) {
      // Do not invalidate for end markers.
      continue;
    }

    InvalidateForLayerChange(item.mItem, data->mLayer, item.mDisplayItemData);
    mLayerBuilder->AddPaintedDisplayItem(data, item, *this, layer);
    item.mDisplayItemData = nullptr;
  }

  if (mLayerBuilder->IsBuildingRetainedLayers()) {
    newLayerEntry->mVisibleRegion = data->mVisibleRegion;
    newLayerEntry->mOpaqueRegion = data->mOpaqueRegion;
    newLayerEntry->mHideAllLayersBelow = data->mHideAllLayersBelow;
    newLayerEntry->mOpaqueForAnimatedGeometryRootParent = data->mOpaqueForAnimatedGeometryRootParent;
  } else {
    SetOuterVisibleRegionForLayer(layer, data->mVisibleRegion);
  }

#ifdef MOZ_DUMP_PAINTING
  if (!data->mLog.IsEmpty()) {
    if (PaintedLayerData* containingPld = mLayerBuilder->GetContainingPaintedLayerData()) {
      containingPld->mLayer->AddExtraDumpInfo(nsCString(data->mLog));
    } else {
      layer->AddExtraDumpInfo(nsCString(data->mLog));
    }
  }
#endif

  mLayerBuilder->AddPaintedLayerItemsEntry(userData);

  nsIntRegion transparentRegion;
  transparentRegion.Sub(data->mVisibleRegion, data->mOpaqueRegion);
  bool isOpaque = transparentRegion.IsEmpty();
  // For translucent PaintedLayers, try to find an opaque background
  // color that covers the entire area beneath it so we can pull that
  // color into this layer to make it opaque.
  if (layer == data->mLayer) {
    nscolor backgroundColor = NS_RGBA(0,0,0,0);
    if (!isOpaque) {
      backgroundColor = aFindOpaqueBackgroundColor();
      if (NS_GET_A(backgroundColor) == 255) {
        isOpaque = true;
      }
    }

    // Store the background color
    if (userData->mForcedBackgroundColor != backgroundColor) {
      // Invalidate the entire target PaintedLayer since we're changing
      // the background color
#ifdef MOZ_DUMP_PAINTING
      if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
        printf_stderr("Forced background color has changed from #%08X to #%08X on layer %p\n",
                      userData->mForcedBackgroundColor, backgroundColor, data->mLayer);
        nsAutoCString str;
        AppendToString(str, data->mLayer->GetValidRegion());
        printf_stderr("Invalidating layer %p: %s\n", data->mLayer, str.get());
      }
#endif
      data->mLayer->InvalidateWholeLayer();
    }
    userData->mForcedBackgroundColor = backgroundColor;
  } else {
    // mask layer for image and color layers
    SetupMaskLayer(layer, *data->mItemClip);
  }

  uint32_t flags = 0;
  nsIWidget* widget = mContainerReferenceFrame->PresContext()->GetRootWidget();
  // See bug 941095. Not quite ready to disable this.
  bool hidpi = false && widget && widget->GetDefaultScale().scale >= 2;
  if (hidpi) {
    flags |= Layer::CONTENT_DISABLE_SUBPIXEL_AA;
  }
  if (isOpaque && !data->mForceTransparentSurface) {
    flags |= Layer::CONTENT_OPAQUE;
  } else if (data->mNeedComponentAlpha && !hidpi) {
    flags |= Layer::CONTENT_COMPONENT_ALPHA;
  }
  if (data->mDisableFlattening) {
    flags |= Layer::CONTENT_DISABLE_FLATTENING;
  }
  layer->SetContentFlags(flags);

  userData->mItems = Move(data->mAssignedDisplayItems);
  userData->mContainerLayerFrame = GetContainerFrame();

  PaintedLayerData* containingPaintedLayerData =
     mLayerBuilder->GetContainingPaintedLayerData();
  // If we're building layers for an inactive layer, the event regions are
  // clipped to the inactive layer's clip prior to being combined into the
  // event regions of the containing PLD.
  // For the dispatch-to-content and maybe-hit regions, rounded corners on
  // the clip are ignored, since these are approximate regions. For the
  // remaining regions, rounded corners in the clip cause the region to
  // be combined into the corresponding "imprecise" region of the
  // containing's PLD (e.g. the maybe-hit region instead of the hit region).
  const DisplayItemClip* inactiveLayerClip = mLayerBuilder->GetInactiveLayerClip();
  if (containingPaintedLayerData) {
    if (!data->mDispatchToContentHitRegion.GetBounds().IsEmpty()) {
      nsRect rect = nsLayoutUtils::TransformFrameRectToAncestor(
        mContainerReferenceFrame,
        data->mDispatchToContentHitRegion.GetBounds(),
        containingPaintedLayerData->mReferenceFrame);
      if (inactiveLayerClip) {
        rect = inactiveLayerClip->ApplyNonRoundedIntersection(rect);
      }
      containingPaintedLayerData->mDispatchToContentHitRegion.Or(
        containingPaintedLayerData->mDispatchToContentHitRegion, rect);
      containingPaintedLayerData->mDispatchToContentHitRegion.SimplifyOutward(8);
      if (data->mDTCRequiresTargetConfirmation) {
        containingPaintedLayerData->mDTCRequiresTargetConfirmation = true;
      }
    }
    if (!data->mMaybeHitRegion.GetBounds().IsEmpty()) {
      nsRect rect = nsLayoutUtils::TransformFrameRectToAncestor(
        mContainerReferenceFrame,
        data->mMaybeHitRegion.GetBounds(),
        containingPaintedLayerData->mReferenceFrame);
      if (inactiveLayerClip) {
        rect = inactiveLayerClip->ApplyNonRoundedIntersection(rect);
      }
      containingPaintedLayerData->mMaybeHitRegion.Or(
        containingPaintedLayerData->mMaybeHitRegion, rect);
      containingPaintedLayerData->mMaybeHitRegion.SimplifyOutward(8);
    }
    Maybe<Matrix4x4Flagged> matrixCache;
    nsLayoutUtils::TransformToAncestorAndCombineRegions(
      data->mHitRegion,
      mContainerReferenceFrame,
      containingPaintedLayerData->mReferenceFrame,
      &containingPaintedLayerData->mHitRegion,
      &containingPaintedLayerData->mMaybeHitRegion,
      &matrixCache,
      inactiveLayerClip);
    // See the comment in nsDisplayList::AddFrame, where the touch action regions
    // are handled. The same thing applies here.
    bool alreadyHadRegions =
        !containingPaintedLayerData->mNoActionRegion.IsEmpty() ||
        !containingPaintedLayerData->mHorizontalPanRegion.IsEmpty() ||
        !containingPaintedLayerData->mVerticalPanRegion.IsEmpty();
    nsLayoutUtils::TransformToAncestorAndCombineRegions(
      data->mNoActionRegion,
      mContainerReferenceFrame,
      containingPaintedLayerData->mReferenceFrame,
      &containingPaintedLayerData->mNoActionRegion,
      &containingPaintedLayerData->mDispatchToContentHitRegion,
      &matrixCache,
      inactiveLayerClip);
    nsLayoutUtils::TransformToAncestorAndCombineRegions(
      data->mHorizontalPanRegion,
      mContainerReferenceFrame,
      containingPaintedLayerData->mReferenceFrame,
      &containingPaintedLayerData->mHorizontalPanRegion,
      &containingPaintedLayerData->mDispatchToContentHitRegion,
      &matrixCache,
      inactiveLayerClip);
    nsLayoutUtils::TransformToAncestorAndCombineRegions(
      data->mVerticalPanRegion,
      mContainerReferenceFrame,
      containingPaintedLayerData->mReferenceFrame,
      &containingPaintedLayerData->mVerticalPanRegion,
      &containingPaintedLayerData->mDispatchToContentHitRegion,
      &matrixCache,
      inactiveLayerClip);
    if (alreadyHadRegions) {
      containingPaintedLayerData->mDispatchToContentHitRegion.OrWith(
        containingPaintedLayerData->CombinedTouchActionRegion());
    }
  } else {
    EventRegions regions(
        ScaleRegionToOutsidePixels(data->mHitRegion),
        ScaleRegionToOutsidePixels(data->mMaybeHitRegion),
        ScaleRegionToOutsidePixels(data->mDispatchToContentHitRegion),
        ScaleRegionToOutsidePixels(data->mNoActionRegion),
        ScaleRegionToOutsidePixels(data->mHorizontalPanRegion),
        ScaleRegionToOutsidePixels(data->mVerticalPanRegion),
        data->mDTCRequiresTargetConfirmation);

    Matrix mat = layer->GetTransform().As2D();
    mat.Invert();
    regions.ApplyTranslationAndScale(mat._31, mat._32, mat._11, mat._22);

    layer->SetEventRegions(regions);
  }

  SetBackfaceHiddenForLayer(data->mBackfaceHidden, data->mLayer);
  if (layer != data->mLayer) {
    SetBackfaceHiddenForLayer(data->mBackfaceHidden, layer);
  }
}

static bool
IsItemAreaInWindowOpaqueRegion(nsDisplayListBuilder* aBuilder,
                               nsDisplayItem* aItem,
                               const nsRect& aComponentAlphaBounds)
{
  if (!aItem->Frame()->PresContext()->IsChrome()) {
    // Assume that Web content is always in the window opaque region.
    return true;
  }
  if (aItem->ReferenceFrame() != aBuilder->RootReferenceFrame()) {
    // aItem is probably in some transformed subtree.
    // We're not going to bother figuring out where this landed, we're just
    // going to assume it might have landed over a transparent part of
    // the window.
    return false;
  }
  return aBuilder->GetWindowOpaqueRegion().Contains(aComponentAlphaBounds);
}

void
PaintedLayerData::Accumulate(ContainerState* aState,
                             nsDisplayItem* aItem,
                             const nsIntRect& aVisibleRect,
                             const DisplayItemClip& aClip,
                             LayerState aLayerState,
                             nsDisplayList* aList,
                             DisplayItemEntryType aType)
{
  FLB_LOG_PAINTED_LAYER_DECISION(this, "Accumulating dp=%s(%p), f=%p against pld=%p\n", aItem->Name(), aItem, aItem->Frame(), this);

  const bool hasOpacity = mOpacityIndices.Length() > 0;

  const DisplayItemClip* oldClip = mItemClip;
  mItemClip = &aClip;

  if (aType == DisplayItemEntryType::POP_OPACITY) {
    MOZ_ASSERT(!mOpacityIndices.IsEmpty());
    mOpacityIndices.RemoveLastElement();

    AssignedDisplayItem item(aItem, aLayerState,
                             nullptr, aType, hasOpacity);
    mAssignedDisplayItems.AppendElement(Move(item));
    return;
  }

  if (aState->mBuilder->NeedToForceTransparentSurfaceForItem(aItem)) {
    mForceTransparentSurface = true;
  }

  nsRect componentAlphaBounds;
  if (aState->mParameters.mDisableSubpixelAntialiasingInDescendants) {
    // Disable component alpha.
    // Note that the transform (if any) on the PaintedLayer is always an integer
    // translation so we don't have to factor that in here.
    aItem->DisableComponentAlpha();
  } else {
    componentAlphaBounds = aItem->GetComponentAlphaBounds(aState->mBuilder);

    if (!componentAlphaBounds.IsEmpty()) {
      // This display item needs background copy when pushing opacity group.
      for (size_t i : mOpacityIndices) {
        AssignedDisplayItem& item = mAssignedDisplayItems[i];
        MOZ_ASSERT(item.mType == DisplayItemEntryType::PUSH_OPACITY ||
                   item.mType == DisplayItemEntryType::PUSH_OPACITY_WITH_BG);
        item.mType = DisplayItemEntryType::PUSH_OPACITY_WITH_BG;
      }
    }
  }

  bool clipMatches = (oldClip == mItemClip) || (oldClip && *oldClip == *mItemClip);

  DisplayItemData* currentData =
    aItem->HasMergedFrames() ? nullptr : aItem->GetDisplayItemData();

  DisplayItemData* oldData =
    aState->mLayerBuilder->GetOldLayerForFrame(aItem->Frame(),
                                               aItem->GetPerFrameKey(),
                                               currentData);
  AssignedDisplayItem item(aItem, aLayerState,
                           oldData, aType, hasOpacity);
  mAssignedDisplayItems.AppendElement(Move(item));

  if (aType == DisplayItemEntryType::PUSH_OPACITY) {
    mOpacityIndices.AppendElement(mAssignedDisplayItems.Length() - 1);
  }

  if (aItem->MustPaintOnContentSide()) {
     mShouldPaintOnContentSide = true;
  }

  if (!mIsSolidColorInVisibleRegion && mOpaqueRegion.Contains(aVisibleRect) &&
      mVisibleRegion.Contains(aVisibleRect) && !mImage) {
    // A very common case! Most pages have a PaintedLayer with the page
    // background (opaque) visible and most or all of the page content over the
    // top of that background.
    // The rest of this method won't do anything. mVisibleRegion and mOpaqueRegion
    // don't need updating. mVisibleRegion contains aVisibleRect already,
    // mOpaqueRegion contains aVisibleRect and therefore whatever the opaque
    // region of the item is. mVisibleRegion must contain mOpaqueRegion
    // and therefore aVisibleRect.
    return;
  }

  nsIntRegion opaquePixels;

  // Active opacity means no opaque pixels.
  if (!hasOpacity) {
    opaquePixels = aState->ComputeOpaqueRect(aItem, mAnimatedGeometryRoot, mASR,
                                             aClip, aList, &mHideAllLayersBelow,
                                             &mOpaqueForAnimatedGeometryRootParent);
    opaquePixels.AndWith(aVisibleRect);
  }

  /* Mark as available for conversion to image layer if this is a nsDisplayImage and
   * it's the only thing visible in this layer.
   */
  if (nsIntRegion(aVisibleRect).Contains(mVisibleRegion) &&
      opaquePixels.Contains(mVisibleRegion) &&
      aItem->SupportsOptimizingToImage()) {
    mImage = static_cast<nsDisplayImageContainer*>(aItem);
    FLB_LOG_PAINTED_LAYER_DECISION(this, "  Tracking image: nsDisplayImageContainer covers the layer\n");
  } else if (mImage) {
    FLB_LOG_PAINTED_LAYER_DECISION(this, "  No longer tracking image\n");
    mImage = nullptr;
  }

  bool isFirstVisibleItem = mVisibleRegion.IsEmpty();

  Maybe<nscolor> uniformColor;
  if (!hasOpacity) {
    uniformColor = aItem->IsUniform(aState->mBuilder);
  }

  // Some display items have to exist (so they can set forceTransparentSurface
  // below) but don't draw anything. They'll return true for isUniform but
  // a color with opacity 0.
  if (!uniformColor || NS_GET_A(*uniformColor) > 0) {
    // Make sure that the visible area is covered by uniform pixels. In
    // particular this excludes cases where the edges of the item are not
    // pixel-aligned (thus the item will not be truly uniform).
    if (uniformColor) {
      bool snap;
      nsRect bounds = aItem->GetBounds(aState->mBuilder, &snap);
      if (!aState->ScaleToInsidePixels(bounds, snap).Contains(aVisibleRect)) {
        uniformColor = Nothing();
        FLB_LOG_PAINTED_LAYER_DECISION(this, "  Display item does not cover the visible rect\n");
      }
    }
    if (uniformColor) {
      if (isFirstVisibleItem) {
        // This color is all we have
        mSolidColor = *uniformColor;
        mIsSolidColorInVisibleRegion = true;
      } else if (mIsSolidColorInVisibleRegion &&
                 mVisibleRegion.IsEqual(nsIntRegion(aVisibleRect)) &&
                 clipMatches) {
        // we can just blend the colors together
        mSolidColor = NS_ComposeColors(mSolidColor, *uniformColor);
      } else {
        FLB_LOG_PAINTED_LAYER_DECISION(this, "  Layer not a solid color: Can't blend colors togethers\n");
        mIsSolidColorInVisibleRegion = false;
      }
    } else {
      FLB_LOG_PAINTED_LAYER_DECISION(this, "  Layer is not a solid color: Display item is not uniform over the visible bound\n");
      mIsSolidColorInVisibleRegion = false;
    }

    mVisibleRegion.Or(mVisibleRegion, aVisibleRect);
    mVisibleRegion.SimplifyOutward(4);
  }

  if (!opaquePixels.IsEmpty()) {
    for (auto iter = opaquePixels.RectIter(); !iter.Done(); iter.Next()) {
      // We don't use SimplifyInward here since it's not defined exactly
      // what it will discard. For our purposes the most important case
      // is a large opaque background at the bottom of z-order (e.g.,
      // a canvas background), so we need to make sure that the first rect
      // we see doesn't get discarded.
      nsIntRegion tmp;
      tmp.Or(mOpaqueRegion, iter.Get());
       // Opaque display items in chrome documents whose window is partially
       // transparent are always added to the opaque region. This helps ensure
       // that we get as much subpixel-AA as possible in the chrome.
       if (tmp.GetNumRects() <= 4 || aItem->Frame()->PresContext()->IsChrome()) {
        mOpaqueRegion = Move(tmp);
      }
    }
  }

  if (!aState->mParameters.mDisableSubpixelAntialiasingInDescendants &&
      !componentAlphaBounds.IsEmpty()) {
    nsIntRect componentAlphaRect =
      aState->ScaleToOutsidePixels(componentAlphaBounds, false).Intersect(aVisibleRect);

    if (!mOpaqueRegion.Contains(componentAlphaRect)) {
      if (IsItemAreaInWindowOpaqueRegion(aState->mBuilder, aItem,
            componentAlphaBounds.Intersect(aItem->GetVisibleRect()))) {
        mNeedComponentAlpha = true;
      } else {
        aItem->DisableComponentAlpha();
      }
    }
  }

  // Ensure animated text does not get flattened, even if it forces other
  // content in the container to be layerized. The content backend might
  // not support subpixel positioning of text that animated transforms can
  // generate. bug 633097
  if (aState->mParameters.mInActiveTransformedSubtree &&
       (mNeedComponentAlpha || !componentAlphaBounds.IsEmpty())) {
    mDisableFlattening = true;
  }
}

nsRegion
PaintedLayerData::CombinedTouchActionRegion()
{
  nsRegion result;
  result.Or(mHorizontalPanRegion, mVerticalPanRegion);
  result.OrWith(mNoActionRegion);
  return result;
}

void
PaintedLayerData::AccumulateEventRegions(ContainerState* aState, nsDisplayLayerEventRegions* aEventRegions)
{
  FLB_LOG_PAINTED_LAYER_DECISION(this, "Accumulating event regions %p against pld=%p\n", aEventRegions, this);

  mHitRegion.OrWith(aEventRegions->HitRegion());
  mMaybeHitRegion.OrWith(aEventRegions->MaybeHitRegion());
  mDispatchToContentHitRegion.OrWith(aEventRegions->DispatchToContentHitRegion());

  // See the comment in nsDisplayList::AddFrame, where the touch action regions
  // are handled. The same thing applies here.
  bool alreadyHadRegions = !mNoActionRegion.IsEmpty() ||
      !mHorizontalPanRegion.IsEmpty() ||
      !mVerticalPanRegion.IsEmpty();
  mNoActionRegion.OrWith(aEventRegions->NoActionRegion());
  mHorizontalPanRegion.OrWith(aEventRegions->HorizontalPanRegion());
  mVerticalPanRegion.OrWith(aEventRegions->VerticalPanRegion());
  if (alreadyHadRegions) {
    mDispatchToContentHitRegion.OrWith(CombinedTouchActionRegion());
  }

  // Avoid quadratic performance as a result of the region growing to include
  // and arbitrarily large number of rects, which can happen on some pages.
  mMaybeHitRegion.SimplifyOutward(8);
  mDispatchToContentHitRegion.SimplifyOutward(8);

  // Calculate scaled versions of the bounds of mHitRegion and mMaybeHitRegion
  // for quick access in FindPaintedLayerFor().
  mScaledHitRegionBounds = aState->ScaleToOutsidePixels(mHitRegion.GetBounds());
  mScaledMaybeHitRegionBounds = aState->ScaleToOutsidePixels(mMaybeHitRegion.GetBounds());
}

void
PaintedLayerData::AccumulateHitTestInfo(ContainerState* aState,
                                        nsDisplayCompositorHitTestInfo* aItem)
{
  FLB_LOG_PAINTED_LAYER_DECISION(this,
    "Accumulating hit test info %p against pld=%p\n", aItem, this);

  const mozilla::DisplayItemClip& clip = aItem->GetClip();
  const nsRect area = clip.ApplyNonRoundedIntersection(aItem->Area());
  const mozilla::gfx::CompositorHitTestInfo hitTestInfo = aItem->HitTestInfo();

  bool hasRoundedCorners = clip.GetRoundedRectCount() > 0;

  // use the NS_FRAME_SIMPLE_EVENT_REGIONS to avoid calling the slightly
  // expensive HasNonZeroCorner function if we know from a previous run that
  // the frame has zero corners.
  nsIFrame* frame = aItem->Frame();

  bool simpleRegions = frame->HasAnyStateBits(NS_FRAME_SIMPLE_EVENT_REGIONS);
  if (!simpleRegions) {
    if (nsLayoutUtils::HasNonZeroCorner(frame->StyleBorder()->mBorderRadius)) {
      hasRoundedCorners = true;
    } else {
      frame->AddStateBits(NS_FRAME_SIMPLE_EVENT_REGIONS);
    }
  }

  if (hasRoundedCorners || (frame->GetStateBits() & NS_FRAME_SVG_LAYOUT)) {
    mMaybeHitRegion.OrWith(area);
  } else {
    mHitRegion.OrWith(area);
  }

  if (aItem->HitTestInfo() & CompositorHitTestInfo::eDispatchToContent) {
    mDispatchToContentHitRegion.OrWith(area);

    if (aItem->HitTestInfo() & CompositorHitTestInfo::eRequiresTargetConfirmation) {
      mDTCRequiresTargetConfirmation = true;
    }
  }

  auto touchFlags = hitTestInfo & CompositorHitTestInfo::eTouchActionMask;
  if (touchFlags) {
    // something was disabled
    if (touchFlags == CompositorHitTestInfo::eTouchActionMask) {
      // everything was disabled, so touch-action:none
      mNoActionRegion.OrWith(area);
    } else {
      // The event regions code does not store enough information to actually
      // represent all the different states. Prior to the introduction of
      // CompositorHitTestInfo here in bug 1389149, the following two cases
      // were effectively getting collapsed:
      //   (1) touch-action: auto
      //   (2) touch-action: manipulation
      // In both of these cases, none of {mNoActionRegion, mHorizontalPanRegion,
      // mVerticalPanRegion} were modified, and so the fact that case (2) should
      // have prevented double-tap-zooming was getting lost.
      // With CompositorHitTestInfo we can now represent that case correctly,
      // but only if we use CompositorHitTestInfo all the way to the compositor
      // (i.e. in the WebRender-enabled case). In the non-WebRender case where
      // we still use the event regions, we must collapse these two cases back
      // together. Or add another region to the event regions to fix this
      // properly.
      if (touchFlags != CompositorHitTestInfo::eTouchActionDoubleTapZoomDisabled) {
        if (!(hitTestInfo & CompositorHitTestInfo::eTouchActionPanXDisabled)) {
          // pan-x is allowed
          mHorizontalPanRegion.OrWith(area);
        }
        if (!(hitTestInfo & CompositorHitTestInfo::eTouchActionPanYDisabled)) {
          // pan-y is allowed
          mVerticalPanRegion.OrWith(area);
        }
      } else {
        // the touch-action: manipulation case described above. To preserve the
        // existing behaviour, don't touch either mHorizontalPanRegion or
        // mVerticalPanRegion
      }
    }
  }

  // If there are multiple touch-action areas, there are multiple elements with
  // touch-action properties. We don't know what the relationship is between
  // those elements in terms of DOM ancestry, and so we don't know how to
  // combine the regions properly. Instead, we just add all the areas to the
  // dispatch-to-content region, so that the APZ knows to check with the
  // main thread. See bug 1286957.
  const int alreadyHadRegions = mNoActionRegion.GetNumRects() +
    mHorizontalPanRegion.GetNumRects() +
    mVerticalPanRegion.GetNumRects();

  if (alreadyHadRegions > 1) {
    mDispatchToContentHitRegion.OrWith(CombinedTouchActionRegion());
  }

  // Avoid quadratic performance as a result of the region growing to include
  // and arbitrarily large number of rects, which can happen on some pages.
  mMaybeHitRegion.SimplifyOutward(8);
  mDispatchToContentHitRegion.SimplifyOutward(8);

  // Calculate scaled versions of the bounds of mHitRegion and mMaybeHitRegion
  // for quick access in FindPaintedLayerFor().
  mScaledHitRegionBounds =
    aState->ScaleToOutsidePixels(mHitRegion.GetBounds());
  mScaledMaybeHitRegionBounds =
    aState->ScaleToOutsidePixels(mMaybeHitRegion.GetBounds());
}

void
ContainerState::NewPaintedLayerData(PaintedLayerData* aData,
                                    AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                    const ActiveScrolledRoot* aASR,
                                    const DisplayItemClipChain* aClipChain,
                                    const ActiveScrolledRoot* aScrollMetadataASR,
                                    const nsPoint& aTopLeft,
                                    const nsIFrame* aReferenceFrame,
                                    const bool aBackfaceHidden)
{
  aData->mAnimatedGeometryRoot = aAnimatedGeometryRoot;
  aData->mASR = aASR;
  aData->mClipChain = aClipChain;
  aData->mAnimatedGeometryRootOffset = aTopLeft;
  aData->mReferenceFrame = aReferenceFrame;
  aData->mBackfaceHidden = aBackfaceHidden;

  aData->mNewChildLayersIndex = mNewChildLayers.Length();
  NewLayerEntry* newLayerEntry = mNewChildLayers.AppendElement();
  newLayerEntry->mAnimatedGeometryRoot = aAnimatedGeometryRoot;
  newLayerEntry->mASR = aASR;
  newLayerEntry->mScrollMetadataASR = aScrollMetadataASR;
  newLayerEntry->mClipChain = aClipChain;
  // newLayerEntry->mOpaqueRegion is filled in later from
  // paintedLayerData->mOpaqueRegion, if necessary.

  // Allocate another entry for this layer's optimization to ColorLayer/ImageLayer
  mNewChildLayers.AppendElement();
}

#ifdef MOZ_DUMP_PAINTING
static void
DumpPaintedImage(nsDisplayItem* aItem, SourceSurface* aSurface)
{
  nsCString string(aItem->Name());
  string.Append('-');
  string.AppendInt((uint64_t)aItem);
  fprintf_stderr(gfxUtils::sDumpPaintFile, "<script>array[\"%s\"]=\"", string.BeginReading());
  gfxUtils::DumpAsDataURI(aSurface, gfxUtils::sDumpPaintFile);
  fprintf_stderr(gfxUtils::sDumpPaintFile, "\";</script>\n");
}
#endif

static void
PaintInactiveLayer(nsDisplayListBuilder* aBuilder,
                   LayerManager* aManager,
                   nsDisplayItem* aItem,
                   gfxContext* aContext,
                   gfxContext* aCtx)
{
  // This item has an inactive layer. Render it to a PaintedLayer
  // using a temporary BasicLayerManager.
  BasicLayerManager* basic = static_cast<BasicLayerManager*>(aManager);
  RefPtr<gfxContext> context = aContext;
#ifdef MOZ_DUMP_PAINTING
  int32_t appUnitsPerDevPixel = AppUnitsPerDevPixel(aItem);
  nsIntRect itemVisibleRect =
    aItem->GetVisibleRect().ToOutsidePixels(appUnitsPerDevPixel);

  RefPtr<DrawTarget> tempDT;
  if (gfxEnv::DumpPaint()) {
    tempDT = gfxPlatform::GetPlatform()->CreateOffscreenContentDrawTarget(
                                      itemVisibleRect.Size(),
                                      SurfaceFormat::B8G8R8A8);
    if (tempDT) {
      context = gfxContext::CreateOrNull(tempDT);
      if (!context) {
        // Leave this as crash, it's in the debugging code, we want to know
        gfxDevCrash(LogReason::InvalidContext) << "PaintInactive context problem " << gfx::hexa(tempDT);
        return;
      }
      context->SetMatrix(Matrix::Translation(-itemVisibleRect.x,
                                             -itemVisibleRect.y));
    }
  }
#endif
  basic->BeginTransaction();
  basic->SetTarget(context);

  if (aItem->GetType() == DisplayItemType::TYPE_MASK) {
    static_cast<nsDisplayMask*>(aItem)->PaintAsLayer(aBuilder, aCtx, basic);
    if (basic->InTransaction()) {
      basic->AbortTransaction();
    }
  } else if (aItem->GetType() == DisplayItemType::TYPE_FILTER){
    static_cast<nsDisplayFilter*>(aItem)->PaintAsLayer(aBuilder, aCtx, basic);
    if (basic->InTransaction()) {
      basic->AbortTransaction();
    }
  } else {
    basic->EndTransaction(FrameLayerBuilder::DrawPaintedLayer, aBuilder);
  }
  FrameLayerBuilder *builder = static_cast<FrameLayerBuilder*>(basic->GetUserData(&gLayerManagerLayerBuilder));
  if (builder) {
    builder->DidEndTransaction();
  }

  basic->SetTarget(nullptr);

#ifdef MOZ_DUMP_PAINTING
  if (gfxEnv::DumpPaint() && tempDT) {
    RefPtr<SourceSurface> surface = tempDT->Snapshot();
    DumpPaintedImage(aItem, surface);

    DrawTarget* drawTarget = aContext->GetDrawTarget();
    Rect rect(itemVisibleRect.x, itemVisibleRect.y,
              itemVisibleRect.width, itemVisibleRect.height);
    drawTarget->DrawSurface(surface, rect, Rect(Point(0,0), rect.Size()));

    aItem->SetPainted();
  }
#endif
}

nsRect
ContainerState::GetDisplayPortForAnimatedGeometryRoot(AnimatedGeometryRoot* aAnimatedGeometryRoot)
{
  if (mLastDisplayPortAGR == aAnimatedGeometryRoot) {
    return mLastDisplayPortRect;
  }

  mLastDisplayPortAGR = aAnimatedGeometryRoot;

  nsIScrollableFrame* sf = nsLayoutUtils::GetScrollableFrameFor(*aAnimatedGeometryRoot);
  if (sf == nullptr || nsLayoutUtils::UsesAsyncScrolling(*aAnimatedGeometryRoot)) {
    mLastDisplayPortRect = nsRect();
    return mLastDisplayPortRect;
  }

  bool usingDisplayport =
    nsLayoutUtils::GetDisplayPort((*aAnimatedGeometryRoot)->GetContent(), &mLastDisplayPortRect,
                                  RelativeTo::ScrollFrame);
  if (!usingDisplayport) {
    // No async scrolling, so all that matters is that the layer contents
    // cover the scrollport.
    mLastDisplayPortRect = sf->GetScrollPortRect();
  }
  nsIFrame* scrollFrame = do_QueryFrame(sf);
  mLastDisplayPortRect += scrollFrame->GetOffsetToCrossDoc(mContainerReferenceFrame);
  return mLastDisplayPortRect;
}

nsIntRegion
ContainerState::ComputeOpaqueRect(nsDisplayItem* aItem,
                                  AnimatedGeometryRoot* aAnimatedGeometryRoot,
                                  const ActiveScrolledRoot* aASR,
                                  const DisplayItemClip& aClip,
                                  nsDisplayList* aList,
                                  bool* aHideAllLayersBelow,
                                  bool* aOpaqueForAnimatedGeometryRootParent)
{
  bool snapOpaque;
  nsRegion opaque = aItem->GetOpaqueRegion(mBuilder, &snapOpaque);
  if (opaque.IsEmpty()) {
    return nsIntRegion();
  }

  nsIntRegion opaquePixels;
  nsRegion opaqueClipped;
  for (auto iter = opaque.RectIter(); !iter.Done(); iter.Next()) {
    opaqueClipped.Or(opaqueClipped,
                     aClip.ApproximateIntersectInward(iter.Get()));
  }
  if (aAnimatedGeometryRoot == mContainerAnimatedGeometryRoot &&
      aASR == mContainerASR &&
      opaqueClipped.Contains(mContainerBounds)) {
    *aHideAllLayersBelow = true;
    aList->SetIsOpaque();
  }
  // Add opaque areas to the "exclude glass" region. Only do this when our
  // container layer is going to be the rootmost layer, otherwise transforms
  // etc will mess us up (and opaque contributions from other containers are
  // not needed).
  if (!nsLayoutUtils::GetCrossDocParentFrame(mContainerFrame)) {
    mBuilder->AddWindowOpaqueRegion(opaqueClipped);
  }
  opaquePixels = ScaleRegionToInsidePixels(opaqueClipped, snapOpaque);

  if (IsInInactiveLayer()) {
    return opaquePixels;
  }

  const nsRect& displayport =
    GetDisplayPortForAnimatedGeometryRoot(aAnimatedGeometryRoot);
  if (!displayport.IsEmpty() &&
      opaquePixels.Contains(ScaleRegionToNearestPixels(displayport))) {
    *aOpaqueForAnimatedGeometryRootParent = true;
  }
  return opaquePixels;
}

Maybe<size_t>
ContainerState::SetupMaskLayerForScrolledClip(Layer* aLayer,
                                              const DisplayItemClip& aClip)
{
  if (aClip.GetRoundedRectCount() > 0) {
    Maybe<size_t> maskLayerIndex = Some(aLayer->GetAncestorMaskLayerCount());
    if (RefPtr<Layer> maskLayer = CreateMaskLayer(aLayer, aClip, maskLayerIndex)) {
      aLayer->AddAncestorMaskLayer(maskLayer);
      return maskLayerIndex;
    }
    // Fall through to |return Nothing()|.
  }
  return Nothing();
}

static const ActiveScrolledRoot*
GetASRForPerspective(const ActiveScrolledRoot* aASR, nsIFrame* aPerspectiveFrame)
{
  for (const ActiveScrolledRoot* asr = aASR; asr; asr = asr->mParent) {
    nsIFrame* scrolledFrame = asr->mScrollableFrame->GetScrolledFrame();
    if (nsLayoutUtils::IsAncestorFrameCrossDoc(scrolledFrame, aPerspectiveFrame)) {
      return asr;
    }
  }
  return nullptr;
}

static CSSMaskLayerUserData*
GetCSSMaskLayerUserData(Layer* aMaskLayer)
{
  if (!aMaskLayer) {
    return nullptr;
  }

  return static_cast<CSSMaskLayerUserData*>(aMaskLayer->GetUserData(&gCSSMaskLayerUserData));
}

static void
SetCSSMaskLayerUserData(Layer* aMaskLayer)
{
  MOZ_ASSERT(aMaskLayer);

  aMaskLayer->SetUserData(&gCSSMaskLayerUserData,
                          new CSSMaskLayerUserData());
}

void
ContainerState::SetupMaskLayerForCSSMask(Layer* aLayer,
                                         nsDisplayMask* aMaskItem)
{
  RefPtr<ImageLayer> maskLayer =
    CreateOrRecycleMaskImageLayerFor(MaskLayerKey(aLayer, Nothing()),
                                     GetCSSMaskLayerUserData,
                                     SetCSSMaskLayerUserData);
  CSSMaskLayerUserData* oldUserData = GetCSSMaskLayerUserData(maskLayer.get());
  MOZ_ASSERT(oldUserData);

  bool snap;
  nsRect bounds = aMaskItem->GetBounds(mBuilder, &snap);
  nsIntRect itemRect = ScaleToOutsidePixels(bounds, snap);

  // Setup mask layer offset.
  // We do not repaint mask for mask position change, so update base transform
  // each time is required.
  Matrix4x4 matrix;
  matrix.PreTranslate(itemRect.x, itemRect.y, 0);
  matrix.PreTranslate(mParameters.mOffset.x, mParameters.mOffset.y, 0);
  maskLayer->SetBaseTransform(matrix);

  nsPoint maskLayerOffset = aMaskItem->ToReferenceFrame() - bounds.TopLeft();

  CSSMaskLayerUserData newUserData(aMaskItem->Frame(), itemRect, maskLayerOffset);
  nsRect dirtyRect;
  if (!aMaskItem->IsInvalid(dirtyRect) && *oldUserData == newUserData) {
    aLayer->SetMaskLayer(maskLayer);
    return;
  }

  int32_t maxSize = mManager->GetMaxTextureSize();
  IntSize surfaceSize(std::min(itemRect.width, maxSize),
                      std::min(itemRect.height, maxSize));

  if (surfaceSize.IsEmpty()) {
    // Return early if we know that the size of this mask surface is empty.
    return;
  }

  MaskImageData imageData(surfaceSize, mManager);
  RefPtr<DrawTarget> dt = imageData.CreateDrawTarget();
  if (!dt || !dt->IsValid()) {
    NS_WARNING("Could not create DrawTarget for mask layer.");
    return;
  }

  RefPtr<gfxContext> maskCtx = gfxContext::CreateOrNull(dt);
  maskCtx->SetMatrix(Matrix::Translation(-itemRect.TopLeft()));
  maskCtx->Multiply(gfxMatrix::Scaling(mParameters.mXScale, mParameters.mYScale));

  bool isPaintFinished = aMaskItem->PaintMask(mBuilder, maskCtx);

  RefPtr<ImageContainer> imgContainer =
    imageData.CreateImageAndImageContainer();
  if (!imgContainer) {
    return;
  }
  maskLayer->SetContainer(imgContainer);

  if (isPaintFinished) {
    *oldUserData = Move(newUserData);
  }
  aLayer->SetMaskLayer(maskLayer);
}

static bool
IsScrollThumbLayer(nsDisplayItem* aItem)
{
  return aItem->GetType() == DisplayItemType::TYPE_OWN_LAYER &&
         static_cast<nsDisplayOwnLayer*>(aItem)->IsScrollThumbLayer();
}

/*
 * Iterate through the non-clip items in aList and its descendants.
 * For each item we compute the effective clip rect. Each item is assigned
 * to a layer. We invalidate the areas in PaintedLayers where an item
 * has moved from one PaintedLayer to another. Also,
 * aState->mInvalidPaintedContent is invalidated in every PaintedLayer.
 * We set the clip rect for items that generated their own layer, and
 * create a mask layer to do any rounded rect clipping.
 * (PaintedLayers don't need a clip rect on the layer, we clip the items
 * individually when we draw them.)
 * We set the visible rect for all layers, although the actual setting
 * of visible rects for some PaintedLayers is deferred until the calling
 * of ContainerState::Finish.
 */
void
ContainerState::ProcessDisplayItems(nsDisplayList* aList)
{
  AUTO_PROFILER_LABEL("ContainerState::ProcessDisplayItems", GRAPHICS);

  nsPoint topLeft(0,0);

  int32_t maxLayers = gfxPrefs::MaxActiveLayers();
  int layerCount = 0;

#ifdef DEBUG
  bool hadLayerEventRegions = false;
  bool hadCompositorHitTestInfo = false;
#endif

  if (!mManager->IsWidgetLayerManager()) {
    mPaintedLayerDataTree.InitializeForInactiveLayer(mContainerAnimatedGeometryRoot);
  }

  AnimatedGeometryRoot* lastAnimatedGeometryRoot = nullptr;
  nsPoint lastTopLeft;

  // Tracks the PaintedLayerData that the item will be accumulated in, if it is
  // non-null. Currently only used with PUSH_OPACITY and POP_OPACITY markers.
  PaintedLayerData* selectedPLD = nullptr;

  FLBDisplayItemIterator iter(mBuilder, aList, this);
  while (iter.HasNext()) {
    DisplayItemEntry e = iter.GetNextEntry();
    nsDisplayItem* i = e.mItem;
    DisplayItemEntryType marker = e.mType;


    nsDisplayItem* item = i;
    MOZ_ASSERT(item);
    DisplayItemType itemType = item->GetType();

    // If the item is a event regions item, but is empty (has no regions in it)
    // then we should just throw it out
    if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
#ifdef DEBUG
      hadLayerEventRegions = true;
#endif
      nsDisplayLayerEventRegions* eventRegions =
        static_cast<nsDisplayLayerEventRegions*>(item);

      if (eventRegions->IsEmpty()) {
        continue;
      }
    }

    if (itemType == DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO) {
#ifdef DEBUG
      hadCompositorHitTestInfo = true;
#endif
      nsDisplayCompositorHitTestInfo* hitTestInfo =
        static_cast<nsDisplayCompositorHitTestInfo*>(item);

      if (hitTestInfo->Area().IsEmpty()) {
        continue;
      }
    }

    // Only allow either LayerEventRegions or CompositorHitTestInfo items.
    MOZ_ASSERT(!(hadLayerEventRegions && hadCompositorHitTestInfo));

    // Peek ahead to the next item and see if it can be merged with the current
    // item. We create a list of consecutive items that can be merged together.
    AutoTArray<nsDisplayItem*, 1> mergedItems;

    if (marker == DisplayItemEntryType::ITEM) {
      mergedItems.AppendElement(item);

      while (nsDisplayItem* peek = iter.PeekNext()) {
        if (!item->CanMerge(peek)) {
          break;
        }

        mergedItems.AppendElement(peek);

        // Move the iterator forward since we will merge this item.
        i = iter.GetNext();
      }
    }

    if (mergedItems.Length() > 1) {
      // We have items that can be merged together. Merge them into a temporary
      // item and process that item immediately.
      item = mBuilder->MergeItems(mergedItems);
      MOZ_ASSERT(item && itemType == item->GetType());
    }

    MOZ_ASSERT(item->GetType() != DisplayItemType::TYPE_WRAP_LIST);

    NS_ASSERTION(mAppUnitsPerDevPixel == AppUnitsPerDevPixel(item),
      "items in a container layer should all have the same app units per dev pixel");

    if (mBuilder->NeedToForceTransparentSurfaceForItem(item)) {
      aList->SetNeedsTransparentSurface();
    }

    if (mParameters.mForEventsAndPluginsOnly && !item->GetChildren() &&
        (itemType != DisplayItemType::TYPE_LAYER_EVENT_REGIONS &&
         itemType != DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO &&
         itemType != DisplayItemType::TYPE_PLUGIN)) {
      continue;
    }

    LayerState layerState = LAYER_NONE;

    if (marker == DisplayItemEntryType::ITEM) {
      layerState = item->GetLayerState(mBuilder, mManager, mParameters);

      if (layerState == LAYER_INACTIVE && nsDisplayItem::ForceActiveLayers()) {
        layerState = LAYER_ACTIVE;
      }
    }

    bool forceInactive = false;
    AnimatedGeometryRoot* animatedGeometryRoot;
    const ActiveScrolledRoot* itemASR = nullptr;
    const DisplayItemClipChain* layerClipChain = nullptr;

    if (mManager->IsWidgetLayerManager()) {
      animatedGeometryRoot = item->GetAnimatedGeometryRoot();
      itemASR = item->GetActiveScrolledRoot();
      const DisplayItemClipChain* itemClipChain = item->GetClipChain();
      if (itemClipChain && itemClipChain->mASR == itemASR &&
          itemType != DisplayItemType::TYPE_STICKY_POSITION) {
        layerClipChain = itemClipChain->mParent;
      } else {
        layerClipChain = itemClipChain;
      }
    } else {
      // For inactive layer subtrees, splitting content into PaintedLayers
      // based on animated geometry roots is pointless. It's more efficient
      // to build the minimum number of layers.
      animatedGeometryRoot = mContainerAnimatedGeometryRoot;
      itemASR = mContainerASR;
      item->FuseClipChainUpTo(mBuilder, mContainerASR);
    }
    if (animatedGeometryRoot == lastAnimatedGeometryRoot) {
      topLeft = lastTopLeft;
    } else {
      lastTopLeft = topLeft = (*animatedGeometryRoot)->GetOffsetToCrossDoc(mContainerReferenceFrame);
      lastAnimatedGeometryRoot = animatedGeometryRoot;
    }

    const ActiveScrolledRoot* scrollMetadataASR =
        layerClipChain ? ActiveScrolledRoot::PickDescendant(itemASR, layerClipChain->mASR) : itemASR;

    bool snap;
    nsRect itemContent = item->GetBounds(mBuilder, &snap);
    if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
      nsDisplayLayerEventRegions* eventRegions =
        static_cast<nsDisplayLayerEventRegions*>(item);
      itemContent = eventRegions->GetHitRegionBounds(mBuilder, &snap);
    }

    if (itemType == DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO) {
      nsDisplayCompositorHitTestInfo* eventRegions =
        static_cast<nsDisplayCompositorHitTestInfo*>(item);
      itemContent = eventRegions->Area();
    }

    nsIntRect itemDrawRect = ScaleToOutsidePixels(itemContent, snap);
    bool prerenderedTransform = itemType == DisplayItemType::TYPE_TRANSFORM &&
        static_cast<nsDisplayTransform*>(item)->MayBeAnimated(mBuilder);
    ParentLayerIntRect clipRect;
    const DisplayItemClip& itemClip = item->GetClip();
    if (itemClip.HasClip()) {
      itemContent.IntersectRect(itemContent, itemClip.GetClipRect());
      clipRect = ViewAs<ParentLayerPixel>(ScaleToNearestPixels(itemClip.GetClipRect()));
      if (!prerenderedTransform && !IsScrollThumbLayer(item)) {
        itemDrawRect.IntersectRect(itemDrawRect, clipRect.ToUnknownRect());
      }
      clipRect.MoveBy(ViewAs<ParentLayerPixel>(mParameters.mOffset));
    }
#ifdef DEBUG
    nsRect bounds = itemContent;

    if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS ||
        itemType == DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO) {
      bounds.SetEmpty();
    }

    if (!bounds.IsEmpty()) {
      if (itemASR != mContainerASR) {
        if (Maybe<nsRect> clip = item->GetClipWithRespectToASR(mBuilder, mContainerASR)) {
          bounds = clip.ref();
        }
      }
    }
    ((nsRect&)mAccumulatedChildBounds).UnionRect(mAccumulatedChildBounds, bounds);
#endif

    nsIntRect itemVisibleRect = itemDrawRect;
    // We haven't computed visibility at this point, so item->GetVisibleRect()
    // is just the dirty rect that item was initialized with. We intersect it
    // with the clipped item bounds to get a tighter visible rect.
    if (!prerenderedTransform) {
      itemVisibleRect = itemVisibleRect.Intersect(
        ScaleToOutsidePixels(item->GetVisibleRect(), false));
    }

    if (maxLayers != -1 && layerCount >= maxLayers) {
      forceInactive = true;
    }

    // Assign the item to a layer
    if (layerState == LAYER_ACTIVE_FORCE ||
        (layerState == LAYER_INACTIVE && !mManager->IsWidgetLayerManager()) ||
        (!forceInactive &&
         (layerState == LAYER_ACTIVE_EMPTY ||
          layerState == LAYER_ACTIVE))) {

      layerCount++;

      // Currently we do not support flattening effects within nested inactive
      // layer trees.
      MOZ_ASSERT(selectedPLD == nullptr);
      MOZ_ASSERT(marker == DisplayItemEntryType::ITEM);

      // LAYER_ACTIVE_EMPTY means the layer is created just for its metadata.
      // We should never see an empty layer with any visible content!
      NS_ASSERTION(layerState != LAYER_ACTIVE_EMPTY ||
                   itemVisibleRect.IsEmpty(),
                   "State is LAYER_ACTIVE_EMPTY but visible rect is not.");

      // As long as the new layer isn't going to be a PaintedLayer,
      // InvalidateForLayerChange doesn't need the new layer pointer.
      // We also need to check the old data now, because BuildLayer
      // can overwrite it.
      DisplayItemData* oldData =
        mLayerBuilder->GetOldLayerForFrame(item->Frame(), item->GetPerFrameKey());
      InvalidateForLayerChange(item, nullptr, oldData);

      // If the item would have its own layer but is invisible, just hide it.
      // Note that items without their own layers can't be skipped this
      // way, since their PaintedLayer may decide it wants to draw them
      // into its buffer even if they're currently covered.
      if (itemVisibleRect.IsEmpty() &&
          !item->ShouldBuildLayerEvenIfInvisible(mBuilder)) {
        continue;
      }

      // 3D-transformed layers don't necessarily draw in the order in which
      // they're added to their parent container layer.
      bool mayDrawOutOfOrder = itemType == DisplayItemType::TYPE_TRANSFORM &&
        (item->Frame()->Combines3DTransformWithAncestors() ||
         item->Frame()->Extend3DContext());

      // Let mPaintedLayerDataTree know about this item, so that
      // FindPaintedLayerFor and FindOpaqueBackgroundColor are aware of this
      // item, even though it's not in any PaintedLayerDataStack.
      // Ideally we'd only need the "else" case here and have
      // mPaintedLayerDataTree figure out the right clip from the animated
      // geometry root that we give it, but it can't easily figure about
      // overflow:hidden clips on ancestors just by looking at the frame.
      // So we'll do a little hand holding and pass the clip instead of the
      // visible rect for the two important cases.
      nscolor uniformColor = NS_RGBA(0,0,0,0);
      nscolor* uniformColorPtr = (mayDrawOutOfOrder || IsInInactiveLayer()) ? nullptr :
                                                                              &uniformColor;
      nsIntRect clipRectUntyped;
      nsIntRect* clipPtr = nullptr;
      if (itemClip.HasClip()) {
        clipRectUntyped = clipRect.ToUnknownRect();
        clipPtr = &clipRectUntyped;
      }

      bool hasScrolledClip = layerClipChain && layerClipChain->mClip.HasClip() &&
        (!ActiveScrolledRoot::IsAncestor(layerClipChain->mASR, itemASR) ||
         itemType == DisplayItemType::TYPE_STICKY_POSITION);

      if (hasScrolledClip) {
        // If the clip is scrolled, reserve just the area of the clip for
        // layerization, so that elements outside the clip can still merge
        // into the same layer.
        const ActiveScrolledRoot* clipASR = layerClipChain->mASR;
        AnimatedGeometryRoot* clipAGR = mBuilder->AnimatedGeometryRootForASR(clipASR);
        nsIntRect scrolledClipRect =
          ScaleToNearestPixels(layerClipChain->mClip.GetClipRect()) + mParameters.mOffset;
        mPaintedLayerDataTree.AddingOwnLayer(clipAGR,
                                             &scrolledClipRect,
                                             uniformColorPtr);
      } else if (item->ShouldFixToViewport(mBuilder) && itemClip.HasClip() &&
                 item->AnimatedGeometryRootForScrollMetadata() != animatedGeometryRoot &&
                 !nsLayoutUtils::UsesAsyncScrolling(item->Frame())) {
        // This is basically the same as the case above, but for the non-APZ
        // case. At the moment, when APZ is off, there is only the root ASR
        // (because scroll frames without display ports don't create ASRs) and
        // the whole clip chain is always just one fused clip.
        // Bug 1336516 aims to change that and to remove this workaround.
        AnimatedGeometryRoot* clipAGR = item->AnimatedGeometryRootForScrollMetadata();
        nsIntRect scrolledClipRect =
          ScaleToNearestPixels(itemClip.GetClipRect()) + mParameters.mOffset;
        mPaintedLayerDataTree.AddingOwnLayer(clipAGR,
                                             &scrolledClipRect,
                                             uniformColorPtr);
      } else if (IsScrollThumbLayer(item) && mManager->IsWidgetLayerManager()) {
        // For scrollbar thumbs, the clip we care about is the clip added by the
        // slider frame.
        mPaintedLayerDataTree.AddingOwnLayer(animatedGeometryRoot->mParentAGR,
                                             clipPtr,
                                             uniformColorPtr);
      } else if (prerenderedTransform && mManager->IsWidgetLayerManager()) {
        if (animatedGeometryRoot->mParentAGR) {
          mPaintedLayerDataTree.AddingOwnLayer(animatedGeometryRoot->mParentAGR,
                                               clipPtr,
                                               uniformColorPtr);
        } else {
          mPaintedLayerDataTree.AddingOwnLayer(animatedGeometryRoot,
                                               nullptr,
                                               uniformColorPtr);
        }
      } else {
        // Using itemVisibleRect here isn't perfect. itemVisibleRect can be
        // larger or smaller than the potential bounds of item's contents in
        // animatedGeometryRoot: It's too large if there's a clipped display
        // port somewhere among item's contents (see bug 1147673), and it can
        // be too small if the contents can move, because it only looks at the
        // contents' current bounds and doesn't anticipate any animations.
        // Time will tell whether this is good enough, or whether we need to do
        // something more sophisticated here.
        mPaintedLayerDataTree.AddingOwnLayer(animatedGeometryRoot,
                                             &itemVisibleRect, uniformColorPtr);
      }

      ContainerLayerParameters params = mParameters;
      params.mBackgroundColor = uniformColor;
      params.mLayerCreationHint = GetLayerCreationHint(animatedGeometryRoot);
      params.mScrollMetadataASR = ActiveScrolledRoot::PickDescendant(mContainerScrollMetadataASR, scrollMetadataASR);
      params.mCompositorASR = params.mScrollMetadataASR != mContainerScrollMetadataASR
                                ? params.mScrollMetadataASR
                                : mContainerCompositorASR;
      if (itemType == DisplayItemType::TYPE_FIXED_POSITION) {
        params.mCompositorASR = itemASR;
      }

      if (itemType == DisplayItemType::TYPE_PERSPECTIVE) {
        // Perspective items have a single child item, an nsDisplayTransform.
        // If the perspective item is scrolled, but the perspective-inducing
        // frame is outside the scroll frame (indicated by item->Frame()
        // being outside that scroll frame), we have to take special care to
        // make APZ scrolling work properly. APZ needs us to put the scroll
        // frame's FrameMetrics on our child transform ContainerLayer instead.
        // It's worth investigating whether this ASR adjustment can be done at
        // display item creation time.
        scrollMetadataASR = GetASRForPerspective(scrollMetadataASR, item->Frame());
        params.mScrollMetadataASR = scrollMetadataASR;
        itemASR = scrollMetadataASR;
      }

      // Just use its layer.
      // Set layerContentsVisibleRect.width/height to -1 to indicate we
      // currently don't know. If BuildContainerLayerFor gets called by
      // item->BuildLayer, this will be set to a proper rect.
      nsIntRect layerContentsVisibleRect(0, 0, -1, -1);
      params.mLayerContentsVisibleRect = &layerContentsVisibleRect;
      RefPtr<Layer> ownLayer = item->BuildLayer(mBuilder, mManager, params);
      if (!ownLayer) {
        continue;
      }

      NS_ASSERTION(!ownLayer->AsPaintedLayer(),
                   "Should never have created a dedicated Painted layer!");

      if (item->BackfaceIsHidden()) {
        ownLayer->SetContentFlags(ownLayer->GetContentFlags() |
                                  Layer::CONTENT_BACKFACE_HIDDEN);
      } else {
        ownLayer->SetContentFlags(ownLayer->GetContentFlags() &
                                  ~Layer::CONTENT_BACKFACE_HIDDEN);
      }

      nsRect invalid;
      if (item->IsInvalid(invalid)) {
        ownLayer->SetInvalidRectToVisibleRegion();
      }

      // If it's not a ContainerLayer, we need to apply the scale transform
      // ourselves.
      if (!ownLayer->AsContainerLayer()) {
        ownLayer->SetPostScale(mParameters.mXScale,
                               mParameters.mYScale);
      }

      // Update that layer's clip and visible rects.
      NS_ASSERTION(ownLayer->Manager() == mManager, "Wrong manager");
      NS_ASSERTION(!ownLayer->HasUserData(&gLayerManagerUserData),
                   "We shouldn't have a FrameLayerBuilder-managed layer here!");
      NS_ASSERTION(itemClip.HasClip() ||
                   itemClip.GetRoundedRectCount() == 0,
                   "If we have rounded rects, we must have a clip rect");

      // It has its own layer. Update that layer's clip and visible rects.
      ownLayer->SetClipRect(Nothing());
      ownLayer->SetScrolledClip(Nothing());
      ownLayer->SetAncestorMaskLayers({});
      if (itemClip.HasClip()) {
        ownLayer->SetClipRect(Some(clipRect));

        // rounded rectangle clipping using mask layers
        // (must be done after visible rect is set on layer)
        if (itemClip.GetRoundedRectCount() > 0) {
          SetupMaskLayer(ownLayer, itemClip);
        }
      }

      if (hasScrolledClip) {
        const DisplayItemClip& scrolledClip = layerClipChain->mClip;
        LayerClip scrolledLayerClip;
        scrolledLayerClip.SetClipRect(ViewAs<ParentLayerPixel>(
          ScaleToNearestPixels(scrolledClip.GetClipRect()) + mParameters.mOffset));
        if (scrolledClip.GetRoundedRectCount() > 0) {
          scrolledLayerClip.SetMaskLayerIndex(
              SetupMaskLayerForScrolledClip(ownLayer.get(), scrolledClip));
        }
        ownLayer->SetScrolledClip(Some(scrolledLayerClip));
      }

      if (item->GetType() == DisplayItemType::TYPE_MASK) {
        MOZ_ASSERT(itemClip.GetRoundedRectCount() == 0);

        nsDisplayMask* maskItem = static_cast<nsDisplayMask*>(item);
        SetupMaskLayerForCSSMask(ownLayer, maskItem);

        if (iter.PeekNext() &&
            iter.PeekNext()->GetType() == DisplayItemType::TYPE_SCROLL_INFO_LAYER) {
          // Since we do build a layer for mask, there is no need for this
          // scroll info layer anymore.
          i = iter.GetNext();
        }
      }

      // Convert the visible rect to a region and give the item
      // a chance to try restrict it further.
      nsIntRegion itemVisibleRegion = itemVisibleRect;
      nsRegion tightBounds = item->GetTightBounds(mBuilder, &snap);
      if (!tightBounds.IsEmpty()) {
        itemVisibleRegion.AndWith(ScaleToOutsidePixels(tightBounds, snap));
      }

      ContainerLayer* oldContainer = ownLayer->GetParent();
      if (oldContainer && oldContainer != mContainerLayer) {
        oldContainer->RemoveChild(ownLayer);
      }
      NS_ASSERTION(FindIndexOfLayerIn(mNewChildLayers, ownLayer) < 0,
                   "Layer already in list???");

      NewLayerEntry* newLayerEntry = mNewChildLayers.AppendElement();
      newLayerEntry->mLayer = ownLayer;
      newLayerEntry->mAnimatedGeometryRoot = animatedGeometryRoot;
      newLayerEntry->mASR = itemASR;
      newLayerEntry->mScrollMetadataASR = scrollMetadataASR;
      newLayerEntry->mClipChain = layerClipChain;
      newLayerEntry->mLayerState = layerState;
      if (itemType == DisplayItemType::TYPE_FIXED_POSITION) {
        newLayerEntry->mIsFixedToRootScrollFrame =
          item->Frame()->StyleDisplay()->mPosition == NS_STYLE_POSITION_FIXED &&
          nsLayoutUtils::IsReallyFixedPos(item->Frame());
      }

      // Don't attempt to flatten compnent alpha layers that are within
      // a forced active layer, or an active transform;
      if (itemType == DisplayItemType::TYPE_TRANSFORM ||
          layerState == LAYER_ACTIVE_FORCE) {
        newLayerEntry->mPropagateComponentAlphaFlattening = false;
      }

      float contentXScale = 1.0f;
      float contentYScale = 1.0f;
      if (ContainerLayer* ownContainer = ownLayer->AsContainerLayer()) {
        contentXScale = 1 / ownContainer->GetPreXScale();
        contentYScale = 1 / ownContainer->GetPreYScale();
      }
      // nsDisplayTransform::BuildLayer must set layerContentsVisibleRect.
      // We rely on this to ensure 3D transforms compute a reasonable
      // layer visible region.
      NS_ASSERTION(itemType != DisplayItemType::TYPE_TRANSFORM ||
                   layerContentsVisibleRect.width >= 0,
                   "Transform items must set layerContentsVisibleRect!");
      if (mLayerBuilder->IsBuildingRetainedLayers()) {
        newLayerEntry->mLayerContentsVisibleRect = layerContentsVisibleRect;
        if (itemType == DisplayItemType::TYPE_PERSPECTIVE ||
            (itemType == DisplayItemType::TYPE_TRANSFORM &&
             (item->Frame()->Extend3DContext() ||
              item->Frame()->Combines3DTransformWithAncestors() ||
              item->Frame()->HasPerspective()))) {
          // Give untransformed visible region as outer visible region
          // to avoid failure caused by singular transforms.
          newLayerEntry->mUntransformedVisibleRegion = true;
          newLayerEntry->mVisibleRegion =
            item->GetVisibleRectForChildren().ScaleToOutsidePixels(contentXScale, contentYScale, mAppUnitsPerDevPixel);
        } else {
          newLayerEntry->mVisibleRegion = itemVisibleRegion;
        }
        newLayerEntry->mOpaqueRegion = ComputeOpaqueRect(item,
          animatedGeometryRoot, itemASR, itemClip, aList,
          &newLayerEntry->mHideAllLayersBelow,
          &newLayerEntry->mOpaqueForAnimatedGeometryRootParent);
      } else {
        bool useChildrenVisible =
          itemType == DisplayItemType::TYPE_TRANSFORM &&
          (item->Frame()->IsPreserve3DLeaf() ||
           item->Frame()->HasPerspective());
        const nsIntRegion &visible = useChildrenVisible ?
          item->GetVisibleRectForChildren().ScaleToOutsidePixels(contentXScale, contentYScale, mAppUnitsPerDevPixel):
          itemVisibleRegion;

        SetOuterVisibleRegionForLayer(ownLayer, visible,
            layerContentsVisibleRect.width >= 0 ? &layerContentsVisibleRect : nullptr,
            useChildrenVisible);
      }
      if (itemType == DisplayItemType::TYPE_SCROLL_INFO_LAYER) {
        nsDisplayScrollInfoLayer* scrollItem = static_cast<nsDisplayScrollInfoLayer*>(item);
        newLayerEntry->mOpaqueForAnimatedGeometryRootParent = false;
        newLayerEntry->mBaseScrollMetadata =
            scrollItem->ComputeScrollMetadata(ownLayer->Manager(), mParameters);
      } else if ((itemType == DisplayItemType::TYPE_SUBDOCUMENT ||
                  itemType == DisplayItemType::TYPE_ZOOM ||
                  itemType == DisplayItemType::TYPE_RESOLUTION) &&
                 gfxPrefs::LayoutUseContainersForRootFrames())
      {
        newLayerEntry->mBaseScrollMetadata =
          static_cast<nsDisplaySubDocument*>(item)->ComputeScrollMetadata(ownLayer->Manager(), mParameters);
      }

      /**
       * No need to allocate geometry for items that aren't
       * part of a PaintedLayer.
       */
      if (ownLayer->Manager() == mLayerBuilder->GetRetainingLayerManager()) {
        oldData =
          mLayerBuilder->GetOldLayerForFrame(item->Frame(), item->GetPerFrameKey());
        mLayerBuilder->StoreDataForFrame(item, ownLayer, layerState, oldData);
      }
    } else {
      const bool backfaceHidden = item->In3DContextAndBackfaceIsHidden();
      const nsIFrame* referenceFrame = item->ReferenceFrame();

      PaintedLayerData* paintedLayerData = selectedPLD;

      if (!selectedPLD) {
        MOZ_ASSERT(marker != DisplayItemEntryType::POP_OPACITY);

        paintedLayerData =
          mPaintedLayerDataTree.FindPaintedLayerFor(animatedGeometryRoot, itemASR, layerClipChain,
                                                    itemVisibleRect, backfaceHidden,
                                                    [&](PaintedLayerData* aData) {
            NewPaintedLayerData(aData, animatedGeometryRoot, itemASR,
                                layerClipChain, scrollMetadataASR, topLeft,
                                referenceFrame, backfaceHidden);
        });
      }
      MOZ_ASSERT(paintedLayerData);

      if (itemType == DisplayItemType::TYPE_LAYER_EVENT_REGIONS) {
        nsDisplayLayerEventRegions* eventRegions =
          static_cast<nsDisplayLayerEventRegions*>(item);
        paintedLayerData->AccumulateEventRegions(this, eventRegions);
      } else if (itemType == DisplayItemType::TYPE_COMPOSITOR_HITTEST_INFO) {
        nsDisplayCompositorHitTestInfo* hitTestInfo =
          static_cast<nsDisplayCompositorHitTestInfo*>(item);
        paintedLayerData->AccumulateHitTestInfo(this, hitTestInfo);
      } else {
        paintedLayerData->Accumulate(this, item, itemVisibleRect, itemClip,
                                     layerState, aList, marker);

        if (!paintedLayerData->mLayer) {
          // Try to recycle the old layer of this display item.
          RefPtr<PaintedLayer> layer =
            AttemptToRecyclePaintedLayer(animatedGeometryRoot, item,
                                         topLeft, referenceFrame);
          if (layer) {
            paintedLayerData->mLayer = layer;

            PaintedDisplayItemLayerUserData* userData = GetPaintedDisplayItemLayerUserData(layer);
            paintedLayerData->mAssignedDisplayItems.SetCapacity(userData->mLastItemCount);

            NS_ASSERTION(FindIndexOfLayerIn(mNewChildLayers, layer) < 0,
                         "Layer already in list???");
            mNewChildLayers[paintedLayerData->mNewChildLayersIndex].mLayer = layer.forget();
          }
        }
      }

      if (marker == DisplayItemEntryType::PUSH_OPACITY) {
        selectedPLD = paintedLayerData;
      }

      if (marker == DisplayItemEntryType::POP_OPACITY ) {
        MOZ_ASSERT(selectedPLD);

        if (selectedPLD->mOpacityIndices.IsEmpty()) {
          selectedPLD = nullptr;
        }
      }
    }

    nsDisplayList* childItems = item->GetSameCoordinateSystemChildren();
    if (childItems && childItems->NeedsTransparentSurface()) {
      aList->SetNeedsTransparentSurface();
    }
  }

  MOZ_ASSERT(selectedPLD == nullptr);
}

void
ContainerState::InvalidateForLayerChange(nsDisplayItem* aItem,
                                         PaintedLayer* aNewLayer,
                                         DisplayItemData* aData)
{
  NS_ASSERTION(aItem->GetPerFrameKey(),
               "Display items that render using Thebes must have a key");
  Layer* oldLayer = aData ? aData->mLayer.get() : nullptr;
  if (aNewLayer != oldLayer && oldLayer) {
    // The item has changed layers.
    // Invalidate the old bounds in the old layer and new bounds in the new layer.
    PaintedLayer* t = oldLayer->AsPaintedLayer();
    if (t && aData->mGeometry) {
      // Note that whenever the layer's scale changes, we invalidate the whole thing,
      // so it doesn't matter whether we are using the old scale at last paint
      // or a new scale here
#ifdef MOZ_DUMP_PAINTING
      if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
        printf_stderr("Display item type %s(%p) changed layers %p to %p!\n", aItem->Name(), aItem->Frame(), t, aNewLayer);
      }
#endif
      InvalidatePostTransformRegion(t,
          aData->mGeometry->ComputeInvalidationRegion(),
          aData->mClip,
          mLayerBuilder->GetLastPaintOffset(t));
    }
    // Clear the old geometry so that invalidation thinks the item has been
    // added this paint.
    aData->mGeometry = nullptr;
  }
}

void
FrameLayerBuilder::ComputeGeometryChangeForItem(DisplayItemData* aData)
{
  nsDisplayItem *item = aData->mItem;
  PaintedLayer* paintedLayer = aData->mLayer->AsPaintedLayer();
  // If aData->mOptLayer is presence, means this item has been optimized to the separate
  // layer. Thus, skip geometry change calculation.
  if (aData->mOptLayer || !item || !paintedLayer) {
    aData->EndUpdate();
    return;
  }

  // If we're a reused display item, then we can't be invalid, so no need to
  // do an in-depth comparison. If we haven't previously stored geometry
  // for this item (if it was an active layer), then we can't skip this
  // yet.
  nsAutoPtr<nsDisplayItemGeometry> geometry;
  if (aData->mReusedItem && aData->mGeometry) {
    aData->EndUpdate();
    return;
  }

  PaintedDisplayItemLayerUserData* layerData =
    static_cast<PaintedDisplayItemLayerUserData*>(aData->mLayer->GetUserData(&gPaintedDisplayItemLayerUserData));
  nsPoint shift = layerData->mAnimatedGeometryRootOrigin - layerData->mLastAnimatedGeometryRootOrigin;

  const DisplayItemClip& clip = item->GetClip();

  // If the frame is marked as invalidated, and didn't specify a rect to invalidate then we want to
  // invalidate both the old and new bounds, otherwise we only want to invalidate the changed areas.
  // If we do get an invalid rect, then we want to add this on top of the change areas.
  nsRect invalid;
  nsRegion combined;
  if (!aData->mGeometry) {
    // This item is being added for the first time, invalidate its entire area.
    geometry = item->AllocateGeometry(mDisplayListBuilder);
    combined = clip.ApplyNonRoundedIntersection(geometry->ComputeInvalidationRegion());
#ifdef MOZ_DUMP_PAINTING
    if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
      printf_stderr("Display item type %s(%p) added to layer %p!\n", item->Name(), item->Frame(), aData->mLayer.get());
    }
#endif
  } else if (aData->mIsInvalid || (item->IsInvalid(invalid) && invalid.IsEmpty())) {
    // Layout marked item/frame as needing repainting (without an explicit rect), invalidate the entire old and new areas.
    geometry = item->AllocateGeometry(mDisplayListBuilder);
    combined = aData->mClip.ApplyNonRoundedIntersection(aData->mGeometry->ComputeInvalidationRegion());
    combined.MoveBy(shift);
    combined.Or(combined, clip.ApplyNonRoundedIntersection(geometry->ComputeInvalidationRegion()));
#ifdef MOZ_DUMP_PAINTING
    if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
      printf_stderr("Display item type %s(%p) (in layer %p) belongs to an invalidated frame!\n", item->Name(), item->Frame(), aData->mLayer.get());
    }
#endif
  } else {
    // Let the display item check for geometry changes and decide what needs to be
    // repainted.

    const nsRegion& changedFrameInvalidations = aData->GetChangedFrameInvalidations();
    aData->mGeometry->MoveBy(shift);
    item->ComputeInvalidationRegion(mDisplayListBuilder, aData->mGeometry, &combined);

    // Only allocate a new geometry object if something actually changed, otherwise the existing
    // one should be fine. We always reallocate for inactive layers, since these types don't
    // implement ComputeInvalidateRegion (and rely on the ComputeDifferences call in
    // AddPaintedDisplayItem instead).
    if (!combined.IsEmpty() || aData->mLayerState == LAYER_INACTIVE) {
      geometry = item->AllocateGeometry(mDisplayListBuilder);
    }
    aData->mClip.AddOffsetAndComputeDifference(shift, aData->mGeometry->ComputeInvalidationRegion(),
                                               clip, geometry ? geometry->ComputeInvalidationRegion() :
                                                                aData->mGeometry->ComputeInvalidationRegion(),
                                               &combined);

    // Add in any rect that the frame specified
    combined.Or(combined, invalid);
    combined.Or(combined, changedFrameInvalidations);

    // Restrict invalidation to the clipped region
    nsRegion clipRegion;
    if (clip.ComputeRegionInClips(&aData->mClip, shift, &clipRegion)) {
      combined.And(combined, clipRegion);
    }
#ifdef MOZ_DUMP_PAINTING
    if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
      if (!combined.IsEmpty()) {
        printf_stderr("Display item type %s(%p) (in layer %p) changed geometry!\n", item->Name(), item->Frame(), aData->mLayer.get());
      }
    }
#endif
  }
  if (!combined.IsEmpty()) {
    InvalidatePostTransformRegion(paintedLayer,
        combined.ScaleToOutsidePixels(layerData->mXScale, layerData->mYScale, layerData->mAppUnitsPerDevPixel),
        layerData->mTranslation);
  }

  aData->EndUpdate(geometry);
}

void
FrameLayerBuilder::AddPaintedDisplayItem(PaintedLayerData* aLayerData,
                                         AssignedDisplayItem& aItem,
                                         ContainerState& aContainerState,
                                         Layer* aLayer)
{
  PaintedLayer* layer = aLayerData->mLayer;
  PaintedDisplayItemLayerUserData* paintedData =
    static_cast<PaintedDisplayItemLayerUserData*>
      (layer->GetUserData(&gPaintedDisplayItemLayerUserData));
  RefPtr<BasicLayerManager> tempManager;
  nsIntRect intClip;
  bool hasClip = false;
  if (aItem.mLayerState != LAYER_NONE) {
    if (aItem.mDisplayItemData) {
      tempManager = aItem.mDisplayItemData->mInactiveManager;

      // We need to grab these before updating the DisplayItemData because it will overwrite them.
      nsRegion clip;
      if (aItem.mItem->GetClip().ComputeRegionInClips(&aItem.mDisplayItemData->GetClip(),
                                                      aLayerData->mAnimatedGeometryRootOffset - paintedData->mLastAnimatedGeometryRootOrigin,
                                                      &clip)) {
        intClip = clip.GetBounds().ScaleToOutsidePixels(paintedData->mXScale,
                                                        paintedData->mYScale,
                                                        paintedData->mAppUnitsPerDevPixel);
      }
    }
    if (!tempManager) {
      tempManager = new BasicLayerManager(BasicLayerManager::BLM_INACTIVE);
    }
  }

  if (layer->Manager() == mRetainingManager) {
    DisplayItemData *data = aItem.mDisplayItemData;
    if (data) {
      if (!data->mUsed) {
        data->BeginUpdate(layer, aItem.mLayerState, aItem.mItem, aItem.mReused, aItem.mMerged);
      }
    } else {
      data = StoreDataForFrame(aItem.mItem, layer, aItem.mLayerState, nullptr);
    }
    data->mInactiveManager = tempManager;
    // We optimized this PaintedLayer into a ColorLayer/ImageLayer. Store the optimized
    // layer here.
    if (aLayer != layer) {
      data->mOptLayer = aLayer;
      data->mItem = nullptr;
    }
  }

  if (tempManager) {
    FLB_LOG_PAINTED_LAYER_DECISION(aLayerData, "Creating nested FLB for item %p\n", aItem.mItem);
    FrameLayerBuilder* layerBuilder = new FrameLayerBuilder();
    layerBuilder->Init(mDisplayListBuilder, tempManager, aLayerData, true,
                       &aItem.mItem->GetClip());

    tempManager->BeginTransaction();
    if (mRetainingManager) {
      layerBuilder->DidBeginRetainedLayerTransaction(tempManager);
    }

    UniquePtr<LayerProperties> props(LayerProperties::CloneFrom(tempManager->GetRoot()));
    RefPtr<Layer> tmpLayer =
      aItem.mItem->BuildLayer(mDisplayListBuilder, tempManager, ContainerLayerParameters());
    // We have no easy way of detecting if this transaction will ever actually get finished.
    // For now, I've just silenced the warning with nested transactions in BasicLayers.cpp
    if (!tmpLayer) {
      tempManager->EndTransaction(nullptr, nullptr);
      tempManager->SetUserData(&gLayerManagerLayerBuilder, nullptr);
      aItem.mItem = nullptr;
      return;
    }

    bool snap;
    nsRect visibleRect =
      aItem.mItem->GetVisibleRect().Intersect(aItem.mItem->GetBounds(mDisplayListBuilder, &snap));
    nsIntRegion rgn = visibleRect.ToOutsidePixels(paintedData->mAppUnitsPerDevPixel);

    // Convert the visible rect to a region and give the item
    // a chance to try restrict it further.
    nsRegion tightBounds = aItem.mItem->GetTightBounds(mDisplayListBuilder, &snap);
    if (!tightBounds.IsEmpty()) {
      rgn.AndWith(tightBounds.ToOutsidePixels(paintedData->mAppUnitsPerDevPixel));
    }
    SetOuterVisibleRegion(tmpLayer, &rgn);

    // If BuildLayer didn't call BuildContainerLayerFor, then our new layer won't have been
    // stored in layerBuilder. Manually add it now.
    if (mRetainingManager) {
#ifdef DEBUG_DISPLAY_ITEM_DATA
      LayerManagerData* parentLmd = static_cast<LayerManagerData*>
        (layer->Manager()->GetUserData(&gLayerManagerUserData));
      LayerManagerData* lmd = static_cast<LayerManagerData*>
        (tempManager->GetUserData(&gLayerManagerUserData));
      lmd->mParent = parentLmd;
#endif
      DisplayItemData* data = layerBuilder->GetDisplayItemDataForManager(aItem.mItem, tempManager);
      layerBuilder->StoreDataForFrame(aItem.mItem, tmpLayer, LAYER_ACTIVE, data);
    }

    tempManager->SetRoot(tmpLayer);
    layerBuilder->WillEndTransaction();
    tempManager->AbortTransaction();

#ifdef MOZ_DUMP_PAINTING
    if (gfxUtils::DumpDisplayList() || gfxEnv::DumpPaint()) {
      fprintf_stderr(gfxUtils::sDumpPaintFile, "Basic layer tree for painting contents of display item %s(%p):\n", aItem.mItem->Name(), aItem.mItem->Frame());
      std::stringstream stream;
      tempManager->Dump(stream, "", gfxEnv::DumpPaintToFile());
      fprint_stderr(gfxUtils::sDumpPaintFile, stream);  // not a typo, fprint_stderr declared in LayersLogging.h
    }
#endif

    nsIntPoint offset = GetLastPaintOffset(layer) - GetTranslationForPaintedLayer(layer);
    props->MoveBy(-offset);
    // Effective transforms are needed by ComputeDifferences().
    tmpLayer->ComputeEffectiveTransforms(Matrix4x4());
    nsIntRegion invalid;
    if (!props->ComputeDifferences(tmpLayer, invalid, nullptr)) {
      nsRect visible = aItem.mItem->Frame()->GetVisualOverflowRect();
      invalid = visible.ToOutsidePixels(paintedData->mAppUnitsPerDevPixel);
    }
    if (aItem.mLayerState == LAYER_SVG_EFFECTS) {
      invalid = nsSVGIntegrationUtils::AdjustInvalidAreaForSVGEffects(aItem.mItem->Frame(),
                                                                      aItem.mItem->ToReferenceFrame(),
                                                                      invalid);
    }
    if (!invalid.IsEmpty()) {
#ifdef MOZ_DUMP_PAINTING
      if (nsLayoutUtils::InvalidationDebuggingIsEnabled()) {
        printf_stderr("Inactive LayerManager(%p) for display item %s(%p) has an invalid region - invalidating layer %p\n", tempManager.get(), aItem.mItem->Name(), aItem.mItem->Frame(), layer);
      }
#endif
      invalid.ScaleRoundOut(paintedData->mXScale, paintedData->mYScale);

      if (hasClip) {
        invalid.And(invalid, intClip);
      }

      InvalidatePostTransformRegion(layer, invalid,
                                    GetTranslationForPaintedLayer(layer));
    }
  }
  aItem.mInactiveLayerManager = tempManager;
}

DisplayItemData*
FrameLayerBuilder::StoreDataForFrame(nsDisplayItem* aItem, Layer* aLayer,
                                     LayerState aState, DisplayItemData* aData)
{
  if (aData) {
    if (!aData->mUsed) {
      aData->BeginUpdate(aLayer, aState, aItem);
    }
    return aData;
  }

  LayerManagerData* lmd = static_cast<LayerManagerData*>
    (mRetainingManager->GetUserData(&gLayerManagerUserData));

  RefPtr<DisplayItemData> data =
    new (aItem->Frame()->PresContext()) DisplayItemData(lmd, aItem->GetPerFrameKey(), aLayer);

  if (!data->HasMergedFrames()) {
    aItem->SetDisplayItemData(data);
  }

  data->BeginUpdate(aLayer, aState, aItem);

  lmd->mDisplayItems.PutEntry(data);
  return data;
}

void
FrameLayerBuilder::StoreDataForFrame(nsIFrame* aFrame,
                                     uint32_t aDisplayItemKey,
                                     Layer* aLayer,
                                     LayerState aState)
{
  DisplayItemData* oldData = GetDisplayItemData(aFrame, aDisplayItemKey);
  if (oldData && oldData->mFrameList.Length() == 1) {
    oldData->BeginUpdate(aLayer, aState);
    return;
  }

  LayerManagerData* lmd = static_cast<LayerManagerData*>
    (mRetainingManager->GetUserData(&gLayerManagerUserData));

  RefPtr<DisplayItemData> data =
    new (aFrame->PresContext()) DisplayItemData(lmd, aDisplayItemKey, aLayer, aFrame);

  data->BeginUpdate(aLayer, aState);

  lmd->mDisplayItems.PutEntry(data);
}

AssignedDisplayItem::AssignedDisplayItem(nsDisplayItem* aItem,
                                         LayerState aLayerState,
                                         DisplayItemData* aData,
                                         DisplayItemEntryType aType,
                                         const bool aHasOpacity)
  : mItem(aItem)
  , mLayerState(aLayerState)
  , mDisplayItemData(aData)
  , mReused(aItem->IsReused())
  , mMerged(aItem->HasMergedFrames())
  , mType(aType)
  , mHasOpacity(aHasOpacity)
{}

AssignedDisplayItem::~AssignedDisplayItem()
{
  if (mInactiveLayerManager) {
    mInactiveLayerManager->SetUserData(&gLayerManagerLayerBuilder, nullptr);
  }
}

nsIntPoint
FrameLayerBuilder::GetLastPaintOffset(PaintedLayer* aLayer)
{
  PaintedDisplayItemLayerUserData* layerData = GetPaintedDisplayItemLayerUserData(aLayer);
  MOZ_ASSERT(layerData);
  if (layerData->mHasExplicitLastPaintOffset) {
    return layerData->mLastPaintOffset;
  }
  return GetTranslationForPaintedLayer(aLayer);
}

bool
FrameLayerBuilder::CheckInLayerTreeCompressionMode()
{
  if (mInLayerTreeCompressionMode) {
    return true;
  }

  // If we wanted to be in layer tree compression mode, but weren't, then scheduled
  // a delayed repaint where we will be.
  mRootPresContext->PresShell()->GetRootFrame()->SchedulePaint(nsIFrame::PAINT_DELAYED_COMPRESS, false);

  return false;
}

void
ContainerState::CollectOldLayers()
{
  for (Layer* layer = mContainerLayer->GetFirstChild(); layer;
       layer = layer->GetNextSibling()) {
    NS_ASSERTION(!layer->HasUserData(&gMaskLayerUserData),
                 "Mask layers should not be part of the layer tree.");
    if (layer->HasUserData(&gPaintedDisplayItemLayerUserData)) {
      NS_ASSERTION(layer->AsPaintedLayer(), "Wrong layer type");
      mPaintedLayersAvailableForRecycling.PutEntry(static_cast<PaintedLayer*>(layer));
    }

    if (Layer* maskLayer = layer->GetMaskLayer()) {
      NS_ASSERTION(maskLayer->GetType() == Layer::TYPE_IMAGE,
                   "Could not recycle mask layer, unsupported layer type.");
      mRecycledMaskImageLayers.Put(MaskLayerKey(layer, Nothing()), static_cast<ImageLayer*>(maskLayer));
    }
    for (size_t i = 0; i < layer->GetAncestorMaskLayerCount(); i++) {
      Layer* maskLayer = layer->GetAncestorMaskLayerAt(i);

      NS_ASSERTION(maskLayer->GetType() == Layer::TYPE_IMAGE,
                   "Could not recycle mask layer, unsupported layer type.");
      mRecycledMaskImageLayers.Put(MaskLayerKey(layer, Some(i)), static_cast<ImageLayer*>(maskLayer));
    }
  }
}

struct OpaqueRegionEntry {
  AnimatedGeometryRoot* mAnimatedGeometryRoot;
  const ActiveScrolledRoot* mASR;
  nsIntRegion mOpaqueRegion;
};

static OpaqueRegionEntry*
FindOpaqueRegionEntry(nsTArray<OpaqueRegionEntry>& aEntries,
                      AnimatedGeometryRoot* aAnimatedGeometryRoot,
                      const ActiveScrolledRoot* aASR)
{
  for (uint32_t i = 0; i < aEntries.Length(); ++i) {
    OpaqueRegionEntry* d = &aEntries[i];
    if (d->mAnimatedGeometryRoot == aAnimatedGeometryRoot &&
        d->mASR == aASR) {
      return d;
    }
  }
  return nullptr;
}

static const ActiveScrolledRoot*
FindDirectChildASR(const ActiveScrolledRoot* aParent,
                   const ActiveScrolledRoot* aDescendant)
{
  MOZ_ASSERT(aDescendant, "can't start at the root when looking for a child");
  MOZ_ASSERT(ActiveScrolledRoot::IsAncestor(aParent, aDescendant));
  const ActiveScrolledRoot* directChild = aDescendant;
  while (directChild->mParent != aParent) {
    directChild = directChild->mParent;
    MOZ_RELEASE_ASSERT(directChild, "this must not be null");
  }
  return directChild;
}

static void
FixUpFixedPositionLayer(Layer* aLayer,
                        const ActiveScrolledRoot* aTargetASR,
                        const ActiveScrolledRoot* aLeafScrollMetadataASR,
                        const ActiveScrolledRoot* aContainerScrollMetadataASR,
                        const ActiveScrolledRoot* aContainerCompositorASR,
                        bool aIsFixedToRootScrollFrame)
{
  if (!aLayer->GetIsFixedPosition()) {
    return;
  }

  // Analyze ASRs to figure out if we need to fix up fixedness annotations on
  // the layer. Fixed annotations are required in multiple cases:
  //  - Sometimes we set scroll metadata on a layer for a scroll frame that we
  //    don't want the layer to be moved by. (We have to do this if there is a
  //    scrolled clip that is moved by that scroll frame.) So we set the fixed
  //    annotation so that the compositor knows that it should ignore that
  //    scroll metadata when determining the layer's position.
  //  - Sometimes there is a scroll meta data on aLayer's parent layer for a
  //    scroll frame that we don't want aLayer to be moved by. The most common
  //    way for this to happen is with containerful root scrolling, where the
  //    scroll metadata for the root scroll frame is on a container layer that
  //    wraps the whole document's contents.
  //  - Sometimes it's just needed for hit testing, i.e. figuring out what
  //    scroll frame should be scrolled by events over the layer.
  // A fixed layer needs to be annotated with the scroll ID of the scroll frame
  // that it is *fixed with respect to*, i.e. the outermost scroll frame which
  // does not move the layer. nsDisplayFixedPosition only ever annotates layers
  // with the scroll ID of the presshell's root scroll frame, which is
  // sometimes the wrong thing to do, so we correct it here. Specifically,
  // it's the wrong thing to do if the fixed frame's containing block is a
  // transformed frame - in that case, the fixed frame needs to scroll along
  // with the transformed frame instead of being fixed with respect to the rsf.
  // (It would be nice to compute the annotation only in one place and get it
  // right, instead of fixing it up after the fact like this, but this will
  // need to do for now.)
  // compositorASR is the ASR that the layer would move with on the compositor
  // if there were no fixed annotation on it.
  const ActiveScrolledRoot* compositorASR =
    aLeafScrollMetadataASR == aContainerScrollMetadataASR
      ? aContainerCompositorASR
      : aLeafScrollMetadataASR;

  // The goal of the annotation is to have the layer move with aTargetASR.
  if (compositorASR && aTargetASR != compositorASR) {
    // Mark this layer as fixed with respect to the child scroll frame of aTargetASR.
    aLayer->SetFixedPositionData(
      FindDirectChildASR(aTargetASR, compositorASR)->GetViewId(),
      aLayer->GetFixedPositionAnchor(),
      aLayer->GetFixedPositionSides());
  } else {
    // Remove the fixed annotation from the layer, unless this layers is fixed
    // to the document's root scroll frame - in that case, the annotation is
    // needed for hit testing, because fixed layers in iframes should scroll
    // the iframe, even though their position is not affected by scrolling in
    // the iframe. (The APZ hit testing code has a special case for this.)
    // nsDisplayFixedPosition has annotated this layer with the document's
    // root scroll frame's scroll id.
    aLayer->SetIsFixedPosition(aIsFixedToRootScrollFrame);
  }
}

void
ContainerState::SetupScrollingMetadata(NewLayerEntry* aEntry)
{
  if (!mBuilder->IsPaintingToWindow()) {
    // async scrolling not possible, and async scrolling info not computed
    // for this paint.
    return;
  }

  const ActiveScrolledRoot* startASR = aEntry->mScrollMetadataASR;
  const ActiveScrolledRoot* stopASR = mContainerScrollMetadataASR;
  if (!ActiveScrolledRoot::IsAncestor(stopASR, startASR)) {
    if (ActiveScrolledRoot::IsAncestor(startASR, stopASR)) {
      // startASR and stopASR are in the same branch of the ASR tree, but
      // startASR is closer to the root. Just start at stopASR so that the loop
      // below doesn't actually do anything.
      startASR = stopASR;
    } else {
      // startASR and stopASR are in different branches of the
      // ASR tree. Find a common ancestor and make that the stopASR.
      // This can happen when there's a scrollable frame inside a fixed layer
      // which has a scrolled clip. As far as scroll metadata is concerned,
      // the scroll frame's scroll metadata will be a child of the scroll ID
      // that scrolls the clip on the fixed layer. But as far as ASRs are
      // concerned, those two ASRs are siblings, parented to the ASR of the
      // fixed layer.
      do {
        stopASR = stopASR->mParent;
      } while (!ActiveScrolledRoot::IsAncestor(stopASR, startASR));
    }
  }

  FixUpFixedPositionLayer(aEntry->mLayer, aEntry->mASR, startASR,
                          mContainerScrollMetadataASR, mContainerCompositorASR,
                          aEntry->mIsFixedToRootScrollFrame);

  AutoTArray<ScrollMetadata,2> metricsArray;
  if (aEntry->mBaseScrollMetadata) {
    metricsArray.AppendElement(*aEntry->mBaseScrollMetadata);

    // The base FrameMetrics was not computed by the nsIScrollableframe, so it
    // should not have a mask layer.
    MOZ_ASSERT(!aEntry->mBaseScrollMetadata->HasMaskLayer());
  }

  // Any extra mask layers we need to attach to ScrollMetadatas.
  // The list may already contain an entry added for the layer's scrolled clip
  // so add to it rather than overwriting it (we clear the list when recycling
  // a layer).
  nsTArray<RefPtr<Layer>> maskLayers(aEntry->mLayer->GetAllAncestorMaskLayers());

  // Iterate over the ASR chain and create the corresponding scroll metadatas.
  // This loop is slightly tricky because the scrollframe-to-clip relationship
  // is reversed between DisplayItemClipChain and ScrollMetadata:
  //  - DisplayItemClipChain associates the clip with the scroll frame that
  //    this clip is *moved by*, i.e. the clip is moving inside the scroll
  //    frame.
  //  - ScrollMetaData associates the scroll frame with the clip that's
  //    *just outside* the scroll frame, i.e. not moved by the scroll frame
  //    itself.
  // This discrepancy means that the leaf clip item of the clip chain is never
  // applied to any scroll meta data. Instead, it was applied earlier as the
  // layer's clip (or fused with the painted layer contents), or it was applied
  // as a ScrolledClip on the layer.
  const DisplayItemClipChain* clipChain = aEntry->mClipChain;

  for (const ActiveScrolledRoot* asr = startASR; asr != stopASR; asr = asr->mParent) {
    if (!asr) {
      MOZ_ASSERT_UNREACHABLE("Should have encountered stopASR on the way up.");
      break;
    }
    if (clipChain && clipChain->mASR == asr) {
      clipChain = clipChain->mParent;
    }

    nsIScrollableFrame* scrollFrame = asr->mScrollableFrame;
    const DisplayItemClip* clip =
      (clipChain && clipChain->mASR == asr->mParent) ? &clipChain->mClip : nullptr;

    scrollFrame->ClipLayerToDisplayPort(aEntry->mLayer, clip, mParameters);

    Maybe<ScrollMetadata> metadata;
    if (mCachedScrollMetadata.mASR == asr &&
        mCachedScrollMetadata.mClip == clip) {
      metadata = mCachedScrollMetadata.mMetadata;
    } else {
      metadata = scrollFrame->ComputeScrollMetadata(aEntry->mLayer->Manager(),
            mContainerReferenceFrame, mParameters, clip);
      mCachedScrollMetadata.mASR = asr;
      mCachedScrollMetadata.mClip = clip;
      mCachedScrollMetadata.mMetadata = metadata;
    }

    if (!metadata) {
      continue;
    }

    if (clip &&
        clip->HasClip() &&
        clip->GetRoundedRectCount() > 0)
    {
      // The clip in between this scrollframe and its ancestor scrollframe
      // requires a mask layer. Since this mask layer should not move with
      // the APZC associated with this FrameMetrics, we attach the mask
      // layer as an additional, separate clip.
      Maybe<size_t> nextIndex = Some(maskLayers.Length());
      RefPtr<Layer> maskLayer =
        CreateMaskLayer(aEntry->mLayer, *clip, nextIndex);
      if (maskLayer) {
        MOZ_ASSERT(metadata->HasScrollClip());
        metadata->ScrollClip().SetMaskLayerIndex(nextIndex);
        maskLayers.AppendElement(maskLayer);
      }
    }

    metricsArray.AppendElement(*metadata);
  }

  // Watch out for FrameMetrics copies in profiles
  aEntry->mLayer->SetScrollMetadata(metricsArray);
  aEntry->mLayer->SetAncestorMaskLayers(maskLayers);
}

static inline Maybe<ParentLayerIntRect>
GetStationaryClipInContainer(Layer* aLayer)
{
  if (size_t metricsCount = aLayer->GetScrollMetadataCount()) {
    return aLayer->GetScrollMetadata(metricsCount - 1).GetClipRect();
  }
  return aLayer->GetClipRect();
}

void
ContainerState::PostprocessRetainedLayers(nsIntRegion* aOpaqueRegionForContainer)
{
  AutoTArray<OpaqueRegionEntry,4> opaqueRegions;
  bool hideAll = false;
  int32_t opaqueRegionForContainer = -1;

  for (int32_t i = mNewChildLayers.Length() - 1; i >= 0; --i) {
    NewLayerEntry* e = &mNewChildLayers.ElementAt(i);
    if (!e->mLayer) {
      continue;
    }

    OpaqueRegionEntry* data = FindOpaqueRegionEntry(opaqueRegions, e->mAnimatedGeometryRoot, e->mASR);

    SetupScrollingMetadata(e);

    if (hideAll) {
      e->mVisibleRegion.SetEmpty();
    } else if (!e->mLayer->IsScrollbarContainer()) {
      Maybe<ParentLayerIntRect> clipRect = GetStationaryClipInContainer(e->mLayer);
      if (clipRect && opaqueRegionForContainer >= 0 &&
          opaqueRegions[opaqueRegionForContainer].mOpaqueRegion.Contains(clipRect->ToUnknownRect())) {
        e->mVisibleRegion.SetEmpty();
      } else if (data) {
        e->mVisibleRegion.Sub(e->mVisibleRegion, data->mOpaqueRegion);
      }
    }

    SetOuterVisibleRegionForLayer(e->mLayer,
                                  e->mVisibleRegion,
                                  e->mLayerContentsVisibleRect.width >= 0 ? &e->mLayerContentsVisibleRect : nullptr,
                                  e->mUntransformedVisibleRegion);

    if (!e->mOpaqueRegion.IsEmpty()) {
      AnimatedGeometryRoot* animatedGeometryRootToCover = e->mAnimatedGeometryRoot;
      const ActiveScrolledRoot* asrToCover = e->mASR;
      if (e->mOpaqueForAnimatedGeometryRootParent &&
          e->mAnimatedGeometryRoot->mParentAGR == mContainerAnimatedGeometryRoot) {
        animatedGeometryRootToCover = mContainerAnimatedGeometryRoot;
        asrToCover = mContainerASR;
        data = FindOpaqueRegionEntry(opaqueRegions, animatedGeometryRootToCover, asrToCover);
      }

      if (!data) {
        if (animatedGeometryRootToCover == mContainerAnimatedGeometryRoot &&
            asrToCover == mContainerASR) {
          NS_ASSERTION(opaqueRegionForContainer == -1, "Already found it?");
          opaqueRegionForContainer = opaqueRegions.Length();
        }
        data = opaqueRegions.AppendElement();
        data->mAnimatedGeometryRoot = animatedGeometryRootToCover;
        data->mASR = asrToCover;
      }

      nsIntRegion clippedOpaque = e->mOpaqueRegion;
      Maybe<ParentLayerIntRect> clipRect = e->mLayer->GetCombinedClipRect();
      if (clipRect) {
        clippedOpaque.AndWith(clipRect->ToUnknownRect());
      }
      if (e->mLayer->GetScrolledClip()) {
        // The clip can move asynchronously, so we can't rely on opaque parts
        // staying visible.
        clippedOpaque.SetEmpty();
      } else if (e->mHideAllLayersBelow) {
        hideAll = true;
      }
      data->mOpaqueRegion.Or(data->mOpaqueRegion, clippedOpaque);
    }

    if (e->mLayer->GetType() == Layer::TYPE_READBACK) {
      // ReadbackLayers need to accurately read what's behind them. So,
      // we don't want to do any occlusion culling of layers behind them.
      // Theoretically we could just punch out the ReadbackLayer's rectangle
      // from all mOpaqueRegions, but that's probably not worth doing.
      opaqueRegions.Clear();
      opaqueRegionForContainer = -1;
    }
  }

  if (opaqueRegionForContainer >= 0) {
    aOpaqueRegionForContainer->Or(*aOpaqueRegionForContainer,
        opaqueRegions[opaqueRegionForContainer].mOpaqueRegion);
  }
}

void
ContainerState::Finish(uint32_t* aTextContentFlags,
                       const nsIntRect& aContainerPixelBounds,
                       nsDisplayList* aChildItems)
{
  mPaintedLayerDataTree.Finish();

  if (!mParameters.mForEventsAndPluginsOnly && !gfxPrefs::LayoutUseContainersForRootFrames()) {
    // Bug 1336544 tracks re-enabling this assertion in the
    // gfxPrefs::LayoutUseContainersForRootFrames() case.
    NS_ASSERTION(mContainerBounds.IsEqualInterior(mAccumulatedChildBounds),
                 "Bounds computation mismatch");
  }

  if (mLayerBuilder->IsBuildingRetainedLayers()) {
    nsIntRegion containerOpaqueRegion;
    PostprocessRetainedLayers(&containerOpaqueRegion);
    if (containerOpaqueRegion.Contains(aContainerPixelBounds)) {
      aChildItems->SetIsOpaque();
    }
  }

  uint32_t textContentFlags = 0;

  // Make sure that current/existing layers are added to the parent and are
  // in the correct order.
  Layer* layer = nullptr;
  Layer* prevChild = nullptr;
  for (uint32_t i = 0; i < mNewChildLayers.Length(); ++i, prevChild = layer) {
    if (!mNewChildLayers[i].mLayer) {
      continue;
    }

    layer = mNewChildLayers[i].mLayer;

    if (!layer->GetVisibleRegion().IsEmpty()) {
      textContentFlags |=
        layer->GetContentFlags() & (Layer::CONTENT_COMPONENT_ALPHA |
                                    Layer::CONTENT_COMPONENT_ALPHA_DESCENDANT |
                                    Layer::CONTENT_DISABLE_FLATTENING);
    }

    if (!layer->GetParent()) {
      // This is not currently a child of the container, so just add it
      // now.
      mContainerLayer->InsertAfter(layer, prevChild);
    } else {
      NS_ASSERTION(layer->GetParent() == mContainerLayer,
                   "Layer shouldn't be the child of some other container");
      if (layer->GetPrevSibling() != prevChild) {
        mContainerLayer->RepositionChild(layer, prevChild);
      }
    }
  }

  // Remove old layers that have become unused.
  if (!layer) {
    layer = mContainerLayer->GetFirstChild();
  } else {
    layer = layer->GetNextSibling();
  }
  while (layer) {
    Layer *layerToRemove = layer;
    layer = layer->GetNextSibling();
    mContainerLayer->RemoveChild(layerToRemove);
  }

  *aTextContentFlags = textContentFlags;
}

static void RestrictScaleToMaxLayerSize(Size& aScale,
                                        const nsRect& aVisibleRect,
                                        nsIFrame* aContainerFrame,
                                        Layer* aContainerLayer)
{
  if (!aContainerLayer->Manager()->IsWidgetLayerManager()) {
    return;
  }

  nsIntRect pixelSize =
    aVisibleRect.ScaleToOutsidePixels(aScale.width, aScale.height,
                                      aContainerFrame->PresContext()->AppUnitsPerDevPixel());

  int32_t maxLayerSize = aContainerLayer->GetMaxLayerSize();

  if (pixelSize.width > maxLayerSize) {
    float scale = (float)pixelSize.width / maxLayerSize;
    scale = gfxUtils::ClampToScaleFactor(scale);
    aScale.width /= scale;
  }
  if (pixelSize.height > maxLayerSize) {
    float scale = (float)pixelSize.height / maxLayerSize;
    scale = gfxUtils::ClampToScaleFactor(scale);
    aScale.height /= scale;
  }
}

static nsSize
ComputeDesiredDisplaySizeForAnimation(nsIFrame* aContainerFrame)
{
  // Use the size of the nearest widget as the maximum size.  This
  // is important since it might be a popup that is bigger than the
  // pres context's size.
  nsPresContext* presContext = aContainerFrame->PresContext();
  nsIWidget* widget = aContainerFrame->GetNearestWidget();
  if (widget) {
    return LayoutDevicePixel::ToAppUnits(widget->GetClientSize(),
                                         presContext->AppUnitsPerDevPixel());
  } else {
    return presContext->GetVisibleArea().Size();
  }
}

static bool
ChooseScaleAndSetTransform(FrameLayerBuilder* aLayerBuilder,
                           nsDisplayListBuilder* aDisplayListBuilder,
                           nsIFrame* aContainerFrame,
                           nsDisplayItem* aContainerItem,
                           const nsRect& aVisibleRect,
                           const Matrix4x4* aTransform,
                           const ContainerLayerParameters& aIncomingScale,
                           ContainerLayer* aLayer,
                           ContainerLayerParameters& aOutgoingScale)
{
  nsIntPoint offset;

  Matrix4x4 transform =
    Matrix4x4::Scaling(aIncomingScale.mXScale, aIncomingScale.mYScale, 1.0);
  if (aTransform) {
    // aTransform is applied first, then the scale is applied to the result
    transform = (*aTransform)*transform;
    // Set any matrix entries close to integers to be those exact integers.
    // This protects against floating-point inaccuracies causing problems
    // in the checks below.
    // We use the fixed epsilon version here because we don't want the nudging
    // to depend on the scroll position.
    transform.NudgeToIntegersFixedEpsilon();
  }
  Matrix transform2d;
  if (aContainerFrame &&
      aLayerBuilder->GetContainingPaintedLayerData() &&
      (!aTransform || (aTransform->Is2D(&transform2d) &&
                       !transform2d.HasNonTranslation()))) {
    // When we have an inactive ContainerLayer, translate the container by the offset to the
    // reference frame (and offset all child layers by the reverse) so that the coordinate
    // space of the child layers isn't affected by scrolling.
    // This gets confusing for complicated transform (since we'd have to compute the scale
    // factors for the matrix), so we don't bother. Any frames that are building an nsDisplayTransform
    // for a css transform would have 0,0 as their offset to the reference frame, so this doesn't
    // matter.
    nsPoint appUnitOffset = aDisplayListBuilder->ToReferenceFrame(aContainerFrame);
    nscoord appUnitsPerDevPixel = aContainerFrame->PresContext()->AppUnitsPerDevPixel();
    offset = nsIntPoint(
        NS_lround(NSAppUnitsToDoublePixels(appUnitOffset.x, appUnitsPerDevPixel)*aIncomingScale.mXScale),
        NS_lround(NSAppUnitsToDoublePixels(appUnitOffset.y, appUnitsPerDevPixel)*aIncomingScale.mYScale));
  }
  transform.PostTranslate(offset.x + aIncomingScale.mOffset.x,
                          offset.y + aIncomingScale.mOffset.y,
                          0);

  if (transform.IsSingular()) {
    return false;
  }

  bool canDraw2D = transform.CanDraw2D(&transform2d);
  Size scale;
  // XXX Should we do something for 3D transforms?
  if (canDraw2D &&
      !aContainerFrame->Combines3DTransformWithAncestors() &&
      !aContainerFrame->HasPerspective()) {
    // If the container's transform is animated off main thread, fix a suitable scale size
    // for animation
    if (aContainerItem &&
        aContainerItem->GetType() == DisplayItemType::TYPE_TRANSFORM &&
        EffectCompositor::HasAnimationsForCompositor(
          aContainerFrame, eCSSProperty_transform)) {
      nsSize displaySize = ComputeDesiredDisplaySizeForAnimation(aContainerFrame);
      // compute scale using the animation on the container, taking ancestors in to account
      nsSize scaledVisibleSize = nsSize(aVisibleRect.Width() * aIncomingScale.mXScale,
                                        aVisibleRect.Height() * aIncomingScale.mYScale);
      scale = nsLayoutUtils::ComputeSuitableScaleForAnimation(
                aContainerFrame, scaledVisibleSize,
                displaySize);
      // multiply by the scale inherited from ancestors--we use a uniform
      // scale factor to prevent blurring when the layer is rotated.
      float incomingScale = std::max(aIncomingScale.mXScale, aIncomingScale.mYScale);
      scale.width *= incomingScale;
      scale.height *= incomingScale;
    } else {
      // Scale factors are normalized to a power of 2 to reduce the number of resolution changes
      scale = transform2d.ScaleFactors(true);
      // For frames with a changing scale transform round scale factors up to
      // nearest power-of-2 boundary so that we don't keep having to redraw
      // the content as it scales up and down. Rounding up to nearest
      // power-of-2 boundary ensures we never scale up, only down --- avoiding
      // jaggies. It also ensures we never scale down by more than a factor of 2,
      // avoiding bad downscaling quality.
      Matrix frameTransform;
      if (ActiveLayerTracker::IsScaleSubjectToAnimation(aContainerFrame)) {
        scale.width = gfxUtils::ClampToScaleFactor(scale.width);
        scale.height = gfxUtils::ClampToScaleFactor(scale.height);

        // Limit animated scale factors to not grow excessively beyond the display size.
        nsSize maxScale(4, 4);
        if (!aVisibleRect.IsEmpty()) {
          nsSize displaySize = ComputeDesiredDisplaySizeForAnimation(aContainerFrame);
          maxScale = Max(maxScale, displaySize / aVisibleRect.Size());
        }
        if (scale.width > maxScale.width) {
          scale.width = gfxUtils::ClampToScaleFactor(maxScale.width, true);
        }
        if (scale.height > maxScale.height) {
          scale.height = gfxUtils::ClampToScaleFactor(maxScale.height, true);
        }
      } else {
        // XXX Do we need to move nearly-integer values to integers here?
      }
    }
    // If the scale factors are too small, just use 1.0. The content is being
    // scaled out of sight anyway.
    if (fabs(scale.width) < 1e-8 || fabs(scale.height) < 1e-8) {
      scale = Size(1.0, 1.0);
    }
    // If this is a transform container layer, then pre-rendering might
    // mean we try render a layer bigger than the max texture size. If we have
    // tiling, that's not a problem, since we'll automatically choose a tiled
    // layer for layers of that size. If not, we need to apply clamping to
    // prevent this.
    if (aTransform && !gfxPrefs::LayersTilesEnabled()) {
      RestrictScaleToMaxLayerSize(scale, aVisibleRect, aContainerFrame, aLayer);
    }
  } else {
    scale = Size(1.0, 1.0);
  }

  // Store the inverse of our resolution-scale on the layer
  aLayer->SetBaseTransform(transform);
  aLayer->SetPreScale(1.0f/scale.width,
                      1.0f/scale.height);
  aLayer->SetInheritedScale(aIncomingScale.mXScale,
                            aIncomingScale.mYScale);

  aOutgoingScale =
    ContainerLayerParameters(scale.width, scale.height, -offset, aIncomingScale);
  if (aTransform) {
    aOutgoingScale.mInTransformedSubtree = true;
    if (ActiveLayerTracker::IsStyleAnimated(aDisplayListBuilder, aContainerFrame,
                                            eCSSProperty_transform)) {
      aOutgoingScale.mInActiveTransformedSubtree = true;
    }
  }
  if ((aLayerBuilder->IsBuildingRetainedLayers() &&
       (!canDraw2D || transform2d.HasNonIntegerTranslation())) ||
      aContainerFrame->Extend3DContext() ||
      aContainerFrame->Combines3DTransformWithAncestors() ||
      // For async transform animation, the value would be changed at
      // any time, integer translation is not always true.
      aContainerFrame->HasAnimationOfTransform()) {
    aOutgoingScale.mDisableSubpixelAntialiasingInDescendants = true;
  }
  return true;
}

already_AddRefed<ContainerLayer>
FrameLayerBuilder::BuildContainerLayerFor(nsDisplayListBuilder* aBuilder,
                                          LayerManager* aManager,
                                          nsIFrame* aContainerFrame,
                                          nsDisplayItem* aContainerItem,
                                          nsDisplayList* aChildren,
                                          const ContainerLayerParameters& aParameters,
                                          const Matrix4x4* aTransform,
                                          uint32_t aFlags)
{
  uint32_t containerDisplayItemKey =
    aContainerItem ? aContainerItem->GetPerFrameKey() : 0;
  NS_ASSERTION(aContainerFrame, "Container display items here should have a frame");
  NS_ASSERTION(!aContainerItem ||
               aContainerItem->Frame() == aContainerFrame,
               "Container display item must match given frame");

  if (!aParameters.mXScale || !aParameters.mYScale) {
    return nullptr;
  }

  RefPtr<ContainerLayer> containerLayer;
  if (aManager == mRetainingManager) {
    // Using GetOldLayerFor will search merged frames, as well as the underlying
    // frame. The underlying frame can change when a page scrolls, so this
    // avoids layer recreation in the situation that a new underlying frame is
    // picked for a layer.
    Layer* oldLayer = nullptr;
    if (aContainerItem) {
      oldLayer = GetOldLayerFor(aContainerItem);
    } else {
      DisplayItemData *data = GetOldLayerForFrame(aContainerFrame, containerDisplayItemKey);
      if (data) {
        oldLayer = data->mLayer;
      }
    }

    if (oldLayer) {
      NS_ASSERTION(oldLayer->Manager() == aManager, "Wrong manager");
      if (oldLayer->HasUserData(&gPaintedDisplayItemLayerUserData)) {
        // The old layer for this item is actually our PaintedLayer
        // because we rendered its layer into that PaintedLayer. So we
        // don't actually have a retained container layer.
      } else {
        NS_ASSERTION(oldLayer->GetType() == Layer::TYPE_CONTAINER,
                     "Wrong layer type");
        containerLayer = static_cast<ContainerLayer*>(oldLayer);
        ResetLayerStateForRecycling(containerLayer);
      }
    }
  }
  if (!containerLayer) {
    // No suitable existing layer was found.
    containerLayer = aManager->CreateContainerLayer();
    if (!containerLayer)
      return nullptr;
  }

  if (aContainerItem && aContainerItem->GetType() == DisplayItemType::TYPE_SCROLL_INFO_LAYER) {
    // Empty layers only have metadata and should never have display items. We
    // early exit because later, invalidation will walk up the frame tree to
    // determine which painted layer gets invalidated. Since an empty layer
    // should never have anything to paint, it should never be invalidated.
    NS_ASSERTION(aChildren->IsEmpty(), "Should have no children");
    return containerLayer.forget();
  }

  const ActiveScrolledRoot* containerASR = aContainerItem ? aContainerItem->GetActiveScrolledRoot() : nullptr;
  const ActiveScrolledRoot* containerScrollMetadataASR = aParameters.mScrollMetadataASR;
  const ActiveScrolledRoot* containerCompositorASR = aParameters.mCompositorASR;

  if (!aContainerItem && gfxPrefs::LayoutUseContainersForRootFrames()) {
    containerASR = aBuilder->ActiveScrolledRootForRootScrollframe();
    containerScrollMetadataASR = containerASR;
    containerCompositorASR = containerASR;
  }

  ContainerLayerParameters scaleParameters;
  nsRect bounds = aChildren->GetClippedBoundsWithRespectToASR(aBuilder, containerASR);
  nsRect childrenVisible =
      aContainerItem ? aContainerItem->GetVisibleRectForChildren() :
          aContainerFrame->GetVisualOverflowRectRelativeToSelf();
  if (!ChooseScaleAndSetTransform(this, aBuilder, aContainerFrame,
                                  aContainerItem,
                                  bounds.Intersect(childrenVisible),
                                  aTransform, aParameters,
                                  containerLayer, scaleParameters)) {
    return nullptr;
  }

  if (mRetainingManager) {
    if (aContainerItem) {
      DisplayItemData* data = GetDisplayItemDataForManager(aContainerItem, mRetainingManager);
      StoreDataForFrame(aContainerItem, containerLayer, LAYER_ACTIVE, data);
    } else {
      StoreDataForFrame(aContainerFrame, containerDisplayItemKey, containerLayer, LAYER_ACTIVE);
    }
  }

  nsIntRect pixBounds;
  nscoord appUnitsPerDevPixel;

  nscolor backgroundColor = NS_RGBA(0,0,0,0);
  if (aFlags & CONTAINER_ALLOW_PULL_BACKGROUND_COLOR) {
    backgroundColor = aParameters.mBackgroundColor;
  }

  uint32_t flags;
  ContainerState state(aBuilder, aManager, aManager->GetLayerBuilder(),
                       aContainerFrame, aContainerItem, bounds,
                       containerLayer, scaleParameters,
                       backgroundColor, containerASR, containerScrollMetadataASR,
                       containerCompositorASR);

  state.ProcessDisplayItems(aChildren);

  // Set CONTENT_COMPONENT_ALPHA if any of our children have it.
  // This is suboptimal ... a child could have text that's over transparent
  // pixels in its own layer, but over opaque parts of previous siblings.
  pixBounds = state.ScaleToOutsidePixels(bounds, false);
  appUnitsPerDevPixel = state.GetAppUnitsPerDevPixel();
  state.Finish(&flags, pixBounds, aChildren);

  // CONTENT_COMPONENT_ALPHA is propogated up to the nearest CONTENT_OPAQUE
  // ancestor so that BasicLayerManager knows when to copy the background into
  // pushed groups. Accelerated layers managers can't necessarily do this (only
  // when the visible region is a simple rect), so we propogate
  // CONTENT_COMPONENT_ALPHA_DESCENDANT all the way to the root.
  if (flags & Layer::CONTENT_COMPONENT_ALPHA) {
    flags |= Layer::CONTENT_COMPONENT_ALPHA_DESCENDANT;
  }

  // Make sure that rounding the visible region out didn't add any area
  // we won't paint
  if (aChildren->IsOpaque() && !aChildren->NeedsTransparentSurface()) {
    bounds.ScaleRoundIn(scaleParameters.mXScale, scaleParameters.mYScale);
    if (bounds.Contains(ToAppUnits(pixBounds, appUnitsPerDevPixel))) {
      // Clear CONTENT_COMPONENT_ALPHA and add CONTENT_OPAQUE instead.
      flags &= ~Layer::CONTENT_COMPONENT_ALPHA;
      flags |= Layer::CONTENT_OPAQUE;
    }
  }
  containerLayer->SetContentFlags(flags);
  // If aContainerItem is non-null some BuildContainerLayer further up the
  // call stack is responsible for setting containerLayer's visible region.
  if (!aContainerItem) {
    containerLayer->SetVisibleRegion(LayerIntRegion::FromUnknownRegion(pixBounds));
  }
  if (aParameters.mLayerContentsVisibleRect) {
    *aParameters.mLayerContentsVisibleRect = pixBounds + scaleParameters.mOffset;
  }

  nsPresContext::ClearNotifySubDocInvalidationData(containerLayer);

  return containerLayer.forget();
}

Layer*
FrameLayerBuilder::GetLeafLayerFor(nsDisplayListBuilder* aBuilder,
                                   nsDisplayItem* aItem)
{
  Layer* layer = GetOldLayerFor(aItem);
  if (!layer)
    return nullptr;
  if (layer->HasUserData(&gPaintedDisplayItemLayerUserData)) {
    // This layer was created to render Thebes-rendered content for this
    // display item. The display item should not use it for its own
    // layer rendering.
    return nullptr;
  }
  ResetLayerStateForRecycling(layer);
  return layer;
}

/* static */ void
FrameLayerBuilder::InvalidateAllLayers(LayerManager* aManager)
{
  LayerManagerData* data = static_cast<LayerManagerData*>
    (aManager->GetUserData(&gLayerManagerUserData));
  if (data) {
    data->mInvalidateAllLayers = true;
  }
}

/* static */ void
FrameLayerBuilder::InvalidateAllLayersForFrame(nsIFrame *aFrame)
{
  const SmallPointerArray<DisplayItemData>& array = aFrame->DisplayItemData();

  for (uint32_t i = 0; i < array.Length(); i++) {
    DisplayItemData::AssertDisplayItemData(array.ElementAt(i))->mParent->mInvalidateAllLayers = true;
  }
}

/* static */
Layer*
FrameLayerBuilder::GetDedicatedLayer(nsIFrame* aFrame, DisplayItemType aDisplayItemKey)
{
  //TODO: This isn't completely correct, since a frame could exist as a layer
  // in the normal widget manager, and as a different layer (or no layer)
  // in the secondary manager

  const SmallPointerArray<DisplayItemData>& array = aFrame->DisplayItemData();;

  for (uint32_t i = 0; i < array.Length(); i++) {
    DisplayItemData *element = DisplayItemData::AssertDisplayItemData(array.ElementAt(i));
    if (!element->mParent->mLayerManager->IsWidgetLayerManager()) {
      continue;
    }
    if (GetDisplayItemTypeFromKey(element->mDisplayItemKey) == aDisplayItemKey) {
      if (element->mOptLayer) {
        return element->mOptLayer;
      }


      Layer* layer = element->mLayer;
      if (!layer->HasUserData(&gColorLayerUserData) &&
          !layer->HasUserData(&gImageLayerUserData) &&
          !layer->HasUserData(&gPaintedDisplayItemLayerUserData)) {
        return layer;
      }
    }
  }
  return nullptr;
}

gfxSize
FrameLayerBuilder::GetPaintedLayerScaleForFrame(nsIFrame* aFrame)
{
  MOZ_ASSERT(aFrame, "need a frame");

  nsPresContext* presCtx = aFrame->PresContext()->GetRootPresContext();

  if (!presCtx) {
    presCtx = aFrame->PresContext();
    MOZ_ASSERT(presCtx);
  }

  nsIFrame* root = presCtx->PresShell()->GetRootFrame();

  MOZ_ASSERT(root);

  float resolution = presCtx->PresShell()->GetResolution();

  Matrix4x4Flagged transform = Matrix4x4::Scaling(resolution, resolution, 1.0);
  if (aFrame != root) {
    // aTransform is applied first, then the scale is applied to the result
    transform = nsLayoutUtils::GetTransformToAncestor(aFrame, root) * transform;
  }

  Matrix transform2d;
  if (transform.CanDraw2D(&transform2d)) {
    return ThebesMatrix(transform2d).ScaleFactors(true);
  }

  return gfxSize(1.0, 1.0);
}

#ifdef MOZ_DUMP_PAINTING
static void DebugPaintItem(DrawTarget& aDrawTarget,
                           nsPresContext* aPresContext,
                           nsDisplayItem *aItem,
                           nsDisplayListBuilder* aBuilder)
{
  bool snap;
  Rect bounds = NSRectToRect(aItem->GetBounds(aBuilder, &snap),
                             aPresContext->AppUnitsPerDevPixel());

  RefPtr<DrawTarget> tempDT =
    aDrawTarget.CreateSimilarDrawTarget(IntSize::Truncate(bounds.width, bounds.height),
                                        SurfaceFormat::B8G8R8A8);
  RefPtr<gfxContext> context = gfxContext::CreateOrNull(tempDT);
  if (!context) {
    // Leave this as crash, it's in the debugging code, we want to know
    gfxDevCrash(LogReason::InvalidContext) << "DebugPaintItem context problem " << gfx::hexa(tempDT);
    return;
  }
  context->SetMatrix(Matrix::Translation(-bounds.x, -bounds.y));

  aItem->Paint(aBuilder, context);
  RefPtr<SourceSurface> surface = tempDT->Snapshot();
  DumpPaintedImage(aItem, surface);

  aDrawTarget.DrawSurface(surface, bounds, Rect(Point(0,0), bounds.Size()));

  aItem->SetPainted();
}
#endif

/* static */ void
FrameLayerBuilder::RecomputeVisibilityForItems(nsTArray<AssignedDisplayItem>& aItems,
                                               nsDisplayListBuilder *aBuilder,
                                               const nsIntRegion& aRegionToDraw,
                                               const nsIntPoint& aOffset,
                                               int32_t aAppUnitsPerDevPixel,
                                               float aXScale,
                                               float aYScale)
{
  uint32_t i;
  // Update visible regions. We perform visibility analysis to take account
  // of occlusion culling.
  nsRegion visible = aRegionToDraw.ToAppUnits(aAppUnitsPerDevPixel);
  visible.MoveBy(NSIntPixelsToAppUnits(aOffset.x, aAppUnitsPerDevPixel),
                 NSIntPixelsToAppUnits(aOffset.y, aAppUnitsPerDevPixel));
  visible.ScaleInverseRoundOut(aXScale, aYScale);

  for (i = aItems.Length(); i > 0; --i) {
    AssignedDisplayItem* cdi = &aItems[i - 1];
    if (!cdi->mItem) {
      continue;
    }

    if (cdi->mType == DisplayItemEntryType::POP_OPACITY ||
        (cdi->mType == DisplayItemEntryType::ITEM && cdi->mHasOpacity)) {
      // The visibility calculations are skipped when the item is an effect end
      // marker, or when the display item is within a flattened opacity group.
      // This is because RecomputeVisibility has already been called for the
      // group item, and all the children.
      continue;
    }

    const DisplayItemClip& clip = cdi->mItem->GetClip();

    NS_ASSERTION(AppUnitsPerDevPixel(cdi->mItem) == aAppUnitsPerDevPixel,
                 "a painted layer should contain items only at the same zoom");

    MOZ_ASSERT(clip.HasClip() || clip.GetRoundedRectCount() == 0,
               "If we have rounded rects, we must have a clip rect");

    if (!clip.IsRectAffectedByClip(visible.GetBounds())) {
      cdi->mItem->RecomputeVisibility(aBuilder, &visible);
      continue;
    }

    // Do a little dance to account for the fact that we're clipping
    // to cdi->mClipRect
    nsRegion clipped;
    clipped.And(visible, clip.NonRoundedIntersection());
    nsRegion finalClipped = clipped;
    cdi->mItem->RecomputeVisibility(aBuilder, &finalClipped);
    // If we have rounded clip rects, don't subtract from the visible
    // region since we aren't displaying everything inside the rect.
    if (clip.GetRoundedRectCount() == 0) {
      nsRegion removed;
      removed.Sub(clipped, finalClipped);
      nsRegion newVisible;
      newVisible.Sub(visible, removed);
      // Don't let the visible region get too complex.
      if (newVisible.GetNumRects() <= 15) {
        visible = Move(newVisible);
      }
    }
  }
}

/**
 * Sets the clip chain and starts a new opacity group.
 */
static void
PushOpacity(gfxContext* aContext,
            const nsRect& aPaintRect,
            AssignedDisplayItem& aItem,
            const int32_t aAUPDP)
{
  MOZ_ASSERT(aItem.mType == DisplayItemEntryType::PUSH_OPACITY ||
             aItem.mType == DisplayItemEntryType::PUSH_OPACITY_WITH_BG);
  MOZ_ASSERT(aItem.mItem->GetType() == DisplayItemType::TYPE_OPACITY);

  aContext->Save();

  DisplayItemClip clip;
  clip.SetTo(aPaintRect);
  clip.IntersectWith(aItem.mItem->GetClip());
  clip.ApplyTo(aContext, aAUPDP);

  nsDisplayOpacity* opacityItem = static_cast<nsDisplayOpacity*>(aItem.mItem);
  const float opacity = opacityItem->GetOpacity();

  if (aItem.mType == DisplayItemEntryType::PUSH_OPACITY_WITH_BG) {
    aContext->PushGroupAndCopyBackground(gfxContentType::COLOR_ALPHA, opacity);
  } else {
    aContext->PushGroupForBlendBack(gfxContentType::COLOR_ALPHA, opacity);
  }
}

/**
 * Tracks item clips per opacity nesting level.
 */
struct ClipTracker {
  explicit ClipTracker(gfxContext* aContext)
    : mContext(aContext)
  {}

  bool HasClip(int aOpacityNesting) const
  {
    return !mClips.IsEmpty() &&
            mClips.LastElement() == aOpacityNesting;
  }

  void PopClipIfNeeded(int aOpacityNesting)
  {
    if (!HasClip(aOpacityNesting)) {
      return;
    }

    mContext->Restore();
    mClips.RemoveLastElement();
  };

  void SaveClip(int aOpacityNesting)
  {
    mContext->Save();
    mClips.AppendElement(aOpacityNesting);
  };

  AutoTArray<int, 2> mClips;
  gfxContext* mContext;
};

static void
UpdateOpacityNesting(int& aOpacityNesting, DisplayItemEntryType aType)
{
  if (aType == DisplayItemEntryType::PUSH_OPACITY ||
      aType == DisplayItemEntryType::PUSH_OPACITY_WITH_BG) {
    aOpacityNesting++;
  }

  if (aType == DisplayItemEntryType::POP_OPACITY) {
    aOpacityNesting--;
  }

  MOZ_ASSERT(aOpacityNesting >= 0);
}

void
FrameLayerBuilder::PaintItems(nsTArray<AssignedDisplayItem>& aItems,
                              const nsIntRect& aRect,
                              gfxContext *aContext,
                              nsDisplayListBuilder* aBuilder,
                              nsPresContext* aPresContext,
                              const nsIntPoint& aOffset,
                              float aXScale, float aYScale)
{
  DrawTarget& aDrawTarget = *aContext->GetDrawTarget();

  int32_t appUnitsPerDevPixel = aPresContext->AppUnitsPerDevPixel();
  nsRect boundRect = ToAppUnits(aRect, appUnitsPerDevPixel);
  boundRect.MoveBy(NSIntPixelsToAppUnits(aOffset.x, appUnitsPerDevPixel),
                 NSIntPixelsToAppUnits(aOffset.y, appUnitsPerDevPixel));
  boundRect.ScaleInverseRoundOut(aXScale, aYScale);

  DisplayItemClip currentClip, tmpClip;

  // Tracks opacity nesting level for item level clipping.
  int opacityNesting = 0;

  // Tracks opacity nesting level for skipping items between opacity markers,
  // when opacity has empty visible rect set.
  int emptyOpacityNesting = 0;

  ClipTracker clipTracker(aContext);

  for (uint32_t i = 0; i < aItems.Length(); ++i) {
    AssignedDisplayItem& cdi = aItems[i];
    nsDisplayItem* item = cdi.mItem;

    if (!item) {
      MOZ_ASSERT(cdi.mType == DisplayItemEntryType::ITEM);
      continue;
    }

    const nsRect& visibleRect = item->GetVisibleRect();
    const nsRect paintRect = visibleRect.Intersect(boundRect);

    if (paintRect.IsEmpty() || emptyOpacityNesting > 0) {
      // In order for this branch to be hit, either this item has an empty paint
      // rect and nothing would be drawn, or a PUSH_OPACITY marker before this
      // item had an empty paint rect. In the latter case, the items are skipped
      // until POP_OPACITY markers bring |emptyOpacityNesting| back to 0.
      UpdateOpacityNesting(emptyOpacityNesting, cdi.mType);
      continue;
    }

#ifdef MOZ_DUMP_PAINTING
    AUTO_PROFILER_LABEL_DYNAMIC_CSTR("FrameLayerBuilder::PaintItems", GRAPHICS,
                                     item->Name());
#else
    AUTO_PROFILER_LABEL("FrameLayerBuilder::PaintItems", GRAPHICS);
#endif

    MOZ_ASSERT((opacityNesting == 0 && !cdi.mHasOpacity) ||
               (opacityNesting > 0 && cdi.mHasOpacity));

    if (cdi.mType == DisplayItemEntryType::PUSH_OPACITY ||
        cdi.mType == DisplayItemEntryType::PUSH_OPACITY_WITH_BG) {
      clipTracker.PopClipIfNeeded(opacityNesting);
      PushOpacity(aContext, paintRect, cdi, appUnitsPerDevPixel);
    }

    if (cdi.mType == DisplayItemEntryType::POP_OPACITY) {
      MOZ_ASSERT(item->GetType() == DisplayItemType::TYPE_OPACITY);
      MOZ_ASSERT(opacityNesting > 0);

      clipTracker.PopClipIfNeeded(opacityNesting);
      aContext->PopGroupAndBlend();
      aContext->Restore();
    }

    if (cdi.mType != DisplayItemEntryType::ITEM) {
      UpdateOpacityNesting(opacityNesting, cdi.mType);
      continue;
    }

    // If the new desired clip state is different from the current state,
    // update the clip.
    const DisplayItemClip* clip = &item->GetClip();
    if (clip->GetRoundedRectCount() > 0 &&
        !clip->IsRectClippedByRoundedCorner(visibleRect)) {
      tmpClip = *clip;
      tmpClip.RemoveRoundedCorners();
      clip = &tmpClip;
    }
    if (clipTracker.HasClip(opacityNesting) != clip->HasClip() ||
        (clip->HasClip() && *clip != currentClip)) {
      clipTracker.PopClipIfNeeded(opacityNesting);

      if (clip->HasClip()) {
        currentClip = *clip;
        clipTracker.SaveClip(opacityNesting);
        currentClip.ApplyTo(aContext, appUnitsPerDevPixel);
        aContext->NewPath();
      }
    }

    if (cdi.mInactiveLayerManager) {
      bool saved = aDrawTarget.GetPermitSubpixelAA();
      PaintInactiveLayer(aBuilder, cdi.mInactiveLayerManager,
                         item, aContext, aContext);
      aDrawTarget.SetPermitSubpixelAA(saved);
    } else {
      nsIFrame* frame = item->Frame();
      if (aBuilder->IsPaintingToWindow()) {
        frame->AddStateBits(NS_FRAME_PAINTED_THEBES);
      }
#ifdef MOZ_DUMP_PAINTING
      if (gfxEnv::DumpPaintItems()) {
        DebugPaintItem(aDrawTarget, aPresContext, item, aBuilder);
      } else
#endif
      {
        item->Paint(aBuilder, aContext);
      }
    }
  }

  clipTracker.PopClipIfNeeded(opacityNesting);
  MOZ_ASSERT(opacityNesting == 0);
  MOZ_ASSERT(emptyOpacityNesting == 0);
}

/**
 * Returns true if it is preferred to draw the list of display
 * items separately for each rect in the visible region rather
 * than clipping to a complex region.
 */
static bool
ShouldDrawRectsSeparately(DrawTarget* aDrawTarget, DrawRegionClip aClip)
{
  if (!gfxPrefs::LayoutPaintRectsSeparately() ||
      aClip == DrawRegionClip::NONE) {
    return false;
  }

  return !aDrawTarget->SupportsRegionClipping();
}

static void DrawForcedBackgroundColor(DrawTarget& aDrawTarget,
                                      const IntRect& aBounds,
                                      nscolor aBackgroundColor)
{
  if (NS_GET_A(aBackgroundColor) > 0) {
    ColorPattern color(ToDeviceColor(aBackgroundColor));
    aDrawTarget.FillRect(Rect(aBounds), color);
  }
}

/*
 * A note on residual transforms:
 *
 * In a transformed subtree we sometimes apply the PaintedLayer's
 * "residual transform" when drawing content into the PaintedLayer.
 * This is a translation by components in the range [-0.5,0.5) provided
 * by the layer system; applying the residual transform followed by the
 * transforms used by layer compositing ensures that the subpixel alignment
 * of the content of the PaintedLayer exactly matches what it would be if
 * we used cairo/Thebes to draw directly to the screen without going through
 * retained layer buffers.
 *
 * The visible and valid regions of the PaintedLayer are computed without
 * knowing the residual transform (because we don't know what the residual
 * transform is going to be until we've built the layer tree!). So we have to
 * consider whether content painted in the range [x, xmost) might be painted
 * outside the visible region we computed for that content. The visible region
 * would be [floor(x), ceil(xmost)). The content would be rendered at
 * [x + r, xmost + r), where -0.5 <= r < 0.5. So some half-rendered pixels could
 * indeed fall outside the computed visible region, which is not a big deal;
 * similar issues already arise when we snap cliprects to nearest pixels.
 * Note that if the rendering of the content is snapped to nearest pixels ---
 * which it often is --- then the content is actually rendered at
 * [snap(x + r), snap(xmost + r)). It turns out that floor(x) <= snap(x + r)
 * and ceil(xmost) >= snap(xmost + r), so the rendering of snapped content
 * always falls within the visible region we computed.
 */

/* static */ void
FrameLayerBuilder::DrawPaintedLayer(PaintedLayer* aLayer,
                                   gfxContext* aContext,
                                   const nsIntRegion& aRegionToDraw,
                                   const nsIntRegion& aDirtyRegion,
                                   DrawRegionClip aClip,
                                   const nsIntRegion& aRegionToInvalidate,
                                   void* aCallbackData)
{
  DrawTarget& aDrawTarget = *aContext->GetDrawTarget();

  AUTO_PROFILER_LABEL("FrameLayerBuilder::DrawPaintedLayer", GRAPHICS);

  nsDisplayListBuilder* builder = static_cast<nsDisplayListBuilder*>
    (aCallbackData);

  FrameLayerBuilder *layerBuilder = aLayer->Manager()->GetLayerBuilder();
  NS_ASSERTION(layerBuilder, "Unexpectedly null layer builder!");

  PaintedDisplayItemLayerUserData* userData =
    static_cast<PaintedDisplayItemLayerUserData*>
      (aLayer->GetUserData(&gPaintedDisplayItemLayerUserData));
  NS_ASSERTION(userData, "where did our user data go?");
  if (!userData->mContainerLayerFrame) {
    return;
  }

  bool shouldDrawRectsSeparately =
    ShouldDrawRectsSeparately(&aDrawTarget, aClip);

  if (!shouldDrawRectsSeparately) {
    if (aClip == DrawRegionClip::DRAW) {
      gfxUtils::ClipToRegion(aContext, aRegionToDraw);
    }

    DrawForcedBackgroundColor(aDrawTarget, aRegionToDraw.GetBounds(),
                              userData->mForcedBackgroundColor);
  }

  // make the origin of the context coincide with the origin of the
  // PaintedLayer
  gfxContextMatrixAutoSaveRestore saveMatrix(aContext);
  nsIntPoint offset = GetTranslationForPaintedLayer(aLayer);
  nsPresContext* presContext = userData->mContainerLayerFrame->PresContext();

  if (!userData->mVisibilityComputedRegion.Contains(aDirtyRegion) &&
      !layerBuilder->GetContainingPaintedLayerData()) {
    // Recompute visibility of items in our PaintedLayer, if required. Note
    // that this recomputes visibility for all descendants of our display
    // items too, so there's no need to do this for the items in inactive
    // PaintedLayers. If aDirtyRegion has not changed since the previous call
    // then we can skip this.
    int32_t appUnitsPerDevPixel = presContext->AppUnitsPerDevPixel();
    RecomputeVisibilityForItems(userData->mItems, builder, aDirtyRegion,
                                offset, appUnitsPerDevPixel,
                                userData->mXScale, userData->mYScale);
    userData->mVisibilityComputedRegion = aDirtyRegion;
  }

  if (shouldDrawRectsSeparately) {
    for (auto iter = aRegionToDraw.RectIter(); !iter.Done(); iter.Next()) {
      const nsIntRect& iterRect = iter.Get();
      gfxContextAutoSaveRestore save(aContext);
      aContext->NewPath();
      aContext->Rectangle(ThebesRect(iterRect));
      aContext->Clip();

      DrawForcedBackgroundColor(aDrawTarget, iterRect,
                                userData->mForcedBackgroundColor);

      // Apply the residual transform if it has been enabled, to ensure that
      // snapping when we draw into aContext exactly matches the ideal transform.
      // See above for why this is OK.
      aContext->SetMatrixDouble(
        aContext->CurrentMatrixDouble().PreTranslate(aLayer->GetResidualTranslation() - gfxPoint(offset.x, offset.y)).
                                        PreScale(userData->mXScale, userData->mYScale));

      layerBuilder->PaintItems(userData->mItems, iterRect, aContext,
                               builder, presContext,
                               offset, userData->mXScale, userData->mYScale);
      if (gfxPrefs::GfxLoggingPaintedPixelCountEnabled()) {
        aLayer->Manager()->AddPaintedPixelCount(iterRect.Area());
      }
    }
  } else {
    // Apply the residual transform if it has been enabled, to ensure that
    // snapping when we draw into aContext exactly matches the ideal transform.
    // See above for why this is OK.
    aContext->SetMatrixDouble(
      aContext->CurrentMatrixDouble().PreTranslate(aLayer->GetResidualTranslation() - gfxPoint(offset.x, offset.y)).
                                      PreScale(userData->mXScale,userData->mYScale));

    layerBuilder->PaintItems(userData->mItems, aRegionToDraw.GetBounds(), aContext,
                             builder, presContext,
                             offset, userData->mXScale, userData->mYScale);
    if (gfxPrefs::GfxLoggingPaintedPixelCountEnabled()) {
      aLayer->Manager()->AddPaintedPixelCount(
        aRegionToDraw.GetBounds().Area());
    }
  }

  bool isActiveLayerManager = !aLayer->Manager()->IsInactiveLayerManager();

  if (presContext->GetPaintFlashing() && isActiveLayerManager) {
    gfxContextAutoSaveRestore save(aContext);
    if (shouldDrawRectsSeparately) {
      if (aClip == DrawRegionClip::DRAW) {
        gfxUtils::ClipToRegion(aContext, aRegionToDraw);
      }
    }
    FlashPaint(aContext);
  }

  if (presContext->GetDocShell() && isActiveLayerManager) {
    nsDocShell* docShell = static_cast<nsDocShell*>(presContext->GetDocShell());
    RefPtr<TimelineConsumers> timelines = TimelineConsumers::Get();

    if (timelines && timelines->HasConsumer(docShell)) {
      timelines->AddMarkerForDocShell(docShell, Move(
        MakeUnique<LayerTimelineMarker>(aRegionToDraw)));
    }
  }

  if (!aRegionToInvalidate.IsEmpty()) {
    aLayer->AddInvalidRect(aRegionToInvalidate.GetBounds());
  }
}

/* static */ void
FrameLayerBuilder::DumpRetainedLayerTree(LayerManager* aManager, std::stringstream& aStream, bool aDumpHtml)
{
  aManager->Dump(aStream, "", aDumpHtml);
}

nsDisplayItemGeometry*
FrameLayerBuilder::GetMostRecentGeometry(nsDisplayItem* aItem)
{
  typedef SmallPointerArray<DisplayItemData> DataArray;

  // Retrieve the array of DisplayItemData associated with our frame.
  const DataArray& dataArray = aItem->Frame()->DisplayItemData();

  // Find our display item data, if it exists, and return its geometry.
  uint32_t itemPerFrameKey = aItem->GetPerFrameKey();
  for (uint32_t i = 0; i < dataArray.Length(); i++) {
    DisplayItemData* data = DisplayItemData::AssertDisplayItemData(dataArray.ElementAt(i));
    if (data->GetDisplayItemKey() == itemPerFrameKey) {
      return data->GetGeometry();
    }
  }
  if (RefPtr<WebRenderFallbackData> data = GetWebRenderUserData<WebRenderFallbackData>(aItem->Frame(), itemPerFrameKey)) {
    return data->GetGeometry();
  }

  return nullptr;
}

static gfx::Rect
CalculateBounds(const nsTArray<DisplayItemClip::RoundedRect>& aRects, int32_t aAppUnitsPerDevPixel)
{
  nsRect bounds = aRects[0].mRect;
  for (uint32_t i = 1; i < aRects.Length(); ++i) {
    bounds.UnionRect(bounds, aRects[i].mRect);
   }

  return gfx::Rect(bounds.ToNearestPixels(aAppUnitsPerDevPixel));
}

void
ContainerState::SetupMaskLayer(Layer *aLayer,
                               const DisplayItemClip& aClip)
{
  // don't build an unnecessary mask
  if (aClip.GetRoundedRectCount() == 0) {
    return;
  }

  RefPtr<Layer> maskLayer =
    CreateMaskLayer(aLayer, aClip, Nothing());

  if (!maskLayer) {
    return;
  }

  aLayer->SetMaskLayer(maskLayer);
}

static MaskLayerUserData*
GetMaskLayerUserData(Layer* aMaskLayer)
{
  if (!aMaskLayer) {
    return nullptr;
  }

  return static_cast<MaskLayerUserData*>(aMaskLayer->GetUserData(&gMaskLayerUserData));
}

static void
SetMaskLayerUserData(Layer* aMaskLayer)
{
  MOZ_ASSERT(aMaskLayer);

  aMaskLayer->SetUserData(&gMaskLayerUserData,
                          new MaskLayerUserData());
}

already_AddRefed<Layer>
ContainerState::CreateMaskLayer(Layer *aLayer,
                               const DisplayItemClip& aClip,
                               const Maybe<size_t>& aForAncestorMaskLayer)
{
  // aLayer will never be the container layer created by an nsDisplayMask
  // because nsDisplayMask propagates the DisplayItemClip to its contents
  // and is not clipped itself.
  // This assertion will fail if that ever stops being the case.
  MOZ_ASSERT(!aLayer->GetUserData(&gCSSMaskLayerUserData),
             "A layer contains round clips should not have css-mask on it.");

  // check if we can re-use the mask layer
  RefPtr<ImageLayer> maskLayer =
      CreateOrRecycleMaskImageLayerFor(MaskLayerKey(aLayer, aForAncestorMaskLayer),
                                       GetMaskLayerUserData,
                                       SetMaskLayerUserData);
  MaskLayerUserData* userData = GetMaskLayerUserData(maskLayer.get());

  int32_t A2D = mContainerFrame->PresContext()->AppUnitsPerDevPixel();
  MaskLayerUserData newData(aClip, A2D, mParameters);
  if (*userData == newData) {
    return maskLayer.forget();
  }

  gfx::Rect boundingRect = CalculateBounds(newData.mRoundedClipRects,
                                           newData.mAppUnitsPerDevPixel);
  boundingRect.Scale(mParameters.mXScale, mParameters.mYScale);
  if (boundingRect.IsEmpty()) {
    // Return early if we know that there is effectively no visible data.
    return nullptr;
  }

  uint32_t maxSize = mManager->GetMaxTextureSize();
  NS_ASSERTION(maxSize > 0, "Invalid max texture size");
#ifdef MOZ_GFX_OPTIMIZE_MOBILE
  // Make mask image width aligned to 4. See Bug 1245552.
  gfx::Size surfaceSize(std::min<gfx::Float>(GetAlignedStride<4>(NSToIntCeil(boundingRect.Width()), 1), maxSize),
                        std::min<gfx::Float>(boundingRect.Height(), maxSize));
#else
  gfx::Size surfaceSize(std::min<gfx::Float>(boundingRect.Width(), maxSize),
                        std::min<gfx::Float>(boundingRect.Height(), maxSize));
#endif

  // maskTransform is applied to the clip when it is painted into the mask (as a
  // component of imageTransform), and its inverse used when the mask is used for
  // masking.
  // It is the transform from the masked layer's space to mask space
  gfx::Matrix maskTransform =
    Matrix::Scaling(surfaceSize.width / boundingRect.Width(),
                    surfaceSize.height / boundingRect.Height());
  if (surfaceSize.IsEmpty()) {
    // Return early if we know that the size of this mask surface is empty.
    return nullptr;
  }

  gfx::Point p = boundingRect.TopLeft();
  maskTransform.PreTranslate(-p.x, -p.y);
  // imageTransform is only used when the clip is painted to the mask
  gfx::Matrix imageTransform = maskTransform;
  imageTransform.PreScale(mParameters.mXScale, mParameters.mYScale);

  nsAutoPtr<MaskLayerImageCache::MaskLayerImageKey> newKey(
    new MaskLayerImageCache::MaskLayerImageKey());

  // copy and transform the rounded rects
  for (uint32_t i = 0; i < newData.mRoundedClipRects.Length(); ++i) {
    newKey->mRoundedClipRects.AppendElement(
      MaskLayerImageCache::PixelRoundedRect(newData.mRoundedClipRects[i],
                                            mContainerFrame->PresContext()));
    newKey->mRoundedClipRects[i].ScaleAndTranslate(imageTransform);
  }
  newKey->mKnowsCompositor = mManager->AsKnowsCompositor();

  const MaskLayerImageCache::MaskLayerImageKey* lookupKey = newKey;

  // check to see if we can reuse a mask image
  RefPtr<ImageContainer> container =
    GetMaskLayerImageCache()->FindImageFor(&lookupKey);

  if (!container) {
    IntSize surfaceSizeInt(NSToIntCeil(surfaceSize.width),
                           NSToIntCeil(surfaceSize.height));
    // no existing mask image, so build a new one
    MaskImageData imageData(surfaceSizeInt, mManager);
    RefPtr<DrawTarget> dt = imageData.CreateDrawTarget();

    // fail if we can't get the right surface
    if (!dt || !dt->IsValid()) {
      NS_WARNING("Could not create DrawTarget for mask layer.");
      return nullptr;
    }

    RefPtr<gfxContext> context = gfxContext::CreateOrNull(dt);
    MOZ_ASSERT(context); // already checked the draw target above
    context->Multiply(ThebesMatrix(imageTransform));

    // paint the clipping rects with alpha to create the mask
    aClip.FillIntersectionOfRoundedRectClips(context,
                                             Color(1.f, 1.f, 1.f, 1.f),
                                             newData.mAppUnitsPerDevPixel);

    // build the image and container
    MOZ_ASSERT(aLayer->Manager() == mManager);
    container = imageData.CreateImageAndImageContainer();
    NS_ASSERTION(container, "Could not create image container for mask layer.");

    if (!container) {
      return nullptr;
    }

    GetMaskLayerImageCache()->PutImage(newKey.forget(), container);
  }

  maskLayer->SetContainer(container);

  maskTransform.Invert();
  Matrix4x4 matrix = Matrix4x4::From2D(maskTransform);
  matrix.PreTranslate(mParameters.mOffset.x, mParameters.mOffset.y, 0);
  maskLayer->SetBaseTransform(matrix);

  // save the details of the clip in user data
  *userData = Move(newData);
  userData->mImageKey.Reset(lookupKey);

  return maskLayer.forget();
}

} // namespace mozilla
