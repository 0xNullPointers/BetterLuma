// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "core/entry.h"

namespace SteamUI {
    void CoreHook();
    void CoreUnhook();
}

namespace BetterLuma {
    void Attach();
    void Detach();
}


#endif // ORCHESTRATOR_H
