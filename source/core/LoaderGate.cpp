// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
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

    static LoadLibraryExW_t  oLoadLibraryExW     = nullptr;
    static HANDLE            g_hClientReadyEvent = nullptr;
    static HANDLE            g_hUiReadyEvent     = nullptr;
    static std::atomic<bool> g_isClientReady{false};
    static std::atomic<bool> g_isUiReady{false};
    static std::atomic<bool> g_installed{false};
    static DWORD             g_initThreadId = 0;

    static const wchar_t* GetFileNameOnly(LPCWSTR path) {
        if (!path) return L"";
        const wchar_t* p1 = wcsrchr(path, L'\\');
        const wchar_t* p2 = wcsrchr(path, L'/');
        if (p2 && (!p1 || p2 > p1)) p1 = p2;
        return p1 ? p1 + 1 : path;
    }

#ifdef BETTERLUMA_LOGGING_ENABLED
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

        // Stage 1 Gate: steamclient64.dll loads are held until diversion is prepared
        // and core steamclient hooks are installed.
        if (isSteamClient) {
            if (!g_isClientReady.load(std::memory_order_acquire) && g_hClientReadyEvent) {
                LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"hold\" \"module\" \"steamclient64\" \"tid\" {}",
                              GetCurrentThreadId());
                WaitForSingleObject(g_hClientReadyEvent, 45000); // 45s safety timeout
            }

            // Redirection leg: steamclient64.dll loads are diverted to lcoverlay.dll
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

        // Stage 2 Gate: steamui.dll loads are held until UI patterns and full bootstrap are complete
        if (isSteamUi) {
            if (!g_isUiReady.load(std::memory_order_acquire) && g_hUiReadyEvent) {
                LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"hold\" \"module\" \"steamui\" \"tid\" {}",
                              GetCurrentThreadId());
                WaitForSingleObject(g_hUiReadyEvent, 45000); // 45s safety timeout
            }

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

        g_hClientReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hUiReadyEvent     = CreateEventW(nullptr, TRUE, FALSE, nullptr);

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

    void SignalClientReady() {
        g_isClientReady.store(true, std::memory_order_release);
        if (g_hClientReadyEvent) {
            SetEvent(g_hClientReadyEvent);
        }
        LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"client_signaled\"");
    }

    void SignalUiReady() {
        g_isUiReady.store(true, std::memory_order_release);
        if (g_hUiReadyEvent) {
            SetEvent(g_hUiReadyEvent);
        }
        LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"ui_signaled\"");
    }

    void SignalBootstrapReady() {
        SignalClientReady();
        SignalUiReady();
        LOG_GATE_INFO("\"stage\" \"LoaderGate\" \"act\" \"bootstrap_signaled\"");
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

        if (g_hClientReadyEvent) {
            CloseHandle(g_hClientReadyEvent);
            g_hClientReadyEvent = nullptr;
        }
        if (g_hUiReadyEvent) {
            CloseHandle(g_hUiReadyEvent);
            g_hUiReadyEvent = nullptr;
        }
    }

    bool IsClientReady() {
        return g_isClientReady.load(std::memory_order_acquire);
    }

    bool IsUiReady() {
        return g_isUiReady.load(std::memory_order_acquire);
    }

    bool IsBootstrapReady() {
        return IsClientReady() && IsUiReady();
    }

} // namespace LoaderGate
