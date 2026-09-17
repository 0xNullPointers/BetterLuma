// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "core/Orchestrator.h"
#include "hooks/client/DepotKeys.h"
#include "hooks/client/DecryptionKeyHook.h"
#include "hooks/client/IPCBus.h"
#include "hooks/client/ManifestBind.h"
#include "patterns/PatternFetcher.h"
#include "hooks/capture/SteamCapture.h"
#include "hooks/ui/SteamUI.h"
#include "hooks/client/PacketRouter.h"
#include "hooks/client/PackagePatch.h"
#include "hooks/client/LicenseHooks.h"
#include "hooks/client/OnlineFixInject.h"
#include "runtime/CloudRedirectHost.h"


namespace LumaCore {

    using HookOp = void(*)();
    static constexpr HookOp kInstallOrder[] = {
        DepotKeys::Install,
        DecryptionKeyHook::Install,
        IPCBus::Install,
        ManifestBind::Install,
        PacketRouter::Install,
        OnlineFixInject::Install,
        LicenseHooks::Install,
    };
    static constexpr HookOp kUninstallOrder[] = {
        DepotKeys::Uninstall,
        DecryptionKeyHook::Uninstall,
        IPCBus::Uninstall,
        ManifestBind::Uninstall,
        SteamCapture::Uninstall,
        SteamUI::CoreUnhook,
        PacketRouter::Uninstall,
        OnlineFixInject::Uninstall,
        PackagePatch::Uninstall,
        LicenseHooks::Uninstall,
    };

    void Attach() { for (auto fn : kInstallOrder) fn(); }

    void Detach() {
        for (auto fn : kUninstallOrder) fn();
        // Shutdown CloudRedirect host session
        CloudRedirectHost::Shutdown();
        PatternFetcher::Reset();
    }
}
