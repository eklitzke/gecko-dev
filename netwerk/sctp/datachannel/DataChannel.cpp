/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#if !defined(__Userspace_os_Windows)
#include <arpa/inet.h>
#endif
// usrsctp.h expects to have errno definitions prior to its inclusion.
#include <errno.h>

#define SCTP_DEBUG 1
#define SCTP_STDINT_INCLUDE <stdint.h>

#ifdef _MSC_VER
// Disable "warning C4200: nonstandard extension used : zero-sized array in
//          struct/union"
// ...which the third-party file usrsctp.h runs afoul of.
#pragma warning(push)
#pragma warning(disable:4200)
#endif

#include "usrsctp.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "DataChannelLog.h"

#include "nsServiceManagerUtils.h"
#include "nsIObserverService.h"
#include "nsIObserver.h"
#include "nsIPrefBranch.h"
#include "nsIPrefService.h"
#include "mozilla/Services.h"
#include "mozilla/Sprintf.h"
#include "nsProxyRelease.h"
#include "nsThread.h"
#include "nsThreadUtils.h"
#include "nsAutoPtr.h"
#include "nsNetUtil.h"
#include "nsNetCID.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/Unused.h"
#ifdef MOZ_PEERCONNECTION
#include "mtransport/runnable_utils.h"
#endif

#define DATACHANNEL_LOG(args) LOG(args)
#include "DataChannel.h"
#include "DataChannelProtocol.h"

// Let us turn on and off important assertions in non-debug builds
#ifdef DEBUG
#define ASSERT_WEBRTC(x) MOZ_ASSERT((x))
#elif defined(MOZ_WEBRTC_ASSERT_ALWAYS)
#define ASSERT_WEBRTC(x) do { if (!(x)) { MOZ_CRASH(); } } while (0)
#endif

static bool sctp_initialized;

namespace mozilla {

LazyLogModule gDataChannelLog("DataChannel");
static LazyLogModule gSCTPLog("SCTP");

#define SCTP_LOG(args) MOZ_LOG(mozilla::gSCTPLog, mozilla::LogLevel::Debug, args)

class DataChannelConnectionShutdown : public nsITimerCallback
{
public:
  explicit DataChannelConnectionShutdown(DataChannelConnection* aConnection)
    : mConnection(aConnection)
  {
    mTimer = NS_NewTimer(); // we'll crash if this fails
    mTimer->InitWithCallback(this, 30*1000, nsITimer::TYPE_ONE_SHOT);
  }

  NS_IMETHODIMP Notify(nsITimer* aTimer) override;

  NS_DECL_THREADSAFE_ISUPPORTS

private:
  virtual ~DataChannelConnectionShutdown()
  {
    mTimer->Cancel();
  }

  RefPtr<DataChannelConnection> mConnection;
  nsCOMPtr<nsITimer> mTimer;
};

class DataChannelShutdown;

StaticRefPtr<DataChannelShutdown> sDataChannelShutdown;

class DataChannelShutdown : public nsIObserver
{
public:
  // This needs to be tied to some object that is guaranteed to be
  // around (singleton likely) unless we want to shutdown sctp whenever
  // we're not using it (and in which case we'd keep a refcnt'd object
  // ref'd by each DataChannelConnection to release the SCTP usrlib via
  // sctp_finish). Right now, the single instance of this class is
  // owned by the observer service and a StaticRefPtr.

  NS_DECL_ISUPPORTS

  DataChannelShutdown() = default;

  void Init()
    {
      nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();
      if (!observerService)
        return;

      nsresult rv = observerService->AddObserver(this,
                                                 "xpcom-will-shutdown",
                                                 false);
      MOZ_ASSERT(rv == NS_OK);
      (void) rv;
    }

  NS_IMETHOD Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) override
  {
    // Note: MainThread
    if (strcmp(aTopic, "xpcom-will-shutdown") == 0) {
      LOG(("Shutting down SCTP"));
      if (sctp_initialized) {
        usrsctp_finish();
        sctp_initialized = false;
      }
      nsCOMPtr<nsIObserverService> observerService =
        mozilla::services::GetObserverService();
      if (!observerService)
        return NS_ERROR_FAILURE;

      nsresult rv = observerService->RemoveObserver(this,
                                                    "xpcom-will-shutdown");
      MOZ_ASSERT(rv == NS_OK);
      (void) rv;

      {
        StaticMutexAutoLock lock(sLock);
        sConnections = nullptr; // clears as well
      }
      sDataChannelShutdown = nullptr;
    }
    return NS_OK;
  }

  void CreateConnectionShutdown(DataChannelConnection* aConnection)
  {
    StaticMutexAutoLock lock(sLock);
    if (!sConnections) {
      sConnections = new nsTArray<RefPtr<DataChannelConnectionShutdown>>();
    }
    sConnections->AppendElement(new DataChannelConnectionShutdown(aConnection));
  }

  void RemoveConnectionShutdown(DataChannelConnectionShutdown* aConnectionShutdown)
  {
    StaticMutexAutoLock lock(sLock);
    if (sConnections) {
      sConnections->RemoveElement(aConnectionShutdown);
    }
  }

private:
  // The only instance of DataChannelShutdown is owned by the observer
  // service, so there is no need to call RemoveObserver here.
  virtual ~DataChannelShutdown() = default;

  // protects sConnections
  static StaticMutex sLock;
  static StaticAutoPtr<nsTArray<RefPtr<DataChannelConnectionShutdown>>> sConnections;
};

StaticMutex DataChannelShutdown::sLock;
StaticAutoPtr<nsTArray<RefPtr<DataChannelConnectionShutdown>>> DataChannelShutdown::sConnections;

NS_IMPL_ISUPPORTS(DataChannelShutdown, nsIObserver);

NS_IMPL_ISUPPORTS(DataChannelConnectionShutdown, nsITimerCallback)

NS_IMETHODIMP
DataChannelConnectionShutdown::Notify(nsITimer* aTimer)
{
  // safely release reference to ourself
  RefPtr<DataChannelConnectionShutdown> grip(this);
  // Might not be set. We don't actually use the |this| pointer in
  // RemoveConnectionShutdown right now, which makes this a bit gratuitous
  // anyway...
  if (sDataChannelShutdown) {
    sDataChannelShutdown->RemoveConnectionShutdown(this);
  }
  return NS_OK;
}

OutgoingMsg::OutgoingMsg(struct sctp_sendv_spa &info, const uint8_t *data,
                         size_t length)
  : mLength(length)
  , mData(data)
{
  mInfo = &info;
  mPos = 0;
}

void OutgoingMsg::Advance(size_t offset)
{
  mPos += offset;
  if (mPos > mLength) {
    mPos = mLength;
  }
}

BufferedOutgoingMsg::BufferedOutgoingMsg(OutgoingMsg &msg)
{
  size_t length = msg.GetLeft();
  auto *tmp = new uint8_t[length]; // infallible malloc!
  memcpy(tmp, msg.GetData(), length);
  mLength = length;
  mData = tmp;
  mInfo = new sctp_sendv_spa;
  *mInfo = msg.GetInfo();
  mPos = 0;
}

BufferedOutgoingMsg::~BufferedOutgoingMsg()
{
  delete mInfo;
  delete mData;
}

static int
receive_cb(struct socket* sock, union sctp_sockstore addr,
           void *data, size_t datalen,
           struct sctp_rcvinfo rcv, int flags, void *ulp_info)
{
  DataChannelConnection *connection = static_cast<DataChannelConnection*>(ulp_info);
  return connection->ReceiveCallback(sock, data, datalen, rcv, flags);
}

static
DataChannelConnection *
GetConnectionFromSocket(struct socket* sock)
{
  struct sockaddr *addrs = nullptr;
  int naddrs = usrsctp_getladdrs(sock, 0, &addrs);
  if (naddrs <= 0 || addrs[0].sa_family != AF_CONN) {
    return nullptr;
  }
  // usrsctp_getladdrs() returns the addresses bound to this socket, which
  // contains the SctpDataMediaChannel* as sconn_addr.  Read the pointer,
  // then free the list of addresses once we have the pointer.  We only open
  // AF_CONN sockets, and they should all have the sconn_addr set to the
  // pointer that created them, so [0] is as good as any other.
  struct sockaddr_conn *sconn = reinterpret_cast<struct sockaddr_conn *>(&addrs[0]);
  DataChannelConnection *connection =
    reinterpret_cast<DataChannelConnection *>(sconn->sconn_addr);
  usrsctp_freeladdrs(addrs);

  return connection;
}

// called when the buffer empties to the threshold value
static int
threshold_event(struct socket* sock, uint32_t sb_free)
{
  DataChannelConnection *connection = GetConnectionFromSocket(sock);
  if (connection) {
    connection->SendDeferredMessages();
  } else {
    LOG(("Can't find connection for socket %p", sock));
  }
  return 0;
}

static void
debug_printf(const char *format, ...)
{
  va_list ap;
  char buffer[1024];

  if (MOZ_LOG_TEST(gSCTPLog, LogLevel::Debug)) {
    va_start(ap, format);
#ifdef _WIN32
    if (vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, ap) > 0) {
#else
    if (VsprintfLiteral(buffer, format, ap) > 0) {
#endif
      SCTP_LOG(("%s", buffer));
    }
    va_end(ap);
  }
}

DataChannelConnection::DataChannelConnection(DataConnectionListener *listener,
                                             nsIEventTarget *aTarget)
  : NeckoTargetHolder(aTarget)
  , mLock("netwerk::sctp::DataChannelConnection")
{
  mCurrentStream = 0;
  mState = CLOSED;
  mSocket = nullptr;
  mMasterSocket = nullptr;
  mListener = listener;
  mLocalPort = 0;
  mRemotePort = 0;
  mPendingType = PENDING_NONE;
  LOG(("Constructor DataChannelConnection=%p, listener=%p", this, mListener.get()));
  mInternalIOThread = nullptr;
}

DataChannelConnection::~DataChannelConnection()
{
  LOG(("Deleting DataChannelConnection %p", (void *) this));
  // This may die on the MainThread, or on the STS thread
  ASSERT_WEBRTC(mState == CLOSED);
  MOZ_ASSERT(!mMasterSocket);
  MOZ_ASSERT(mPending.GetSize() == 0);
  MOZ_ASSERT(!mTransportFlow);

  // Already disconnected from sigslot/mTransportFlow
  // TransportFlows must be released from the STS thread
  if (!IsSTSThread()) {
    ASSERT_WEBRTC(NS_IsMainThread());

    if (mInternalIOThread) {
      // Avoid spinning the event thread from here (which if we're mainthread
      // is in the event loop already)
      nsCOMPtr<nsIRunnable> r = WrapRunnable(nsCOMPtr<nsIThread>(mInternalIOThread),
                                             &nsIThread::Shutdown);
      Dispatch(r.forget());
    }
  } else {
    // on STS, safe to call shutdown
    if (mInternalIOThread) {
      mInternalIOThread->Shutdown();
    }
  }
}

void
DataChannelConnection::Destroy()
{
  // Though it's probably ok to do this and close the sockets;
  // if we really want it to do true clean shutdowns it can
  // create a dependant Internal object that would remain around
  // until the network shut down the association or timed out.
  LOG(("Destroying DataChannelConnection %p", (void *) this));
  ASSERT_WEBRTC(NS_IsMainThread());
  CloseAll();

  MutexAutoLock lock(mLock);
  // If we had a pending reset, we aren't waiting for it - clear the list so
  // we can deregister this DataChannelConnection without leaking.
  ClearResets();

  MOZ_ASSERT(mSTS);
  ASSERT_WEBRTC(NS_IsMainThread());
  // Finish Destroy on STS thread to avoid bug 876167 - once that's fixed,
  // the usrsctp_close() calls can move back here (and just proxy the
  // disconnect_all())
  RUN_ON_THREAD(mSTS, WrapRunnable(RefPtr<DataChannelConnection>(this),
                                   &DataChannelConnection::DestroyOnSTS,
                                   mSocket, mMasterSocket),
                NS_DISPATCH_NORMAL);

  // These will be released on STS
  mSocket = nullptr;
  mMasterSocket = nullptr; // also a flag that we've Destroyed this connection

  // We can't get any more new callbacks from the SCTP library
  // All existing callbacks have refs to DataChannelConnection

  // nsDOMDataChannel objects have refs to DataChannels that have refs to us
}

void DataChannelConnection::DestroyOnSTS(struct socket *aMasterSocket,
                                         struct socket *aSocket)
{
  if (aSocket && aSocket != aMasterSocket)
    usrsctp_close(aSocket);
  if (aMasterSocket)
    usrsctp_close(aMasterSocket);

  usrsctp_deregister_address(static_cast<void *>(this));
  LOG(("Deregistered %p from the SCTP stack.", static_cast<void *>(this)));

  disconnect_all();

  // we may have queued packet sends on STS after this; dispatch to ourselves before finishing here
  // so we can be sure there aren't anymore runnables active that can try to touch the flow.
  // DON'T use RUN_ON_THREAD, it queue-jumps!
  mSTS->Dispatch(WrapRunnable(RefPtr<DataChannelConnection>(this),
                              &DataChannelConnection::DestroyOnSTSFinal),
                 NS_DISPATCH_NORMAL);
}

void DataChannelConnection::DestroyOnSTSFinal()
{
  mTransportFlow = nullptr;
  sDataChannelShutdown->CreateConnectionShutdown(this);
}

