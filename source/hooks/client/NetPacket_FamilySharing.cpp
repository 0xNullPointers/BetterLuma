// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/NetPacket.h"
#include "runtime/Logger.h"

namespace NetPacket::Handlers::FamilySharing {

void ClearBody(const uint8_t*, uint32_t) {
    LOG_PKTRT_DEBUG("{{{{\"evt\":\"FamilySharing\",\"act\":\"clear\"}}}}");
    s_rx.BodyLen = 0;
    s_rx.PatchBody = true;
}

} // namespace NetPacket::Handlers::FamilySharing
