// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "stats/GlobalAchievementManager.h"
#include "runtime/RuntimeHttp.h"
#include "runtime/Logger.h"
#include "hooks/ui/SteamUI.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>

namespace GlobalAchievementManager {

    struct AppAchievements {
        std::vector<AchievementEntry> sortedList;
        std::unordered_map<std::string, float> percentByName;
        bool isReady = false;
        bool isFetching = false;
    };

    static std::mutex g_lock;
    static std::unordered_map<AppId_t, AppAchievements> g_apps;
    static std::filesystem::path g_cacheDir;
    static std::atomic<bool> g_isShuttingDown{false};

    void Initialize() {
        g_isShuttingDown.store(false, std::memory_order_release);
        std::error_code ec;
        if (SteamInstallPath[0] != '\0') {
            g_cacheDir = std::filesystem::path(SteamInstallPath) / "betterluma" / "cache" / "achievements";
        } else {
            g_cacheDir = std::filesystem::current_path() / "betterluma" / "cache" / "achievements";
        }
        std::filesystem::create_directories(g_cacheDir, ec);
    }

    void Shutdown() {
        g_isShuttingDown.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(g_lock);
        g_apps.clear();
    }

    bool HasData(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_apps.find(appId);
        return it != g_apps.end() && it->second.isReady && !it->second.sortedList.empty();
    }

    int GetMostAchievedAchievementInfo(AppId_t appId, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_apps.find(appId);
        if (it == g_apps.end() || !it->second.isReady || it->second.sortedList.empty()) {
            return -1;
        }

        const auto& first = it->second.sortedList[0];
        if (pchName && unNameBufLen > 0) {
            strncpy_s(pchName, unNameBufLen, first.name.c_str(), _TRUNCATE);
        }
        if (pflPercent) {
            *pflPercent = first.percent;
        }
        if (pbAchieved) {
            *pbAchieved = false;
        }
        return 0;
    }

    int GetNextMostAchievedAchievementInfo(AppId_t appId, int iPreviousAchievement, char* pchName, uint32 unNameBufLen, float* pflPercent, bool* pbAchieved) {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_apps.find(appId);
        if (it == g_apps.end() || !it->second.isReady) {
            return -1;
        }

        if (iPreviousAchievement < -1 || iPreviousAchievement >= static_cast<int>(it->second.sortedList.size())) {
            return -1;
        }

        size_t nextIndex = static_cast<size_t>(iPreviousAchievement + 1);
        if (nextIndex >= it->second.sortedList.size()) {
            return -1;
        }

        const auto& next = it->second.sortedList[nextIndex];
        if (pchName && unNameBufLen > 0) {
            strncpy_s(pchName, unNameBufLen, next.name.c_str(), _TRUNCATE);
        }
        if (pflPercent) {
            *pflPercent = next.percent;
        }
        if (pbAchieved) {
            *pbAchieved = false;
        }
        return static_cast<int>(nextIndex);
    }

    bool GetAchievementAchievedPercent(AppId_t appId, const char* pchName, float* pflPercent) {
        if (!pchName) return false;
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_apps.find(appId);
        if (it == g_apps.end() || !it->second.isReady) {
            return false;
        }

        auto mapIt = it->second.percentByName.find(pchName);
        if (mapIt != it->second.percentByName.end()) {
            if (pflPercent) {
                *pflPercent = mapIt->second;
            }
            return true;
        }
        return false;
    }