bool
DataChannelConnection::Init(unsigned short aPort, uint16_t aNumStreams, bool aMaxMessageSizeSet,
                            uint64_t aMaxMessageSize)
{
  struct sctp_initmsg initmsg;
  struct sctp_assoc_value av;
  struct sctp_event event;
  socklen_t len;

  uint16_t event_types[] = {SCTP_ASSOC_CHANGE,
                            SCTP_PEER_ADDR_CHANGE,
                            SCTP_REMOTE_ERROR,
                            SCTP_SHUTDOWN_EVENT,
                            SCTP_ADAPTATION_INDICATION,
                            SCTP_PARTIAL_DELIVERY_EVENT,
                            SCTP_SEND_FAILED_EVENT,
                            SCTP_STREAM_RESET_EVENT,
                            SCTP_STREAM_CHANGE_EVENT};
  {
    ASSERT_WEBRTC(NS_IsMainThread());
    // MutexAutoLock lock(mLock); Not needed since we're on mainthread always

    mSendInterleaved = false;
    mPpidFragmentation = false;
    mMaxMessageSizeSet = false;
    SetMaxMessageSize(aMaxMessageSizeSet, aMaxMessageSize);

    if (!sctp_initialized) {
      LOG(("sctp_init"));
#ifdef MOZ_PEERCONNECTION
      usrsctp_init(0,
                   DataChannelConnection::SctpDtlsOutput,
                   debug_printf
                  );
#else
      MOZ_CRASH("Trying to use SCTP/DTLS without mtransport");
#endif

      // Set logging to SCTP:LogLevel::Debug to get SCTP debugs
      if (MOZ_LOG_TEST(gSCTPLog, LogLevel::Debug)) {
        usrsctp_sysctl_set_sctp_debug_on(SCTP_DEBUG_ALL);
      }

      // Do not send ABORTs in response to INITs (1).
      // Do not send ABORTs for received Out of the Blue packets (2).
      usrsctp_sysctl_set_sctp_blackhole(2);

      // Disable the Explicit Congestion Notification extension (currently not supported by the
      // Firefox code)
      usrsctp_sysctl_set_sctp_ecn_enable(0);

      // Enable interleaving messages for different streams (incoming)
      // See: https://tools.ietf.org/html/rfc6458#section-8.1.20
      usrsctp_sysctl_set_sctp_default_frag_interleave(2);

      sctp_initialized = true;

      sDataChannelShutdown = new DataChannelShutdown();
      sDataChannelShutdown->Init();
    }
  }

  // XXX FIX! make this a global we get once
  // Find the STS thread
  nsresult rv;
  mSTS = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  // Open sctp with a callback
  if ((mMasterSocket = usrsctp_socket(
         AF_CONN, SOCK_STREAM, IPPROTO_SCTP, receive_cb, threshold_event,
         usrsctp_sysctl_get_sctp_sendspace() / 2, this)) == nullptr) {
    return false;
  }

  // Make non-blocking for bind/connect.  SCTP over UDP defaults to non-blocking
  // in associations for normal IO
  if (usrsctp_set_non_blocking(mMasterSocket, 1) < 0) {
    LOG(("Couldn't set non_blocking on SCTP socket"));
    // We can't handle connect() safely if it will block, not that this will
    // even happen.
    goto error_cleanup;
  }

  // Make sure when we close the socket, make sure it doesn't call us back again!
  // This would cause it try to use an invalid DataChannelConnection pointer
  struct linger l;
  l.l_onoff = 1;
  l.l_linger = 0;
  if (usrsctp_setsockopt(mMasterSocket, SOL_SOCKET, SO_LINGER,
                         (const void *)&l, (socklen_t)sizeof(struct linger)) < 0) {
    LOG(("Couldn't set SO_LINGER on SCTP socket"));
    // unsafe to allow it to continue if this fails
    goto error_cleanup;
  }

  // XXX Consider disabling this when we add proper SDP negotiation.
  // We may want to leave enabled for supporting 'cloning' of SDP offers, which
  // implies re-use of the same pseudo-port number, or forcing a renegotiation.
  {
    const int option_value = 1;
    if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_REUSE_PORT,
                           (const void *)&option_value, (socklen_t)sizeof(option_value)) < 0) {
      LOG(("Couldn't set SCTP_REUSE_PORT on SCTP socket"));
    }
    if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_NODELAY,
                           (const void *)&option_value, (socklen_t)sizeof(option_value)) < 0) {
      LOG(("Couldn't set SCTP_NODELAY on SCTP socket"));
    }
  }

  // Set explicit EOR
  {
    const int option_value = 1;
    if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_EXPLICIT_EOR,
                           (const void *)&option_value, (socklen_t)sizeof(option_value)) < 0) {
      LOG(("*** failed enable explicit EOR mode %d", errno));
      goto error_cleanup;
    }
  }

  // Enable ndata
  // TODO: Bug 1381145, enable this once ndata has been deployed
#if 0
  av.assoc_id = SCTP_FUTURE_ASSOC;
  av.assoc_value = 1;
  if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_INTERLEAVING_SUPPORTED, &av,
                         (socklen_t)sizeof(struct sctp_assoc_value)) < 0) {
    LOG(("*** failed enable ndata errno %d", errno));
    goto error_cleanup;
  }
#endif

  av.assoc_id = SCTP_ALL_ASSOC;
  av.assoc_value = SCTP_ENABLE_RESET_STREAM_REQ | SCTP_ENABLE_CHANGE_ASSOC_REQ;
  if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_ENABLE_STREAM_RESET, &av,
                         (socklen_t)sizeof(struct sctp_assoc_value)) < 0) {
    LOG(("*** failed enable stream reset errno %d", errno));
    goto error_cleanup;
  }

  /* Enable the events of interest. */
  memset(&event, 0, sizeof(event));
  event.se_assoc_id = SCTP_ALL_ASSOC;
  event.se_on = 1;
  for (unsigned short event_type : event_types) {
    event.se_type = event_type;
    if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_EVENT, &event, sizeof(event)) < 0) {
      LOG(("*** failed setsockopt SCTP_EVENT errno %d", errno));
      goto error_cleanup;
    }
  }

  // Update number of streams
  mStreams.AppendElements(aNumStreams);
  for (uint32_t i = 0; i < aNumStreams; ++i) {
    mStreams[i] = nullptr;
  }
  memset(&initmsg, 0, sizeof(initmsg));
  len = sizeof(initmsg);
  if (usrsctp_getsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, &len) < 0) {
    LOG(("*** failed getsockopt SCTP_INITMSG"));
    goto error_cleanup;
  }
  LOG(("Setting number of SCTP streams to %u, was %u/%u", aNumStreams,
       initmsg.sinit_num_ostreams, initmsg.sinit_max_instreams));
  initmsg.sinit_num_ostreams  = aNumStreams;
  initmsg.sinit_max_instreams = MAX_NUM_STREAMS;
  if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_INITMSG, &initmsg,
                         (socklen_t)sizeof(initmsg)) < 0) {
    LOG(("*** failed setsockopt SCTP_INITMSG, errno %d", errno));
    goto error_cleanup;
  }

  mSocket = nullptr;
  usrsctp_register_address(static_cast<void *>(this));
  LOG(("Registered %p within the SCTP stack.", static_cast<void *>(this)));
  return true;

error_cleanup:
  usrsctp_close(mMasterSocket);
  mMasterSocket = nullptr;
  return false;
}

void
DataChannelConnection::SetMaxMessageSize(bool aMaxMessageSizeSet, uint64_t aMaxMessageSize)
{
  MutexAutoLock lock(mLock); // TODO: Needed?

  if (mMaxMessageSizeSet && !aMaxMessageSizeSet) {
    // Don't overwrite already set MMS with default values
    return;
  }

  mMaxMessageSizeSet = aMaxMessageSizeSet;
  mMaxMessageSize = aMaxMessageSize;

  bool ppidFragmentationEnforced = false;
  nsresult rv;
  nsCOMPtr<nsIPrefService> prefs = do_GetService("@mozilla.org/preferences-service;1", &rv);
  if (!NS_WARN_IF(NS_FAILED(rv))) {
    nsCOMPtr<nsIPrefBranch> branch = do_QueryInterface(prefs);

    if (branch) {
      if (!NS_FAILED(branch->GetBoolPref(
          "media.peerconnection.sctp.force_ppid_fragmentation", &mPpidFragmentation))) {
        // Ensure that forced on/off PPID fragmentation does not get overridden when Firefox has
        // been detected.
        mMaxMessageSizeSet = true;
        ppidFragmentationEnforced = true;
      }

      int32_t temp;
      if (!NS_FAILED(branch->GetIntPref(
          "media.peerconnection.sctp.force_maximum_message_size", &temp))) {
        if (temp >= 0) {
          mMaxMessageSize = (uint64_t)temp;
        }
      }
    }
  }

  // Fix remote MMS. This code exists, so future implementations of RTCSctpTransport.maxMessageSize
  // can simply provide that value from GetMaxMessageSize.

  // TODO: Bug 1382779, once resolved, can be increased to min(Uint8ArrayMaxSize, UINT32_MAX)
  // TODO: Bug 1381146, once resolved, can be increased to whatever we support then (hopefully
  //       SIZE_MAX)
  if (mMaxMessageSize == 0 || mMaxMessageSize > WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_REMOTE) {
    mMaxMessageSize = WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_REMOTE;
  }

  LOG(("Use PPID-based fragmentation/reassembly: %s (enforced=%s)",
       mPpidFragmentation ? "yes" : "no", ppidFragmentationEnforced ? "yes" : "no"));
  LOG(("Maximum message size (outgoing data): %" PRIu64 " (set=%s, enforced=%s)",
       mMaxMessageSize, mMaxMessageSizeSet ? "yes" : "no",
       aMaxMessageSize != mMaxMessageSize ? "yes" : "no"));
}

uint64_t
DataChannelConnection::GetMaxMessageSize()
{
  return mMaxMessageSize;
}

#ifdef MOZ_PEERCONNECTION
void
DataChannelConnection::SetEvenOdd()
{
  ASSERT_WEBRTC(IsSTSThread());

  TransportLayerDtls *dtls = static_cast<TransportLayerDtls *>(
      mTransportFlow->GetLayer(TransportLayerDtls::ID()));
  MOZ_ASSERT(dtls);  // DTLS is mandatory
  mAllocateEven = (dtls->role() == TransportLayerDtls::CLIENT);
}

bool
DataChannelConnection::ConnectViaTransportFlow(TransportFlow *aFlow, uint16_t localport, uint16_t remoteport)
{
  LOG(("Connect DTLS local %u, remote %u", localport, remoteport));

  MOZ_ASSERT(mMasterSocket, "SCTP wasn't initialized before ConnectViaTransportFlow!");
  if (NS_WARN_IF(!aFlow)) {
    return false;
  }

  mTransportFlow = aFlow;
  mLocalPort = localport;
  mRemotePort = remoteport;
  mState = CONNECTING;

  RUN_ON_THREAD(mSTS, WrapRunnable(RefPtr<DataChannelConnection>(this),
                                   &DataChannelConnection::SetSignals),
                NS_DISPATCH_NORMAL);
  return true;
}

void
DataChannelConnection::SetSignals()
{
  ASSERT_WEBRTC(IsSTSThread());
  ASSERT_WEBRTC(mTransportFlow);
  LOG(("Setting transport signals, state: %d", mTransportFlow->state()));
  mTransportFlow->SignalPacketReceived.connect(this, &DataChannelConnection::SctpDtlsInput);
  // SignalStateChange() doesn't call you with the initial state
  mTransportFlow->SignalStateChange.connect(this, &DataChannelConnection::CompleteConnect);
  CompleteConnect(mTransportFlow, mTransportFlow->state());
}

void
DataChannelConnection::CompleteConnect(TransportFlow *flow, TransportLayer::State state)
{
  LOG(("Data transport state: %d", state));
  MutexAutoLock lock(mLock);
  ASSERT_WEBRTC(IsSTSThread());
  // We should abort connection on TS_ERROR.
  // Note however that the association will also fail (perhaps with a delay) and
  // notify us in that way
  if (state != TransportLayer::TS_OPEN || !mMasterSocket)
    return;

  struct sockaddr_conn addr;
  memset(&addr, 0, sizeof(addr));
  addr.sconn_family = AF_CONN;
#if defined(__Userspace_os_Darwin)
  addr.sconn_len = sizeof(addr);
#endif
  addr.sconn_port = htons(mLocalPort);
  addr.sconn_addr = static_cast<void *>(this);

  LOG(("Calling usrsctp_bind"));
  int r = usrsctp_bind(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr),
                       sizeof(addr));
  if (r < 0) {
    LOG(("usrsctp_bind failed: %d", r));
  } else {
    // This is the remote addr
    addr.sconn_port = htons(mRemotePort);
    LOG(("Calling usrsctp_connect"));
    r = usrsctp_connect(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr),
                        sizeof(addr));
    if (r >= 0 || errno == EINPROGRESS) {
      struct sctp_paddrparams paddrparams;
      socklen_t opt_len;

      memset(&paddrparams, 0, sizeof(struct sctp_paddrparams));
      memcpy(&paddrparams.spp_address, &addr, sizeof(struct sockaddr_conn));
      opt_len = (socklen_t)sizeof(struct sctp_paddrparams);
      r = usrsctp_getsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS,
                             &paddrparams, &opt_len);
      if (r < 0) {
        LOG(("usrsctp_getsockopt failed: %d", r));
      } else {
        // draft-ietf-rtcweb-data-channel-13 section 5: max initial MTU IPV4 1200, IPV6 1280
        paddrparams.spp_pathmtu = 1200; // safe for either
        paddrparams.spp_flags &= ~SPP_PMTUD_ENABLE;
        paddrparams.spp_flags |= SPP_PMTUD_DISABLE;
        opt_len = (socklen_t)sizeof(struct sctp_paddrparams);
        r = usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS,
                               &paddrparams, opt_len);
        if (r < 0) {
          LOG(("usrsctp_getsockopt failed: %d", r));
        } else {
          LOG(("usrsctp: PMTUD disabled, MTU set to %u", paddrparams.spp_pathmtu));
        }
      }
    }
    if (r < 0) {
      if (errno == EINPROGRESS) {
        // non-blocking
        return;
      }
      LOG(("usrsctp_connect failed: %d", errno));
      mState = CLOSED;
    } else {
      // We set Even/Odd and fire ON_CONNECTION via SCTP_COMM_UP when we get that
      // This also avoids issues with calling TransportFlow stuff on Mainthread
      return;
    }
  }
  // Note: currently this doesn't actually notify the application
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_CONNECTION,
             this)));
}

// Process any pending Opens
void
DataChannelConnection::ProcessQueuedOpens()
{
  // The nsDeque holds channels with an AddRef applied.  Another reference
  // (may) be held by the DOMDataChannel, unless it's been GC'd.  No other
  // references should exist.

  // Can't copy nsDeque's.  Move into temp array since any that fail will
  // go back to mPending
  nsDeque temp;
  DataChannel *temp_channel; // really already_AddRefed<>
  while (nullptr != (temp_channel = static_cast<DataChannel *>(mPending.PopFront()))) {
    temp.Push(static_cast<void *>(temp_channel));
  }

  RefPtr<DataChannel> channel;
  // All these entries have an AddRef(); make that explicit now via the dont_AddRef()
  while (nullptr != (channel = dont_AddRef(static_cast<DataChannel *>(temp.PopFront())))) {
    if (channel->mFlags & DATA_CHANNEL_FLAGS_FINISH_OPEN) {
      LOG(("Processing queued open for %p (%u)", channel.get(), channel->mStream));
      channel->mFlags &= ~DATA_CHANNEL_FLAGS_FINISH_OPEN;
      // OpenFinish returns a reference itself, so we need to take it can Release it
      channel = OpenFinish(channel.forget()); // may reset the flag and re-push
    } else {
      NS_ASSERTION(false, "How did a DataChannel get queued without the FINISH_OPEN flag?");
    }
  }

}
void
DataChannelConnection::SctpDtlsInput(TransportFlow *flow,
                                     const unsigned char *data, size_t len)
{
  if (MOZ_LOG_TEST(gSCTPLog, LogLevel::Debug)) {
    char *buf;

    if ((buf = usrsctp_dumppacket((void *)data, len, SCTP_DUMP_INBOUND)) != nullptr) {
      SCTP_LOG(("%s", buf));
      usrsctp_freedumpbuffer(buf);
    }
  }
  // Pass the data to SCTP
  MutexAutoLock lock(mLock);
  usrsctp_conninput(static_cast<void *>(this), data, len, 0);
}

