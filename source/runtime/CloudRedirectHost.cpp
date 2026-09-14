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
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
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

    std::mutex              g_mutex;
    std::mutex              g_drainMutex;
    std::condition_variable g_drainCv;
    std::atomic<bool>       g_active{false};
    std::atomic<int32_t>    g_inFlightCalls{0};
    HMODULE                 g_module = nullptr;

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

    std::atomic<uint32_t>   g_consecutiveExceptions{0};
    constexpr uint32_t      kMaxConsecutiveExceptions = 3;

    // Structured exception handling isolation and diagnostics for third-party functions
    static const char* ExceptionCodeToString(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
            case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
            case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
            case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
            case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
            case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
            case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
            case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
            case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
            case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
            case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
            case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
            case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
            case EXCEPTION_GUARD_PAGE:               return "EXCEPTION_GUARD_PAGE";
            case 0xC0000374:                         return "STATUS_HEAP_CORRUPTION";
            case 0xC0000409:                         return "STATUS_STACK_BUFFER_OVERRUN";
            default:                                 return "UNKNOWN_EXCEPTION";
        }
    }

    struct ModuleInfo {
        std::string name = "unknown";
        uintptr_t   base = 0;
        uintptr_t   offset = 0;
    };

    static ModuleInfo ResolveModuleInfo(const void* addr) {
        ModuleInfo info;
        if (!addr) return info;

        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &hMod) && hMod) {
            info.base = reinterpret_cast<uintptr_t>(hMod);
            info.offset = reinterpret_cast<uintptr_t>(addr) - info.base;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(hMod, path, static_cast<DWORD>(std::size(path)))) {
                info.name = std::filesystem::path(path).filename().string();
            }
        }
        return info;
    }

    static void TripCircuitBreaker(const char* reason) {
        if (g_active.exchange(false, std::memory_order_acq_rel)) {
            LOG_ERROR("CloudRedirect: CIRCUIT BREAKER TRIPPED ({}) - third-party cloud redirection disabled to protect process stability",
                      reason);
            char dbgMsg[256];
            std::snprintf(dbgMsg, sizeof(dbgMsg),
                          "[CloudRedirect] CIRCUIT BREAKER TRIPPED (%s) - cloud redirection disabled\n", reason);
            OutputDebugStringA(dbgMsg);
        }
    }

    static DWORD FilterException(const char* fnName, LPEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord) {
            LOG_WARN("CloudRedirect: exception in {} with null exception record", fnName);
            return EXCEPTION_EXECUTE_HANDLER;
        }

        const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        const PCONTEXT ctx = ep->ContextRecord;
        DWORD code = rec->ExceptionCode;
        const char* codeStr = ExceptionCodeToString(code);
        ModuleInfo faultMod = ResolveModuleInfo(rec->ExceptionAddress);

        // Classify exception severity
        bool isNonContinuable = (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0;
        bool isHeapCorruption = (code == 0xC0000374); // STATUS_HEAP_CORRUPTION
        bool isStackCorruption = (code == 0xC0000409) || (code == EXCEPTION_STACK_OVERFLOW);
        bool isGuardPage = (code == EXCEPTION_GUARD_PAGE);
        bool isDepViolation = (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 1 && rec->ExceptionInformation[0] == 8);
        bool isFatal = isNonContinuable || isHeapCorruption || isStackCorruption || isGuardPage || isDepViolation;

        // Log rich diagnostic report
        if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            const char* op = "accessing";
            switch (rec->ExceptionInformation[0]) {
                case 0: op = "reading from"; break;
                case 1: op = "writing to"; break;
                case 8: op = "executing DEP address"; break;
            }
            uintptr_t targetAddr = rec->ExceptionInformation[1];
            LOG_ERROR("CloudRedirect: EXCEPTION in {}! Code: {} (0x{:08X}) attempting {} 0x{:X} at {}+0x{:X} (addr=0x{:X}, flags=0x{:X})",
                      fnName, codeStr, code, op, targetAddr, faultMod.name, faultMod.offset,
                      reinterpret_cast<uintptr_t>(rec->ExceptionAddress), rec->ExceptionFlags);

            char dbgMsg[512];
            std::snprintf(dbgMsg, sizeof(dbgMsg),
                          "[CloudRedirect] EXCEPTION in %s! %s (0x%08X) %s 0x%llX at %s+0x%llX (addr=0x%llX)\n",
                          fnName, codeStr, code, op, static_cast<unsigned long long>(targetAddr),
                          faultMod.name.c_str(), static_cast<unsigned long long>(faultMod.offset),
                          reinterpret_cast<unsigned long long>(rec->ExceptionAddress));
            OutputDebugStringA(dbgMsg);
        } else {
            LOG_ERROR("CloudRedirect: EXCEPTION in {}! Code: {} (0x{:08X}) at {}+0x{:X} (addr=0x{:X}, flags=0x{:X})",
                      fnName, codeStr, code, faultMod.name, faultMod.offset,
                      reinterpret_cast<uintptr_t>(rec->ExceptionAddress), rec->ExceptionFlags);

            char dbgMsg[512];
            std::snprintf(dbgMsg, sizeof(dbgMsg),
                          "[CloudRedirect] EXCEPTION in %s! %s (0x%08X) at %s+0x%llX (addr=0x%llX, flags=0x%X)\n",
                          fnName, codeStr, code, faultMod.name.c_str(),
                          static_cast<unsigned long long>(faultMod.offset),
                          reinterpret_cast<unsigned long long>(rec->ExceptionAddress), rec->ExceptionFlags);
            OutputDebugStringA(dbgMsg);
        }

        if (ctx) {
#if defined(_M_X64) || defined(__x86_64__)
            LOG_ERROR("CloudRedirect Context: RIP=0x{:X} RSP=0x{:X} RBP=0x{:X} RAX=0x{:X} RBX=0x{:X} RCX=0x{:X} RDX=0x{:X}",
                      ctx->Rip, ctx->Rsp, ctx->Rbp, ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
            LOG_ERROR("CloudRedirect Context: R8=0x{:X} R9=0x{:X} R10=0x{:X} R11=0x{:X} R12=0x{:X} R13=0x{:X} R14=0x{:X} R15=0x{:X} EFLAGS=0x{:08X}",
                      ctx->R8, ctx->R9, ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15, ctx->EFlags);
            char regMsg[512];
            std::snprintf(regMsg, sizeof(regMsg),
                          "[CloudRedirect] RIP=0x%llX RSP=0x%llX RBP=0x%llX RAX=0x%llX RCX=0x%llX RDX=0x%llX\n",
                          ctx->Rip, ctx->Rsp, ctx->Rbp, ctx->Rax, ctx->Rcx, ctx->Rdx);
            OutputDebugStringA(regMsg);
#elif defined(_M_IX86) || defined(__i386__)
            LOG_ERROR("CloudRedirect Context: EIP=0x{:X} ESP=0x{:X} EBP=0x{:X} EAX=0x{:X} EBX=0x{:X} ECX=0x{:X} EDX=0x{:X} EFLAGS=0x{:08X}",
                      ctx->Eip, ctx->Esp, ctx->Ebp, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->EFlags);
#endif
        }

        // Fatal heap/stack corruption or DEP violation must NEVER be swallowed.
        // Continuing execution in a corrupted heap or stack is an exploitable security vulnerability.
        // Return EXCEPTION_CONTINUE_SEARCH so the system crash handler or debugger captures the dump.
        if (isFatal) {
            TripCircuitBreaker(isDepViolation ? "DEP execution violation" : "fatal memory/stack corruption or non-continuable exception");
            LOG_ERROR("CloudRedirect: fatal memory corruption cannot be safely recovered; propagating exception to crash handler");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Warn if the fault originated outside cloud_redirect.dll
        if (faultMod.base != reinterpret_cast<uintptr_t>(g_module)) {
            LOG_WARN("CloudRedirect: exception occurred in external module {} (base 0x{:X}), not inside cloud_redirect.dll",
                     faultMod.name, faultMod.base);
        }

        // For catchable faults (e.g. null pointer read in third-party DLL):
        // Track consecutive failures and trip circuit breaker if threshold is exceeded.
        uint32_t count = g_consecutiveExceptions.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count >= kMaxConsecutiveExceptions) {
            TripCircuitBreaker("repeated consecutive exceptions");
        }

        return EXCEPTION_EXECUTE_HANDLER;
    }

    static bool SafeInvokeInit(CR_InitCloudSave_t fn, const char* path, CR_NotifyFn notify) {
        __try {
            bool ok = fn ? fn(path, notify) : false;
            if (ok) g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            return ok;
        } __except (FilterException("CR_InitCloudSave", GetExceptionInformation())) {
            return false;
        }
    }

    static bool SafeInvokeHandleRpc(CR_HandleCloudRpc_t fn, const char* method, uint32_t appId,
                                    uint32_t accountId, const uint8_t* reqBody, uint32_t reqLen,
                                    uint8_t* respBuf, uint32_t respMaxLen,
                                    uint32_t* respLen, int32_t* eresult) {
        __try {
            bool ok = fn ? fn(method, appId, accountId, reqBody, reqLen, respBuf, respMaxLen, respLen, eresult) : false;
            if (ok) g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            return ok;
        } __except (FilterException("CR_HandleCloudRpc", GetExceptionInformation())) {
            return false;
        }
    }

    static void SafeInvokeSetApps(CR_SetApps_t fn, const uint32_t* appIds, uint32_t count) {
        __try {
            if (fn) {
                fn(appIds, count);
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_SetApps", GetExceptionInformation())) {
        }
    }

    static bool SafeInvokeIsApp(CR_IsApp_t fn, uint32_t appId) {
        __try {
            bool res = fn ? fn(appId) : false;
            g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            return res;
        } __except (FilterException("CR_IsApp", GetExceptionInformation())) {
            return false;
        }
    }

    static void SafeInvokeShutdown(CR_Shutdown_t fn) {
        __try {
            if (fn) {
                fn();
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_Shutdown", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeEnableStatsSync(CR_EnableStatsSync_t fn, bool a, bool b) {
        __try {
            if (fn) {
                fn(a, b);
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_EnableStatsSync", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeSetAccountId(CR_SetAccountId_t fn, uint32_t accountId) {
        __try {
            if (fn) {
                fn(accountId);
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_SetAccountId", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeNotifyAppRunning(CR_NotifyAppRunning_t fn, uint32_t appId, bool running) {
        __try {
            if (fn) {
                fn(appId, running);
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_NotifyAppRunning", GetExceptionInformation())) {
        }
    }

    static void SafeInvokeNotifyStatsStored(CR_NotifyStatsStored_t fn, uint32_t appId) {
        __try {
            if (fn) {
                fn(appId);
                g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            }
        } __except (FilterException("CR_NotifyStatsStored", GetExceptionInformation())) {
        }
    }

    static uint32_t SafeInvokeGetAchievements(CR_GetAchievements_t fn, uint32_t appId,
                                              CloudRedirectHost::AchievementBlock* out,
                                              uint32_t maxBlocks) {
        __try {
            uint32_t res = fn ? fn(appId, out, maxBlocks) : 0;
            g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            return res;
        } __except (FilterException("CR_GetAchievements", GetExceptionInformation())) {
            return 0;
        }
    }

    static bool SafeInvokeInstallVtableHooks(CR_InstallVtableHooks_t fn) {
        __try {
            bool ok = fn ? fn() : false;
            if (ok) g_consecutiveExceptions.store(0, std::memory_order_relaxed);
            return ok;
        } __except (FilterException("CR_InstallVtableHooks", GetExceptionInformation())) {
            return false;
        }
    }

    static void ReleaseInFlight(std::atomic<int32_t>& counter) {
        if (counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> drainLock(g_drainMutex);
            g_drainCv.notify_all();
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
                ReleaseInFlight(counter);
            }
        }

        ~InFlightGuard() {
            if (valid) {
                ReleaseInFlight(counter);
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
        g_consecutiveExceptions.store(0, std::memory_order_relaxed);

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
        g_consecutiveExceptions.store(0, std::memory_order_relaxed);

        // Drain in-flight calls using condition variable with a safety timeout
        bool drained = false;
        {
            std::unique_lock<std::mutex> drainLock(g_drainMutex);
            drained = g_drainCv.wait_for(drainLock, std::chrono::seconds(3), [&] {
                return g_inFlightCalls.load(std::memory_order_acquire) == 0;
            });
        }

        int32_t remaining = g_inFlightCalls.load(std::memory_order_acquire);
        if (!drained || remaining > 0) {
            LOG_WARN("CloudRedirect: shutdown timed out with {} in-flight call(s) still active; module permanently pinned to prevent crash on unmapped code", remaining);
            if (g_module) {
                HMODULE pinnedMod = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCWSTR>(g_module), &pinnedMod);
                // Do not invoke CR_Shutdown or call FreeLibrary; keep exports intact for active callers
                g_module = nullptr;
            }
        } else {
            if (g_shutdownFn) {
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
    }

} // namespace CloudRedirectHost
