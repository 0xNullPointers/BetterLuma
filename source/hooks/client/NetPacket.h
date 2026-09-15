// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include "steam_messages.pb.h"

struct CNetPacket;
struct MsgHdr;

namespace NetPacket {

// ── Packet pool constants (shared across all NetPacket handlers) ──
inline constexpr uint32_t kBodyCap   = 245760;
inline constexpr uint32_t kHdrCap    = 2048;
inline constexpr uint32_t kPktCap    = 8 + kHdrCap + kBodyCap;
inline constexpr int      kPoolSlots = 12;

// ── Packet pool (ring buffer) ──────────────────────────────────────
template<bool IsRx>
class PacketPool {
public:
    uint8_t Body[kBodyCap];
    uint32_t BodyLen   = 0;
    uint8_t* BodyPtr   = nullptr;
    uint8_t  Hdr[kHdrCap];
    uint32_t HdrLen    = 0;
    uint8_t* HdrPtr    = nullptr;
    bool     PatchBody = false;
    bool     PatchHdr  = false;
    bool     Shrunk    = false;
    bool     SuppressSend = false;
    uint32_t NewBodySize = 0;

    uint8_t               Frame[kPoolSlots][kPktCap];
    std::atomic<uint32_t> FrameIdx{0};

    uint8_t* Replace(CNetPacket* p, const uint8_t* newHdr, uint32_t cbNewHdr,
                     const uint8_t* newBody, uint32_t cbNewBody);
    uint8_t* Build(const uint8_t* pubData, uint32_t cbHdr, const uint8_t* pHdr,
                   const uint8_t* newBody, uint32_t cbNewBody,
                   uint32_t* pNewSize);
};

extern PacketPool<true>  s_rx;
extern PacketPool<false> s_tx;

void RouteOutbound(EMsg eMsg, const uint8_t* pBody, uint32_t cbBody,
                   const uint8_t* pHdr, uint32_t cbHdr);
void RouteInbound(EMsg eMsg, const uint8_t* pBody, uint32_t cbBody,
                  const uint8_t* pHdr, uint32_t cbHdr);

void Install();
void Uninstall();

// ── Handler namespaces (implemented in NetPacket_*.cpp) ──────────
namespace Handlers::AccessToken {
    bool HandleSend(const uint8_t* pBody, uint32_t cbBody);
}

namespace Handlers::UserStats {
    bool HandleSend_GetUserStats(const uint8_t* pBody, uint32_t cbBody,
                                 const uint8_t* pHdr, uint32_t cbHdr);
    void HandleRecv_GetUserStatsResponse(const uint8_t* pHdr, uint32_t cbHdr,
                                         const uint8_t* pBody, uint32_t cbBody);
    bool HandleSend_ClientGetUserStats(const uint8_t* pBody, uint32_t cbBody);
    bool HandleRecv_ClientGetUserStatsResponse(const uint8_t* pBody, uint32_t cbBody);
    bool HandleSend_ClientStoreUserStats2(const uint8_t* pBody, uint32_t cbBody);
}

namespace Handlers::ETicket {
    void HandleEncryptedAppTicketResponse(const uint8_t* pBody, uint32_t cbBody);
}

namespace Handlers::DepotFallback {
    bool HandleSend(const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);
    void HandleRecv(const uint8_t* pHdr, uint32_t cbHdr,
                    const uint8_t* pBody, uint32_t cbBody);
}

namespace Handlers::FamilySharing {
    void ClearBody(const uint8_t*, uint32_t);
}

namespace Handlers::OnlineFix {
    bool HandleSend(const uint8_t* pBody, uint32_t cbBody);
}

namespace Handlers::Cloud {
    using RecvDispatcher_t = bool (*)(void* pThis, CNetPacket* pPacket);
    void SetRecvContext(void* pThis, HCONNECTION hConn, uint8_t* pNetworkBuffer);
    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);
    void DispatchSynthesized(RecvDispatcher_t dispatch);
    void DrainDispatches();
    void Init();
    bool HandleRecv(const CMsgProtoBufHeader& hdr,
                    const uint8_t* pBody, uint32_t cbBody);
    void Reset();
}

} // namespace NetPacket
