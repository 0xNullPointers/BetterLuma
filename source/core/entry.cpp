// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "core/entry.h"
#include "core/LoaderGate.h"
#include "core/Orchestrator.h"
#include "hooks/capture/RuntimeCapture.h"
#include "hooks/client/PackagePatch.h"
#include "hooks/client/IpcMethodLoader.h"
#include "hooks/client/IpcDispatch.h"
#include "hooks/client/IpcHooks.h"
#include "hooks/client/DenuvoAuthenticator.h"
#include "patterns/PatternFetcher.h"
#include "runtime/DirWatch.h"
#include "runtime/HookStatus.h"
#include "runtime/IpcSpecLoader.h"
#include "runtime/BootDiag.h"
#include "runtime/LibraryInjector.h"
#include "runtime/CloudRedirectHost.h"
#include "runtime/BuildInfo.h"

#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <string_view>// ═══════════════════════════════════════════════════════════════════════
//  CoreInit - module SHA tracking + bootstrap pipeline
// ═══════════════════════════════════════════════════════════════════════

namespace CoreInit {

    // Latest-known cache-key SHA per module. HookStatus::SetShas takes the pair
    // at once, but the steamclient and steamui legs of the pattern fetch can
    // finish independently (the steamclient leg lands inline in Bootstrap::Run,
    // the steamui leg defers to LoadModuleWithPath when steamui.dll is not yet
    // mapped at bootstrap time). The helper threads each leg's update through
    // SetShas with the most recent pair so the on-disk Status File never
    // regresses an already-known SHA when only one module has just refreshed.
    struct ModuleShas {
        std::mutex  mtx;
        std::string clientSha;
        std::string uiSha;

        void Publish(std::string_view moduleName, std::string sha) {
            std::lock_guard<std::mutex> lk(mtx);
            if (moduleName == "steamclient") {
                clientSha = std::move(sha);
            } else if (moduleName == "steamui") {
                uiSha = std::move(sha);
            }
            HookStatus::SetShas(clientSha, uiSha);
        }
    };

    ModuleShas g_shas;

    // Set when the steamui leg has been resolved (either inline in Bootstrap::Run
    // when steamui.dll was already mapped, or via LoadModuleWithPath when
    // Steam's loader maps it later). Prevents the deferred-dispatch handler
    // in SteamUI::LoadModuleWithPath from running the fetch twice.
    std::atomic<bool> g_steamUiPatternDispatched{false};
    std::atomic<bool> g_steamUiRetryStarted{false};

    // ── Patterns ─────────────────────────────────────────────────────
    namespace Patterns {

        void TrySteamUiLateInstall(const char* reason) {
            HMODULE ui = GetModuleHandleA("steamui.dll");
            if (!ui) {
                HookStatus::RecordSteamUiLateRetry(
                    std::string(reason ? reason : "late") + ":waiting-module");
                return;
            }

            PatternFetcher::AssociateModule(ui, "steamui");
            PatternFetcher::PatternResult r = PatternFetcher::Get(ui);
            if (!r.ok) {
                bool expected = false;
                if (g_steamUiPatternDispatched.compare_exchange_strong(expected, true)) {
                    r = PatternFetcher::LoadFor(ui, "steamui");
                } else {
                    if (r.sha.empty() || (!r.ok && (r.source.empty() || r.source == "none")))
                        r = PatternFetcher::LoadFor(ui, "steamui");
                }
            }

            LOG_COREIN_INFO("\"stage\" \"Patterns\" \"module\" \"steamui\" \"deferred\" 1 \"sha\" \"{}\" \"entries\" {} \"ok\" {}",
                       r.sha.empty() ? "<unknown>" : r.sha,
                       static_cast<unsigned>(r.entries.size()),
                       r.ok ? 1 : 0);
            g_shas.Publish("steamui", r.sha);
            HookStatus::SetTomlAvailability("steamui", r.ok);
            HookStatus::RecordSteamUiLateRetry(
                std::string(reason ? reason : "late") + (r.ok ? ":toml-ok" : ":toml-missing"));
            if (r.ok) SteamUI::CoreHook();
        }

