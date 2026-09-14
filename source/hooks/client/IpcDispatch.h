// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>
#include <string_view>

// Forward declares from Steam headers
struct CPipeClient;
struct CUtlBuffer;
struct CSteamPipeClient;

namespace IpcDispatch {

    using PreFn  = void(*)(CSteamPipeClient*, CUtlBuffer*, CUtlBuffer*);
    using PostFn = void(*)(CSteamPipeClient*, CUtlBuffer*, CUtlBuffer*);

    struct Entry {
        std::string_view ifaceName;
        std::string_view methodName;
        PreFn pre = nullptr;
        PostFn post = nullptr;
        uint32_t funcHash = 0;
    };

    // Register a handler entry. Called at startup from each interface's
    // registration function. funcHash is looked up from IpcLoader metadata
    // during registration; if no metadata exists the entry logs a warning
    // and does not dispatch.
    void Register(std::string_view ifaceName, std::string_view methodName, PreFn pre, PostFn post);

    // Install the IPCProcessMessage hook
    void Install();

    // Remove the hook
    void Uninstall();

}
