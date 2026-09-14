// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "core/entry.h"

#include <cstdint>
#include <string>

namespace OnlineFixInject {
    void Install();
    void Uninstall();

    void QueueInjection(const char* exePath, AppId_t realAppId);
    void RecordNoEos(uint32_t pid, const std::string& imageName, AppId_t realAppId);
    bool TryFallbackInject(uint32_t pid, const std::string& imageName, AppId_t realAppId);
}
