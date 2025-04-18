/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/CompositorBridgeParent.h"

#include <stdio.h>                      // for fprintf, stdout
#include <stdint.h>                     // for uint64_t
#include <map>                          // for _Rb_tree_iterator, etc
#include <utility>                      // for pair

#include "apz/src/APZCTreeManager.h"    // for APZCTreeManager
#include "LayerTransactionParent.h"     // for LayerTransactionParent
#include "RenderTrace.h"                // for RenderTraceLayers
#include "base/message_loop.h"          // for MessageLoop
#include "base/process.h"               // for ProcessId
#include "base/task.h"                  // for CancelableTask, etc
#include "base/thread.h"                // for Thread
#include "gfxContext.h"                 // for gfxContext
#include "gfxPlatform.h"                // for gfxPlatform
#include "TreeTraversal.h"              // for ForEachNode
#ifdef MOZ_WIDGET_GTK
#include "gfxPlatformGtk.h"             // for gfxPlatform
#endif
#include "gfxPrefs.h"                   // for gfxPrefs
#include "mozilla/AutoRestore.h"        // for AutoRestore
#include "mozilla/ClearOnShutdown.h"    // for ClearOnShutdown
#include "mozilla/DebugOnly.h"          // for DebugOnly
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/TabParent.h"
#include "mozilla/gfx/2D.h"          // for DrawTarget
#include "mozilla/gfx/GPUChild.h"       // for GfxPrefValue
#include "mozilla/gfx/Point.h"          // for IntSize
#include "mozilla/gfx/Rect.h"          // for IntSize
#include "mozilla/gfx/gfxVars.h"        // for gfxVars
#include "VRManager.h"                  // for VRManager
#include "mozilla/ipc/Transport.h"      // for Transport
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gfx/GPUParent.h"
#include "mozilla/layers/AnimationHelper.h" // for CompositorAnimationStorage
#include "mozilla/layers/APZCTreeManagerParent.h"  // for APZCTreeManagerParent
#include "mozilla/layers/APZSampler.h"  // for APZSampler
#include "mozilla/layers/APZThreadUtils.h"  // for APZThreadUtils
#include "mozilla/layers/APZUpdater.h"  // for APZUpdater
#include "mozilla/layers/AsyncCompositionManager.h"
#include "mozilla/layers/BasicCompositor.h"  // for BasicCompositor
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/CompositorManagerParent.h" // for CompositorManagerParent
#include "mozilla/layers/CompositorOGL.h"  // for CompositorOGL
#include "mozilla/layers/CompositorThread.h"
#include "mozilla/layers/CompositorTypes.h"
#include "mozilla/layers/CompositorVsyncScheduler.h"
#include "mozilla/layers/CrossProcessCompositorBridgeParent.h"
#include "mozilla/layers/FrameUniformityData.h"
#include "mozilla/layers/ImageBridgeParent.h"
#include "mozilla/layers/LayerManagerComposite.h"
#include "mozilla/layers/LayerManagerMLGPU.h"
#include "mozilla/layers/LayerTreeOwnerTracker.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/PLayerTransactionParent.h"
#include "mozilla/layers/RemoteContentController.h"
#include "mozilla/layers/WebRenderBridgeParent.h"
#include "mozilla/layers/AsyncImagePipelineManager.h"
#include "mozilla/layout/RenderFrameParent.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "mozilla/media/MediaSystemResourceService.h" // for MediaSystemResourceService
#include "mozilla/mozalloc.h"           // for operator new, etc
#include "mozilla/Telemetry.h"
#ifdef MOZ_WIDGET_GTK
#include "basic/X11BasicCompositor.h" // for X11BasicCompositor
#endif
#include "nsCOMPtr.h"                   // for already_AddRefed
#include "nsDebug.h"                    // for NS_ASSERTION, etc
#include "nsISupportsImpl.h"            // for MOZ_COUNT_CTOR, etc
#include "nsIWidget.h"                  // for nsIWidget
#include "nsTArray.h"                   // for nsTArray
#include "nsThreadUtils.h"              // for NS_IsMainThread
#include "nsXULAppAPI.h"                // for XRE_GetIOMessageLoop
#ifdef XP_WIN
#include "mozilla/layers/CompositorD3D11.h"
#include "mozilla/widget/WinCompositorWidget.h"
#endif
#include "GeckoProfiler.h"
#include "mozilla/ipc/ProtocolTypes.h"
#include "mozilla/Unused.h"
#include "mozilla/Hal.h"
#include "mozilla/HalTypes.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/Telemetry.h"
#ifdef MOZ_GECKO_PROFILER
# include "ProfilerMarkerPayload.h"
#endif
#include "mozilla/VsyncDispatcher.h"
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
# include "VsyncSource.h"
#endif
#include "mozilla/widget/CompositorWidget.h"
#ifdef MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING
# include "mozilla/widget/CompositorWidgetParent.h"
#endif
#ifdef XP_WIN
# include "mozilla/gfx/DeviceManagerDx.h"
#endif

#include "LayerScope.h"

namespace mozilla {

namespace layers {

using namespace mozilla::ipc;
using namespace mozilla::gfx;
using namespace std;

using base::ProcessId;
using base::Thread;

CompositorBridgeParentBase::CompositorBridgeParentBase(CompositorManagerParent* aManager)
  : mCanSend(true)
  , mCompositorManager(aManager)
{
}

CompositorBridgeParentBase::~CompositorBridgeParentBase()
{
}

ProcessId
CompositorBridgeParentBase::GetChildProcessId()
{
  return OtherPid();
}

void
CompositorBridgeParentBase::NotifyNotUsed(PTextureParent* aTexture, uint64_t aTransactionId)
{
  RefPtr<TextureHost> texture = TextureHost::AsTextureHost(aTexture);
  if (!texture) {
    return;
  }

  if (!(texture->GetFlags() & TextureFlags::RECYCLE)) {
    return;
  }

  uint64_t textureId = TextureHost::GetTextureSerial(aTexture);
  mPendingAsyncMessage.push_back(
    OpNotifyNotUsed(textureId, aTransactionId));
}

void
CompositorBridgeParentBase::SendAsyncMessage(const InfallibleTArray<AsyncParentMessageData>& aMessage)
{
  Unused << SendParentAsyncMessages(aMessage);
}

bool
CompositorBridgeParentBase::AllocShmem(size_t aSize,
                                   ipc::SharedMemory::SharedMemoryType aType,
                                   ipc::Shmem* aShmem)
{
  return PCompositorBridgeParent::AllocShmem(aSize, aType, aShmem);
}

bool
CompositorBridgeParentBase::AllocUnsafeShmem(size_t aSize,
                                         ipc::SharedMemory::SharedMemoryType aType,
                                         ipc::Shmem* aShmem)
{
  return PCompositorBridgeParent::AllocUnsafeShmem(aSize, aType, aShmem);
}

void
CompositorBridgeParentBase::DeallocShmem(ipc::Shmem& aShmem)
{
  PCompositorBridgeParent::DeallocShmem(aShmem);
}

static inline MessageLoop*
CompositorLoop()
{
  return CompositorThreadHolder::Loop();
}

base::ProcessId
CompositorBridgeParentBase::RemotePid()
{
  return OtherPid();
}

bool
CompositorBridgeParentBase::StartSharingMetrics(ipc::SharedMemoryBasic::Handle aHandle,
                                                CrossProcessMutexHandle aMutexHandle,
                                                LayersId aLayersId,
                                                uint32_t aApzcId)
{
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    MOZ_ASSERT(CompositorLoop());
    CompositorLoop()->PostTask(
      NewRunnableMethod<ipc::SharedMemoryBasic::Handle,
                        CrossProcessMutexHandle,
                        LayersId,
                        uint32_t>(
        "layers::CompositorBridgeParent::StartSharingMetrics",
        this,
        &CompositorBridgeParentBase::StartSharingMetrics,
        aHandle, aMutexHandle, aLayersId, aApzcId));
    return true;
  }

  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCanSend) {
    return false;
  }
  return PCompositorBridgeParent::SendSharedCompositorFrameMetrics(
    aHandle, aMutexHandle, aLayersId, aApzcId);
}

bool
CompositorBridgeParentBase::StopSharingMetrics(FrameMetrics::ViewID aScrollId,
                                               uint32_t aApzcId)
{
  if (!CompositorThreadHolder::IsInCompositorThread()) {
    MOZ_ASSERT(CompositorLoop());
    CompositorLoop()->PostTask(
      NewRunnableMethod<FrameMetrics::ViewID, uint32_t>(
        "layers::CompositorBridgeParent::StopSharingMetrics",
        this,
        &CompositorBridgeParentBase::StopSharingMetrics,
        aScrollId, aApzcId));
    return true;
  }

  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCanSend) {
    return false;
  }
  return PCompositorBridgeParent::SendReleaseSharedCompositorFrameMetrics(
    aScrollId, aApzcId);
}

CompositorBridgeParent::LayerTreeState::LayerTreeState()
  : mApzcTreeManagerParent(nullptr)
  , mParent(nullptr)
  , mLayerManager(nullptr)
  , mCrossProcessParent(nullptr)
  , mLayerTree(nullptr)
  , mUpdatedPluginDataAvailable(false)
{
}

CompositorBridgeParent::LayerTreeState::~LayerTreeState()
{
  if (mController) {
    mController->Destroy();
  }
}

typedef map<LayersId, CompositorBridgeParent::LayerTreeState> LayerTreeMap;
LayerTreeMap sIndirectLayerTrees;
StaticAutoPtr<mozilla::Monitor> sIndirectLayerTreesLock;

static void EnsureLayerTreeMapReady()
{
  MOZ_ASSERT(NS_IsMainThread());
  if (!sIndirectLayerTreesLock) {
    sIndirectLayerTreesLock = new Monitor("IndirectLayerTree");
    mozilla::ClearOnShutdown(&sIndirectLayerTreesLock);
  }
}

template <typename Lambda>
inline void
CompositorBridgeParent::ForEachIndirectLayerTree(const Lambda& aCallback)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  for (auto it = sIndirectLayerTrees.begin(); it != sIndirectLayerTrees.end(); it++) {
    LayerTreeState* state = &it->second;
    if (state->mParent == this) {
      aCallback(state, it->first);
    }
  }
}

/**
  * A global map referencing each compositor by ID.
  *
  * This map is used by the ImageBridge protocol to trigger
  * compositions without having to keep references to the
  * compositor
  */
typedef map<uint64_t,CompositorBridgeParent*> CompositorMap;
static StaticAutoPtr<CompositorMap> sCompositorMap;

