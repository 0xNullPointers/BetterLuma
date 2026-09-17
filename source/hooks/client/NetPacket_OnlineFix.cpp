// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/NetPacket.h"
#include "hooks/capture/SteamCapture.h"
#include "config/LuaLoader.h"
#include "core/entry.h"
#include "runtime/Logger.h"

namespace NetPacket::Handlers::OnlineFix {

bool HandleSend(const uint8_t* pBody, uint32_t cbBody) {
    // Support multi-game OnlineFix presence without premature teardown.
    if (!SteamCapture::HasActiveOnlineFixApps())
        return false;

    CMsgClientGamesPlayed msg;
    if (!msg.ParseFromArray(pBody, cbBody)) {
        LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"parse-fail\"}}");
        return false;
    }
    LOG_PKTRT_DEBUG("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"original\":{}}}", msg.DebugString());

    std::vector<const CMsgClientGamesPlayed::GamePlayed*> activeOnlineFixGames;
    CMsgClientGamesPlayed::GamePlayed* fixGame = nullptr;

    // Track active OnlineFix applications and guard against race-condition unregistration during game launch.
    for (int i = 0; i < msg.games_played_size(); ++i) {
        auto* game = msg.mutable_games_played(i);
        AppId_t appid = static_cast<AppId_t>(game->game_id() & UINT32_MAX);
        if (SteamCapture::IsOnlineFixApp(appid)) {
            activeOnlineFixGames.push_back(game);
            SteamCapture::MarkOnlineFixAppSeen(appid);
            LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"detect_active_game\",\"appId\":{},\"pid\":{}}}",
                           appid, game->has_process_id() ? game->process_id() : 0);
        } else if (appid == kOnlineFixAppId) {
            fixGame = game;
        }
    }

    bool patched = false;
    if (!activeOnlineFixGames.empty()) {
        // At least one OnlineFix game is currently running in Steam.
        const auto* primaryGame = activeOnlineFixGames.back();
        AppId_t primaryAppId = static_cast<AppId_t>(primaryGame->game_id() & UINT32_MAX);
        std::string name = SteamCapture::GetGameNameByAppID(primaryAppId);

        if (fixGame == nullptr) {
            fixGame = msg.add_games_played();
            fixGame->CopyFrom(*primaryGame);
            fixGame->set_game_id(kOnlineFixAppId);
            if (!name.empty()) {
                fixGame->set_game_extra_info(name);
            }
            patched = true;
            LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"add_480_presence\",\"name\":\"{}\",\"appId\":{},\"pid\":{},\"totalActive\":{}}}",
                           name, primaryAppId, primaryGame->has_process_id() ? primaryGame->process_id() : 0,
                           activeOnlineFixGames.size());
        } else if (!name.empty() && fixGame->game_extra_info() != name) {
            fixGame->set_game_extra_info(name);
            patched = true;
            LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"update_480_presence\",\"name\":\"{}\",\"appId\":{}}}",
                           name, primaryAppId);
        }

        // Clean up registered OnlineFix apps that have legitimately exited
        auto registeredApps = SteamCapture::GetActiveOnlineFixApps();
        for (AppId_t regId : registeredApps) {
            bool found = false;
            for (const auto* g : activeOnlineFixGames) {
                if (static_cast<AppId_t>(g->game_id() & UINT32_MAX) == regId) {
                    found = true;
                    break;
                }
            }
            if (!found && SteamCapture::CanUnregisterOnlineFixApp(regId)) {
                SteamCapture::UnregisterOnlineFixApp(regId);
                LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"unregister_exited_app\",\"appId\":{},\"remaining\":{}}}",
                               regId, activeOnlineFixGames.size());
            }
        }
    } else {
        // No registered OnlineFix games currently in msg. Only unregister if grace period passed or previously active.
        auto registeredApps = SteamCapture::GetActiveOnlineFixApps();
        bool anyPending = false;
        for (AppId_t regId : registeredApps) {
            if (SteamCapture::CanUnregisterOnlineFixApp(regId)) {
                SteamCapture::UnregisterOnlineFixApp(regId);
                LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"unregister_exited_app\",\"appId\":{}}}", regId);
            } else {
                anyPending = true;
                LOG_PKTRT_DEBUG("{{\"evt\":\"OnlineFix\",\"act\":\"pending_grace_period\",\"appId\":{}}}", regId);
            }
        }
        if (!anyPending) {
            SteamCapture::SetOnlineFixRoute(0, SteamCapture::OnlineFixRouteMode::None);

            if (fixGame != nullptr) {
                CMsgClientGamesPlayed cleanedMsg;
                for (int i = 0; i < msg.games_played_size(); ++i) {
                    const auto& g = msg.games_played(i);
                    if (static_cast<AppId_t>(g.game_id() & UINT32_MAX) != kOnlineFixAppId) {
                        cleanedMsg.add_games_played()->CopyFrom(g);
                    }
                }
                if (msg.has_client_os_type()) cleanedMsg.set_client_os_type(msg.client_os_type());
                if (msg.has_cloud_gaming_platform()) cleanedMsg.set_cloud_gaming_platform(msg.cloud_gaming_platform());
                if (msg.has_recent_reauthentication()) cleanedMsg.set_recent_reauthentication(msg.recent_reauthentication());
                msg.Swap(&cleanedMsg);
                patched = true;
                LOG_PKTRT_INFO("{{\"evt\":\"OnlineFix\",\"act\":\"clear_480_presence\",\"reason\":\"all_exited\"}}");
            }
        }
    }

    s_tx.BodyLen = static_cast<uint32_t>(msg.ByteSizeLong());
    if (s_tx.BodyLen > kBodyCap) {
        LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"overflow\",\"size\":{}}}", s_tx.BodyLen);
        return false;
    }
    if (!msg.SerializeToArray(s_tx.Body, kBodyCap)) {
        LOG_PKTRT_WARN("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"err\":\"encode-fail\"}}");
        return false;
    }

    LOG_PKTRT_DEBUG("{{\"evt\":\"OnlineFix\",\"act\":\"send\",\"modified\":{}}}", msg.DebugString());
    return true;
}

} // namespace NetPacket::Handlers::OnlineFix
