// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/NetPacket.h"
#include "hooks/client/NetPacket_AccessToken.h"
#include "hooks/client/NetPacket_UserStats.h"
#include "hooks/client/NetPacket_ETicket.h"
#include "hooks/client/NetPacket_Manifest.h"
#include "hooks/client/NetPacket_FamilySharing.h"
#include "hooks/client/NetPacket_OnlineFix.h"
#include "hooks/client/NetPacket_SteamStub.h"
#include "hooks/client/RichPresence.h"
#include "hooks/client/NetPacket_Cloud.h"
#include "hooks/client/PacketRouter.h"
#include "runtime/CloudRedirectHost.h"
#include "runtime/Logger.h"
#include "config/LuaLoader.h"
#include "runtime/Ticket.h"
#include "runtime/ManifestFetch.h"
#include "hooks/capture/SteamCapture.h"
#include "runtime/LcFnvHash.h"
#include "hooks/Macros.h"
#include <vector>

#include <unordered_map>
#include <mutex>

// ── Packet pool instances ──────────────────────────────────────────
namespace NetPacket {
    PacketPool<true>  s_rx;
    PacketPool<false> s_tx;
}

// ── Packet layout ───────────────────────────
static bool ParsePacket(const uint8_t* data, uint32_t size,
                        EMsg& eMsg, const uint8_t*& pHdr, uint32_t& cbHdr,
                        const uint8_t*& pBody, uint32_t& cbBody) {
    eMsg = static_cast<EMsg>(0);
    cbHdr = 0;
    pHdr = nullptr;
    pBody = nullptr;
    cbBody = 0;
    if (!data || size < sizeof(MsgHdr)) return false;
    const MsgHdr* hdr = reinterpret_cast<const MsgHdr*>(data);
    if (!(hdr->eMsg & kMsgHdrProtoFlag)) return false;
    eMsg  = static_cast<EMsg>(hdr->eMsg & ~kMsgHdrProtoFlag);
    cbHdr = hdr->headerLength;
    uint32_t off = sizeof(MsgHdr) + cbHdr;
    if (off > size) return false;
    pHdr   = data + sizeof(MsgHdr);
    pBody  = data + off;
    cbBody = size - off;
    return true;
}

static std::mutex s_rxLock;
static std::mutex s_txLock;

// ── Hash constants for service dispatch ──────
constexpr uint32_t HASH_JOB_NotifyRunningApps      = LcFnvHash("FamilyGroupsClient.NotifyRunningApps#1");
constexpr uint32_t HASH_JOB_GetUserStats            = LcFnvHash("Player.GetUserStats#1");
constexpr uint32_t HASH_JOB_GetManifestRequestCode  = LcFnvHash("ContentServerDirectory.GetManifestRequestCode#1");

// ── TX service dispatch ──────────────────────
struct DispatchEntry {
    uint32_t hash;
    bool   (*handler)(const uint8_t*, uint32_t, const uint8_t*, uint32_t);
};

static constexpr DispatchEntry kTxServiceDispatch[] = {
    { HASH_JOB_GetUserStats,           NetPacket::Handlers::UserStats::HandleSend_GetUserStats },
    { HASH_JOB_GetManifestRequestCode, NetPacket::Handlers::DepotFallback::HandleSend },
};

static bool RouteTxService(const char* targetJobName,
                           const uint8_t* pBody, uint32_t cbBody,
                           const uint8_t* pHdr, uint32_t cbHdr) {
    const uint32_t hash = LcFnvHash(targetJobName);
    for (const auto& entry : kTxServiceDispatch) {
        if (entry.hash == hash) return entry.handler(pBody, cbBody, pHdr, cbHdr);
    }
    return false;
}