int
DataChannelConnection::SendPacket(unsigned char data[], size_t len, bool release)
{
  //LOG(("%p: SCTP/DTLS sent %ld bytes", this, len));
  int res = mTransportFlow->SendPacket(data, len) < 0 ? 1 : 0;
  if (release)
    delete [] data;
  return res;
}

/* static */
int
DataChannelConnection::SctpDtlsOutput(void *addr, void *buffer, size_t length,
                                      uint8_t tos, uint8_t set_df)
{
  DataChannelConnection *peer = static_cast<DataChannelConnection *>(addr);
  int res;

  if (MOZ_LOG_TEST(gSCTPLog, LogLevel::Debug)) {
    char *buf;

    if ((buf = usrsctp_dumppacket(buffer, length, SCTP_DUMP_OUTBOUND)) != nullptr) {
      SCTP_LOG(("%s", buf));
      usrsctp_freedumpbuffer(buf);
    }
  }
  // We're async proxying even if on the STSThread because this is called
  // with internal SCTP locks held in some cases (such as in usrsctp_connect()).
  // SCTP has an option for Apple, on IP connections only, to release at least
  // one of the locks before calling a packet output routine; with changes to
  // the underlying SCTP stack this might remove the need to use an async proxy.
  if ((false /*peer->IsSTSThread()*/)) {
    res = peer->SendPacket(static_cast<unsigned char *>(buffer), length, false);
  } else {
    auto *data = new unsigned char[length];
    memcpy(data, buffer, length);
    // Commented out since we have to Dispatch SendPacket to avoid deadlock"
    // res = -1;

    // XXX It might be worthwhile to add an assertion against the thread
    // somehow getting into the DataChannel/SCTP code again, as
    // DISPATCH_SYNC is not fully blocking.  This may be tricky, as it
    // needs to be a per-thread check, not a global.
    peer->mSTS->Dispatch(WrapRunnable(
                           RefPtr<DataChannelConnection>(peer),
                           &DataChannelConnection::SendPacket, data, length, true),
                                   NS_DISPATCH_NORMAL);
    res = 0; // cheat!  Packets can always be dropped later anyways
  }
  return res;
}
#endif

#ifdef ALLOW_DIRECT_SCTP_LISTEN_CONNECT
// listen for incoming associations
// Blocks! - Don't call this from main thread!

#error This code will not work as-is since SetEvenOdd() runs on Mainthread

bool
DataChannelConnection::Listen(unsigned short port)
{
  struct sockaddr_in addr;
  socklen_t addr_len;

  NS_WARNING_ASSERTION(!NS_IsMainThread(),
                       "Blocks, do not call from main thread!!!");

  /* Acting as the 'server' */
  memset((void *)&addr, 0, sizeof(addr));
#ifdef HAVE_SIN_LEN
  addr.sin_len = sizeof(struct sockaddr_in);
#endif
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  LOG(("Waiting for connections on port %u", ntohs(addr.sin_port)));
  mState = CONNECTING;
  if (usrsctp_bind(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr), sizeof(struct sockaddr_in)) < 0) {
    LOG(("***Failed userspace_bind"));
    return false;
  }
  if (usrsctp_listen(mMasterSocket, 1) < 0) {
    LOG(("***Failed userspace_listen"));
    return false;
  }

  LOG(("Accepting connection"));
  addr_len = 0;
  if ((mSocket = usrsctp_accept(mMasterSocket, nullptr, &addr_len)) == nullptr) {
    LOG(("***Failed accept"));
    return false;
  }
  mState = OPEN;

  struct linger l;
  l.l_onoff = 1;
  l.l_linger = 0;
  if (usrsctp_setsockopt(mSocket, SOL_SOCKET, SO_LINGER,
                         (const void *)&l, (socklen_t)sizeof(struct linger)) < 0) {
    LOG(("Couldn't set SO_LINGER on SCTP socket"));
  }

  SetEvenOdd();

  // Notify Connection open
  // XXX We need to make sure connection sticks around until the message is delivered
  LOG(("%s: sending ON_CONNECTION for %p", __FUNCTION__, this));
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_CONNECTION,
             this, (DataChannel *) nullptr)));
  return true;
}

// Blocks! - Don't call this from main thread!
bool
DataChannelConnection::Connect(const char *addr, unsigned short port)
{
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;

  NS_WARNING_ASSERTION(!NS_IsMainThread(),
                       "Blocks, do not call from main thread!!!");

  /* Acting as the connector */
  LOG(("Connecting to %s, port %u", addr, port));
  memset((void *)&addr4, 0, sizeof(struct sockaddr_in));
  memset((void *)&addr6, 0, sizeof(struct sockaddr_in6));
#ifdef HAVE_SIN_LEN
  addr4.sin_len = sizeof(struct sockaddr_in);
#endif
#ifdef HAVE_SIN6_LEN
  addr6.sin6_len = sizeof(struct sockaddr_in6);
#endif
  addr4.sin_family = AF_INET;
  addr6.sin6_family = AF_INET6;
  addr4.sin_port = htons(port);
  addr6.sin6_port = htons(port);
  mState = CONNECTING;

#if !defined(__Userspace_os_Windows)
  if (inet_pton(AF_INET6, addr, &addr6.sin6_addr) == 1) {
    if (usrsctp_connect(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr6), sizeof(struct sockaddr_in6)) < 0) {
      LOG(("*** Failed userspace_connect"));
      return false;
    }
  } else if (inet_pton(AF_INET, addr, &addr4.sin_addr) == 1) {
    if (usrsctp_connect(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr4), sizeof(struct sockaddr_in)) < 0) {
      LOG(("*** Failed userspace_connect"));
      return false;
    }
  } else {
    LOG(("*** Illegal destination address."));
  }
#else
  {
    struct sockaddr_storage ss;
    int sslen = sizeof(ss);

    if (!WSAStringToAddressA(const_cast<char *>(addr), AF_INET6, nullptr, (struct sockaddr*)&ss, &sslen)) {
      addr6.sin6_addr = (reinterpret_cast<struct sockaddr_in6 *>(&ss))->sin6_addr;
      if (usrsctp_connect(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr6), sizeof(struct sockaddr_in6)) < 0) {
        LOG(("*** Failed userspace_connect"));
        return false;
      }
    } else if (!WSAStringToAddressA(const_cast<char *>(addr), AF_INET, nullptr, (struct sockaddr*)&ss, &sslen)) {
      addr4.sin_addr = (reinterpret_cast<struct sockaddr_in *>(&ss))->sin_addr;
      if (usrsctp_connect(mMasterSocket, reinterpret_cast<struct sockaddr *>(&addr4), sizeof(struct sockaddr_in)) < 0) {
        LOG(("*** Failed userspace_connect"));
        return false;
      }
    } else {
      LOG(("*** Illegal destination address."));
    }
  }
#endif

  mSocket = mMasterSocket;

  LOG(("connect() succeeded!  Entering connected mode"));
  mState = OPEN;

  SetEvenOdd();

  // Notify Connection open
  // XXX We need to make sure connection sticks around until the message is delivered
  LOG(("%s: sending ON_CONNECTION for %p", __FUNCTION__, this));
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_CONNECTION,
             this, (DataChannel *) nullptr)));
  return true;
}
#endif

DataChannel *
DataChannelConnection::FindChannelByStream(uint16_t stream)
{
  return mStreams.SafeElementAt(stream);
}

uint16_t
DataChannelConnection::FindFreeStream()
{
  uint32_t i, j, limit;

  limit = mStreams.Length();
  if (limit > MAX_NUM_STREAMS)
    limit = MAX_NUM_STREAMS;

  for (i = (mAllocateEven ? 0 : 1); i < limit; i += 2) {
    if (!mStreams[i]) {
      // Verify it's not still in the process of closing
      for (j = 0; j < mStreamsResetting.Length(); ++j) {
        if (mStreamsResetting[j] == i) {
          break;
        }
      }
      if (j == mStreamsResetting.Length())
        break;
    }
  }
  if (i >= limit) {
    return INVALID_STREAM;
  }
  return i;
}

uint32_t
DataChannelConnection::UpdateCurrentStreamIndex()
{
  if (mCurrentStream == mStreams.Length() - 1) {
      mCurrentStream = 0;
  } else {
    ++mCurrentStream;
  }

  return mCurrentStream;
}

uint32_t
DataChannelConnection::GetCurrentStreamIndex()
{
  // Fix current stream index (in case #streams decreased)
  if (mCurrentStream >= mStreams.Length()) {
    mCurrentStream = 0;
  }

  return mCurrentStream;
}

bool
DataChannelConnection::RequestMoreStreams(int32_t aNeeded)
{
  struct sctp_status status;
  struct sctp_add_streams sas;
  uint32_t outStreamsNeeded;
  socklen_t len;

  if (aNeeded + mStreams.Length() > MAX_NUM_STREAMS) {
    aNeeded = MAX_NUM_STREAMS - mStreams.Length();
  }
  if (aNeeded <= 0) {
    return false;
  }

  len = (socklen_t)sizeof(struct sctp_status);
  if (usrsctp_getsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_STATUS, &status, &len) < 0) {
    LOG(("***failed: getsockopt SCTP_STATUS"));
    return false;
  }
  outStreamsNeeded = aNeeded; // number to add

  // Note: if multiple channel opens happen when we don't have enough space,
  // we'll call RequestMoreStreams() multiple times
  memset(&sas, 0, sizeof(sas));
  sas.sas_instrms = 0;
  sas.sas_outstrms = (uint16_t)outStreamsNeeded; /* XXX error handling */
  // Doesn't block, we get an event when it succeeds or fails
  if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_ADD_STREAMS, &sas,
                         (socklen_t) sizeof(struct sctp_add_streams)) < 0) {
    if (errno == EALREADY) {
      LOG(("Already have %u output streams", outStreamsNeeded));
      return true;
    }

    LOG(("***failed: setsockopt ADD errno=%d", errno));
    return false;
  }
  LOG(("Requested %u more streams", outStreamsNeeded));
  // We add to mStreams when we get a SCTP_STREAM_CHANGE_EVENT and the
  // values are larger than mStreams.Length()
  return true;
}

// Returns a POSIX error code.
int
DataChannelConnection::SendControlMessage(const uint8_t *data, uint32_t len, uint16_t stream)
{
  struct sctp_sendv_spa info = {0};

  // General flags
  info.sendv_flags = SCTP_SEND_SNDINFO_VALID;

  // Set stream identifier, protocol identifier and flags
  info.sendv_sndinfo.snd_sid = stream;
  info.sendv_sndinfo.snd_flags = SCTP_EOR;
  info.sendv_sndinfo.snd_ppid = htonl(DATA_CHANNEL_PPID_CONTROL);

  // Create message instance and send
  // Note: Main-thread IO, but doesn't block
#if (UINT32_MAX > SIZE_MAX)
  if (len > SIZE_MAX) {
    return EMSGSIZE;
  }
#endif
  OutgoingMsg msg(info, data, (size_t)len);
  bool buffered;
  int error = SendMsgInternalOrBuffer(mBufferedControl, msg, buffered);

  // Set pending type (if buffered)
  if (!error && buffered && !mPendingType) {
    mPendingType = PENDING_DCEP;
  }
  return error;
}

// Returns a POSIX error code.
int
DataChannelConnection::SendOpenAckMessage(uint16_t stream)
{
  struct rtcweb_datachannel_ack ack;

  memset(&ack, 0, sizeof(struct rtcweb_datachannel_ack));
  ack.msg_type = DATA_CHANNEL_ACK;

  return SendControlMessage((const uint8_t *)&ack, sizeof(ack), stream);
}

// Returns a POSIX error code.
int
DataChannelConnection::SendOpenRequestMessage(const nsACString& label,
                                              const nsACString& protocol,
                                              uint16_t stream, bool unordered,
                                              uint16_t prPolicy, uint32_t prValue)
{
  const int label_len = label.Length(); // not including nul
  const int proto_len = protocol.Length(); // not including nul
  // careful - request struct include one char for the label
  const int req_size = sizeof(struct rtcweb_datachannel_open_request) - 1 +
                        label_len + proto_len;
  struct rtcweb_datachannel_open_request *req =
    (struct rtcweb_datachannel_open_request*) moz_xmalloc(req_size);

  memset(req, 0, req_size);
  req->msg_type = DATA_CHANNEL_OPEN_REQUEST;
  switch (prPolicy) {
  case SCTP_PR_SCTP_NONE:
    req->channel_type = DATA_CHANNEL_RELIABLE;
    break;
  case SCTP_PR_SCTP_TTL:
    req->channel_type = DATA_CHANNEL_PARTIAL_RELIABLE_TIMED;
    break;
  case SCTP_PR_SCTP_RTX:
    req->channel_type = DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT;
    break;
  default:
    free(req);
    return EINVAL;
  }
  if (unordered) {
    // Per the current types, all differ by 0x80 between ordered and unordered
    req->channel_type |= 0x80; // NOTE: be careful if new types are added in the future
  }

  req->reliability_param = htonl(prValue);
  req->priority = htons(0); /* XXX: add support */
  req->label_length = htons(label_len);
  req->protocol_length = htons(proto_len);
  memcpy(&req->label[0], PromiseFlatCString(label).get(), label_len);
  memcpy(&req->label[label_len], PromiseFlatCString(protocol).get(), proto_len);

  // TODO: req_size is an int... that looks hairy
  int error = SendControlMessage((const uint8_t *)req, req_size, stream);

  free(req);
  return error;
}

// XXX This should use a separate thread (outbound queue) which should
// select() to know when to *try* to send data to the socket again.
// Alternatively, it can use a timeout, but that's guaranteed to be wrong
// (just not sure in what direction).  We could re-implement NSPR's
// PR_POLL_WRITE/etc handling... with a lot of work.

// Better yet, use the SCTP stack's notifications on buffer state to avoid
// filling the SCTP's buffers.

