// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include <vector>
#include "Steam/Types.h"

namespace AppTicket {

    struct OwnershipTicket {
        std::vector<uint8_t> data;
        uint32 totalSize = 0;
        uint32 appIdOffset = 16;
        uint32 steamIdOffset = 8;
        uint32 signatureOffset = 0;
        uint32 signatureSize = 128;
    };

    enum class Source { CredentialOnly, ForgeOnly, CredentialThenForge };

    std::vector<uint8_t> ReadTicketFromStore(AppId_t appId);
    std::vector<uint8_t> ReadETicketFromStore(AppId_t appId);
    bool GetTicket(AppId_t appId, OwnershipTicket& out, Source src);
    std::vector<uint8_t> ForgeFromApp7(AppId_t appId);
    std::vector<uint8_t> ForgeFromBestSource(AppId_t appId, AppId_t& sourceAppId);
    uint64_t GetSpoofSteamID(AppId_t appId);
    bool WriteTicket(AppId_t appId, const std::vector<uint8_t>& data);
    bool WriteETicket(AppId_t appId, const std::vector<uint8_t>& data);
    bool WriteSteamID(AppId_t appId, uint64_t steamId);

}