void
CompositorBridgeParent::Setup()
{
  EnsureLayerTreeMapReady();

  MOZ_ASSERT(!sCompositorMap);
  sCompositorMap = new CompositorMap;
}

void
CompositorBridgeParent::Shutdown()
{
  MOZ_ASSERT(sCompositorMap);
  MOZ_ASSERT(sCompositorMap->empty());
  sCompositorMap = nullptr;
}

void
CompositorBridgeParent::FinishShutdown()
{
  // TODO: this should be empty by now...
  sIndirectLayerTrees.clear();
}

#ifdef COMPOSITOR_PERFORMANCE_WARNING
static int32_t
CalculateCompositionFrameRate()
{
  // Used when layout.frame_rate is -1. Needs to be kept in sync with
  // DEFAULT_FRAME_RATE in nsRefreshDriver.cpp.
  // TODO: This should actually return the vsync rate.
  const int32_t defaultFrameRate = 60;
  int32_t compositionFrameRatePref = gfxPrefs::LayersCompositionFrameRate();
  if (compositionFrameRatePref < 0) {
    // Use the same frame rate for composition as for layout.
    int32_t layoutFrameRatePref = gfxPrefs::LayoutFrameRate();
    if (layoutFrameRatePref < 0) {
      // TODO: The main thread frame scheduling code consults the actual
      // monitor refresh rate in this case. We should do the same.
      return defaultFrameRate;
    }
    return layoutFrameRatePref;
  }
  return compositionFrameRatePref;
}
#endif

CompositorBridgeParent::CompositorBridgeParent(CompositorManagerParent* aManager,
                                               CSSToLayoutDeviceScale aScale,
                                               const TimeDuration& aVsyncRate,
                                               const CompositorOptions& aOptions,
                                               bool aUseExternalSurfaceSize,
                                               const gfx::IntSize& aSurfaceSize)
  : CompositorBridgeParentBase(aManager)
  , mWidget(nullptr)
  , mScale(aScale)
  , mVsyncRate(aVsyncRate)
  , mPendingTransaction{0}
  , mPaused(false)
  , mUseExternalSurfaceSize(aUseExternalSurfaceSize)
  , mEGLSurfaceSize(aSurfaceSize)
  , mOptions(aOptions)
  , mPauseCompositionMonitor("PauseCompositionMonitor")
  , mResumeCompositionMonitor("ResumeCompositionMonitor")
  , mRootLayerTreeID{0}
  , mOverrideComposeReadiness(false)
  , mForceCompositionTask(nullptr)
  , mCompositorScheduler(nullptr)
  , mAnimationStorage(nullptr)
  , mPaintTime(TimeDuration::Forever())
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  , mLastPluginUpdateLayerTreeId{0}
  , mDeferPluginWindows(false)
  , mPluginWindowsHidden(false)
#endif
{
}

void
CompositorBridgeParent::InitSameProcess(widget::CompositorWidget* aWidget,
                                        const LayersId& aLayerTreeId)
{
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(NS_IsMainThread());

  mWidget = aWidget;
  mRootLayerTreeID = aLayerTreeId;

  Initialize();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvInitialize(const LayersId& aRootLayerTreeId)
{
  MOZ_ASSERT(XRE_IsGPUProcess());

  mRootLayerTreeID = aRootLayerTreeId;

  Initialize();
  return IPC_OK();
}

void
CompositorBridgeParent::Initialize()
{
  MOZ_ASSERT(CompositorThread(),
             "The compositor thread must be Initialized before instanciating a CompositorBridgeParent.");

  if (mOptions.UseAPZ()) {
    MOZ_ASSERT(!mApzcTreeManager);
    MOZ_ASSERT(!mApzSampler);
    MOZ_ASSERT(!mApzUpdater);
    mApzcTreeManager = new APZCTreeManager(mRootLayerTreeID);
    mApzSampler = new APZSampler(mApzcTreeManager, mOptions.UseWebRender());
    mApzUpdater = new APZUpdater(mApzcTreeManager, mOptions.UseWebRender());
  }

  mCompositorBridgeID = 0;
  // FIXME: This holds on the the fact that right now the only thing that
  // can destroy this instance is initialized on the compositor thread after
  // this task has been processed.
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableFunction("AddCompositorRunnable",
                                                 &AddCompositor,
                                                 this, &mCompositorBridgeID));


  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees[mRootLayerTreeID].mParent = this;
  }

  LayerScope::SetPixelScale(mScale.scale);

  if (!mOptions.UseWebRender()) {
    mCompositorScheduler = new CompositorVsyncScheduler(this, mWidget);
  }
}

LayersId
CompositorBridgeParent::RootLayerTreeId()
{
  MOZ_ASSERT(mRootLayerTreeID.IsValid());
  return mRootLayerTreeID;
}

CompositorBridgeParent::~CompositorBridgeParent()
{
  InfallibleTArray<PTextureParent*> textures;
  ManagedPTextureParent(textures);
  // We expect all textures to be destroyed by now.
  MOZ_DIAGNOSTIC_ASSERT(textures.Length() == 0);
  for (unsigned int i = 0; i < textures.Length(); ++i) {
    RefPtr<TextureHost> tex = TextureHost::AsTextureHost(textures[i]);
    tex->DeallocateDeviceData();
  }
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvForceIsFirstPaint()
{
  mCompositionManager->ForceIsFirstPaint();
  return IPC_OK();
}

void
CompositorBridgeParent::StopAndClearResources()
{
  if (mForceCompositionTask) {
    mForceCompositionTask->Cancel();
    mForceCompositionTask = nullptr;
  }

  mPaused = true;

  // We need to clear the APZ tree before we destroy the WebRender API below,
  // because in the case of async scene building that will shut down the updater
  // thread and we need to run the task before that happens.
  MOZ_ASSERT((mApzSampler != nullptr) == (mApzcTreeManager != nullptr));
  MOZ_ASSERT((mApzUpdater != nullptr) == (mApzcTreeManager != nullptr));
  if (mApzUpdater) {
    mApzSampler = nullptr;
    mApzUpdater->ClearTree(mRootLayerTreeID);
    mApzUpdater = nullptr;
    mApzcTreeManager = nullptr;
  }

  // Ensure that the layer manager is destroyed before CompositorBridgeChild.
  if (mLayerManager) {
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    ForEachIndirectLayerTree([this] (LayerTreeState* lts, LayersId) -> void {
      mLayerManager->ClearCachedResources(lts->mRoot);
      lts->mLayerManager = nullptr;
      lts->mParent = nullptr;
    });
    mLayerManager->Destroy();
    mLayerManager = nullptr;
    mCompositionManager = nullptr;
  }

  if (mWrBridge) {
    { // scope lock
      MonitorAutoLock lock(*sIndirectLayerTreesLock);
      ForEachIndirectLayerTree([] (LayerTreeState* lts, LayersId) -> void {
        if (lts->mWrBridge) {
          lts->mWrBridge->Destroy();
          lts->mWrBridge = nullptr;
        }
        lts->mParent = nullptr;
      });
    }

    // Ensure we are not holding the sIndirectLayerTreesLock here because we
    // are going to block on WR threads in order to shut it down properly.
    mWrBridge->Destroy();
    mWrBridge = nullptr;
    if (mAsyncImageManager) {
      mAsyncImageManager->Destroy();
      // WebRenderAPI should be already destructed
      mAsyncImageManager = nullptr;
    }
  }

  if (mCompositor) {
    mCompositor->DetachWidget();
    mCompositor->Destroy();
    mCompositor = nullptr;
  }

  // This must be destroyed now since it accesses the widget.
  if (mCompositorScheduler) {
    mCompositorScheduler->Destroy();
    mCompositorScheduler = nullptr;
  }

  // After this point, it is no longer legal to access the widget.
  mWidget = nullptr;

  // Clear mAnimationStorage here to ensure that the compositor thread
  // still exists when we destroy it.
  mAnimationStorage = nullptr;
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvWillClose()
{
  StopAndClearResources();
  // Once we get the WillClose message, the client side is going to go away
  // soon and we can't be guaranteed that sending messages will work.
  mCanSend = false;
  return IPC_OK();
}

void CompositorBridgeParent::DeferredDestroy()
{
  MOZ_ASSERT(!NS_IsMainThread());
  mSelfRef = nullptr;
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvPause()
{
  PauseComposition();
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvResume()
{
  ResumeComposition();
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvMakeSnapshot(const SurfaceDescriptor& aInSnapshot,
                                         const gfx::IntRect& aRect)
{
  RefPtr<DrawTarget> target = GetDrawTargetForDescriptor(aInSnapshot, gfx::BackendType::CAIRO);
  MOZ_ASSERT(target);
  if (!target) {
    // We kill the content process rather than have it continue with an invalid
    // snapshot, that may be too harsh and we could decide to return some sort
    // of error to the child process and let it deal with it...
    return IPC_FAIL_NO_REASON(this);
  }
  ForceComposeToTarget(target, &aRect);
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvWaitOnTransactionProcessed()
{
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvFlushRendering()
{
  if (mWrBridge) {
    mWrBridge->FlushRendering();
    return IPC_OK();
  }

  if (mCompositorScheduler->NeedsComposite()) {
    CancelCurrentCompositeTask();
    ForceComposeToTarget(nullptr);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvFlushRenderingAsync()
{
  if (mWrBridge) {
    mWrBridge->FlushRenderingAsync();
    return IPC_OK();
  }

  return RecvFlushRendering();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvForcePresent()
{
  if (mWrBridge) {
    mWrBridge->ScheduleGenerateFrame();
  }
  // During the shutdown sequence mLayerManager may be null
  if (mLayerManager) {
    mLayerManager->ForcePresent();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvNotifyRegionInvalidated(const nsIntRegion& aRegion)
{
  if (mLayerManager) {
    mLayerManager->AddInvalidRegion(aRegion);
  }
  return IPC_OK();
}

void
CompositorBridgeParent::Invalidate()
{
  if (mLayerManager && mLayerManager->GetRoot()) {
    mLayerManager->AddInvalidRegion(
                                    mLayerManager->GetRoot()->GetLocalVisibleRegion().ToUnknownRegion().GetBounds());
  }
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvStartFrameTimeRecording(const int32_t& aBufferSize, uint32_t* aOutStartIndex)
{
  if (mLayerManager) {
    *aOutStartIndex = mLayerManager->StartFrameTimeRecording(aBufferSize);
  } else {
    *aOutStartIndex = 0;
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvStopFrameTimeRecording(const uint32_t& aStartIndex,
                                                   InfallibleTArray<float>* intervals)
{
  if (mLayerManager) {
    mLayerManager->StopFrameTimeRecording(aStartIndex, *intervals);
  }
  return IPC_OK();
}

void
CompositorBridgeParent::ActorDestroy(ActorDestroyReason why)
{
  mCanSend = false;

  StopAndClearResources();

  RemoveCompositor(mCompositorBridgeID);

  mCompositionManager = nullptr;

  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    sIndirectLayerTrees.erase(mRootLayerTreeID);
  }

  // There are chances that the ref count reaches zero on the main thread shortly
  // after this function returns while some ipdl code still needs to run on
  // this thread.
  // We must keep the compositor parent alive untill the code handling message
  // reception is finished on this thread.
  mSelfRef = this;
  MessageLoop::current()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::DeferredDestroy",
                      this,
                      &CompositorBridgeParent::DeferredDestroy));
}

void
CompositorBridgeParent::ScheduleRenderOnCompositorThread()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::ScheduleComposition",
                      this,
                      &CompositorBridgeParent::ScheduleComposition));
}

void
CompositorBridgeParent::InvalidateOnCompositorThread()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::Invalidate",
                      this,
                      &CompositorBridgeParent::Invalidate));
}

void
CompositorBridgeParent::PauseComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "PauseComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mPauseCompositionMonitor);

  if (!mPaused) {
    mPaused = true;

    if (mCompositor) {
      mCompositor->Pause();
    } else if (mWrBridge) {
      mWrBridge->Pause();
    }
    TimeStamp now = TimeStamp::Now();
    DidComposite(now, now);
  }

  // if anyone's waiting to make sure that composition really got paused, tell them
  lock.NotifyAll();
}

void
CompositorBridgeParent::ResumeComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "ResumeComposition() can only be called on the compositor thread");

  MonitorAutoLock lock(mResumeCompositionMonitor);

  bool resumed = mOptions.UseWebRender() ? mWrBridge->Resume() : mCompositor->Resume();
  if (!resumed) {
#ifdef MOZ_WIDGET_ANDROID
    // We can't get a surface. This could be because the activity changed between
    // the time resume was scheduled and now.
    __android_log_print(ANDROID_LOG_INFO, "CompositorBridgeParent", "Unable to renew compositor surface; remaining in paused state");
#endif
    lock.NotifyAll();
    return;
  }

  mPaused = false;

  Invalidate();
  mCompositorScheduler->ForceComposeToTarget(nullptr, nullptr);

  // if anyone's waiting to make sure that composition really got resumed, tell them
  lock.NotifyAll();
}

void
CompositorBridgeParent::ForceComposition()
{
  // Cancel the orientation changed state to force composition
  mForceCompositionTask = nullptr;
  ScheduleRenderOnCompositorThread();
}

void
CompositorBridgeParent::CancelCurrentCompositeTask()
{
  mCompositorScheduler->CancelCurrentCompositeTask();
}

void
CompositorBridgeParent::SetEGLSurfaceSize(int width, int height)
{
  NS_ASSERTION(mUseExternalSurfaceSize, "Compositor created without UseExternalSurfaceSize provided");
  mEGLSurfaceSize.SizeTo(width, height);
  if (mCompositor) {
    mCompositor->SetDestinationSurfaceSize(gfx::IntSize(mEGLSurfaceSize.width, mEGLSurfaceSize.height));
  }
}

void
CompositorBridgeParent::ResumeCompositionAndResize(int width, int height)
{
  SetEGLSurfaceSize(width, height);
  ResumeComposition();
}

/*
 * This will execute a pause synchronously, waiting to make sure that the compositor
 * really is paused.
 */
void
CompositorBridgeParent::SchedulePauseOnCompositorThread()
{
  MonitorAutoLock lock(mPauseCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::PauseComposition",
                      this,
                      &CompositorBridgeParent::PauseComposition));

  // Wait until the pause has actually been processed by the compositor thread
  lock.Wait();
}

bool
CompositorBridgeParent::ScheduleResumeOnCompositorThread()
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::ResumeComposition",
                      this,
                      &CompositorBridgeParent::ResumeComposition));

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