static void RouteOutboundDispatch(EMsg eMsg, const uint8_t* pBody, uint32_t cbBody,
                                  const uint8_t* pHdr, uint32_t cbHdr) {
    NetPacket::s_tx.PatchBody = false;
    NetPacket::s_tx.SuppressSend = false;
    switch (eMsg) {
    case k_EMsgServiceMethodCallFromClient: {
        CMsgProtoBufHeader hdr;
        if (hdr.ParseFromArray(pHdr, cbHdr) && hdr.has_target_job_name()) {
            const char* jobName = hdr.target_job_name().c_str();
            if (std::strncmp(jobName, "Cloud.", 6) == 0) {
                if (std::strcmp(jobName, "Cloud.SignalAppExitSyncDone#1") != 0 &&
                    std::strcmp(jobName, "Cloud.ClientConflictResolution#1") != 0) {
                    if (NetPacket::Handlers::Cloud::HandleSend(jobName, pBody, cbBody, pHdr, cbHdr)) {
                        NetPacket::s_tx.SuppressSend = true;
                        return;
                    }
                }
            }
            NetPacket::s_tx.PatchBody = RouteTxService(jobName, pBody, cbBody, pHdr, cbHdr);
        }
        return;
    }
    case k_EMsgClientPICSProductInfoRequest:
        NetPacket::s_tx.PatchBody = NetPacket::Handlers::AccessToken::HandleSend(pBody, cbBody);
        return;
    case k_EMsgClientGamesPlayed:
    case k_EMsgClientGamesPlayedWithDataBlob:
        RichPresence::TrackGamesPlayed(pBody, cbBody, pHdr, cbHdr);
        NetPacket::s_tx.PatchBody = NetPacket::Handlers::SteamStub::HandleSend(pBody, cbBody);
        if (!NetPacket::s_tx.PatchBody)
            NetPacket::s_tx.PatchBody = NetPacket::Handlers::OnlineFix::HandleSend(pBody, cbBody);
        return;
    case k_EMsgClientRichPresenceUpload:
        RichPresence::TrackUpload(pBody, cbBody);
        return;
    case k_EMsgClientGetUserStats:
        NetPacket::s_tx.PatchBody = NetPacket::Handlers::UserStats::HandleSend_ClientGetUserStats(pBody, cbBody);
        return;
    case k_EMsgClientStoreUserStats2:
        NetPacket::s_tx.PatchBody = NetPacket::Handlers::UserStats::HandleSend_ClientStoreUserStats2(pBody, cbBody);
        {
            AppId_t appId = RichPresence::GetPlayingApp();
            if (appId != 0)
                CloudRedirectHost::NotifyStatsStored(appId);
        }
        return;
    case k_EMsgClientGetAppOwnershipTicket:
        return;
    }
}

struct RxDispatchEntry {
    uint32_t hash;
    void (*handler)(const uint8_t*, uint32_t, const uint8_t*, uint32_t);
};

static constexpr RxDispatchEntry kRxServiceDispatch[] = {
    { HASH_JOB_GetUserStats,           NetPacket::Handlers::UserStats::HandleRecv_GetUserStatsResponse },
    { HASH_JOB_GetManifestRequestCode, NetPacket::Handlers::DepotFallback::HandleRecv },
};

static void RouteRxService(const char* targetJobName,
                           const uint8_t* pBody, uint32_t cbBody,
                           const uint8_t* pHdr, uint32_t cbHdr) {
    const uint32_t hash = LcFnvHash(targetJobName);
    if (hash == HASH_JOB_NotifyRunningApps) {
        NetPacket::Handlers::FamilySharing::ClearBody(pBody, cbBody);
        return;
    }
    for (const auto& entry : kRxServiceDispatch) {
        if (entry.hash == hash) {
            entry.handler(pHdr, cbHdr, pBody, cbBody);
            return;
        }
    }
}

