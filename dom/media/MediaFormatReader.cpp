/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaFormatReader.h"

#include "AutoTaskQueue.h"
#include "MediaData.h"
#include "MediaInfo.h"
#include "VideoFrameContainer.h"
#include "VideoUtils.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/CDMProxy.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/NotNull.h"
#include "mozilla/Preferences.h"
#include "mozilla/SharedThreadPool.h"
#include "mozilla/StaticPrefs.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Unused.h"
#include "nsContentUtils.h"
#include "nsPrintfCString.h"
#ifdef MOZ_WIDGET_ANDROID
#include "mozilla/jni/Utils.h"
#endif

#include <algorithm>
#include <map>
#include <queue>

using namespace mozilla::media;

static mozilla::LazyLogModule sFormatDecoderLog("MediaFormatReader");
mozilla::LazyLogModule gMediaDemuxerLog("MediaDemuxer");

#define LOG(arg, ...)                                                          \
  DDMOZ_LOG(sFormatDecoderLog,                                                 \
            mozilla::LogLevel::Debug,                                          \
            "::%s: " arg,                                                      \
            __func__,                                                          \
            ##__VA_ARGS__)
#define LOGV(arg, ...)                                                         \
  DDMOZ_LOG(sFormatDecoderLog,                                                 \
            mozilla::LogLevel::Verbose,                                        \
            "::%s: " arg,                                                      \
            __func__,                                                          \
            ##__VA_ARGS__)

#define NS_DispatchToMainThread(...) CompileError_UseAbstractMainThreadInstead

namespace mozilla {


typedef void* MediaDataDecoderID;

/**
 * This helper class is used to report telemetry of the time used to recover a
 * decoder from GPU crash.
 * It uses MediaDecoderOwnerID to identify which video we're dealing with.
 * It uses MediaDataDecoderID to make sure that the old MediaDataDecoder has
 * been deleted and we're already recovered.
 * It reports two recovery times, one is calculated from GPU crashed (that is,
 * the time when VideoDecoderChild::ActorDestory() is called) and the other is
 * calculated from the MFR is notified with NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER
 * error.
 */
class GPUProcessCrashTelemetryLogger
{
  struct GPUCrashData
  {
    GPUCrashData(MediaDataDecoderID aMediaDataDecoderID,
                 mozilla::TimeStamp aGPUCrashTime,
                 mozilla::TimeStamp aErrorNotifiedTime)
      : mMediaDataDecoderID(aMediaDataDecoderID)
      , mGPUCrashTime(aGPUCrashTime)
      , mErrorNotifiedTime(aErrorNotifiedTime)
    {
      MOZ_ASSERT(mMediaDataDecoderID);
      MOZ_ASSERT(!mGPUCrashTime.IsNull());
      MOZ_ASSERT(!mErrorNotifiedTime.IsNull());
    }

    MediaDataDecoderID mMediaDataDecoderID;
    mozilla::TimeStamp mGPUCrashTime;
    mozilla::TimeStamp mErrorNotifiedTime;
  };

public:
  static void
  RecordGPUCrashData(MediaDecoderOwnerID aMediaDecoderOwnerID,
                     MediaDataDecoderID aMediaDataDecoderID,
                     const TimeStamp& aGPUCrashTime,
                     const TimeStamp& aErrorNotifiedTime)
  {
    MOZ_ASSERT(aMediaDecoderOwnerID);
    MOZ_ASSERT(aMediaDataDecoderID);
    MOZ_ASSERT(!aGPUCrashTime.IsNull());
    MOZ_ASSERT(!aErrorNotifiedTime.IsNull());
    StaticMutexAutoLock lock(sGPUCrashMapMutex);
    auto it = sGPUCrashDataMap.find(aMediaDecoderOwnerID);
    if (it == sGPUCrashDataMap.end()) {
      sGPUCrashDataMap.insert(std::make_pair(aMediaDecoderOwnerID,
                                             GPUCrashData(aMediaDataDecoderID,
                                                          aGPUCrashTime,
                                                          aErrorNotifiedTime)));
    }
  }

  static void
  ReportTelemetry(MediaDecoderOwnerID aMediaDecoderOwnerID,
                  MediaDataDecoderID aMediaDataDecoderID)
  {
    MOZ_ASSERT(aMediaDecoderOwnerID);
    MOZ_ASSERT(aMediaDataDecoderID);
    StaticMutexAutoLock lock(sGPUCrashMapMutex);
    auto it = sGPUCrashDataMap.find(aMediaDecoderOwnerID);
    if (it != sGPUCrashDataMap.end() &&
        it->second.mMediaDataDecoderID != aMediaDataDecoderID) {
      Telemetry::AccumulateTimeDelta(
        Telemetry::VIDEO_HW_DECODER_CRASH_RECOVERY_TIME_SINCE_GPU_CRASHED_MS,
        it->second.mGPUCrashTime);
      Telemetry::AccumulateTimeDelta(
        Telemetry::VIDEO_HW_DECODER_CRASH_RECOVERY_TIME_SINCE_MFR_NOTIFIED_MS,
        it->second.mErrorNotifiedTime);
      sGPUCrashDataMap.erase(aMediaDecoderOwnerID);
    }
  }

private:
  static std::map<MediaDecoderOwnerID, GPUCrashData> sGPUCrashDataMap;
  static StaticMutex sGPUCrashMapMutex;
};

std::map<MediaDecoderOwnerID, GPUProcessCrashTelemetryLogger::GPUCrashData>
GPUProcessCrashTelemetryLogger::sGPUCrashDataMap;
StaticMutex GPUProcessCrashTelemetryLogger::sGPUCrashMapMutex;

/**
 * This is a singleton which controls the number of decoders that can be
 * created concurrently. Before calling PDMFactory::CreateDecoder(), Alloc()
 * must be called to get a token object as a permission to create a decoder.
 * The token should stay alive until Shutdown() is called on the decoder.
 * The destructor of the token will restore the decoder count so it is available
 * for next calls of Alloc().
 */
class GlobalAllocPolicy
{
  using TrackType = TrackInfo::TrackType;

public:
  class Token
  {
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(Token)
  protected:
    virtual ~Token() {}
  };

  using Promise = MozPromise<RefPtr<Token>, bool, true>;

  // Acquire a token for decoder creation. Thread-safe.
  auto Alloc() -> RefPtr<Promise>;

  // Called by ClearOnShutdown() to delete the singleton.
  void operator=(decltype(nullptr));

  // Get the singleton for the given track type. Thread-safe.
  static GlobalAllocPolicy& Instance(TrackType aTrack);

private:
  class AutoDeallocToken;
  using PromisePrivate = Promise::Private;
  GlobalAllocPolicy();
  ~GlobalAllocPolicy();
  // Called by the destructor of TokenImpl to restore the decoder limit.
  void Dealloc();
  // Decrement the decoder limit and resolve a promise if available.
  void ResolvePromise(ReentrantMonitorAutoEnter& aProofOfLock);

  // Protect access to Instance().
  static StaticMutex sMutex;

  ReentrantMonitor mMonitor;
  // The number of decoders available for creation.
  int mDecoderLimit;
  // Requests to acquire tokens.
  std::queue<RefPtr<PromisePrivate>> mPromises;
};

StaticMutex GlobalAllocPolicy::sMutex;

class GlobalAllocPolicy::AutoDeallocToken : public Token
{
public:
  explicit AutoDeallocToken(GlobalAllocPolicy& aPolicy) : mPolicy(aPolicy) { }

private:
  ~AutoDeallocToken()
  {
    mPolicy.Dealloc();
  }

  GlobalAllocPolicy& mPolicy; // reference to a singleton object.
};

static int32_t
MediaDecoderLimitDefault()
{
#ifdef MOZ_WIDGET_ANDROID
  if (jni::GetAPIVersion() < 18) {
    // Older Android versions have broken support for multiple simultaneous
    // decoders, see bug 1278574.
    return 1;
  }
#endif
  // Otherwise, set no decoder limit.
  return -1;
}

GlobalAllocPolicy::GlobalAllocPolicy()
  : mMonitor("DecoderAllocPolicy::mMonitor")
  , mDecoderLimit(MediaDecoderLimitDefault())
{
  SystemGroup::Dispatch(
    TaskCategory::Other,
    NS_NewRunnableFunction("GlobalAllocPolicy::GlobalAllocPolicy", [this]() {
      ClearOnShutdown(this, ShutdownPhase::ShutdownThreads);
    }));
}

GlobalAllocPolicy::~GlobalAllocPolicy()
{
  while (!mPromises.empty()) {
    RefPtr<PromisePrivate> p = mPromises.front().forget();
    mPromises.pop();
    p->Reject(true, __func__);
  }
}

GlobalAllocPolicy&
GlobalAllocPolicy::Instance(TrackType aTrack)
{
  StaticMutexAutoLock lock(sMutex);
  if (aTrack == TrackType::kAudioTrack) {
    static auto sAudioPolicy = new GlobalAllocPolicy();
    return *sAudioPolicy;
  } else {
    static auto sVideoPolicy = new GlobalAllocPolicy();
    return *sVideoPolicy;
  }
}

auto
GlobalAllocPolicy::Alloc() -> RefPtr<Promise>
{
  // No decoder limit set.
  if (mDecoderLimit < 0) {
    return Promise::CreateAndResolve(new Token(), __func__);
  }

  ReentrantMonitorAutoEnter mon(mMonitor);
  RefPtr<PromisePrivate> p = new PromisePrivate(__func__);
  mPromises.push(p);
  ResolvePromise(mon);
  return p.forget();
}

void
GlobalAllocPolicy::Dealloc()
{
  ReentrantMonitorAutoEnter mon(mMonitor);
  ++mDecoderLimit;
  ResolvePromise(mon);
}

void
GlobalAllocPolicy::ResolvePromise(ReentrantMonitorAutoEnter& aProofOfLock)
{
  MOZ_ASSERT(mDecoderLimit >= 0);

  if (mDecoderLimit > 0 && !mPromises.empty()) {
    --mDecoderLimit;
    RefPtr<PromisePrivate> p = mPromises.front().forget();
    mPromises.pop();
    p->Resolve(new AutoDeallocToken(*this), __func__);
  }
}

void
GlobalAllocPolicy::operator=(std::nullptr_t)
{
  delete this;
}

/**
 * This class addresses the concern of bug 1339310 comment 4 where the Widevine
 * CDM doesn't support running multiple instances of a video decoder at once per
 * CDM instance by sequencing the order of decoder creation and shutdown. Note
 * this class addresses a different concern from that of GlobalAllocPolicy which
 * controls a system-wide number of decoders while this class control a per-MFR
 * number (which is one per CDM requirement).
 */
class LocalAllocPolicy
{
  using TrackType = TrackInfo::TrackType;
  using Promise = GlobalAllocPolicy::Promise;
  using Token = GlobalAllocPolicy::Token;

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(LocalAllocPolicy)

public:
  LocalAllocPolicy(TrackType aTrack, TaskQueue* aOwnerThread)
    : mTrack(aTrack)
    , mOwnerThread(aOwnerThread)
  {
  }

  // Acquire a token for decoder creation. Note the resolved token will
  // aggregate a GlobalAllocPolicy token to comply to its policy. Note
  // this function shouldn't be called again until the returned promise
  // is resolved or rejected.
  RefPtr<Promise> Alloc();

  // Cancel the request to GlobalAllocPolicy and reject the current token
  // request. Note this must happen before mOwnerThread->BeginShutdown().
  void Cancel();

private:
  /*
   * An RAII class to manage LocalAllocPolicy::mDecoderLimit.
   */
  class AutoDeallocToken : public Token
  {
  public:
    explicit AutoDeallocToken(LocalAllocPolicy* aOwner)
      : mOwner(aOwner)
    {
      MOZ_DIAGNOSTIC_ASSERT(mOwner->mDecoderLimit > 0);
      --mOwner->mDecoderLimit;
    }
    // Aggregate a GlobalAllocPolicy token to present a single instance of
    // Token to the client so the client doesn't have to deal with
    // GlobalAllocPolicy and LocalAllocPolicy separately.
    void Append(Token* aToken)
    {
      mToken = aToken;
    }
  private:
    // Release tokens allocated from GlobalAllocPolicy and LocalAllocPolicy
    // and process next token request if any.
    ~AutoDeallocToken()
    {
      mToken = nullptr; // Dealloc the global token.
      ++mOwner->mDecoderLimit; // Dealloc the local token.
      mOwner->ProcessRequest(); // Process next pending request.
    }
    RefPtr<LocalAllocPolicy> mOwner;
    RefPtr<Token> mToken;
  };

  ~LocalAllocPolicy() { }
  void ProcessRequest();

  int mDecoderLimit = 1;
  const TrackType mTrack;
  RefPtr<TaskQueue> mOwnerThread;
  MozPromiseHolder<Promise> mPendingPromise;
  MozPromiseRequestHolder<Promise> mTokenRequest;
};

RefPtr<LocalAllocPolicy::Promise>
LocalAllocPolicy::Alloc()
{
  MOZ_ASSERT(mOwnerThread->IsCurrentThreadIn());
  MOZ_DIAGNOSTIC_ASSERT(mPendingPromise.IsEmpty());
  RefPtr<Promise> p = mPendingPromise.Ensure(__func__);
  if (mDecoderLimit > 0) {
    ProcessRequest();
  }
  return p.forget();
}

void
LocalAllocPolicy::ProcessRequest()
{
  MOZ_ASSERT(mOwnerThread->IsCurrentThreadIn());
  MOZ_DIAGNOSTIC_ASSERT(mDecoderLimit > 0);

  // No pending request.
  if (mPendingPromise.IsEmpty()) {
    return;
  }

  RefPtr<AutoDeallocToken> token = new AutoDeallocToken(this);
  RefPtr<LocalAllocPolicy> self = this;

  GlobalAllocPolicy::Instance(mTrack).Alloc()->Then(
    mOwnerThread, __func__,
    [self, token](RefPtr<Token> aToken) {
      self->mTokenRequest.Complete();
      token->Append(aToken);
      self->mPendingPromise.Resolve(token, __func__);
    },
    [self, token]() {
      self->mTokenRequest.Complete();
      self->mPendingPromise.Reject(true, __func__);
    })->Track(mTokenRequest);
}

void
LocalAllocPolicy::Cancel()
{
  MOZ_ASSERT(mOwnerThread->IsCurrentThreadIn());
  mPendingPromise.RejectIfExists(true, __func__);
  mTokenRequest.DisconnectIfExists();
}

/**
 * This class tracks shutdown promises to ensure all decoders are shut down
 * completely before MFR continues the rest of the shutdown procedure.
 */
class MediaFormatReader::ShutdownPromisePool
{
public:
  ShutdownPromisePool()
    : mOnShutdownComplete(new ShutdownPromise::Private(__func__))
  {
  }

  // Return a promise which will be resolved when all the tracking promises
  // are resolved. Note no more promises should be added for tracking once
  // this function is called.
  RefPtr<ShutdownPromise> Shutdown();

   // Track a shutdown promise.
  void Track(RefPtr<ShutdownPromise> aPromise);

   // Shut down a decoder and track its shutdown promise.
  void ShutdownDecoder(already_AddRefed<MediaDataDecoder> aDecoder)
  {
    Track(RefPtr<MediaDataDecoder>(aDecoder)->Shutdown());
  }

private:
  bool mShutdown = false;
  const RefPtr<ShutdownPromise::Private> mOnShutdownComplete;
  nsTHashtable<nsRefPtrHashKey<ShutdownPromise>> mPromises;
};

RefPtr<ShutdownPromise>
MediaFormatReader::ShutdownPromisePool::Shutdown()
{
  MOZ_DIAGNOSTIC_ASSERT(!mShutdown);
  mShutdown = true;
  if (mPromises.Count() == 0) {
    mOnShutdownComplete->Resolve(true, __func__);
  }
  return mOnShutdownComplete;
}

void
MediaFormatReader::ShutdownPromisePool::Track(RefPtr<ShutdownPromise> aPromise)
{
  MOZ_DIAGNOSTIC_ASSERT(!mShutdown);
  MOZ_DIAGNOSTIC_ASSERT(!mPromises.Contains(aPromise));
  mPromises.PutEntry(aPromise);
  aPromise->Then(
    AbstractThread::GetCurrent(), __func__,
    [aPromise, this]() {
      MOZ_DIAGNOSTIC_ASSERT(mPromises.Contains(aPromise));
      mPromises.RemoveEntry(aPromise);
      if (mShutdown && mPromises.Count() == 0) {
        mOnShutdownComplete->Resolve(true, __func__);
      }
    });
}

void
MediaFormatReader::DecoderData::ShutdownDecoder()
{
  MutexAutoLock lock(mMutex);

  if (!mDecoder) {
    // No decoder to shut down.
    return;
  }

  if (mFlushing) {
    // Flush is is in action. Shutdown will be initiated after flush completes.
    MOZ_DIAGNOSTIC_ASSERT(mShutdownPromise);
    mOwner->mShutdownPromisePool->Track(mShutdownPromise->Ensure(__func__));
    // The order of decoder creation and shutdown is handled by LocalAllocPolicy
    // and ShutdownPromisePool. MFR can now reset these members to a fresh state
    // and be ready to create new decoders again without explicitly waiting for
    // flush/shutdown to complete.
    mShutdownPromise = nullptr;
    mFlushing = false;
  } else {
    // No flush is in action. We can shut down the decoder now.
    mOwner->mShutdownPromisePool->Track(mDecoder->Shutdown());
  }

  // mShutdownPromisePool will handle the order of decoder shutdown so
  // we can forget mDecoder and be ready to create a new one.
  mDecoder = nullptr;
  mDescription = NS_LITERAL_CSTRING("shutdown");
  mOwner->ScheduleUpdate(mType == MediaData::AUDIO_DATA
                         ? TrackType::kAudioTrack
                         : TrackType::kVideoTrack);
}

void
MediaFormatReader::DecoderData::Flush()
{
  if (mFlushing || mFlushed) {
    // Flush still pending or already flushed, nothing more to do.
    return;
  }
  mDecodeRequest.DisconnectIfExists();
  mDrainRequest.DisconnectIfExists();
  mDrainState = DrainState::None;
  CancelWaitingForKey();
  mOutput.Clear();
  mNumSamplesInput = 0;
  mNumSamplesOutput = 0;
  mSizeOfQueue = 0;
  if (mDecoder) {
    TrackType type = mType == MediaData::AUDIO_DATA
                     ? TrackType::kAudioTrack
                     : TrackType::kVideoTrack;
    mFlushing = true;
    MOZ_DIAGNOSTIC_ASSERT(!mShutdownPromise);
    mShutdownPromise = new SharedShutdownPromiseHolder();
    RefPtr<SharedShutdownPromiseHolder> p = mShutdownPromise;
    RefPtr<MediaDataDecoder> d = mDecoder;
    DDLOGEX2("MediaFormatReader::DecoderData",
             this,
             DDLogCategory::Log,
             "flushing",
             DDNoValue{});
    mDecoder->Flush()->Then(mOwner->OwnerThread(),
                            __func__,
                            [type, this, p, d]() {
                              DDLOGEX2("MediaFormatReader::DecoderData",
                                       this,
                                       DDLogCategory::Log,
                                       "flushed",
                                       DDNoValue{});
                              if (!p->IsEmpty()) {
                                // Shutdown happened before flush completes.
                                // Let's continue to shut down the decoder. Note
                                // we don't access |this| because this decoder
                                // is no longer managed by MFR::DecoderData.
                                d->Shutdown()->ChainTo(p->Steal(), __func__);
                                return;
                              }
                              mFlushing = false;
                              mShutdownPromise = nullptr;
                              mOwner->ScheduleUpdate(type);
                            },
                            [type, this, p, d](const MediaResult& aError) {
                              DDLOGEX2("MediaFormatReader::DecoderData",
                                       this,
                                       DDLogCategory::Log,
                                       "flush_error",
                                       aError);
                              if (!p->IsEmpty()) {
                                d->Shutdown()->ChainTo(p->Steal(), __func__);
                                return;
                              }
                              mFlushing = false;
                              mShutdownPromise = nullptr;
                              mOwner->NotifyError(type, aError);
                            });
  }
  mFlushed = true;
}

class MediaFormatReader::DecoderFactory
{
  using InitPromise = MediaDataDecoder::InitPromise;
  using TokenPromise = GlobalAllocPolicy::Promise;
  using Token = GlobalAllocPolicy::Token;

public:
  explicit DecoderFactory(MediaFormatReader* aOwner)
    : mAudio(aOwner->mAudio, TrackInfo::kAudioTrack, aOwner->OwnerThread())
    , mVideo(aOwner->mVideo, TrackInfo::kVideoTrack, aOwner->OwnerThread())
    , mOwner(WrapNotNull(aOwner))
  {
    DecoderDoctorLogger::LogConstruction("MediaFormatReader::DecoderFactory",
                                         this);
    DecoderDoctorLogger::LinkParentAndChild(
      aOwner, "decoder factory", "MediaFormatReader::DecoderFactory", this);
  }

  ~DecoderFactory()
  {
    DecoderDoctorLogger::LogDestruction("MediaFormatReader::DecoderFactory",
                                        this);
  }

  void CreateDecoder(TrackType aTrack);

  // Shutdown any decoder pending initialization and reset mAudio/mVideo to its
  // pristine state so CreateDecoder() is ready to be called again immediately.
  void ShutdownDecoder(TrackType aTrack)
  {
    MOZ_ASSERT(aTrack == TrackInfo::kAudioTrack ||
               aTrack == TrackInfo::kVideoTrack);
    auto& data = aTrack == TrackInfo::kAudioTrack ? mAudio : mVideo;
    data.mPolicy->Cancel();
    data.mTokenRequest.DisconnectIfExists();
    data.mInitRequest.DisconnectIfExists();
    if (data.mDecoder) {
      mOwner->mShutdownPromisePool->ShutdownDecoder(data.mDecoder.forget());
    }
    data.mStage = Stage::None;
    MOZ_ASSERT(!data.mToken);
  }

private:
  class Wrapper;

  enum class Stage : int8_t
  {
    None,
    WaitForToken,
    CreateDecoder,
    WaitForInit
  };

  struct Data
  {
    Data(DecoderData& aOwnerData, TrackType aTrack, TaskQueue* aThread)
      : mOwnerData(aOwnerData)
      , mTrack(aTrack)
      , mPolicy(new LocalAllocPolicy(aTrack, aThread)) { }
    DecoderData& mOwnerData;
    const TrackType mTrack;
    RefPtr<LocalAllocPolicy> mPolicy;
    Stage mStage = Stage::None;
    RefPtr<Token> mToken;
    RefPtr<MediaDataDecoder> mDecoder;
    MozPromiseRequestHolder<TokenPromise> mTokenRequest;
    MozPromiseRequestHolder<InitPromise> mInitRequest;
  } mAudio, mVideo;

  void RunStage(Data& aData);
  MediaResult DoCreateDecoder(Data& aData);
  void DoInitDecoder(Data& aData);

  // guaranteed to be valid by the owner.
  const NotNull<MediaFormatReader*> mOwner;
};

void
MediaFormatReader::DecoderFactory::CreateDecoder(TrackType aTrack)
{
  MOZ_ASSERT(aTrack == TrackInfo::kAudioTrack ||
             aTrack == TrackInfo::kVideoTrack);
  RunStage(aTrack == TrackInfo::kAudioTrack ? mAudio : mVideo);
}

class MediaFormatReader::DecoderFactory::Wrapper : public MediaDataDecoder
{
  using Token = GlobalAllocPolicy::Token;

public:
  Wrapper(already_AddRefed<MediaDataDecoder> aDecoder,
          already_AddRefed<Token> aToken)
    : mDecoder(aDecoder)
    , mToken(aToken)
  {
    DecoderDoctorLogger::LogConstructionAndBase(
      "MediaFormatReader::DecoderFactory::Wrapper",
      this,
      static_cast<const MediaDataDecoder*>(this));
    DecoderDoctorLogger::LinkParentAndChild(
      "MediaFormatReader::DecoderFactory::Wrapper",
      this,
      "decoder",
      mDecoder.get());
  }

  ~Wrapper()
  {
    DecoderDoctorLogger::LogDestruction(
      "MediaFormatReader::DecoderFactory::Wrapper", this);
  }

  RefPtr<InitPromise> Init() override { return mDecoder->Init(); }
  RefPtr<DecodePromise> Decode(MediaRawData* aSample) override
  {
    return mDecoder->Decode(aSample);
  }
  RefPtr<DecodePromise> Drain() override { return mDecoder->Drain(); }
  RefPtr<FlushPromise> Flush() override { return mDecoder->Flush(); }
  bool IsHardwareAccelerated(nsACString& aFailureReason) const override
  {
    return mDecoder->IsHardwareAccelerated(aFailureReason);
  }
  nsCString GetDescriptionName() const override
  {
    return mDecoder->GetDescriptionName();
  }
  void SetSeekThreshold(const TimeUnit& aTime) override
  {
    mDecoder->SetSeekThreshold(aTime);
  }
  bool SupportDecoderRecycling() const override
  {
    return mDecoder->SupportDecoderRecycling();
  }
  RefPtr<ShutdownPromise> Shutdown() override
  {
    RefPtr<MediaDataDecoder> decoder = mDecoder.forget();
    RefPtr<Token> token = mToken.forget();
    return decoder->Shutdown()->Then(
      AbstractThread::GetCurrent(), __func__,
      [token]() {
        return ShutdownPromise::CreateAndResolve(true, __func__);
      });
  }

private:
  RefPtr<MediaDataDecoder> mDecoder;
  RefPtr<Token> mToken;
};

void
MediaFormatReader::DecoderFactory::RunStage(Data& aData)
{
  switch (aData.mStage) {
    case Stage::None: {
      MOZ_ASSERT(!aData.mToken);
      aData.mPolicy->Alloc()->Then(
        mOwner->OwnerThread(), __func__,
        [this, &aData] (RefPtr<Token> aToken) {
          aData.mTokenRequest.Complete();
          aData.mToken = aToken.forget();
          aData.mStage = Stage::CreateDecoder;
          RunStage(aData);
        },
        [&aData] () {
          aData.mTokenRequest.Complete();
          aData.mStage = Stage::None;
        })->Track(aData.mTokenRequest);
      aData.mStage = Stage::WaitForToken;
      break;
    }

    case Stage::WaitForToken: {
      MOZ_ASSERT(!aData.mToken);
      MOZ_ASSERT(aData.mTokenRequest.Exists());
      break;
    }

    case Stage::CreateDecoder: {
      MOZ_ASSERT(aData.mToken);
      MOZ_ASSERT(!aData.mDecoder);
      MOZ_ASSERT(!aData.mInitRequest.Exists());

      MediaResult rv = DoCreateDecoder(aData);
      if (NS_FAILED(rv)) {
        NS_WARNING("Error constructing decoders");
        aData.mToken = nullptr;
        aData.mStage = Stage::None;
        aData.mOwnerData.mDescription = rv.Description();
        DDLOGEX2("MediaFormatReader::DecoderFactory",
                 this,
                 DDLogCategory::Log,
                 "create_decoder_error",
                 rv);
        mOwner->NotifyError(aData.mTrack, rv);
        return;
      }

      aData.mDecoder = new Wrapper(aData.mDecoder.forget(), aData.mToken.forget());
      DecoderDoctorLogger::LinkParentAndChild(
        aData.mDecoder.get(),
        "decoder",
        "MediaFormatReader::DecoderFactory",
        this);

      DoInitDecoder(aData);
      aData.mStage = Stage::WaitForInit;
      break;
    }

    case Stage::WaitForInit: {
      MOZ_ASSERT(aData.mDecoder);
      MOZ_ASSERT(aData.mInitRequest.Exists());
      break;
    }
  }
}

MediaResult
MediaFormatReader::DecoderFactory::DoCreateDecoder(Data& aData)
{
  auto& ownerData = aData.mOwnerData;

  if (!mOwner->mPlatform) {
    mOwner->mPlatform = new PDMFactory();
    if (mOwner->IsEncrypted()) {
      MOZ_ASSERT(mOwner->mCDMProxy);
      mOwner->mPlatform->SetCDMProxy(mOwner->mCDMProxy);
    }
  }

  // result may not be updated by PDMFactory::CreateDecoder, as such it must be
  // initialized to a fatal error by default.
  MediaResult result = MediaResult(
    NS_ERROR_DOM_MEDIA_FATAL_ERR,
    nsPrintfCString("error creating %s decoder", TrackTypeToStr(aData.mTrack)));

  switch (aData.mTrack) {
    case TrackInfo::kAudioTrack: {
      aData.mDecoder = mOwner->mPlatform->CreateDecoder({
        ownerData.mInfo
        ? *ownerData.mInfo->GetAsAudioInfo()
        : *ownerData.mOriginalInfo->GetAsAudioInfo(),
        ownerData.mTaskQueue,
        mOwner->mCrashHelper,
        CreateDecoderParams::UseNullDecoder(ownerData.mIsNullDecode),
        &result,
        TrackInfo::kAudioTrack,
        &mOwner->OnTrackWaitingForKeyProducer()
      });
      break;
    }

    case TrackType::kVideoTrack: {
      // Decoders use the layers backend to decide if they can use hardware decoding,
      // so specify LAYERS_NONE if we want to forcibly disable it.
      aData.mDecoder = mOwner->mPlatform->CreateDecoder(
        { ownerData.mInfo ? *ownerData.mInfo->GetAsVideoInfo()
                          : *ownerData.mOriginalInfo->GetAsVideoInfo(),
          ownerData.mTaskQueue,
          mOwner->mKnowsCompositor,
          mOwner->GetImageContainer(),
          mOwner->mCrashHelper,
          CreateDecoderParams::UseNullDecoder(ownerData.mIsNullDecode),
          &result,
          TrackType::kVideoTrack,
          &mOwner->OnTrackWaitingForKeyProducer(),
          CreateDecoderParams::VideoFrameRate(ownerData.mMeanRate.Mean()) });
      break;
    }

    default:
      break;
  }

  if (aData.mDecoder) {
    return NS_OK;
  }

  MOZ_RELEASE_ASSERT(NS_FAILED(result), "PDM returned an invalid error code");

  return result;
}

void
MediaFormatReader::DecoderFactory::DoInitDecoder(Data& aData)
{
  auto& ownerData = aData.mOwnerData;

  DDLOGEX2("MediaFormatReader::DecoderFactory",
           this,
           DDLogCategory::Log,
           "initialize_decoder",
           DDNoValue{});
  aData.mDecoder->Init()
    ->Then(mOwner->OwnerThread(), __func__,
           [this, &aData, &ownerData](TrackType aTrack) {
             aData.mInitRequest.Complete();
             aData.mStage = Stage::None;
             MutexAutoLock lock(ownerData.mMutex);
             ownerData.mDecoder = aData.mDecoder.forget();
             ownerData.mDescription = ownerData.mDecoder->GetDescriptionName();
             DDLOGEX2("MediaFormatReader::DecoderFactory",
                      this,
                      DDLogCategory::Log,
                      "decoder_initialized",
                      DDNoValue{});
             DecoderDoctorLogger::LinkParentAndChild(
               "MediaFormatReader::DecoderData",
               &ownerData,
               "decoder",
               ownerData.mDecoder.get());
             mOwner->SetVideoDecodeThreshold();
             mOwner->ScheduleUpdate(aTrack);
           },
           [this, &aData, &ownerData](const MediaResult& aError) {
             aData.mInitRequest.Complete();
             MOZ_RELEASE_ASSERT(!ownerData.mDecoder,
                                "Can't have a decoder already set");
             aData.mStage = Stage::None;
             mOwner->mShutdownPromisePool->ShutdownDecoder(aData.mDecoder.forget());
             DDLOGEX2("MediaFormatReader::DecoderFactory",
                      this,
                      DDLogCategory::Log,
                      "initialize_decoder_error",
                      aError);
             mOwner->NotifyError(aData.mTrack, aError);
           })
    ->Track(aData.mInitRequest);
}

// DemuxerProxy ensures that the original main demuxer is only ever accessed
// via its own dedicated task queue.
// This ensure that the reader's taskqueue will never blocked while a demuxer
// is itself blocked attempting to access the MediaCache or the MediaResource.
class MediaFormatReader::DemuxerProxy
{
  using TrackType = TrackInfo::TrackType;
  class Wrapper;

public:
  explicit DemuxerProxy(MediaDataDemuxer* aDemuxer)
    : mTaskQueue(new AutoTaskQueue(
        GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER),
        "DemuxerProxy::mTaskQueue"))
    , mData(new Data(aDemuxer))
  {
    MOZ_COUNT_CTOR(DemuxerProxy);
  }

  ~DemuxerProxy()
  {
    MOZ_COUNT_DTOR(DemuxerProxy);
  }

  RefPtr<ShutdownPromise> Shutdown()
  {
    RefPtr<Data> data = mData.forget();
    return InvokeAsync(mTaskQueue, __func__, [data]() {
      // We need to clear our reference to the demuxer now. So that in the event
      // the init promise wasn't resolved, such as what can happen with the
      // mediasource demuxer that is waiting on more data, it will force the
      // init promise to be rejected.
      data->mDemuxer = nullptr;
      data->mAudioDemuxer = nullptr;
      data->mVideoDemuxer = nullptr;
      return ShutdownPromise::CreateAndResolve(true, __func__);
    });
  }

  RefPtr<MediaDataDemuxer::InitPromise> Init();

  Wrapper*
  GetTrackDemuxer(TrackType aTrack, uint32_t aTrackNumber)
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    switch (aTrack) {
      case TrackInfo::kAudioTrack:
        return mData->mAudioDemuxer;
      case TrackInfo::kVideoTrack:
        return mData->mVideoDemuxer;
      default:
        return nullptr;
    }
  }

  uint32_t GetNumberTracks(TrackType aTrack) const
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    switch (aTrack) {
      case TrackInfo::kAudioTrack:
        return mData->mNumAudioTrack;
      case TrackInfo::kVideoTrack:
        return mData->mNumVideoTrack;
      default:
        return 0;
    }
  }

  bool IsSeekable() const
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    return mData->mSeekable;
  }

  bool IsSeekableOnlyInBufferedRanges() const
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    return mData->mSeekableOnlyInBufferedRange;
  }

  UniquePtr<EncryptionInfo> GetCrypto() const
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    if (!mData->mCrypto) {
      return nullptr;
    }
    auto crypto = MakeUnique<EncryptionInfo>();
    *crypto = *mData->mCrypto;
    return crypto;
  }

  RefPtr<NotifyDataArrivedPromise> NotifyDataArrived();

  bool ShouldComputeStartTime() const
  {
    MOZ_RELEASE_ASSERT(mData && mData->mInitDone);

    return mData->mShouldComputeStartTime;
  }

