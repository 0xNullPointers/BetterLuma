// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <string>

namespace ProtectionProbe {
    struct ScanResult {
        bool valid = false;
        bool detected = false;
        bool routeAccepted = false;
        std::string method;
        std::string routeReason;
        std::string imagePath;
        std::uint64_t fileSize = 0;
        std::uint64_t bytesScanned = 0;
        std::uint64_t candidates = 0;
    };

    ScanResult ScanOnce(uint32 pid, uint64 creation, AppId_t appId, const std::string& imagePath);
    ScanResult ScanBeforeSpawn(AppId_t appId, const std::string& imagePath);
}