// returns if we're still blocked (true)
bool
DataChannelConnection::SendDeferredMessages()
{
  RefPtr<DataChannel> channel; // we may null out the refs to this

  // This may block while something is modifying channels, but should not block for IO
  mLock.AssertCurrentThreadOwns();

  LOG(("SendDeferredMessages called, pending type: %d", mPendingType));
  if (!mPendingType) {
    return false;
  }

  // Send pending control messages
  // Note: If ndata is not active, check if DCEP messages are currently outstanding. These need to
  //       be sent first before other streams can be used for sending.
  if (!mBufferedControl.IsEmpty() && (mSendInterleaved || mPendingType == PENDING_DCEP)) {
    if (SendBufferedMessages(mBufferedControl)) {
      return true;
    }

    // Note: There may or may not be pending data messages
    mPendingType = PENDING_DATA;
  }

  bool blocked = false;
  uint32_t i = GetCurrentStreamIndex();
  uint32_t end = i;
  do {
    channel = mStreams[i];
    if (!channel || channel->mBufferedData.IsEmpty()) {
      i = UpdateCurrentStreamIndex();
      continue;
    }

    // Clear if closing/closed
    if (channel->mState == CLOSED || channel->mState == CLOSING) {
      channel->mBufferedData.Clear();
      i = UpdateCurrentStreamIndex();
      continue;
    }

    size_t bufferedAmount = channel->GetBufferedAmountLocked();
    size_t threshold = channel->mBufferedThreshold;
    bool wasOverThreshold = bufferedAmount >= threshold;

    // Send buffered data messages
    // Warning: This will fail in case ndata is inactive and a previously deallocated data channel
    //          has not been closed properly. If you ever see that no messages can be sent on any
    //          channel, this is likely the cause (an explicit EOR message partially sent whose
    //          remaining chunks are still being waited for).
    blocked = SendBufferedMessages(channel->mBufferedData);
    bufferedAmount = channel->GetBufferedAmountLocked();

    // can never fire with default threshold of 0
    if (wasOverThreshold && bufferedAmount < threshold) {
      LOG(("%s: sending BUFFER_LOW_THRESHOLD for %s/%s: %u", __FUNCTION__,
           channel->mLabel.get(), channel->mProtocol.get(), channel->mStream));
      Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                 DataChannelOnMessageAvailable::BUFFER_LOW_THRESHOLD,
                 this, channel)));
    }

    if (bufferedAmount == 0) {
      // buffered-to-not-buffered transition; tell the DOM code in case this makes it
      // available for GC
      LOG(("%s: sending NO_LONGER_BUFFERED for %s/%s: %u", __FUNCTION__,
           channel->mLabel.get(), channel->mProtocol.get(), channel->mStream));
      Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                 DataChannelOnMessageAvailable::NO_LONGER_BUFFERED,
                 this, channel)));
    }

    // Update current stream index
    // Note: If ndata is not active, the outstanding data messages on this stream need to be sent
    //       first before other streams can be used for sending.
    if (mSendInterleaved || !blocked) {
      i = UpdateCurrentStreamIndex();
    }
  } while (!blocked && i != end);

  if (!blocked) {
    mPendingType = mBufferedControl.IsEmpty() ? PENDING_NONE : PENDING_DCEP;
  }
  return blocked;
}


// Called with mLock locked!
// buffer MUST have at least one item!
// returns if we're still blocked (true)
bool
DataChannelConnection::SendBufferedMessages(nsTArray<nsAutoPtr<BufferedOutgoingMsg>> &buffer)
{
  do {
    // Re-send message
    int error = SendMsgInternal(*buffer[0]);
    switch (error) {
      case 0:
        buffer.RemoveElementAt(0);
        break;
      case EAGAIN:
#if (EAGAIN != EWOULDBLOCK)
      case EWOULDBLOCK:
#endif
        return true;
      default:
        buffer.RemoveElementAt(0);
        LOG(("error on sending: %d", error));
        break;
    }
  } while (!buffer.IsEmpty());

  return false;
}

// Caller must ensure that length <= SIZE_MAX
void
DataChannelConnection::HandleOpenRequestMessage(const struct rtcweb_datachannel_open_request *req,
                                                uint32_t length, uint16_t stream)
{
  RefPtr<DataChannel> channel;
  uint32_t prValue;
  uint16_t prPolicy;
  uint32_t flags;

  mLock.AssertCurrentThreadOwns();

  const size_t requiredLength =
    (sizeof(*req) - 1) + ntohs(req->label_length) + ntohs(req->protocol_length);
  if (((size_t)length) != requiredLength) {
    LOG(("%s: Inconsistent length: %u, should be %zu",
         __FUNCTION__, length, requiredLength));
    if (((size_t)length) < requiredLength)
      return;
  }

  LOG(("%s: length %u, sizeof(*req) = %zu", __FUNCTION__, length, sizeof(*req)));

  switch (req->channel_type) {
    case DATA_CHANNEL_RELIABLE:
    case DATA_CHANNEL_RELIABLE_UNORDERED:
      prPolicy = SCTP_PR_SCTP_NONE;
      break;
    case DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT:
    case DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT_UNORDERED:
      prPolicy = SCTP_PR_SCTP_RTX;
      break;
    case DATA_CHANNEL_PARTIAL_RELIABLE_TIMED:
    case DATA_CHANNEL_PARTIAL_RELIABLE_TIMED_UNORDERED:
      prPolicy = SCTP_PR_SCTP_TTL;
      break;
    default:
      LOG(("Unknown channel type %d", req->channel_type));
      /* XXX error handling */
      return;
  }
  prValue = ntohl(req->reliability_param);
  flags = (req->channel_type & 0x80) ? DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED : 0;

  if ((channel = FindChannelByStream(stream))) {
    if (!(channel->mFlags & DATA_CHANNEL_FLAGS_EXTERNAL_NEGOTIATED)) {
      LOG(("ERROR: HandleOpenRequestMessage: channel for stream %u is in state %d instead of CLOSED.",
           stream, channel->mState));
     /* XXX: some error handling */
    } else {
      LOG(("Open for externally negotiated channel %u", stream));
      // XXX should also check protocol, maybe label
      if (prPolicy != channel->mPrPolicy ||
          prValue != channel->mPrValue ||
          flags != (channel->mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED))
      {
        LOG(("WARNING: external negotiation mismatch with OpenRequest:"
             "channel %u, policy %u/%u, value %u/%u, flags %x/%x",
             stream, prPolicy, channel->mPrPolicy,
             prValue, channel->mPrValue, flags, channel->mFlags));
      }
    }
    return;
  }
  if (stream >= mStreams.Length()) {
    LOG(("%s: stream %u out of bounds (%zu)", __FUNCTION__, stream, mStreams.Length()));
    return;
  }

  nsCString label(nsDependentCSubstring(&req->label[0], ntohs(req->label_length)));
  nsCString protocol(nsDependentCSubstring(&req->label[ntohs(req->label_length)],
                                           ntohs(req->protocol_length)));

  channel = new DataChannel(this,
                            stream,
                            DataChannel::CONNECTING,
                            label,
                            protocol,
                            prPolicy, prValue,
                            flags,
                            nullptr, nullptr);
  mStreams[stream] = channel;

  channel->mState = DataChannel::WAITING_TO_OPEN;

  LOG(("%s: sending ON_CHANNEL_CREATED for %s/%s: %u (state %u)", __FUNCTION__,
       channel->mLabel.get(), channel->mProtocol.get(), stream, channel->mState));
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_CHANNEL_CREATED,
             this, channel)));

  LOG(("%s: deferring sending ON_CHANNEL_OPEN for %p", __FUNCTION__, channel.get()));

  int error = SendOpenAckMessage(stream);
  if (error) {
    LOG(("SendOpenRequest failed, error = %d", error));
    // Close the channel, inform the user
    CloseInt(channel);
    // XXX send error via DataChannelOnMessageAvailable (bug 843625)
    return;
  }

  // Now process any queued data messages for the channel (which will
  // themselves likely get queued until we leave WAITING_TO_OPEN, plus any
  // more that come in before that happens)
  DeliverQueuedData(stream);
}

// NOTE: the updated spec from the IETF says we should set in-order until we receive an ACK.
// That would make this code moot.  Keep it for now for backwards compatibility.
void
DataChannelConnection::DeliverQueuedData(uint16_t stream)
{
  mLock.AssertCurrentThreadOwns();

  uint32_t i = 0;
  while (i < mQueuedData.Length()) {
    // Careful! we may modify the array length from within the loop!
    if (mQueuedData[i]->mStream == stream) {
      LOG(("Delivering queued data for stream %u, length %u",
           stream, mQueuedData[i]->mLength));
      // Deliver the queued data
      HandleDataMessage(mQueuedData[i]->mData, mQueuedData[i]->mLength,
                        mQueuedData[i]->mPpid, mQueuedData[i]->mStream,
                        mQueuedData[i]->mFlags);
      mQueuedData.RemoveElementAt(i);
      continue; // don't bump index since we removed the element
    }
    i++;
  }
}

// Caller must ensure that length <= SIZE_MAX
void
DataChannelConnection::HandleOpenAckMessage(const struct rtcweb_datachannel_ack *ack,
                                            uint32_t length, uint16_t stream)
{
  DataChannel *channel;

  mLock.AssertCurrentThreadOwns();

  channel = FindChannelByStream(stream);
  if (NS_WARN_IF(!channel)) {
    return;
  }

  LOG(("OpenAck received for stream %u, waiting=%d", stream,
       (channel->mFlags & DATA_CHANNEL_FLAGS_WAITING_ACK) ? 1 : 0));

  channel->mFlags &= ~DATA_CHANNEL_FLAGS_WAITING_ACK;
}

// Caller must ensure that length <= SIZE_MAX
void
DataChannelConnection::HandleUnknownMessage(uint32_t ppid, uint32_t length, uint16_t stream)
{
  /* XXX: Send an error message? */
  LOG(("unknown DataChannel message received: %u, len %u on stream %d", ppid, length, stream));
  // XXX Log to JS error console if possible
}

uint8_t
DataChannelConnection::BufferMessage(nsACString& recvBuffer, const void *data,
                                     uint32_t length, uint32_t ppid, int flags)
{
  const char *buffer = (const char *) data;
  uint8_t bufferFlags = 0;

  if ((flags & MSG_EOR) &&
      ppid != DATA_CHANNEL_PPID_BINARY_PARTIAL &&
      ppid != DATA_CHANNEL_PPID_DOMSTRING_PARTIAL) {
    bufferFlags |= DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_COMPLETE;

    // Return directly if nothing has been buffered
    if (recvBuffer.IsEmpty()) {
      return bufferFlags;
    }
  }

  // Ensure it doesn't blow up our buffer
  // TODO: Change 'WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_LOCAL' to whatever the new buffer is capable
  //       of holding.
  if (((uint64_t) recvBuffer.Length()) + ((uint64_t) length) > WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_LOCAL) {
    bufferFlags |= DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_TOO_LARGE;
    return bufferFlags;
  }

  // Copy & add to receive buffer
  recvBuffer.Append(buffer, length);
  bufferFlags |= DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED;
  return bufferFlags;
}

void
DataChannelConnection::HandleDataMessage(const void *data, size_t length, uint32_t ppid,
                                         uint16_t stream, int flags)
{
  DataChannel *channel;
  const char *buffer = (const char *) data;

  mLock.AssertCurrentThreadOwns();
  channel = FindChannelByStream(stream);

  // Note: Until we support SIZE_MAX sized messages, we need this check
#if (SIZE_MAX > UINT32_MAX)
  if (length > UINT32_MAX) {
    LOG(("DataChannel: Cannot handle message of size %zu (max=%" PRIu32 ")",
         length, UINT32_MAX));
    CloseInt(channel);
    return;
  }
#endif
  uint32_t data_length = (uint32_t)length;

  // XXX A closed channel may trip this... check
  // NOTE: the updated spec from the IETF says we should set in-order until we receive an ACK.
  // That would make this code moot.  Keep it for now for backwards compatibility.
  if (!channel) {
    // In the updated 0-RTT open case, the sender can send data immediately
    // after Open, and doesn't set the in-order bit (since we don't have a
    // response or ack).  Also, with external negotiation, data can come in
    // before we're told about the external negotiation.  We need to buffer
    // data until either a) Open comes in, if the ordering get messed up,
    // or b) the app tells us this channel was externally negotiated.  When
    // these occur, we deliver the data.

    // Since this is rare and non-performance, keep a single list of queued
    // data messages to deliver once the channel opens.
    LOG(("Queuing data for stream %u, length %u", stream, data_length));
    // Copies data
    mQueuedData.AppendElement(new QueuedDataMessage(stream, ppid, flags, data, data_length));
    return;
  }

  // Ignore incoming data in case the channel is closed
  if (channel->mState == CLOSED) {
    return;
  }

  bool is_binary = true;
  uint8_t bufferFlags;
  int32_t type;
  const char* info = "";

  if (ppid == DATA_CHANNEL_PPID_DOMSTRING_PARTIAL ||
      ppid == DATA_CHANNEL_PPID_DOMSTRING) {
    is_binary = false;
  }
  if (is_binary != channel->mIsRecvBinary && !channel->mRecvBuffer.IsEmpty()) {
    NS_WARNING("DataChannel message aborted by fragment type change!");
    // TODO: Maybe closing would be better as this is a hard to detect protocol violation?
    channel->mRecvBuffer.Truncate(0);
  }
  channel->mIsRecvBinary = is_binary;

  // Remaining chunks of previously truncated message (due to the buffer being full)?
  if (channel->mFlags & DATA_CHANNEL_FLAGS_CLOSING_TOO_LARGE) {
    LOG(("DataChannel: Ignoring partial message of length %u, buffer full and closing",
         data_length));
    // Only unblock if unordered
    if ((channel->mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED) && (flags & MSG_EOR)) {
      channel->mFlags &= ~DATA_CHANNEL_FLAGS_CLOSING_TOO_LARGE;
    }
  }

  // Buffer message until complete
  bufferFlags = BufferMessage(channel->mRecvBuffer, buffer, data_length, ppid, flags);
  if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_TOO_LARGE) {
    LOG(("DataChannel: Buffered message would become too large to handle, closing channel"));
    channel->mRecvBuffer.Truncate(0);
    channel->mFlags |= DATA_CHANNEL_FLAGS_CLOSING_TOO_LARGE;
    CloseInt(channel);
    return;
  }
  if (!(bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_COMPLETE)) {
    LOG(("DataChannel: Partial %s message of length %u (total %u) on channel id %u",
         is_binary ? "binary" : "string", data_length, channel->mRecvBuffer.Length(),
         channel->mStream));
    return; // Not ready to notify application
  }
  if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED) {
    data_length = channel->mRecvBuffer.Length();
  }

  // Complain about large messages (only complain - we can handle it)
  if (data_length > WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_LOCAL) {
    LOG(("DataChannel: Received message of length %u is > announced maximum message size (%u)",
         data_length, WEBRTC_DATACHANNEL_MAX_MESSAGE_SIZE_LOCAL));
  }

  switch (ppid) {
    case DATA_CHANNEL_PPID_DOMSTRING:
      LOG(("DataChannel: Received string message of length %u on channel %u",
           data_length, channel->mStream));
      type = DataChannelOnMessageAvailable::ON_DATA_STRING;
      if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED) {
        info = " (string fragmented)";
      }
      // else send using recvData normally

      // WebSockets checks IsUTF8() here; we can try to deliver it
      break;

    case DATA_CHANNEL_PPID_BINARY:
      LOG(("DataChannel: Received binary message of length %u on channel id %u",
           data_length, channel->mStream));
      type = DataChannelOnMessageAvailable::ON_DATA_BINARY;
      if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED) {
        info = " (binary fragmented)";
      }

      // else send using recvData normally
      break;

    default:
      NS_ERROR("Unknown data PPID");
      return;
  }

  // Notify onmessage
  LOG(("%s: sending ON_DATA_%s%s for %p", __FUNCTION__,
       (type == DataChannelOnMessageAvailable::ON_DATA_STRING) ? "STRING" : "BINARY",
       info, channel));
  if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED) {
    channel->SendOrQueue(new DataChannelOnMessageAvailable(
                           type, this, channel, channel->mRecvBuffer));
    channel->mRecvBuffer.Truncate(0);
  } else {
    nsAutoCString recvData(buffer, data_length); // copies (<64) or allocates
    channel->SendOrQueue(new DataChannelOnMessageAvailable(
                           type, this, channel, recvData));
  }
}