private:
  const RefPtr<AutoTaskQueue> mTaskQueue;
  struct Data
  {
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(Data)

    explicit Data(MediaDataDemuxer* aDemuxer)
      : mInitDone(false)
      , mDemuxer(aDemuxer)
    {
    }

    Atomic<bool> mInitDone;
    // Only ever accessed over mTaskQueue once.
    RefPtr<MediaDataDemuxer> mDemuxer;
    // Only accessed once InitPromise has been resolved and immutable after.
    // So we can safely access them without the use of the mutex.
    uint32_t mNumAudioTrack = 0;
    RefPtr<Wrapper> mAudioDemuxer;
    uint32_t mNumVideoTrack = 0;
    RefPtr<Wrapper> mVideoDemuxer;
    bool mSeekable = false;
    bool mSeekableOnlyInBufferedRange = false;
    bool mShouldComputeStartTime = true;
    UniquePtr<EncryptionInfo> mCrypto;
  private:
    ~Data() { }
  };
  RefPtr<Data> mData;
};

class MediaFormatReader::DemuxerProxy::Wrapper : public MediaTrackDemuxer
{
public:
  Wrapper(MediaTrackDemuxer* aTrackDemuxer, AutoTaskQueue* aTaskQueue)
    : mMutex("TrackDemuxer Mutex")
    , mTaskQueue(aTaskQueue)
    , mGetSamplesMayBlock(aTrackDemuxer->GetSamplesMayBlock())
    , mInfo(aTrackDemuxer->GetInfo())
    , mTrackDemuxer(aTrackDemuxer)
  {
    DecoderDoctorLogger::LogConstructionAndBase(
      "MediaFormatReader::DemuxerProxy::Wrapper",
      this,
      static_cast<const MediaTrackDemuxer*>(this));
    DecoderDoctorLogger::LinkParentAndChild(
      "MediaFormatReader::DemuxerProxy::Wrapper",
      this,
      "track demuxer",
      aTrackDemuxer);
  }

