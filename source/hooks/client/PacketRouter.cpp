// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/PacketRouter.h"
#include "hooks/client/NetPacket.h"

namespace PacketRouter {
    void Install() {
        NetPacket::Install();
    }

    void Uninstall() {
        NetPacket::Uninstall();
    }
}