void
DataChannelConnection::HandleDCEPMessage(const void *buffer, size_t length, uint32_t ppid,
                                         uint16_t stream, int flags)
{
  const struct rtcweb_datachannel_open_request *req;
  const struct rtcweb_datachannel_ack *ack;

  // Note: Until we support SIZE_MAX sized messages, we need this check
#if (SIZE_MAX > UINT32_MAX)
  if (length > UINT32_MAX) {
    LOG(("DataChannel: Cannot handle message of size %zu (max=%u)", length, UINT32_MAX));
    Stop();
    return;
  }
#endif
  uint32_t data_length = (uint32_t)length;

  mLock.AssertCurrentThreadOwns();

  // Buffer message until complete
  const uint8_t bufferFlags = BufferMessage(mRecvBuffer, buffer, data_length, ppid, flags);
  if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_TOO_LARGE) {
    LOG(("DataChannel: Buffered message would become too large to handle, closing connection"));
    mRecvBuffer.Truncate(0);
    Stop();
    return;
  }
  if (!(bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_COMPLETE)) {
    LOG(("Buffered partial DCEP message of length %u", data_length));
    return;
  }
  if (bufferFlags & DATA_CHANNEL_BUFFER_MESSAGE_FLAGS_BUFFERED) {
    buffer = reinterpret_cast<const void *>(mRecvBuffer.BeginReading());
    data_length = mRecvBuffer.Length();
  }

  req = static_cast<const struct rtcweb_datachannel_open_request *>(buffer);
  LOG(("Handling DCEP message of length %u", data_length));

  // Ensure minimum message size (ack is the smallest DCEP message)
  if ((size_t)data_length < sizeof(*ack)) {
    LOG(("Ignored invalid DCEP message (too short)"));
    return;
  }

  switch (req->msg_type) {
    case DATA_CHANNEL_OPEN_REQUEST:
      // structure includes a possibly-unused char label[1] (in a packed structure)
      if (NS_WARN_IF((size_t)data_length < sizeof(*req) - 1)) {
        return;
      }

      HandleOpenRequestMessage(req, data_length, stream);
      break;
    case DATA_CHANNEL_ACK:
      // >= sizeof(*ack) checked above

      ack = static_cast<const struct rtcweb_datachannel_ack *>(buffer);
      HandleOpenAckMessage(ack, data_length, stream);
      break;
    default:
      HandleUnknownMessage(ppid, data_length, stream);
      break;
  }

  // Reset buffer
  mRecvBuffer.Truncate(0);
}

// Called with mLock locked!
void
DataChannelConnection::HandleMessage(const void *buffer, size_t length, uint32_t ppid,
                                     uint16_t stream, int flags)
{
  mLock.AssertCurrentThreadOwns();

  switch (ppid) {
    case DATA_CHANNEL_PPID_CONTROL:
      HandleDCEPMessage(buffer, length, ppid, stream, flags);
      break;
    case DATA_CHANNEL_PPID_DOMSTRING_PARTIAL:
    case DATA_CHANNEL_PPID_DOMSTRING:
    case DATA_CHANNEL_PPID_BINARY_PARTIAL:
    case DATA_CHANNEL_PPID_BINARY:
      HandleDataMessage(buffer, length, ppid, stream, flags);
      break;
    default:
      LOG(("Message of length %zu PPID %u on stream %u received (%s).",
           length, ppid, stream, (flags & MSG_EOR) ? "complete" : "partial"));
      break;
  }
}

void
DataChannelConnection::HandleAssociationChangeEvent(const struct sctp_assoc_change *sac)
{
  uint32_t i, n;

  switch (sac->sac_state) {
  case SCTP_COMM_UP:
    LOG(("Association change: SCTP_COMM_UP"));
    if (mState == CONNECTING) {
      mSocket = mMasterSocket;
      mState = OPEN;

      // Check for older Firefox by looking at the amount of incoming streams
      LOG(("Negotiated number of incoming streams: %" PRIu16, sac->sac_inbound_streams));
      if (!mMaxMessageSizeSet
          && sac->sac_inbound_streams == WEBRTC_DATACHANNEL_STREAMS_OLDER_FIREFOX) {
        LOG(("Older Firefox detected, using PPID-based fragmentation"));
        mPpidFragmentation = true;
      }

      SetEvenOdd();

      Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                 DataChannelOnMessageAvailable::ON_CONNECTION,
                 this)));
      LOG(("DTLS connect() succeeded!  Entering connected mode"));

      // Open any streams pending...
      ProcessQueuedOpens();

    } else if (mState == OPEN) {
      LOG(("DataConnection Already OPEN"));
    } else {
      LOG(("Unexpected state: %d", mState));
    }
    break;
  case SCTP_COMM_LOST:
    LOG(("Association change: SCTP_COMM_LOST"));
    // This association is toast, so also close all the channels -- from mainthread!
    Stop();
    break;
  case SCTP_RESTART:
    LOG(("Association change: SCTP_RESTART"));
    break;
  case SCTP_SHUTDOWN_COMP:
    LOG(("Association change: SCTP_SHUTDOWN_COMP"));
    Stop();
    break;
  case SCTP_CANT_STR_ASSOC:
    LOG(("Association change: SCTP_CANT_STR_ASSOC"));
    break;
  default:
    LOG(("Association change: UNKNOWN"));
    break;
  }
  LOG(("Association change: streams (in/out) = (%u/%u)",
       sac->sac_inbound_streams, sac->sac_outbound_streams));

  if (NS_WARN_IF(!sac)) {
    return;
  }

  n = sac->sac_length - sizeof(*sac);
  if ((sac->sac_state == SCTP_COMM_UP) || (sac->sac_state == SCTP_RESTART)) {
    if (n > 0) {
      for (i = 0; i < n; ++i) {
        switch (sac->sac_info[i]) {
        case SCTP_ASSOC_SUPPORTS_PR:
          LOG(("Supports: PR"));
          break;
        case SCTP_ASSOC_SUPPORTS_AUTH:
          LOG(("Supports: AUTH"));
          break;
        case SCTP_ASSOC_SUPPORTS_ASCONF:
          LOG(("Supports: ASCONF"));
          break;
        case SCTP_ASSOC_SUPPORTS_MULTIBUF:
          LOG(("Supports: MULTIBUF"));
          break;
        case SCTP_ASSOC_SUPPORTS_RE_CONFIG:
          LOG(("Supports: RE-CONFIG"));
          break;
#if defined(SCTP_ASSOC_SUPPORTS_INTERLEAVING)
        case SCTP_ASSOC_SUPPORTS_INTERLEAVING:
          LOG(("Supports: NDATA"));
          // TODO: This should probably be set earlier above in 'case SCTP_COMM_UP' but we also
          //       need this for 'SCTP_RESTART'.
          mSendInterleaved = true;
          break;
#endif
        default:
          LOG(("Supports: UNKNOWN(0x%02x)", sac->sac_info[i]));
          break;
        }
      }
    }
  } else if (((sac->sac_state == SCTP_COMM_LOST) ||
              (sac->sac_state == SCTP_CANT_STR_ASSOC)) && (n > 0)) {
    LOG(("Association: ABORT ="));
    for (i = 0; i < n; ++i) {
      LOG((" 0x%02x", sac->sac_info[i]));
    }
  }
  if ((sac->sac_state == SCTP_CANT_STR_ASSOC) ||
      (sac->sac_state == SCTP_SHUTDOWN_COMP) ||
      (sac->sac_state == SCTP_COMM_LOST)) {
    return;
  }
}

void
DataChannelConnection::HandlePeerAddressChangeEvent(const struct sctp_paddr_change *spc)
{
  const char *addr = "";
#if !defined(__Userspace_os_Windows)
  char addr_buf[INET6_ADDRSTRLEN];
  struct sockaddr_in *sin;
  struct sockaddr_in6 *sin6;
#endif

  switch (spc->spc_aaddr.ss_family) {
  case AF_INET:
#if !defined(__Userspace_os_Windows)
    sin = (struct sockaddr_in *)&spc->spc_aaddr;
    addr = inet_ntop(AF_INET, &sin->sin_addr, addr_buf, INET6_ADDRSTRLEN);
#endif
    break;
  case AF_INET6:
#if !defined(__Userspace_os_Windows)
    sin6 = (struct sockaddr_in6 *)&spc->spc_aaddr;
    addr = inet_ntop(AF_INET6, &sin6->sin6_addr, addr_buf, INET6_ADDRSTRLEN);
#endif
    break;
  case AF_CONN:
    addr = "DTLS connection";
    break;
  default:
    break;
  }
  LOG(("Peer address %s is now ", addr));
  switch (spc->spc_state) {
  case SCTP_ADDR_AVAILABLE:
    LOG(("SCTP_ADDR_AVAILABLE"));
    break;
  case SCTP_ADDR_UNREACHABLE:
    LOG(("SCTP_ADDR_UNREACHABLE"));
    break;
  case SCTP_ADDR_REMOVED:
    LOG(("SCTP_ADDR_REMOVED"));
    break;
  case SCTP_ADDR_ADDED:
    LOG(("SCTP_ADDR_ADDED"));
    break;
  case SCTP_ADDR_MADE_PRIM:
    LOG(("SCTP_ADDR_MADE_PRIM"));
    break;
  case SCTP_ADDR_CONFIRMED:
    LOG(("SCTP_ADDR_CONFIRMED"));
    break;
  default:
    LOG(("UNKNOWN"));
    break;
  }
  LOG((" (error = 0x%08x).\n", spc->spc_error));
}

void
DataChannelConnection::HandleRemoteErrorEvent(const struct sctp_remote_error *sre)
{
  size_t i, n;

  n = sre->sre_length - sizeof(struct sctp_remote_error);
  LOG(("Remote Error (error = 0x%04x): ", sre->sre_error));
  for (i = 0; i < n; ++i) {
    LOG((" 0x%02x", sre-> sre_data[i]));
  }
}

void
DataChannelConnection::HandleShutdownEvent(const struct sctp_shutdown_event *sse)
{
  LOG(("Shutdown event."));
  /* XXX: notify all channels. */
  // Attempts to actually send anything will fail
}

void
DataChannelConnection::HandleAdaptationIndication(const struct sctp_adaptation_event *sai)
{
  LOG(("Adaptation indication: %x.", sai-> sai_adaptation_ind));
}

void
DataChannelConnection::HandlePartialDeliveryEvent(const struct sctp_pdapi_event *spde)
{
  // Note: Be aware that stream and sequence number being u32 instead of u16 is
  //       a bug in the SCTP API. This may change in the future.

  LOG(("Partial delivery event: "));
  switch (spde->pdapi_indication) {
    case SCTP_PARTIAL_DELIVERY_ABORTED:
      LOG(("delivery aborted "));
      break;
    default:
      LOG(("??? "));
      break;
  }
  LOG(("(flags = %x), stream = %" PRIu32 ", sn = %" PRIu32, spde->pdapi_flags, spde->pdapi_stream,
       spde->pdapi_seq));

  // Validate stream ID
  if (spde->pdapi_stream >= UINT16_MAX) {
    LOG(("Invalid stream id in partial delivery event: %" PRIu32 "\n", spde->pdapi_stream));
    return;
  }

  // Find channel and reset buffer
  DataChannel *channel = FindChannelByStream((uint16_t)spde->pdapi_stream);
  if (channel) {
    LOG(("Abort partially delivered message of %u bytes\n", channel->mRecvBuffer.Length()));
    channel->mRecvBuffer.Truncate(0);
  }
}

void
DataChannelConnection::HandleSendFailedEvent(const struct sctp_send_failed_event *ssfe)
{
  size_t i, n;

  if (ssfe->ssfe_flags & SCTP_DATA_UNSENT) {
    LOG(("Unsent "));
  }
   if (ssfe->ssfe_flags & SCTP_DATA_SENT) {
    LOG(("Sent "));
  }
  if (ssfe->ssfe_flags & ~(SCTP_DATA_SENT | SCTP_DATA_UNSENT)) {
    LOG(("(flags = %x) ", ssfe->ssfe_flags));
  }
  LOG(("message with PPID = %u, SID = %d, flags: 0x%04x due to error = 0x%08x",
       ntohl(ssfe->ssfe_info.snd_ppid), ssfe->ssfe_info.snd_sid,
       ssfe->ssfe_info.snd_flags, ssfe->ssfe_error));
  n = ssfe->ssfe_length - sizeof(struct sctp_send_failed_event);
  for (i = 0; i < n; ++i) {
    LOG((" 0x%02x", ssfe->ssfe_data[i]));
  }
}

void
DataChannelConnection::ClearResets()
{
  // Clear all pending resets
  if (!mStreamsResetting.IsEmpty()) {
    LOG(("Clearing resets for %zu streams", mStreamsResetting.Length()));
  }

  for (uint32_t i = 0; i < mStreamsResetting.Length(); ++i) {
    RefPtr<DataChannel> channel;
    channel = FindChannelByStream(mStreamsResetting[i]);
    if (channel) {
      LOG(("Forgetting channel %u (%p) with pending reset",channel->mStream, channel.get()));
      mStreams[channel->mStream] = nullptr;
    }
  }
  mStreamsResetting.Clear();
}

void
DataChannelConnection::ResetOutgoingStream(uint16_t stream)
{
  uint32_t i;

  mLock.AssertCurrentThreadOwns();
  LOG(("Connection %p: Resetting outgoing stream %u",
       (void *) this, stream));
  // Rarely has more than a couple items and only for a short time
  for (i = 0; i < mStreamsResetting.Length(); ++i) {
    if (mStreamsResetting[i] == stream) {
      return;
    }
  }
  mStreamsResetting.AppendElement(stream);
}

