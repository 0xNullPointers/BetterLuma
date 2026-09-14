// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

namespace NetPacket::Handlers::UserStats {
    bool HandleSend_GetUserStats(const uint8_t* pBody, uint32_t cbBody,
                                 const uint8_t* pHdr, uint32_t cbHdr);
    void HandleRecv_GetUserStatsResponse(const uint8_t* pHdr, uint32_t cbHdr,
                                         const uint8_t* pBody, uint32_t cbBody);
    bool HandleSend_ClientGetUserStats(const uint8_t* pBody, uint32_t cbBody);
    bool HandleRecv_ClientGetUserStatsResponse(const uint8_t* pBody, uint32_t cbBody);
}
