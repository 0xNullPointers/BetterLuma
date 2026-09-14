// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "runtime/Ticket.h"
#include "runtime/TicketProvider.h"

struct CSteamPipeClient;

namespace SteamStubTicket {
    bool IsApp7ForgeForTarget(const Ticket::AppTicketInspection& inspection, AppId_t targetAppId);
    bool ResolveRequest(CSteamPipeClient* pipe, AppId_t requestedAppId, AppId_t& ticketAppId);
    bool GetForRoute(AppId_t appId, Ticket::AppOwnershipTicket& out);
    bool GetForRoute(AppId_t appId, AppTicket::OwnershipTicket& out);
    bool GetForgeOnly(AppId_t appId, Ticket::AppOwnershipTicket& out);
    bool GetForgeOnly(AppId_t appId, AppTicket::OwnershipTicket& out);
}