bool
CompositorBridgeParent::ScheduleResumeOnCompositorThread(int width, int height)
{
  MonitorAutoLock lock(mResumeCompositionMonitor);

  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(NewRunnableMethod<int, int>(
    "layers::CompositorBridgeParent::ResumeCompositionAndResize",
    this,
    &CompositorBridgeParent::ResumeCompositionAndResize,
    width,
    height));

  // Wait until the resume has actually been processed by the compositor thread
  lock.Wait();

  return !mPaused;
}

void
CompositorBridgeParent::ScheduleTask(already_AddRefed<CancelableRunnable> task, int time)
{
  if (time == 0) {
    MessageLoop::current()->PostTask(Move(task));
  } else {
    MessageLoop::current()->PostDelayedTask(Move(task), time);
  }
}

void
CompositorBridgeParent::UpdatePaintTime(LayerTransactionParent* aLayerTree,
                                        const TimeDuration& aPaintTime)
{
  // We get a lot of paint timings for things with empty transactions.
  if (!mLayerManager || aPaintTime.ToMilliseconds() < 1.0) {
    return;
  }

  mLayerManager->SetPaintTime(aPaintTime);
}

void
CompositorBridgeParent::NotifyShadowTreeTransaction(LayersId aId, bool aIsFirstPaint,
    const FocusTarget& aFocusTarget,
    bool aScheduleComposite, uint32_t aPaintSequenceNumber,
    bool aIsRepeatTransaction, bool aHitTestUpdate)
{
  if (!aIsRepeatTransaction &&
      mLayerManager &&
      mLayerManager->GetRoot()) {
    // Process plugin data here to give time for them to update before the next
    // composition.
    bool pluginsUpdatedFlag = true;
    AutoResolveRefLayers resolve(mCompositionManager, this, nullptr,
                                 &pluginsUpdatedFlag);

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
    // If plugins haven't been updated, stop waiting.
    if (!pluginsUpdatedFlag) {
      mWaitForPluginsUntil = TimeStamp();
      mHaveBlockedForPlugins = false;
    }
#endif

    if (mApzUpdater) {
      mApzUpdater->UpdateFocusState(mRootLayerTreeID, aId, aFocusTarget);
      if (aHitTestUpdate) {
        mApzUpdater->UpdateHitTestingTree(mRootLayerTreeID,
            mLayerManager->GetRoot(), aIsFirstPaint, aId, aPaintSequenceNumber);
      }
    }

    mLayerManager->NotifyShadowTreeTransaction();
  }
  if (aScheduleComposite) {
    ScheduleComposition();
  }
}

void
CompositorBridgeParent::ScheduleComposition()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (mPaused) {
    return;
  }

  if (mWrBridge) {
    mWrBridge->ScheduleGenerateFrame();
  } else {
    mCompositorScheduler->ScheduleComposition();
  }
}

// Go down the composite layer tree, setting properties to match their
// content-side counterparts.
/* static */ void
CompositorBridgeParent::SetShadowProperties(Layer* aLayer)
{
  ForEachNode<ForwardIterator>(
      aLayer,
      [] (Layer *layer)
      {
        if (Layer* maskLayer = layer->GetMaskLayer()) {
          SetShadowProperties(maskLayer);
        }
        for (size_t i = 0; i < layer->GetAncestorMaskLayerCount(); i++) {
          SetShadowProperties(layer->GetAncestorMaskLayerAt(i));
        }

        // FIXME: Bug 717688 -- Do these updates in LayerTransactionParent::RecvUpdate.
        HostLayer* layerCompositor = layer->AsHostLayer();
        // Set the layerComposite's base transform to the layer's base transform.
        AnimationArray& animations = layer->GetAnimations();
        // If there is any animation, the animation value will override
        // non-animated value later, so we don't need to set the non-animated
        // value here.
        if (animations.IsEmpty()) {
          layerCompositor->SetShadowBaseTransform(layer->GetBaseTransform());
          layerCompositor->SetShadowTransformSetByAnimation(false);
          layerCompositor->SetShadowOpacity(layer->GetOpacity());
          layerCompositor->SetShadowOpacitySetByAnimation(false);
        }
        layerCompositor->SetShadowVisibleRegion(layer->GetVisibleRegion());
        layerCompositor->SetShadowClipRect(layer->GetClipRect());
      }
    );
}

void
CompositorBridgeParent::CompositeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  AUTO_PROFILER_TRACING("Paint", "Composite");
  AUTO_PROFILER_LABEL("CompositorBridgeParent::CompositeToTarget", GRAPHICS);

  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread(),
             "Composite can only be called on the compositor thread");
  TimeStamp start = TimeStamp::Now();

  if (!CanComposite()) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
    return;
  }

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  if (!mWaitForPluginsUntil.IsNull() &&
      mWaitForPluginsUntil > start) {
    mHaveBlockedForPlugins = true;
    ScheduleComposition();
    return;
  }
#endif

  /*
   * AutoResolveRefLayers handles two tasks related to Windows and Linux
   * plugin window management:
   * 1) calculating if we have remote content in the view. If we do not have
   * remote content, all plugin windows for this CompositorBridgeParent (window)
   * can be hidden since we do not support plugins in chrome when running
   * under e10s.
   * 2) Updating plugin position, size, and clip. We do this here while the
   * remote layer tree is hooked up to to chrome layer tree. This is needed
   * since plugin clipping can depend on chrome (for example, due to tab modal
   * prompts). Updates in step 2 are applied via an async ipc message sent
   * to the main thread.
   */
  bool hasRemoteContent = false;
  bool updatePluginsFlag = true;
  AutoResolveRefLayers resolve(mCompositionManager, this,
                               &hasRemoteContent,
                               &updatePluginsFlag);

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  // We do not support plugins in local content. When switching tabs
  // to local pages, hide every plugin associated with the window.
  if (!hasRemoteContent && gfxVars::BrowserTabsRemoteAutostart() &&
      mCachedPluginData.Length()) {
    Unused << SendHideAllPlugins(GetWidget()->GetWidgetKey());
    mCachedPluginData.Clear();
  }
