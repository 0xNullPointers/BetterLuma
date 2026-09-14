// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "steam/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace RemoteTools {
    enum class ProcessBits {
        Unknown,
        X86,
        X64,
    };

    struct ModuleInfo {
        std::wstring name;
        std::wstring path;
        uintptr_t base = 0;
        uint32 size = 0;
    };

    struct LoadResult {
        bool ok = false;
        bool alreadyLoaded = false;
        std::string error;
    };

    const char* BitsName(ProcessBits bits);
    ProcessBits DetectBits(uint32 pid);
    std::vector<ModuleInfo> EnumerateModules(uint32 pid);
    bool HasModuleFileName(const std::vector<ModuleInfo>& modules,
                           const std::filesystem::path& dllPath);
    LoadResult LoadLibraryInto(uint32 pid, const std::filesystem::path& dllPath);
}
