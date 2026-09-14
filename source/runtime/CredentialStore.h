// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include "Steam/Types.h"
#include <string>
#include <vector>

// Abstraction over the registry-backed Steam credential store.
// LumaCore keeps tickets and SteamIDs in HKCU\Software\Valve\Steam\Apps\<AppId>
// which mirrors what Steam's own wrapper reads. The Status enum gives callers
// a consistent error surface no matter which backing store we end up talking to.

namespace CredentialStore {

    enum class Status { Ok, NotFound, AccessDenied, Error };
    const char* ToString(Status s);

    Status ReadSteamId(AppId_t appId, uint64_t& outSteamId);
    Status WriteSteamId(AppId_t appId, uint64_t steamId);

    Status ReadTicket(AppId_t appId, std::vector<uint8_t>& out);
    Status WriteTicket(AppId_t appId, const std::vector<uint8_t>& data);

    Status ReadETicket(AppId_t appId, std::vector<uint8_t>& out);
    Status WriteETicket(AppId_t appId, const std::vector<uint8_t>& data);

    struct ActiveUser {
        uint32_t accountId = 0;
        std::wstring universe;
    };
    Status GetActiveUser(ActiveUser& out);

}