  UniquePtr<TrackInfo> GetInfo() const override
  {
    if (!mInfo) {
      return nullptr;
    }
    return mInfo->Clone();
  }

  RefPtr<SeekPromise> Seek(const TimeUnit& aTime) override
  {
    RefPtr<Wrapper> self = this;
    return InvokeAsync(
             mTaskQueue, __func__,
             [self, aTime]() { return self->mTrackDemuxer->Seek(aTime); })
      ->Then(mTaskQueue, __func__,
             [self](const TimeUnit& aTime) {
               self->UpdateRandomAccessPoint();
               return SeekPromise::CreateAndResolve(aTime, __func__);
             },
             [self](const MediaResult& aError) {
               self->UpdateRandomAccessPoint();
               return SeekPromise::CreateAndReject(aError, __func__);
             });
  }

  RefPtr<SamplesPromise> GetSamples(int32_t aNumSamples) override
  {
    RefPtr<Wrapper> self = this;
    return InvokeAsync(mTaskQueue, __func__,
                       [self, aNumSamples]() {
                         return self->mTrackDemuxer->GetSamples(aNumSamples);
                       })
      ->Then(mTaskQueue, __func__,
             [self](RefPtr<SamplesHolder> aSamples) {
               self->UpdateRandomAccessPoint();
               return SamplesPromise::CreateAndResolve(aSamples.forget(), __func__);
             },
             [self](const MediaResult& aError) {
               self->UpdateRandomAccessPoint();
               return SamplesPromise::CreateAndReject(aError, __func__);
             });
  }

  bool GetSamplesMayBlock() const override
  {
    return mGetSamplesMayBlock;
  }

  void Reset() override
  {
    RefPtr<Wrapper> self = this;
    nsresult rv =
      mTaskQueue->Dispatch(
        NS_NewRunnableFunction("MediaFormatReader::DemuxerProxy::Wrapper::Reset",
                               [self]() { self->mTrackDemuxer->Reset(); }));
    MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));
    Unused << rv;
  }

  nsresult GetNextRandomAccessPoint(TimeUnit* aTime) override
  {
    MutexAutoLock lock(mMutex);
    if (NS_SUCCEEDED(mNextRandomAccessPointResult)) {
      *aTime = mNextRandomAccessPoint;
    }
    return mNextRandomAccessPointResult;
  }

  RefPtr<SkipAccessPointPromise>
  SkipToNextRandomAccessPoint(const TimeUnit& aTimeThreshold) override
  {
    RefPtr<Wrapper> self = this;
    return InvokeAsync(
             mTaskQueue, __func__,
             [self, aTimeThreshold]()  {
               return self->mTrackDemuxer->SkipToNextRandomAccessPoint(
                 aTimeThreshold);
             })
      ->Then(mTaskQueue, __func__,
             [self](uint32_t aVal) {
               self->UpdateRandomAccessPoint();
               return SkipAccessPointPromise::CreateAndResolve(aVal, __func__);
             },
             [self](const SkipFailureHolder& aError) {
               self->UpdateRandomAccessPoint();
               return SkipAccessPointPromise::CreateAndReject(aError, __func__);
             });
  }

  TimeIntervals GetBuffered() override
  {
    MutexAutoLock lock(mMutex);
    return mBuffered;
  }

  void BreakCycles() override { }

private:
  Mutex mMutex;
  const RefPtr<AutoTaskQueue> mTaskQueue;
  const bool mGetSamplesMayBlock;
  const UniquePtr<TrackInfo> mInfo;
  // mTrackDemuxer is only ever accessed on demuxer's task queue.
  RefPtr<MediaTrackDemuxer> mTrackDemuxer;
  // All following members are protected by mMutex
  nsresult mNextRandomAccessPointResult = NS_OK;
  TimeUnit mNextRandomAccessPoint;
  TimeIntervals mBuffered;
  friend class DemuxerProxy;

  ~Wrapper()
  {
    RefPtr<MediaTrackDemuxer> trackDemuxer = mTrackDemuxer.forget();
    nsresult rv =
      mTaskQueue->Dispatch(NS_NewRunnableFunction(
        "MediaFormatReader::DemuxerProxy::Wrapper::~Wrapper",
        [trackDemuxer]() { trackDemuxer->BreakCycles(); }));
    MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));
    Unused << rv;
    DecoderDoctorLogger::LogDestruction(
      "MediaFormatReader::DemuxerProxy::Wrapper", this);
  }

  void UpdateRandomAccessPoint()
  {
    MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
    if (!mTrackDemuxer) {
      // Detached.
      return;
    }
    MutexAutoLock lock(mMutex);
    mNextRandomAccessPointResult =
      mTrackDemuxer->GetNextRandomAccessPoint(&mNextRandomAccessPoint);
  }

  void UpdateBuffered()
  {
    MOZ_ASSERT(mTaskQueue->IsCurrentThreadIn());
    if (!mTrackDemuxer) {
      // Detached.
      return;
    }
    MutexAutoLock lock(mMutex);
    mBuffered = mTrackDemuxer->GetBuffered();
  }
};

RefPtr<MediaDataDemuxer::InitPromise>
MediaFormatReader::DemuxerProxy::Init()
{
  using InitPromise = MediaDataDemuxer::InitPromise;

  RefPtr<Data> data = mData;
  RefPtr<AutoTaskQueue> taskQueue = mTaskQueue;
  return InvokeAsync(mTaskQueue, __func__,
                     [data, taskQueue]() {
                       if (!data->mDemuxer) {
                         return InitPromise::CreateAndReject(
                           NS_ERROR_DOM_MEDIA_CANCELED, __func__);
                       }
                       return data->mDemuxer->Init();
                     })
    ->Then(taskQueue, __func__,
           [data, taskQueue]() {
             if (!data->mDemuxer) { // Was shutdown.
               return InitPromise::CreateAndReject(
                   NS_ERROR_DOM_MEDIA_CANCELED, __func__);
             }
             data->mNumAudioTrack =
               data->mDemuxer->GetNumberTracks(TrackInfo::kAudioTrack);
             if (data->mNumAudioTrack) {
               RefPtr<MediaTrackDemuxer> d =
                 data->mDemuxer->GetTrackDemuxer(TrackInfo::kAudioTrack, 0);
               if (d) {
                 RefPtr<Wrapper> wrapper =
                   new DemuxerProxy::Wrapper(d, taskQueue);
                 wrapper->UpdateBuffered();
                 data->mAudioDemuxer = wrapper;
                 DecoderDoctorLogger::LinkParentAndChild(
                   data->mDemuxer.get(),
                   "decoder factory wrapper",
                   "MediaFormatReader::DecoderFactory::Wrapper",
                   wrapper.get());
               }
             }
             data->mNumVideoTrack =
               data->mDemuxer->GetNumberTracks(TrackInfo::kVideoTrack);
             if (data->mNumVideoTrack) {
               RefPtr<MediaTrackDemuxer> d =
                 data->mDemuxer->GetTrackDemuxer(TrackInfo::kVideoTrack, 0);
               if (d) {
                 RefPtr<Wrapper> wrapper =
                   new DemuxerProxy::Wrapper(d, taskQueue);
                 wrapper->UpdateBuffered();
                 data->mVideoDemuxer = wrapper;
                 DecoderDoctorLogger::LinkParentAndChild(
                   data->mDemuxer.get(),
                   "decoder factory wrapper",
                   "MediaFormatReader::DecoderFactory::Wrapper",
                   wrapper.get());
               }
             }
             data->mCrypto = data->mDemuxer->GetCrypto();
             data->mSeekable = data->mDemuxer->IsSeekable();
             data->mSeekableOnlyInBufferedRange =
               data->mDemuxer->IsSeekableOnlyInBufferedRanges();
             data->mShouldComputeStartTime =
               data->mDemuxer->ShouldComputeStartTime();
             data->mInitDone = true;
             return InitPromise::CreateAndResolve(NS_OK, __func__);
           },
           [](const MediaResult& aError) {
             return InitPromise::CreateAndReject(aError, __func__);
           });
}

RefPtr<MediaFormatReader::NotifyDataArrivedPromise>
MediaFormatReader::DemuxerProxy::NotifyDataArrived()
{
  RefPtr<Data> data = mData;
  return InvokeAsync(mTaskQueue, __func__, [data]() {
    if (!data->mDemuxer) {
      // Was shutdown.
      return NotifyDataArrivedPromise::CreateAndReject(
        NS_ERROR_DOM_MEDIA_CANCELED, __func__);
    }
    data->mDemuxer->NotifyDataArrived();
    if (data->mAudioDemuxer) {
      data->mAudioDemuxer->UpdateBuffered();
    }
    if (data->mVideoDemuxer) {
      data->mVideoDemuxer->UpdateBuffered();
    }
    return NotifyDataArrivedPromise::CreateAndResolve(true, __func__);
  });
}

MediaFormatReader::MediaFormatReader(MediaFormatReaderInit& aInit,
                                     MediaDataDemuxer* aDemuxer)
  : mTaskQueue(new TaskQueue(GetMediaThreadPool(MediaThreadType::PLAYBACK),
                             "MediaFormatReader::mTaskQueue",
                             /* aSupportsTailDispatch = */ true))
  , mAudio(this, MediaData::AUDIO_DATA,
           StaticPrefs::MediaAudioMaxDecodeError())
  , mVideo(this, MediaData::VIDEO_DATA,
           StaticPrefs::MediaVideoMaxDecodeError())
  , mDemuxer(new DemuxerProxy(aDemuxer))
  , mDemuxerInitDone(false)
  , mPendingNotifyDataArrived(false)
  , mLastReportedNumDecodedFrames(0)
  , mPreviousDecodedKeyframeTime_us(sNoPreviousDecodedKeyframe)
  , mKnowsCompositor(aInit.mKnowsCompositor)
  , mInitDone(false)
  , mTrackDemuxersMayBlock(false)
  , mSeekScheduled(false)
  , mVideoFrameContainer(aInit.mVideoFrameContainer)
  , mCrashHelper(aInit.mCrashHelper)
  , mDecoderFactory(new DecoderFactory(this))
  , mShutdownPromisePool(new ShutdownPromisePool())
  , mBuffered(mTaskQueue,
              TimeIntervals(),
              "MediaFormatReader::mBuffered (Canonical)")
  , mFrameStats(aInit.mFrameStats)
  , mMediaDecoderOwnerID(aInit.mMediaDecoderOwnerID)
{
  MOZ_ASSERT(aDemuxer);
  MOZ_COUNT_CTOR(MediaFormatReader);
  DDLINKCHILD(
    "audio decoder data", "MediaFormatReader::DecoderDataWithPromise", &mAudio);
  DDLINKCHILD(
    "video decoder data", "MediaFormatReader::DecoderDataWithPromise", &mVideo);
  DDLINKCHILD("demuxer", aDemuxer);
  mOnTrackWaitingForKeyListener = OnTrackWaitingForKey().Connect(
    mTaskQueue, this, &MediaFormatReader::NotifyWaitingForKey);
}

MediaFormatReader::~MediaFormatReader()
{
  MOZ_COUNT_DTOR(MediaFormatReader);
  MOZ_ASSERT(mShutdown);
}

RefPtr<ShutdownPromise>
MediaFormatReader::Shutdown()
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("");

  mDemuxerInitRequest.DisconnectIfExists();
  mNotifyDataArrivedPromise.DisconnectIfExists();
  mMetadataPromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mSeekPromise.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  mSkipRequest.DisconnectIfExists();
  mSetCDMPromise.RejectIfExists(
    MediaResult(NS_ERROR_DOM_INVALID_STATE_ERR,
                "MediaFormatReader is shutting down"),
    __func__);

  if (mAudio.HasPromise()) {
    mAudio.RejectPromise(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  }
  if (mVideo.HasPromise()) {
    mVideo.RejectPromise(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  }

  if (HasAudio()) {
    mAudio.ResetDemuxer();
    mAudio.mTrackDemuxer->BreakCycles();
    mAudio.mTrackDemuxer = nullptr;
    mAudio.ResetState();
    ShutdownDecoder(TrackInfo::kAudioTrack);
  }

  if (HasVideo()) {
    mVideo.ResetDemuxer();
    mVideo.mTrackDemuxer->BreakCycles();
    mVideo.mTrackDemuxer = nullptr;
    mVideo.ResetState();
    ShutdownDecoder(TrackInfo::kVideoTrack);
  }

  mShutdownPromisePool->Track(mDemuxer->Shutdown());
  mDemuxer = nullptr;

  mOnTrackWaitingForKeyListener.Disconnect();

  mShutdown = true;
  return mShutdownPromisePool->Shutdown()
    ->Then(OwnerThread(), __func__, this,
           &MediaFormatReader::TearDownDecoders,
           &MediaFormatReader::TearDownDecoders);
}

void
MediaFormatReader::ShutdownDecoder(TrackType aTrack)
{
  LOGV("%s", TrackTypeToStr(aTrack));

  // Shut down the pending decoder if any.
  mDecoderFactory->ShutdownDecoder(aTrack);

  auto& decoder = GetDecoderData(aTrack);
  // Flush the decoder if necessary.
  decoder.Flush();
  // Shut down the decoder if any.
  decoder.ShutdownDecoder();
}

RefPtr<ShutdownPromise>
MediaFormatReader::TearDownDecoders()
{
  if (mAudio.mTaskQueue) {
    mAudio.mTaskQueue->BeginShutdown();
    mAudio.mTaskQueue->AwaitShutdownAndIdle();
    mAudio.mTaskQueue = nullptr;
  }
  if (mVideo.mTaskQueue) {
    mVideo.mTaskQueue->BeginShutdown();
    mVideo.mTaskQueue->AwaitShutdownAndIdle();
    mVideo.mTaskQueue = nullptr;
  }

  mDecoderFactory = nullptr;
  mPlatform = nullptr;
  mVideoFrameContainer = nullptr;

  ReleaseResources();
  mBuffered.DisconnectAll();
  return mTaskQueue->BeginShutdown();
}

nsresult
MediaFormatReader::Init()
{
  MOZ_ASSERT(NS_IsMainThread(), "Must be on main thread.");

  mAudio.mTaskQueue = new TaskQueue(
    GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER),
    "MFR::mAudio::mTaskQueue");

  mVideo.mTaskQueue = new TaskQueue(
    GetMediaThreadPool(MediaThreadType::PLATFORM_DECODER),
    "MFR::mVideo::mTaskQueue");

  return NS_OK;
}