void
DataChannelConnection::SendOutgoingStreamReset()
{
  struct sctp_reset_streams *srs;
  uint32_t i;
  size_t len;

  LOG(("Connection %p: Sending outgoing stream reset for %zu streams",
       (void *) this, mStreamsResetting.Length()));
  mLock.AssertCurrentThreadOwns();
  if (mStreamsResetting.IsEmpty()) {
    LOG(("No streams to reset"));
    return;
  }
  len = sizeof(sctp_assoc_t) + (2 + mStreamsResetting.Length()) * sizeof(uint16_t);
  srs = static_cast<struct sctp_reset_streams *> (moz_xmalloc(len)); // infallible malloc
  memset(srs, 0, len);
  srs->srs_flags = SCTP_STREAM_RESET_OUTGOING;
  srs->srs_number_streams = mStreamsResetting.Length();
  for (i = 0; i < mStreamsResetting.Length(); ++i) {
    srs->srs_stream_list[i] = mStreamsResetting[i];
  }
  if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_RESET_STREAMS, srs, (socklen_t)len) < 0) {
    LOG(("***failed: setsockopt RESET, errno %d", errno));
    // if errno == EALREADY, this is normal - we can't send another reset
    // with one pending.
    // When we get an incoming reset (which may be a response to our
    // outstanding one), see if we have any pending outgoing resets and
    // send them
  } else {
    mStreamsResetting.Clear();
  }
  free(srs);
}

void
DataChannelConnection::HandleStreamResetEvent(const struct sctp_stream_reset_event *strrst)
{
  uint32_t n, i;
  RefPtr<DataChannel> channel; // since we may null out the ref to the channel

  if (!(strrst->strreset_flags & SCTP_STREAM_RESET_DENIED) &&
      !(strrst->strreset_flags & SCTP_STREAM_RESET_FAILED)) {
    n = (strrst->strreset_length - sizeof(struct sctp_stream_reset_event)) / sizeof(uint16_t);
    for (i = 0; i < n; ++i) {
      if (strrst->strreset_flags & SCTP_STREAM_RESET_INCOMING_SSN) {
        channel = FindChannelByStream(strrst->strreset_stream_list[i]);
        if (channel) {
          // The other side closed the channel
          // We could be in three states:
          // 1. Normal state (input and output streams (OPEN)
          //    Notify application, send a RESET in response on our
          //    outbound channel.  Go to CLOSED
          // 2. We sent our own reset (CLOSING); either they crossed on the
          //    wire, or this is a response to our Reset.
          //    Go to CLOSED
          // 3. We've sent a open but haven't gotten a response yet (CONNECTING)
          //    I believe this is impossible, as we don't have an input stream yet.

          LOG(("Incoming: Channel %u  closed, state %d",
               channel->mStream, channel->mState));
          ASSERT_WEBRTC(channel->mState == DataChannel::OPEN ||
                        channel->mState == DataChannel::CLOSING ||
                        channel->mState == DataChannel::CONNECTING ||
                        channel->mState == DataChannel::WAITING_TO_OPEN);
          if (channel->mState == DataChannel::OPEN ||
              channel->mState == DataChannel::WAITING_TO_OPEN) {
            // Mark the stream for reset (the reset is sent below)
            ResetOutgoingStream(channel->mStream);
          }
          mStreams[channel->mStream] = nullptr;

          LOG(("Disconnected DataChannel %p from connection %p",
               (void *) channel.get(), (void *) channel->mConnection.get()));
          // This sends ON_CHANNEL_CLOSED to mainthread
          channel->StreamClosedLocked();
        } else {
          LOG(("Can't find incoming channel %d",i));
        }
      }
    }
  }

  // Process any pending resets now:
  if (!mStreamsResetting.IsEmpty()) {
    LOG(("Sending %zu pending resets", mStreamsResetting.Length()));
    SendOutgoingStreamReset();
  }
}

void
DataChannelConnection::HandleStreamChangeEvent(const struct sctp_stream_change_event *strchg)
{
  uint16_t stream;
  RefPtr<DataChannel> channel;

  if (strchg->strchange_flags == SCTP_STREAM_CHANGE_DENIED) {
    LOG(("*** Failed increasing number of streams from %zu (%u/%u)",
         mStreams.Length(),
         strchg->strchange_instrms,
         strchg->strchange_outstrms));
    // XXX FIX! notify pending opens of failure
    return;
  }
  if (strchg->strchange_instrms > mStreams.Length()) {
    LOG(("Other side increased streams from %zu to %u",
         mStreams.Length(), strchg->strchange_instrms));
  }
  if (strchg->strchange_outstrms > mStreams.Length() ||
      strchg->strchange_instrms > mStreams.Length()) {
    uint16_t old_len = mStreams.Length();
    uint16_t new_len = std::max(strchg->strchange_outstrms,
                                strchg->strchange_instrms);
    LOG(("Increasing number of streams from %u to %u - adding %u (in: %u)",
         old_len, new_len, new_len - old_len,
         strchg->strchange_instrms));
    // make sure both are the same length
    mStreams.AppendElements(new_len - old_len);
    LOG(("New length = %zu (was %d)", mStreams.Length(), old_len));
    for (size_t i = old_len; i < mStreams.Length(); ++i) {
      mStreams[i] = nullptr;
    }
    // Re-process any channels waiting for streams.
    // Linear search, but we don't increase channels often and
    // the array would only get long in case of an app error normally

    // Make sure we request enough streams if there's a big jump in streams
    // Could make a more complex API for OpenXxxFinish() and avoid this loop
    size_t num_needed = mPending.GetSize();
    LOG(("%zu of %d new streams already needed", num_needed,
         new_len - old_len));
    num_needed -= (new_len - old_len); // number we added
    if (num_needed > 0) {
      if (num_needed < 16)
        num_needed = 16;
      LOG(("Not enough new streams, asking for %zu more", num_needed));
      // TODO: parameter is an int32_t but we pass size_t
      RequestMoreStreams(num_needed);
    } else if (strchg->strchange_outstrms < strchg->strchange_instrms) {
      LOG(("Requesting %d output streams to match partner",
           strchg->strchange_instrms - strchg->strchange_outstrms));
      RequestMoreStreams(strchg->strchange_instrms - strchg->strchange_outstrms);
    }

    ProcessQueuedOpens();
  }
  // else probably not a change in # of streams

  for (uint32_t i = 0; i < mStreams.Length(); ++i) {
    channel = mStreams[i];
    if (!channel)
      continue;

    if ((channel->mState == CONNECTING) &&
        (channel->mStream == INVALID_STREAM)) {
      if ((strchg->strchange_flags & SCTP_STREAM_CHANGE_DENIED) ||
          (strchg->strchange_flags & SCTP_STREAM_CHANGE_FAILED)) {
        /* XXX: Signal to the other end. */
        channel->mState = CLOSED;
        Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                   DataChannelOnMessageAvailable::ON_CHANNEL_CLOSED, this,
                   channel)));
        // maybe fire onError (bug 843625)
      } else {
        stream = FindFreeStream();
        if (stream != INVALID_STREAM) {
          channel->mStream = stream;
          mStreams[stream] = channel;

          // Send open request
          int error = SendOpenRequestMessage(
              channel->mLabel, channel->mProtocol, channel->mStream,
              !!(channel->mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED), channel->mPrPolicy,
              channel->mPrValue);
          if (error) {
            LOG(("SendOpenRequest failed, error = %d", error));
            // Close the channel, inform the user
            mStreams[channel->mStream] = nullptr;
            channel->mState = CLOSED;
            // Don't need to reset; we didn't open it
            Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                       DataChannelOnMessageAvailable::ON_CHANNEL_CLOSED, this,
                       channel)));
          } else {
            channel->mState = OPEN;
            channel->mFlags |= DATA_CHANNEL_FLAGS_READY;
            LOG(("%s: sending ON_CHANNEL_OPEN for %p", __FUNCTION__, channel.get()));
            Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                       DataChannelOnMessageAvailable::ON_CHANNEL_OPEN, this,
                       channel)));
          }
        } else {
          /* We will not find more ... */
          break;
        }
      }
    }
  }
}

// Called with mLock locked!
void
DataChannelConnection::HandleNotification(const union sctp_notification *notif, size_t n)
{
  mLock.AssertCurrentThreadOwns();
  if (notif->sn_header.sn_length != (uint32_t)n) {
    return;
  }
  switch (notif->sn_header.sn_type) {
  case SCTP_ASSOC_CHANGE:
    HandleAssociationChangeEvent(&(notif->sn_assoc_change));
    break;
  case SCTP_PEER_ADDR_CHANGE:
    HandlePeerAddressChangeEvent(&(notif->sn_paddr_change));
    break;
  case SCTP_REMOTE_ERROR:
    HandleRemoteErrorEvent(&(notif->sn_remote_error));
    break;
  case SCTP_SHUTDOWN_EVENT:
    HandleShutdownEvent(&(notif->sn_shutdown_event));
    break;
  case SCTP_ADAPTATION_INDICATION:
    HandleAdaptationIndication(&(notif->sn_adaptation_event));
    break;
  case SCTP_AUTHENTICATION_EVENT:
    LOG(("SCTP_AUTHENTICATION_EVENT"));
    break;
  case SCTP_SENDER_DRY_EVENT:
    //LOG(("SCTP_SENDER_DRY_EVENT"));
    break;
  case SCTP_NOTIFICATIONS_STOPPED_EVENT:
    LOG(("SCTP_NOTIFICATIONS_STOPPED_EVENT"));
    break;
  case SCTP_PARTIAL_DELIVERY_EVENT:
    HandlePartialDeliveryEvent(&(notif->sn_pdapi_event));
    break;
  case SCTP_SEND_FAILED_EVENT:
    HandleSendFailedEvent(&(notif->sn_send_failed_event));
    break;
  case SCTP_STREAM_RESET_EVENT:
    HandleStreamResetEvent(&(notif->sn_strreset_event));
    break;
  case SCTP_ASSOC_RESET_EVENT:
    LOG(("SCTP_ASSOC_RESET_EVENT"));
    break;
  case SCTP_STREAM_CHANGE_EVENT:
    HandleStreamChangeEvent(&(notif->sn_strchange_event));
    break;
  default:
    LOG(("unknown SCTP event: %u", (uint32_t)notif->sn_header.sn_type));
    break;
   }
 }

int
DataChannelConnection::ReceiveCallback(struct socket* sock, void *data, size_t datalen,
                                       struct sctp_rcvinfo rcv, int flags)
{
  ASSERT_WEBRTC(!NS_IsMainThread());

  if (!data) {
    LOG(("ReceiveCallback: SCTP has finished shutting down"));
  } else {
    mLock.AssertCurrentThreadOwns();
    if (flags & MSG_NOTIFICATION) {
      HandleNotification(static_cast<union sctp_notification *>(data), datalen);
    } else {
      HandleMessage(data, datalen, ntohl(rcv.rcv_ppid), rcv.rcv_sid, flags);
    }
  }
  // sctp allocates 'data' with malloc(), and expects the receiver to free
  // it (presumably with free).
  // XXX future optimization: try to deliver messages without an internal
  // alloc/copy, and if so delay the free until later.
  free(data);
  // usrsctp defines the callback as returning an int, but doesn't use it
  return 1;
}

already_AddRefed<DataChannel>
DataChannelConnection::Open(const nsACString& label, const nsACString& protocol,
                            Type type, bool inOrder,
                            uint32_t prValue, DataChannelListener *aListener,
                            nsISupports *aContext, bool aExternalNegotiated,
                            uint16_t aStream)
{
  // aStream == INVALID_STREAM to have the protocol allocate
  uint16_t prPolicy = SCTP_PR_SCTP_NONE;
  uint32_t flags;

  LOG(("DC Open: label %s/%s, type %u, inorder %d, prValue %u, listener %p, context %p, external: %s, stream %u",
       PromiseFlatCString(label).get(), PromiseFlatCString(protocol).get(),
       type, inOrder, prValue, aListener, aContext,
       aExternalNegotiated ? "true" : "false", aStream));
  switch (type) {
    case DATA_CHANNEL_RELIABLE:
      prPolicy = SCTP_PR_SCTP_NONE;
      break;
    case DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT:
      prPolicy = SCTP_PR_SCTP_RTX;
      break;
    case DATA_CHANNEL_PARTIAL_RELIABLE_TIMED:
      prPolicy = SCTP_PR_SCTP_TTL;
      break;
    default:
      LOG(("ERROR: unsupported channel type: %u", type));
      MOZ_ASSERT(false);
      return nullptr;
  }
  if ((prPolicy == SCTP_PR_SCTP_NONE) && (prValue != 0)) {
    return nullptr;
  }

  // Don't look past currently-negotiated streams
  if (aStream != INVALID_STREAM && aStream < mStreams.Length() && mStreams[aStream]) {
    LOG(("ERROR: external negotiation of already-open channel %u", aStream));
    // XXX How do we indicate this up to the application?  Probably the
    // caller's job, but we may need to return an error code.
    return nullptr;
  }

  flags = !inOrder ? DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED : 0;

  RefPtr<DataChannel> channel(new DataChannel(this,
                                                aStream,
                                                DataChannel::CONNECTING,
                                                label, protocol,
                                                prPolicy, prValue,
                                                flags,
                                                aListener, aContext));
  if (aExternalNegotiated) {
    channel->mFlags |= DATA_CHANNEL_FLAGS_EXTERNAL_NEGOTIATED;
  }

  MutexAutoLock lock(mLock); // OpenFinish assumes this
  return OpenFinish(channel.forget());
}

