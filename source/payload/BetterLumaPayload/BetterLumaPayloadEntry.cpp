// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "EpicOnlineBridge.h"
#include "LcPayloadLogging.h"
#include "PayloadPropagator.h"

#include <windows.h>
#include <detours.h>
#include <psapi.h>
#include <string>

// Detours requires any injected DLL to export ordinal #1 so the Windows loader
// can link to it when loading the target process imports.
#if defined(_WIN64)
#pragma comment(linker, "/export:DetourFinishHelperProcess,@1,NONAME")
#else
#pragma comment(linker, "/export:DetourFinishHelperProcess=_DetourFinishHelperProcess@16,@1,NONAME")
#endif

namespace {

    struct UNICODE_STRING_ { USHORT Length, MaximumLength; PWSTR Buffer; };
    struct LDR_DLL_NOTIF {
        ULONG  Flags;
        const UNICODE_STRING_* FullDllName;
        const UNICODE_STRING_* BaseDllName;
        PVOID  DllBase;
        ULONG  SizeOfImage;
    };
    using LdrNotifyFn   = VOID(CALLBACK*)(ULONG, const LDR_DLL_NOTIF*, PVOID);
    using LdrRegisterFn = LONG(NTAPI*)(ULONG, LdrNotifyFn, PVOID, PVOID*);
    constexpr ULONG LDR_LOADED = 1;
    bool IsEosDll(const wchar_t* name) {
        if (!name) return false;
        return _wcsicmp(name, L"EOSSDK-Win64-Shipping.dll") == 0
            || _wcsicmp(name, L"EOSSDK-Win32-Shipping.dll") == 0;
    }

    void TryInstall(HMODULE m) {
        wchar_t base[MAX_PATH] = {};
        if (!GetModuleBaseNameW(GetCurrentProcess(), m, base, MAX_PATH)) return;
        if (IsEosDll(base)) EosBridge::InstallOn(m);
    }

    VOID CALLBACK OnDllLoad(ULONG reason, const LDR_DLL_NOTIF* d, PVOID) {
        if (reason != LDR_LOADED || !d || !d->BaseDllName || !d->BaseDllName->Buffer || !d->DllBase) return;
        const size_t chars = d->BaseDllName->Length / sizeof(wchar_t);
        if (chars == 0 || chars >= MAX_PATH) return;
        wchar_t buf[MAX_PATH];
        memcpy(buf, d->BaseDllName->Buffer, d->BaseDllName->Length);
        buf[chars] = L'\0';
        if (IsEosDll(buf))
            EosBridge::InstallOn(reinterpret_cast<HMODULE>(d->DllBase));
    }

    void SubscribeToDllLoads() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return;
        auto reg = reinterpret_cast<LdrRegisterFn>(
            GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        if (!reg) return;
        PVOID cookie = nullptr;
        reg(0, OnDllLoad, nullptr, &cookie);
    }

    void ScanLoadedModules() {
        HMODULE mods[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) TryInstall(mods[i]);
    }

    DWORD WINAPI PayloadMain(LPVOID hSelf) {
        PayloadLog::Init(static_cast<HMODULE>(hSelf));
        PayloadLog::Write("payload attached");
        SelfPropagate::Install(static_cast<HMODULE>(hSelf));
        SubscribeToDllLoads();
        ScanLoadedModules();
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DetourRestoreAfterWith();
        DisableThreadLibraryCalls(hModule);
        if (HANDLE h = CreateThread(nullptr, 0, PayloadMain, hModule, 0, nullptr))
            CloseHandle(h);
    }
    return TRUE;
}
