// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "runtime/HashUtil.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <condition_variable>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace HashUtil {

    namespace {

        struct CacheEntry {
            uint64_t    fileSize      = 0;
            uint64_t    lastWriteTime = 0;
            std::string sha256;
            bool        inFlight      = false;
        };

        std::mutex                                   g_cacheMutex;
        std::condition_variable                      g_cacheCv;
        std::unordered_map<std::wstring, CacheEntry> g_cache;

        std::wstring NormalizePathKey(const std::wstring& path) {
            std::error_code ec;
            std::filesystem::path p = std::filesystem::weakly_canonical(path, ec);
            std::wstring w = ec ? path : p.wstring();
            for (auto& c : w) {
                c = static_cast<wchar_t>(std::towlower(c));
            }
            return w;
        }

    } // anonymous namespace

    std::string ToHexLower(const std::uint8_t* data, std::size_t len) {
        static const char kDigits[] = "0123456789abcdef";
        std::string out;
        out.resize(len * 2);
        for (std::size_t i = 0; i < len; ++i) {
            out[2 * i + 0] = kDigits[(data[i] >> 4) & 0xF];
            out[2 * i + 1] = kDigits[data[i] & 0xF];
        }
        return out;
    }

    std::string ComputeSha256(const std::wstring& path) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return {};

        BCRYPT_ALG_HANDLE  hAlg  = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        std::array<std::uint8_t, 32> digest{};
        std::vector<std::uint8_t> buf(1u << 20); // 1 MiB stream buffer
        std::string out;

        do {
            if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) break;
            if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) break;

            bool ok = true;
            for (;;) {
                DWORD got = 0;
                if (!ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
                    ok = false; break;
                }
                if (got == 0) break;
                if (BCryptHashData(hHash, buf.data(), got, 0) != 0) { ok = false; break; }
            }
            if (!ok) break;
            if (BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) break;

            out = ToHexLower(digest.data(), digest.size());
        } while (false);

        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return out;
    }

    std::string Sha256OfFile(const std::wstring& path) {
        if (path.empty()) return {};

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            return {};
        }

        uint64_t currentSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        uint64_t currentMtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
        std::wstring normKey = NormalizePathKey(path);

        {
            std::unique_lock<std::mutex> lock(g_cacheMutex);
            for (;;) {
                auto it = g_cache.find(normKey);
                if (it != g_cache.end()) {
                    if (it->second.inFlight) {
                        // Another thread is actively hashing this file; wait for it to complete.
                        g_cacheCv.wait(lock);
                        continue;
                    }
                    if (it->second.fileSize == currentSize &&
                        it->second.lastWriteTime == currentMtime &&
                        it->second.sha256.size() == 64) {
                        return it->second.sha256;
                    }
                }
                // Not in cache or attributes changed: mark as in-flight on this thread
                g_cache[normKey] = CacheEntry{currentSize, currentMtime, "", true};
                break;
            }
        }

        // Compute outside of lock so other files can be queried/hashed concurrently
        std::string computed = ComputeSha256(path);

        {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (computed.size() == 64) {
                g_cache[normKey] = CacheEntry{currentSize, currentMtime, computed, false};
            } else {
                g_cache.erase(normKey);
            }
        }
        g_cacheCv.notify_all();

        return computed;
    }

    std::string Sha256OfFile(const std::string& path) {
        if (path.empty()) return {};
        std::filesystem::path fp(path);
        return Sha256OfFile(fp.wstring());
    }

    std::string Sha256OfFile(const char* path) {
        if (!path || !*path) return {};
        std::filesystem::path fp(path);
        return Sha256OfFile(fp.wstring());
    }

    void ClearCache() {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache.clear();
        g_cacheCv.notify_all();
    }

} // namespace HashUtil
