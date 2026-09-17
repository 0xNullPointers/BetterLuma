// BetterLuma - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

namespace BuildInfo {

    // Returns the canonical release version string
    const char* Version();

    // Returns the exact build timestamp (e.g. "Sep 16 2026 02:15:30")
    const char* BuildStamp();

} // namespace BuildInfo
