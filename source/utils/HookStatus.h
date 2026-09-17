// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <string>
#include <string_view>

namespace HookStatus {

    void SetBuildId(std::string buildId);
    void SetTomlAvailability(std::string_view moduleName, bool found);
    void SetShas(std::string steamclientSha, std::string steamuiSha);
    void RecordInstalled() noexcept;
    void RecordMissed(std::string hookName);
    void WriteToDisk();

}  // namespace HookStatus
