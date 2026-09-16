// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "core/LoaderGate.h"
#include "core/entry.h"
#include "runtime/Logger.h"

#include <detours.h>
#include <atomic>
#include <cwctype>

namespace LoaderGate {

    using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

    static LoadLibraryExW_t  oLoadLibraryExW = nullptr;
    static HANDLE            g_hBootstrapReadyEvent = nullptr;
    static std::atomic<bool> g_isReady{false};
    static std::atomic<bool> g_installed{false};
    static DWORD             g_initThreadId = 0;

    static const wchar_t* GetFileNameOnly(LPCWSTR path) {
        if (!path) return L"";
        const wchar_t* p1 = wcsrchr(path, L'\\');
        const wchar_t* p2 = wcsrchr(path, L'/');
        if (p2 && (!p1 || p2 > p1)) p1 = p2;
        return p1 ? p1 + 1 : path;
    }

#ifdef LUMACORE_LOGGING_ENABLED
#define LOG_GATE_INFO(...) do { if (Logger::CoreInCh) { LOG_COREIN_INFO(__VA_ARGS__); } } while (0)
#else
#define LOG_GATE_INFO(...) ((void)0)
#endif

    static HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
        if (!lpLibFileName || !oLoadLibraryExW) {
            return oLoadLibraryExW ? oLoadLibraryExW(lpLibFileName, hFile, dwFlags) : nullptr;
        }

        // Never gate the initialization worker thread itself
        if (g_initThreadId != 0 && GetCurrentThreadId() == g_initThreadId) {
            return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
        }

        const wchar_t* fn = GetFileNameOnly(lpLibFileName);
        const bool isSteamClient = (_wcsicmp(fn, L"steamclient64.dll") == 0);
        const bool isSteamUi     = (_wcsicmp(fn, L"steamui.dll") == 0);

        if (!isSteamClient && !isSteamUi) {
            return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
        }

        // Host process (steam.exe) is loading steamui or steamclient.
        // Block until LumaCore background bootstrap has finished all pattern
        // downloads and installed critical hooks.
        if (!g_isReady.load(std::memory_order_acquire) && g_hBootstrapReadyEvent) {
            LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"hold\" \"module\" \"{}\" \"tid\" {}",
                          isSteamClient ? "steamclient64" : "steamui",
                          GetCurrentThreadId());
            WaitForSingleObject(g_hBootstrapReadyEvent, 45000); // 45s safety timeout
        }

        // Redirection leg: steamclient64.dll loads are diverted to lcoverlay.dll
        if (isSteamClient) {
            if (DiversionPath[0] != '\0') {
                wchar_t wDiversion[MAX_PATH] = {};
                MultiByteToWideChar(CP_ACP, 0, DiversionPath, -1, wDiversion, MAX_PATH);
                LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"redirect\" \"to\" \"{}\"", DiversionPath);
                HMODULE hOverlay = oLoadLibraryExW(wDiversion, hFile, dwFlags);
                if (!hOverlay && diversion_hModule) {
                    hOverlay = diversion_hModule;
                }
                return hOverlay;
            }
            if (diversion_hModule) {
                return diversion_hModule;
            }
        }

        // SteamUI leg: map steamui.dll, then immediately attach hooks before returning
        if (isSteamUi) {
            HMODULE hSteamUI = oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
            constexpr DWORD kDataFileFlags = LOAD_LIBRARY_AS_DATAFILE |
                                            LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                            LOAD_LIBRARY_AS_IMAGE_RESOURCE;
            if (hSteamUI && !(dwFlags & kDataFileFlags)) {
                LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"hook_steamui\"");
                CoreInit::Patterns::TrySteamUiLateInstall("loader-gate");
            }
            return hSteamUI;
        }

        return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }

    void Install() {
        bool expected = false;
        if (!g_installed.compare_exchange_strong(expected, true)) {
            return;
        }

        g_hBootstrapReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        HMODULE hKernel = GetModuleHandleW(L"kernelbase.dll");
        if (!hKernel) {
            hKernel = GetModuleHandleW(L"kernel32.dll");
        }
        if (!hKernel) return;

        oLoadLibraryExW = reinterpret_cast<LoadLibraryExW_t>(
            GetProcAddress(hKernel, "LoadLibraryExW"));
        if (!oLoadLibraryExW) return;

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oLoadLibraryExW),
                     reinterpret_cast<PVOID>(hkLoadLibraryExW));
        DetourTransactionCommit();

        LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"installed\"");
    }

    void SetInitThreadId(DWORD tid) {
        g_initThreadId = tid;
    }

    void SignalBootstrapReady() {
        g_isReady.store(true, std::memory_order_release);
        if (g_hBootstrapReadyEvent) {
            SetEvent(g_hBootstrapReadyEvent);
        }
        LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"signaled\"");
    }

    void Uninstall() {
        bool expected = true;
        if (!g_installed.compare_exchange_strong(expected, false)) {
            return;
        }

        SignalBootstrapReady();

        if (oLoadLibraryExW) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(reinterpret_cast<PVOID*>(&oLoadLibraryExW),
                         reinterpret_cast<PVOID>(hkLoadLibraryExW));
            DetourTransactionCommit();
            oLoadLibraryExW = nullptr;
        }

        if (g_hBootstrapReadyEvent) {
            CloseHandle(g_hBootstrapReadyEvent);
            g_hBootstrapReadyEvent = nullptr;
        }
    }

    bool IsBootstrapReady() {
        return g_isReady.load(std::memory_order_acquire);
    }

} // namespace LoaderGate
