// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

struct CPipeClient;

namespace DenuvoAuth {

    // Called when a handshake is detected on a pipe.
    // Runs the one-time Denuvo detection (cached per process) and advances
    // the authorization state machine.
    void OnHandshake(const CPipeClient* pipe, uint32_t pid, AppId_t appId);

    // True only while the pipe is the selected authorization pipe and Denuvo
    // has not reached the end-authorization handshake count.
    bool IsAuthorizedPipe(const CPipeClient* pipe);

    // Installs the handshake callback into PipeWatch
    void Init();

}
