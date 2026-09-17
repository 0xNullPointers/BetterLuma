// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <windows.h>
#include <string>

#ifdef BETTERLUMA_PAYLOAD_LOGGING_ENABLED
namespace PayloadLog {
    void Init(HMODULE self);
    void Write(const std::string& line);
}
#else
namespace PayloadLog {
    inline void Init(HMODULE) {}
    inline void Write(const std::string&) {}
}
#endif
