// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "runtime/ManifestCache.h"
#include "runtime/RuntimeHttp.h"
#include "runtime/Logger.h"
#include "config/Settings.h"
#include "core/entry.h"
#include "hooks/capture/RuntimeCapture.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

    std::mutex g_lock;
    std::condition_variable g_cv;
    std::set<std::pair<uint32_t, uint64_t>> g_inflight;

    bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            unsigned char ac = static_cast<unsigned char>(a[i]);
            unsigned char bc = static_cast<unsigned char>(b[i]);
            if (std::tolower(ac) != std::tolower(bc)) return false;
        }
        return true;
    }

    std::string_view ExtractHost(std::string_view url) {
        size_t begin = 0;
        size_t scheme = url.find("://");
        if (scheme != std::string_view::npos) begin = scheme + 3;
        size_t end = url.find_first_of("/?#", begin);
        std::string_view host = end == std::string_view::npos
            ? url.substr(begin)
            : url.substr(begin, end - begin);
        size_t at = host.rfind('@');
        if (at != std::string_view::npos) host.remove_prefix(at + 1);
        size_t port = host.find(':');
        if (port != std::string_view::npos) host = host.substr(0, port);
        return host;
    }

    bool IsHostAllowed(std::string_view host, const std::vector<std::string>& trusted) {
        if (trusted.empty()) return true;
        for (const auto& t : trusted) {
            if (EqualsIgnoreCase(host, t)) return true;
        }
        return false;
    }

    std::string ExpandTemplate(std::string_view tmpl,
                               uint32_t depotId, uint64_t gid, uint32_t appId) {
        std::string out;
        out.reserve(tmpl.size() + 32);
        for (size_t i = 0; i < tmpl.size(); ) {
            if (tmpl[i] != '{') { out.push_back(tmpl[i++]); continue; }
            size_t end = tmpl.find('}', i + 1);
            if (end == std::string_view::npos) { out.push_back(tmpl[i++]); continue; }
            std::string_view tag = tmpl.substr(i + 1, end - i - 1);
            if (tag == "depotid" || tag == "depot")
                out += std::to_string(depotId);
            else if (tag == "gid" || tag == "manifestid")
                out += std::to_string(gid);
            else if (tag == "appid")
                out += std::to_string(appId);
            else if (EqualsIgnoreCase(tag, "hubcap_key")) {
                if (Settings::hubcapKey.empty()) return {};
                out += Settings::hubcapKey;
            } else if (EqualsIgnoreCase(tag, "manifesthub_key")) {
                if (Settings::manifestHubKey.empty()) return {};
                out += Settings::manifestHubKey;
            } else
                out.append(tmpl.substr(i, end - i + 1));
            i = end + 1;
        }
        return out;
    }

    std::filesystem::path GetDepotCachePath(uint32_t depotId, uint64_t gid) {
        if (SteamInstallPath[0] == '\0') return {};
        std::filesystem::path root(SteamInstallPath);
        return root / "depotcache" / (std::to_string(depotId) + "_" + std::to_string(gid) + ".manifest");
    }

} // anonymous namespace

namespace ManifestCache {

    bool ValidateManifestBytes(const void* data, size_t size) {
        if (!data || size < 8) return false;
        uint32_t header = 0;
        std::memcpy(&header, data, sizeof(uint32_t));
        uint32_t trailer = 0;
        std::memcpy(&trailer, static_cast<const uint8_t*>(data) + size - sizeof(uint32_t), sizeof(uint32_t));
        return (header == kSteamManifestHeaderMagic && trailer == kSteamManifestTrailerMagic);
    }

    bool ValidateManifestFile(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return false;
        auto sz = std::filesystem::file_size(path, ec);
        if (ec || sz < 8) return false;

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        uint32_t header = 0;
        if (!f.read(reinterpret_cast<char*>(&header), sizeof(uint32_t))) return false;
        if (header != kSteamManifestHeaderMagic) return false;

        if (!f.seekg(static_cast<std::streamoff>(sz - sizeof(uint32_t)), std::ios::beg)) return false;
        uint32_t trailer = 0;
        if (!f.read(reinterpret_cast<char*>(&trailer), sizeof(uint32_t))) return false;
        return (trailer == kSteamManifestTrailerMagic);
    }

