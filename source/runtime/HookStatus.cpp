// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "HookStatus.h"
#include "runtime/BuildInfo.h"
#include "runtime/Logger.h"
#include "core/entry.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace HookStatus {

    namespace {

        std::mutex g_mu;

        std::string g_buildId;
        std::string g_steamclientSha;
        std::string g_steamuiSha;
        bool        g_steamclientToml = false;
        bool        g_steamuiToml     = false;
        std::uint64_t            g_installed = 0;
        std::vector<std::string> g_missed;

#ifdef LUMACORE_LOGGING_ENABLED
        constexpr const char* kBuildConfig = "Debug";
#else
        constexpr const char* kBuildConfig = "Release";
#endif

        std::string JsonEscape(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 2);
            for (char ch : s) {
                unsigned char c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b";  break;
                    case '\f': out += "\\f";  break;
                    case '\n': out += "\\n";  break;
                    case '\r': out += "\\r";  break;
                    case '\t': out += "\\t";  break;
                    default:
                        if (c < 0x20 || c > 0x7E) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                            out += buf;
                        } else {
                            out += static_cast<char>(c);
                        }
                        break;
                }
            }
            return out;
        }

        std::string ComputeStatusLocked() {
            if (g_installed == 0 && (!g_steamclientToml || !g_steamuiToml)) {
                return "failed";
            }
            if (!g_missed.empty() || !g_steamclientToml || !g_steamuiToml) {
                return "degraded";
            }
            return "ready";
        }

        std::string SerializeLocked() {
            std::string out;
            out.reserve(512 + g_missed.size() * 32);
            out += "{\n";
            out += "  \"version\": \"" + JsonEscape(BuildInfo::Version()) + "\",\n";
            out += "  \"betterluma_version\": \"" + JsonEscape(BuildInfo::Version()) + "\",\n";
            out += "  \"build_stamp\": \"" + JsonEscape(BuildInfo::BuildStamp()) + "\",\n";
            out += "  \"betterluma_build_stamp\": \"" + JsonEscape(BuildInfo::BuildStamp()) + "\",\n";
            out += "  \"build_config\": \"" + JsonEscape(kBuildConfig) + "\",\n";
            out += "  \"steam_build_id\": \"" + JsonEscape(g_buildId) + "\",\n";
            out += "  \"build_id\": \"" + JsonEscape(g_buildId) + "\",\n";
            out += "  \"steamclient_sha\": \"" + JsonEscape(g_steamclientSha) + "\",\n";
            out += "  \"steamui_sha\": \"" + JsonEscape(g_steamuiSha) + "\",\n";
            out += "  \"toml_found\": {\n";
            out += "    \"steamclient\": ";
            out += (g_steamclientToml ? "true" : "false");
            out += ",\n";
            out += "    \"steamui\": ";
            out += (g_steamuiToml ? "true" : "false");
            out += "\n  },\n";
            out += "  \"hooks_installed\": " + std::to_string(g_installed) + ",\n";
            out += "  \"hooks_missed\": [";
            for (size_t i = 0; i < g_missed.size(); ++i) {
                if (i > 0) out += ", ";
                out += "\"" + JsonEscape(g_missed[i]) + "\"";
            }
            out += "],\n";
            out += "  \"status\": \"" + JsonEscape(ComputeStatusLocked()) + "\"\n";
            out += "}\n";
            return out;
        }

        bool WriteBodyAtomic(const std::string& body) {
            static std::atomic<DWORD> s_lastFailMs{0};
            static constexpr DWORD kCooldownMs = 5000;
            {
                DWORD last = s_lastFailMs.load(std::memory_order_relaxed);
                if (last && GetTickCount() - last < kCooldownMs) return false;
            }

            if (!SteamInstallPath[0]) {
                LOG_WARN("HookStatus: SteamInstallPath unset, skipping write");
                return false;
            }
            std::filesystem::path dir = std::filesystem::path(SteamInstallPath) / "betterluma";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                LOG_WARN("HookStatus: create_directories failed: {}", ec.message());
                return false;
            }

            std::filesystem::path target = dir / "status.json";
            std::filesystem::path tmp    = target;
            tmp += ".tmp";

            std::string narrowTmp    = tmp.string();
            std::string narrowTarget = target.string();

            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) {
                    LOG_WARN("HookStatus: open tmp failed for {}", narrowTarget);
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                f.flush();
                if (!f) {
                    LOG_WARN("HookStatus: write tmp failed for {}", narrowTarget);
                    f.close();
                    DeleteFileA(narrowTmp.c_str());
                    return false;
                }
            }

            if (!MoveFileExA(narrowTmp.c_str(), narrowTarget.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
                DWORD err = GetLastError();
                if (err == ERROR_ACCESS_DENIED || err == ERROR_FILE_NOT_FOUND) {
                    SetFileAttributesA(narrowTarget.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileA(narrowTarget.c_str());
                    if (MoveFileA(narrowTmp.c_str(), narrowTarget.c_str())) {
                        s_lastFailMs.store(0, std::memory_order_relaxed);
                        return true;
                    }
                }
                LOG_WARN("HookStatus: MoveFileExA failed err={} for {}",
                         err, narrowTarget);
                s_lastFailMs.store(GetTickCount(), std::memory_order_relaxed);
                DeleteFileA(narrowTmp.c_str());
                return false;
            }
            s_lastFailMs.store(0, std::memory_order_relaxed);
            return true;
        }

    }  // namespace

    void SetBuildId(std::string buildId) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_buildId = std::move(buildId);
    }

    void SetTomlAvailability(std::string_view moduleName, bool found) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (moduleName == "steamclient") {
            g_steamclientToml = found;
        } else if (moduleName == "steamui") {
            g_steamuiToml = found;
        } else {
            LOG_WARN("HookStatus: unknown module '{}' in SetTomlAvailability",
                     std::string(moduleName));
        }
    }

    void SetShas(std::string steamclientSha, std::string steamuiSha) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_steamclientSha = std::move(steamclientSha);
        g_steamuiSha     = std::move(steamuiSha);
    }

    void RecordInstalled() noexcept {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_installed;
    }

    void RecordMissed(std::string hookName) {
        if (hookName.empty()) return;
        std::lock_guard<std::mutex> lk(g_mu);
        g_missed.push_back(std::move(hookName));
    }

    void WriteToDisk() {
        std::string body;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            body = SerializeLocked();
        }
        try {
            (void)WriteBodyAtomic(body);
        } catch (const std::exception& e) {
            LOG_WARN("HookStatus: write threw '{}'", e.what());
        } catch (...) {
            LOG_WARN("HookStatus: write threw unknown");
        }
    }

}  // namespace HookStatus
