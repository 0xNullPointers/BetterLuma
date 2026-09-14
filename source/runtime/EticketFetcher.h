// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// on-demand encrypted app ticket minting from a user-configured backend.
// strict denuvo games nonce-bind their eticket at launch and reject any
// pre-baked one with 88500012. if the user set up a backend url via
// seteticketurl() in their lua config, we POST {app_id, nonce} to it and
// get back a fresh eticket + ownership ticket minted against a pool account.
// if no url is set, returns empty - caller falls back to credential store.
namespace EticketFetcher {

    std::optional<std::vector<uint8_t>> MintEticket(AppId_t appId, std::span<const uint8_t> nonce);

    // same backend call but returns the ownership ticket blob instead.
    // both come from one round-trip so the eticket served at ipc and the
    // ownership ticket spoofed at netpacket always match the same account.
    std::optional<std::vector<uint8_t>> MintOwnership(AppId_t appId, std::span<const uint8_t> nonce);

}