bool
MediaFormatReader::ResolveSetCDMPromiseIfDone(TrackType aTrack)
{
  // When a CDM proxy is set, MFR would shutdown the existing MediaDataDecoder
  // and would create new one for specific track in the next Update.
  MOZ_ASSERT(OnTaskQueue());

  if (mSetCDMPromise.IsEmpty()) {
    return true;
  }

  MOZ_ASSERT(mCDMProxy);
  if (mSetCDMForTracks.contains(aTrack)) {
    mSetCDMForTracks -= aTrack;
  }

  if (mSetCDMForTracks.isEmpty()) {
    LOGV("%s : Done ", __func__);
    mSetCDMPromise.Resolve(/* aIgnored = */ true, __func__);
    ScheduleUpdate(TrackInfo::kAudioTrack);
    ScheduleUpdate(TrackInfo::kVideoTrack);
    return true;
  }
  LOGV("%s : %s track is ready.", __func__, TrackTypeToStr(aTrack));
  return false;
}

void
MediaFormatReader::PrepareToSetCDMForTrack(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("%s : %s", __func__, TrackTypeToStr(aTrack));

  mSetCDMForTracks += aTrack;
  if (mCDMProxy) {
    // An old cdm proxy exists, so detaching old cdm proxy by shutting down
    // MediaDataDecoder.
    ShutdownDecoder(aTrack);
  }
  ScheduleUpdate(aTrack);
}

bool
MediaFormatReader::IsDecoderWaitingForCDM(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  return IsEncrypted() && mSetCDMForTracks.contains(aTrack) && !mCDMProxy;
}

RefPtr<SetCDMPromise>
MediaFormatReader::SetCDMProxy(CDMProxy* aProxy)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("SetCDMProxy (%p)", aProxy);

  if (mShutdown) {
    return SetCDMPromise::CreateAndReject(
      MediaResult(NS_ERROR_DOM_INVALID_STATE_ERR,
                  "MediaFormatReader is shutting down"),
      __func__);
  }

  mSetCDMPromise.RejectIfExists(
    MediaResult(NS_ERROR_DOM_INVALID_STATE_ERR,
                "Another new CDM proxy is being set."),
    __func__);

  // Shutdown all decoders as switching CDM proxy indicates that it's
  // inappropriate for the existing decoders to continue decoding via the old
  // CDM proxy.
  if (HasAudio()) {
    PrepareToSetCDMForTrack(TrackInfo::kAudioTrack);
  }
  if (HasVideo()) {
    PrepareToSetCDMForTrack(TrackInfo::kVideoTrack);
  }

  mCDMProxy = aProxy;

  if (IsEncrypted() && !mCDMProxy) {
    // Release old PDMFactory which contains an EMEDecoderModule.
    mPlatform = nullptr;
  }

  if (!mInitDone || mSetCDMForTracks.isEmpty() || !mCDMProxy) {
    // 1) MFR is not initialized yet or
    // 2) Demuxer is initialized without active audio and video or
    // 3) A null cdm proxy is set
    // the promise can be resolved directly.
    mSetCDMForTracks.clear();
    return SetCDMPromise::CreateAndResolve(/* aIgnored = */ true, __func__);
  }

  RefPtr<SetCDMPromise> p = mSetCDMPromise.Ensure(__func__);
  return p;
}

bool
MediaFormatReader::IsWaitingOnCDMResource()
{
  MOZ_ASSERT(OnTaskQueue());
  return IsEncrypted() && !mCDMProxy;
}

RefPtr<MediaFormatReader::MetadataPromise>
MediaFormatReader::AsyncReadMetadata()
{
  MOZ_ASSERT(OnTaskQueue());

  MOZ_DIAGNOSTIC_ASSERT(mMetadataPromise.IsEmpty());

  if (mInitDone) {
    // We are returning from dormant.
    MetadataHolder metadata;
    metadata.mInfo = MakeUnique<MediaInfo>(mInfo);
    return MetadataPromise::CreateAndResolve(Move(metadata), __func__);
  }

  RefPtr<MetadataPromise> p = mMetadataPromise.Ensure(__func__);

  mDemuxer->Init()
    ->Then(OwnerThread(), __func__, this,
           &MediaFormatReader::OnDemuxerInitDone,
           &MediaFormatReader::OnDemuxerInitFailed)
    ->Track(mDemuxerInitRequest);
  return p;
}

void
MediaFormatReader::OnDemuxerInitDone(const MediaResult& aResult)
{
  MOZ_ASSERT(OnTaskQueue());
  mDemuxerInitRequest.Complete();

  if (NS_FAILED(aResult) && StaticPrefs::MediaPlaybackWarningsAsErrors()) {
    mMetadataPromise.Reject(aResult, __func__);
    return;
  }

  mDemuxerInitDone = true;

  UniquePtr<MetadataTags> tags(MakeUnique<MetadataTags>());

  RefPtr<PDMFactory> platform;
  if (!IsWaitingOnCDMResource()) {
    platform = new PDMFactory();
  }

  // To decode, we need valid video and a place to put it.
  bool videoActive =
    !!mDemuxer->GetNumberTracks(TrackInfo::kVideoTrack) && GetImageContainer();

  if (videoActive) {
    // We currently only handle the first video track.
    mVideo.mTrackDemuxer = mDemuxer->GetTrackDemuxer(TrackInfo::kVideoTrack, 0);
    if (!mVideo.mTrackDemuxer) {
      mMetadataPromise.Reject(NS_ERROR_DOM_MEDIA_METADATA_ERR, __func__);
      return;
    }

    UniquePtr<TrackInfo> videoInfo = mVideo.mTrackDemuxer->GetInfo();
    videoActive = videoInfo && videoInfo->IsValid();
    if (videoActive) {
      if (platform &&
          !platform->SupportsMimeType(videoInfo->mMimeType, nullptr)) {
        // We have no decoder for this track. Error.
        mMetadataPromise.Reject(NS_ERROR_DOM_MEDIA_METADATA_ERR, __func__);
        return;
      }
      {
        MutexAutoLock lock(mVideo.mMutex);
        mInfo.mVideo = *videoInfo->GetAsVideoInfo();
      }
      for (const MetadataTag& tag : videoInfo->mTags) {
        tags->Put(tag.mKey, tag.mValue);
      }
      mVideo.mOriginalInfo = Move(videoInfo);
      mTrackDemuxersMayBlock |= mVideo.mTrackDemuxer->GetSamplesMayBlock();
    } else {
      mVideo.mTrackDemuxer->BreakCycles();
      mVideo.mTrackDemuxer = nullptr;
    }
  }

  bool audioActive = !!mDemuxer->GetNumberTracks(TrackInfo::kAudioTrack);
  if (audioActive) {
    mAudio.mTrackDemuxer = mDemuxer->GetTrackDemuxer(TrackInfo::kAudioTrack, 0);
    if (!mAudio.mTrackDemuxer) {
      mMetadataPromise.Reject(NS_ERROR_DOM_MEDIA_METADATA_ERR, __func__);
      return;
    }

    UniquePtr<TrackInfo> audioInfo = mAudio.mTrackDemuxer->GetInfo();
    // We actively ignore audio tracks that we know we can't play.
    audioActive =
      audioInfo && audioInfo->IsValid() &&
      (!platform || platform->SupportsMimeType(audioInfo->mMimeType, nullptr));

    if (audioActive) {
      {
        MutexAutoLock lock(mAudio.mMutex);
        mInfo.mAudio = *audioInfo->GetAsAudioInfo();
      }
      for (const MetadataTag& tag : audioInfo->mTags) {
        tags->Put(tag.mKey, tag.mValue);
      }
      mAudio.mOriginalInfo = Move(audioInfo);
      mTrackDemuxersMayBlock |= mAudio.mTrackDemuxer->GetSamplesMayBlock();
    } else {
      mAudio.mTrackDemuxer->BreakCycles();
      mAudio.mTrackDemuxer = nullptr;
    }
  }

  UniquePtr<EncryptionInfo> crypto = mDemuxer->GetCrypto();
  if (crypto && crypto->IsEncrypted()) {
    // Try and dispatch 'encrypted'. Won't go if ready state still HAVE_NOTHING.
    for (uint32_t i = 0; i < crypto->mInitDatas.Length(); i++) {
      mOnEncrypted.Notify(crypto->mInitDatas[i].mInitData,
                          crypto->mInitDatas[i].mType);
    }
    mInfo.mCrypto = *crypto;
  }

  auto videoDuration = HasVideo() ? mInfo.mVideo.mDuration : TimeUnit::Zero();
  auto audioDuration = HasAudio() ? mInfo.mAudio.mDuration : TimeUnit::Zero();

  auto duration = std::max(videoDuration, audioDuration);
  if (duration.IsPositive()) {
    mInfo.mMetadataDuration = Some(duration);
  }

  mInfo.mMediaSeekable = mDemuxer->IsSeekable();
  mInfo.mMediaSeekableOnlyInBufferedRanges =
    mDemuxer->IsSeekableOnlyInBufferedRanges();

  if (!videoActive && !audioActive) {
    mMetadataPromise.Reject(NS_ERROR_DOM_MEDIA_METADATA_ERR, __func__);
    return;
  }

  mTags = Move(tags);
  mInitDone = true;

  // Try to get the start time.
  // For MSE case, the start time of each track is assumed to be 0.
  // For others, we must demux the first sample to know the start time for each
  // track.
  if (!mDemuxer->ShouldComputeStartTime()) {
    mAudio.mFirstDemuxedSampleTime.emplace(TimeUnit::Zero());
    mVideo.mFirstDemuxedSampleTime.emplace(TimeUnit::Zero());
  } else {
    if (HasAudio()) {
      RequestDemuxSamples(TrackInfo::kAudioTrack);
    }

    if (HasVideo()) {
      RequestDemuxSamples(TrackInfo::kVideoTrack);
    }
  }

  if (aResult != NS_OK) {
    mOnDecodeWarning.Notify(aResult);
  }

  MaybeResolveMetadataPromise();
}

void
MediaFormatReader::MaybeResolveMetadataPromise()
{
  MOZ_ASSERT(OnTaskQueue());

  if ((HasAudio() && mAudio.mFirstDemuxedSampleTime.isNothing()) ||
      (HasVideo() && mVideo.mFirstDemuxedSampleTime.isNothing())) {
    return;
  }

  TimeUnit startTime =
    std::min(mAudio.mFirstDemuxedSampleTime.refOr(TimeUnit::FromInfinity()),
             mVideo.mFirstDemuxedSampleTime.refOr(TimeUnit::FromInfinity()));

  if (!startTime.IsInfinite()) {
    mInfo.mStartTime = startTime; // mInfo.mStartTime is initialized to 0.
  }

  MetadataHolder metadata;
  metadata.mInfo = MakeUnique<MediaInfo>(mInfo);
  metadata.mTags = mTags->Count() ? Move(mTags) : nullptr;

  // We now have all the informations required to calculate the initial buffered
  // range.
  mHasStartTime = true;
  UpdateBuffered();

  mMetadataPromise.Resolve(Move(metadata), __func__);
}

bool
MediaFormatReader::IsEncrypted() const
{
  return (HasAudio() && mInfo.mAudio.mCrypto.mValid) ||
         (HasVideo() && mInfo.mVideo.mCrypto.mValid);
}

void
MediaFormatReader::OnDemuxerInitFailed(const MediaResult& aError)
{
  mDemuxerInitRequest.Complete();
  mMetadataPromise.Reject(aError, __func__);
}

void
MediaFormatReader::ReadUpdatedMetadata(MediaInfo* aInfo)
{
  *aInfo = mInfo;
}

MediaFormatReader::DecoderData&
MediaFormatReader::GetDecoderData(TrackType aTrack)
{
  MOZ_ASSERT(aTrack == TrackInfo::kAudioTrack ||
             aTrack == TrackInfo::kVideoTrack);
  if (aTrack == TrackInfo::kAudioTrack) {
    return mAudio;
  }
  return mVideo;
}

bool
MediaFormatReader::ShouldSkip(TimeUnit aTimeThreshold)
{
  MOZ_ASSERT(HasVideo());

  if (!StaticPrefs::MediaDecoderSkipToNextKeyFrameEnabled()) {
    return false;
  }

  TimeUnit nextKeyframe;
  nsresult rv = mVideo.mTrackDemuxer->GetNextRandomAccessPoint(&nextKeyframe);
  if (NS_FAILED(rv)) {
    // Only OggTrackDemuxer with video type gets into here.
    // We don't support skip-to-next-frame for this case.
    return false;
  }
  return (nextKeyframe <= aTimeThreshold ||
          (mVideo.mTimeThreshold &&
           mVideo.mTimeThreshold.ref().EndTime() < aTimeThreshold)) &&
         nextKeyframe.ToMicroseconds() >= 0 && !nextKeyframe.IsInfinite();
}

RefPtr<MediaFormatReader::VideoDataPromise>
MediaFormatReader::RequestVideoData(const TimeUnit& aTimeThreshold)
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_DIAGNOSTIC_ASSERT(mSeekPromise.IsEmpty(),
                        "No sample requests allowed while seeking");
  MOZ_DIAGNOSTIC_ASSERT(!mVideo.HasPromise(), "No duplicate sample requests");
  MOZ_DIAGNOSTIC_ASSERT(!mVideo.mSeekRequest.Exists() ||
                        mVideo.mTimeThreshold.isSome());
  MOZ_DIAGNOSTIC_ASSERT(!IsSeeking(), "called mid-seek");
  LOGV("RequestVideoData(%" PRId64 ")", aTimeThreshold.ToMicroseconds());

  if (!HasVideo()) {
    LOG("called with no video track");
    return VideoDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                                             __func__);
  }

  if (IsSeeking()) {
    LOG("called mid-seek. Rejecting.");
    return VideoDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_CANCELED,
                                             __func__);
  }

  if (mShutdown) {
    NS_WARNING("RequestVideoData on shutdown MediaFormatReader!");
    return VideoDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_CANCELED,
                                             __func__);
  }

  // Ensure we have no pending seek going as ShouldSkip could return out of date
  // information.
  if (!mVideo.HasInternalSeekPending() && ShouldSkip(aTimeThreshold)) {
    RefPtr<VideoDataPromise> p = mVideo.EnsurePromise(__func__);
    SkipVideoDemuxToNextKeyFrame(aTimeThreshold);
    return p;
  }

  RefPtr<VideoDataPromise> p = mVideo.EnsurePromise(__func__);
  ScheduleUpdate(TrackInfo::kVideoTrack);

  return p;
}