#endif

  if (aTarget) {
    mLayerManager->BeginTransactionWithDrawTarget(aTarget, *aRect);
  } else {
    mLayerManager->BeginTransaction();
  }

  SetShadowProperties(mLayerManager->GetRoot());

  if (mForceCompositionTask && !mOverrideComposeReadiness) {
    if (mCompositionManager->ReadyForCompose()) {
      mForceCompositionTask->Cancel();
      mForceCompositionTask = nullptr;
    } else {
      return;
    }
  }

  mCompositionManager->ComputeRotation();

  TimeStamp time = mTestTime.valueOr(mCompositorScheduler->GetLastComposeTime());
  bool requestNextFrame = mCompositionManager->TransformShadowTree(time, mVsyncRate);
  if (requestNextFrame) {
    ScheduleComposition();
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
    // If we have visible windowed plugins then we need to wait for content (and
    // then the plugins) to have been updated by the active animation.
    if (!mPluginWindowsHidden && mCachedPluginData.Length()) {
      mWaitForPluginsUntil = mCompositorScheduler->GetLastComposeTime() + (mVsyncRate * 2);
    }
#endif
  }

  RenderTraceLayers(mLayerManager->GetRoot(), "0000");

#ifdef MOZ_DUMP_PAINTING
  if (gfxPrefs::DumpHostLayers()) {
    printf_stderr("Painting --- compositing layer tree:\n");
    mLayerManager->Dump(/* aSorted = */ true);
  }
#endif
  mLayerManager->SetDebugOverlayWantsNextFrame(false);
  mLayerManager->EndTransaction(time);

  if (!aTarget) {
    TimeStamp end = TimeStamp::Now();
    DidComposite(start, end);
  }

  // We're not really taking advantage of the stored composite-again-time here.
  // We might be able to skip the next few composites altogether. However,
  // that's a bit complex to implement and we'll get most of the advantage
  // by skipping compositing when we detect there's nothing invalid. This is why
  // we do "composite until" rather than "composite again at".
  //
  // TODO(bug 1328602) Figure out what we should do here with the render thread.
  if (!mLayerManager->GetCompositeUntilTime().IsNull() ||
      mLayerManager->DebugOverlayWantsNextFrame())
  {
      ScheduleComposition();
  }

#ifdef COMPOSITOR_PERFORMANCE_WARNING
  TimeDuration executionTime = TimeStamp::Now() - mCompositorScheduler->GetLastComposeTime();
  TimeDuration frameBudget = TimeDuration::FromMilliseconds(15);
  int32_t frameRate = CalculateCompositionFrameRate();
  if (frameRate > 0) {
    frameBudget = TimeDuration::FromSeconds(1.0 / frameRate);
  }
  if (executionTime > frameBudget) {
    printf_stderr("Compositor: Composite execution took %4.1f ms\n",
                  executionTime.ToMilliseconds());
  }
#endif

  // 0 -> Full-tilt composite
  if (gfxPrefs::LayersCompositionFrameRate() == 0 ||
      mLayerManager->AlwaysScheduleComposite())
  {
    // Special full-tilt composite mode for performance testing
    ScheduleComposition();
  }

  // TODO(bug 1328602) Need an equivalent that works with the rende thread.
  mLayerManager->SetCompositionTime(TimeStamp());

  mozilla::Telemetry::AccumulateTimeDelta(mozilla::Telemetry::COMPOSITE_TIME, start);
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvRemotePluginsReady()
{
#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
  mWaitForPluginsUntil = TimeStamp();
  if (mHaveBlockedForPlugins) {
    mHaveBlockedForPlugins = false;
    ForceComposeToTarget(nullptr);
  } else {
    ScheduleComposition();
  }
  return IPC_OK();
#else
  NS_NOTREACHED("CompositorBridgeParent::RecvRemotePluginsReady calls "
                "unexpected on this platform.");
  return IPC_FAIL_NO_REASON(this);
#endif
}

void
CompositorBridgeParent::ForceComposeToTarget(DrawTarget* aTarget, const gfx::IntRect* aRect)
{
  AUTO_PROFILER_LABEL("CompositorBridgeParent::ForceComposeToTarget", GRAPHICS);

  AutoRestore<bool> override(mOverrideComposeReadiness);
  mOverrideComposeReadiness = true;
  mCompositorScheduler->ForceComposeToTarget(aTarget, aRect);
}

PAPZCTreeManagerParent*
CompositorBridgeParent::AllocPAPZCTreeManagerParent(const LayersId& aLayersId)
{
  // This should only ever get called in the GPU process.
  MOZ_ASSERT(XRE_IsGPUProcess());
  // We should only ever get this if APZ is enabled in this compositor.
  MOZ_ASSERT(mOptions.UseAPZ());
  // The mApzcTreeManager and mApzUpdater should have been created via RecvInitialize()
  MOZ_ASSERT(mApzcTreeManager);
  MOZ_ASSERT(mApzUpdater);
  // The main process should pass in 0 because we assume mRootLayerTreeID
  MOZ_ASSERT(!aLayersId.IsValid());

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& state = sIndirectLayerTrees[mRootLayerTreeID];
  MOZ_ASSERT(state.mParent.get() == this);
  MOZ_ASSERT(!state.mApzcTreeManagerParent);
  state.mApzcTreeManagerParent = new APZCTreeManagerParent(mRootLayerTreeID, mApzcTreeManager, mApzUpdater);

  return state.mApzcTreeManagerParent;
}

bool
CompositorBridgeParent::DeallocPAPZCTreeManagerParent(PAPZCTreeManagerParent* aActor)
{
  delete aActor;
  return true;
}

void
CompositorBridgeParent::AllocateAPZCTreeManagerParent(const MonitorAutoLock& aProofOfLayerTreeStateLock,
                                                      const LayersId& aLayersId,
                                                      LayerTreeState& aState)
{
  MOZ_ASSERT(aState.mParent == this);
  MOZ_ASSERT(mApzcTreeManager);
  MOZ_ASSERT(mApzUpdater);
  MOZ_ASSERT(!aState.mApzcTreeManagerParent);
  aState.mApzcTreeManagerParent = new APZCTreeManagerParent(aLayersId, mApzcTreeManager, mApzUpdater);
}

PAPZParent*
CompositorBridgeParent::AllocPAPZParent(const LayersId& aLayersId)
{
  // The main process should pass in 0 because we assume mRootLayerTreeID
  MOZ_ASSERT(!aLayersId.IsValid());

  RemoteContentController* controller = new RemoteContentController();

  // Increment the controller's refcount before we return it. This will keep the
  // controller alive until it is released by IPDL in DeallocPAPZParent.
  controller->AddRef();

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& state = sIndirectLayerTrees[mRootLayerTreeID];
  MOZ_ASSERT(!state.mController);
  state.mController = controller;

  return controller;
}

bool
CompositorBridgeParent::DeallocPAPZParent(PAPZParent* aActor)
{
  RemoteContentController* controller = static_cast<RemoteContentController*>(aActor);
  controller->Release();
  return true;
}

#if defined(MOZ_WIDGET_ANDROID)
AndroidDynamicToolbarAnimator*
CompositorBridgeParent::GetAndroidDynamicToolbarAnimator()
{
  return mApzcTreeManager ? mApzcTreeManager->GetAndroidDynamicToolbarAnimator() : nullptr;
}
#endif

RefPtr<APZSampler>
CompositorBridgeParent::GetAPZSampler()
{
  return mApzSampler;
}

RefPtr<APZUpdater>
CompositorBridgeParent::GetAPZUpdater()
{
  return mApzUpdater;
}

CompositorBridgeParent*
CompositorBridgeParent::GetCompositorBridgeParentFromLayersId(const LayersId& aLayersId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  return sIndirectLayerTrees[aLayersId].mParent;
}

/*static*/ RefPtr<CompositorBridgeParent>
CompositorBridgeParent::GetCompositorBridgeParentFromWindowId(const wr::WindowId& aWindowId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  for (auto it = sIndirectLayerTrees.begin(); it != sIndirectLayerTrees.end(); it++) {
    LayerTreeState* state = &it->second;
    if (!state->mWrBridge) {
      continue;
    }
    // state->mWrBridge might be a root WebRenderBridgeParent or one of a content
    // process, but in either case the state->mParent will be the same. So we
    // don't need to distinguish between the two.
    if (RefPtr<wr::WebRenderAPI> api = state->mWrBridge->GetWebRenderAPI()) {
      if (api->GetId() == aWindowId) {
        return state->mParent;
      }
    }
  }
  return nullptr;
}

bool
CompositorBridgeParent::CanComposite()
{
  return mLayerManager &&
         mLayerManager->GetRoot() &&
         !mPaused;
}

void
CompositorBridgeParent::ScheduleRotationOnCompositorThread(const TargetConfig& aTargetConfig,
                                                     bool aIsFirstPaint)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  if (!aIsFirstPaint &&
      !mCompositionManager->IsFirstPaint() &&
      mCompositionManager->RequiresReorientation(aTargetConfig.orientation())) {
    if (mForceCompositionTask != nullptr) {
      mForceCompositionTask->Cancel();
    }
    RefPtr<CancelableRunnable> task = NewCancelableRunnableMethod(
      "layers::CompositorBridgeParent::ForceComposition",
      this,
      &CompositorBridgeParent::ForceComposition);
    mForceCompositionTask = task;
    ScheduleTask(task.forget(), gfxPrefs::OrientationSyncMillis());
  }
}

