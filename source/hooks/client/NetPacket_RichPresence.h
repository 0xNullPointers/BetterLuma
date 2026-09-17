// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include "Steam/Types.h"

struct CNetPacket;

namespace RichPresence {
    bool HandleRecv(const uint8_t* pBody, uint32_t cbBody,
                    uint8_t* pOutBuf, uint32_t outBufSize, uint32_t* pOutSize);
    void TrackGamesPlayed(const uint8_t* pBody, uint32_t cbBody,
                          const uint8_t* pHdr, uint32_t cbHdr);
    void TrackUpload(const uint8_t* pBody, uint32_t cbBody);
    void DeliverPending(void* pThis, CNetPacket* pPacket,
                        bool (*callOriginal)(void*, CNetPacket*));
}
