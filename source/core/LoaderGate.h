// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <windows.h>

namespace LoaderGate {

    // Installs the LoadLibraryExW hook and initializes the synchronization event.
    // Must be called early in DllMain on DLL_PROCESS_ATTACH.
    void Install();

    // Registers the thread ID of Bootstrap::Run so its module loads are never gated.
    void SetInitThreadId(DWORD tid);

    // Signals that LumaCore bootstrap has completed and all hooks/patterns are primed.
    // Releases any pending threads held at the LoadLibraryExW gate.
    void SignalBootstrapReady();

    // Detaches the LoadLibraryExW hook and cleans up synchronization handles.
    void Uninstall();

    // Returns true if bootstrap has finished.
    bool IsBootstrapReady();

} // namespace LoaderGate
