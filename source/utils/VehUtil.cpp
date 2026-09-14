// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "VehUtil.h"

namespace VehUtil {
    void ArmInt3(void* target) {
        if (!target) return;
        DWORD oldProtect = 0;
        if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *static_cast<volatile uint8_t*>(target) = 0xCC;
            DWORD dummy = 0;
            VirtualProtect(target, 1, oldProtect, &dummy);
            FlushInstructionCache(GetCurrentProcess(), target, 1);
        }
    }

    void RestoreByte(void* target, uint8_t original) {
        if (!target) return;
        DWORD oldProtect = 0;
        if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *static_cast<volatile uint8_t*>(target) = original;
            DWORD dummy = 0;
            VirtualProtect(target, 1, oldProtect, &dummy);
            FlushInstructionCache(GetCurrentProcess(), target, 1);
        }
    }
}
