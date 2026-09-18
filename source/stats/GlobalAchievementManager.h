// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "core/entry.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace GlobalAchievementManager {

    struct AchievementEntry {
        std::string name;
        float percent = 0.0f;
    };

    // Initialize paths / state.
    void Initialize();

    // Check if global achievement data is already loaded and ready for appId.
    bool HasData(AppId_t appId);

    // Ensure data is loaded: checks memory cache, then tries disk cache synchronously,
    // and if neither exists, kicks off asynchronous background fetch.
    // Returns true if data is ready in memory.
    bool EnsureLoaded(AppId_t appId, uint64_t gameId = 0);

    // IClientUserStats interface query delegates.
    int GetMostAchievedAchievementInfo(AppId_t appId, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved);
    int GetNextMostAchievedAchievementInfo(AppId_t appId, int iPreviousAchievement, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved);
    bool GetAchievementAchievedPercent(AppId_t appId, const char* pchName, float* pflPercent);

    // Asynchronously fetch/load percentages for this game.
    // Handles memory cache, disk cache (<Steam>\betterluma\cache\achievements\<appId>.json),
    // and live Web API fetch via RuntimeHttp::Get.
    void RequestPercentagesAsync(AppId_t appId, uint64_t gameId);

    // Flush and reset state on shutdown.
    void Shutdown();

} // namespace GlobalAchievementManager