void
MediaFormatReader::OnDemuxFailed(TrackType aTrack, const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("Failed to demux %s, failure:%s",
      aTrack == TrackType::kVideoTrack ? "video" : "audio",
      aError.ErrorName().get());
  auto& decoder = GetDecoderData(aTrack);
  decoder.mDemuxRequest.Complete();
  switch (aError.Code()) {
    case NS_ERROR_DOM_MEDIA_END_OF_STREAM:
      DDLOG(DDLogCategory::Log,
            aTrack == TrackType::kVideoTrack ? "video_demux_interruption"
                                             : "audio_demux_interruption",
            aError);
      if (!decoder.mWaitingForData) {
        decoder.RequestDrain();
      }
      NotifyEndOfStream(aTrack);
      break;
    case NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA:
      DDLOG(DDLogCategory::Log,
            aTrack == TrackType::kVideoTrack ? "video_demux_interruption"
                                             : "audio_demux_interruption",
            aError);
      if (!decoder.mWaitingForData) {
        decoder.RequestDrain();
      }
      NotifyWaitingForData(aTrack);
      break;
    case NS_ERROR_DOM_MEDIA_CANCELED:
      DDLOG(DDLogCategory::Log,
            aTrack == TrackType::kVideoTrack ? "video_demux_interruption"
                                             : "audio_demux_interruption",
            aError);
      if (decoder.HasPromise()) {
        decoder.RejectPromise(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
      }
      break;
    default:
      DDLOG(DDLogCategory::Log,
            aTrack == TrackType::kVideoTrack ? "video_demux_error"
                                             : "audio_demux_error",
            aError);
      NotifyError(aTrack, aError);
      break;
  }
}

void
MediaFormatReader::DoDemuxVideo()
{
  using SamplesPromise = MediaTrackDemuxer::SamplesPromise;

  DDLOG(DDLogCategory::Log, "video_demuxing", DDNoValue{});
  auto p = mVideo.mTrackDemuxer->GetSamples(1);

  if (mVideo.mFirstDemuxedSampleTime.isNothing()) {
    RefPtr<MediaFormatReader> self = this;
    p = p->Then(
      OwnerThread(),
      __func__,
      [self](RefPtr<MediaTrackDemuxer::SamplesHolder> aSamples) {
        DDLOGEX(
          self.get(), DDLogCategory::Log, "video_first_demuxed", DDNoValue{});
        self->OnFirstDemuxCompleted(TrackInfo::kVideoTrack, aSamples);
        return SamplesPromise::CreateAndResolve(aSamples.forget(), __func__);
      },
      [self](const MediaResult& aError) {
        DDLOGEX(
          self.get(), DDLogCategory::Log, "video_first_demuxing_error", aError);
        self->OnFirstDemuxFailed(TrackInfo::kVideoTrack, aError);
        return SamplesPromise::CreateAndReject(aError, __func__);
      });
  }

  p->Then(OwnerThread(), __func__, this,
          &MediaFormatReader::OnVideoDemuxCompleted,
          &MediaFormatReader::OnVideoDemuxFailed)
   ->Track(mVideo.mDemuxRequest);
}

void
MediaFormatReader::OnVideoDemuxCompleted(
  RefPtr<MediaTrackDemuxer::SamplesHolder> aSamples)
{
  LOGV("%zu video samples demuxed (sid:%d)",
       aSamples->mSamples.Length(),
       aSamples->mSamples[0]->mTrackInfo
       ? aSamples->mSamples[0]->mTrackInfo->GetID()
       : 0);
  DDLOG(DDLogCategory::Log,
        "video_demuxed_samples",
        uint64_t(aSamples->mSamples.Length()));
  mVideo.mDemuxRequest.Complete();
  mVideo.mQueuedSamples.AppendElements(aSamples->mSamples);
  ScheduleUpdate(TrackInfo::kVideoTrack);
}

RefPtr<MediaFormatReader::AudioDataPromise>
MediaFormatReader::RequestAudioData()
{
  MOZ_ASSERT(OnTaskQueue());
  MOZ_DIAGNOSTIC_ASSERT(!mAudio.HasPromise(), "No duplicate sample requests");
  MOZ_DIAGNOSTIC_ASSERT(IsVideoSeeking() || mSeekPromise.IsEmpty(),
                        "No sample requests allowed while seeking");
  MOZ_DIAGNOSTIC_ASSERT(IsVideoSeeking() || !mAudio.mSeekRequest.Exists() ||
                        mAudio.mTimeThreshold.isSome());
  MOZ_DIAGNOSTIC_ASSERT(IsVideoSeeking() || !IsSeeking(), "called mid-seek");
  LOGV("");

  if (!HasAudio()) {
    LOG("called with no audio track");
    return AudioDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                                             __func__);
  }

  if (IsSeeking()) {
    LOG("called mid-seek. Rejecting.");
    return AudioDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_CANCELED,
                                             __func__);
  }

  if (mShutdown) {
    NS_WARNING("RequestAudioData on shutdown MediaFormatReader!");
    return AudioDataPromise::CreateAndReject(NS_ERROR_DOM_MEDIA_CANCELED,
                                             __func__);
  }

  RefPtr<AudioDataPromise> p = mAudio.EnsurePromise(__func__);
  ScheduleUpdate(TrackInfo::kAudioTrack);

  return p;
}

void
MediaFormatReader::DoDemuxAudio()
{
  using SamplesPromise = MediaTrackDemuxer::SamplesPromise;

  DDLOG(DDLogCategory::Log, "audio_demuxing", DDNoValue{});
  auto p = mAudio.mTrackDemuxer->GetSamples(1);

  if (mAudio.mFirstDemuxedSampleTime.isNothing()) {
    RefPtr<MediaFormatReader> self = this;
    p = p->Then(
      OwnerThread(),
      __func__,
      [self](RefPtr<MediaTrackDemuxer::SamplesHolder> aSamples) {
        DDLOGEX(
          self.get(), DDLogCategory::Log, "audio_first_demuxed", DDNoValue{});
        self->OnFirstDemuxCompleted(TrackInfo::kAudioTrack, aSamples);
        return SamplesPromise::CreateAndResolve(aSamples.forget(), __func__);
      },
      [self](const MediaResult& aError) {
        DDLOGEX(
          self.get(), DDLogCategory::Log, "audio_first_demuxing_error", aError);
        self->OnFirstDemuxFailed(TrackInfo::kAudioTrack, aError);
        return SamplesPromise::CreateAndReject(aError, __func__);
      });
  }

  p->Then(OwnerThread(), __func__, this,
          &MediaFormatReader::OnAudioDemuxCompleted,
          &MediaFormatReader::OnAudioDemuxFailed)
   ->Track(mAudio.mDemuxRequest);
}

void
MediaFormatReader::OnAudioDemuxCompleted(
  RefPtr<MediaTrackDemuxer::SamplesHolder> aSamples)
{
  LOGV("%zu audio samples demuxed (sid:%d)",
       aSamples->mSamples.Length(),
       aSamples->mSamples[0]->mTrackInfo
       ? aSamples->mSamples[0]->mTrackInfo->GetID()
       : 0);
  DDLOG(DDLogCategory::Log,
        "audio_demuxed_samples",
        uint64_t(aSamples->mSamples.Length()));
  mAudio.mDemuxRequest.Complete();
  mAudio.mQueuedSamples.AppendElements(aSamples->mSamples);
  ScheduleUpdate(TrackInfo::kAudioTrack);
}

void
MediaFormatReader::NotifyNewOutput(
  TrackType aTrack, const MediaDataDecoder::DecodedData& aResults)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  if (aResults.IsEmpty()) {
    DDLOG(DDLogCategory::Log,
          aTrack == TrackInfo::kAudioTrack ? "decoded_audio" : "decoded_video",
          "no output samples");
  } else
    for (auto& sample : aResults) {
      if (DecoderDoctorLogger::IsDDLoggingEnabled()) {
        switch (sample->mType) {
          case MediaData::AUDIO_DATA:
            DDLOGPR(DDLogCategory::Log,
                    aTrack == TrackInfo::kAudioTrack ? "decoded_audio"
                                                     : "decoded_got_audio!?",
                    "{\"type\":\"AudioData\", \"offset\":%" PRIi64
                    ", \"time_us\":%" PRIi64 ", \"timecode_us\":%" PRIi64
                    ", \"duration_us\":%" PRIi64 ", \"frames\":%" PRIu32
                    ", \"kf\":%s, \"channels\":%" PRIu32 ", \"rate\":%" PRIu32
                    ", \"bytes\":%zu}",
                    sample->mOffset,
                    sample->mTime.ToMicroseconds(),
                    sample->mTimecode.ToMicroseconds(),
                    sample->mDuration.ToMicroseconds(),
                    sample->mFrames,
                    sample->mKeyframe ? "true" : "false",
                    sample->As<AudioData>()->mChannels,
                    sample->As<AudioData>()->mRate,
                    sample->As<AudioData>()->mAudioData.Size());
            break;
          case MediaData::VIDEO_DATA:
            DDLOGPR(DDLogCategory::Log,
                    aTrack == TrackInfo::kVideoTrack ? "decoded_video"
                                                     : "decoded_got_video!?",
                    "{\"type\":\"VideoData\", \"offset\":%" PRIi64
                    ", \"time_us\":%" PRIi64 ", \"timecode_us\":%" PRIi64
                    ", \"duration_us\":%" PRIi64 ", \"frames\":%" PRIu32
                    ", \"kf\":%s, \"size\":[%" PRIi32 ",%" PRIi32 "]}",
                    sample->mOffset,
                    sample->mTime.ToMicroseconds(),
                    sample->mTimecode.ToMicroseconds(),
                    sample->mDuration.ToMicroseconds(),
                    sample->mFrames,
                    sample->mKeyframe ? "true" : "false",
                    sample->As<VideoData>()->mDisplay.width,
                    sample->As<VideoData>()->mDisplay.height);
            break;
          case MediaData::RAW_DATA:
            DDLOGPR(DDLogCategory::Log,
                    aTrack == TrackInfo::kAudioTrack
                      ? "decoded_audio"
                      : aTrack == TrackInfo::kVideoTrack ? "decoded_video"
                                                         : "decoded_?",
                    "{\"type\":\"RawData\", \"offset\":%" PRIi64
                    " \"time_us\":%" PRIi64 ", \"timecode_us\":%" PRIi64
                    ", \"duration_us\":%" PRIi64 ", \"frames\":%" PRIu32
                    ", \"kf\":%s}",
                    sample->mOffset,
                    sample->mTime.ToMicroseconds(),
                    sample->mTimecode.ToMicroseconds(),
                    sample->mDuration.ToMicroseconds(),
                    sample->mFrames,
                    sample->mKeyframe ? "true" : "false");
            break;
          case MediaData::NULL_DATA:
            DDLOGPR(DDLogCategory::Log,
                    aTrack == TrackInfo::kAudioTrack
                      ? "decoded_audio"
                      : aTrack == TrackInfo::kVideoTrack ? "decoded_video"
                                                         : "decoded_?",
                    "{\"type\":\"NullData\", \"offset\":%" PRIi64
                    " \"time_us\":%" PRIi64 ", \"timecode_us\":%" PRIi64
                    ", \"duration_us\":%" PRIi64 ", \"frames\":%" PRIu32
                    ", \"kf\":%s}",
                    sample->mOffset,
                    sample->mTime.ToMicroseconds(),
                    sample->mTimecode.ToMicroseconds(),
                    sample->mDuration.ToMicroseconds(),
                    sample->mFrames,
                    sample->mKeyframe ? "true" : "false");
            break;
        }
      }
      LOGV("Received new %s sample time:%" PRId64 " duration:%" PRId64,
           TrackTypeToStr(aTrack),
           sample->mTime.ToMicroseconds(),
           sample->mDuration.ToMicroseconds());
      decoder.mOutput.AppendElement(sample);
      decoder.mNumSamplesOutput++;
      decoder.mNumOfConsecutiveError = 0;
    }
  LOG("Done processing new %s samples", TrackTypeToStr(aTrack));

  if (!aResults.IsEmpty()) {
    // We have decoded our first frame, we can now starts to skip future errors.
    decoder.mFirstFrameTime.reset();
  }
  ScheduleUpdate(aTrack);
}

void
MediaFormatReader::NotifyError(TrackType aTrack, const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());
  NS_WARNING(aError.Description().get());
  LOGV("%s Decoding error", TrackTypeToStr(aTrack));
  auto& decoder = GetDecoderData(aTrack);
  decoder.mError = decoder.HasFatalError() ? decoder.mError : Some(aError);

  // The GPU process had crashed and we receive a
  // NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER because we were doing HW decoding.
  // Now, save the related data and we will report the recovery time when a new
  // decoder is ready.
  if (aTrack == TrackType::kVideoTrack &&
      aError == NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER &&
      !aError.GPUCrashTimeStamp().IsNull()) {

    GPUProcessCrashTelemetryLogger::RecordGPUCrashData(mMediaDecoderOwnerID,
                                                       decoder.mDecoder.get(),
                                                       aError.GPUCrashTimeStamp(),
                                                       TimeStamp::Now());
  }

  ScheduleUpdate(aTrack);
}

void
MediaFormatReader::NotifyWaitingForData(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  decoder.mWaitingForData = true;
  if (decoder.mTimeThreshold) {
    decoder.mTimeThreshold.ref().mWaiting = true;
  }
  ScheduleUpdate(aTrack);
}

void
MediaFormatReader::NotifyWaitingForKey(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  mOnWaitingForKey.Notify();
  if (!decoder.mDecodeRequest.Exists()) {
    LOGV("WaitingForKey received while no pending decode. Ignoring");
    return;
  }
  decoder.mWaitingForKey = true;
  ScheduleUpdate(aTrack);
}

void
MediaFormatReader::NotifyEndOfStream(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  decoder.mDemuxEOS = true;
  ScheduleUpdate(aTrack);
}

bool
MediaFormatReader::NeedInput(DecoderData& aDecoder)
{
  // The decoder will not be fed a new raw sample until the current decoding
  // requests has completed.
  return
    (aDecoder.HasPromise() || aDecoder.mTimeThreshold.isSome()) &&
    !aDecoder.HasPendingDrain() &&
    !aDecoder.HasFatalError() &&
    !aDecoder.mDemuxRequest.Exists() &&
    !aDecoder.mOutput.Length() &&
    !aDecoder.HasInternalSeekPending() &&
    !aDecoder.mDecodeRequest.Exists();
}

void
MediaFormatReader::ScheduleUpdate(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  if (mShutdown) {
    return;
  }
  auto& decoder = GetDecoderData(aTrack);
  if (decoder.mUpdateScheduled) {
    return;
  }
  LOGV("SchedulingUpdate(%s)", TrackTypeToStr(aTrack));
  decoder.mUpdateScheduled = true;
  RefPtr<nsIRunnable> task(NewRunnableMethod<TrackType>(
    "MediaFormatReader::Update", this, &MediaFormatReader::Update, aTrack));
  nsresult rv = OwnerThread()->Dispatch(task.forget());
  MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));
  Unused << rv;
}

bool
MediaFormatReader::UpdateReceivedNewData(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);

  if (!decoder.mReceivedNewData) {
    return false;
  }

  // We do not want to clear mWaitingForData while there are pending
  // demuxing or seeking operations that could affect the value of this flag.
  // This is in order to ensure that we will retry once they complete as we may
  // now have new data that could potentially allow those operations to
  // successfully complete if tried again.
  if (decoder.mSeekRequest.Exists()) {
    // Nothing more to do until this operation complete.
    return true;
  }

  if (aTrack == TrackType::kVideoTrack && mSkipRequest.Exists()) {
    LOGV("Skipping in progress, nothing more to do");
    return true;
  }

  if (decoder.mDemuxRequest.Exists()) {
    // We may have pending operations to process, so we want to continue
    // after UpdateReceivedNewData returns.
    return false;
  }

  if (decoder.HasPendingDrain()) {
    // We do not want to clear mWaitingForData or mDemuxEOS while
    // a drain is in progress in order to properly complete the operation.
    return false;
  }

  decoder.mReceivedNewData = false;
  if (decoder.mTimeThreshold) {
    decoder.mTimeThreshold.ref().mWaiting = false;
  }
  decoder.mWaitingForData = false;

  if (decoder.HasFatalError()) {
    return false;
  }

  if (!mSeekPromise.IsEmpty() &&
      (!IsVideoSeeking() || aTrack == TrackInfo::kVideoTrack)) {
    MOZ_ASSERT(!decoder.HasPromise());
    MOZ_DIAGNOSTIC_ASSERT(
      (IsVideoSeeking() || !mAudio.mTimeThreshold) && !mVideo.mTimeThreshold,
      "InternalSeek must have been aborted when Seek was first called");
    MOZ_DIAGNOSTIC_ASSERT(
      (IsVideoSeeking() || !mAudio.HasWaitingPromise()) &&
      !mVideo.HasWaitingPromise(),
      "Waiting promises must have been rejected when Seek was first called");
    if (mVideo.mSeekRequest.Exists() ||
        (!IsVideoSeeking() && mAudio.mSeekRequest.Exists())) {
      // Already waiting for a seek to complete. Nothing more to do.
      return true;
    }
    LOG("Attempting Seek");
    ScheduleSeek();
    return true;
  }
  if (decoder.HasInternalSeekPending() || decoder.HasWaitingPromise()) {
    if (decoder.HasInternalSeekPending()) {
      LOG("Attempting Internal Seek");
      InternalSeek(aTrack, decoder.mTimeThreshold.ref());
    }
    if (decoder.HasWaitingPromise() && !decoder.IsWaitingForKey() &&
        !decoder.IsWaitingForData()) {
      MOZ_ASSERT(!decoder.HasPromise());
      LOG("We have new data. Resolving WaitingPromise");
      decoder.mWaitingPromise.Resolve(decoder.mType, __func__);
    }
    return true;
  }
  return false;
}

void
MediaFormatReader::RequestDemuxSamples(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  MOZ_ASSERT(!decoder.mDemuxRequest.Exists());

  if (!decoder.mQueuedSamples.IsEmpty()) {
    // No need to demux new samples.
    return;
  }

  if (decoder.mDemuxEOS) {
    // Nothing left to demux.
    // We do not want to attempt to demux while in waiting for data mode
    // as it would retrigger an unnecessary drain.
    return;
  }

  LOGV("Requesting extra demux %s", TrackTypeToStr(aTrack));
  if (aTrack == TrackInfo::kVideoTrack) {
    DoDemuxVideo();
  } else {
    DoDemuxAudio();
  }
}

