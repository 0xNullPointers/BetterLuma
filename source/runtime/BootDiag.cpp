// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "runtime/BootDiag.h"

#include "core/entry.h"
#include "runtime/Logger.h"
#include "config/Settings.h"
#include "runtime/HashUtil.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace BootDiag {

    namespace {

        std::string g_capturedBuildId;
        std::string g_capturedSha;

        using HashUtil::Sha256OfFile;

        void PopupThread() {
            if (g_capturedSha.empty() && SteamclientPath[0]) {
                g_capturedSha = Sha256OfFile(SteamclientPath);
            }

            char msg[4096];
            std::snprintf(msg, sizeof(msg),
                "BetterLuma: IPC specs unavailable\n\n"
                "Steam build ID: %s\n"
                "Steamclient:    %s\n\n"
                "This Steam version may not be supported yet. "
                "Some game features may not work correctly.\n\n"
                "This is a read-only diagnostic -- your files are not affected.",
                g_capturedBuildId.empty() ? "unknown" : g_capturedBuildId.c_str(),
                g_capturedSha.empty()     ? "unknown" : g_capturedSha.c_str());

            MessageBoxA(nullptr, msg, "BetterLuma -- Steam Diagnostics",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }

    } // anonymous namespace

    void Capture(std::string_view knownSha) {
        g_capturedBuildId = g_steamBuildId;
        if (!knownSha.empty()) {
            g_capturedSha = std::string(knownSha);
        } else {
            g_capturedSha.clear();
        }

        LOG_MISC_DEBUG("BootDiag: captured build={} sha={}",
                       g_capturedBuildId.empty() ? "unknown" : g_capturedBuildId,
                       g_capturedSha.empty()     ? "deferred" : g_capturedSha);
    }

    void ReportMissing() {
        if (!Settings::diagnosticPopupEnabled) {
            LOG_MISC_DEBUG("BootDiag: popup disabled, skipping");
            return;
        }
        std::thread(PopupThread).detach();
        LOG_MISC_DEBUG("BootDiag: popup dispatched");
    }

} // namespace BootDiag
