// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/capture/RuntimeCapture.h"
#include "hooks/Macros.h"
#include "hooks/client/SteamStubAuto.h"
#include "hooks/client/PackagePatch.h"
#include "hooks/ui/SteamUI.h"
#include "runtime/VehUtil.h"
#include "runtime/HookStatus.h"
#include "runtime/ProtectionProbe.h"
#include "runtime/Ticket.h"
#include "hooks/client/OnlineFixInject.h"
#include "hooks/client/DecryptionKeyHook.h"
#include "core/entry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {
    // ── function type aliases (alphabetical) ─────────────────────────────────
    using BuildSpawnEnvBlock_t           = __int64(*)(void*, uint64_t*, void*, void*, uint64_t*, void*, int, void*, void*, unsigned int, char);
    using CUtlBufferEnsureCapacity_t     = void*(*)(CUtlBuffer*, int);
    using CUtlMemoryGrow_t               = void*(*)(CUtlVector<AppId_t>*, int);
    using GetAppDataFromAppInfo_t        = int64(*)(void*, AppId_t, const char*, uint8*, int32);
    using GetAppIDForCurrentPipe_t       = AppId_t(*)(void*);
    using GetPackageInfo_t               = PackageInfo*(*)(void*, uint32, int64);
    using MarkLicenseAsChanged_t         = int64(*)(void*, uint32, bool);
    using ProcessPendingLicenseUpdates_t = bool(*)(void*);

    // ── X-macro lists ────────────────────────────────────────────────────────
    // One-shot int3: on hit, ctx->Rcx stored to the named output variable.
    #define VEH_GRAB_LIST(X)                         \
        X(GetAppDataFromAppInfo,  g_pCAppInfoCache)

    // Resolve-only (no int3).
    #define VEH_TRACK_LIST(X)            \
        X(CUtlBufferEnsureCapacity)      \
        X(CUtlMemoryGrow)               \
        X(ProcessPendingLicenseUpdates)

    // ── generated declarations ───────────────────────────────────────────────
    VEH_GRAB_LIST(VEH_DECL_CAPTURE)
    VEH_TRACK_LIST(VEH_DECL_RESOLVE)

    // ── Detours-captured pointers (set on first call, never cleared) ─────────
    // These replace the old VEH int3 captures for MarkLicenseAsChanged and
    // GetPackageInfo. Detours hooks fire on every call regardless of when they
    // were installed, so we capture pCUser and pCPackageInfo on the first
    // natural Steam call after login - even if that happens after startup.
    void* g_pCUser        = nullptr;
    void* g_pCPackageInfo = nullptr;
    std::atomic<bool> g_startupInjectionDone{false};
    std::atomic<bool> g_startupRetryThreadStarted{false};
    std::atomic<uint32> g_startupPackageMissLogs{0};

    // Forward declaration
    void TryStartupInjection(const char* reason);
    void StartStartupInjectionRetry();

    // ── per-session state ─────────────────────────────────────────────────────
    void*                 g_steamEngine        = nullptr;
    uint8_t*              g_spawnProcessTarget = nullptr;
    PVOID                 g_vehHandle          = nullptr;
    std::atomic<AppId_t>  g_OnlineFixRealAppId{0};
    std::atomic<uint32>   g_OnlineFixRouteMode{static_cast<uint32>(SteamCapture::OnlineFixRouteMode::None)};
    std::atomic<HSteamPipe> g_StatsScopePipe{0};
    std::atomic<AppId_t>  g_StatsScopeAppId{0};
    thread_local uint32   g_userStatsAppIdOverrideDepth = 0;
    std::unordered_map<AppId_t, std::string> g_GameNameCache;
    static std::vector<CaptureEntry> g_captures;

    // ── multi-game OnlineFix tracking ─────────────────────────────────────────
    struct OnlineFixAppEntry {
        AppId_t appId = 0;
        SteamCapture::OnlineFixRouteMode mode = SteamCapture::OnlineFixRouteMode::None;
        uint64_t registeredAt = 0;
        bool seenInPacket = false;
    };

    std::mutex                                     g_onlineFixMutex;
    std::unordered_map<AppId_t, OnlineFixAppEntry> g_onlineFixAppEntries;
    std::unordered_map<uint32_t, AppId_t>          g_onlineFixPidToAppId;

    static bool SafeReadUint64(const void* ptr, uint64_t& outVal) {
        if (!ptr) return false;
        __try {
            outVal = *reinterpret_cast<const uint64_t*>(ptr);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Safely write a uint64 value through an unverified pointer under structured exception handling
    static bool SafeWriteUint64(void* ptr, uint64_t val) {
        if (!ptr) return false;
        __try {
            *reinterpret_cast<uint64_t*>(ptr) = val;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Validate that a string pointer references accessible readable memory up to null terminator
    static bool SafeValidateString(const char* ptr, size_t maxLen = 4096) {
        if (!ptr) return false;
        __try {
            volatile char dummy = 0;
            for (size_t i = 0; i < maxLen; ++i) {
                dummy = ptr[i];
                if (dummy == '\0') return true;
            }
            return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ── PID transfer bypass hook state (prevents detachment of single-process games) ──
    uint8_t* g_pidTransferCheckTarget = nullptr;
    uint8_t  g_pidTransferCheckOriginalBytes[6]{};
    uint64_t g_pidTransferCheckJumpTarget = 0;
    uint64_t g_pidTransferCheckFallthroughTarget = 0;

    static uint8_t* FindPidTransferCheckSite(HMODULE hMod) {
        if (!hMod) return nullptr;
        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<uint8_t*>(hMod) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const uint8_t* start = reinterpret_cast<uint8_t*>(hMod) + sec->VirtualAddress;
            const size_t size = sec->Misc.VirtualSize;
            if (size < 16) continue;
            const uint8_t* end = start + size - 16;
            for (const uint8_t* p = start; p <= end; ++p) {
                // Pattern at steamclient64 InternalUpdateClientGame:
                // 48 39 07          cmp qword ptr [rdi], rax
                // 0F 85 ?? ?? ?? ?? jne 0x9c3559
                // 41 83 EE 01       sub r14d, 1
                if (p[0] == 0x48 && p[1] == 0x39 && p[2] == 0x07 &&
                    p[3] == 0x0F && p[4] == 0x85 &&
                    p[9] == 0x41 && p[10] == 0x83 && p[11] == 0xEE && p[12] == 0x01) {
                    LOG_MISC_INFO("FindPidTransferCheckSite: matched pattern @ RVA 0x{:X}",
                                  static_cast<uintptr_t>((p + 3) - reinterpret_cast<uint8_t*>(hMod)));
                    return const_cast<uint8_t*>(p + 3);
                }
            }
        }
        LOG_MISC_WARN("FindPidTransferCheckSite: pattern scan in executable sections found no match");
        return nullptr;
    }

    SteamCapture::OnlineFixRouteMode CurrentOnlineFixMode() {
        return static_cast<SteamCapture::OnlineFixRouteMode>(
            g_OnlineFixRouteMode.load(std::memory_order_acquire));
    }

    AppId_t ActiveRouteRealAppIdInternal() {
        if (SteamStubAuto::IsActive())
            return SteamStubAuto::RealAppId();
        AppId_t statsId = g_StatsScopeAppId.load(std::memory_order_acquire);
        if (statsId != 0)
            return statsId;
        return g_OnlineFixRealAppId.load(std::memory_order_acquire);
    }

    // ── GetAppIDForCurrentPipe Detours hook ───────────────────────────────────
    // Captures g_steamEngine (RCX = this) on first call and applies the scoped
    // real-appid override for IClientUserStats traffic.
    //
    // The override returns the real appid only when ALL of:
    //   1. SetUserStatsContext(true) is currently on the stack on this thread
    //      (g_userStatsAppIdOverrideDepth > 0)
    //   2. A route is active for this session (manual OnlineFix or SteamStub)
    //   3. The engine itself reports the Spacewar masquerade (appid == 480)
    //
    // Every other call path returns the engine's value untouched. That keeps
    // the lobby / friends / controller / RemoteStorage paths byte-identical
    // to the existing 480 behaviour. The depth counter is thread-local so
    // concurrent IPC pipes don't bleed into each other.
    LM_HOOK(GetAppIDForCurrentPipe, AppId_t, void* pEngine) {
        if (g_steamEngine == nullptr && pEngine != nullptr) {
            g_steamEngine = pEngine;
            LOG_MISC_INFO("Captured g_steamEngine: 0x{:X}",
                          reinterpret_cast<uint64_t>(pEngine));
        }
        AppId_t appid = oGetAppIDForCurrentPipe(pEngine);
        AppId_t real = ActiveRouteRealAppIdInternal();
        // OnlineFix 480 route handling: return 480 for all pipe traffic and only expose real in stats scope.
        // Route through 480 for OnlineFix networking/lobbies while stats scope returns real appid.
        if (real != 0 && (appid == real || appid == kOnlineFixAppId || SteamCapture::IsOnlineFixApp(appid))) {
            if (g_userStatsAppIdOverrideDepth > 0) {
                LOG_MISC_TRACE("GetAppIDForCurrentPipe: stats-scope override {} -> {}",
                               appid, real);
                return real;
            }
            return kOnlineFixAppId;
        }
        return appid;
    }

    // ── Language sync for OnlineFix route ──────────────────────────
    void SyncLanguageToSpacewar(AppId_t realAppId) {
        if (!realAppId) return;
        std::string realAcf = DecryptionKeyHook::FindAcfPath(realAppId);
        if (realAcf.empty()) return;
        std::string lang = DecryptionKeyHook::ReadAcfLanguage(realAcf);
        if (lang.empty()) return;
        DecryptionKeyHook::StoreSpacewarLanguage(lang.c_str());
        std::string swAcf = DecryptionKeyHook::FindAcfPath(kOnlineFixAppId);
        if (swAcf.empty() && SteamInstallPath[0]) {
            swAcf = std::string(SteamInstallPath) + "/steamapps/appmanifest_480.acf";
        }
        if (!swAcf.empty()) {
            DecryptionKeyHook::WriteAcfLanguage(swAcf, lang);
            LOG_MISC_INFO("SpawnProcess: 480 ACF language set to {}", lang);
        }
        std::string csKey = "UserAppConfig\\" + std::to_string(kOnlineFixAppId);
        std::string blob = DecryptionKeyHook::BuildUserAppConfigBlob(lang);
        if (!blob.empty()) {
            bool ok = DecryptionKeyHook::SetConfigStoreStringEx(csKey.c_str(), blob.c_str(),
                                                                k_EConfigStoreUserLocal);
            LOG_MISC_INFO("SpawnProcess: ConfigStore UserAppConfig store=3 ok={} key={} lang={}",
                          ok, csKey, lang);
        }
    }

    // Manual -onlinefix keeps the old overlay trick. Dedicated SteamStub auto
    // keeps CGameID as 480 for Steam tracking, and exposes only overlay identity
    // to the real app.
    LM_HOOK(BuildSpawnEnvBlock, __int64,
            void* pThis, uint64_t* pCGameID, void* a3, void* env,
            uint64_t* pOverlayCGameID, void* a6, int a7,
            void* a8, void* a9, unsigned int a10, char a11)
    {
        SteamCapture::OnlineFixRouteMode mode = CurrentOnlineFixMode();
        AppId_t onlineFixRealAppId = g_OnlineFixRealAppId.load(std::memory_order_acquire);
        AppId_t steamStubRealAppId = SteamStubAuto::RealAppId();
        AppId_t overlayAppId = pOverlayCGameID
            ? static_cast<AppId_t>(*pOverlayCGameID & 0xFFFFFF) : 0;
        AppId_t cgameAppId = pCGameID
            ? static_cast<AppId_t>(*pCGameID & 0xFFFFFF) : 0;
        AppId_t realAppId = steamStubRealAppId ? steamStubRealAppId :
            (SteamCapture::IsOnlineFixApp(cgameAppId) ? cgameAppId : onlineFixRealAppId);

        uint64_t prevCGame = pCGameID ? *pCGameID : 0;
        uint64_t prevOverlay = pOverlayCGameID ? *pOverlayCGameID : 0;
        bool patchedOverlay = false;

        LOG_MISC_INFO("BuildSpawnEnvBlock: input routeMode={} pThis=0x{:X} env=0x{:X} pCGameID=0x{:X} rawCGame={:#x} pOverlay=0x{:X} rawOverlay={:#x} realAppId={}",
                      SteamCapture::OnlineFixRouteModeName(mode),
                      reinterpret_cast<uint64_t>(pThis),
                      reinterpret_cast<uint64_t>(env),
                      reinterpret_cast<uint64_t>(pCGameID),
                      prevCGame,
                      reinterpret_cast<uint64_t>(pOverlayCGameID),
                      prevOverlay,
                      realAppId);

        if (realAppId
            && (mode != SteamCapture::OnlineFixRouteMode::None || SteamStubAuto::IsActive())
            && pOverlayCGameID
            && overlayAppId == kOnlineFixAppId) {
            *pOverlayCGameID = (prevOverlay & ~static_cast<uint64_t>(0xFFFFFF))
                             | static_cast<uint64_t>(realAppId);
            patchedOverlay = true;
        }

        // OnlineFix child environment needs SteamAppId=480 while Steam tracks real appid.
        if (mode == SteamCapture::OnlineFixRouteMode::ManualFlag && pCGameID && realAppId) {
            *pCGameID = (prevCGame & ~static_cast<uint64_t>(0xFFFFFF))
                      | static_cast<uint64_t>(kOnlineFixAppId);
        }

        if (SteamStubAuto::IsActive() && steamStubRealAppId) {
            LOG_MISC_INFO("BuildSpawnEnvBlock: steamstub-auto dedicated cgame kept {:#x}->{:#x} overlay {:#x}->{:#x} realAppId={} env=0x{:X}",
                          prevCGame, pCGameID ? *pCGameID : 0,
                          prevOverlay, pOverlayCGameID ? *pOverlayCGameID : 0,
                          steamStubRealAppId,
                          reinterpret_cast<uint64_t>(env));
        } else if (patchedOverlay) {
            LOG_MISC_INFO("BuildSpawnEnvBlock: routeMode={} cgame kept {:#x}->{:#x} overlay {:#x}->{:#x} realAppId={} env=0x{:X}",
                          SteamCapture::OnlineFixRouteModeName(mode),
                          prevCGame, pCGameID ? *pCGameID : 0,
                          prevOverlay, pOverlayCGameID ? *pOverlayCGameID : 0,
                          realAppId,
                          reinterpret_cast<uint64_t>(env));
        } else {
            LOG_MISC_TRACE("BuildSpawnEnvBlock: routeMode={} cgame={} overlay={} realAppId={} env=0x{:X} (no patch)",
                           SteamCapture::OnlineFixRouteModeName(mode),
                           cgameAppId, overlayAppId, realAppId,
                           reinterpret_cast<uint64_t>(env));
        }
        __int64 result = oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                               pOverlayCGameID, a6, a7, a8, a9, a10, a11);

        // Restore real CGameID so Steam client's SpawnProcess tracks realAppId.
        if (mode == SteamCapture::OnlineFixRouteMode::ManualFlag && pCGameID) {
            *pCGameID = prevCGame;
        }

        if (realAppId && env) {
            const char* p = static_cast<const char*>(env);
            const char* end = p + 32768;
            while (p < end && *p) {
                if (strncmp(p, "SteamAppId=", 11) == 0) {
                    LOG_MISC_INFO("BuildSpawnEnvBlock: env SteamAppId={}", p + 11);
                    break;
                }
                p += strlen(p) + 1;
            }
        }

        return result;
    }

    // ── MarkLicenseAsChanged Detours hook ────────────────────────────────────
    // Captures pCUser (RCX = this) on first call, then triggers startup injection.
    // This replaces the old VEH int3 capture. Detours fires on every call
    // regardless of when the hook was installed, so we always get pCUser.
    LM_HOOK(MarkLicenseAsChanged, int64, void* pThis, uint32 packageId, bool bReloadAll) {
        bool justCaptured = false;
        if (!g_pCUser) {
            g_pCUser = pThis;
            justCaptured = true;
            LOG_PACKAGE_INFO("MarkLicenseAsChanged: captured pCUser=0x{:X}",
                             reinterpret_cast<uint64_t>(pThis));
        }
        if (justCaptured && g_startupInjectionDone.load(std::memory_order_acquire)
            && oMarkLicenseAsChanged) {
            oMarkLicenseAsChanged(pThis, 0, true);
            HookStatus::SetStartupRefreshState("startup-injected");
            LOG_PACKAGE_INFO("MarkLicenseAsChanged: pCUser captured, package 0 marked for reload");
        }
        TryStartupInjection("mark-license");
        return oMarkLicenseAsChanged(pThis, packageId, bReloadAll);
    }

    // ── GetPackageInfo Detours hook ───────────────────────────────────────────
    // Captures pCPackageInfo (RCX = this) on first call - kept for NotifyLicenseChanged.
    LM_HOOK(GetPackageInfo, PackageInfo*, void* pThis, uint32 packageId, int64 p3) {
        if (!g_pCPackageInfo) {
            g_pCPackageInfo = pThis;
            LOG_PACKAGE_INFO("GetPackageInfo: captured pCPackageInfo=0x{:X}",
                             reinterpret_cast<uint64_t>(pThis));
        }
        PackageInfo* result = oGetPackageInfo(pThis, packageId, p3);
        if (packageId == 0 && result && !g_startupInjectionDone.load(std::memory_order_acquire)) {
            PackagePatch::InjectIntoPackage0(result, LuaLoader::GetAllDepotIds(), "getpackageinfo-capture");
            TryStartupInjection("getpackageinfo-capture");
        }
        return result;
    }

    // ── Startup injection ─────────────────────────────────────────────────────
    PackageInfo* ResolvePackage0ForMutation(const char* reason) {
        PackageInfo* pPkg = nullptr;
        if (g_pCPackageInfo && oGetPackageInfo) {
            pPkg = oGetPackageInfo(g_pCPackageInfo, 0, 0);
        }
        if (!pPkg) pPkg = PackagePatch::GetPackage0();
        if (!pPkg) {
            const bool retry = reason && std::string_view(reason) == "post-hooks-retry";
            uint32 misses = retry
                ? g_startupPackageMissLogs.fetch_add(1, std::memory_order_acq_rel) + 1
                : 0;
            if (!retry || misses == 1 || misses % 20 == 0) {
                if (retry) {
                    LOG_PACKAGE_DEBUG("{}: package 0 not captured yet (misses={})",
                                      reason ? reason : "package0", misses);
                } else {
                    LOG_PACKAGE_DEBUG("{}: package 0 not captured yet",
                                      reason ? reason : "package0");
                }
            }
        }
        return pPkg;
    }

    void TryStartupInjection(const char* reason) {
        if (g_startupInjectionDone.load(std::memory_order_acquire)) return;
        const char* safeReason = reason ? reason : "startup";
        HookStatus::RecordStartupPackageRetry(safeReason);

        std::vector<AppId_t> additions = LuaLoader::GetAllDepotIds();
        if (additions.empty()) {
            LOG_PACKAGE_DEBUG("TryStartupInjection: no Lua app ids loaded (reason={})", safeReason);
            HookStatus::SetStartupRefreshState(PackagePatch::GetPackage0()
                ? "package0-captured-awaiting-lua"
                : "startup-waiting-lua");
            return;
        }

        PackageInfo* pPkg = ResolvePackage0ForMutation(safeReason);
        if (!pPkg) {
            HookStatus::SetStartupRefreshState("startup-waiting-packageinfo");
            return;
        }

        if (!PackagePatch::InjectIntoPackage0(pPkg, additions, safeReason)) {
            HookStatus::SetStartupRefreshState(
                pPkg->Status == EPackageStatus::Available
                    ? "startup-waiting-packageinfo"
                    : "package0-not-available");
            return;
        }

        g_startupInjectionDone.store(true, std::memory_order_release);
        HookStatus::SetStartupRefreshState(
            g_pCUser ? "startup-injected" : "startup-injected-local-only");
        HookStatus::SetPackageState(false, false, true, false);
        LOG_PACKAGE_INFO("TryStartupInjection: reason={} done, verified {} Lua ids in package 0{}",
                         safeReason, additions.size(),
                         g_pCUser ? "" : " (local-only)");
    }

    DWORD WINAPI StartupInjectionRetryThread(LPVOID) {
        for (int i = 0; i < 150; ++i) {
            if (g_startupInjectionDone.load(std::memory_order_acquire)) break;
            TryStartupInjection("post-hooks-retry");
            if (g_startupInjectionDone.load(std::memory_order_acquire)) break;
            Sleep(i < 40 ? 250 : 1000);
        }
        if (!g_startupInjectionDone.load(std::memory_order_acquire)) {
            HookStatus::SetStartupRefreshState("package0-not-seen-after-library-init");
            LOG_PACKAGE_WARN("StartupInjectionRetryThread: package 0 still missing after retry window");
        }
        return 0;
    }

    void StartStartupInjectionRetry() {
        if (g_startupRetryThreadStarted.exchange(true, std::memory_order_acq_rel)) return;
        HANDLE h = CreateThread(nullptr, 0, StartupInjectionRetryThread, nullptr, 0, nullptr);
        if (h) {
            CloseHandle(h);
        } else {
            LOG_PACKAGE_WARN("StartStartupInjectionRetry: CreateThread failed err={}", GetLastError());
        }
    }
    // Prevents substring matches like "-onlinefixpatch" triggering the -onlinefix path.
    static bool HasExactFlag(const char* cmd, const char* flag) {
        const char* p = cmd;
        size_t n = strlen(flag);
        while ((p = strstr(p, flag))) {
            bool startOk = (p == cmd || p[-1] == ' ');
            bool endOk   = (p[n] == '\0' || p[n] == ' ');
            if (startOk && endOk) return true;
            p += n;
        }
        return false;
    }

    // ── VEH handler ──────────────────────────────────────────────────────────
    // Scoped to this module's int3 sites only. Foreign RIP ->
    // EXCEPTION_CONTINUE_SEARCH so other VEH handlers still get their turn.
    thread_local bool g_inVeh = false;

    LONG CALLBACK VehHandler(PEXCEPTION_POINTERS pExInfo) {
        if (g_inVeh) return EXCEPTION_CONTINUE_SEARCH;
        struct VehGuard {
            VehGuard()  { g_inVeh = true; }
            ~VehGuard() { g_inVeh = false; }
        } guard;

        PCONTEXT ctx = pExInfo->ContextRecord;

        if (pExInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
            for (auto& cap : g_captures) {
                if (*cap.funcPtr && ctx->Rip == reinterpret_cast<uint64_t>(*cap.funcPtr)) {
                    *cap.outPtr = reinterpret_cast<void*>(ctx->Rcx);
                    *reinterpret_cast<uint8_t*>(*cap.funcPtr) = cap.restoreByte;
                    LOG_MISC_INFO("Captured {}: 0x{:X}", cap.label,
                                  reinterpret_cast<uint64_t>(*cap.outPtr));
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
            //                   pGameID, ...)
            // RCX=pCUser, RDX=pExePath, R8=pCommandLine, R9=pWorkingDir
            // [RSP+0x28]=pGameID (5th arg, pointer to CGameID, low 24 bits = AppId)
            if (g_spawnProcessTarget
                && ctx->Rip == reinterpret_cast<uint64_t>(g_spawnProcessTarget)) {
                // Safely read the CGameID pointer argument passed on stack at RSP+0x28
                uint64_t pGameIdAddr = 0;
                if (!SafeReadUint64(reinterpret_cast<const void*>(ctx->Rsp + 0x28), pGameIdAddr) || pGameIdAddr == 0) {
                    LOG_MISC_WARN("SpawnProcess: cannot read pGameID pointer from stack (RSP=0x{:X})", ctx->Rsp);
                    *g_spawnProcessTarget = 0x48;
                    ctx->EFlags |= 0x100;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                auto* pGameID = reinterpret_cast<uint64_t*>(pGameIdAddr);
                uint64_t gameIdVal = 0;
                if (!SafeReadUint64(pGameID, gameIdVal)) {
                    LOG_MISC_WARN("SpawnProcess: pGameID at 0x{:X} is unreadable", pGameIdAddr);
                    *g_spawnProcessTarget = 0x48;
                    ctx->EFlags |= 0x100;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                const char* exePath = reinterpret_cast<const char*>(ctx->Rdx);
                const char* cmdLine = reinterpret_cast<const char*>(ctx->R8);
                const char* workDir = reinterpret_cast<const char*>(ctx->R9);

                if (!SafeValidateString(exePath)) exePath = nullptr;
                if (!SafeValidateString(cmdLine)) cmdLine = nullptr;
                if (!SafeValidateString(workDir)) workDir = nullptr;

                AppId_t appId = static_cast<AppId_t>(gameIdVal & 0xFFFFFF);

                *g_spawnProcessTarget = 0x48;
                ctx->EFlags |= 0x100;

                bool hasDepot = LuaLoader::HasDepot(appId);
                bool owned = LuaLoader::IsOwned(appId);
                bool hasFlag = (cmdLine != nullptr) && HasExactFlag(cmdLine, "-onlinefix");
                bool knownSteamStub = Ticket::IsKnownSteamDrmApp(appId);
                ProtectionProbe::ScanResult steamStubProbe{};
                if (hasDepot && !owned && !knownSteamStub && exePath) {
                    steamStubProbe = ProtectionProbe::ScanBeforeSpawn(appId, exePath);
                }
                const bool probeSteamStub = steamStubProbe.valid && steamStubProbe.routeAccepted;
                const bool detectedSteamStub = knownSteamStub || probeSteamStub;
                const std::string steamStubSource = knownSteamStub ? "known-list" :
                    (steamStubProbe.valid && steamStubProbe.detected ? "pre-spawn-probe" : "none");
                const std::string steamStubMethod = knownSteamStub ? "known-list" :
                    (steamStubProbe.valid ? steamStubProbe.method : "skipped");
                const std::string steamStubImage = steamStubProbe.valid && steamStubProbe.detected
                    ? steamStubProbe.imagePath
                    : (exePath ? exePath : "");
                bool autoSteamStubCandidate = hasDepot && !owned && !hasFlag && detectedSteamStub;
                HookStatus::RecordSteamStubDetection(appId, steamStubSource, steamStubMethod,
                                                     steamStubImage, steamStubProbe.candidates,
                                                     detectedSteamStub,
                                                     knownSteamStub ? "accepted" : steamStubProbe.routeReason);

                LOG_MISC_INFO("SpawnProcess: hit appid={} hasDepot={} owned={} hasFlag={} autoSteamStubCandidate={} steamStubRouteAccepted={} steamStubRouteReason={} steamStubSource={} steamStubMethod={} steamStubImage=\"{}\" candidates={} exe=\"{}\" cmd=\"{}\"",
                              appId, hasDepot, owned, hasFlag, autoSteamStubCandidate,
                              detectedSteamStub,
                              knownSteamStub ? "accepted" : steamStubProbe.routeReason,
                              steamStubSource,
                              steamStubMethod,
                              steamStubImage,
                              steamStubProbe.candidates,
                              exePath ? exePath : "(null)",
                              cmdLine ? cmdLine : "(null)");

                Ticket::TicketPreflightResult ticketPreflight{};
                if (hasDepot) {
                    ticketPreflight = Ticket::EnsureRegistryTicketsForApp(appId, detectedSteamStub);
                    LOG_MISC_INFO("SpawnProcess: ticketPreflight={} ticketStatus={} ticketSource={} sourceAppId={} changed={} knownSteamStub={} steamStubSource={} steamStubMethod={}",
                                  Ticket::TicketPreflightActionName(ticketPreflight.action),
                                  Ticket::AppTicketStatusName(ticketPreflight.ticketStatus),
                                  Ticket::TicketPreflightSourceName(ticketPreflight.ticketSource),
                                  ticketPreflight.sourceAppId,
                                  ticketPreflight.changed,
                                  ticketPreflight.knownSteamStub,
                                  steamStubSource,
                                  steamStubMethod);
                }

                bool steamStubAuto = SteamStubAuto::ShouldActivate(appId, hasDepot, owned,
                                                                   hasFlag, detectedSteamStub);
                bool missingSteamStubTicket =
                    autoSteamStubCandidate
                    && ticketPreflight.ticketSource == Ticket::TicketPreflightSource::Missing;
                bool routeThrough480 = hasDepot && hasFlag;
                const char* routeReason = hasFlag ? "manual-flag" :
                                          (steamStubAuto ? "steamstub-auto" :
                                           (missingSteamStubTicket ? "steamstub-ticket-missing" : "none"));
                auto routeMode = hasFlag ? SteamCapture::OnlineFixRouteMode::ManualFlag
                                          : SteamCapture::OnlineFixRouteMode::None;

                if (routeThrough480) {
                    SteamCapture::SetOnlineFixRoute(appId, routeMode);
                    // Keep *pGameID as realAppId so Steam tracks process lifetime, UI Stop button, and playtime.
                    LOG_MISC_INFO("SpawnProcess: OnlineFix route active reason={} routeMode={} appid={}, tracking real app",
                                  routeReason, SteamCapture::OnlineFixRouteModeName(routeMode),
                                  appId);
                    SyncLanguageToSpacewar(appId);
                    if (detectedSteamStub) {
                        SteamStubAuto::Arm(appId, exePath, probeSteamStub ? steamStubProbe.imagePath : "");
                        LOG_MISC_INFO("SpawnProcess: -onlinefix with SteamStub DRM, armed ticket handler");
                    } else {
                        SteamStubAuto::Clear();
                    }
                    OnlineFixInject::QueueInjection(exePath, appId);
                } else if (steamStubAuto) {
                    SteamCapture::SetOnlineFixRoute(0, SteamCapture::OnlineFixRouteMode::None);
                    SteamStubAuto::Arm(appId, exePath, probeSteamStub ? steamStubProbe.imagePath : "");
                    SafeWriteUint64(pGameID, kOnlineFixAppId);
                    LOG_MISC_INFO("SpawnProcess: SteamStubAuto active reason={} appid {} -> {}, CGameID stays 480, overlay resolves real ticketSource={} sourceAppId={} steamStubSource={} steamStubMethod={} matchedImage=\"{}\"",
                                  routeReason, appId, kOnlineFixAppId,
                                  Ticket::TicketPreflightSourceName(ticketPreflight.ticketSource),
                                  ticketPreflight.sourceAppId,
                                  steamStubSource,
                                  steamStubMethod,
                                  probeSteamStub ? steamStubProbe.imagePath : "");
                    if (missingSteamStubTicket) {
                        LOG_MISC_WARN("SpawnProcess: SteamStubAuto ticket source missing for appid={}, route still active",
                                      appId);
                    }
                } else {
                    SteamCapture::SetOnlineFixRoute(0, SteamCapture::OnlineFixRouteMode::None);
                    SteamStubAuto::Clear();
                    if (missingSteamStubTicket) {
                        LOG_MISC_WARN("SpawnProcess: steamstub-ticket-missing appid={} ticketPreflight={} ticketStatus={} ticketSource={}",
                                      appId,
                                      Ticket::TicketPreflightActionName(ticketPreflight.action),
                                      Ticket::AppTicketStatusName(ticketPreflight.ticketStatus),
                                      Ticket::TicketPreflightSourceName(ticketPreflight.ticketSource));
                    }
                    LOG_MISC_DEBUG("SpawnProcess: 480 route not activated for appid={} "
                                   "(reason: {}{}{}{})",
                                   appId,
                                   !hasDepot ? "no-depot " : "",
                                   owned ? "owned " : "",
                                   !hasFlag && !autoSteamStubCandidate ? "no-flag " : "",
                                   routeThrough480 ? "(internal)" : "");
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // PID transfer bypass: intercepts steamclient64 InternalUpdateClientGame (RVA 0x9C33E9).
            // When an OnlineFix process initializes Steamworks with 480, Steam compares registering GameID 480
            // against the existing tracked game CGameID (e.g. 3405340).
            // At 0x9C33E9, 'jne 0x9C3559' jumps to remove the PID from the real game.
            // By redirecting RIP to +6 (fallthrough to sub r14d, 1), Steam retains the PID in the real game
            // while ALSO tracking it under 480! Playtime, Stop button, and multiplayer presence all stay alive!
            if (g_pidTransferCheckTarget
                && ctx->Rip == reinterpret_cast<uint64_t>(g_pidTransferCheckTarget)) {
                // Verify saved instruction bytes match JNE rel32 pattern (0F 85 ?? ?? ?? ??)
                if (g_pidTransferCheckOriginalBytes[0] != 0x0F || g_pidTransferCheckOriginalBytes[1] != 0x85) {
                    LOG_MISC_ERROR("PidTransferCheck: opcode verification failed (expected 0F 85, got {:02X} {:02X})",
                                   g_pidTransferCheckOriginalBytes[0], g_pidTransferCheckOriginalBytes[1]);
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                uint64_t newGameId = 0;
                SafeReadUint64(reinterpret_cast<const void*>(ctx->Rdi), newGameId);
                AppId_t newAppId = static_cast<AppId_t>(newGameId & 0xFFFFFF);
                uint64_t oldGameId = ctx->Rax;
                AppId_t oldAppId = static_cast<AppId_t>(oldGameId & 0xFFFFFF);
                uint32_t pid = static_cast<uint32_t>(ctx->R15);

                int32_t disp = *reinterpret_cast<const int32_t*>(g_pidTransferCheckOriginalBytes + 2);
                uint64_t jumpTarget = reinterpret_cast<uint64_t>(g_pidTransferCheckTarget + 6 + disp);
                uint64_t fallthroughTarget = reinterpret_cast<uint64_t>(g_pidTransferCheckTarget + 6);

                // Ensure computed targets match pre-validated initialization targets
                if (g_pidTransferCheckJumpTarget != 0 && jumpTarget != g_pidTransferCheckJumpTarget) {
                    LOG_MISC_ERROR("PidTransferCheck: jump target integrity check failed (expected 0x{:X}, got 0x{:X})",
                                   g_pidTransferCheckJumpTarget, jumpTarget);
                    return EXCEPTION_CONTINUE_SEARCH;
                }

                if (!oldAppId && pid) {
                    oldAppId = SteamCapture::GetOnlineFixAppForPid(pid);
                }
                // Verify if the process switching to 480 is an active OnlineFix title.
                bool isOnlineFixNew = (newAppId == kOnlineFixAppId);
                bool isOnlineFixOld = SteamCapture::IsOnlineFixApp(oldAppId)
                    || (pid && SteamCapture::GetOnlineFixAppForPid(pid) == oldAppId);

                LOG_MISC_INFO("PidTransferCheck: hit! pid={} oldAppId={} (oldGameId={:#x}) newAppId={} (newGameId={:#x}) isOFNew={} isOFOld={}",
                              pid, oldAppId, oldGameId, newAppId, newGameId, isOnlineFixNew, isOnlineFixOld);

                if (isOnlineFixNew && isOnlineFixOld) {
                    SteamCapture::AssociateOnlineFixPid(pid, oldAppId);
                    LOG_MISC_INFO("PidTransferCheck: [BYPASS] OnlineFix PID {} switching to 480 from real AppId {} - preventing detachment! Continuing at +6 (0x{:X})",
                                  pid, oldAppId, fallthroughTarget);
                    ctx->Rip = fallthroughTarget;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                if (newGameId == oldGameId) {
                    LOG_MISC_DEBUG("PidTransferCheck: [PASSTHROUGH] newGameId == oldGameId, fallthrough to 0x{:X}", fallthroughTarget);
                    ctx->Rip = fallthroughTarget;
                } else {
                    LOG_MISC_DEBUG("PidTransferCheck: [PASSTHROUGH] newGameId != oldGameId, jumping to 0x{:X}", jumpTarget);
                    ctx->Rip = jumpTarget;
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        if (pExInfo->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
            if (g_spawnProcessTarget
                && ctx->Rip == reinterpret_cast<uint64_t>(g_spawnProcessTarget + 5)) {
                *g_spawnProcessTarget = 0xCC;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace SteamCapture {
    void Install() {
        if (g_vehHandle) return;

        VEH_TRACK_LIST(VEH_LOCATE)

        // GetAppDataFromAppInfo: lives in CAppInfoCache. The xref is the
        // unique "name_localized/%s" literal the analyzer pinned the function
        // by. LM_CAPTURE resolves the hook through the per-build TOML; the
        // xref is only kept as informational metadata for log readers and
        // for future TOML re-publishing.
        {
            static constexpr StringXRefSig kAppDataStrSigs[] = {
                {"GetAppDataFromAppInfo", "name_localized/%s"},
            };
            LM_CAPTURE(GetAppDataFromAppInfo, g_pCAppInfoCache,
                       kAppDataStrSigs, std::size(kAppDataStrSigs));
        }

        if (auto* _sp_ = ByteSearch(diversion_hModule, "SpawnProcess")) {
            g_spawnProcessTarget = static_cast<uint8_t*>(_sp_);
            LOG_MISC_INFO("SpawnProcess: VEH trap armed @ 0x{:X}", reinterpret_cast<uintptr_t>(_sp_));
            VehUtil::ArmInt3(_sp_);
        } else {
            LOG_MISC_WARN("SpawnProcess: ByteSearch returned null, VEH trap NOT armed");
        }

        g_pidTransferCheckTarget = FindPidTransferCheckSite(diversion_hModule);
        if (g_pidTransferCheckTarget) {
            memcpy(g_pidTransferCheckOriginalBytes, g_pidTransferCheckTarget, sizeof(g_pidTransferCheckOriginalBytes));
            // Verify original instruction bytes match JNE rel32 pattern (0F 85 ?? ?? ?? ??)
            if (g_pidTransferCheckOriginalBytes[0] == 0x0F && g_pidTransferCheckOriginalBytes[1] == 0x85) {
                int32_t disp = *reinterpret_cast<const int32_t*>(g_pidTransferCheckOriginalBytes + 2);
                g_pidTransferCheckJumpTarget = reinterpret_cast<uint64_t>(g_pidTransferCheckTarget + 6 + disp);
                g_pidTransferCheckFallthroughTarget = reinterpret_cast<uint64_t>(g_pidTransferCheckTarget + 6);
                LOG_MISC_INFO("PidTransferCheck: VEH trap armed @ 0x{:X}, orig bytes: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} (jump: 0x{:X}, fallthrough: 0x{:X})",
                              reinterpret_cast<uintptr_t>(g_pidTransferCheckTarget),
                              g_pidTransferCheckOriginalBytes[0], g_pidTransferCheckOriginalBytes[1],
                              g_pidTransferCheckOriginalBytes[2], g_pidTransferCheckOriginalBytes[3],
                              g_pidTransferCheckOriginalBytes[4], g_pidTransferCheckOriginalBytes[5],
                              g_pidTransferCheckJumpTarget, g_pidTransferCheckFallthroughTarget);
                VehUtil::ArmInt3(g_pidTransferCheckTarget);
            } else {
                LOG_MISC_WARN("PidTransferCheck: opcode verification failed ({:02X} {:02X}), expected 0F 85, bypass disabled",
                              g_pidTransferCheckOriginalBytes[0], g_pidTransferCheckOriginalBytes[1]);
                g_pidTransferCheckTarget = nullptr;
                g_pidTransferCheckJumpTarget = 0;
                g_pidTransferCheckFallthroughTarget = 0;
            }
        } else {
            LOG_MISC_WARN("PidTransferCheck: target not found, PID transfer bypass disabled");
        }

        if (!g_captures.empty() || g_spawnProcessTarget || g_pidTransferCheckTarget)
            g_vehHandle = AddVectoredExceptionHandler(1, VehHandler);

        // Hook MarkLicenseAsChanged and GetPackageInfo with Detours to capture
        // pCUser and pCPackageInfo on first call. This replaces the old VEH int3
        // approach which missed the first call (happened before hooks were installed).
        // GetAppIDForCurrentPipe is also detoured so it can apply the scoped
        // real-appid override for IClientUserStats traffic and capture the
        // engine pointer inline on the first natural call.
        LM_TX_BEGIN();
        LM_INSTALL(GetAppIDForCurrentPipe);
        LM_INSTALL(MarkLicenseAsChanged);
        LM_INSTALL(GetPackageInfo);
        LM_INSTALL(BuildSpawnEnvBlock);
        LM_TX_COMMIT();

        StartStartupInjectionRetry();
    }

    void Uninstall() {
        if (g_vehHandle) {
            RemoveVectoredExceptionHandler(g_vehHandle);
            g_vehHandle = nullptr;
        }

        VEH_CLEANUP_CAPTURES(g_captures);

        if (g_spawnProcessTarget && *g_spawnProcessTarget == 0xCC)
            VehUtil::RestoreByte(g_spawnProcessTarget, 0x48);
        g_spawnProcessTarget = nullptr;

        if (g_pidTransferCheckTarget && *g_pidTransferCheckTarget == 0xCC)
            VehUtil::RestoreByte(g_pidTransferCheckTarget, g_pidTransferCheckOriginalBytes[0]);
        g_pidTransferCheckTarget = nullptr;
        g_pidTransferCheckJumpTarget = 0;
        g_pidTransferCheckFallthroughTarget = 0;
        memset(g_pidTransferCheckOriginalBytes, 0, sizeof(g_pidTransferCheckOriginalBytes));

        LM_TX_BEGIN();
        LM_REMOVE(GetAppIDForCurrentPipe);
        LM_REMOVE(MarkLicenseAsChanged);
        LM_REMOVE(GetPackageInfo);
        LM_REMOVE(BuildSpawnEnvBlock);
        LM_TX_COMMIT();

        VEH_TRACK_LIST(VEH_ZERO_RESOLVE)
        SetOnlineFixRoute(0, OnlineFixRouteMode::None);
        SteamStubAuto::Clear();
        g_StatsScopePipe.store(0, std::memory_order_relaxed);
        g_userStatsAppIdOverrideDepth = 0;
        g_steamEngine   = nullptr;
        g_GameNameCache.clear();
        g_pCUser        = nullptr;
        g_pCPackageInfo = nullptr;
        g_startupInjectionDone.store(false);
        g_startupRetryThreadStarted.store(false);
    }

    AppId_t GetAppIDForCurrentPipe() {
        if (!g_steamEngine || !oGetAppIDForCurrentPipe) {
            LOG_MISC_WARN("GetAppIDForCurrentPipe called before capture - returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipe: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipe: AppId={}", appid);
        }
        return appid;
    }

    AppId_t ResolveAppId() {
        AppId_t routed = ActiveRouteRealAppIdInternal();
        if (routed) return routed;
        return GetAppIDForCurrentPipe();
    }

    AppId_t ActiveRouteRealAppId() {
        return ActiveRouteRealAppIdInternal();
    }

    AppId_t OnlineFixRealAppId() {
        return g_OnlineFixRealAppId.load(std::memory_order_acquire);
    }

    OnlineFixRouteMode OnlineFixMode() {
        return CurrentOnlineFixMode();
    }

    // Track multi-game OnlineFix lifecycle, packet heartbeat grace periods, and PID associations.
    void RegisterOnlineFixApp(AppId_t realAppId, OnlineFixRouteMode mode) {
        if (!realAppId) return;
        std::scoped_lock lock(g_onlineFixMutex);
        auto& entry = g_onlineFixAppEntries[realAppId];
        entry.appId = realAppId;
        entry.mode = mode;
        entry.registeredAt = GetTickCount64();
        entry.seenInPacket = false;
        g_OnlineFixRealAppId.store(realAppId, std::memory_order_release);
        g_OnlineFixRouteMode.store(static_cast<uint32>(mode), std::memory_order_release);
        LOG_MISC_INFO("OnlineFix: registered realAppId={} mode={} (total active: {})",
                      realAppId, OnlineFixRouteModeName(mode), g_onlineFixAppEntries.size());
    }

    void MarkOnlineFixAppSeen(AppId_t realAppId) {
        if (!realAppId) return;
        std::scoped_lock lock(g_onlineFixMutex);
        auto it = g_onlineFixAppEntries.find(realAppId);
        if (it != g_onlineFixAppEntries.end()) {
            if (!it->second.seenInPacket) {
                it->second.seenInPacket = true;
                LOG_MISC_INFO("OnlineFix: appid {} confirmed active in games played packet", realAppId);
            }
        }
    }

    bool CanUnregisterOnlineFixApp(AppId_t realAppId) {
        if (!realAppId) return true;
        std::scoped_lock lock(g_onlineFixMutex);
        auto it = g_onlineFixAppEntries.find(realAppId);
        if (it == g_onlineFixAppEntries.end()) return true;
        if (it->second.seenInPacket) return true;
        uint64_t elapsed = GetTickCount64() - it->second.registeredAt;
        if (elapsed > 20000) {
            LOG_MISC_WARN("OnlineFix: appid {} never appeared in games played packet, grace period expired ({}ms)",
                          realAppId, elapsed);
            return true;
        }
        return false;
    }

    void UnregisterOnlineFixApp(AppId_t realAppId) {
        if (!realAppId) return;
        std::scoped_lock lock(g_onlineFixMutex);
        g_onlineFixAppEntries.erase(realAppId);
        for (auto it = g_onlineFixPidToAppId.begin(); it != g_onlineFixPidToAppId.end();) {
            if (it->second == realAppId) it = g_onlineFixPidToAppId.erase(it);
            else ++it;
        }
        if (g_OnlineFixRealAppId.load(std::memory_order_acquire) == realAppId) {
            AppId_t fallback = g_onlineFixAppEntries.empty() ? 0 : g_onlineFixAppEntries.begin()->first;
            g_OnlineFixRealAppId.store(fallback, std::memory_order_release);
            if (!fallback) {
                g_OnlineFixRouteMode.store(static_cast<uint32>(OnlineFixRouteMode::None), std::memory_order_release);
            } else {
                g_OnlineFixRouteMode.store(static_cast<uint32>(g_onlineFixAppEntries.begin()->second.mode), std::memory_order_release);
            }
        }
        LOG_MISC_INFO("OnlineFix: unregistered realAppId={} (remaining active: {})",
                      realAppId, g_onlineFixAppEntries.size());
    }

    bool IsOnlineFixApp(AppId_t realAppId) {
        if (!realAppId) return false;
        std::scoped_lock lock(g_onlineFixMutex);
        return g_onlineFixAppEntries.find(realAppId) != g_onlineFixAppEntries.end()
            || g_OnlineFixRealAppId.load(std::memory_order_acquire) == realAppId;
    }

    bool HasActiveOnlineFixApps() {
        std::scoped_lock lock(g_onlineFixMutex);
        return !g_onlineFixAppEntries.empty() || g_OnlineFixRealAppId.load(std::memory_order_acquire) != 0;
    }

    std::vector<AppId_t> GetActiveOnlineFixApps() {
        std::scoped_lock lock(g_onlineFixMutex);
        std::vector<AppId_t> result;
        result.reserve(g_onlineFixAppEntries.size());
        for (const auto& [id, _] : g_onlineFixAppEntries) {
            result.push_back(id);
        }
        if (result.empty()) {
            AppId_t id = g_OnlineFixRealAppId.load(std::memory_order_acquire);
            if (id) result.push_back(id);
        }
        return result;
    }

    void AssociateOnlineFixPid(uint32_t pid, AppId_t realAppId) {
        if (!pid || !realAppId) return;
        std::scoped_lock lock(g_onlineFixMutex);
        g_onlineFixPidToAppId[pid] = realAppId;
        if (g_onlineFixAppEntries.find(realAppId) == g_onlineFixAppEntries.end()) {
            auto& entry = g_onlineFixAppEntries[realAppId];
            entry.appId = realAppId;
            entry.mode = OnlineFixRouteMode::ManualFlag;
            entry.registeredAt = GetTickCount64();
            entry.seenInPacket = false;
        }
        LOG_MISC_INFO("OnlineFix: associated PID {} -> realAppId {}", pid, realAppId);
    }

    AppId_t GetOnlineFixAppForPid(uint32_t pid) {
        if (!pid) return 0;
        std::scoped_lock lock(g_onlineFixMutex);
        auto it = g_onlineFixPidToAppId.find(pid);
        if (it != g_onlineFixPidToAppId.end()) return it->second;
        return 0;
    }

    void DisassociateOnlineFixPid(uint32_t pid) {
        if (!pid) return;
        std::scoped_lock lock(g_onlineFixMutex);
        auto it = g_onlineFixPidToAppId.find(pid);
        if (it != g_onlineFixPidToAppId.end()) {
            LOG_MISC_DEBUG("OnlineFix: disassociated PID {} (was realAppId {})", pid, it->second);
            g_onlineFixPidToAppId.erase(it);
        }
    }

    void SetOnlineFixRoute(AppId_t realAppId, OnlineFixRouteMode mode) {
        if (realAppId != 0 && mode != OnlineFixRouteMode::None) {
            RegisterOnlineFixApp(realAppId, mode);
        } else if (realAppId == 0) {
            std::scoped_lock lock(g_onlineFixMutex);
            g_onlineFixAppEntries.clear();
            g_onlineFixPidToAppId.clear();
            g_OnlineFixRealAppId.store(0, std::memory_order_release);
            g_OnlineFixRouteMode.store(static_cast<uint32>(OnlineFixRouteMode::None), std::memory_order_release);
            LOG_MISC_INFO("OnlineFix: route reset to none");
        } else {
            UnregisterOnlineFixApp(realAppId);
        }
    }

    bool OnlineFixRouteIsSteamStubAuto() {
        return SteamStubAuto::IsActive();
    }

    const char* OnlineFixRouteModeName(OnlineFixRouteMode mode) {
        switch (mode) {
        case OnlineFixRouteMode::None:          return "none";
        case OnlineFixRouteMode::ManualFlag:    return "manual-flag";
        }
        return "unknown";
    }

    void SetUserStatsContext(bool active) {
        if (active) {
            ++g_userStatsAppIdOverrideDepth;
        } else if (g_userStatsAppIdOverrideDepth > 0) {
            --g_userStatsAppIdOverrideDepth;
        } else {
            LOG_MISC_WARN("SetUserStatsContext(false) called with depth=0; clamping");
        }
    }

    void EnterStatsScope(HSteamPipe pipe, AppId_t appId) {
        g_StatsScopePipe.store(pipe, std::memory_order_release);
        if (appId != 0) {
            g_StatsScopeAppId.store(appId, std::memory_order_release);
        }
    }

    void LeaveStatsScope() {
        g_StatsScopePipe.store(0, std::memory_order_release);
        g_StatsScopeAppId.store(0, std::memory_order_release);
    }

    HSteamPipe StatsScopePipe() {
        return g_StatsScopePipe.load(std::memory_order_acquire);
    }

    AppId_t StatsScopeAppId() {
        return g_StatsScopeAppId.load(std::memory_order_acquire);
    }

    void EnsureBufferSize(CUtlBuffer* pWrite, int32 size)
    {
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, size);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
        }
        pWrite->m_Put = size;
    }

    // ── Game name ────────────────────────────────────────────────
    std::string GetGameNameByAppID(AppId_t appId)
    {
        auto& entry = g_GameNameCache.try_emplace(appId).first->second;
        if (!entry.empty()) return entry;

        if (g_pCAppInfoCache && oGetAppDataFromAppInfo) {
            char buf[256] = {};
            int64 len = oGetAppDataFromAppInfo(
                g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1)
                entry.assign(buf, static_cast<size_t>(len - 1));
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, entry);
        return entry;
    }

    // ── License refresh (no-restart) ────────────────────────────────
    bool IsReadyForNotify() {
        return (PackagePatch::GetPackage0() != nullptr || (g_pCPackageInfo && oGetPackageInfo))
            && oCUtlMemoryGrow != nullptr;
    }

    void TryStartupPackageInjection(const char* reason) {
        TryStartupInjection(reason);
    }

    static std::atomic_bool g_networkingSocketsActive{false};

    void NotifyNetworkingSocketsUsed() {
        bool expected = false;
        if (OnlineFixRealAppId() && g_networkingSocketsActive.compare_exchange_strong(expected, true))
            LOG_MISC_INFO("NetworkingSockets active: GetAppID now reports 480 for cert match");
    }

    bool ShouldReportOnlineFixAppId() {
        return OnlineFixRealAppId() != 0 && g_networkingSocketsActive.load(std::memory_order_acquire);
    }

    void NotifyLicenseChanged() {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: CUtlMemoryGrow not resolved yet, skipping");
            return;
        }
        PackageInfo* pPkg = ResolvePackage0ForMutation("hot-reload");
        if (!pPkg) {
            HookStatus::SetStartupRefreshState("startup-waiting-packageinfo");
            LOG_PACKAGE_WARN("NotifyLicenseChanged: package 0 not captured yet, leaving Lua changes pending");
            return;
        }
        if (pPkg->Status != EPackageStatus::Available) {
            HookStatus::SetStartupRefreshState("package0-not-available");
            LOG_PACKAGE_WARN("NotifyLicenseChanged: package 0 status={} not Available, leaving Lua changes pending",
                             static_cast<int>(pPkg->Status));
            return;
        }

        // Two-phase order.
        //   1. Mutate the package vector (drop removals, append additions)
        //   2. Trigger Steam's license refresh only if pCUser exists
        //   3. Queue UI refresh on the SteamUI run-frame hook

        // ── Phase 1a: drop removals from the package vector ──
        std::vector<AppId_t> removals = LuaLoader::TakePendingRemovals();

        std::unordered_set<AppId_t> removedLibraryRoots = LuaLoader::TakePendingLibraryRemovals();
        uint32_t removedCount = 0;
        for (AppId_t id : removals) {
            if (pPkg->AppIdVec.FindAndFastRemove(id)) {
                ++removedCount;
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {} from vector", id);
            } else {
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: AppId {} not in vector (hot-reload)", id);
            }
        }

        // ── Phase 1b: append additions to the package vector ──
        std::vector<AppId_t> additions = LuaLoader::TakePendingAdditions();
        if (!additions.empty())
            PackagePatch::InjectIntoPackage0(pPkg, additions, "hot-reload");

        if (additions.empty() && removals.empty()) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // ── Phase 2: license refresh (Steam re-evaluates package state) ──
        bool refreshedLicense = false;
        if (g_pCUser && oMarkLicenseAsChanged && oProcessPendingLicenseUpdates) {
            oMarkLicenseAsChanged(g_pCUser, 0, true);
            oProcessPendingLicenseUpdates(g_pCUser);
            HookStatus::SetPackageState(false, false, false, true);
            refreshedLicense = true;
        } else {
            HookStatus::SetStartupRefreshState("startup-waiting-cuser");
            LOG_PACKAGE_WARN("NotifyLicenseChanged: pCUser not ready, package vector updated locally only");
        }

        std::unordered_set<AppId_t> libraryRoots;
        for (AppId_t id : LuaLoader::GetLibraryAppIds()) {
            libraryRoots.insert(id);
        }
        uint32_t queuedTouches = 0;
        for (AppId_t id : additions) {
            if (libraryRoots.count(id)) {
                SteamUI::CancelLibraryRemoval(id);
                SteamUI::QueueLibraryTouch(id);
                ++queuedTouches;
            }
        }
        uint32_t queuedRemovals = 0;
        for (AppId_t id : removals) {
            if (removedLibraryRoots.count(id)) {
                SteamUI::QueueLibraryRemoval(id);
                ++queuedRemovals;
            }
        }
        if (queuedTouches || queuedRemovals)
            HookStatus::SetStartupRefreshState("library-refresh-queued");
        HookStatus::RecordHotReload(static_cast<uint32_t>(additions.size()),
                                    static_cast<uint32_t>(removals.size()),
                                    queuedTouches, queuedRemovals,
                                    refreshedLicense ? "license-refresh"
                                                     : "local-package-only");
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed ({} from vector)",
                         additions.size(), removals.size(), removedCount);
    }
}