void
MediaFormatReader::DecodeDemuxedSamples(TrackType aTrack,
                                        MediaRawData* aSample)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  RefPtr<MediaFormatReader> self = this;
  decoder.mFlushed = false;
  DDLOGPR(DDLogCategory::Log,
          aTrack == TrackInfo::kAudioTrack
            ? "decode_audio"
            : aTrack == TrackInfo::kVideoTrack ? "decode_video" : "decode_?",
          "{\"type\":\"MediaRawData\", \"offset\":%" PRIi64
          ", \"bytes\":%zu, \"time_us\":%" PRIi64 ", \"timecode_us\":%" PRIi64
          ", \"duration_us\":%" PRIi64 ", \"frames\":%" PRIu32 "%s%s}",
          aSample->mOffset,
          aSample->Size(),
          aSample->mTime.ToMicroseconds(),
          aSample->mTimecode.ToMicroseconds(),
          aSample->mDuration.ToMicroseconds(),
          aSample->mFrames,
          aSample->mKeyframe ? " kf" : "",
          aSample->mEOS ? " eos" : "");
  decoder.mDecoder->Decode(aSample)
    ->Then(mTaskQueue, __func__,
           [self, aTrack, &decoder]
           (const MediaDataDecoder::DecodedData& aResults) {
             decoder.mDecodeRequest.Complete();
             self->NotifyNewOutput(aTrack, aResults);

             // When we recovered from a GPU crash and get the first decoded
             // frame, report the recovery time telemetry.
             if (aTrack == TrackType::kVideoTrack) {
               GPUProcessCrashTelemetryLogger::ReportTelemetry(
                 self->mMediaDecoderOwnerID, decoder.mDecoder.get());
             }
           },
           [self, aTrack, &decoder](const MediaResult& aError) {
             decoder.mDecodeRequest.Complete();
             self->NotifyError(aTrack, aError);
           })
    ->Track(decoder.mDecodeRequest);
}

void
MediaFormatReader::HandleDemuxedSamples(
  TrackType aTrack, FrameStatistics::AutoNotifyDecoded& aA)
{
  MOZ_ASSERT(OnTaskQueue());

  auto& decoder = GetDecoderData(aTrack);

  if (decoder.mFlushing) {
    LOGV("Decoder operation in progress, let it complete.");
    return;
  }

  if (decoder.mQueuedSamples.IsEmpty()) {
    return;
  }

  RefPtr<MediaRawData> sample = decoder.mQueuedSamples[0];
  const RefPtr<TrackInfoSharedPtr> info = sample->mTrackInfo;

  if (info && decoder.mLastStreamSourceID != info->GetID()) {
    nsTArray<RefPtr<MediaRawData>> samples;
    if (decoder.mDecoder) {
      bool recyclable = StaticPrefs::MediaDecoderRecycleEnabled() &&
                        decoder.mDecoder->SupportDecoderRecycling();
      if (!recyclable && decoder.mTimeThreshold.isNothing() &&
          (decoder.mNextStreamSourceID.isNothing() ||
            decoder.mNextStreamSourceID.ref() != info->GetID())) {
        LOG("%s stream id has changed from:%d to:%d, draining decoder.",
            TrackTypeToStr(aTrack),
            decoder.mLastStreamSourceID,
            info->GetID());
        decoder.RequestDrain();
        decoder.mNextStreamSourceID = Some(info->GetID());
        ScheduleUpdate(aTrack);
        return;
      }

      // If flushing is required, it will clear our array of queued samples.
      // So we may need to make a copy.
      samples = decoder.mQueuedSamples;
      if (!recyclable) {
        LOG("Decoder does not support recycling, recreate decoder.");
        ShutdownDecoder(aTrack);
      } else if (decoder.HasWaitingPromise()) {
        decoder.Flush();
      }
    }

    LOG("%s stream id has changed from:%d to:%d.",
        TrackTypeToStr(aTrack),
        decoder.mLastStreamSourceID,
        info->GetID());

    decoder.mNextStreamSourceID.reset();
    decoder.mLastStreamSourceID = info->GetID();
    decoder.mInfo = info;

    decoder.mMeanRate.Reset();

    if (sample->mKeyframe) {
      if (samples.Length()) {
        decoder.mQueuedSamples = Move(samples);
      }
    } else {
      auto time = TimeInterval(sample->mTime, sample->GetEndTime());
      InternalSeekTarget seekTarget =
        decoder.mTimeThreshold.refOr(InternalSeekTarget(time, false));
      LOG("Stream change occurred on a non-keyframe. Seeking to:%" PRId64,
          sample->mTime.ToMicroseconds());
      InternalSeek(aTrack, seekTarget);
      return;
    }
  }

  // Calculate the average frame rate. The first frame will be accounted
  // for twice.
  decoder.mMeanRate.Update(sample->mDuration);

  if (!decoder.mDecoder) {
    mDecoderFactory->CreateDecoder(aTrack);
    return;
  }

  LOGV("Input:%" PRId64 " (dts:%" PRId64 " kf:%d)",
        sample->mTime.ToMicroseconds(), sample->mTimecode.ToMicroseconds(),
        sample->mKeyframe);
  decoder.mNumSamplesInput++;
  decoder.mSizeOfQueue++;
  if (aTrack == TrackInfo::kVideoTrack) {
    aA.mStats.mParsedFrames++;
  }

  DecodeDemuxedSamples(aTrack, sample);

  decoder.mQueuedSamples.RemoveElementAt(0);
}

void
MediaFormatReader::InternalSeek(TrackType aTrack,
                                const InternalSeekTarget& aTarget)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("%s internal seek to %f",
      TrackTypeToStr(aTrack), aTarget.Time().ToSeconds());

  auto& decoder = GetDecoderData(aTrack);
  decoder.Flush();
  decoder.ResetDemuxer();
  decoder.mTimeThreshold = Some(aTarget);
  DDLOG(DDLogCategory::Log, "seeking", DDNoValue{});
  RefPtr<MediaFormatReader> self = this;
  decoder.mTrackDemuxer->Seek(decoder.mTimeThreshold.ref().Time())
    ->Then(
      OwnerThread(),
      __func__,
      [self, aTrack](TimeUnit aTime) {
        DDLOGEX(self.get(), DDLogCategory::Log, "seeked", DDNoValue{});
        auto& decoder = self->GetDecoderData(aTrack);
        decoder.mSeekRequest.Complete();
        MOZ_ASSERT(
          decoder.mTimeThreshold,
          "Seek promise must be disconnected when timethreshold is reset");
        decoder.mTimeThreshold.ref().mHasSeeked = true;
        self->SetVideoDecodeThreshold();
        self->ScheduleUpdate(aTrack);
      },
      [self, aTrack](const MediaResult& aError) {
        auto& decoder = self->GetDecoderData(aTrack);
        decoder.mSeekRequest.Complete();
        switch (aError.Code()) {
          case NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA:
            DDLOGEX(
              self.get(), DDLogCategory::Log, "seeking_interrupted", aError);
            self->NotifyWaitingForData(aTrack);
            break;
          case NS_ERROR_DOM_MEDIA_END_OF_STREAM:
            DDLOGEX(
              self.get(), DDLogCategory::Log, "seeking_interrupted", aError);
            decoder.mTimeThreshold.reset();
            self->NotifyEndOfStream(aTrack);
            break;
          case NS_ERROR_DOM_MEDIA_CANCELED:
            DDLOGEX(
              self.get(), DDLogCategory::Log, "seeking_interrupted", aError);
            decoder.mTimeThreshold.reset();
            break;
          default:
            DDLOGEX(self.get(), DDLogCategory::Log, "seeking_error", aError);
            decoder.mTimeThreshold.reset();
            self->NotifyError(aTrack, aError);
            break;
        }
      })
    ->Track(decoder.mSeekRequest);
}

void
MediaFormatReader::DrainDecoder(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());

  auto& decoder = GetDecoderData(aTrack);
  if (decoder.mDrainState == DrainState::Draining) {
    return;
  }
  if (!decoder.mDecoder ||
      (decoder.mDrainState != DrainState::PartialDrainPending &&
       decoder.mNumSamplesInput == decoder.mNumSamplesOutput)) {
    // No frames to drain.
    LOGV("Draining %s with nothing to drain", TrackTypeToStr(aTrack));
    decoder.mDrainState = DrainState::DrainAborted;
    ScheduleUpdate(aTrack);
    return;
  }

  decoder.mDrainState = DrainState::Draining;

  DDLOG(DDLogCategory::Log, "draining", DDNoValue{});
  RefPtr<MediaFormatReader> self = this;
  decoder.mDecoder->Drain()
    ->Then(mTaskQueue, __func__,
           [self, aTrack, &decoder]
           (const MediaDataDecoder::DecodedData& aResults) {
             decoder.mDrainRequest.Complete();
             DDLOGEX(self.get(), DDLogCategory::Log, "drained", DDNoValue{});
             if (aResults.IsEmpty()) {
               decoder.mDrainState = DrainState::DrainCompleted;
             } else {
               self->NotifyNewOutput(aTrack, aResults);
               // Let's see if we have any more data available to drain.
               decoder.mDrainState = DrainState::PartialDrainPending;
             }
             self->ScheduleUpdate(aTrack);
           },
           [self, aTrack, &decoder](const MediaResult& aError) {
             decoder.mDrainRequest.Complete();
             DDLOGEX(self.get(), DDLogCategory::Log, "draining_error", aError);
             self->NotifyError(aTrack, aError);
           })
    ->Track(decoder.mDrainRequest);
  LOG("Requesting %s decoder to drain", TrackTypeToStr(aTrack));
}

void
MediaFormatReader::Update(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());

  if (mShutdown) {
    return;
  }

  LOGV("Processing update for %s", TrackTypeToStr(aTrack));

  bool needOutput = false;
  auto& decoder = GetDecoderData(aTrack);
  decoder.mUpdateScheduled = false;

  if (!mInitDone) {
    return;
  }

  if (aTrack == TrackType::kVideoTrack && mSkipRequest.Exists()) {
    LOGV("Skipping in progress, nothing more to do");
    return;
  }

  if (UpdateReceivedNewData(aTrack)) {
    LOGV("Nothing more to do");
    return;
  }

  if (decoder.mSeekRequest.Exists()) {
    LOGV("Seeking hasn't completed, nothing more to do");
    return;
  }

  MOZ_DIAGNOSTIC_ASSERT(
    !decoder.HasInternalSeekPending() ||
      (!decoder.mOutput.Length() && !decoder.mQueuedSamples.Length()),
    "No frames can be demuxed or decoded while an internal seek is pending");

  // Record number of frames decoded and parsed. Automatically update the
  // stats counters using the AutoNotifyDecoded stack-based class.
  FrameStatistics::AutoNotifyDecoded a(mFrameStats);

  // Drop any frames found prior our internal seek target.
  while (decoder.mTimeThreshold && decoder.mOutput.Length()) {
    RefPtr<MediaData>& output = decoder.mOutput[0];
    InternalSeekTarget target = decoder.mTimeThreshold.ref();
    auto time = output->mTime;
    if (time >= target.Time()) {
      // We have reached our internal seek target.
      decoder.mTimeThreshold.reset();
      // We might have dropped some keyframes.
      mPreviousDecodedKeyframeTime_us = sNoPreviousDecodedKeyframe;
    }
    if (time < target.Time() || (target.mDropTarget && target.Contains(time))) {
      LOGV("Internal Seeking: Dropping %s frame time:%f wanted:%f (kf:%d)",
           TrackTypeToStr(aTrack),
           output->mTime.ToSeconds(),
           target.Time().ToSeconds(),
           output->mKeyframe);
      decoder.mOutput.RemoveElementAt(0);
      decoder.mSizeOfQueue -= 1;
    }
  }

  while (decoder.mOutput.Length() &&
         decoder.mOutput[0]->mType == MediaData::NULL_DATA) {
    LOGV("Dropping null data. Time: %" PRId64,
         decoder.mOutput[0]->mTime.ToMicroseconds());
    decoder.mOutput.RemoveElementAt(0);
    decoder.mSizeOfQueue -= 1;
  }

  if (decoder.HasPromise()) {
    needOutput = true;
    if (decoder.mOutput.Length()) {
      RefPtr<MediaData> output = decoder.mOutput[0];
      decoder.mOutput.RemoveElementAt(0);
      decoder.mSizeOfQueue -= 1;
      decoder.mLastDecodedSampleTime =
        Some(TimeInterval(output->mTime, output->GetEndTime()));
      decoder.mNumSamplesOutputTotal++;
      ReturnOutput(output, aTrack);
      // We have a decoded sample ready to be returned.
      if (aTrack == TrackType::kVideoTrack) {
        uint64_t delta =
          decoder.mNumSamplesOutputTotal - mLastReportedNumDecodedFrames;
        a.mStats.mDecodedFrames = static_cast<uint32_t>(delta);
        mLastReportedNumDecodedFrames = decoder.mNumSamplesOutputTotal;
        if (output->mKeyframe) {
          if (mPreviousDecodedKeyframeTime_us < output->mTime.ToMicroseconds()) {
            // There is a previous keyframe -> Record inter-keyframe stats.
            uint64_t segment_us =
              output->mTime.ToMicroseconds() - mPreviousDecodedKeyframeTime_us;
            a.mStats.mInterKeyframeSum_us += segment_us;
            a.mStats.mInterKeyframeCount += 1;
            if (a.mStats.mInterKeyFrameMax_us < segment_us) {
              a.mStats.mInterKeyFrameMax_us = segment_us;
            }
          }
          mPreviousDecodedKeyframeTime_us = output->mTime.ToMicroseconds();
        }
        nsCString error;
        mVideo.mIsHardwareAccelerated =
          mVideo.mDecoder && mVideo.mDecoder->IsHardwareAccelerated(error);
#ifdef XP_WIN
        // D3D11_YCBCR_IMAGE images are GPU based, we try to limit the amount
        // of GPU RAM used.
        VideoData* videoData = static_cast<VideoData*>(output.get());
        mVideo.mIsHardwareAccelerated =
          mVideo.mIsHardwareAccelerated ||
          (videoData->mImage &&
           videoData->mImage->GetFormat() == ImageFormat::D3D11_YCBCR_IMAGE);
#endif
      }
    } else if (decoder.HasFatalError()) {
      LOG("Rejecting %s promise: DECODE_ERROR", TrackTypeToStr(aTrack));
      decoder.RejectPromise(decoder.mError.ref(), __func__);
      return;
    } else if (decoder.HasCompletedDrain()) {
      if (decoder.mDemuxEOS) {
        LOG("Rejecting %s promise: EOS", TrackTypeToStr(aTrack));
        decoder.RejectPromise(NS_ERROR_DOM_MEDIA_END_OF_STREAM, __func__);
      } else if (decoder.mWaitingForData) {
        if (decoder.mDrainState == DrainState::DrainCompleted &&
            decoder.mLastDecodedSampleTime &&
            !decoder.mNextStreamSourceID) {
          // We have completed draining the decoder following WaitingForData.
          // Set up the internal seek machinery to be able to resume from the
          // last sample decoded.
          LOG("Seeking to last sample time: %" PRId64,
              decoder.mLastDecodedSampleTime.ref().mStart.ToMicroseconds());
          InternalSeek(aTrack,
                       InternalSeekTarget(decoder.mLastDecodedSampleTime.ref(), true));
        }
        if (!decoder.mReceivedNewData) {
          LOG("Rejecting %s promise: WAITING_FOR_DATA", TrackTypeToStr(aTrack));
          decoder.RejectPromise(NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA, __func__);
        }
      }

      decoder.mDrainState = DrainState::None;

      // Now that draining has completed, we check if we have received
      // new data again as the result may now be different from the earlier
      // run.
      if (UpdateReceivedNewData(aTrack) || decoder.mSeekRequest.Exists()) {
        LOGV("Nothing more to do");
        return;
      }
    } else if (decoder.mDemuxEOS &&
               !decoder.HasPendingDrain() &&
               decoder.mQueuedSamples.IsEmpty()) {
      // It is possible to transition from WAITING_FOR_DATA directly to EOS
      // state during the internal seek; in which case no draining would occur.
      // There is no more samples left to be decoded and we are already in
      // EOS state. We can immediately reject the data promise.
      LOG("Rejecting %s promise: EOS", TrackTypeToStr(aTrack));
      decoder.RejectPromise(NS_ERROR_DOM_MEDIA_END_OF_STREAM, __func__);
    } else if (decoder.mWaitingForKey) {
      LOG("Rejecting %s promise: WAITING_FOR_DATA due to waiting for key",
          TrackTypeToStr(aTrack));
      decoder.RejectPromise(NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA, __func__);
    } else if (IsDecoderWaitingForCDM(aTrack)) {
      // Rejecting the promise could lead to entering buffering state for MDSM,
      // once a qualified(with the same key system and sessions created by the
      // same InitData) new cdm proxy is set, decoding can be resumed.
      LOG("Rejecting %s promise: WAITING_FOR_DATA due to waiting for CDM",
          TrackTypeToStr(aTrack));
      decoder.RejectPromise(NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA, __func__);
    }
  }

  if (decoder.mDrainState == DrainState::DrainRequested ||
      decoder.mDrainState == DrainState::PartialDrainPending) {
    if (decoder.mOutput.IsEmpty()) {
      DrainDecoder(aTrack);
    }
    return;
  }

  if (decoder.mError && !decoder.HasFatalError()) {
    MOZ_RELEASE_ASSERT(!decoder.HasInternalSeekPending(),
                       "No error can occur while an internal seek is pending");
    bool needsNewDecoder =
      decoder.mError.ref() == NS_ERROR_DOM_MEDIA_NEED_NEW_DECODER;
    if (!needsNewDecoder &&
        ++decoder.mNumOfConsecutiveError > decoder.mMaxConsecutiveError) {
      DDLOG(DDLogCategory::Log, "too_many_decode_errors", decoder.mError.ref());
      NotifyError(aTrack, decoder.mError.ref());
      return;
    }
    decoder.mError.reset();

    LOG("%s decoded error count %d", TrackTypeToStr(aTrack),
        decoder.mNumOfConsecutiveError);

    if (needsNewDecoder) {
      LOG("Error: Need new decoder");
      ShutdownDecoder(aTrack);
    }
    if (decoder.mFirstFrameTime) {
      TimeInterval seekInterval = TimeInterval(decoder.mFirstFrameTime.ref(),
                                                decoder.mFirstFrameTime.ref());
      InternalSeek(aTrack, InternalSeekTarget(seekInterval, false));
      return;
    }

    TimeUnit nextKeyframe;
    if (aTrack == TrackType::kVideoTrack &&
        NS_SUCCEEDED(
          decoder.mTrackDemuxer->GetNextRandomAccessPoint(&nextKeyframe)) &&
        !nextKeyframe.IsInfinite()) {
      SkipVideoDemuxToNextKeyFrame(
        decoder.mLastDecodedSampleTime.refOr(TimeInterval()).Length());
    } else if (aTrack == TrackType::kAudioTrack) {
      decoder.Flush();
    } else {
      DDLOG(DDLogCategory::Log, "no_keyframe", NS_ERROR_DOM_MEDIA_FATAL_ERR);
      // We can't recover from this error.
      NotifyError(aTrack, NS_ERROR_DOM_MEDIA_FATAL_ERR);
    }
    return;
  }

  bool needInput = NeedInput(decoder);

  LOGV("Update(%s) ni=%d no=%d in:%" PRIu64 " out:%" PRIu64
       " qs=%u decoding:%d flushing:%d desc:%s pending:%u waiting:%d eos:%d "
       "ds:%d sid:%u waitcdm:%d",
       TrackTypeToStr(aTrack),
       needInput,
       needOutput,
       decoder.mNumSamplesInput,
       decoder.mNumSamplesOutput,
       uint32_t(size_t(decoder.mSizeOfQueue)),
       decoder.mDecodeRequest.Exists(),
       decoder.mFlushing,
       decoder.mDescription.get(),
       uint32_t(decoder.mOutput.Length()),
       decoder.mWaitingForData,
       decoder.mDemuxEOS,
       int32_t(decoder.mDrainState),
       decoder.mLastStreamSourceID,
       IsDecoderWaitingForCDM(aTrack));

  if (IsWaitingOnCDMResource() || !ResolveSetCDMPromiseIfDone(aTrack)) {
    // If the content is encrypted, MFR won't start to create decoder until
    // CDMProxy is set.
    return;
  }

  if ((decoder.IsWaitingForData() &&
       (!decoder.mTimeThreshold || decoder.mTimeThreshold.ref().mWaiting)) ||
      (decoder.IsWaitingForKey())) {
    // Nothing more we can do at present.
    LOGV("Still waiting for data or key. data(%d)/key(%d)",
         decoder.mWaitingForData,
         decoder.mWaitingForKey);
    return;
  }

  if (decoder.CancelWaitingForKey()) {
    LOGV("No longer waiting for key. Resolving waiting promise");
    return;
  }

  if (!needInput) {
    LOGV("No need for additional input (pending:%u)",
         uint32_t(decoder.mOutput.Length()));
    return;
  }

  // Demux samples if we don't have some.
  RequestDemuxSamples(aTrack);

  HandleDemuxedSamples(aTrack, a);
}

