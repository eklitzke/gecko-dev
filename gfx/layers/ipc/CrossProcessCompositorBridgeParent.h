/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_CrossProcessCompositorBridgeParent_h
#define mozilla_layers_CrossProcessCompositorBridgeParent_h

#include "mozilla/layers/CompositorBridgeParent.h"
#include "mozilla/layers/CompositorThread.h"

namespace mozilla {
namespace layers {

class CompositorOptions;

/**
 * This class handles layer updates pushed directly from child processes to
 * the compositor thread. It's associated with a CompositorBridgeParent on the
 * compositor thread. While it uses the PCompositorBridge protocol to manage
 * these updates, it doesn't actually drive compositing itself. For that it
 * hands off work to the CompositorBridgeParent it's associated with.
 */
class CrossProcessCompositorBridgeParent final : public CompositorBridgeParentBase
{
  friend class CompositorBridgeParent;

public:
  explicit CrossProcessCompositorBridgeParent(CompositorManagerParent* aManager)
    : CompositorBridgeParentBase(aManager)
    , mNotifyAfterRemotePaint(false)
    , mDestroyCalled(false)
  {
  }

  void ActorDestroy(ActorDestroyReason aWhy) override;

  // FIXME/bug 774388: work out what shutdown protocol we need.
  mozilla::ipc::IPCResult RecvInitialize(const LayersId& aRootLayerTreeId) override { return IPC_FAIL_NO_REASON(this); }
  mozilla::ipc::IPCResult RecvWillClose() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvPause() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvResume() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvForceIsFirstPaint() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvNotifyChildCreated(const LayersId& child, CompositorOptions* aOptions) override;
  mozilla::ipc::IPCResult RecvMapAndNotifyChildCreated(const LayersId& child, const base::ProcessId& pid, CompositorOptions* aOptions) override;
  mozilla::ipc::IPCResult RecvNotifyChildRecreated(const LayersId& child, CompositorOptions* aOptions) override { return IPC_FAIL_NO_REASON(this); }
  mozilla::ipc::IPCResult RecvAdoptChild(const LayersId& child) override { return IPC_FAIL_NO_REASON(this); }
  mozilla::ipc::IPCResult RecvMakeSnapshot(const SurfaceDescriptor& aInSnapshot,
                                           const gfx::IntRect& aRect) override
  { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvFlushRendering() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvFlushRenderingAsync() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvForcePresent() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvWaitOnTransactionProcessed() override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvNotifyRegionInvalidated(const nsIntRegion& aRegion) override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvStartFrameTimeRecording(const int32_t& aBufferSize, uint32_t* aOutStartIndex) override { return IPC_OK(); }
  mozilla::ipc::IPCResult RecvStopFrameTimeRecording(const uint32_t& aStartIndex, InfallibleTArray<float>* intervals) override  { return IPC_OK(); }

  mozilla::ipc::IPCResult RecvCheckContentOnlyTDR(const uint32_t& sequenceNum, bool* isContentOnlyTDR) override;

  mozilla::ipc::IPCResult RecvAllPluginsCaptured() override { return IPC_OK(); }

  mozilla::ipc::IPCResult RecvGetFrameUniformity(FrameUniformityData* aOutData) override
  {
    // Don't support calculating frame uniformity on the child process and
    // this is just a stub for now.
    MOZ_ASSERT(false);
    return IPC_OK();
  }

  /**
   * Tells this CompositorBridgeParent to send a message when the compositor has received the transaction.
   */
  mozilla::ipc::IPCResult RecvRequestNotifyAfterRemotePaint() override;

  PLayerTransactionParent* AllocPLayerTransactionParent(
      const nsTArray<LayersBackend>& aBackendHints,
      const LayersId& aId) override;

  bool DeallocPLayerTransactionParent(PLayerTransactionParent* aLayers) override;

  void ShadowLayersUpdated(LayerTransactionParent* aLayerTree,
                           const TransactionInfo& aInfo,
                           bool aHitTestUpdate) override;
  void ScheduleComposite(LayerTransactionParent* aLayerTree) override;
  void NotifyClearCachedResources(LayerTransactionParent* aLayerTree) override;
  bool SetTestSampleTime(const LayersId& aId,
                         const TimeStamp& aTime) override;
  void LeaveTestMode(const LayersId& aId) override;
  void ApplyAsyncProperties(LayerTransactionParent* aLayerTree) override;
  void SetTestAsyncScrollOffset(const LayersId& aLayersId,
                                const FrameMetrics::ViewID& aScrollId,
                                const CSSPoint& aPoint) override;
  void SetTestAsyncZoom(const LayersId& aLayersId,
                        const FrameMetrics::ViewID& aScrollId,
                        const LayerToParentLayerScale& aZoom) override;
  void FlushApzRepaints(const LayersId& aLayersId) override;
  void GetAPZTestData(const LayersId& aLayersId,
                      APZTestData* aOutData) override;
  void SetConfirmedTargetAPZC(const LayersId& aLayersId,
                              const uint64_t& aInputBlockId,
                              const nsTArray<ScrollableLayerGuid>& aTargets) override;

  AsyncCompositionManager* GetCompositionManager(LayerTransactionParent* aParent) override;
  mozilla::ipc::IPCResult RecvRemotePluginsReady()  override { return IPC_FAIL_NO_REASON(this); }

  // Use DidCompositeLocked if you already hold a lock on
  // sIndirectLayerTreesLock; Otherwise use DidComposite, which would request
  // the lock automatically.
  void DidCompositeLocked(LayersId aId,
                                  TimeStamp& aCompositeStart,
                                  TimeStamp& aCompositeEnd);
  void DidComposite(LayersId aId,
                    TimeStamp& aCompositeStart,
                    TimeStamp& aCompositeEnd) override;

  PTextureParent* AllocPTextureParent(const SurfaceDescriptor& aSharedData,
                                      const ReadLockDescriptor& aReadLock,
                                      const LayersBackend& aLayersBackend,
                                      const TextureFlags& aFlags,
                                      const LayersId& aId,
                                      const uint64_t& aSerial,
                                      const wr::MaybeExternalImageId& aExternalImageId) override;

  bool DeallocPTextureParent(PTextureParent* actor) override;

  bool IsSameProcess() const override;

  PCompositorWidgetParent* AllocPCompositorWidgetParent(const CompositorWidgetInitData& aInitData) override {
    // Not allowed.
    return nullptr;
  }
  bool DeallocPCompositorWidgetParent(PCompositorWidgetParent* aActor) override {
    // Not allowed.
    return false;
  }

  PAPZCTreeManagerParent* AllocPAPZCTreeManagerParent(const LayersId& aLayersId) override;
  bool DeallocPAPZCTreeManagerParent(PAPZCTreeManagerParent* aActor) override;

  PAPZParent* AllocPAPZParent(const LayersId& aLayersId) override;
  bool DeallocPAPZParent(PAPZParent* aActor) override;

  void UpdatePaintTime(LayerTransactionParent* aLayerTree, const TimeDuration& aPaintTime) override;

  PWebRenderBridgeParent* AllocPWebRenderBridgeParent(const wr::PipelineId& aPipelineId,
                                                      const LayoutDeviceIntSize& aSize,
                                                      TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                                      wr::IdNamespace* aIdNamespace) override;
  bool DeallocPWebRenderBridgeParent(PWebRenderBridgeParent* aActor) override;

  void ObserveLayerUpdate(LayersId aLayersId, uint64_t aEpoch, bool aActive) override;

  bool IsRemote() const override {
    return true;
  }

private:
  // Private destructor, to discourage deletion outside of Release():
  virtual ~CrossProcessCompositorBridgeParent();

  void DeferredDestroy();

  // There can be many CPCPs, and IPDL-generated code doesn't hold a
  // reference to top-level actors.  So we hold a reference to
  // ourself.  This is released (deferred) in ActorDestroy().
  RefPtr<CrossProcessCompositorBridgeParent> mSelfRef;

  // If true, we should send a RemotePaintIsReady message when the layer transaction
  // is received
  bool mNotifyAfterRemotePaint;
  bool mDestroyCalled;
};

} // namespace layers
} // namespace mozilla

#endif // mozilla_layers_CrossProcessCompositorBridgeParent_h
