// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include <string>

namespace HashUtil {

    // Converts raw byte buffer to a lowercase hex string.
    std::string ToHexLower(const std::uint8_t* data, std::size_t len);

    // BCrypt-based SHA-256 over a file on disk, backed by a thread-safe
    // in-memory cache for the process lifetime.
    // The first query computes the hash using hardware-accelerated BCrypt.
    // All subsequent queries across any module return the cached 64-character
    // hex digest in O(1) in-memory time (< 0.001 ms) without disk I/O.
    // Concurrent requests for the same file wait for the active computation,
    // preventing redundant hash operations.
    // Returns empty string on failure.
    std::string Sha256OfFile(const std::wstring& path);
    std::string Sha256OfFile(const std::string& path);
    std::string Sha256OfFile(const char* path);

    // Directly computes SHA-256 via BCrypt in 1 MiB chunks without checking or updating the cache.
    std::string ComputeSha256(const std::wstring& path);

    // Clears the in-memory hash cache.
    void ClearCache();

} // namespace HashUtil