void
MediaFormatReader::ReturnOutput(MediaData* aData, TrackType aTrack)
{
  MOZ_ASSERT(GetDecoderData(aTrack).HasPromise());
  MOZ_DIAGNOSTIC_ASSERT(aData->mType != MediaData::NULL_DATA);
  LOG("Resolved data promise for %s [%" PRId64 ", %" PRId64 "]",
      TrackTypeToStr(aTrack),
      aData->mTime.ToMicroseconds(),
      aData->GetEndTime().ToMicroseconds());

  if (aTrack == TrackInfo::kAudioTrack) {
    AudioData* audioData = static_cast<AudioData*>(aData);

    if (audioData->mChannels != mInfo.mAudio.mChannels ||
        audioData->mRate != mInfo.mAudio.mRate) {
      LOG("change of audio format (rate:%d->%d). "
          "This is an unsupported configuration",
          mInfo.mAudio.mRate,
          audioData->mRate);
      mInfo.mAudio.mRate = audioData->mRate;
      mInfo.mAudio.mChannels = audioData->mChannels;
    }
    mAudio.ResolvePromise(audioData, __func__);
  } else if (aTrack == TrackInfo::kVideoTrack) {
    VideoData* videoData = static_cast<VideoData*>(aData);

    if (videoData->mDisplay != mInfo.mVideo.mDisplay) {
      LOG("change of video display size (%dx%d->%dx%d)",
          mInfo.mVideo.mDisplay.width, mInfo.mVideo.mDisplay.height,
          videoData->mDisplay.width, videoData->mDisplay.height);
      mInfo.mVideo.mDisplay = videoData->mDisplay;
    }

    TimeUnit nextKeyframe;
    if (!mVideo.HasInternalSeekPending() &&
        NS_SUCCEEDED(
          mVideo.mTrackDemuxer->GetNextRandomAccessPoint(&nextKeyframe))) {
      videoData->SetNextKeyFrameTime(nextKeyframe);
    }

    mVideo.ResolvePromise(videoData, __func__);
  }
}

size_t
MediaFormatReader::SizeOfVideoQueueInFrames()
{
  return SizeOfQueue(TrackInfo::kVideoTrack);
}

size_t
MediaFormatReader::SizeOfAudioQueueInFrames()
{
  return SizeOfQueue(TrackInfo::kAudioTrack);
}

size_t
MediaFormatReader::SizeOfQueue(TrackType aTrack)
{
  auto& decoder = GetDecoderData(aTrack);
  return decoder.mSizeOfQueue;
}

RefPtr<MediaFormatReader::WaitForDataPromise>
MediaFormatReader::WaitForData(MediaData::Type aType)
{
  MOZ_ASSERT(OnTaskQueue());
  TrackType trackType = aType == MediaData::VIDEO_DATA ?
    TrackType::kVideoTrack : TrackType::kAudioTrack;
  auto& decoder = GetDecoderData(trackType);
  if (!decoder.IsWaitingForData() && !decoder.IsWaitingForKey()) {
    // We aren't waiting for anything.
    return WaitForDataPromise::CreateAndResolve(decoder.mType, __func__);
  }
  RefPtr<WaitForDataPromise> p = decoder.mWaitingPromise.Ensure(__func__);
  ScheduleUpdate(trackType);
  return p;
}

nsresult
MediaFormatReader::ResetDecode(TrackSet aTracks)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("");

  mSeekPromise.RejectIfExists(NS_OK, __func__);
  mSkipRequest.DisconnectIfExists();

  // Do the same for any data wait promises.
  if (aTracks.contains(TrackInfo::kAudioTrack)) {
    mAudio.mWaitingPromise.RejectIfExists(
        WaitForDataRejectValue(MediaData::AUDIO_DATA,
                               WaitForDataRejectValue::CANCELED), __func__);
  }

  if (aTracks.contains(TrackInfo::kVideoTrack)) {
    mVideo.mWaitingPromise.RejectIfExists(
        WaitForDataRejectValue(MediaData::VIDEO_DATA,
                               WaitForDataRejectValue::CANCELED), __func__);
  }

  // Reset miscellaneous seeking state.
  mPendingSeekTime.reset();

  if (HasVideo() && aTracks.contains(TrackInfo::kVideoTrack)) {
    mVideo.ResetDemuxer();
    mVideo.mFirstFrameTime = Some(media::TimeUnit::Zero());
    Reset(TrackInfo::kVideoTrack);
    if (mVideo.HasPromise()) {
      mVideo.RejectPromise(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
    }
  }

  if (HasAudio() && aTracks.contains(TrackInfo::kAudioTrack)) {
    mAudio.ResetDemuxer();
    mVideo.mFirstFrameTime = Some(media::TimeUnit::Zero());
    Reset(TrackInfo::kAudioTrack);
    if (mAudio.HasPromise()) {
      mAudio.RejectPromise(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
    }
  }

  return NS_OK;
}

void
MediaFormatReader::Reset(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("Reset(%s) BEGIN", TrackTypeToStr(aTrack));

  auto& decoder = GetDecoderData(aTrack);

  decoder.ResetState();
  decoder.Flush();

  LOG("Reset(%s) END", TrackTypeToStr(aTrack));
}

void
MediaFormatReader::DropDecodedSamples(TrackType aTrack)
{
  MOZ_ASSERT(OnTaskQueue());
  auto& decoder = GetDecoderData(aTrack);
  size_t lengthDecodedQueue = decoder.mOutput.Length();
  if (lengthDecodedQueue && decoder.mTimeThreshold.isSome()) {
    auto time = decoder.mOutput.LastElement()->mTime;
    if (time >= decoder.mTimeThreshold.ref().Time()) {
      // We would have reached our internal seek target.
      decoder.mTimeThreshold.reset();
    }
  }
  decoder.mOutput.Clear();
  decoder.mSizeOfQueue -= lengthDecodedQueue;
  if (aTrack == TrackInfo::kVideoTrack && mFrameStats) {
    mFrameStats->NotifyDecodedFrames({ 0, 0, lengthDecodedQueue });
  }
}

void
MediaFormatReader::SkipVideoDemuxToNextKeyFrame(TimeUnit aTimeThreshold)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("Skipping up to %" PRId64, aTimeThreshold.ToMicroseconds());

  // We've reached SkipVideoDemuxToNextKeyFrame when our decoding is late.
  // As such we can drop all already decoded samples and discard all pending
  // samples.
  DropDecodedSamples(TrackInfo::kVideoTrack);

  mVideo.mTrackDemuxer->SkipToNextRandomAccessPoint(aTimeThreshold)
    ->Then(OwnerThread(), __func__, this,
           &MediaFormatReader::OnVideoSkipCompleted,
           &MediaFormatReader::OnVideoSkipFailed)
    ->Track(mSkipRequest);
}

void
MediaFormatReader::VideoSkipReset(uint32_t aSkipped)
{
  MOZ_ASSERT(OnTaskQueue());

  // Some frames may have been output by the decoder since we initiated the
  // videoskip process and we know they would be late.
  DropDecodedSamples(TrackInfo::kVideoTrack);
  // Report the pending frames as dropped.
  if (mFrameStats) {
    mFrameStats->NotifyDecodedFrames({ 0, 0, SizeOfVideoQueueInFrames() });
  }

  // Cancel any pending demux request and pending demuxed samples.
  mVideo.mDemuxRequest.DisconnectIfExists();
  Reset(TrackType::kVideoTrack);

  if (mFrameStats) {
    mFrameStats->NotifyDecodedFrames({ aSkipped, 0, aSkipped });
  }

  mVideo.mNumSamplesSkippedTotal += aSkipped;
}

void
MediaFormatReader::OnVideoSkipCompleted(uint32_t aSkipped)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("Skipping succeeded, skipped %u frames", aSkipped);
  mSkipRequest.Complete();

  DDLOG(DDLogCategory::Log, "video_skipped", DDNoValue());

  VideoSkipReset(aSkipped);

  ScheduleUpdate(TrackInfo::kVideoTrack);
}

void
MediaFormatReader::OnVideoSkipFailed(
  MediaTrackDemuxer::SkipFailureHolder aFailure)
{
  MOZ_ASSERT(OnTaskQueue());
  LOG("Skipping failed, skipped %u frames", aFailure.mSkipped);
  mSkipRequest.Complete();

  switch (aFailure.mFailure.Code()) {
    case NS_ERROR_DOM_MEDIA_END_OF_STREAM:
    case NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA:
      DDLOG(
        DDLogCategory::Log, "video_skipping_interruption", aFailure.mFailure);
      // Some frames may have been output by the decoder since we initiated the
      // videoskip process and we know they would be late.
      DropDecodedSamples(TrackInfo::kVideoTrack);
      // We can't complete the skip operation, will just service a video frame
      // normally.
      ScheduleUpdate(TrackInfo::kVideoTrack);
      break;
    case NS_ERROR_DOM_MEDIA_CANCELED:
      DDLOG(
        DDLogCategory::Log, "video_skipping_interruption", aFailure.mFailure);
      if (mVideo.HasPromise()) {
        mVideo.RejectPromise(aFailure.mFailure, __func__);
      }
      break;
    default:
      DDLOG(DDLogCategory::Log, "video_skipping_error", aFailure.mFailure);
      NotifyError(TrackType::kVideoTrack, aFailure.mFailure);
      break;
  }
}

