// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"
#include <cstdint>

class CMsgProtoBufHeader;

namespace NetPacket::Handlers::Cloud {

    // Handles outbound ServiceMethodCallFromClient (151) with target_job_name starting with "Cloud."
    // Pre-synthesizes the CloudRedirect response and records it mapped to the request's job ID.
    // Returns true when CloudRedirect handled the request locally (outbound frame continues
    // to Valve CM to elicit an inbound response packet carrier).
    bool HandleSend(const char* jobName,
                    const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);

    // Intercepts inbound ServiceMethodResponse. If the incoming response corresponds
    // to an intercepted Cloud RPC, replaces the packet payload in NetPacket::s_rx
    // with the synthesized response and returns true.
    bool HandleRecv(const CMsgProtoBufHeader& hdr,
                    const uint8_t* pBody, uint32_t cbBody);

    void Reset();

} // namespace NetPacket::Handlers::Cloud