        void StartSteamUiLateRetryLoop() {
            bool expected = false;
            if (!g_steamUiRetryStarted.compare_exchange_strong(expected, true))
                return;
            HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                for (int i = 0; i < 60; ++i) {
                    if (GetModuleHandleA("steamui.dll")) {
                        TrySteamUiLateInstall("bootstrap-late-retry");
                        return 0;
                    }
                    HookStatus::RecordSteamUiLateRetry("bootstrap-late-retry:waiting-module");
                    Sleep(20);
                }
                HookStatus::SetSteamUiAttachState("steamui-not-loaded-after-retry", 60, false);
                HookStatus::RecordSteamUiLateRetry("bootstrap-late-retry:timeout");
                return 0;
            }, nullptr, 0, nullptr);
            if (h) CloseHandle(h);
        }

        // Fetches the steamui.dll TOML once SteamUI is mapped. Older code
        // passed nullptr into LoadFor here, so the deferred leg returned
        // before doing anything. Keep this tiny and let TrySteamUiLateInstall
        // own the real module lookup.
        void FetchSteamUIDeferred() {
            TrySteamUiLateInstall("loadmodulewithpath");
        }

    } // namespace Patterns

    // ── Diversion ────────────────────────────────────────────────────
    namespace Diversion {

        // Prepares the runtime paths and loads the hooked copy of steamclient64.dll.
        //
        // The diversion pattern: instead of hooking the real steamclient64.dll directly,
        // BetterLuma copies it to bin\lcoverlay.dll and loads that copy. The SteamUI hook then
        // intercepts steamui.dll's LoadModuleWithPath("steamclient64.dll") call and returns
        // diversion_hModule, so Steam's UI layer ends up using the hooked copy transparently.
        //
        // CopyFileA is retried up to 25 times (3 seconds total) because steamclient64.dll can be
        // briefly locked by the Steam service during early startup. Same retry logic for LoadLibraryA.
        // Returns false if either operation fails after all retries.
        // Verifies whether dstPath is already an exact, uncorrupted replica of srcPath.
        // Checks:
        // 1. Both files exist and have non-zero identical sizes.
        // 2. Exact timestamp equality (CompareFileTime == 0).
        // 3. 4 KB PE header comparison (DOS header, NT header, PE checksum, section table)
        //    to guard against truncated/corrupted copies without reading 40 MB.
        bool IsUpToDate(const char* srcPath, const char* dstPath) {
            WIN32_FILE_ATTRIBUTE_DATA srcAttr{}, dstAttr{};
            if (!GetFileAttributesExA(srcPath, GetFileExInfoStandard, &srcAttr) ||
                !GetFileAttributesExA(dstPath, GetFileExInfoStandard, &dstAttr)) {
                return false;
            }

            if (srcAttr.nFileSizeHigh != dstAttr.nFileSizeHigh ||
                srcAttr.nFileSizeLow  != dstAttr.nFileSizeLow  ||
                (srcAttr.nFileSizeHigh == 0 && srcAttr.nFileSizeLow == 0)) {
                return false;
            }

            if (CompareFileTime(&srcAttr.ftLastWriteTime, &dstAttr.ftLastWriteTime) != 0) {
                return false;
            }

            HANDLE hSrc = CreateFileA(srcPath, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hSrc == INVALID_HANDLE_VALUE) return false;

            HANDLE hDst = CreateFileA(dstPath, GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hDst == INVALID_HANDLE_VALUE) {
                CloseHandle(hSrc);
                return false;
            }

            constexpr DWORD kHeaderSize = 4096;
            char srcBuf[kHeaderSize];
            char dstBuf[kHeaderSize];
            DWORD srcRead = 0, dstRead = 0;

            bool readOk = ReadFile(hSrc, srcBuf, kHeaderSize, &srcRead, nullptr) &&
                          ReadFile(hDst, dstBuf, kHeaderSize, &dstRead, nullptr);
            CloseHandle(hSrc);
            CloseHandle(hDst);

            if (!readOk || srcRead == 0 || srcRead != dstRead) {
                return false;
            }

            return memcmp(srcBuf, dstBuf, srcRead) == 0;
        }

        bool CopyWithRetry(const char* srcPath, const char* dstPath, int maxRetries, int delayMs) {
            int attempts = 0;
            while (!CopyFileA(srcPath, dstPath, FALSE)) {
                if (++attempts >= maxRetries) {
                    LOG_COREIN_ERROR("\"stage\" \"Diversion\" \"err\" \"copy-fail\" \"from\" \"{}\" \"to\" \"{}\"", srcPath, dstPath);
                    return false;
                }
                LOG_COREIN_WARN("\"stage\" \"Diversion\" \"act\" \"copy-retry\" {} err={}", attempts, GetLastError());
                Sleep(delayMs);
            }

            // Sync destination timestamps with source to guarantee exact CompareFileTime equality
            WIN32_FILE_ATTRIBUTE_DATA srcAttr{};
            if (GetFileAttributesExA(srcPath, GetFileExInfoStandard, &srcAttr)) {
                HANDLE hDst = CreateFileA(dstPath, FILE_WRITE_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hDst != INVALID_HANDLE_VALUE) {
                    SetFileTime(hDst, &srcAttr.ftCreationTime, &srcAttr.ftLastAccessTime, &srcAttr.ftLastWriteTime);
                    CloseHandle(hDst);
                }
            }
            return true;
        }

        // Copies steamclient64.dll to bin\lcoverlay.dll (if not already up to date)
        // and loads the copy so Steam's UI layer uses the diversion module.
        bool PrepareAndLoad()
        {
            constexpr int kCopyRetries  = 25;
            constexpr int kLoadRetries  = 25;
            constexpr int kRetryDelayMs = 120;

            if (SteamInstallPath[0] == '\0') {
                HMODULE hSelf = nullptr;
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&PrepareAndLoad), &hSelf);
                wchar_t wInstallPath[MAX_PATH] = {};
                if (!GetModuleFileNameW(hSelf, wInstallPath, MAX_PATH))
                    return false;
                wchar_t* lastSlash = wcsrchr(wInstallPath, L'\\');
                if (lastSlash) *lastSlash = L'\0';

                wchar_t wShort[MAX_PATH] = {};
                if (GetShortPathNameW(wInstallPath, wShort, MAX_PATH) > 0) {
                    WideCharToMultiByte(CP_ACP, 0, wShort, -1, SteamInstallPath, MAX_PATH, nullptr, nullptr);
                } else {
                    WideCharToMultiByte(CP_ACP, 0, wInstallPath, -1, SteamInstallPath, MAX_PATH, nullptr, nullptr);
                }
            }

            sprintf_s(SteamclientPath, MAX_PATH, "%s\\steamclient64.dll",   SteamInstallPath);
            sprintf_s(SteamuiPath,     MAX_PATH, "%s\\steamui.dll",         SteamInstallPath);
            sprintf_s(DiversionPath,   MAX_PATH, "%s\\bin\\lcoverlay.dll",  SteamInstallPath);
            sprintf_s(LuaDir,          MAX_PATH, "%s\\config\\stplug-in",   SteamInstallPath);
            sprintf_s(ConfigPath,      MAX_PATH, "%s\\BetterLuma.toml",     SteamInstallPath);
            sprintf_s(PayloadPath,     MAX_PATH, "%s\\BetterLumaPayload.dll", SteamInstallPath);
            // ensure bin\ directory exists before copying
            char binDir[MAX_PATH];
            sprintf_s(binDir, MAX_PATH, "%s\\bin", SteamInstallPath);
            CreateDirectoryA(binDir, nullptr);

            bool upToDate = IsUpToDate(SteamclientPath, DiversionPath);
            if (upToDate) {
                LOG_COREIN_INFO("\"stage\" \"Diversion\" \"act\" \"up-to-date\" \"path\" \"{}\"", DiversionPath);
            } else {
                LOG_COREIN_INFO("\"stage\" \"Diversion\" \"act\" \"copy-start\" \"from\" \"{}\" \"to\" \"{}\"", SteamclientPath, DiversionPath);
                if (!CopyWithRetry(SteamclientPath, DiversionPath, kCopyRetries, kRetryDelayMs)) {
                    return false;
                }
            }

            int attempts = 0;
            while (!(diversion_hModule = LoadLibraryA(DiversionPath))) {
                if (upToDate) {
                    // Cached copy failed to load; force a fresh copy and retry
                    LOG_COREIN_WARN("\"stage\" \"Diversion\" \"act\" \"cached-load-failed-recopying\" err={}", GetLastError());
                    upToDate = false;
                    if (CopyWithRetry(SteamclientPath, DiversionPath, kCopyRetries, kRetryDelayMs)) {
                        continue;
                    }
                }
                if (++attempts >= kLoadRetries) {
                    LOG_COREIN_ERROR("\"stage\" \"Diversion\" \"err\" \"load-fail\" \"path\" \"{}\"", DiversionPath);
                    return false;
                }
                LOG_COREIN_WARN("\"stage\" \"Diversion\" \"act\" \"load-retry\" {} err={}", attempts, GetLastError());
                Sleep(kRetryDelayMs);
            }
            LOG_COREIN_INFO("\"stage\" \"Diversion\" \"act\" \"loaded\" \"path\" \"{}\"", DiversionPath);
            HookStatus::SetDiversionState(true, "loaded");
            return true;
        }

    } // namespace Diversion

    // ── BuildId ──────────────────────────────────────────────────────
    namespace BuildId {

        // Reads the current Steam build number from steam.exe.
        void Detect() {
            using GetBootstrapperVersion_t = int64_t (*)();
            HMODULE hSteam = GetModuleHandleA("steam.exe");
            if (!hSteam) {
                LOG_COREIN_WARN("\"stage\" \"BuildId\" \"err\" \"steam-not-loaded\"");
                return;
            }
            auto fn = reinterpret_cast<GetBootstrapperVersion_t>(
                GetProcAddress(hSteam, "GetBootstrapperVersion"));
            if (!fn) {
                LOG_COREIN_WARN("\"stage\" \"BuildId\" \"err\" \"no-export\"");
                return;
            }
            g_steamBuildId = std::to_string(fn());
            LOG_COREIN_INFO("\"stage\" \"BuildId\" \"value\" \"{}\"", g_steamBuildId);
        }

    } // namespace BuildId

    // ── Bootstrap ────────────────────────────────────────────────────
    namespace Bootstrap {

        // Worker thread that runs all real startup work outside of DllMain.
        // Windows holds the loader lock during DllMain, which means calling LoadLibrary, doing
        // file I/O, or installing Detours hooks from DllMain risks a deadlock. Spinning up a
        // separate thread lets us do all of that safely once the loader lock is released.
        DWORD Run(HMODULE selfModule)
        {
            LoaderGate::SetInitThreadId(GetCurrentThreadId());
            Logger::Init(selfModule);

            // Compute SteamInstallPath and ConfigPath early
            wchar_t wSelf[MAX_PATH] = {};
            if (GetModuleFileNameW(selfModule, wSelf, MAX_PATH)) {
                wchar_t* ls = wcsrchr(wSelf, L'\\');
                if (ls) *ls = L'\0';
                wchar_t wShort[MAX_PATH] = {};
                if (GetShortPathNameW(wSelf, wShort, MAX_PATH) > 0) {
                    WideCharToMultiByte(CP_ACP, 0, wShort, -1, SteamInstallPath, MAX_PATH, nullptr, nullptr);
                } else {
                    WideCharToMultiByte(CP_ACP, 0, wSelf, -1, SteamInstallPath, MAX_PATH, nullptr, nullptr);
                }
            }
            sprintf_s(ConfigPath,      MAX_PATH, "%s\\BetterLuma.toml",     SteamInstallPath);
            sprintf_s(SteamclientPath, MAX_PATH, "%s\\steamclient64.dll",   SteamInstallPath);
            sprintf_s(SteamuiPath,     MAX_PATH, "%s\\steamui.dll",         SteamInstallPath);
            sprintf_s(DiversionPath,   MAX_PATH, "%s\\bin\\lcoverlay.dll",  SteamInstallPath);

            // Load config and init ALL module loggers before any LOG_COREIN_* call
            Settings::Load(ConfigPath);
            Logger::InitModules();

            LOG_COREIN_INFO("\"stage\" \"Bootstrap\" \"act\" \"start\" \"version\" \"{}\" \"build\" \"{}\"",
                            BuildInfo::Version(), BuildInfo::BuildStamp());
            HookStatus::SetStartupPhase("start");

            // Build id first so HookStatus has a value to surface even if the
            // diversion copy below fails.
            BuildId::Detect();
            HookStatus::SetBuildId(g_steamBuildId);

            // ── TASK 1: Dispatch SteamUI pattern loading asynchronously on background thread ──
            wchar_t wSteamuiPath[MAX_PATH] = {};
            MultiByteToWideChar(CP_ACP, 0, SteamuiPath, -1, wSteamuiPath, MAX_PATH);
            auto steamUiFuture = std::async(std::launch::async, [wSteamuiPath]() -> PatternFetcher::PatternResult {
                try {
                    return PatternFetcher::LoadForPath(wSteamuiPath, "steamui", GetModuleHandleA("steamui.dll"));
                } catch (const std::exception& e) {
                    LOG_COREIN_ERROR("\"stage\" \"Patterns\" \"module\" \"steamui\" \"err\" \"exception\" \"msg\" \"{}\"", e.what());
                    return PatternFetcher::PatternResult{};
                } catch (...) {
                    LOG_COREIN_ERROR("\"stage\" \"Patterns\" \"module\" \"steamui\" \"err\" \"unknown_exception\"");
                    return PatternFetcher::PatternResult{};
                }
            });

            // ── TASK 2: Dispatch Lua directory parsing asynchronously on background thread ──
            std::vector<std::string> watchDirs = Settings::luaPaths;
            std::filesystem::path defaultLuaPath = std::filesystem::path(LuaDir).lexically_normal().make_preferred();
            bool hasDefault = false;
            for (const auto& dir : watchDirs) {
                if (std::filesystem::path(dir).lexically_normal().make_preferred() == defaultLuaPath) {
                    hasDefault = true;
                    break;
                }
            }
            if (!hasDefault) {
                watchDirs.push_back(defaultLuaPath.string());
            }

            auto luaFuture = std::async(std::launch::async, [watchDirs]() {
                try {
                    for (const auto& dir : watchDirs)
                        LuaLoader::ParseDirectory(dir);
                } catch (const std::exception& e) {
                    LOG_COREIN_ERROR("\"stage\" \"Lua\" \"err\" \"exception\" \"msg\" \"{}\"", e.what());
                } catch (...) {
                    LOG_COREIN_ERROR("\"stage\" \"Lua\" \"err\" \"unknown_exception\"");
                }
            });

            // ── TASK 3: Main thread: Prepare diversion module ──
            if (!Diversion::PrepareAndLoad()) {
                LOG_COREIN_ERROR("\"stage\" \"Bootstrap\" \"err\" \"diversion-fail\"");
                HookStatus::SetTomlAvailability("steamclient", false);
                HookStatus::SetTomlAvailability("steamui", false);
                HookStatus::WriteToDisk();
                LoaderGate::SignalBootstrapReady();
                return 1;
            }

            // ── Steamclient leg: synchronous cache + network ─────────
            PatternFetcher::PatternResult pcResult =
                PatternFetcher::LoadFor(diversion_hModule, "steamclient");
            LOG_COREIN_INFO("\"stage\" \"Patterns\" \"module\" \"steamclient\" \"sha\" \"{}\" \"entries\" {} \"ok\" {}",
                       pcResult.sha.empty() ? "<unknown>" : pcResult.sha,
                       static_cast<unsigned>(pcResult.entries.size()),
                       pcResult.ok ? 1 : 0);

            // ── Parallelize IPC spec loader and IPC method loader ────
            auto ipcSpecFuture = std::async(std::launch::async, [&pcResult]() {
                try {
                    IpcSpecLoader::Load(pcResult.sha);
                } catch (const std::exception& e) {
                    LOG_COREIN_ERROR("\"stage\" \"IpcSpecLoader\" \"err\" \"exception\" \"msg\" \"{}\"", e.what());
                } catch (...) {
                    LOG_COREIN_ERROR("\"stage\" \"IpcSpecLoader\" \"err\" \"unknown_exception\"");
                }
            });

            try {
                IpcLoader::Load(SteamclientPath, pcResult.sha);
            } catch (const std::exception& e) {
                LOG_COREIN_ERROR("\"stage\" \"IpcLoader\" \"err\" \"exception\" \"msg\" \"{}\"", e.what());
            } catch (...) {
                LOG_COREIN_ERROR("\"stage\" \"IpcLoader\" \"err\" \"unknown_exception\"");
            }

            ipcSpecFuture.get();

            // ── Attach ALL core hooks on steamclient ─────────────────
            HookStatus::SetStartupPhase("installing_critical_hooks");
            PackagePatch::Install();
            SteamCapture::Install();
            IpcHooks::Install();
            DenuvoAuth::Init();
            BetterLuma::Attach();

            // Join Lua background parsing & inject startup package
            // Must complete before opening the client gate so all depot/ownership
            // collections and package injections are 100% frozen and ready before
            // Steam can unblock and invoke hooked methods (HasDepot, IsOwned, etc.).
            luaFuture.get();
            SteamCapture::TryStartupPackageInjection("lua-loaded");
            DirWatch::Start(watchDirs);

            // ── STAGE 1 COMPLETE: Open client gate ───────────────────
            // Steam's loader thread unblocks immediately, returning the diverted
            // steamclient module handle and allowing Steam symbol resolution to proceed.
            LoaderGate::SignalClientReady();
            SignalHooksInstalled();
            LOG_COREIN_INFO("\"stage\" \"Bootstrap\" \"act\" \"client_gate_opened\"");

            // ── Diagnostics capture ──────────────────────────────────
            BootDiag::Capture(pcResult.sha);
            if (!IpcSpecLoader::IsLoaded() && !IpcLoader::IsLoaded())
                BootDiag::ReportMissing();

            // ── Wait for SteamUI patterns to finish loading ──────────
            PatternFetcher::PatternResult puResult = steamUiFuture.get();
            LOG_COREIN_INFO("\"stage\" \"Patterns\" \"module\" \"steamui\" \"sha\" \"{}\" \"entries\" {} \"ok\" {}",
                       puResult.sha.empty() ? "<unknown>" : puResult.sha,
                       static_cast<unsigned>(puResult.entries.size()),
                       puResult.ok ? 1 : 0);
            if (!puResult.ok) {
                LOG_COREIN_INFO("\"stage\" \"Patterns\" \"module\" \"steamui\" \"act\" \"deferred\"");
                Patterns::StartSteamUiLateRetryLoop();
            }

            // SHAs first, then per-module availability.
            {
                std::lock_guard<std::mutex> lk(g_shas.mtx);
                g_shas.clientSha = pcResult.sha;
                g_shas.uiSha     = puResult.sha;
            }
            HookStatus::SetShas(pcResult.sha, puResult.sha);
            HookStatus::SetTomlAvailability("steamclient", pcResult.ok);
            HookStatus::SetTomlAvailability("steamui",     puResult.ok);
            HookStatus::SetStartupPhase("patterns_loaded");
            HookStatus::WriteToDisk();

            // ── SteamUI::CoreHook() must be installed to catch LoadModuleWithPath ──
            HookStatus::SetStartupPhase("installing_hooks");
            SteamUI::CoreHook();

            // Initialize CloudRedirect host (loads DLL if enabled in settings)
            CloudRedirectHost::Initialize(SteamInstallPath);

            HookStatus::SetStartupPhase("hooks_complete");
            HookStatus::WriteToDisk();
            LOG_COREIN_INFO("\"stage\" \"Bootstrap\" \"act\" \"complete\"");

            // ── STAGE 2 COMPLETE: Open UI gate ───────────────────────
            LoaderGate::SignalUiReady();
            return 0;
        }

    } // namespace Bootstrap

} // namespace CoreInit