void
CompositorBridgeParent::ShadowLayersUpdated(LayerTransactionParent* aLayerTree,
                                            const TransactionInfo& aInfo,
                                            bool aHitTestUpdate)
{
  const TargetConfig& targetConfig = aInfo.targetConfig();

  ScheduleRotationOnCompositorThread(targetConfig, aInfo.isFirstPaint());

  // Instruct the LayerManager to update its render bounds now. Since all the orientation
  // change, dimension change would be done at the stage, update the size here is free of
  // race condition.
  mLayerManager->UpdateRenderBounds(targetConfig.naturalBounds());
  mLayerManager->SetRegionToClear(targetConfig.clearRegion());
  if (mLayerManager->GetCompositor()) {
    mLayerManager->GetCompositor()->SetScreenRotation(targetConfig.rotation());
  }

  mCompositionManager->Updated(aInfo.isFirstPaint(), targetConfig);
  Layer* root = aLayerTree->GetRoot();
  mLayerManager->SetRoot(root);

  if (mApzUpdater && !aInfo.isRepeatTransaction()) {
    mApzUpdater->UpdateFocusState(mRootLayerTreeID,
                                  mRootLayerTreeID,
                                  aInfo.focusTarget());

    if (aHitTestUpdate) {
      AutoResolveRefLayers resolve(mCompositionManager);

      mApzUpdater->UpdateHitTestingTree(
        mRootLayerTreeID, root, aInfo.isFirstPaint(),
        mRootLayerTreeID, aInfo.paintSequenceNumber());
    }
  }

  // The transaction ID might get reset to 1 if the page gets reloaded, see
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1145295#c41
  // Otherwise, it should be continually increasing.
  MOZ_ASSERT(aInfo.id() == TransactionId{1} || aInfo.id() > mPendingTransaction);
  mPendingTransaction = aInfo.id();
  mTxnStartTime = aInfo.transactionStart();
  mFwdTime = aInfo.fwdTime();

  if (root) {
    SetShadowProperties(root);
  }
  if (aInfo.scheduleComposite()) {
    ScheduleComposition();
    if (mPaused) {
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
  mLayerManager->NotifyShadowTreeTransaction();
}

void
CompositorBridgeParent::ScheduleComposite(LayerTransactionParent* aLayerTree)
{
  ScheduleComposition();
}

bool
CompositorBridgeParent::SetTestSampleTime(const LayersId& aId,
                                          const TimeStamp& aTime)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  if (aTime.IsNull()) {
    return false;
  }

  mTestTime = Some(aTime);

  if (mWrBridge) {
    mWrBridge->FlushRendering();
    return true;
  }

  bool testComposite = mCompositionManager &&
                       mCompositorScheduler->NeedsComposite();

  // Update but only if we were already scheduled to animate
  if (testComposite) {
    AutoResolveRefLayers resolve(mCompositionManager);
    bool requestNextFrame = mCompositionManager->TransformShadowTree(aTime, mVsyncRate);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is wating for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }

  return true;
}

void
CompositorBridgeParent::LeaveTestMode(const LayersId& aId)
{
  mTestTime = Nothing();
}

void
CompositorBridgeParent::ApplyAsyncProperties(LayerTransactionParent* aLayerTree)
{
  // NOTE: This should only be used for testing. For example, when mTestTime is
  // non-empty, or when called from test-only methods like
  // LayerTransactionParent::RecvGetAnimationTransform.

  // Synchronously update the layer tree
  if (aLayerTree->GetRoot()) {
    AutoResolveRefLayers resolve(mCompositionManager);
    SetShadowProperties(mLayerManager->GetRoot());

    TimeStamp time = mTestTime.valueOr(mCompositorScheduler->GetLastComposeTime());
    bool requestNextFrame =
      mCompositionManager->TransformShadowTree(time, mVsyncRate,
        AsyncCompositionManager::TransformsToSkip::APZ);
    if (!requestNextFrame) {
      CancelCurrentCompositeTask();
      // Pretend we composited in case someone is waiting for this event.
      TimeStamp now = TimeStamp::Now();
      DidComposite(now, now);
    }
  }
}

CompositorAnimationStorage*
CompositorBridgeParent::GetAnimationStorage()
{
  if (!mAnimationStorage) {
    mAnimationStorage = new CompositorAnimationStorage();
  }
  return mAnimationStorage;
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvGetFrameUniformity(FrameUniformityData* aOutData)
{
  mCompositionManager->GetFrameUniformity(aOutData);
  return IPC_OK();
}

void
CompositorBridgeParent::SetTestAsyncScrollOffset(
    const LayersId& aLayersId,
    const FrameMetrics::ViewID& aScrollId,
    const CSSPoint& aPoint)
{
  if (mApzUpdater) {
    MOZ_ASSERT(aLayersId.IsValid());
    mApzUpdater->SetTestAsyncScrollOffset(aLayersId, aScrollId, aPoint);
  }
}

void
CompositorBridgeParent::SetTestAsyncZoom(
    const LayersId& aLayersId,
    const FrameMetrics::ViewID& aScrollId,
    const LayerToParentLayerScale& aZoom)
{
  if (mApzUpdater) {
    MOZ_ASSERT(aLayersId.IsValid());
    mApzUpdater->SetTestAsyncZoom(aLayersId, aScrollId, aZoom);
  }
}

void
CompositorBridgeParent::FlushApzRepaints(const LayersId& aLayersId)
{
  MOZ_ASSERT(mApzcTreeManager);
  MOZ_ASSERT(mApzUpdater);
  MOZ_ASSERT(aLayersId.IsValid());
  RefPtr<CompositorBridgeParent> self = this;
  mApzUpdater->RunOnControllerThread(aLayersId, NS_NewRunnableFunction(
    "layers::CompositorBridgeParent::FlushApzRepaints",
    [=]() { self->mApzcTreeManager->FlushApzRepaints(aLayersId); }));
}

void
CompositorBridgeParent::GetAPZTestData(const LayersId& aLayersId,
                                       APZTestData* aOutData)
{
  if (mApzUpdater) {
    MOZ_ASSERT(aLayersId.IsValid());
    mApzUpdater->GetAPZTestData(aLayersId, aOutData);
  }
}

void
CompositorBridgeParent::SetConfirmedTargetAPZC(const LayersId& aLayersId,
                                               const uint64_t& aInputBlockId,
                                               const nsTArray<ScrollableLayerGuid>& aTargets)
{
  if (!mApzcTreeManager || !mApzUpdater) {
    return;
  }
  // Need to specifically bind this since it's overloaded.
  void (APZCTreeManager::*setTargetApzcFunc)
        (uint64_t, const nsTArray<ScrollableLayerGuid>&) =
        &APZCTreeManager::SetTargetAPZC;
  RefPtr<Runnable> task =
    NewRunnableMethod<uint64_t,
                      StoreCopyPassByConstLRef<nsTArray<ScrollableLayerGuid>>>(
      "layers::CompositorBridgeParent::SetConfirmedTargetAPZC",
      mApzcTreeManager.get(),
      setTargetApzcFunc,
      aInputBlockId,
      aTargets);
  mApzUpdater->RunOnControllerThread(aLayersId, task.forget());
}

void
CompositorBridgeParent::InitializeLayerManager(const nsTArray<LayersBackend>& aBackendHints)
{
  NS_ASSERTION(!mLayerManager, "Already initialised mLayerManager");
  NS_ASSERTION(!mCompositor,   "Already initialised mCompositor");

  if (!InitializeAdvancedLayers(aBackendHints, nullptr)) {
    mCompositor = NewCompositor(aBackendHints);
    if (!mCompositor) {
      return;
    }
    mLayerManager = new LayerManagerComposite(mCompositor);
  }
  mLayerManager->SetCompositorBridgeID(mCompositorBridgeID);

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[mRootLayerTreeID].mLayerManager = mLayerManager;
}

bool
CompositorBridgeParent::InitializeAdvancedLayers(const nsTArray<LayersBackend>& aBackendHints,
                                                 TextureFactoryIdentifier* aOutIdentifier)
{
#ifdef XP_WIN
  if (!mOptions.UseAdvancedLayers()) {
    return false;
  }

  // Currently LayerManagerMLGPU hardcodes a D3D11 device, so we reject using
  // AL if LAYERS_D3D11 isn't in the backend hints.
  if (!aBackendHints.Contains(LayersBackend::LAYERS_D3D11)) {
    return false;
  }

  RefPtr<LayerManagerMLGPU> manager = new LayerManagerMLGPU(mWidget);
  if (!manager->Initialize()) {
    return false;
  }

  if (aOutIdentifier) {
    *aOutIdentifier = manager->GetTextureFactoryIdentifier();
  }
  mLayerManager = manager;
  return true;
#else
  return false;
#endif
}

RefPtr<Compositor>
CompositorBridgeParent::NewCompositor(const nsTArray<LayersBackend>& aBackendHints)
{
  for (size_t i = 0; i < aBackendHints.Length(); ++i) {
    RefPtr<Compositor> compositor;
    if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL) {
      compositor = new CompositorOGL(this,
                                     mWidget,
                                     mEGLSurfaceSize.width,
                                     mEGLSurfaceSize.height,
                                     mUseExternalSurfaceSize);
    } else if (aBackendHints[i] == LayersBackend::LAYERS_BASIC) {
#ifdef MOZ_WIDGET_GTK
      if (gfxVars::UseXRender()) {
        compositor = new X11BasicCompositor(this, mWidget);
      } else
#endif
      {
        compositor = new BasicCompositor(this, mWidget);
      }
#ifdef XP_WIN
    } else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11) {
      compositor = new CompositorD3D11(this, mWidget);
#endif
    }
    nsCString failureReason;

    // Some software GPU emulation implementations will happily try to create
    // unreasonably big surfaces and then fail in awful ways.
    // Let's at least limit this to the default max texture size we use for content,
    // anything larger than that will fail to render on the content side anyway.
    // We can revisit this value and make it even tighter if need be.
    const int max_fb_size = 32767;
    const LayoutDeviceIntSize size = mWidget->GetClientSize();
    if (size.width > max_fb_size || size.height > max_fb_size) {
      failureReason = "FEATURE_FAILURE_MAX_FRAMEBUFFER_SIZE";
      return nullptr;
    }

    MOZ_ASSERT(!gfxVars::UseWebRender() || aBackendHints[i] == LayersBackend::LAYERS_BASIC);
    if (compositor && compositor->Initialize(&failureReason)) {
      if (failureReason.IsEmpty()){
        failureReason = "SUCCESS";
      }

      // should only report success here
      if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL){
        Telemetry::Accumulate(Telemetry::OPENGL_COMPOSITING_FAILURE_ID, failureReason);
      }
#ifdef XP_WIN
      else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11){
        Telemetry::Accumulate(Telemetry::D3D11_COMPOSITING_FAILURE_ID, failureReason);
      }
#endif

      return compositor;
    }

    // report any failure reasons here
    if (aBackendHints[i] == LayersBackend::LAYERS_OPENGL){
      gfxCriticalNote << "[OPENGL] Failed to init compositor with reason: "
                      << failureReason.get();
      Telemetry::Accumulate(Telemetry::OPENGL_COMPOSITING_FAILURE_ID, failureReason);
    }
#ifdef XP_WIN
    else if (aBackendHints[i] == LayersBackend::LAYERS_D3D11){
      gfxCriticalNote << "[D3D11] Failed to init compositor with reason: "
                      << failureReason.get();
      Telemetry::Accumulate(Telemetry::D3D11_COMPOSITING_FAILURE_ID, failureReason);
    }
#endif
  }

  return nullptr;
}

PLayerTransactionParent*
CompositorBridgeParent::AllocPLayerTransactionParent(const nsTArray<LayersBackend>& aBackendHints,
                                                     const LayersId& aId)
{
  MOZ_ASSERT(!aId.IsValid());

  InitializeLayerManager(aBackendHints);

  if (!mLayerManager) {
    NS_WARNING("Failed to initialise Compositor");
    LayerTransactionParent* p = new LayerTransactionParent(/* aManager */ nullptr, this, /* aAnimStorage */ nullptr, mRootLayerTreeID);
    p->AddIPDLReference();
    return p;
  }

  mCompositionManager = new AsyncCompositionManager(this, mLayerManager);

  LayerTransactionParent* p = new LayerTransactionParent(mLayerManager, this, GetAnimationStorage(), mRootLayerTreeID);
  p->AddIPDLReference();
  return p;
}

