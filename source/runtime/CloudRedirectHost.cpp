// BetterLumaCore - Steam client hook layer.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "CloudRedirectHost.h"
#include "Logger.h"
#include "config/Settings.h"
#include "config/LuaLoader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

    using CR_NotifyFn = void (*)(int level, const char* title, const char* message);

    using CR_InitCloudSave_t      = bool (*)(const char* steamPath, CR_NotifyFn notify);
    using CR_HandleCloudRpc_t     = bool (*)(const char* method, uint32_t appId,
                                             uint32_t accountId,
                                             const uint8_t* reqBody, uint32_t reqLen,
                                             uint8_t* respBuf, uint32_t respMaxLen,
                                             uint32_t* respLen, int32_t* eresult);
    using CR_AddApp_t             = void (*)(uint32_t appId);
    using CR_RemoveApp_t          = void (*)(uint32_t appId);
    using CR_IsApp_t              = bool (*)(uint32_t appId);
    using CR_SetApps_t            = void (*)(const uint32_t* appIds, uint32_t count);
    using CR_Shutdown_t           = void (*)();
    using CR_EnableStatsSync_t    = void (*)(bool, bool);
    using CR_SetAccountId_t       = void (*)(uint32_t);
    using CR_NotifyAppRunning_t   = void (*)(uint32_t, bool);
    using CR_NotifyStatsStored_t  = void (*)(uint32_t);
    using CR_GetAchievements_t    = uint32_t (*)(uint32_t, CloudRedirectHost::AchievementBlock*, uint32_t);
    using CR_InstallVtableHooks_t = bool (*)();

    std::mutex           g_mutex;
    std::atomic<bool>    g_active{false};
    std::atomic<int32_t> g_inFlightCalls{0};
    HMODULE              g_module = nullptr;

    CR_InitCloudSave_t      g_initCloudSave      = nullptr;
    CR_HandleCloudRpc_t     g_handleCloudRpc     = nullptr;
    CR_SetApps_t            g_setApps            = nullptr;
    CR_IsApp_t              g_isApp              = nullptr;
    CR_Shutdown_t           g_shutdownFn         = nullptr;
    CR_EnableStatsSync_t    g_enableStatsSync    = nullptr;
    CR_SetAccountId_t       g_setAccountId       = nullptr;
    CR_NotifyAppRunning_t   g_notifyAppRunning   = nullptr;
    CR_NotifyStatsStored_t  g_notifyStatsStored  = nullptr;
    CR_GetAchievements_t    g_getAchievements    = nullptr;
    CR_InstallVtableHooks_t g_installVtableHooks = nullptr;

    // Structured exception handling isolation wrappers for third-party functions
    static DWORD LogException(const char* fnName, LPEXCEPTION_POINTERS ep) {
        if (ep && ep->ExceptionRecord) {
            LOG_WARN("CloudRedirect: exception in {} (code=0x{:08X}, addr=0x{:X})",
                     fnName,
                     ep->ExceptionRecord->ExceptionCode,
                     reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
        } else {
            LOG_WARN("CloudRedirect: exception in {}", fnName);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    static bool SafeInvokeInit(CR_InitCloudSave_t fn, const char* path, CR_NotifyFn notify) {
        __try {
            return fn ? fn(path, notify) : false;
        } __except (LogException("CR_InitCloudSave", GetExceptionInformation())) {
            return false;
        }
    }

    static bool SafeInvokeHandleRpc(CR_HandleCloudRpc_t fn, const char* method, uint32_t appId,
                                    uint32_t accountId, const uint8_t* reqBody, uint32_t reqLen,
                                    uint8_t* respBuf, uint32_t respMaxLen,
                                    uint32_t* respLen, int32_t* eresult) {
        __try {
            return fn ? fn(method, appId, accountId, reqBody, reqLen, respBuf, respMaxLen, respLen, eresult) : false;
        } __except (LogException("CR_HandleCloudRpc", GetExceptionInformation())) {
            return false;
        }
    }

    static void SafeInvokeSetApps(CR_SetApps_t fn, const uint32_t* appIds, uint32_t count) {
        __try {
            if (fn) fn(appIds, count);
        } __except (LogException("CR_SetApps", GetExceptionInformation())) {
        }
    }

    static bool SafeInvokeIsApp(CR_IsApp_t fn, uint32_t appId) {
        __try {
            return fn ? fn(appId) : false;
        } __except (LogException("CR_IsApp", GetExceptionInformation())) {
            return false;
        }
    }

    static void SafeInvokeShutdown(CR_Shutdown_t fn) {
        __try {
            if (fn) fn();
        } __except (LogException("CR_Shutdown", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeEnableStatsSync(CR_EnableStatsSync_t fn, bool a, bool b) {
        __try {
            if (fn) fn(a, b);
        } __except (LogException("CR_EnableStatsSync", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeSetAccountId(CR_SetAccountId_t fn, uint32_t accountId) {
        __try {
            if (fn) fn(accountId);
        } __except (LogException("CR_SetAccountId", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeNotifyAppRunning(CR_NotifyAppRunning_t fn, uint32_t appId, bool running) {
        __try {
            if (fn) fn(appId, running);
        } __except (LogException("CR_NotifyAppRunning", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeNotifyStatsStored(CR_NotifyStatsStored_t fn, uint32_t appId) {
        __try {
            if (fn) fn(appId);
        } __except (LogException("CR_NotifyStatsStored", GetExceptionInformation())) {
        }
    }

    static uint32_t SafeInvokeGetAchievements(CR_GetAchievements_t fn, uint32_t appId,
                                              CloudRedirectHost::AchievementBlock* out,
                                              uint32_t maxBlocks) {
        __try {
            return fn ? fn(appId, out, maxBlocks) : 0;
        } __except (LogException("CR_GetAchievements", GetExceptionInformation())) {
            return 0;
        }
    }

    static bool SafeInvokeInstallVtableHooks(CR_InstallVtableHooks_t fn) {
        __try {
            return fn ? fn() : false;
        } __except (LogException("CR_InstallVtableHooks", GetExceptionInformation())) {
            return false;
        }
    }

    // Guard object tracking in-flight invocations and holding an OS module reference
    // to guarantee the DLL physical memory pages cannot be unmapped while in use.
    struct InFlightGuard {
        std::atomic<int32_t>& counter;
        HMODULE hModule = nullptr;
        bool valid = false;

        explicit InFlightGuard(std::atomic<int32_t>& c) : counter(c) {
            counter.fetch_add(1, std::memory_order_acq_rel);

            HMODULE currentMod = g_module;
            if (g_active.load(std::memory_order_acquire) && currentMod) {
                HMODULE mod = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                       reinterpret_cast<LPCWSTR>(currentMod), &mod)) {
                    hModule = mod;
                    valid = true;
                }
            }

            if (!valid) {
                counter.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        ~InFlightGuard() {
            if (valid) {
                counter.fetch_sub(1, std::memory_order_acq_rel);
                if (hModule) {
                    FreeLibrary(hModule);
                    hModule = nullptr;
                }
            }
        }

        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
    };

    void CloudNotify(int level, const char* title, const char* message) {
        // Defensive string sanitization for external library callback
        std::string safeTitle = "CloudRedirect";
        std::string safeMessage;

        if (title) {
            safeTitle.assign(title, strnlen(title, 256));
        }
        if (message) {
            safeMessage.assign(message, strnlen(message, 1024));
        }

        for (char& c : safeTitle) {
            unsigned char uc = static_cast<unsigned char>(c);
            if ((uc < 0x20 || uc >= 0x7F) && c != '\t') c = ' ';
        }
        for (char& c : safeMessage) {
            unsigned char uc = static_cast<unsigned char>(c);
            if ((uc < 0x20 || uc >= 0x7F) && c != '\t' && c != '\n') c = ' ';
        }

        switch (level) {
        case 2:  LOG_ERROR("[CloudRedirect] {}: {}", safeTitle, safeMessage); break;
        case 1:  LOG_WARN("[CloudRedirect] {}: {}", safeTitle, safeMessage);  break;
        default: LOG_INFO("[CloudRedirect] {}: {}", safeTitle, safeMessage);  break;
        }
    }

    std::filesystem::path ResolveLibraryPath(const std::string& steamRoot,
                                             const std::string& configured) {
        std::filesystem::path base(steamRoot);
        std::filesystem::path lib(configured.empty() ? "cloud_redirect.dll" : configured);
        std::filesystem::path target = lib.is_absolute() ? lib : (base / lib);
        std::error_code ec;
        return std::filesystem::weakly_canonical(target, ec);
    }

    template <typename T>
    bool ResolveSymbol(HMODULE module, const char* name, T& out) {
        out = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!out) {
            LOG_WARN("CloudRedirect: export {} not found in cloud_redirect.dll", name);
            return false;
        }
        return true;
    }

    // Retargets hardcoded "steamclient64.dll" module references inside cloud_redirect.dll
    // to LumaCore's diversion module "lcoverlay.dll". This allows CloudRedirect's built-in
    // RTTI scanner and prologue validator to discover CClientUnifiedServiceTransport on
    // lcoverlay.dll and install in-memory vtable hooks directly (0ms latency, full offline support).
    // If scanning finds no matches (e.g. future CloudRedirect changes), it logs a diagnostic
    // notice and cleanly falls back to the asynchronous wire pass-through path.
    static void PatchModuleReferences(HMODULE hModule) {
        if (!hModule) return;

        auto base = reinterpret_cast<const uint8_t*>(hModule);
        auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;

        // Exact 18-byte ANSI sequence: "steamclient64.dll\0" (17 chars + null terminator)
        // Replaced with "lcoverlay.dll\0\0\0\0\0" (13 chars + 5 null terminators = 18 bytes)
        static constexpr char kTargetAnsi[]  = "steamclient64.dll";
        static constexpr char kReplaceAnsi[] = "lcoverlay.dll\0\0\0\0";
        static_assert(sizeof(kTargetAnsi) == sizeof(kReplaceAnsi), "ANSI size mismatch");
        static_assert(sizeof(kTargetAnsi) == 18, "ANSI target must be 18 bytes");

        // Exact 36-byte WIDE sequence: L"steamclient64.dll\0" (17 wchar_t + null terminator)
        // Replaced with L"lcoverlay.dll\0\0\0\0\0" (13 wchar_t + 5 null terminators = 36 bytes)
        static constexpr wchar_t kTargetWide[]  = L"steamclient64.dll";
        static constexpr wchar_t kReplaceWide[] = L"lcoverlay.dll\0\0\0\0";
        static_assert(sizeof(kTargetWide) == sizeof(kReplaceWide), "WIDE size mismatch");
        static_assert(sizeof(kTargetWide) == 36, "WIDE target must be 36 bytes");

        uint32_t patchedAnsi = 0;
        uint32_t patchedWide = 0;

        auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            // Scan read-only data (.rdata) and general data (.data) sections
            const bool isRdata = (memcmp(section->Name, ".rdata", 6) == 0);
            const bool isData  = (memcmp(section->Name, ".data", 5) == 0);
            if (!isRdata && !isData) continue;

            uint8_t* secStart = const_cast<uint8_t*>(base + section->VirtualAddress);
            const size_t secSize = section->Misc.VirtualSize > 0 ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (!secStart || secSize < sizeof(kTargetAnsi)) continue;

            // 1. Scan for ANSI matches
            for (size_t off = 0; off + sizeof(kTargetAnsi) <= secSize; ++off) {
                if (memcmp(secStart + off, kTargetAnsi, sizeof(kTargetAnsi)) == 0) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(secStart + off, sizeof(kTargetAnsi), PAGE_READWRITE, &oldProtect)) {
                        memcpy(secStart + off, kReplaceAnsi, sizeof(kTargetAnsi));
                        VirtualProtect(secStart + off, sizeof(kTargetAnsi), oldProtect, &oldProtect);
                        FlushInstructionCache(GetCurrentProcess(), secStart + off, sizeof(kTargetAnsi));
                        patchedAnsi++;
                    }
                }
            }

            // 2. Scan for WIDE matches (2-byte aligned)
            for (size_t off = 0; off + sizeof(kTargetWide) <= secSize; off += 2) {
                if (memcmp(secStart + off, kTargetWide, sizeof(kTargetWide)) == 0) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(secStart + off, sizeof(kTargetWide), PAGE_READWRITE, &oldProtect)) {
                        memcpy(secStart + off, kReplaceWide, sizeof(kTargetWide));
                        VirtualProtect(secStart + off, sizeof(kTargetWide), oldProtect, &oldProtect);
                        FlushInstructionCache(GetCurrentProcess(), secStart + off, sizeof(kTargetWide));
                        patchedWide++;
                    }
                }
            }
        }

        if (patchedAnsi > 0 || patchedWide > 0) {
            LOG_INFO("CloudRedirect: retargeted {} ANSI and {} WIDE module reference(s) to lcoverlay.dll",
                     patchedAnsi, patchedWide);
        } else {
            LOG_WARN("CloudRedirect: module targets not found in .rdata/.data; fallback to packet-layer path active");
        }
    }

    std::vector<uint32_t> CollectAllUnlockedApps() {
        std::unordered_set<uint32_t> set;
        for (AppId_t id : LuaLoader::GetAllDepotIds()) {
            if (id != 0) set.insert(static_cast<uint32_t>(id));
        }
        for (AppId_t id : LuaLoader::GetLibraryAppIds()) {
            if (id != 0) set.insert(static_cast<uint32_t>(id));
        }
        return std::vector<uint32_t>(set.begin(), set.end());
    }

} // anonymous namespace

namespace CloudRedirectHost {

    void Initialize(const char* steamInstallPath) {
        if (!Settings::cloudEnabled) {
            LOG_INFO("CloudRedirect: [cloud].enabled is false, cloud save redirection disabled");
            return;
        }
        if (!steamInstallPath || steamInstallPath[0] == '\0') {
            LOG_WARN("CloudRedirect: empty Steam install path, cannot initialise");
            return;
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active.load(std::memory_order_acquire)) return;

        const std::filesystem::path libPath = ResolveLibraryPath(steamInstallPath, Settings::cloudLibrary);
        std::error_code ec;
        if (!std::filesystem::exists(libPath, ec) || !std::filesystem::is_regular_file(libPath, ec)) {
            LOG_WARN("CloudRedirect: cloud_redirect.dll not found or invalid at {}", libPath.string());
            return;
        }

        g_module = LoadLibraryW(libPath.c_str());
        if (!g_module) {
            LOG_WARN("CloudRedirect: failed to load {} (err={})", libPath.string(), GetLastError());
            return;
        }

        PatchModuleReferences(g_module);

        bool ok = true;
        ok &= ResolveSymbol(g_module, "CR_InitCloudSave",  g_initCloudSave);
        ok &= ResolveSymbol(g_module, "CR_HandleCloudRpc", g_handleCloudRpc);
        ok &= ResolveSymbol(g_module, "CR_SetApps",        g_setApps);
        ok &= ResolveSymbol(g_module, "CR_IsApp",          g_isApp);
        ok &= ResolveSymbol(g_module, "CR_Shutdown",       g_shutdownFn);
        if (!ok) {
            LOG_WARN("CloudRedirect: cloud_redirect.dll is missing required exports, disabling");
            FreeLibrary(g_module);
            g_module = nullptr;
            return;
        }

        // Optional exports (CR 2.2.5+)
        ResolveSymbol(g_module, "CR_EnableStatsSync",    g_enableStatsSync);
        ResolveSymbol(g_module, "CR_SetAccountId",       g_setAccountId);
        ResolveSymbol(g_module, "CR_NotifyAppRunning",   g_notifyAppRunning);
        ResolveSymbol(g_module, "CR_NotifyStatsStored",  g_notifyStatsStored);
        ResolveSymbol(g_module, "CR_GetAchievements",    g_getAchievements);
        ResolveSymbol(g_module, "CR_InstallVtableHooks", g_installVtableHooks);

        if (!SafeInvokeInit(g_initCloudSave, steamInstallPath, &CloudNotify)) {
            LOG_WARN("CloudRedirect: CR_InitCloudSave failed, disabling cloud save redirection");
            FreeLibrary(g_module);
            g_module = nullptr;
            return;
        }

        g_active.store(true, std::memory_order_release);
        LOG_INFO("CloudRedirect: loaded {} and initialised cloud save redirection", libPath.string());

        if (g_enableStatsSync) {
            SafeInvokeEnableStatsSync(g_enableStatsSync, true, true);
            LOG_INFO("CloudRedirect: stats sync registered");
        }

        std::vector<uint32_t> appIds = CollectAllUnlockedApps();
        SafeInvokeSetApps(g_setApps, appIds.empty() ? nullptr : appIds.data(),
                          static_cast<uint32_t>(appIds.size()));
        LOG_INFO("CloudRedirect: registered {} redirected app(s)", appIds.size());

        if (g_installVtableHooks) {
            if (SafeInvokeInstallVtableHooks(g_installVtableHooks))
                LOG_INFO("CloudRedirect: vtable hooks installed");
            else
                LOG_WARN("CloudRedirect: vtable hook install failed, using packet-layer path");
        }
    }

    void SyncAppSet() {
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_setApps) return;

        std::vector<uint32_t> appIds = CollectAllUnlockedApps();
        SafeInvokeSetApps(g_setApps, appIds.empty() ? nullptr : appIds.data(),
                          static_cast<uint32_t>(appIds.size()));
        LOG_DEBUG("CloudRedirect: re-synced redirected app set ({} app(s))", appIds.size());
    }

    bool IsActive() {
        return g_active.load(std::memory_order_acquire);
    }

    bool IsApp(uint32_t appId) {
        if (appId == 0) return false;
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_isApp) return false;
        return SafeInvokeIsApp(g_isApp, appId);
    }

    bool HandleCloudRpc(const char* method, uint32_t appId, uint32_t accountId,
                        const uint8_t* reqBody, uint32_t reqLen,
                        uint8_t* respBuf, uint32_t respMaxLen,
                        uint32_t* respLen, int32_t* eresult) {
        if (!method || !reqBody || reqLen == 0 || !respBuf || respMaxLen == 0 || !respLen || !eresult)
            return false;

        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_handleCloudRpc) return false;

        // Boundary guard canaries to verify memory integrity across the library interface
        static constexpr size_t kCanarySize = 32;
        static constexpr uint8_t kCanaryByte = 0xAA;
        std::vector<uint8_t> safeBuf(respMaxLen + kCanarySize, kCanaryByte);

        uint32_t localRespLen = 0;
        int32_t  localResult = 2; // EResult::Fail

        bool ok = SafeInvokeHandleRpc(g_handleCloudRpc, method, appId, accountId,
                                      reqBody, reqLen,
                                      safeBuf.data(), respMaxLen,
                                      &localRespLen, &localResult);
        if (!ok) return false;

        // Boundary verification: response length must remain within requested capacity
        if (localRespLen > respMaxLen) {
            LOG_WARN("CloudRedirect: response size {} exceeded buffer limit {}, operation discarded",
                     localRespLen, respMaxLen);
            return false;
        }

        // Verify guard canaries remained intact
        for (size_t i = 0; i < kCanarySize; ++i) {
            if (safeBuf[respMaxLen + i] != kCanaryByte) {
                LOG_WARN("CloudRedirect: boundary guard byte mismatch on RPC {}, operation discarded", method);
                return false;
            }
        }

        if (localRespLen > 0)
            std::memcpy(respBuf, safeBuf.data(), localRespLen);
        *respLen = localRespLen;
        *eresult = (localResult >= 1 && localResult <= 120) ? localResult : 2;
        return true;
    }

    void SetAccountId(uint32_t accountId) {
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_setAccountId) return;
        SafeInvokeSetAccountId(g_setAccountId, accountId);
    }

    void NotifyAppRunning(uint32_t appId, bool running) {
        if (appId == 0) return;
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_notifyAppRunning) return;
        SafeInvokeNotifyAppRunning(g_notifyAppRunning, appId, running);
    }

    void NotifyStatsStored(uint32_t appId) {
        if (appId == 0) return;
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_notifyStatsStored) return;
        SafeInvokeNotifyStatsStored(g_notifyStatsStored, appId);
    }

    uint32_t GetAchievements(uint32_t appId, AchievementBlock* out, uint32_t maxBlocks) {
        if (appId == 0 || !out || maxBlocks == 0) return 0;
        InFlightGuard guard(g_inFlightCalls);
        if (!guard.valid || !g_getAchievements) return 0;
        uint32_t res = SafeInvokeGetAchievements(g_getAchievements, appId, out, maxBlocks);
        return (res > maxBlocks) ? maxBlocks : res;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_active.exchange(false, std::memory_order_acq_rel)) return;

        // Drain in-flight calls before invoking shutdown export or unmapping module
        constexpr int kMaxDrainIterations = 200;
        for (int i = 0; i < kMaxDrainIterations && g_inFlightCalls.load(std::memory_order_acquire) > 0; ++i) {
            Sleep(10);
        }

        int32_t remaining = g_inFlightCalls.load(std::memory_order_acquire);
        if (remaining > 0) {
            LOG_WARN("CloudRedirect: shutdown proceeding with {} in-flight call(s) still active; module will remain pinned until they complete", remaining);
        } else if (g_shutdownFn) {
            SafeInvokeShutdown(g_shutdownFn);
        }

        g_initCloudSave      = nullptr;
        g_handleCloudRpc     = nullptr;
        g_setApps            = nullptr;
        g_isApp              = nullptr;
        g_shutdownFn         = nullptr;
        g_enableStatsSync    = nullptr;
        g_setAccountId       = nullptr;
        g_notifyAppRunning   = nullptr;
        g_notifyStatsStored  = nullptr;
        g_getAchievements    = nullptr;
        g_installVtableHooks = nullptr;
        if (g_module) {
            FreeLibrary(g_module);
            g_module = nullptr;
        }
        LOG_INFO("CloudRedirect: shut down");
    }

} // namespace CloudRedirectHost
