// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once
#include <cstdint>

// compile-time FNV-1a hash (32-bit) used by betterluma for target_job_name dispatch
constexpr uint32_t LcFnvHash(const char* str)
{
    uint32_t h = 0x811c9dc5u;
    while (*str) {
        h ^= static_cast<uint32_t>(static_cast<unsigned char>(*str++));
        h *= 0x01000193u;
    }
    return h;
}
