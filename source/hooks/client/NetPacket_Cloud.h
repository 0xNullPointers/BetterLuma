// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"
#include <cstdint>

struct CNetPacket;

namespace NetPacket::Handlers::Cloud {

    using RecvDispatcher_t = bool (*)(void* pThis, CNetPacket* pPacket);

    // Records the active connection context during RecvPkt so that synthetic responses
    // can be immediately dispatched back into the client without waiting for inbound traffic.
    void SetRecvContext(void* pThis, HCONNECTION hConn, uint8_t* pNetworkBuffer,
                        RecvDispatcher_t fn);

    bool HasRecvContext();

    // Handles outbound ServiceMethodCallFromClient (151) with target_job_name starting with "Cloud."
    // Returns true when CloudRedirect handled the request locally and the outbound frame should
    // be suppressed from being transmitted to Valve servers.
    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);

    // Delivers queued synthesized responses to Steam by borrowing the carrier packet
    // during RecvPkt hook execution.
    void Drain(void* pThis, CNetPacket* pCarrier,
               RecvDispatcher_t invokeOriginal);

    // Immediately delivers all queued synthesized responses to Steam on the active connection.
    // Safe against re-entrant calls when Steam issues follow-up RPCs within callback handlers.
    void DrainImmediate();

    void Reset();

} // namespace NetPacket::Handlers::Cloud
