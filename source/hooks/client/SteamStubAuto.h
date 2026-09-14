// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "core/entry.h"

#include <string_view>

namespace SteamStubAuto {
    bool ShouldActivate(AppId_t appId, bool hasDepot, bool owned,
                        bool hasManualFlag, bool detectedSteamStub);

    void Arm(AppId_t realAppId, const char* exePath, std::string_view detectedImagePath = {});
    void Clear();
    bool IsActive();
    AppId_t RealAppId();

    AppId_t ResolveForImage(std::string_view imageName, AppId_t envAppId);
}
