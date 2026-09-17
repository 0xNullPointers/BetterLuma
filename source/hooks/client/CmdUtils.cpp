// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/CmdUser.h"
#include "hooks/client/CmdUtils.h"

namespace CmdUtils {
    void Register() {
        CmdUser::Register();
    }
}
