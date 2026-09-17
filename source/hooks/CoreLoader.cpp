// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "CoreLoader.h"
#include "DepotKeys.h"
#include "IPCBus.h"
#include "KeyValues.h"
#include "ManifestBind.h"
#include "PatternFetcher.h"
#include "SteamCapture.h"
#include "SteamUI.h"
#include "PacketRouter.h"
#include "PackagePatch.h"
#include "LicenseHooks.h"


namespace LumaCore {

    void Attach() {
        DepotKeys::Install();
        IPCBus::Install();
        KVHooks::Install();
        ManifestBind::Install();
        SteamCapture::Install();
        PacketRouter::Install();
        // PackagePatch::Install() is called early in entry.cpp InitThread,
        // immediately after LoadDiversion(), to catch LoadPackage before Steam calls it.
        LicenseHooks::Install();
    }

    void Detach() {
        DepotKeys::Uninstall();
        IPCBus::Uninstall();
        KVHooks::Uninstall();
        ManifestBind::Uninstall();
        SteamCapture::Uninstall();
        SteamUI::CoreUnhook();
        PacketRouter::Uninstall();
        PackagePatch::Uninstall();
        LicenseHooks::Uninstall();
        // Drop the runtime pattern map last; nothing else looks it up here.
        PatternFetcher::Reset();
    }
}