// Separate routine so we can also call it to finish up from pending opens
already_AddRefed<DataChannel>
DataChannelConnection::OpenFinish(already_AddRefed<DataChannel>&& aChannel)
{
  RefPtr<DataChannel> channel(aChannel); // takes the reference passed in
  // Normally 1 reference if called from ::Open(), or 2 if called from
  // ProcessQueuedOpens() unless the DOMDataChannel was gc'd
  uint16_t stream = channel->mStream;
  bool queue = false;

  mLock.AssertCurrentThreadOwns();

  // Cases we care about:
  // Pre-negotiated:
  //    Not Open:
  //      Doesn't fit:
  //         -> change initial ask or renegotiate after open
  //      -> queue open
  //    Open:
  //      Doesn't fit:
  //         -> RequestMoreStreams && queue
  //      Does fit:
  //         -> open
  // Not negotiated:
  //    Not Open:
  //      -> queue open
  //    Open:
  //      -> Try to get a stream
  //      Doesn't fit:
  //         -> RequestMoreStreams && queue
  //      Does fit:
  //         -> open
  // So the Open cases are basically the same
  // Not Open cases are simply queue for non-negotiated, and
  // either change the initial ask or possibly renegotiate after open.

  if (mState == OPEN) {
    if (stream == INVALID_STREAM) {
      stream = FindFreeStream(); // may be INVALID_STREAM if we need more
    }
    if (stream == INVALID_STREAM || stream >= mStreams.Length()) {
      // RequestMoreStreams() limits to MAX_NUM_STREAMS -- allocate extra streams
      // to avoid going back immediately for more if the ask to N, N+1, etc
      int32_t more_needed = (stream == INVALID_STREAM) ? 16 :
                            (stream-((int32_t)mStreams.Length())) + 16;
      if (!RequestMoreStreams(more_needed)) {
        // Something bad happened... we're done
        goto request_error_cleanup;
      }
      queue = true;
    }
  } else {
    // not OPEN
    if (stream != INVALID_STREAM && stream >= mStreams.Length() &&
        mState == CLOSED) {
      // Update number of streams for init message
      struct sctp_initmsg initmsg;
      socklen_t len = sizeof(initmsg);
      int32_t total_needed = stream+16;

      memset(&initmsg, 0, sizeof(initmsg));
      if (usrsctp_getsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, &len) < 0) {
        LOG(("*** failed getsockopt SCTP_INITMSG"));
        goto request_error_cleanup;
      }
      LOG(("Setting number of SCTP streams to %u, was %u/%u", total_needed,
           initmsg.sinit_num_ostreams, initmsg.sinit_max_instreams));
      initmsg.sinit_num_ostreams  = total_needed;
      initmsg.sinit_max_instreams = MAX_NUM_STREAMS;
      if (usrsctp_setsockopt(mMasterSocket, IPPROTO_SCTP, SCTP_INITMSG, &initmsg,
                             (socklen_t)sizeof(initmsg)) < 0) {
        LOG(("*** failed setsockopt SCTP_INITMSG, errno %d", errno));
        goto request_error_cleanup;
      }

      int32_t old_len = mStreams.Length();
      mStreams.AppendElements(total_needed - old_len);
      for (int32_t i = old_len; i < total_needed; ++i) {
        mStreams[i] = nullptr;
      }
    }
    // else if state is CONNECTING, we'll just re-negotiate when OpenFinish
    // is called, if needed
    queue = true;
  }
  if (queue) {
    LOG(("Queuing channel %p (%u) to finish open", channel.get(), stream));
    // Also serves to mark we told the app
    channel->mFlags |= DATA_CHANNEL_FLAGS_FINISH_OPEN;
    // we need a ref for the nsDeQue and one to return
    DataChannel* rawChannel = channel;
    rawChannel->AddRef();
    mPending.Push(rawChannel);
    return channel.forget();
  }

  MOZ_ASSERT(stream != INVALID_STREAM);
  // just allocated (& OPEN), or externally negotiated
  mStreams[stream] = channel; // holds a reference
  channel->mStream = stream;

#ifdef TEST_QUEUED_DATA
  // It's painful to write a test for this...
  channel->mState = OPEN;
  channel->mFlags |= DATA_CHANNEL_FLAGS_READY;
  SendDataMsgInternalOrBuffer(channel, "Help me!", 8, DATA_CHANNEL_PPID_DOMSTRING);
#endif

  if (channel->mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED) {
    // Don't send unordered until this gets cleared
    channel->mFlags |= DATA_CHANNEL_FLAGS_WAITING_ACK;
  }

  if (!(channel->mFlags & DATA_CHANNEL_FLAGS_EXTERNAL_NEGOTIATED)) {
    int error = SendOpenRequestMessage(
        channel->mLabel, channel->mProtocol, stream,
        !!(channel->mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED), channel->mPrPolicy,
        channel->mPrValue);
    if (error) {
      LOG(("SendOpenRequest failed, error = %d", error));
      if (channel->mFlags & DATA_CHANNEL_FLAGS_FINISH_OPEN) {
        // We already returned the channel to the app.
        NS_ERROR("Failed to send open request");
        Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
                   DataChannelOnMessageAvailable::ON_CHANNEL_CLOSED, this,
                   channel)));
      }
      // If we haven't returned the channel yet, it will get destroyed when we exit
      // this function.
      mStreams[stream] = nullptr;
      channel->mStream = INVALID_STREAM;
      // we'll be destroying the channel
      channel->mState = CLOSED;
      return nullptr;
      /* NOTREACHED */
    }
  }
  // Either externally negotiated or we sent Open
  channel->mState = OPEN;
  channel->mFlags |= DATA_CHANNEL_FLAGS_READY;
  // FIX?  Move into DOMDataChannel?  I don't think we can send it yet here
  LOG(("%s: sending ON_CHANNEL_OPEN for %p", __FUNCTION__, channel.get()));
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_CHANNEL_OPEN, this,
             channel)));

  return channel.forget();

request_error_cleanup:
  channel->mState = CLOSED;
  if (channel->mFlags & DATA_CHANNEL_FLAGS_FINISH_OPEN) {
    // We already returned the channel to the app.
    NS_ERROR("Failed to request more streams");
    Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
               DataChannelOnMessageAvailable::ON_CHANNEL_CLOSED, this,
               channel)));
    return channel.forget();
  }
  // we'll be destroying the channel, but it never really got set up
  // Alternative would be to RUN_ON_THREAD(channel.forget(),::Destroy,...) and
  // Dispatch it to ourselves
  return nullptr;
}

// Requires mLock to be locked!
// Returns a POSIX error code directly instead of setting errno.
int
DataChannelConnection::SendMsgInternal(OutgoingMsg &msg)
{
  auto &info = msg.GetInfo().sendv_sndinfo;
  int error;

  // EOR set?
  bool eor_set = info.snd_flags & SCTP_EOR ? true : false;

  // Send until buffer is empty
  size_t left = msg.GetLeft();
  do {
    size_t length;

    // Carefully chunk the buffer
    if (left > DATA_CHANNEL_MAX_BINARY_FRAGMENT) {
      length = DATA_CHANNEL_MAX_BINARY_FRAGMENT;

      // Unset EOR flag
      info.snd_flags &= ~SCTP_EOR;
    } else {
      length = left;

      // Set EOR flag
      if (eor_set) {
        info.snd_flags |= SCTP_EOR;
      }
    }

    // Send (or try at least)
    // SCTP will return EMSGSIZE if the message is bigger than the buffer
    // size (or EAGAIN if there isn't space). However, we can avoid EMSGSIZE
    // by carefully crafting small enough message chunks.
    ssize_t written = usrsctp_sendv(
        mSocket, msg.GetData(), length, nullptr, 0,
        (void *)&msg.GetInfo(), (socklen_t)sizeof(struct sctp_sendv_spa),
        SCTP_SENDV_SPA, 0);
    if (written < 0) {
      error = errno;
      goto out;
    }
    LOG(("Sent buffer (written=%zu, len=%zu, left=%zu)",
         (size_t)written, length, left - (size_t)written));

    // TODO: Remove once resolved (https://github.com/sctplab/usrsctp/issues/132)
    if (written == 0) {
      LOG(("@tuexen: usrsctp_sendv returned 0"));
      error = EAGAIN;
      goto out;
    }

    // If not all bytes have been written, this obviously means that usrsctp's buffer is full
    // and we need to try again later.
    if ((size_t)written < length) {
      msg.Advance((size_t)written);
      error = EAGAIN;
      goto out;
    }

    // Update buffer position
    msg.Advance((size_t)written);

    // Get amount of bytes left in the buffer
    left = msg.GetLeft();
  } while (left > 0);

  // Done
  error = 0;

out:
  // Reset EOR flag
  if (eor_set) {
    info.snd_flags |= SCTP_EOR;
  }

  return error;
}

// Requires mLock to be locked!
// Returns a POSIX error code directly instead of setting errno.
// IMPORTANT: Ensure that the buffer passed is guarded by mLock!
int
DataChannelConnection::SendMsgInternalOrBuffer(nsTArray<nsAutoPtr<BufferedOutgoingMsg>> &buffer,
                                               OutgoingMsg &msg, bool &buffered)
{
  NS_WARNING_ASSERTION(msg.GetLength() > 0, "Length is 0?!");

  int error = 0;
  bool need_buffering = false;

  // Note: Main-thread IO, but doesn't block!
  // XXX FIX!  to deal with heavy overruns of JS trying to pass data in
  // (more than the buffersize) queue data onto another thread to do the
  // actual sends.  See netwerk/protocol/websocket/WebSocketChannel.cpp

  // Avoid a race between buffer-full-failure (where we have to add the
  // packet to the buffered-data queue) and the buffer-now-only-half-full
  // callback, which happens on a different thread.  Otherwise we might
  // fail here, then before we add it to the queue get the half-full
  // callback, find nothing to do, then on this thread add it to the
  // queue - which would sit there.  Also, if we later send more data, it
  // would arrive ahead of the buffered message, but if the buffer ever
  // got to 1/2 full, the message would get sent - but at a semi-random
  // time, after other data it was supposed to be in front of.

  // Must lock before empty check for similar reasons!
  mLock.AssertCurrentThreadOwns();
  if (buffer.IsEmpty() && (mSendInterleaved || !mPendingType)) {
    error = SendMsgInternal(msg);
    switch (error) {
      case 0:
        break;
      case EAGAIN:
#if (EAGAIN != EWOULDBLOCK)
      case EWOULDBLOCK:
#endif
        need_buffering = true;
        break;
      default:
        LOG(("error %d on sending", error));
        break;
    }
  } else {
    need_buffering = true;
  }

  if (need_buffering) {
    // queue data for resend!  And queue any further data for the stream until it is...
    auto *bufferedMsg = new BufferedOutgoingMsg(msg); // infallible malloc
    buffer.AppendElement(bufferedMsg); // owned by mBufferedData array
    LOG(("Queued %zu buffers (left=%zu, total=%zu)",
         buffer.Length(), msg.GetLeft(), msg.GetLength()));
    buffered = true;
    return 0;
  }

  buffered = false;
  return error;
}

// Caller must ensure that length <= UINT32_MAX
// Returns a POSIX error code.
int
DataChannelConnection::SendDataMsgInternalOrBuffer(DataChannel &channel, const uint8_t *data,
                                                   size_t len, uint32_t ppid)
{
  if (NS_WARN_IF(channel.mState != OPEN && channel.mState != CONNECTING)) {
    return EINVAL; // TODO: Find a better error code
  }

  struct sctp_sendv_spa info = {0};

  // General flags
  info.sendv_flags = SCTP_SEND_SNDINFO_VALID;

  // Set stream identifier, protocol identifier and flags
  info.sendv_sndinfo.snd_sid = channel.mStream;
  info.sendv_sndinfo.snd_flags = SCTP_EOR;
  info.sendv_sndinfo.snd_ppid = htonl(ppid);

  // Unordered?
  // To avoid problems where an in-order OPEN is lost and an
  // out-of-order data message "beats" it, require data to be in-order
  // until we get an ACK.
  if ((channel.mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED) &&
      !(channel.mFlags & DATA_CHANNEL_FLAGS_WAITING_ACK)) {
    info.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
  }

  // Partial reliability policy
  if (channel.mPrPolicy != SCTP_PR_SCTP_NONE) {
    info.sendv_prinfo.pr_policy = channel.mPrPolicy;
    info.sendv_prinfo.pr_value = channel.mPrValue;
    info.sendv_flags |= SCTP_SEND_PRINFO_VALID;
  }

  // Create message instance and send
  OutgoingMsg msg(info, data, len);
  MutexAutoLock lock(mLock);
  bool buffered;
  int error = SendMsgInternalOrBuffer(channel.mBufferedData, msg, buffered);

  // Set pending type and stream index (if buffered)
  if (!error && buffered && !mPendingType) {
    mPendingType = PENDING_DATA;
    mCurrentStream = channel.mStream;
  }
  return error;
}

// Caller must ensure that length <= UINT32_MAX
// Returns a POSIX error code.
int
DataChannelConnection::SendDataMsg(DataChannel &channel, const uint8_t *data, size_t len,
                                   uint32_t ppidPartial, uint32_t ppidFinal)
{
  // We *really* don't want to do this from main thread! - and
  // SendDataMsgInternalOrBuffer avoids blocking.

  if (mPpidFragmentation) {
    // TODO: Bug 1381136, remove this block and all other code that uses PPIDs for fragmentation
    //       and reassembly once older Firefoxes without EOR are no longer supported as target
    //       clients.

    // Use the deprecated PPID-level fragmentation if enabled. Should be enabled
    // in case we can be certain that the other peer is an older Firefox browser
    // that does support PPID-level fragmentation/reassembly.

    // PPID-level fragmentation can only be applied on reliable data channels.
    if (len > DATA_CHANNEL_MAX_BINARY_FRAGMENT &&
        channel.mPrPolicy == DATA_CHANNEL_RELIABLE &&
        !(channel.mFlags & DATA_CHANNEL_FLAGS_OUT_OF_ORDER_ALLOWED)) {
      LOG(("Sending data message (total=%zu) using deprecated PPID-based chunks", len));

      size_t left = len;
      while (left > 0) {
        // Note: For correctness, chunkLen should also consider mMaxMessageSize as minimum but as
        //       this block is going to be removed soon, I see no need for it.
        size_t chunkLen = std::min<size_t>(left, DATA_CHANNEL_MAX_BINARY_FRAGMENT);
        left -= chunkLen;
        uint32_t ppid = left > 0 ? ppidPartial : ppidFinal;

        // Send the chunk
        // Note that these might end up being deferred and queued.
        LOG(("Send chunk (len=%zu, left=%zu, total=%zu, ppid %u",
             chunkLen, left, len, ppid));
        int error = SendDataMsgInternalOrBuffer(channel, data, chunkLen, ppid);
        if (error) {
          LOG(("*** send chunk fail %d", error));
          return error;
        }

        // Update data position
        data += chunkLen;
      }

      // Sending chunks complete
      LOG(("Sent %zu chunks using deprecated PPID-based fragmentation",
           (size_t)(len+DATA_CHANNEL_MAX_BINARY_FRAGMENT-1)/DATA_CHANNEL_MAX_BINARY_FRAGMENT));
      return 0;
    }

    // Cannot do PPID-based fragmentaton on unreliable channels
    NS_WARNING_ASSERTION(len <= DATA_CHANNEL_MAX_BINARY_FRAGMENT,
                         "Sending too-large data on unreliable channel!");
  } else {
    if (mMaxMessageSize != 0 && len > mMaxMessageSize) {
      LOG(("Message rejected, too large (%zu > %" PRIu64 ")", len, mMaxMessageSize));
      return EMSGSIZE;
    }
  }

  // This will use EOR-based fragmentation if the message is too large (> 64 KiB)
  return SendDataMsgInternalOrBuffer(channel, data, len, ppidFinal);
}

class ReadBlobRunnable : public Runnable {
public:
  ReadBlobRunnable(DataChannelConnection* aConnection,
                   uint16_t aStream,
                   nsIInputStream* aBlob)
    : Runnable("ReadBlobRunnable")
    , mConnection(aConnection)
    , mStream(aStream)
    , mBlob(aBlob)
  {}

