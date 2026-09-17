// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "core/entry.h"

#include <optional>

namespace PendingCallMap {

    void RecordEncryptedTicket(SteamAPICall_t call, AppId_t appID);
    std::optional<AppId_t> TakeEncryptedTicket(SteamAPICall_t call);

}
