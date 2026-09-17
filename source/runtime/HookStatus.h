// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

// Tracks hook resolution status and pattern availability.
// The result lands in <Steam>\betterluma\status.json so external tools
// can inspect engine health and detect missing pattern sets.
//
// Threading: every active function is protected by an internal mutex.
// Disk writes are strictly explicit via WriteToDisk() at startup completion
// or fatal error, ensuring ZERO I/O overhead on Steam's critical runtime paths.
//
// Schema produced by WriteToDisk:
//   version                string
//   betterluma_version     string
//   build_stamp            string
//   betterluma_build_stamp string
//   build_config           string (Debug or Release)
//   steam_build_id         string
//   build_id               string
//   steamclient_sha        string (empty when unknown)
//   steamui_sha            string (empty when unknown)
//   toml_found             object with steamclient and steamui booleans
//   hooks_installed        non-negative integer
//   hooks_missed           array of strings
//   status                 string ("ready", "degraded", "failed")

#include <cstdint>
#include <string>
#include <string_view>

namespace HookStatus {

    void SetBuildId(std::string buildId);
    void SetTomlAvailability(std::string_view moduleName, bool found);
    void SetShas(std::string steamclientSha, std::string steamuiSha);
    void RecordInstalled() noexcept;
    void RecordMissed(std::string hookName);
    void WriteToDisk();

    // No-op compatibility stubs for legacy call sites (zero runtime cost)
    inline void SetBinarySnapshot(std::string, std::string, std::string, std::string, std::string, std::string, std::string) noexcept {}
    inline void SetLoaderState(std::string, std::string, std::string) noexcept {}
    inline void SetPackageState(bool, bool, bool, bool) noexcept {}
    inline void SetLuaCounts(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept {}
    inline void RecordPackage0Seen(std::int32_t, std::uint32_t, std::uint32_t) noexcept {}
    inline void RecordPackage0Capture(std::string, bool) noexcept {}
    inline void RecordStartupPackageRetry(std::string) noexcept {}
    inline void RecordPackageContainment(std::int32_t, std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::string) noexcept {}
    inline void RecordHotReload(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::string) noexcept {}
    inline void RecordOwnershipCheck(std::uint32_t, bool, bool, bool, std::int32_t, std::uint32_t, bool, bool) noexcept {}
    inline void RecordSubscribedApps(std::uint32_t, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t) noexcept {}
    inline void RecordCloudDecision(std::uint32_t, bool, bool, bool, bool, bool, bool, std::string) noexcept {}
    inline void RecordCloudCloseState(std::uint32_t, std::string, bool, bool) noexcept {}
    inline void RecordCloudSyncGate(std::uint32_t, std::string, std::string, std::string, bool) noexcept {}
    inline void RecordPatternStatus(std::string, std::string, bool, std::string, std::string) noexcept {}
    inline void RecordSteamUiLateRetry(std::string) noexcept {}
    inline void RecordSteamStubDetection(std::uint32_t, std::string, std::string, std::string, std::uint64_t, bool, std::string) noexcept {}
    inline void RecordOnlineFixPayload(std::uint32_t, std::uint32_t, std::string, std::string, std::string) noexcept {}
    inline void RecordStatsState(std::uint32_t, std::string, std::uint64_t, std::uint64_t, std::string, std::int32_t, std::string) noexcept {}
    inline void SetStartupPhase(std::string) noexcept {}
    inline void SetStartupRefreshState(std::string) noexcept {}
    inline void SetStartupSafety(std::string, bool, std::string) noexcept {}
    inline void SetMappedLoaders(std::string) noexcept {}
    inline void SetDiversionState(bool, std::string) noexcept {}
    inline void SetDiversionDetails(bool, bool, std::string, std::string) noexcept {}
    inline void SetSteamUiAttachState(std::string, int, bool) noexcept {}

}  // namespace HookStatus
