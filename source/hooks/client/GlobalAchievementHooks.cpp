// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/GlobalAchievementHooks.h"
#include "hooks/client/StringFind.h"
#include "hooks/capture/RuntimeCapture.h"
#include "hooks/Macros.h"
#include "core/entry.h"
#include "stats/GlobalAchievementManager.h"
#include "config/LuaLoader.h"
#include "runtime/Logger.h"
#include "runtime/HookStatus.h"

#include <atomic>

namespace {

    using RequestGlobalAchievementPercentages_t = SteamAPICall_t(__fastcall*)(void* pThis, const uint64_t* pGameID);
    using GetMostAchievedAchievementInfo_t = int(__fastcall*)(void* pThis, const uint64_t* pGameID, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved);
    using GetNextMostAchievedAchievementInfo_t = int(__fastcall*)(void* pThis, const uint64_t* pGameID, int iPreviousAchievement, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved);
    using GetAchievementAchievedPercent_t = bool(__fastcall*)(void* pThis, const uint64_t* pGameID, const char* pchName, float* pflPercent);

    RequestGlobalAchievementPercentages_t oRequestGlobalAchievementPercentages = nullptr;
    GetMostAchievedAchievementInfo_t oGetMostAchievedAchievementInfo = nullptr;
    GetNextMostAchievedAchievementInfo_t oGetNextMostAchievedAchievementInfo = nullptr;
    GetAchievementAchievedPercent_t oGetAchievementAchievedPercent = nullptr;

    std::atomic<bool> g_installed{false};

    static AppId_t ResolveEffectiveAppId(AppId_t appId) {
        if (appId == kOnlineFixAppId || appId == 0 || SteamCapture::IsOnlineFixApp(appId)) {
            AppId_t real = SteamCapture::ActiveRouteRealAppId();
            if (!real) {
                real = SteamCapture::ResolveAppId();
            }
            if (!real || real == kOnlineFixAppId) {
                auto apps = SteamCapture::GetActiveOnlineFixApps();
                if (apps.size() == 1) {
                    real = apps.front();
                }
            }
            if (real != 0 && real != kOnlineFixAppId) {
                return real;
            }
        }
        return appId;
    }

    static uint64_t MakeEffectiveGameId(uint64_t gameId, AppId_t effectiveAppId) {
        AppId_t origAppId = static_cast<AppId_t>(gameId & 0xFFFFFF);
        if (effectiveAppId != 0 && effectiveAppId != origAppId) {
            return (gameId & ~0xFFFFFFULL) | static_cast<uint64_t>(effectiveAppId);
        }
        return gameId;
    }

    SteamAPICall_t __fastcall hkRequestGlobalAchievementPercentages(void* pThis, const uint64_t* pGameID) {
        if (!pGameID) {
            return oRequestGlobalAchievementPercentages ? oRequestGlobalAchievementPercentages(pThis, pGameID) : k_uAPICallInvalid;
        }

        uint64_t gameId = *pGameID;
        AppId_t appId = static_cast<AppId_t>(gameId & 0xFFFFFF);
        AppId_t effectiveAppId = ResolveEffectiveAppId(appId);
        uint64_t effectiveGameId = MakeEffectiveGameId(gameId, effectiveAppId);

        LOG_ACHIEVEMENTCH_INFO("RequestGlobalAchievementPercentages called: gameId=0x{:016X} appId={} effectiveAppId={}",
                               gameId, appId, effectiveAppId);

        if (effectiveAppId != 0) {
            GlobalAchievementManager::EnsureLoaded(effectiveAppId, effectiveGameId);
        }

        SteamAPICall_t call = k_uAPICallInvalid;
        if (oRequestGlobalAchievementPercentages) {
            call = oRequestGlobalAchievementPercentages(pThis, &effectiveGameId);
        }

        LOG_ACHIEVEMENTCH_INFO("Native RequestGlobalAchievementPercentages returned 0x{:016X} for appId={}", call, effectiveAppId);
        return call;
    }