  NS_IMETHOD Run() override {
    // ReadBlob() is responsible to releasing the reference
    DataChannelConnection *self = mConnection;
    self->ReadBlob(mConnection.forget(), mStream, mBlob);
    return NS_OK;
  }

private:
  // Make sure the Connection doesn't die while there are jobs outstanding.
  // Let it die (if released by PeerConnectionImpl while we're running)
  // when we send our runnable back to MainThread.  Then ~DataChannelConnection
  // can send the IOThread to MainThread to die in a runnable, avoiding
  // unsafe event loop recursion.  Evil.
  RefPtr<DataChannelConnection> mConnection;
  uint16_t mStream;
  // Use RefCount for preventing the object is deleted when SendBlob returns.
  RefPtr<nsIInputStream> mBlob;
};

// Returns a POSIX error code.
int
DataChannelConnection::SendBlob(uint16_t stream, nsIInputStream *aBlob)
{
  DataChannel *channel = mStreams[stream];
  if (NS_WARN_IF(!channel)) {
    return EINVAL; // TODO: Find a better error code
  }

  // Spawn a thread to send the data
  if (!mInternalIOThread) {
    nsresult rv = NS_NewNamedThread("DataChannel IO",
                                    getter_AddRefs(mInternalIOThread));
    if (NS_FAILED(rv)) {
      return EINVAL; // TODO: Find a better error code
    }
  }

  mInternalIOThread->Dispatch(do_AddRef(new ReadBlobRunnable(this, stream, aBlob)), NS_DISPATCH_NORMAL);
  return 0;
}

class DataChannelBlobSendRunnable : public Runnable
{
public:
  DataChannelBlobSendRunnable(
    already_AddRefed<DataChannelConnection>& aConnection,
    uint16_t aStream)
    : Runnable("DataChannelBlobSendRunnable")
    , mConnection(aConnection)
    , mStream(aStream)
  {}

  ~DataChannelBlobSendRunnable() override
  {
    if (!NS_IsMainThread() && mConnection) {
      MOZ_ASSERT(false);
      // explicitly leak the connection if destroyed off mainthread
      Unused << mConnection.forget().take();
    }
  }

  NS_IMETHOD Run() override
  {
    ASSERT_WEBRTC(NS_IsMainThread());

    mConnection->SendBinaryMsg(mStream, mData);
    mConnection = nullptr;
    return NS_OK;
  }

  // explicitly public so we can avoid allocating twice and copying
  nsCString mData;

private:
  // Note: we can be destroyed off the target thread, so be careful not to let this
  // get Released()ed on the temp thread!
  RefPtr<DataChannelConnection> mConnection;
  uint16_t mStream;
};

void
DataChannelConnection::ReadBlob(already_AddRefed<DataChannelConnection> aThis,
                                uint16_t aStream, nsIInputStream* aBlob)
{
  // NOTE: 'aThis' has been forgotten by the caller to avoid releasing
  // it off mainthread; if PeerConnectionImpl has released then we want
  // ~DataChannelConnection() to run on MainThread

  // XXX to do this safely, we must enqueue these atomically onto the
  // output socket.  We need a sender thread(s?) to enqueue data into the
  // socket and to avoid main-thread IO that might block.  Even on a
  // background thread, we may not want to block on one stream's data.
  // I.e. run non-blocking and service multiple channels.

  // Must not let Dispatching it cause the DataChannelConnection to get
  // released on the wrong thread.  Using WrapRunnable(RefPtr<DataChannelConnection>(aThis),...
  // will occasionally cause aThis to get released on this thread.  Also, an explicit Runnable
  // lets us avoid copying the blob data an extra time.
  RefPtr<DataChannelBlobSendRunnable> runnable = new DataChannelBlobSendRunnable(aThis,
                                                                                 aStream);
  // avoid copying the blob data by passing the mData from the runnable
  if (NS_FAILED(NS_ReadInputStreamToString(aBlob, runnable->mData, -1))) {
    // Bug 966602:  Doesn't return an error to the caller via onerror.
    // We must release DataChannelConnection on MainThread to avoid issues (bug 876167)
    // aThis is now owned by the runnable; release it there
    NS_ReleaseOnMainThreadSystemGroup(
      "DataChannelBlobSendRunnable", runnable.forget());
    return;
  }
  aBlob->Close();
  Dispatch(runnable.forget());
}

void
DataChannelConnection::GetStreamIds(std::vector<uint16_t>* aStreamList)
{
  ASSERT_WEBRTC(NS_IsMainThread());
  for (uint32_t i = 0; i < mStreams.Length(); ++i) {
    if (mStreams[i]) {
      aStreamList->push_back(mStreams[i]->mStream);
    }
  }
}

// Returns a POSIX error code.
int
DataChannelConnection::SendDataMsgCommon(uint16_t stream, const nsACString &aMsg,
                                         bool isBinary)
{
  ASSERT_WEBRTC(NS_IsMainThread());
  // We really could allow this from other threads, so long as we deal with
  // asynchronosity issues with channels closing, in particular access to
  // mStreams, and issues with the association closing (access to mSocket).

  const uint8_t *data = (const uint8_t *)aMsg.BeginReading();
  uint32_t len     = aMsg.Length();
#if (UINT32_MAX > SIZE_MAX)
  if (len > SIZE_MAX) {
    return EMSGSIZE;
  }
#endif
  DataChannel *channelPtr;

  LOG(("Sending %sto stream %u: %u bytes", isBinary ? "binary " : "", stream, len));
  // XXX if we want more efficiency, translate flags once at open time
  channelPtr = mStreams[stream];
  if (NS_WARN_IF(!channelPtr)) {
    return EINVAL; // TODO: Find a better error code
  }

  auto &channel = *channelPtr;

  if (isBinary) {
    return SendDataMsg(channel, data, len,
                       DATA_CHANNEL_PPID_BINARY_PARTIAL, DATA_CHANNEL_PPID_BINARY);
  }
  return SendDataMsg(channel, data, len,
                     DATA_CHANNEL_PPID_DOMSTRING_PARTIAL, DATA_CHANNEL_PPID_DOMSTRING);
}

void
DataChannelConnection::Stop()
{
  // Note: This will call 'CloseAll' from the main thread
  Dispatch(do_AddRef(new DataChannelOnMessageAvailable(
             DataChannelOnMessageAvailable::ON_DISCONNECTED,
             this)));
}

void
DataChannelConnection::Close(DataChannel *aChannel)
{
  MutexAutoLock lock(mLock);
  CloseInt(aChannel);
}

// So we can call Close() with the lock already held
// Called from someone who holds a ref via ::Close(), or from ~DataChannel
void
DataChannelConnection::CloseInt(DataChannel *aChannel)
{
  MOZ_ASSERT(aChannel);
  RefPtr<DataChannel> channel(aChannel); // make sure it doesn't go away on us

  mLock.AssertCurrentThreadOwns();
  LOG(("Connection %p/Channel %p: Closing stream %u",
       channel->mConnection.get(), channel.get(), channel->mStream));
  // re-test since it may have closed before the lock was grabbed
  if (aChannel->mState == CLOSED || aChannel->mState == CLOSING) {
    LOG(("Channel already closing/closed (%u)", aChannel->mState));
    if (mState == CLOSED && channel->mStream != INVALID_STREAM) {
      // called from CloseAll()
      // we're not going to hang around waiting any more
      mStreams[channel->mStream] = nullptr;
    }
    return;
  }
  aChannel->mBufferedData.Clear();
  if (channel->mStream != INVALID_STREAM) {
    ResetOutgoingStream(channel->mStream);
    if (mState == CLOSED) { // called from CloseAll()
      // Let resets accumulate then send all at once in CloseAll()
      // we're not going to hang around waiting
      mStreams[channel->mStream] = nullptr;
    } else {
      SendOutgoingStreamReset();
    }
  }
  aChannel->mState = CLOSING;
  if (mState == CLOSED) {
    // we're not going to hang around waiting
    channel->StreamClosedLocked();
  }
  // At this point when we leave here, the object is a zombie held alive only by the DOM object
}

void DataChannelConnection::CloseAll()
{
  LOG(("Closing all channels (connection %p)", (void*) this));
  // Don't need to lock here

  // Make sure no more channels will be opened
  {
    MutexAutoLock lock(mLock);
    mState = CLOSED;
  }

  // Close current channels
  // If there are runnables, they hold a strong ref and keep the channel
  // and/or connection alive (even if in a CLOSED state)
  bool closed_some = false;
  for (uint32_t i = 0; i < mStreams.Length(); ++i) {
    if (mStreams[i]) {
      mStreams[i]->Close();
      closed_some = true;
    }
  }

  // Clean up any pending opens for channels
  RefPtr<DataChannel> channel;
  while (nullptr != (channel = dont_AddRef(static_cast<DataChannel *>(mPending.PopFront())))) {
    LOG(("closing pending channel %p, stream %u", channel.get(), channel->mStream));
    channel->Close(); // also releases the ref on each iteration
    closed_some = true;
  }
  // It's more efficient to let the Resets queue in shutdown and then
  // SendOutgoingStreamReset() here.
  if (closed_some) {
    MutexAutoLock lock(mLock);
    SendOutgoingStreamReset();
  }
}

DataChannel::~DataChannel()
{
  // NS_ASSERTION since this is more "I think I caught all the cases that
  // can cause this" than a true kill-the-program assertion.  If this is
  // wrong, nothing bad happens.  A worst it's a leak.
  NS_ASSERTION(mState == CLOSED || mState == CLOSING, "unexpected state in ~DataChannel");
}

void
DataChannel::Close()
{
  if (mConnection) {
    // ensure we don't get deleted
    RefPtr<DataChannelConnection> connection(mConnection);
    connection->Close(this);
  }
}

// Used when disconnecting from the DataChannelConnection
void
DataChannel::StreamClosedLocked()
{
  mConnection->mLock.AssertCurrentThreadOwns();
  ENSURE_DATACONNECTION;

  LOG(("Destroying Data channel %u", mStream));
  MOZ_ASSERT_IF(mStream != INVALID_STREAM,
                !mConnection->FindChannelByStream(mStream));
  mStream = INVALID_STREAM;
  mState = CLOSED;
  mMainThreadEventTarget->Dispatch(
    do_AddRef(new DataChannelOnMessageAvailable(
                DataChannelOnMessageAvailable::ON_CHANNEL_CLOSED,
                mConnection, this)));
  // We leave mConnection live until the DOM releases us, to avoid races
}

void
DataChannel::ReleaseConnection()
{
  ASSERT_WEBRTC(NS_IsMainThread());
  mConnection = nullptr;
}

void
DataChannel::SetListener(DataChannelListener *aListener, nsISupports *aContext)
{
  MutexAutoLock mLock(mListenerLock);
  mContext = aContext;
  mListener = aListener;
}

void
DataChannel::SendErrnoToErrorResult(int error, ErrorResult& aRv)
{
  switch (error) {
    case 0:
      break;
    case EMSGSIZE:
      aRv.Throw(NS_ERROR_DOM_TYPE_ERR);
      break;
    default:
      aRv.Throw(NS_ERROR_DOM_OPERATION_ERR);
      break;
  }
}

void
DataChannel::SendMsg(const nsACString &aMsg, ErrorResult& aRv)
{
  if (!EnsureValidStream(aRv)) {
    return;
  }

  SendErrnoToErrorResult(mConnection->SendMsg(mStream, aMsg), aRv);
}

void
DataChannel::SendBinaryMsg(const nsACString &aMsg, ErrorResult& aRv)
{
  if (!EnsureValidStream(aRv)) {
    return;
  }

  SendErrnoToErrorResult(mConnection->SendBinaryMsg(mStream, aMsg), aRv);
}

void
DataChannel::SendBinaryStream(nsIInputStream *aBlob,ErrorResult& aRv)
{
  if (!EnsureValidStream(aRv)) {
    return;
  }

  SendErrnoToErrorResult(mConnection->SendBlob(mStream, aBlob), aRv);
}

// May be called from another (i.e. Main) thread!
void
DataChannel::AppReady()
{
  ENSURE_DATACONNECTION;

  MutexAutoLock lock(mConnection->mLock);

  mFlags |= DATA_CHANNEL_FLAGS_READY;
  if (mState == WAITING_TO_OPEN) {
    mState = OPEN;
    mMainThreadEventTarget->Dispatch(
      do_AddRef(new DataChannelOnMessageAvailable(
                  DataChannelOnMessageAvailable::ON_CHANNEL_OPEN, mConnection,
                  this)));
    for (uint32_t i = 0; i < mQueuedMessages.Length(); ++i) {
      nsCOMPtr<nsIRunnable> runnable = mQueuedMessages[i];
      MOZ_ASSERT(runnable);
      mMainThreadEventTarget->Dispatch(runnable.forget());
    }
  } else {
    NS_ASSERTION(mQueuedMessages.IsEmpty(), "Shouldn't have queued messages if not WAITING_TO_OPEN");
  }
  mQueuedMessages.Clear();
  mQueuedMessages.Compact();
  // We never use it again...  We could even allocate the array in the odd
  // cases we need it.
}

size_t
DataChannel::GetBufferedAmountLocked() const
{
  size_t buffered = 0;

  for (auto &msg : mBufferedData) {
    buffered += msg->GetLeft();
  }
  // XXX Note: per Michael Tuexen, there's no way to currently get the buffered
  // amount from the SCTP stack for a single stream.  It is on their to-do
  // list, and once we import a stack with support for that, we'll need to
  // add it to what we buffer.  Also we'll need to ask for notification of a per-
  // stream buffer-low event and merge that into the handling of buffer-low
  // (the equivalent to TCP_NOTSENT_LOWAT on TCP sockets)

  return buffered;
}

uint32_t
DataChannel::GetBufferedAmountLowThreshold()
{
  return mBufferedThreshold;
}

// Never fire immediately, as it's defined to fire on transitions, not state
void
DataChannel::SetBufferedAmountLowThreshold(uint32_t aThreshold)
{
  mBufferedThreshold = aThreshold;
}

// Called with mLock locked!
void
DataChannel::SendOrQueue(DataChannelOnMessageAvailable *aMessage)
{
  if (!(mFlags & DATA_CHANNEL_FLAGS_READY) &&
      (mState == CONNECTING || mState == WAITING_TO_OPEN)) {
    mQueuedMessages.AppendElement(aMessage);
  } else {
    nsCOMPtr<nsIRunnable> runnable = aMessage;
    mMainThreadEventTarget->Dispatch(runnable.forget());
  }
}

bool
DataChannel::EnsureValidStream(ErrorResult& aRv)
{
  MOZ_ASSERT(mConnection);
  if (mConnection && mStream != INVALID_STREAM) {
    return true;
  }
  aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
  return false;
}

} // namespace mozilla