    bool IsCached(uint32_t depotId, uint64_t gid) {
        if (depotId == 0 || gid == 0) return false;
        auto path = GetDepotCachePath(depotId, gid);
        return !path.empty() && ValidateManifestFile(path);
    }

    bool WaitForInflight(uint32_t depotId, uint64_t gid, uint32_t timeoutMs) {
        if (depotId == 0 || gid == 0) return false;
        auto finalPath = GetDepotCachePath(depotId, gid);
        if (finalPath.empty()) return false;
        if (ValidateManifestFile(finalPath)) return true;

        std::unique_lock<std::mutex> lk(g_lock);
        auto key = std::make_pair(depotId, gid);
        if (g_inflight.count(key) == 0) {
            return ValidateManifestFile(finalPath);
        }
        g_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&]() {
            return g_inflight.count(key) == 0;
        });
        return ValidateManifestFile(finalPath);
    }

    bool EnsureCached(uint32_t depotId, uint64_t gid, uint32_t appId, bool triggerRefresh) {
        if (!Settings::manifestCacheEnabled) return false;
        if (depotId == 0 || gid == 0) return false;

        auto finalPath = GetDepotCachePath(depotId, gid);
        if (finalPath.empty()) {
            LOG_MANIFESTCH_WARN("ManifestCache: SteamInstallPath not set, cannot locate depotcache");
            return false;
        }

        // Fast path: already on disk and passes magic byte verification
        if (ValidateManifestFile(finalPath)) {
            LOG_MANIFESTCH_DEBUG("ManifestCache: depot={} gid={} already cached and valid", depotId, gid);
            return true;
        }

        // Concurrency synchronization: ensure only one thread downloads (depotId, gid)
        {
            std::unique_lock<std::mutex> lk(g_lock);
            auto key = std::make_pair(depotId, gid);
            if (g_inflight.count(key)) {
                LOG_MANIFESTCH_INFO("ManifestCache: depot={} gid={} joining in-flight download", depotId, gid);
                g_cv.wait(lk, [&]() { return g_inflight.count(key) == 0; });
                bool valid = ValidateManifestFile(finalPath);
                if (valid && triggerRefresh) {
                    AppId_t targetAppId = (appId != 0) ? appId : depotId;
                    SteamCapture::RefreshAppUpdate(targetAppId);
                }
                return valid;
            }
            g_inflight.insert(key);
        }

        struct InflightGuard {
            uint32_t d;
            uint64_t g;
            ~InflightGuard() {
                std::lock_guard<std::mutex> lk(g_lock);
                g_inflight.erase(std::make_pair(d, g));
                g_cv.notify_all();
            }
        } guard{depotId, gid};

        // Re-check after acquiring single-flight slot
        if (ValidateManifestFile(finalPath)) {
            return true;
        }

        const auto& chain = Settings::manifestCacheUrls;
        if (chain.empty()) {
            LOG_MANIFESTCH_DEBUG("ManifestCache: depot={} gid={} skipped, no cache URLs configured", depotId, gid);
            return false;
        }

        DWORD timeoutMs = static_cast<DWORD>(
            Settings::manifestCacheTimeoutSec > 0 ? Settings::manifestCacheTimeoutSec * 1000 : 30'000);
        constexpr size_t kManifestMaxCap = 256u * 1024u * 1024u; // 256 MiB ceiling for large manifests

        for (size_t i = 0; i < chain.size(); ++i) {
            const std::string& tmpl = chain[i];
            if (tmpl.empty()) continue;
            std::string url = ExpandTemplate(tmpl, depotId, gid, appId);
            if (url.empty()) {
                LOG_MANIFESTCH_DEBUG("ManifestCache: depot={} gid={} provider {}/{} skipped (required API key not configured)",
                                     depotId, gid, i + 1, chain.size());
                continue;
            }
            std::string_view host = ExtractHost(url);

            if (!IsHostAllowed(host, Settings::manifestCacheTrustedHosts)) {
                LOG_MANIFESTCH_WARN("ManifestCache: depot={} gid={} provider {}/{} host '{}' untrusted, skipping",
                                    depotId, gid, i + 1, chain.size(), host);
                continue;
            }

            std::string logUrl = url;
            for (const char* param : {"apikey=", "api_key="}) {
                auto keyPos = logUrl.find(param);
                if (keyPos != std::string::npos) {
                    size_t pLen = std::strlen(param);
                    auto ampPos = logUrl.find('&', keyPos);
                    auto valStart = keyPos + pLen;
                    if (ampPos != std::string::npos && ampPos > valStart) {
                        logUrl.replace(valStart, ampPos - valStart, "***");
                    } else if (logUrl.size() > valStart) {
                        logUrl.replace(valStart, logUrl.size() - valStart, "***");
                    }
                }
            }

            LOG_MANIFESTCH_INFO("ManifestCache: depot={} gid={} provider {}/{} downloading {}",
                                depotId, gid, i + 1, chain.size(), logUrl);

            auto resp = RuntimeHttp::Get(url, L"BetterLuma-ManifestCache/1.0", kManifestMaxCap, timeoutMs);
            if (resp.networkError) {
                LOG_MANIFESTCH_WARN("ManifestCache: depot={} gid={} provider {} net err '{}', trying next",
                                    depotId, gid, i + 1, resp.diagnostic);
                continue;
            }
            if (resp.status != 200) {
                LOG_MANIFESTCH_WARN("ManifestCache: depot={} gid={} provider {} HTTP={} body_bytes={}, trying next",
                                    depotId, gid, i + 1, resp.status, resp.body.size());
                continue;
            }
            if (!ValidateManifestBytes(resp.body.data(), resp.body.size())) {
                LOG_MANIFESTCH_WARN("ManifestCache: depot={} gid={} provider {} invalid manifest magic (size={}), trying next",
                                    depotId, gid, i + 1, resp.body.size());
                continue;
            }

            // Ensure depotcache directory exists
            std::error_code ec;
            std::filesystem::create_directories(finalPath.parent_path(), ec);

            // Atomic file write: write to unique process/thread temporary file, then atomic MoveFileExW
            auto tmpPath = finalPath;
            tmpPath += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".tmp";

            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                LOG_MANIFESTCH_ERROR("ManifestCache: failed to open tmp file '{}' for writing", tmpPath.string());
                continue;
            }
            out.write(resp.body.data(), resp.body.size());
            out.close();

            bool replaced = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                    replaced = true;
                    break;
                }
                Sleep(50);
            }
            if (!replaced) {
                DWORD err = GetLastError();
                LOG_MANIFESTCH_ERROR("ManifestCache: MoveFileExW failed err={} replacing '{}'", err, finalPath.string());
                std::filesystem::remove(tmpPath, ec);
                continue;
            }

            LOG_MANIFESTCH_INFO("ManifestCache: successfully cached depot={} gid={} ({} bytes) -> '{}'",
                                depotId, gid, resp.body.size(), finalPath.string());
            if (triggerRefresh) {
                AppId_t targetAppId = (appId != 0) ? appId : depotId;
                SteamCapture::RefreshAppUpdate(targetAppId);
            }
            return true;
        }

        LOG_MANIFESTCH_WARN("ManifestCache: depot={} gid={} all {} providers exhausted, falling through to request code",
                            depotId, gid, chain.size());
        return false;
    }

    void EnsureCachedAsync(uint32_t depotId, uint64_t gid, uint32_t appId, bool triggerRefresh) {
        if (!Settings::manifestCacheEnabled) return;
        if (depotId == 0 || gid == 0) return;

        auto finalPath = GetDepotCachePath(depotId, gid);
        if (finalPath.empty() || ValidateManifestFile(finalPath)) return;

        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_inflight.count(std::make_pair(depotId, gid))) return;
        }

        std::thread([depotId, gid, appId, triggerRefresh]() {
            EnsureCached(depotId, gid, appId, triggerRefresh);
        }).detach();
    }

} // namespace ManifestCache