    static bool ParseValveAchievementJson(std::string_view json, std::vector<AchievementEntry>& out) {
        out.clear();
        size_t pos = json.find("\"achievements\"");
        if (pos == std::string_view::npos) return false;

        pos = json.find('[', pos);
        if (pos == std::string_view::npos) return false;

        while (pos < json.size()) {
            size_t objStart = json.find('{', pos);
            if (objStart == std::string_view::npos) break;

            size_t objEnd = json.find('}', objStart);
            if (objEnd == std::string_view::npos) break;

            std::string_view obj = json.substr(objStart, objEnd - objStart + 1);
            pos = objEnd + 1;

            size_t nameKey = obj.find("\"name\"");
            if (nameKey == std::string_view::npos) continue;

            size_t nameColon = obj.find(':', nameKey);
            if (nameColon == std::string_view::npos) continue;

            size_t nameValStart = obj.find('"', nameColon);
            if (nameValStart == std::string_view::npos) continue;
            nameValStart++;

            size_t nameValEnd = obj.find('"', nameValStart);
            if (nameValEnd == std::string_view::npos) continue;

            std::string name(obj.substr(nameValStart, nameValEnd - nameValStart));

            size_t pctKey = obj.find("\"percent\"");
            if (pctKey == std::string_view::npos) continue;

            size_t pctColon = obj.find(':', pctKey);
            if (pctColon == std::string_view::npos) continue;

            size_t valPos = pctColon + 1;
            while (valPos < obj.size() && (obj[valPos] == ' ' || obj[valPos] == '\t' || obj[valPos] == '\r' || obj[valPos] == '\n')) {
                valPos++;
            }
            if (valPos >= obj.size()) continue;

            float percent = 0.0f;
            if (obj[valPos] == '"') {
                valPos++;
                size_t valEnd = obj.find('"', valPos);
                if (valEnd != std::string_view::npos) {
                    std::string s(obj.substr(valPos, valEnd - valPos));
                    percent = std::strtof(s.c_str(), nullptr);
                }
            } else {
                size_t valEnd = obj.find_first_of(",} \t\r\n", valPos);
                std::string s = (valEnd != std::string_view::npos)
                    ? std::string(obj.substr(valPos, valEnd - valPos))
                    : std::string(obj.substr(valPos));
                percent = std::strtof(s.c_str(), nullptr);
            }

            out.push_back({std::move(name), percent});
        }

        // Sort descending by percentage as required by GetMostAchievedAchievementInfo
        std::sort(out.begin(), out.end(), [](const AchievementEntry& a, const AchievementEntry& b) {
            return a.percent > b.percent;
        });

        return !out.empty();
    }

    static void PopulateAppCache(AppId_t appId, std::vector<AchievementEntry> entries) {
        if (g_isShuttingDown.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_isShuttingDown.load(std::memory_order_acquire)) return;
        auto& app = g_apps[appId];
        app.sortedList = std::move(entries);
        app.percentByName.clear();
        for (const auto& item : app.sortedList) {
            app.percentByName[item.name] = item.percent;
        }
        app.isReady = true;
        app.isFetching = false;
    }

