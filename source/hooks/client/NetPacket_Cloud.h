// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"
#include <cstdint>

struct CNetPacket;

namespace NetPacket::Handlers::Cloud {

    // Handles outbound ServiceMethodCallFromClient (151) with target_job_name starting with "Cloud."
    // Returns true when CloudRedirect handled the request locally and the outbound frame should
    // be suppressed from being transmitted to Valve servers.
    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);

    // Delivers queued synthesized responses to Steam by borrowing the carrier packet
    // during RecvPkt hook execution.
    void Drain(void* pThis, CNetPacket* pCarrier,
               bool (*invokeOriginal)(void*, CNetPacket*));

    void Reset();

} // namespace NetPacket::Handlers::Cloud
