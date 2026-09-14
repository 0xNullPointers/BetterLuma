// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

namespace KVHooks {
    // Hooks KeyValues::ReadAsBinary so the keyvalue category gets at least
    // one entry per session. Triage on KV-tree regressions needs the file
    // non-empty.
    void Install();
    void Uninstall();
}