    int __fastcall hkGetMostAchievedAchievementInfo(void* pThis, const uint64_t* pGameID, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved) {
        if (pGameID) {
            AppId_t appId = static_cast<AppId_t>(*pGameID & 0xFFFFFF);
            AppId_t effectiveAppId = ResolveEffectiveAppId(appId);
            uint64_t effectiveGameId = MakeEffectiveGameId(*pGameID, effectiveAppId);

            if (GlobalAchievementManager::HasData(effectiveAppId) || GlobalAchievementManager::EnsureLoaded(effectiveAppId, effectiveGameId)) {
                int res = GlobalAchievementManager::GetMostAchievedAchievementInfo(effectiveAppId, pchName, unNameBufLen, pflPercent, pbAchieved);
                if (res != -1) {
                    return res;
                }
            }

            if (oGetMostAchievedAchievementInfo) {
                return oGetMostAchievedAchievementInfo(pThis, &effectiveGameId, pchName, unNameBufLen, pflPercent, pbAchieved);
            }
        }
        return -1;
    }

    int __fastcall hkGetNextMostAchievedAchievementInfo(void* pThis, const uint64_t* pGameID, int iPreviousAchievement, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved) {
        if (pGameID) {
            AppId_t appId = static_cast<AppId_t>(*pGameID & 0xFFFFFF);
            AppId_t effectiveAppId = ResolveEffectiveAppId(appId);
            uint64_t effectiveGameId = MakeEffectiveGameId(*pGameID, effectiveAppId);

            if (GlobalAchievementManager::HasData(effectiveAppId) || GlobalAchievementManager::EnsureLoaded(effectiveAppId, effectiveGameId)) {
                int res = GlobalAchievementManager::GetNextMostAchievedAchievementInfo(effectiveAppId, iPreviousAchievement, pchName, unNameBufLen, pflPercent, pbAchieved);
                if (res != -1) {
                    return res;
                }
            }

            if (oGetNextMostAchievedAchievementInfo) {
                return oGetNextMostAchievedAchievementInfo(pThis, &effectiveGameId, iPreviousAchievement, pchName, unNameBufLen, pflPercent, pbAchieved);
            }
        }
        return -1;
    }

    bool __fastcall hkGetAchievementAchievedPercent(void* pThis, const uint64_t* pGameID, const char* pchName, float* pflPercent) {
        if (pGameID && pchName) {
            AppId_t appId = static_cast<AppId_t>(*pGameID & 0xFFFFFF);
            AppId_t effectiveAppId = ResolveEffectiveAppId(appId);
            uint64_t effectiveGameId = MakeEffectiveGameId(*pGameID, effectiveAppId);

            if (GlobalAchievementManager::HasData(effectiveAppId) || GlobalAchievementManager::EnsureLoaded(effectiveAppId, effectiveGameId)) {
                if (GlobalAchievementManager::GetAchievementAchievedPercent(effectiveAppId, pchName, pflPercent)) {
                    return true;
                }
            }

            if (oGetAchievementAchievedPercent) {
                return oGetAchievementAchievedPercent(pThis, &effectiveGameId, pchName, pflPercent);
            }
        }
        return false;
    }

} // namespace

namespace GlobalAchievementHooks {

