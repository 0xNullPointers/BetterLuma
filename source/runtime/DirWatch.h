// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace DirWatch {
    void Start(const std::vector<std::string>& directories);
    void Stop();
}