bool
CompositorBridgeParent::DeallocPLayerTransactionParent(PLayerTransactionParent* actor)
{
  static_cast<LayerTransactionParent*>(actor)->ReleaseIPDLReference();
  return true;
}

CompositorBridgeParent* CompositorBridgeParent::GetCompositorBridgeParent(uint64_t id)
{
  MOZ_RELEASE_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  CompositorMap::iterator it = sCompositorMap->find(id);
  return it != sCompositorMap->end() ? it->second : nullptr;
}

void CompositorBridgeParent::AddCompositor(CompositorBridgeParent* compositor, uint64_t* outID)
{
  MOZ_RELEASE_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  static uint64_t sNextID = 1;

  ++sNextID;
  (*sCompositorMap)[sNextID] = compositor;
  *outID = sNextID;
}

CompositorBridgeParent* CompositorBridgeParent::RemoveCompositor(uint64_t id)
{
  MOZ_RELEASE_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  CompositorMap::iterator it = sCompositorMap->find(id);
  if (it == sCompositorMap->end()) {
    return nullptr;
  }
  CompositorBridgeParent *retval = it->second;
  sCompositorMap->erase(it);
  return retval;
}

void
CompositorBridgeParent::NotifyVsync(const TimeStamp& aTimeStamp, const LayersId& aLayersId)
{
  MOZ_ASSERT(XRE_GetProcessType() == GeckoProcessType_GPU);
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  auto it = sIndirectLayerTrees.find(aLayersId);
  if (it == sIndirectLayerTrees.end())
    return;

  CompositorBridgeParent* cbp = it->second.mParent;
  if (!cbp || !cbp->mWidget)
    return;

  RefPtr<VsyncObserver> obs = cbp->mWidget->GetVsyncObserver();
  if (!obs)
    return;

  obs->NotifyVsync(aTimeStamp);
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvNotifyChildCreated(const LayersId& child,
                                               CompositorOptions* aOptions)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NotifyChildCreated(child);
  *aOptions = mOptions;
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvNotifyChildRecreated(const LayersId& aChild,
                                                 CompositorOptions* aOptions)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);

  if (sIndirectLayerTrees.find(aChild) != sIndirectLayerTrees.end()) {
    NS_WARNING("Invalid to register the same layer tree twice");
    return IPC_FAIL_NO_REASON(this);
  }

  NotifyChildCreated(aChild);
  *aOptions = mOptions;
  return IPC_OK();
}

void
CompositorBridgeParent::NotifyChildCreated(LayersId aChild)
{
  sIndirectLayerTreesLock->AssertCurrentThreadOwns();
  sIndirectLayerTrees[aChild].mParent = this;
  sIndirectLayerTrees[aChild].mLayerManager = mLayerManager;
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvMapAndNotifyChildCreated(const LayersId& aChild,
                                                     const base::ProcessId& aOwnerPid,
                                                     CompositorOptions* aOptions)
{
  // We only use this message when the remote compositor is in the GPU process.
  // It is harmless to call it, though.
  MOZ_ASSERT(XRE_IsGPUProcess());

  LayerTreeOwnerTracker::Get()->Map(aChild, aOwnerPid);

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  NotifyChildCreated(aChild);
  *aOptions = mOptions;
  return IPC_OK();
}

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvAdoptChild(const LayersId& child)
{
  RefPtr<APZUpdater> oldApzUpdater;
  APZCTreeManagerParent* parent;
  bool scheduleComposition = false;
  RefPtr<CrossProcessCompositorBridgeParent> cpcp;
  RefPtr<WebRenderBridgeParent> childWrBridge;

  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    if (sIndirectLayerTrees[child].mParent) {
      // We currently don't support adopting children from one compositor to
      // another if the two compositors don't have the same options.
      MOZ_ASSERT(sIndirectLayerTrees[child].mParent->mOptions == mOptions);
      oldApzUpdater = sIndirectLayerTrees[child].mParent->mApzUpdater;
    }
    NotifyChildCreated(child);
    if (sIndirectLayerTrees[child].mLayerTree) {
      sIndirectLayerTrees[child].mLayerTree->SetLayerManager(mLayerManager, GetAnimationStorage());
      // Trigger composition to handle a case that mLayerTree was not composited yet
      // by previous CompositorBridgeParent, since nsRefreshDriver might wait composition complete.
      scheduleComposition = true;
    }
    if (mWrBridge) {
      childWrBridge = sIndirectLayerTrees[child].mWrBridge;
      cpcp = sIndirectLayerTrees[child].mCrossProcessParent;
    }
    parent = sIndirectLayerTrees[child].mApzcTreeManagerParent;
  }

  if (scheduleComposition) {
    ScheduleComposition();
  }

  if (childWrBridge) {
    MOZ_ASSERT(mWrBridge);
    RefPtr<wr::WebRenderAPI> api = mWrBridge->GetWebRenderAPI();
    api = api->Clone();
    childWrBridge->UpdateWebRender(mWrBridge->CompositorScheduler(),
                                   api,
                                   mWrBridge->AsyncImageManager(),
                                   GetAnimationStorage());
    // Pretend we composited, since parent CompositorBridgeParent was replaced.
    if (cpcp) {
      TimeStamp now = TimeStamp::Now();
      cpcp->DidComposite(child, now, now);
    }
  }

  if (oldApzUpdater) {
    // We don't support moving a child from an APZ-enabled compositor to a
    // APZ-disabled compositor. The mOptions assertion above should already
    // ensure this, since APZ-ness is one of the things in mOptions. Note
    // however it is possible for mApzUpdater to be non-null here with
    // oldApzUpdater null, because the child may not have been previously
    // composited.
    MOZ_ASSERT(mApzUpdater);
  }
  if (mApzUpdater) {
    if (parent) {
      MOZ_ASSERT(mApzcTreeManager);
      parent->ChildAdopted(mApzcTreeManager, mApzUpdater);
    }
    mApzUpdater->NotifyLayerTreeAdopted(child, oldApzUpdater);
  }
  return IPC_OK();
}

PWebRenderBridgeParent*
CompositorBridgeParent::AllocPWebRenderBridgeParent(const wr::PipelineId& aPipelineId,
                                                    const LayoutDeviceIntSize& aSize,
                                                    TextureFactoryIdentifier* aTextureFactoryIdentifier,
                                                    wr::IdNamespace* aIdNamespace)
{
#ifndef MOZ_BUILD_WEBRENDER
  // Extra guard since this in the parent process and we don't want a malicious
  // child process invoking this codepath before it's ready
  MOZ_RELEASE_ASSERT(false);
#endif
  MOZ_ASSERT(wr::AsLayersId(aPipelineId) == mRootLayerTreeID);
  MOZ_ASSERT(!mWrBridge);
  MOZ_ASSERT(!mCompositor);
  MOZ_ASSERT(!mCompositorScheduler);
  MOZ_ASSERT(mWidget);

  RefPtr<widget::CompositorWidget> widget = mWidget;
  wr::WrWindowId windowId = wr::NewWindowId();
  if (mApzUpdater) {
    // If APZ is enabled, we need to register the APZ updater with the window id
    // before the updater thread is created in WebRenderAPI::Create, so
    // that the callback from the updater thread can find the right APZUpdater.
    mApzUpdater->SetWebRenderWindowId(windowId);
  }
  if (mApzSampler) {
    // Same as for mApzUpdater, but for the sampler thread.
    mApzSampler->SetWebRenderWindowId(windowId);
  }
  RefPtr<wr::WebRenderAPI> api = wr::WebRenderAPI::Create(this, Move(widget), windowId, aSize);
  if (!api) {
    mWrBridge = WebRenderBridgeParent::CreateDestroyed(aPipelineId);
    mWrBridge.get()->AddRef(); // IPDL reference
    *aIdNamespace = mWrBridge->GetIdNamespace();
    *aTextureFactoryIdentifier = TextureFactoryIdentifier(LayersBackend::LAYERS_NONE);
    return mWrBridge;
  }
  mAsyncImageManager = new AsyncImagePipelineManager(api->Clone());
  RefPtr<AsyncImagePipelineManager> asyncMgr = mAsyncImageManager;
  wr::TransactionBuilder txn;
  txn.SetRootPipeline(aPipelineId);
  api->SendTransaction(txn);
  RefPtr<CompositorAnimationStorage> animStorage = GetAnimationStorage();
  mWrBridge = new WebRenderBridgeParent(this, aPipelineId, mWidget, nullptr, Move(api), Move(asyncMgr), Move(animStorage));
  mWrBridge.get()->AddRef(); // IPDL reference

  *aIdNamespace = mWrBridge->GetIdNamespace();
  mCompositorScheduler = mWrBridge->CompositorScheduler();
  MOZ_ASSERT(mCompositorScheduler);
  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    MOZ_ASSERT(sIndirectLayerTrees[mRootLayerTreeID].mWrBridge == nullptr);
    sIndirectLayerTrees[mRootLayerTreeID].mWrBridge = mWrBridge;
  }
  *aTextureFactoryIdentifier = mWrBridge->GetTextureFactoryIdentifier();
  return mWrBridge;
}

bool
CompositorBridgeParent::DeallocPWebRenderBridgeParent(PWebRenderBridgeParent* aActor)
{
#ifndef MOZ_BUILD_WEBRENDER
  // Extra guard since this in the parent process and we don't want a malicious
  // child process invoking this codepath before it's ready
  MOZ_RELEASE_ASSERT(false);
#endif
  WebRenderBridgeParent* parent = static_cast<WebRenderBridgeParent*>(aActor);
  {
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    auto it = sIndirectLayerTrees.find(wr::AsLayersId(parent->PipelineId()));
    if (it != sIndirectLayerTrees.end()) {
      it->second.mWrBridge = nullptr;
    }
  }
  parent->Release(); // IPDL reference
  return true;
}

RefPtr<WebRenderBridgeParent>
CompositorBridgeParent::GetWebRenderBridgeParent() const
{
  return mWrBridge;
}

Maybe<TimeStamp>
CompositorBridgeParent::GetTestingTimeStamp() const
{
  return mTestTime;
}

