// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

namespace NetPacket::Handlers::SteamStub {
    bool HandleSend(const uint8_t* pBody, uint32_t cbBody);
}
