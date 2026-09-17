// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <windows.h>
#include <detours.h>

namespace RemoteInject {

    inline bool LoadDll(HANDLE proc, LPCWSTR dllPath) {
        if (!proc || !dllPath || !dllPath[0]) return false;

        BOOL isWow64 = FALSE;
        if (IsWow64Process(proc, &isWow64) && isWow64) {
            return false;
        }

        // Convert path to 8.3 short path to ensure it consists strictly of 7-bit ASCII characters.
        // PE import tables (IMAGE_IMPORT_DESCRIPTOR.Name) only support 8-bit ASCII.
        // If the path contains non-ASCII characters outside the active Windows code page,
        // WideCharToMultiByte emits '?', which causes ntdll!LdrpLoadDll to fail with STATUS_DLL_NOT_FOUND.
        wchar_t shortPathW[MAX_PATH] = {};
        LPCWSTR targetPathW = dllPath;
        if (GetShortPathNameW(dllPath, shortPathW, MAX_PATH) > 0) {
            targetPathW = shortPathW;
        } else {
            wchar_t fullPathW[MAX_PATH] = {};
            if (GetFullPathNameW(dllPath, MAX_PATH, fullPathW, nullptr) > 0) {
                targetPathW = fullPathW;
            }
        }

        char pathA[MAX_PATH] = {};
        if (WideCharToMultiByte(CP_ACP, 0, targetPathW, -1, pathA, MAX_PATH, nullptr, nullptr) <= 0) {
            return false;
        }

        if (strchr(pathA, '?') != nullptr) {
            return false;
        }

        LPCSTR rlpDlls[1] = { pathA };
        return DetourUpdateProcessWithDll(proc, rlpDlls, 1) == TRUE;
    }

}