    void Install() {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        GlobalAchievementManager::Initialize();

        void* pRequest = StringFind::FindFunction(diversion_hModule, "CAPIJobRequestGlobalAchievementPercentages");
        void* pMost = StringFind::FindFunction(diversion_hModule, "GetMostAchievedAchievementInfo() failed, stats are not loaded for game %llu. Call RequestCurrentStats() and RequestGlobalAchievementPercentages() first.\n");
        void* pNext = StringFind::FindFunction(diversion_hModule, "GetNextMostAchievedAchievementInfo() failed, stats are not loaded for game %llu. Call RequestCurrentStats() and RequestGlobalAchievementPercentages() first.\n");
        void* pPercent = StringFind::FindFunction(diversion_hModule, "GetAchievementAchievedPercent() failed, stats are not loaded for game %llu. Call RequestCurrentStats() and RequestGlobalAchievementPercentages() first.\n");

        LM_TX_BEGIN();
        if (pRequest) {
            oRequestGlobalAchievementPercentages = reinterpret_cast<RequestGlobalAchievementPercentages_t>(pRequest);
            DetourAttach(reinterpret_cast<PVOID*>(&oRequestGlobalAchievementPercentages), reinterpret_cast<PVOID>(hkRequestGlobalAchievementPercentages));
            LOG_ACHIEVEMENTCH_INFO("Hook: RequestGlobalAchievementPercentages attached @ 0x{:X}", reinterpret_cast<uintptr_t>(pRequest));
            HookStatus::RecordInstalled();
        } else {
            LOG_ACHIEVEMENTCH_WARN("Hook: RequestGlobalAchievementPercentages string-xref missed");
            HookStatus::RecordMissed("RequestGlobalAchievementPercentages");
        }

        if (pMost) {
            oGetMostAchievedAchievementInfo = reinterpret_cast<GetMostAchievedAchievementInfo_t>(pMost);
            DetourAttach(reinterpret_cast<PVOID*>(&oGetMostAchievedAchievementInfo), reinterpret_cast<PVOID>(hkGetMostAchievedAchievementInfo));
            LOG_ACHIEVEMENTCH_INFO("Hook: GetMostAchievedAchievementInfo attached @ 0x{:X}", reinterpret_cast<uintptr_t>(pMost));
            HookStatus::RecordInstalled();
        } else {
            LOG_ACHIEVEMENTCH_WARN("Hook: GetMostAchievedAchievementInfo string-xref missed");
            HookStatus::RecordMissed("GetMostAchievedAchievementInfo");
        }

        if (pNext) {
            oGetNextMostAchievedAchievementInfo = reinterpret_cast<GetNextMostAchievedAchievementInfo_t>(pNext);
            DetourAttach(reinterpret_cast<PVOID*>(&oGetNextMostAchievedAchievementInfo), reinterpret_cast<PVOID>(hkGetNextMostAchievedAchievementInfo));
            LOG_ACHIEVEMENTCH_INFO("Hook: GetNextMostAchievedAchievementInfo attached @ 0x{:X}", reinterpret_cast<uintptr_t>(pNext));
            HookStatus::RecordInstalled();
        } else {
            LOG_ACHIEVEMENTCH_WARN("Hook: GetNextMostAchievedAchievementInfo string-xref missed");
            HookStatus::RecordMissed("GetNextMostAchievedAchievementInfo");
        }

        if (pPercent) {
            oGetAchievementAchievedPercent = reinterpret_cast<GetAchievementAchievedPercent_t>(pPercent);
            DetourAttach(reinterpret_cast<PVOID*>(&oGetAchievementAchievedPercent), reinterpret_cast<PVOID>(hkGetAchievementAchievedPercent));
            LOG_ACHIEVEMENTCH_INFO("Hook: GetAchievementAchievedPercent attached @ 0x{:X}", reinterpret_cast<uintptr_t>(pPercent));
            HookStatus::RecordInstalled();
        } else {
            LOG_ACHIEVEMENTCH_WARN("Hook: GetAchievementAchievedPercent string-xref missed");
            HookStatus::RecordMissed("GetAchievementAchievedPercent");
        }
        LM_TX_COMMIT();
    }

    void Uninstall() {
        if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        LM_TX_BEGIN();
        if (oRequestGlobalAchievementPercentages) {
            DetourDetach(reinterpret_cast<PVOID*>(&oRequestGlobalAchievementPercentages), reinterpret_cast<PVOID>(hkRequestGlobalAchievementPercentages));
            oRequestGlobalAchievementPercentages = nullptr;
        }
        if (oGetMostAchievedAchievementInfo) {
            DetourDetach(reinterpret_cast<PVOID*>(&oGetMostAchievedAchievementInfo), reinterpret_cast<PVOID>(hkGetMostAchievedAchievementInfo));
            oGetMostAchievedAchievementInfo = nullptr;
        }
        if (oGetNextMostAchievedAchievementInfo) {
            DetourDetach(reinterpret_cast<PVOID*>(&oGetNextMostAchievedAchievementInfo), reinterpret_cast<PVOID>(hkGetNextMostAchievedAchievementInfo));
            oGetNextMostAchievedAchievementInfo = nullptr;
        }
        if (oGetAchievementAchievedPercent) {
            DetourDetach(reinterpret_cast<PVOID*>(&oGetAchievementAchievedPercent), reinterpret_cast<PVOID>(hkGetAchievementAchievedPercent));
            oGetAchievementAchievedPercent = nullptr;
        }
        LM_TX_COMMIT();

        GlobalAchievementManager::Shutdown();
    }

} // namespace GlobalAchievementHooks
