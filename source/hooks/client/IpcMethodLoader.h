// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace IpcLoader {

    struct MethodMeta {
        uint32_t funcHash = 0;
        uint32_t fencepost = 0;
        uint32_t argc = 0;
    };

    // Parses ipc_methods.toml alongside the steamclient dll path.
    // The caller supplies the steamclient path so we can derive the TOML path.
    // Falls back to a remote fetch from the pattern repo when no local file exists.
    bool Load(const std::string& steamclientPath, const std::string& knownSha = "");

    bool IsLoaded();

    const MethodMeta* Find(std::string_view ifaceName, std::string_view methodName);

    // FNV-1a hash helpers used by the dispatch table
    uint32_t HashInterfaceName(std::string_view name);
    uint32_t HashMethodName(std::string_view name);

}