void
EraseLayerState(LayersId aId)
{
  RefPtr<APZUpdater> apz;

  { // scope lock
    MonitorAutoLock lock(*sIndirectLayerTreesLock);
    auto iter = sIndirectLayerTrees.find(aId);
    if (iter != sIndirectLayerTrees.end()) {
      CompositorBridgeParent* parent = iter->second.mParent;
      if (parent) {
        apz = parent->GetAPZUpdater();
      }
      sIndirectLayerTrees.erase(iter);
    }
  }

  if (apz) {
    apz->NotifyLayerTreeRemoved(aId);
  }
}

/*static*/ void
CompositorBridgeParent::DeallocateLayerTreeId(LayersId aId)
{
  MOZ_ASSERT(NS_IsMainThread());
  // Here main thread notifies compositor to remove an element from
  // sIndirectLayerTrees. This removed element might be queried soon.
  // Checking the elements of sIndirectLayerTrees exist or not before using.
  if (!CompositorLoop()) {
    gfxCriticalError() << "Attempting to post to a invalid Compositor Loop";
    return;
  }
  CompositorLoop()->PostTask(NewRunnableFunction("EraseLayerStateRunnable",
                                                 &EraseLayerState, aId));
}

static void
UpdateControllerForLayersId(LayersId aLayersId,
                            GeckoContentController* aController)
{
  // Adopt ref given to us by SetControllerForLayerTree()
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mController =
    already_AddRefed<GeckoContentController>(aController);
}

ScopedLayerTreeRegistration::ScopedLayerTreeRegistration(APZCTreeManager* aApzctm,
                                                         LayersId aLayersId,
                                                         Layer* aRoot,
                                                         GeckoContentController* aController)
    : mLayersId(aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aLayersId].mRoot = aRoot;
  sIndirectLayerTrees[aLayersId].mController = aController;
}

ScopedLayerTreeRegistration::~ScopedLayerTreeRegistration()
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees.erase(mLayersId);
}

/*static*/ void
CompositorBridgeParent::SetControllerForLayerTree(LayersId aLayersId,
                                                  GeckoContentController* aController)
{
  // This ref is adopted by UpdateControllerForLayersId().
  aController->AddRef();
  CompositorLoop()->PostTask(NewRunnableFunction("UpdateControllerForLayersIdRunnable",
                                                 &UpdateControllerForLayersId,
                                                 aLayersId,
                                                 aController));
}

/*static*/ already_AddRefed<IAPZCTreeManager>
CompositorBridgeParent::GetAPZCTreeManager(LayersId aLayersId)
{
  EnsureLayerTreeMapReady();
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aLayersId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  LayerTreeState* lts = &cit->second;

  RefPtr<IAPZCTreeManager> apzctm = lts->mParent
                                   ? lts->mParent->mApzcTreeManager.get()
                                   : nullptr;
  return apzctm.forget();
}

#if defined(MOZ_GECKO_PROFILER)
static void
InsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  profiler_add_marker(
    "VsyncTimestamp",
    MakeUnique<VsyncMarkerPayload>(aVsyncTimestamp));
}
#endif

/*static */ void
CompositorBridgeParent::PostInsertVsyncProfilerMarker(TimeStamp aVsyncTimestamp)
{
#if defined(MOZ_GECKO_PROFILER)
  // Called in the vsync thread
  if (profiler_is_active() && CompositorThreadHolder::IsActive()) {
    CompositorLoop()->PostTask(
      NewRunnableFunction("InsertVsyncProfilerMarkerRunnable", InsertVsyncProfilerMarker,
                          aVsyncTimestamp));
  }
#endif
}

widget::PCompositorWidgetParent*
CompositorBridgeParent::AllocPCompositorWidgetParent(const CompositorWidgetInitData& aInitData)
{
#if defined(MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING)
  if (mWidget) {
    // Should not create two widgets on the same compositor.
    return nullptr;
  }

  widget::CompositorWidgetParent* widget =
    new widget::CompositorWidgetParent(aInitData, mOptions);
  widget->AddRef();

#ifdef XP_WIN
  if (DeviceManagerDx::Get()->CanUseDComp()) {
    widget->AsWindows()->EnsureCompositorWindow();
  }
#endif

  // Sending the constructor acts as initialization as well.
  mWidget = widget;
  return widget;
#else
  return nullptr;
#endif
}

bool
CompositorBridgeParent::DeallocPCompositorWidgetParent(PCompositorWidgetParent* aActor)
{
#if defined(MOZ_WIDGET_SUPPORTS_OOP_COMPOSITING)
  static_cast<widget::CompositorWidgetParent*>(aActor)->Release();
  return true;
#else
  return false;
#endif
}

bool
CompositorBridgeParent::IsPendingComposite()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCompositor) {
    return false;
  }
  return mCompositor->IsPendingComposite();
}

void
CompositorBridgeParent::FinishPendingComposite()
{
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());
  if (!mCompositor) {
    return;
  }
  return mCompositor->FinishPendingComposite();
}

CompositorController*
CompositorBridgeParent::LayerTreeState::GetCompositorController() const
{
  return mParent;
}

MetricsSharingController*
CompositorBridgeParent::LayerTreeState::CrossProcessSharingController() const
{
  return mCrossProcessParent;
}

MetricsSharingController*
CompositorBridgeParent::LayerTreeState::InProcessSharingController() const
{
  return mParent;
}

void
CompositorBridgeParent::DidComposite(TimeStamp& aCompositeStart,
                                     TimeStamp& aCompositeEnd)
{
  if (mWrBridge) {
    NotifyDidComposite(mWrBridge->FlushPendingTransactionIds(), aCompositeStart, aCompositeEnd);
  } else {
    NotifyDidComposite(mPendingTransaction, aCompositeStart, aCompositeEnd);
#if defined(ENABLE_FRAME_LATENCY_LOG)
    if (mPendingTransaction.IsValid()) {
      if (mTxnStartTime) {
        uint32_t latencyMs = round((aCompositeEnd - mTxnStartTime).ToMilliseconds());
        printf_stderr("From transaction start to end of generate frame latencyMs %d this %p\n", latencyMs, this);
      }
      if (mFwdTime) {
        uint32_t latencyMs = round((aCompositeEnd - mFwdTime).ToMilliseconds());
        printf_stderr("From forwarding transaction to end of generate frame latencyMs %d this %p\n", latencyMs, this);
      }
    }
    mTxnStartTime = TimeStamp();
    mFwdTime = TimeStamp();
#endif
    mPendingTransaction = TransactionId{0};
  }
}

void
CompositorBridgeParent::NotifyPipelineRemoved(const wr::PipelineId& aPipelineId)
{
  if (mAsyncImageManager) {
    mAsyncImageManager->PipelineRemoved(aPipelineId);
  }
}

void
CompositorBridgeParent::NotifyPipelineRendered(const wr::PipelineId& aPipelineId,
                                               const wr::Epoch& aEpoch,
                                               TimeStamp& aCompositeStart,
                                               TimeStamp& aCompositeEnd)
{
  if (mAsyncImageManager) {
    mAsyncImageManager->PipelineRendered(aPipelineId, aEpoch);
  }

  if (!mWrBridge) {
    return;
  }

  if (mWrBridge->PipelineId() == aPipelineId) {
    mWrBridge->RemoveEpochDataPriorTo(aEpoch);

    if (!mPaused) {
      TransactionId transactionId = mWrBridge->FlushTransactionIdsForEpoch(aEpoch, aCompositeEnd);
      Unused << SendDidComposite(LayersId{0}, transactionId, aCompositeStart, aCompositeEnd);

      nsTArray<ImageCompositeNotificationInfo> notifications;
      mWrBridge->ExtractImageCompositeNotifications(&notifications);
      if (!notifications.IsEmpty()) {
        Unused << ImageBridgeParent::NotifyImageComposites(notifications);
      }
    }
    return;
  }

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([&] (LayerTreeState* lts, const LayersId& aLayersId) -> void {
    if (lts->mCrossProcessParent &&
        lts->mWrBridge &&
        lts->mWrBridge->PipelineId() == aPipelineId) {

      lts->mWrBridge->RemoveEpochDataPriorTo(aEpoch);

      if (!mPaused) {
        CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent;
        TransactionId transactionId = lts->mWrBridge->FlushTransactionIdsForEpoch(aEpoch, aCompositeEnd);
        Unused << cpcp->SendDidComposite(aLayersId, transactionId, aCompositeStart, aCompositeEnd);
      }
    }
  });
}

void
CompositorBridgeParent::NotifyDidComposite(TransactionId aTransactionId, TimeStamp& aCompositeStart, TimeStamp& aCompositeEnd)
{
  Unused << SendDidComposite(LayersId{0}, aTransactionId, aCompositeStart, aCompositeEnd);

  if (mLayerManager) {
    nsTArray<ImageCompositeNotificationInfo> notifications;
    mLayerManager->ExtractImageCompositeNotifications(&notifications);
    if (!notifications.IsEmpty()) {
      Unused << ImageBridgeParent::NotifyImageComposites(notifications);
    }
  }

  if (mWrBridge) {
    nsTArray<ImageCompositeNotificationInfo> notifications;
    mWrBridge->ExtractImageCompositeNotifications(&notifications);
    if (!notifications.IsEmpty()) {
      Unused << ImageBridgeParent::NotifyImageComposites(notifications);
    }
  }

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([&] (LayerTreeState* lts, const LayersId& aLayersId) -> void {
    if (lts->mCrossProcessParent && lts->mParent == this) {
      CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent;
      cpcp->DidCompositeLocked(aLayersId, aCompositeStart, aCompositeEnd);
    }
  });
}

void
CompositorBridgeParent::InvalidateRemoteLayers()
{
  MOZ_ASSERT(CompositorLoop() == MessageLoop::current());

  Unused << PCompositorBridgeParent::SendInvalidateLayers(LayersId{0});

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  ForEachIndirectLayerTree([] (LayerTreeState* lts, const LayersId& aLayersId) -> void {
    if (lts->mCrossProcessParent) {
      CrossProcessCompositorBridgeParent* cpcp = lts->mCrossProcessParent;
      Unused << cpcp->SendInvalidateLayers(aLayersId);
    }
  });
}

void
UpdateIndirectTree(LayersId aId, Layer* aRoot, const TargetConfig& aTargetConfig)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  sIndirectLayerTrees[aId].mRoot = aRoot;
  sIndirectLayerTrees[aId].mTargetConfig = aTargetConfig;
}

/* static */ CompositorBridgeParent::LayerTreeState*
CompositorBridgeParent::GetIndirectShadowTree(LayersId aId)
{
  // Only the compositor thread should use this method variant
  MOZ_ASSERT(CompositorThreadHolder::IsInCompositorThread());

  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aId);
  if (sIndirectLayerTrees.end() == cit) {
    return nullptr;
  }
  return &cit->second;
}

