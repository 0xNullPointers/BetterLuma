// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "steam/Structs.h"
#include "steam/Types.h"

#include <optional>
#include <string>

namespace PipeWatch {

    struct ProcessKey {
        uint32 pid = 0;
        uint64 creation = 0;

        bool IsValid() const {
            return pid != 0 && creation != 0;
        }

        bool operator==(const ProcessKey&) const = default;
    };

    struct ProcessSnapshot {
        ProcessKey key;
        std::string imagePath;
        std::string imageName;
        AppId_t appId = k_uAppIdInvalid;
        AppId_t envAppId = k_uAppIdInvalid;
        AppId_t envSteamAppId = k_uAppIdInvalid;
        AppId_t envSteamGameId = k_uAppIdInvalid;
        AppId_t envSteamOverlayGameId = k_uAppIdInvalid;
        std::string appIdSource;
        bool steamProcess = false;
        bool likelyGame = false;
        bool luaManaged = false;
        bool ownedByAccount = false;
        uint32 moduleCount = 0;
        bool steamClientModule = false;
        bool steamApiModule = false;
        bool eosSdkModule = false;
        std::string steamClientPath;
        std::string steamApiPath;
        std::string eosSdkPath;

        std::string DebugString() const;
    };

    void Reset();
    void OnHandshake(CSteamPipeClient* pipe, CUtlBuffer* pRead);
    void TouchPipe(CSteamPipeClient* pipe);

    // Restore PipeWatch interface to cdf5c42 without redundant handle-lookup extensions.
    std::optional<ProcessSnapshot> SnapshotForPipe(const CSteamPipeClient* pipe);
    AppId_t ResolveAppId(const CSteamPipeClient* pipe);
    bool IsLikelyGamePipe(const CSteamPipeClient* pipe);
    bool IsLuaManagedPipe(const CSteamPipeClient* pipe);
    bool IsAccountOwnedPipe(const CSteamPipeClient* pipe);

}