    static bool TryLoadFromDiskCache(AppId_t appId) {
        if (g_isShuttingDown.load(std::memory_order_acquire)) return false;
        if (g_cacheDir.empty()) {
            Initialize();
        }

        std::filesystem::path cacheFile = g_cacheDir / std::format("{}.json", appId);
        std::error_code ec;
        if (!std::filesystem::exists(cacheFile, ec)) {
            return false;
        }

        auto fileSize = std::filesystem::file_size(cacheFile, ec);
        if (ec || fileSize == 0 || fileSize > 2 * 1024 * 1024) {
            return false;
        }

        auto lastWrite = std::filesystem::last_write_time(cacheFile, ec);
        if (ec) return false;

        auto now = std::filesystem::file_time_type::clock::now();
        auto age = std::chrono::duration_cast<std::chrono::hours>(now - lastWrite).count();
        constexpr int64_t kMaxAgeHours = 7 * 24; // 7 days
        if (age >= kMaxAgeHours) {
            return false;
        }

        std::ifstream f(cacheFile, std::ios::binary);
        if (!f.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<AchievementEntry> entries;
        if (!ParseValveAchievementJson(content, entries)) {
            return false;
        }

        LOG_ACHIEVEMENTCH_INFO("Global achievements for appId={} loaded synchronously from disk cache ({} items, age={}h)",
                               appId, entries.size(), age);
        PopulateAppCache(appId, std::move(entries));
        return true;
    }

    static void DoFetchAndLoad(AppId_t appId, uint64_t gameId) {
        if (g_isShuttingDown.load(std::memory_order_acquire)) return;
        if (g_cacheDir.empty()) {
            Initialize();
        }

        std::filesystem::path cacheFile = g_cacheDir / std::format("{}.json", appId);
        std::error_code ec;

        // 1. Try disk cache if fresh (< 7 days)
        if (std::filesystem::exists(cacheFile, ec)) {
            auto fileSize = std::filesystem::file_size(cacheFile, ec);
            if (!ec && fileSize > 0 && fileSize <= 2 * 1024 * 1024) {
                auto lastWrite = std::filesystem::last_write_time(cacheFile, ec);
                if (!ec) {
                    auto now = std::filesystem::file_time_type::clock::now();
                    auto age = std::chrono::duration_cast<std::chrono::hours>(now - lastWrite).count();
                    constexpr int64_t kMaxAgeHours = 7 * 24; // 7 days

                    if (age < kMaxAgeHours) {
                        std::ifstream f(cacheFile, std::ios::binary);
                        if (f.is_open()) {
                            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                            std::vector<AchievementEntry> entries;
                            if (ParseValveAchievementJson(content, entries)) {
                                LOG_ACHIEVEMENTCH_INFO("Global achievements for appId={} loaded from disk cache ({} items, age={}h)",
                                                       appId, entries.size(), age);
                                PopulateAppCache(appId, std::move(entries));
                                if (!g_isShuttingDown.load(std::memory_order_acquire)) {
                                    SteamUI::QueueLibraryTouch(appId);
                                }
                                return;
                            }
                        }
                    }
                }
            }
        }

        if (g_isShuttingDown.load(std::memory_order_acquire)) return;

        // 2. Fetch from Steam Web API
        std::string url = std::format("https://api.steampowered.com/ISteamUserStats/GetGlobalAchievementPercentagesForApp/v0002/?gameid={}&format=json", appId);
        LOG_ACHIEVEMENTCH_INFO("Fetching global achievements from Web API for appId={}", appId);

        auto resp = RuntimeHttp::Get(url);
        if (g_isShuttingDown.load(std::memory_order_acquire)) return;

        if (!resp.networkError && resp.status == 200 && !resp.body.empty()) {
            std::vector<AchievementEntry> entries;
            ParseValveAchievementJson(resp.body, entries);

            // Only cache to disk if parsing succeeded and produced entries
            if (!entries.empty()) {
                std::filesystem::create_directories(g_cacheDir, ec);
                std::ofstream out(cacheFile, std::ios::binary | std::ios::trunc);
                if (out.is_open()) {
                    out.write(resp.body.data(), static_cast<std::streamsize>(resp.body.size()));
                }
            }

            LOG_ACHIEVEMENTCH_INFO("Successfully fetched {} global achievements for appId={}", entries.size(), appId);
            PopulateAppCache(appId, std::move(entries));
            if (!g_isShuttingDown.load(std::memory_order_acquire)) {
                SteamUI::QueueLibraryTouch(appId);
            }
            return;
        }

        // 3. Fallback: If Web API failed (e.g. offline), try stale disk cache if it exists
        if (std::filesystem::exists(cacheFile, ec)) {
            std::ifstream f(cacheFile, std::ios::binary);
            if (f.is_open()) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                std::vector<AchievementEntry> entries;
                if (ParseValveAchievementJson(content, entries)) {
                    LOG_ACHIEVEMENTCH_WARN("Web API failed ({}, status={}), using stale disk cache for appId={} ({} items)",
                                           resp.diagnostic, resp.status, appId, entries.size());
                    PopulateAppCache(appId, std::move(entries));
                    if (!g_isShuttingDown.load(std::memory_order_acquire)) {
                        SteamUI::QueueLibraryTouch(appId);
                    }
                    return;
                }
            }
        }

        // 4. No achievements or game has none configured (e.g. 403 or empty)
        LOG_ACHIEVEMENTCH_INFO("No global achievements available for appId={} (status={}, diag={})",
                               appId, resp.status, resp.diagnostic);
        PopulateAppCache(appId, {});
    }

    bool EnsureLoaded(AppId_t appId, uint64_t gameId) {
        if (appId == 0 || g_isShuttingDown.load(std::memory_order_acquire)) return false;

        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto it = g_apps.find(appId);
            if (it != g_apps.end() && it->second.isReady) {
                return !it->second.sortedList.empty();
            }
        }

        if (TryLoadFromDiskCache(appId)) {
            return true;
        }

        RequestPercentagesAsync(appId, gameId);
        return false;
    }

    void RequestPercentagesAsync(AppId_t appId, uint64_t gameId) {
        if (appId == 0 || g_isShuttingDown.load(std::memory_order_acquire)) return;

        {
            std::lock_guard<std::mutex> lock(g_lock);
            auto& app = g_apps[appId];
            if (app.isReady || app.isFetching) {
                return;
            }

            app.isFetching = true;
        }

        std::thread([appId, gameId]() {
            DoFetchAndLoad(appId, gameId);
        }).detach();
    }

} // namespace GlobalAchievementManager
