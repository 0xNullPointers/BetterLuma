// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "runtime/BuildInfo.h"

#if __has_include("BuildStamp.h")
#include "BuildStamp.h"
#endif

#ifndef LUMACORE_VERSION_STRING
#define LUMACORE_VERSION_STRING "debug"
#endif

#ifndef LUMACORE_BUILD_STAMP_STRING
#define LUMACORE_BUILD_STAMP_STRING __DATE__ " " __TIME__
#endif

namespace BuildInfo {

    const char* Version() {
        return LUMACORE_VERSION_STRING;
    }

    const char* BuildStamp() {
        return LUMACORE_BUILD_STAMP_STRING;
    }

} // namespace BuildInfo
