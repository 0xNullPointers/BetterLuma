// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#ifndef HOOKMANAGER_H
#define HOOKMANAGER_H

#include "entry.h"

namespace SteamUI {
    void CoreHook();
    void CoreUnhook();
}

namespace LumaCore {
    void Attach();
    void Detach();
}


#endif // HOOKMANAGER_H
