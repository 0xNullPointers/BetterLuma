// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"
#include <cstdint>

struct CNetPacket;
class CMsgProtoBufHeader;

namespace NetPacket::Handlers::Cloud {

    using RecvDispatcher_t = bool (*)(void* pThis, CNetPacket* pPacket);

    // Records the active connection context during RecvPkt so that synthetic responses
    // can be dispatched directly into Steam's receive handler.
    void SetRecvContext(void* pThis, HCONNECTION hConn, uint8_t* pNetworkBuffer);

    // Handles outbound ServiceMethodCallFromClient (151) with target_job_name starting with "Cloud."
    // Pre-synthesizes the CloudRedirect response, serializes into full wire format, and stages it.
    // Returns true when CloudRedirect handled the request locally and the outbound frame should
    // be suppressed from being transmitted to Valve CM servers.
    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);

    // Asynchronously dispatches staged synthesized responses directly to oRecvPkt
    // via a thread pool work item after a 1ms yield, preventing re-entrancy transport deadlocks.
    void DispatchSynthesized(RecvDispatcher_t dispatch);

    // Drains any in-flight background dispatches before unhooking or unloading.
    void DrainDispatches();

    // Initializes dispatch state.
    void Init();

    // Intercepts inbound ServiceMethodResponse. If the incoming response corresponds
    // to an intercepted Cloud RPC, replaces the packet payload in NetPacket::s_rx
    // with the synthesized response and returns true.
    bool HandleRecv(const CMsgProtoBufHeader& hdr,
                    const uint8_t* pBody, uint32_t cbBody);

    void Reset();

} // namespace NetPacket::Handlers::Cloud
