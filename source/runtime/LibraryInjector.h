// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include <string>

namespace Injection {

    struct Settings {
        bool enabled = false;
        std::string libraryX64;
        std::string libraryX86;
    };

    // Try to inject the configured library into the given process.
    // Safe to call multiple times per process; only the first call injects.
    void Apply(uint32_t pid);

    // Load settings from config
    Settings LoadSettings();

}
