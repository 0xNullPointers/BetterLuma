// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <windows.h>

namespace LoaderGate {

    // Installs the LoadLibraryExW hook and initializes the synchronization events.
    // Must be called early in DllMain on DLL_PROCESS_ATTACH.
    void Install();

    // Registers the thread ID of Bootstrap::Run so its module loads are never gated.
    void SetInitThreadId(DWORD tid);

    // Signals Stage 1: steamclient diversion is loaded and all core steamclient hooks are installed.
    // Releases any pending threads held at the LoadLibraryExW gate for steamclient64.dll.
    void SignalClientReady();

    // Signals Stage 2: all bootstrap tasks including steamui patterns/hooks and Lua parsing are complete.
    // Releases any pending threads held at the LoadLibraryExW gate for steamui.dll.
    void SignalUiReady();

    // Signals both Stage 1 and Stage 2. Called on complete bootstrap or fallback/error paths.
    void SignalBootstrapReady();

    // Detaches the LoadLibraryExW hook and cleans up synchronization handles.
    void Uninstall();

    // Query gate readiness states
    bool IsClientReady();
    bool IsUiReady();
    bool IsBootstrapReady();

} // namespace LoaderGate
