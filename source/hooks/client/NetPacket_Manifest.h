// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

namespace NetPacket::Handlers::DepotFallback {
    bool HandleSend(const uint8_t* pBody, uint32_t cbBody,
                    const uint8_t* pHdr, uint32_t cbHdr);
    void HandleRecv(const uint8_t* pHdr, uint32_t cbHdr,
                    const uint8_t* pBody, uint32_t cbBody);
}
