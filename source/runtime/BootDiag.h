// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <string_view>

// Startup diagnostics collector. Captures the Steam build ID and
// steamclient SHA256 at init time so a diagnostic popup can surface
// the state when IPC specs fail to load.
//
// The popup is gated by Settings::diagnosticPopupEnabled (default
// false). When enabled, ReportMissing() spawns a detached thread
// that shows a MessageBoxA with the captured data - useful for
// users sharing diagnostics when a Steam update breaks dispatch.
namespace BootDiag {

    // Capture the current build ID and optional steamclient SHA.
    // Reuses knownSha if provided, avoiding redundant disk hashing on startup.
    void Capture(std::string_view knownSha = {});

    // Show a non-blocking MessageBoxA popup on a detached thread.
    // Content includes the build ID and SHA captured above.
    // This is a read-only diagnostic - never modifies user files.
    void ReportMissing();

} // namespace BootDiag
