// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "hooks/client/PipeWatch.h"

namespace ProcessExtension {
    void Reset();
    void QueueLaunchHint(const char* exePath, AppId_t appId);
    void OnGamePipe(const PipeWatch::ProcessSnapshot& snapshot);
}