static HANDLE g_hHooksInstalledEvent = nullptr;

void SignalHooksInstalled() {
    g_HooksInstalled.store(true, std::memory_order_release);
    if (g_hHooksInstalledEvent) {
        SetEvent(g_hHooksInstalledEvent);
    }
}

bool WaitForHooksInstalled(DWORD timeoutMs) {
    if (g_HooksInstalled.load(std::memory_order_acquire)) {
        return true;
    }
    if (!g_hHooksInstalledEvent) {
        return false;
    }
    DWORD res = WaitForSingleObject(g_hHooksInstalledEvent, timeoutMs);
    return (res == WAIT_OBJECT_0) || g_HooksInstalled.load(std::memory_order_acquire);
}

// ═══════════════════════════════════════════════════════════════════════
//  DllMain
// ═══════════════════════════════════════════════════════════════════════

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_hHooksInstalledEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Pin the module so a stray FreeLibrary cannot unmap BetterLuma while
        // hooks and worker threads are still live. Failure is non-fatal; we
        // just lose the unmap protection and continue attach.
        HMODULE selfPin = nullptr;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCSTR>(&DllMain), &selfPin)) {
            LOG_COREIN_WARN("\"stage\" \"DllMain\" \"err\" \"pin-fail\" err={}", GetLastError());
        }
        LoaderGate::Install();
        // Start Bootstrap::Run on a worker thread to do all real work
        // outside the loader lock.
        // DllMain must return quickly and must not call LoadLibrary, open files,
        // or install hooks - doing so under the loader lock causes deadlocks.
        g_InitThread = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            return CoreInit::Bootstrap::Run(static_cast<HMODULE>(param));
        }, hModule, 0, nullptr);
        if (g_InitThread) {
            LoaderGate::SetInitThreadId(GetThreadId(g_InitThread));
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        LoaderGate::Uninstall();
        if (g_hHooksInstalledEvent) {
            SetEvent(g_hHooksInstalledEvent);
        }
        if (g_InitThread) {
            WaitForSingleObject(g_InitThread, 5000);
            CloseHandle(g_InitThread);
            g_InitThread = nullptr;
        }
        if (g_hHooksInstalledEvent) {
            CloseHandle(g_hHooksInstalledEvent);
            g_hHooksInstalledEvent = nullptr;
        }
        if (g_HooksInstalled.load()) {
            DirWatch::Stop();
            // Shutdown CloudRedirect host if initialized
            CloudRedirectHost::Shutdown();
            if (pvReserved == nullptr) {
                SteamUI::CoreUnhook();
                BetterLuma::Detach();
            }
        }
    }

    return TRUE;
}

void DispatchSteamUiPatternFetch() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            CoreInit::Patterns::FetchSteamUIDeferred();
            return 0;
        }, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    });
}
