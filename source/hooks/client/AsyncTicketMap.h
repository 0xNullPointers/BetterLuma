// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

#include <optional>

namespace AsyncTicketMap {
    void Remember(SteamAPICall_t call, AppId_t appId);
    std::optional<AppId_t> Claim(SteamAPICall_t call);
    void Forget(SteamAPICall_t call);
    void Reset();
}