static void RouteInboundDispatch(EMsg eMsg, const uint8_t* pBody, uint32_t cbBody,
                                 const uint8_t* pHdr, uint32_t cbHdr) {
    NetPacket::s_rx.PatchBody = false;
    NetPacket::s_rx.PatchHdr  = false;
    if (eMsg == k_EMsgMulti) return;

    switch (eMsg) {
    case k_EMsgServiceMethodResponse: {
        CMsgProtoBufHeader hdr;
        if (hdr.ParseFromArray(pHdr, cbHdr)) {
            if (NetPacket::Handlers::Cloud::HandleRecv(hdr, pBody, cbBody))
                return;
            if (hdr.has_target_job_name())
                RouteRxService(hdr.target_job_name().c_str(), pBody, cbBody, pHdr, cbHdr);
        }
        return;
    }
    case k_EMsgClientGetUserStatsResponse:
        NetPacket::s_rx.PatchBody = NetPacket::Handlers::UserStats::HandleRecv_ClientGetUserStatsResponse(pBody, cbBody);
        return;
    case k_EMsgClientGetAppOwnershipTicketResponse:
        return;
    case k_EMsgClientPersonaState:
    {
        uint32_t rpSize = 0;
        if (RichPresence::HandleRecv(pBody, cbBody, NetPacket::s_rx.Body, NetPacket::kBodyCap, &rpSize)) {
            NetPacket::s_rx.BodyLen = rpSize;
            NetPacket::s_rx.PatchBody = true;
        }
        return;
    }
    case k_EMsgClientSharedLibraryLockStatus:
    case k_EMsgClientSharedLibraryStopPlaying:
        NetPacket::Handlers::FamilySharing::ClearBody(pBody, cbBody);
        return;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Hooks
// ═══════════════════════════════════════════════════════════════════

LM_HOOK(RecvPkt, void*, void* pThis, CNetPacket* pPacket)
{
    if (pThis && pPacket) {
        NetPacket::Handlers::Cloud::SetRecvContext(
            pThis, pPacket->m_hConnection, pPacket->m_pubNetworkBuffer);
    }

    RichPresence::DeliverPending(
        pThis, pPacket,
        [](void* pT, CNetPacket* pP) -> bool {
            return oRecvPkt(pT, pP) != nullptr;
        });

    std::vector<uint8_t> patchedRxBuf;
    uint8_t* origData = pPacket ? pPacket->m_pubData : nullptr;
    uint32_t origSize = pPacket ? pPacket->m_cubData : 0;
    bool wasPatched = false;

    if (pPacket && pPacket->m_pubData && pPacket->m_cubData >= sizeof(MsgHdr)) {
        std::lock_guard<std::mutex> lock(s_rxLock);
        EMsg eMsg;
        const uint8_t *pBody, *pHdr;
        uint32_t cbBody, cbHdr;
        if (ParsePacket(pPacket->m_pubData, pPacket->m_cubData,
                        eMsg, pHdr, cbHdr, pBody, cbBody)) {
            NetPacket::s_rx.Shrunk = false;
            RouteInboundDispatch(eMsg, pBody, cbBody, pHdr, cbHdr);

            uint8_t* poolBuf = nullptr;
            if (NetPacket::s_rx.Shrunk && NetPacket::s_rx.PatchHdr) {
                poolBuf = NetPacket::s_rx.Replace(pPacket,
                    NetPacket::s_rx.Hdr, NetPacket::s_rx.HdrLen,
                    pBody, NetPacket::s_rx.NewBodySize);
            } else if (NetPacket::s_rx.Shrunk) {
                pPacket->m_cubData = sizeof(MsgHdr) + cbHdr + NetPacket::s_rx.NewBodySize;
            } else if (NetPacket::s_rx.PatchHdr || NetPacket::s_rx.PatchBody) {
                poolBuf = NetPacket::s_rx.Replace(pPacket,
                    NetPacket::s_rx.PatchHdr  ? NetPacket::s_rx.Hdr  : pHdr,
                    NetPacket::s_rx.PatchHdr  ? NetPacket::s_rx.HdrLen : cbHdr,
                    NetPacket::s_rx.PatchBody ? NetPacket::s_rx.Body : pBody,
                    NetPacket::s_rx.PatchBody ? NetPacket::s_rx.BodyLen : cbBody);
            }

            if (poolBuf && pPacket->m_cubData > 0) {
                // Copy into thread-private buffer before releasing s_rxLock.
                // This guarantees the payload remains immutable on this thread's call stack
                // even if concurrent incoming packets wrap around the shared pool slots.
                patchedRxBuf.assign(poolBuf, poolBuf + pPacket->m_cubData);
                pPacket->m_pubData = patchedRxBuf.data();
                wasPatched = true;
            }
        }
    }

    void* ret = oRecvPkt(pThis, pPacket);

    // Restore original packet pointers after synchronous consumption by Steam dispatcher
    if (pPacket && wasPatched) {
        pPacket->m_pubData = origData;
        pPacket->m_cubData = origSize;
    }
    return ret;
}

LM_HOOK(BBuildAndAsyncSendFrame, bool,
        void* pObject, EWebSocketOpCode eWebSocketOpCode,
        uint8_t* pubData, uint32_t cubData)
{
    if (eWebSocketOpCode != k_eWebSocketOpCode_Binary)
        return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, pubData, cubData);

    std::vector<uint8_t> patchedBuf;
    uint8_t* sendBuf = pubData;
    uint32_t sendSize = cubData;

    {
        std::lock_guard<std::mutex> lock(s_txLock);
        EMsg eMsg;
        const uint8_t *pHdr, *pBody;
        uint32_t cbHdr, cbBody;
        if (ParsePacket(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody)) {
            RouteOutboundDispatch(eMsg, pBody, cbBody, pHdr, cbHdr);
            if (NetPacket::s_tx.SuppressSend) {
                NetPacket::Handlers::Cloud::DispatchSynthesized(
                    [](void* pT, CNetPacket* pP) -> bool {
                        return oRecvPkt(pT, pP) != nullptr;
                    });
                return true;
            }
            if (NetPacket::s_tx.PatchBody) {
                uint8_t* poolBuf = NetPacket::s_tx.Build(pubData, cbHdr, pHdr,
                                                         NetPacket::s_tx.Body, NetPacket::s_tx.BodyLen,
                                                         &sendSize);
                if (poolBuf && sendSize > 0) {
                    // Copy into thread-private buffer before releasing s_txLock.
                    // This eliminates ring-buffer wrap races and TOCTOU use-after-free
                    // while oBBuildAndAsyncSendFrame is actively consuming the frame.
                    patchedBuf.assign(poolBuf, poolBuf + sendSize);
                    sendBuf = patchedBuf.data();
                } else {
                    sendBuf = pubData;
                    sendSize = cubData;
                }
            }
        }
    }

    return oBBuildAndAsyncSendFrame(pObject, eWebSocketOpCode, sendBuf, sendSize);
}

// ── PacketPool method implementations ────────
namespace NetPacket {

template<>
uint8_t* PacketPool<true>::Replace(CNetPacket* p, const uint8_t* newHdr, uint32_t cbNewHdr,
                                    const uint8_t* newBody, uint32_t cbNewBody) {
    uint32_t newSize = sizeof(MsgHdr) + cbNewHdr + cbNewBody;
    if (newSize > sizeof(Frame[0])) return nullptr;
    uint32_t slot = FrameIdx.fetch_add(1, std::memory_order_relaxed) % kPoolSlots;
    uint8_t* buf = Frame[slot];
    const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(p->m_pubData);
    MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
    out->eMsg         = orig->eMsg;
    out->headerLength = cbNewHdr;
    memcpy(buf + sizeof(MsgHdr), newHdr, cbNewHdr);
    if (cbNewBody) memcpy(buf + sizeof(MsgHdr) + cbNewHdr, newBody, cbNewBody);
    p->m_pubData = buf;
    p->m_cubData = newSize;
    return buf;
}

template<>
uint8_t* PacketPool<false>::Build(const uint8_t* pubData, uint32_t cbHdr, const uint8_t* pHdr,
                                   const uint8_t* newBody, uint32_t cbNewBody,
                                   uint32_t* pNewSize) {
    *pNewSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
    if (*pNewSize > sizeof(Frame[0])) return nullptr;
    uint32_t slot = FrameIdx.fetch_add(1, std::memory_order_relaxed) % kPoolSlots;
    uint8_t* buf = Frame[slot];
    const MsgHdr* orig = reinterpret_cast<const MsgHdr*>(pubData);
    MsgHdr* out = reinterpret_cast<MsgHdr*>(buf);
    out->eMsg         = orig->eMsg;
    out->headerLength = cbHdr;
    memcpy(buf + sizeof(MsgHdr), pHdr, cbHdr);
    memcpy(buf + sizeof(MsgHdr) + cbHdr, newBody, cbNewBody);
    return buf;
}

} // namespace NetPacket

// ═══════════════════════════════════════════════════════════════════
//  NetPacket::Install / Uninstall
// ═══════════════════════════════════════════════════════════════════
namespace NetPacket {

void Install() {
    NetPacket::Handlers::Cloud::Init();
    LM_TX_BEGIN();
    LM_INSTALL(BBuildAndAsyncSendFrame);
    LM_INSTALL(RecvPkt);
    LM_TX_COMMIT();
}

void Uninstall() {
    NetPacket::Handlers::Cloud::DrainDispatches();
    LM_TX_BEGIN();
    LM_REMOVE(BBuildAndAsyncSendFrame);
    LM_REMOVE(RecvPkt);
    LM_TX_COMMIT();
    NetPacket::Handlers::Cloud::Reset();
}

} // namespace NetPacket