/* static */ bool
CompositorBridgeParent::CallWithIndirectShadowTree(LayersId aId,
                                                   const std::function<void(CompositorBridgeParent::LayerTreeState&)>& aFunc)
{
  // Note that this does not make things universally threadsafe just because the
  // sIndirectLayerTreesLock mutex is held. This is because the compositor
  // thread can mutate the LayerTreeState outside the lock. It does however
  // ensure that the *storage* for the LayerTreeState remains stable, since we
  // should always hold the lock when adding/removing entries to the map.
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  LayerTreeMap::iterator cit = sIndirectLayerTrees.find(aId);
  if (sIndirectLayerTrees.end() == cit) {
    return false;
  }
  aFunc(cit->second);
  return true;
}

static CompositorBridgeParent::LayerTreeState*
GetStateForRoot(LayersId aContentLayersId, const MonitorAutoLock& aProofOfLock)
{
  CompositorBridgeParent::LayerTreeState* state = nullptr;
  LayerTreeMap::iterator itr = sIndirectLayerTrees.find(aContentLayersId);
  if (sIndirectLayerTrees.end() != itr) {
    state = &itr->second;
  }

  // |state| is the state for the content process, but we want the APZCTMParent
  // for the parent process owning that content process. So we have to jump to
  // the LayerTreeState for the root layer tree id for that layer tree, and use
  // the mApzcTreeManagerParent from that. This should also work with nested
  // content processes, because RootLayerTreeId() will bypass any intermediate
  // processes' ids and go straight to the root.
  if (state) {
    LayersId rootLayersId = state->mParent->RootLayerTreeId();
    itr = sIndirectLayerTrees.find(rootLayersId);
    state = (sIndirectLayerTrees.end() != itr) ? &itr->second : nullptr;
  }

  return state;
}

/* static */ APZCTreeManagerParent*
CompositorBridgeParent::GetApzcTreeManagerParentForRoot(LayersId aContentLayersId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState* state =
      GetStateForRoot(aContentLayersId, lock);
  return state ? state->mApzcTreeManagerParent : nullptr;
}

/* static */ GeckoContentController*
CompositorBridgeParent::GetGeckoContentControllerForRoot(LayersId aContentLayersId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState* state =
      GetStateForRoot(aContentLayersId, lock);
  return state ? state->mController.get() : nullptr;
}

PTextureParent*
CompositorBridgeParent::AllocPTextureParent(const SurfaceDescriptor& aSharedData,
                                            const ReadLockDescriptor& aReadLock,
                                            const LayersBackend& aLayersBackend,
                                            const TextureFlags& aFlags,
                                            const LayersId& aId,
                                            const uint64_t& aSerial,
                                            const wr::MaybeExternalImageId& aExternalImageId)
{
  return TextureHost::CreateIPDLActor(this, aSharedData, aReadLock, aLayersBackend, aFlags, aSerial, aExternalImageId);
}

bool
CompositorBridgeParent::DeallocPTextureParent(PTextureParent* actor)
{
  return TextureHost::DestroyIPDLActor(actor);
}

bool
CompositorBridgeParent::IsSameProcess() const
{
  return OtherPid() == base::GetCurrentProcId();
}

void
CompositorBridgeParent::NotifyWebRenderError(wr::WebRenderError aError)
{
  MOZ_ASSERT(CompositorLoop() == MessageLoop::current());
  Unused << SendNotifyWebRenderError(aError);
}

#if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)
//#define PLUGINS_LOG(...) printf_stderr("CP [%s]: ", __FUNCTION__);
//                         printf_stderr(__VA_ARGS__);
//                         printf_stderr("\n");
#define PLUGINS_LOG(...)

bool
CompositorBridgeParent::UpdatePluginWindowState(LayersId aId)
{
  MonitorAutoLock lock(*sIndirectLayerTreesLock);
  CompositorBridgeParent::LayerTreeState& lts = sIndirectLayerTrees[aId];
  if (!lts.mParent) {
    PLUGINS_LOG("[%" PRIu64 "] layer tree compositor parent pointer is null", aId);
    return false;
  }

  // Check if this layer tree has received any shadow layer updates
  if (!lts.mUpdatedPluginDataAvailable) {
    PLUGINS_LOG("[%" PRIu64 "] no plugin data", aId);
    return false;
  }

  // pluginMetricsChanged tracks whether we need to send plugin update
  // data to the main thread. If we do we'll have to block composition,
  // which we want to avoid if at all possible.
  bool pluginMetricsChanged = false;

  // Same layer tree checks
  if (mLastPluginUpdateLayerTreeId == aId) {
    // no plugin data and nothing has changed, bail.
    if (!mCachedPluginData.Length() && !lts.mPluginData.Length()) {
      PLUGINS_LOG("[%" PRIu64 "] no data, no changes", aId);
      return false;
    }

    if (mCachedPluginData.Length() == lts.mPluginData.Length()) {
      // check for plugin data changes
      for (uint32_t idx = 0; idx < lts.mPluginData.Length(); idx++) {
        if (!(mCachedPluginData[idx] == lts.mPluginData[idx])) {
          pluginMetricsChanged = true;
          break;
        }
      }
    } else {
      // array lengths don't match, need to update
      pluginMetricsChanged = true;
    }
  } else {
    // exchanging layer trees, we need to update
    pluginMetricsChanged = true;
  }

  // Check if plugin windows are currently hidden due to scrolling
  if (mDeferPluginWindows) {
    PLUGINS_LOG("[%" PRIu64 "] suppressing", aId);
    return false;
  }

  // If the plugin windows were hidden but now are not, we need to force
  // update the metrics to make sure they are visible again.
  if (mPluginWindowsHidden) {
    PLUGINS_LOG("[%" PRIu64 "] re-showing", aId);
    mPluginWindowsHidden = false;
    pluginMetricsChanged = true;
  }

  if (!lts.mPluginData.Length()) {
    // Don't hide plugins if the previous remote layer tree didn't contain any.
    if (!mCachedPluginData.Length()) {
      PLUGINS_LOG("[%" PRIu64 "] nothing to hide", aId);
      return false;
    }

    uintptr_t parentWidget = GetWidget()->GetWidgetKey();

    // We will pass through here in cases where the previous shadow layer
    // tree contained visible plugins and the new tree does not. All we need
    // to do here is hide the plugins for the old tree, so don't waste time
    // calculating clipping.
    mPluginsLayerOffset = nsIntPoint(0,0);
    mPluginsLayerVisibleRegion.SetEmpty();
    Unused << lts.mParent->SendHideAllPlugins(parentWidget);
    lts.mUpdatedPluginDataAvailable = false;
    PLUGINS_LOG("[%" PRIu64 "] hide all", aId);
  } else {
    // Retrieve the offset and visible region of the layer that hosts
    // the plugins, CompositorBridgeChild needs these in calculating proper
    // plugin clipping.
    LayerTransactionParent* layerTree = lts.mLayerTree;
    Layer* contentRoot = layerTree->GetRoot();
    if (contentRoot) {
      nsIntPoint offset;
      nsIntRegion visibleRegion;
      if (contentRoot->GetVisibleRegionRelativeToRootLayer(visibleRegion,
                                                            &offset)) {
        // Check to see if these values have changed, if so we need to
        // update plugin window position within the window.
        if (!pluginMetricsChanged &&
            mPluginsLayerVisibleRegion == visibleRegion &&
            mPluginsLayerOffset == offset) {
          PLUGINS_LOG("[%" PRIu64 "] no change", aId);
          return false;
        }
        mPluginsLayerOffset = offset;
        mPluginsLayerVisibleRegion = visibleRegion;
        Unused << lts.mParent->SendUpdatePluginConfigurations(
          LayoutDeviceIntPoint::FromUnknownPoint(offset),
          LayoutDeviceIntRegion::FromUnknownRegion(visibleRegion),
          lts.mPluginData);
        lts.mUpdatedPluginDataAvailable = false;
        PLUGINS_LOG("[%" PRIu64 "] updated", aId);
      } else {
        PLUGINS_LOG("[%" PRIu64 "] no visibility data", aId);
        return false;
      }
    } else {
      PLUGINS_LOG("[%" PRIu64 "] no content root", aId);
      return false;
    }
  }

  mLastPluginUpdateLayerTreeId = aId;
  mCachedPluginData = lts.mPluginData;
  return true;
}

void
CompositorBridgeParent::ScheduleShowAllPluginWindows()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::ShowAllPluginWindows",
                      this,
                      &CompositorBridgeParent::ShowAllPluginWindows));
}

void
CompositorBridgeParent::ShowAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  mDeferPluginWindows = false;
  ScheduleComposition();
}

void
CompositorBridgeParent::ScheduleHideAllPluginWindows()
{
  MOZ_ASSERT(CompositorLoop());
  CompositorLoop()->PostTask(
    NewRunnableMethod("layers::CompositorBridgeParent::HideAllPluginWindows",
                      this,
                      &CompositorBridgeParent::HideAllPluginWindows));
}

void
CompositorBridgeParent::HideAllPluginWindows()
{
  MOZ_ASSERT(!NS_IsMainThread());
  // No plugins in the cache implies no plugins to manage
  // in this content.
  if (!mCachedPluginData.Length() || mDeferPluginWindows) {
    return;
  }

  uintptr_t parentWidget = GetWidget()->GetWidgetKey();

  mDeferPluginWindows = true;
  mPluginWindowsHidden = true;

#if defined(XP_WIN)
  // We will get an async reply that this has happened and then send hide.
  mWaitForPluginsUntil = TimeStamp::Now() + mVsyncRate;
  Unused << SendCaptureAllPlugins(parentWidget);
#else
  Unused << SendHideAllPlugins(parentWidget);
  ScheduleComposition();
#endif
}
#endif // #if defined(XP_WIN) || defined(MOZ_WIDGET_GTK)

mozilla::ipc::IPCResult
CompositorBridgeParent::RecvAllPluginsCaptured()
{
#if defined(XP_WIN)
  mWaitForPluginsUntil = TimeStamp();
  mHaveBlockedForPlugins = false;
  ForceComposeToTarget(nullptr);
  Unused << SendHideAllPlugins(GetWidget()->GetWidgetKey());
  return IPC_OK();
#else
  MOZ_ASSERT_UNREACHABLE(
    "CompositorBridgeParent::RecvAllPluginsCaptured calls unexpected.");
  return IPC_FAIL_NO_REASON(this);
#endif
}

} // namespace layers
} // namespace mozilla
