// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#ifndef BETTERLUMA_MANIFEST_CACHE_H
#define BETTERLUMA_MANIFEST_CACHE_H

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace ManifestCache {

    // Magic bytes present in all valid Steam binary DepotManifest files:
    // Header uint32 (first 4 bytes): 0x71F617D0
    // Trailer/EOF uint32 (last 4 bytes): 0x32C415AB
    constexpr uint32_t kSteamManifestHeaderMagic  = 0x71F617D0;
    constexpr uint32_t kSteamManifestTrailerMagic = 0x32C415AB;

    // Checks if the manifest is cached at depotcache/<depotId>_<gid>.manifest.
    // If not present or corrupt, downloads from configured manifest_cache providers,
    // verifies header & EOF magic bytes, and saves atomically.
    // If triggerRefresh is true and the manifest is freshly cached, triggers Steam
    // license/update re-evaluation to immediately process downloads.
    //
    // Returns true if present and valid on disk, false if skipped/failed.
    bool EnsureCached(uint32_t depotId, uint64_t gid, uint32_t appId = 0, bool triggerRefresh = false);

    // Asynchronously ensures the manifest is cached without blocking the caller.
    // Checks if already present/valid; if missing, downloads in a detached background thread.
    void EnsureCachedAsync(uint32_t depotId, uint64_t gid, uint32_t appId = 0, bool triggerRefresh = true);

    // Returns true if the manifest file is present in depotcache/ and passes magic byte validation.
    bool IsCached(uint32_t depotId, uint64_t gid);

    // Waits up to timeoutMs for any active in-flight download of (depotId, gid) to complete.
    // Returns true if the manifest is valid and cached on disk.
    bool WaitForInflight(uint32_t depotId, uint64_t gid, uint32_t timeoutMs = 1500);

    // Validates whether the provided memory buffer contains a valid Steam binary manifest.
    bool ValidateManifestBytes(const void* data, size_t size);

    // Validates whether the file at path on disk contains a valid Steam binary manifest.
    bool ValidateManifestFile(const std::filesystem::path& path);

} // namespace ManifestCache

#endif // BETTERLUMA_MANIFEST_CACHE_H