RefPtr<MediaFormatReader::SeekPromise>
MediaFormatReader::Seek(const SeekTarget& aTarget)
{
  MOZ_ASSERT(OnTaskQueue());

  LOG("aTarget=(%" PRId64 ")", aTarget.GetTime().ToMicroseconds());

  MOZ_DIAGNOSTIC_ASSERT(mSeekPromise.IsEmpty());
  MOZ_DIAGNOSTIC_ASSERT(!mVideo.HasPromise());
  MOZ_DIAGNOSTIC_ASSERT(aTarget.IsVideoOnly() || !mAudio.HasPromise());
  MOZ_DIAGNOSTIC_ASSERT(mPendingSeekTime.isNothing());
  MOZ_DIAGNOSTIC_ASSERT(mVideo.mTimeThreshold.isNothing());
  MOZ_DIAGNOSTIC_ASSERT(aTarget.IsVideoOnly() ||
                        mAudio.mTimeThreshold.isNothing());

  if (!mInfo.mMediaSeekable && !mInfo.mMediaSeekableOnlyInBufferedRanges) {
    LOG("Seek() END (Unseekable)");
    return SeekPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  if (mShutdown) {
    return SeekPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  SetSeekTarget(aTarget);

  RefPtr<SeekPromise> p = mSeekPromise.Ensure(__func__);

  ScheduleSeek();

  return p;
}

void
MediaFormatReader::SetSeekTarget(const SeekTarget& aTarget)
{
  MOZ_ASSERT(OnTaskQueue());

  mOriginalSeekTarget = aTarget;
  mFallbackSeekTime = mPendingSeekTime = Some(aTarget.GetTime());
}

void
MediaFormatReader::ScheduleSeek()
{
  if (mSeekScheduled) {
    return;
  }
  mSeekScheduled = true;
  nsresult rv =
    OwnerThread()->Dispatch(NewRunnableMethod(
      "MediaFormatReader::AttemptSeek", this, &MediaFormatReader::AttemptSeek));
  MOZ_DIAGNOSTIC_ASSERT(NS_SUCCEEDED(rv));
  Unused << rv;
}

void
MediaFormatReader::AttemptSeek()
{
  MOZ_ASSERT(OnTaskQueue());

  mSeekScheduled = false;

  if (mPendingSeekTime.isNothing()) {
    return;
  }

  if (HasVideo()) {
    mVideo.ResetDemuxer();
    mVideo.ResetState();
  }

  // Don't reset the audio demuxer not state when seeking video only
  // as it will cause the audio to seek back to the beginning
  // resulting in out-of-sync audio from video.
  if (HasAudio() && !mOriginalSeekTarget.IsVideoOnly()) {
    mAudio.ResetDemuxer();
    mAudio.ResetState();
  }

  if (HasVideo()) {
    DoVideoSeek();
  } else if (HasAudio()) {
    DoAudioSeek();
  } else {
    MOZ_CRASH();
  }
}

void
MediaFormatReader::OnSeekFailed(TrackType aTrack, const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("%s failure:%s", TrackTypeToStr(aTrack), aError.ErrorName().get());
  if (aTrack == TrackType::kVideoTrack) {
    mVideo.mSeekRequest.Complete();
  } else {
    mAudio.mSeekRequest.Complete();
  }

  if (aError == NS_ERROR_DOM_MEDIA_WAITING_FOR_DATA) {
    if (HasVideo() &&
        aTrack == TrackType::kAudioTrack &&
        mFallbackSeekTime.isSome() &&
        mPendingSeekTime.ref() != mFallbackSeekTime.ref()) {
      // We have failed to seek audio where video seeked to earlier.
      // Attempt to seek instead to the closest point that we know we have in
      // order to limit A/V sync discrepency.

      // Ensure we have the most up to date buffered ranges.
      UpdateReceivedNewData(TrackType::kAudioTrack);
      Maybe<TimeUnit> nextSeekTime;
      // Find closest buffered time found after video seeked time.
      for (const auto& timeRange : mAudio.mTimeRanges) {
        if (timeRange.mStart >= mPendingSeekTime.ref()) {
          nextSeekTime.emplace(timeRange.mStart);
          break;
        }
      }
      if (nextSeekTime.isNothing() ||
          nextSeekTime.ref() > mFallbackSeekTime.ref()) {
        nextSeekTime = Some(mFallbackSeekTime.ref());
        LOG("Unable to seek audio to video seek time. A/V sync may be broken");
      } else {
        mFallbackSeekTime.reset();
      }
      mPendingSeekTime = nextSeekTime;
      DoAudioSeek();
      return;
    }
    NotifyWaitingForData(aTrack);
  }
  MOZ_ASSERT(!mVideo.mSeekRequest.Exists() && !mAudio.mSeekRequest.Exists());
  mPendingSeekTime.reset();

  auto type = aTrack == TrackType::kAudioTrack ? MediaData::AUDIO_DATA
                                               : MediaData::VIDEO_DATA;
  mSeekPromise.Reject(SeekRejectValue(type, aError), __func__);
}

void
MediaFormatReader::DoVideoSeek()
{
  MOZ_ASSERT(mPendingSeekTime.isSome());
  LOGV("Seeking video to %" PRId64, mPendingSeekTime.ref().ToMicroseconds());
  auto seekTime = mPendingSeekTime.ref();
  mVideo.mTrackDemuxer->Seek(seekTime)
    ->Then(OwnerThread(), __func__, this,
           &MediaFormatReader::OnVideoSeekCompleted,
           &MediaFormatReader::OnVideoSeekFailed)
    ->Track(mVideo.mSeekRequest);
}

void
MediaFormatReader::OnVideoSeekCompleted(TimeUnit aTime)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("Video seeked to %" PRId64, aTime.ToMicroseconds());
  mVideo.mSeekRequest.Complete();

  mVideo.mFirstFrameTime = Some(aTime);
  mPreviousDecodedKeyframeTime_us = sNoPreviousDecodedKeyframe;

  SetVideoDecodeThreshold();

  if (HasAudio() && !mOriginalSeekTarget.IsVideoOnly()) {
    MOZ_ASSERT(mPendingSeekTime.isSome());
    if (mOriginalSeekTarget.IsFast()) {
      // We are performing a fast seek. We need to seek audio to where the
      // video seeked to, to ensure proper A/V sync once playback resume.
      mPendingSeekTime = Some(aTime);
    }
    DoAudioSeek();
  } else {
    mPendingSeekTime.reset();
    mSeekPromise.Resolve(aTime, __func__);
  }
}

void
MediaFormatReader::OnVideoSeekFailed(const MediaResult& aError)
{
  mPreviousDecodedKeyframeTime_us = sNoPreviousDecodedKeyframe;
  OnSeekFailed(TrackType::kVideoTrack, aError);
}

void
MediaFormatReader::SetVideoDecodeThreshold()
{
  MOZ_ASSERT(OnTaskQueue());

  if (!HasVideo() || !mVideo.mDecoder) {
    return;
  }

  if (!mVideo.mTimeThreshold && !IsSeeking()) {
    return;
  }

  TimeUnit threshold;
  if (mVideo.mTimeThreshold) {
    // For internalSeek.
    threshold = mVideo.mTimeThreshold.ref().Time();
  } else if (IsSeeking()) {
    // If IsSeeking() is true, then video seek must have completed already.
    TimeUnit keyframe;
    if (NS_FAILED(mVideo.mTrackDemuxer->GetNextRandomAccessPoint(&keyframe))) {
      return;
    }

    // If the key frame is invalid/infinite, it means the target position is
    // closing to end of stream. We don't want to skip any frame at this point.
    if (!keyframe.IsValid() || keyframe.IsInfinite()) {
      return;
    }
    threshold = mOriginalSeekTarget.GetTime();
  } else {
    return;
  }

  LOG("Set seek threshold to %" PRId64, threshold.ToMicroseconds());
  mVideo.mDecoder->SetSeekThreshold(threshold);
}

void
MediaFormatReader::DoAudioSeek()
{
  MOZ_ASSERT(mPendingSeekTime.isSome());
  LOGV("Seeking audio to %" PRId64, mPendingSeekTime.ref().ToMicroseconds());
  auto seekTime = mPendingSeekTime.ref();
  mAudio.mTrackDemuxer->Seek(seekTime)
    ->Then(OwnerThread(), __func__, this,
           &MediaFormatReader::OnAudioSeekCompleted,
           &MediaFormatReader::OnAudioSeekFailed)
    ->Track(mAudio.mSeekRequest);
}

void
MediaFormatReader::OnAudioSeekCompleted(TimeUnit aTime)
{
  MOZ_ASSERT(OnTaskQueue());
  LOGV("Audio seeked to %" PRId64, aTime.ToMicroseconds());
  mAudio.mSeekRequest.Complete();
  mAudio.mFirstFrameTime = Some(aTime);
  mPendingSeekTime.reset();
  mSeekPromise.Resolve(aTime, __func__);
}

void
MediaFormatReader::OnAudioSeekFailed(const MediaResult& aError)
{
  OnSeekFailed(TrackType::kAudioTrack, aError);
}

void MediaFormatReader::ReleaseResources()
{
  LOGV("");
  if (mShutdown) {
    return;
  }
  ShutdownDecoder(TrackInfo::kAudioTrack);
  ShutdownDecoder(TrackInfo::kVideoTrack);
}

bool
MediaFormatReader::VideoIsHardwareAccelerated() const
{
  return mVideo.mIsHardwareAccelerated;
}

void
MediaFormatReader::NotifyTrackDemuxers()
{
  MOZ_ASSERT(OnTaskQueue());

  LOGV("");

  if (!mInitDone) {
    return;
  }

  if (HasVideo()) {
    mVideo.mReceivedNewData = true;
    ScheduleUpdate(TrackType::kVideoTrack);
  }
  if (HasAudio()) {
    mAudio.mReceivedNewData = true;
    ScheduleUpdate(TrackType::kAudioTrack);
  }
}

void
MediaFormatReader::NotifyDataArrived()
{
  MOZ_ASSERT(OnTaskQueue());

  if (mShutdown || !mDemuxer || !mDemuxerInitDone) {
    return;
  }

  if (mNotifyDataArrivedPromise.Exists()) {
    // Already one in progress. Set the dirty flag so we can process it later.
    mPendingNotifyDataArrived = true;
    return;
  }

  RefPtr<MediaFormatReader> self = this;
  mDemuxer->NotifyDataArrived()
    ->Then(OwnerThread(), __func__,
           [self]() {
             self->mNotifyDataArrivedPromise.Complete();
             self->UpdateBuffered();
             self->NotifyTrackDemuxers();
             if (self->mPendingNotifyDataArrived) {
               self->mPendingNotifyDataArrived = false;
               self->NotifyDataArrived();
             }
           },
           [self]() { self->mNotifyDataArrivedPromise.Complete(); })
    ->Track(mNotifyDataArrivedPromise);
}

void
MediaFormatReader::UpdateBuffered()
{
  MOZ_ASSERT(OnTaskQueue());

  if (mShutdown) {
    return;
  }

  if (!mInitDone || !mHasStartTime) {
    mBuffered = TimeIntervals();
    return;
  }

  if (HasVideo()) {
    mVideo.mTimeRanges = mVideo.mTrackDemuxer->GetBuffered();
    bool hasLastEnd;
    auto lastEnd = mVideo.mTimeRanges.GetEnd(&hasLastEnd);
    if (hasLastEnd) {
      if (mVideo.mLastTimeRangesEnd &&
          mVideo.mLastTimeRangesEnd.ref() < lastEnd) {
        // New data was added after our previous end, we can clear the EOS flag.
        mVideo.mDemuxEOS = false;
        ScheduleUpdate(TrackInfo::kVideoTrack);
      }
      mVideo.mLastTimeRangesEnd = Some(lastEnd);
    }
  }
  if (HasAudio()) {
    mAudio.mTimeRanges = mAudio.mTrackDemuxer->GetBuffered();
    bool hasLastEnd;
    auto lastEnd = mAudio.mTimeRanges.GetEnd(&hasLastEnd);
    if (hasLastEnd) {
      if (mAudio.mLastTimeRangesEnd &&
          mAudio.mLastTimeRangesEnd.ref() < lastEnd) {
        // New data was added after our previous end, we can clear the EOS flag.
        mAudio.mDemuxEOS = false;
        ScheduleUpdate(TrackInfo::kAudioTrack);
      }
      mAudio.mLastTimeRangesEnd = Some(lastEnd);
    }
  }

  media::TimeIntervals intervals;
  if (HasAudio() && HasVideo()) {
    intervals = media::Intersection(mVideo.mTimeRanges, mAudio.mTimeRanges);
  } else if (HasAudio()) {
    intervals = mAudio.mTimeRanges;
  } else if (HasVideo()) {
    intervals = mVideo.mTimeRanges;
  }

  if (!intervals.Length() || intervals.GetStart() == TimeUnit::Zero()) {
    // IntervalSet already starts at 0 or is empty, nothing to shift.
    mBuffered = intervals;
  } else {
    mBuffered =
      intervals.Shift(TimeUnit::Zero() - mInfo.mStartTime);
  }
}

layers::ImageContainer*
MediaFormatReader::GetImageContainer()
{
  return mVideoFrameContainer ? mVideoFrameContainer->GetImageContainer()
                              : nullptr;
}

void
MediaFormatReader::GetMozDebugReaderData(nsACString& aString)
{
  nsCString result;
  nsAutoCString audioDecoderName("unavailable");
  nsAutoCString videoDecoderName = audioDecoderName;
  nsAutoCString audioType("none");
  nsAutoCString videoType("none");

  if (HasAudio()) {
    MutexAutoLock lock(mAudio.mMutex);
    audioDecoderName = mAudio.mDecoder
                       ? mAudio.mDecoder->GetDescriptionName()
                       : mAudio.mDescription;
    audioType = mAudio.mInfo ? mAudio.mInfo->mMimeType : mInfo.mAudio.mMimeType;
  }
  if (HasVideo()) {
    MutexAutoLock mon(mVideo.mMutex);
    videoDecoderName = mVideo.mDecoder
                       ? mVideo.mDecoder->GetDescriptionName()
                       : mVideo.mDescription;
    videoType = mVideo.mInfo ? mVideo.mInfo->mMimeType : mInfo.mVideo.mMimeType;
  }

  result +=
    nsPrintfCString("Audio Decoder(%s, %u channels @ %0.1fkHz): %s\n",
                    audioType.get(),
                    mAudio.mInfo ? mAudio.mInfo->GetAsAudioInfo()->mChannels
                                 : mInfo.mAudio.mChannels,
                    (mAudio.mInfo ? mAudio.mInfo->GetAsAudioInfo()->mRate
                                  : mInfo.mAudio.mRate) /
                      1000.0f,
                    audioDecoderName.get());
  result += nsPrintfCString("Audio Frames Decoded: %" PRIu64 "\n",
                            mAudio.mNumSamplesOutputTotal);
  if (HasAudio()) {
    result += nsPrintfCString(
      "Audio State: ni=%d no=%d wp=%d demuxr=%d demuxq=%u decoder=%d tt=%.1f "
      "tths=%d in=%" PRIu64 " out=%" PRIu64
      " qs=%u pending=%u wfd=%d eos=%d ds=%d wfk=%d sid=%u\n",
      NeedInput(mAudio),
      mAudio.HasPromise(),
      !mAudio.mWaitingPromise.IsEmpty(),
      mAudio.mDemuxRequest.Exists(),
      uint32_t(mAudio.mQueuedSamples.Length()),
      mAudio.mDecodeRequest.Exists(),
      mAudio.mTimeThreshold ? mAudio.mTimeThreshold.ref().Time().ToSeconds()
                            : -1.0,
      mAudio.mTimeThreshold ? mAudio.mTimeThreshold.ref().mHasSeeked : -1,
      mAudio.mNumSamplesInput,
      mAudio.mNumSamplesOutput,
      unsigned(size_t(mAudio.mSizeOfQueue)),
      unsigned(mAudio.mOutput.Length()),
      mAudio.mWaitingForData,
      mAudio.mDemuxEOS,
      int32_t(mAudio.mDrainState),
      mAudio.mWaitingForKey,
      mAudio.mLastStreamSourceID);
  }

  VideoInfo videoInfo = mVideo.mInfo
                        ? *mVideo.mInfo->GetAsVideoInfo()
                        : mVideo.mOriginalInfo
                          ? *mVideo.mOriginalInfo->GetAsVideoInfo()
                          : VideoInfo();

  result += nsPrintfCString(
    "Video Decoder(%s, %dx%d @ %0.2f): %s\n",
    videoType.get(),
    videoInfo.mDisplay.width < 0 ? 0 : videoInfo.mDisplay.width,
    videoInfo.mDisplay.height < 0 ? 0 : videoInfo.mDisplay.height,
    mVideo.mMeanRate.Mean(),
    videoDecoderName.get());

  result +=
    nsPrintfCString("Hardware Video Decoding: %s\n",
                    VideoIsHardwareAccelerated() ? "enabled" : "disabled");
  result +=
    nsPrintfCString("Video Frames Decoded: %" PRIu64 " (skipped=%" PRIu64 ")\n",
                    mVideo.mNumSamplesOutputTotal,
                    mVideo.mNumSamplesSkippedTotal);
  if (HasVideo()) {
    result += nsPrintfCString(
      "Video State: ni=%d no=%d wp=%d demuxr=%d demuxq=%u decoder=%d tt=%.1f "
      "tths=%d in=%" PRIu64 " out=%" PRIu64
      " qs=%u pending:%u wfd=%d eos=%d ds=%d wfk=%d sid=%u\n",
      NeedInput(mVideo),
      mVideo.HasPromise(),
      !mVideo.mWaitingPromise.IsEmpty(),
      mVideo.mDemuxRequest.Exists(),
      uint32_t(mVideo.mQueuedSamples.Length()),
      mVideo.mDecodeRequest.Exists(),
      mVideo.mTimeThreshold ? mVideo.mTimeThreshold.ref().Time().ToSeconds()
                            : -1.0,
      mVideo.mTimeThreshold ? mVideo.mTimeThreshold.ref().mHasSeeked : -1,
      mVideo.mNumSamplesInput,
      mVideo.mNumSamplesOutput,
      unsigned(size_t(mVideo.mSizeOfQueue)),
      unsigned(mVideo.mOutput.Length()),
      mVideo.mWaitingForData,
      mVideo.mDemuxEOS,
      int32_t(mVideo.mDrainState),
      mVideo.mWaitingForKey,
      mVideo.mLastStreamSourceID);
  }
  aString += result;
}

void
MediaFormatReader::SetVideoNullDecode(bool aIsNullDecode)
{
  MOZ_ASSERT(OnTaskQueue());
  return SetNullDecode(TrackType::kVideoTrack, aIsNullDecode);
}

void
MediaFormatReader::UpdateCompositor(
  already_AddRefed<layers::KnowsCompositor> aCompositor)
{
  MOZ_ASSERT(OnTaskQueue());
  mKnowsCompositor = aCompositor;
}

void
MediaFormatReader::SetNullDecode(TrackType aTrack, bool aIsNullDecode)
{
  MOZ_ASSERT(OnTaskQueue());

  auto& decoder = GetDecoderData(aTrack);
  if (decoder.mIsNullDecode == aIsNullDecode) {
    return;
  }

  LOG("%s, decoder.mIsNullDecode = %d => aIsNullDecode = %d",
      TrackTypeToStr(aTrack), decoder.mIsNullDecode, aIsNullDecode);

  decoder.mIsNullDecode = aIsNullDecode;
  ShutdownDecoder(aTrack);
}

void
MediaFormatReader::OnFirstDemuxCompleted(
  TrackInfo::TrackType aType, RefPtr<MediaTrackDemuxer::SamplesHolder> aSamples)
{
  MOZ_ASSERT(OnTaskQueue());

  if (mShutdown) {
    return;
  }

  auto& decoder = GetDecoderData(aType);
  MOZ_ASSERT(decoder.mFirstDemuxedSampleTime.isNothing());
  decoder.mFirstDemuxedSampleTime.emplace(aSamples->mSamples[0]->mTime);
  MaybeResolveMetadataPromise();
}

void
MediaFormatReader::OnFirstDemuxFailed(TrackInfo::TrackType aType,
                                      const MediaResult& aError)
{
  MOZ_ASSERT(OnTaskQueue());

  if (mShutdown) {
    return;
  }

  auto& decoder = GetDecoderData(aType);
  MOZ_ASSERT(decoder.mFirstDemuxedSampleTime.isNothing());
  decoder.mFirstDemuxedSampleTime.emplace(TimeUnit::FromInfinity());
  MaybeResolveMetadataPromise();
}

} // namespace mozilla

#undef NS_DispatchToMainThread
