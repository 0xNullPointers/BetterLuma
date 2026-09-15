// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/OnlineFixInject.h"
#include "hooks/capture/RuntimeCapture.h"
#include "hooks/Macros.h"
#include "config/Settings.h"
#include "runtime/HookStatus.h"
#include "runtime/RemoteTools.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

    struct QueuedInjection {
        AppId_t      appId = 0;
        std::wstring expectedExePath;     // Normalized full executable path
        std::wstring expectedInstallDir;   // Normalized install directory
        std::wstring expectedBasename;     // Lowercase basename (e.g. game.exe)
        uint64_t     queuedAt = 0;
    };

    struct PendingRoute {
        AppId_t      appId = 0;
        std::wstring launchExe;
        std::wstring expectedExePath;
        std::wstring expectedInstallDir;
        uint64_t     queuedAt = 0;
        std::unordered_set<uint32_t> fallbackPids;
    };

    std::mutex                                g_queueLock;
    std::vector<QueuedInjection>              g_queue;
    std::unordered_map<AppId_t, PendingRoute> g_pendingRoutes;

    std::wstring LowerBasename(LPCWSTR path) {
        if (!path || !*path) return {};
        std::wstring name = std::filesystem::path(path).filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(),
            [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
        return name;
    }

    std::wstring NormalizePath(std::wstring_view path) {
        while (!path.empty() && (path.front() == L'"' || path.front() == L' ' || path.front() == L'\t'))
            path.remove_prefix(1);
        while (!path.empty() && (path.back() == L'"' || path.back() == L' ' || path.back() == L'\t'))
            path.remove_suffix(1);
        if (path.empty()) return {};

        std::wstring out;
        out.reserve(path.size());
        for (wchar_t c : path) {
            if (c == L'/') c = L'\\';
            out.push_back(static_cast<wchar_t>(towlower(c)));
        }
        while (out.size() > 3 && out.back() == L'\\') {
            out.pop_back();
        }
        return out;
    }

    bool IsSubpathOf(std::wstring_view child, std::wstring_view parent) {
        if (parent.empty() || child.size() < parent.size())
            return false;
        if (child.substr(0, parent.size()) != parent)
            return false;
        if (child.size() == parent.size())
            return true;
        wchar_t nextChar = child[parent.size()];
        return nextChar == L'\\' || nextChar == L'/';
    }

    std::wstring DeriveInstallDir(std::wstring_view normPath) {
        if (normPath.empty()) return {};
        constexpr std::wstring_view kCommon = L"\\steamapps\\common\\";
        size_t pos = normPath.find(kCommon);
        if (pos != std::wstring_view::npos) {
            size_t start = pos + kCommon.size();
            size_t nextSlash = normPath.find(L'\\', start);
            if (nextSlash != std::wstring_view::npos) {
                return std::wstring(normPath.substr(0, nextSlash));
            }
        }
        std::filesystem::path p(normPath);
        std::filesystem::path parent = p.parent_path();
        std::wstring parentName = parent.filename().wstring();
        if (parentName == L"win64" || parentName == L"x64" || parentName == L"bin" || parentName == L"binaries") {
            if (parent.has_parent_path()) {
                parent = parent.parent_path();
            }
        }
        return NormalizePath(parent.wstring());
    }

    std::wstring ExeFromCmd(LPCWSTR cmd) {
        if (!cmd) return {};
        while (*cmd == L' ' || *cmd == L'\t') ++cmd;
        std::wstring out;
        if (*cmd == L'"') {
            for (++cmd; *cmd && *cmd != L'"'; ++cmd) out.push_back(*cmd);
        } else {
            for (; *cmd && *cmd != L' ' && *cmd != L'\t'; ++cmd) out.push_back(*cmd);
        }
        return out;
    }

    std::string ImageForLog(std::string_view imageName) {
        if (!imageName.empty()) return std::string(imageName);
        return "-";
    }

    std::string NarrowPath(std::wstring_view text) {
        if (text.empty()) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(),
                            static_cast<int>(text.size()),
                            out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::wstring WideFromUtf8(std::string_view text) {
        if (text.empty()) return {};
        int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
        if (needed > 0) {
            std::wstring out(static_cast<size_t>(needed), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                    static_cast<int>(text.size()),
                                    out.data(), needed) > 0) {
                return out;
            }
        }
        needed = MultiByteToWideChar(CP_ACP, 0, text.data(),
                                     static_cast<int>(text.size()),
                                     nullptr, 0);
        if (needed <= 0) return {};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(),
                            static_cast<int>(text.size()),
                            out.data(), needed);
        return out;
    }

    bool ClaimPending(LPCWSTR app, LPCWSTR cmd, LPCWSTR cwd, QueuedInjection& outClaimed) {
        std::wstring rawExe;
        if (app && *app) {
            rawExe = app;
        } else if (cmd && *cmd) {
            rawExe = ExeFromCmd(cmd);
        }
        if (rawExe.empty()) {
            LOG_ONLINEFIX_DEBUG("claim miss: empty exe");
            return false;
        }

        std::filesystem::path rawPath(rawExe);
        if (rawPath.is_relative() && cwd && *cwd) {
            rawPath = std::filesystem::path(cwd) / rawPath;
        }
        std::wstring normCandidate = NormalizePath(rawPath.wstring());
        std::wstring candidateBasename = LowerBasename(normCandidate.c_str());

        std::lock_guard lk(g_queueLock);
        if (g_queue.empty()) {
            return false;
        }

        uint64_t now = GetTickCount64();
        // Purge expired items older than 60 seconds (throttled to at most once every 5 seconds)
        static uint64_t s_lastPurgeTime = 0;
        if (now - s_lastPurgeTime >= 5000) {
            s_lastPurgeTime = now;
            std::erase_if(g_queue, [now](const QueuedInjection& q) {
                return (now - q.queuedAt) > 60000;
            });
            if (g_queue.empty()) {
                return false;
            }
        }

        auto it = g_queue.end();
        // 1. Exact full path match
        it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueuedInjection& q) {
            return !q.expectedExePath.empty() && q.expectedExePath == normCandidate;
        });

        // 2. Install dir prefix + basename match
        if (it == g_queue.end()) {
            it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueuedInjection& q) {
                return q.expectedBasename == candidateBasename &&
                       !q.expectedInstallDir.empty() &&
                       IsSubpathOf(normCandidate, q.expectedInstallDir);
            });
        }

        // 3. Basename match with working directory verification:
        // Require cwd to be within expectedInstallDir (or installDir within cwd) to prevent
        // collisions across games sharing common directories.
        if (it == g_queue.end() && !candidateBasename.empty()) {
            std::wstring normCwd = (cwd && *cwd) ? NormalizePath(cwd) : std::wstring{};
            if (!normCwd.empty()) {
                it = std::find_if(g_queue.begin(), g_queue.end(), [&](const QueuedInjection& q) {
                    return q.expectedBasename == candidateBasename &&
                           !q.expectedInstallDir.empty() &&
                           (IsSubpathOf(normCwd, q.expectedInstallDir) ||
                            IsSubpathOf(q.expectedInstallDir, normCwd));
                });
            } else {
                // If no cwd was supplied, only match if there is unambiguously exactly
                // one queued item matching this basename. Single linear scan without nested find.
                auto singleMatch = g_queue.end();
                size_t matchCount = 0;
                for (auto qIt = g_queue.begin(); qIt != g_queue.end(); ++qIt) {
                    if (qIt->expectedBasename == candidateBasename) {
                        singleMatch = qIt;
                        ++matchCount;
                    }
                }
                if (matchCount == 1) {
                    it = singleMatch;
                }
            }
        }

        if (it == g_queue.end()) {
            LOG_ONLINEFIX_DEBUG("claim miss exe={} cwd={}", NarrowPath(normCandidate), cwd ? NarrowPath(cwd) : "-");
            return false;
        }

        outClaimed = *it;
        g_queue.erase(it);

        LOG_ONLINEFIX_INFO("claim hit appid={} exe=\"{}\" installDir=\"{}\"",
                           outClaimed.appId, NarrowPath(outClaimed.expectedExePath),
                           NarrowPath(outClaimed.expectedInstallDir));
        HookStatus::RecordOnlineFixPayload(outClaimed.appId, 0, NarrowPath(outClaimed.expectedBasename),
                                           "claimed", "createprocess");
        return true;
    }

    static void PrunePendingRoutesLocked(uint64_t now) {
        for (auto it = g_pendingRoutes.begin(); it != g_pendingRoutes.end();) {
            bool expired = (now - it->second.queuedAt) > 300'000; // 5-minute hard timeout
            if (!expired && (now - it->second.queuedAt) > 60'000) {
                // If route has been active for >60s, check if all associated processes have exited
                bool anyAlive = false;
                for (uint32_t p : it->second.fallbackPids) {
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p);
                    if (h) {
                        DWORD exitCode = 0;
                        if (GetExitCodeProcess(h, &exitCode) && exitCode == STILL_ACTIVE) {
                            anyAlive = true;
                        }
                        CloseHandle(h);
                    }
                    if (anyAlive) break;
                }
                if (!anyAlive && !it->second.fallbackPids.empty()) {
                    expired = true;
                }
            }
            if (expired) {
                LOG_ONLINEFIX_INFO("OnlineFix: pruned stale pending route for appid={}", it->first);
                it = g_pendingRoutes.erase(it);
            } else {
                ++it;
            }
        }
    }

    AppId_t ClaimFallbackRoute(uint32_t pid, std::string_view imageName, AppId_t expectedAppId) {
        std::wstring wide = WideFromUtf8(imageName);
        std::wstring key = LowerBasename(wide.c_str());
        std::lock_guard lk(g_queueLock);

        uint64_t now = GetTickCount64();
        PrunePendingRoutesLocked(now);

        if (g_pendingRoutes.empty()) {
            LOG_ONLINEFIX_DEBUG("fallback skip appid={} pid={} exe={} reason=no-pending",
                                expectedAppId, pid, NarrowPath(key));
            return 0;
        }
        PendingRoute* pRoute = nullptr;
        if (expectedAppId) {
            auto it = g_pendingRoutes.find(expectedAppId);
            if (it != g_pendingRoutes.end()) pRoute = &it->second;
        } else if (g_pendingRoutes.size() == 1) {
            auto& candidate = g_pendingRoutes.begin()->second;
            if (!pid || !candidate.fallbackPids.contains(pid)) {
                pRoute = &candidate;
            }
        } else {
            for (auto& [id, r] : g_pendingRoutes) {
                if (!pid || !r.fallbackPids.contains(pid)) {
                    pRoute = &r;
                    break;
                }
            }
        }
        if (!pRoute) {
            LOG_ONLINEFIX_WARN("fallback skip expected={} pid={} exe={} reason=no-matching-route",
                               expectedAppId, pid, NarrowPath(key));
            return 0;
        }
        if (pid && pRoute->fallbackPids.contains(pid)) {
            LOG_ONLINEFIX_DEBUG("fallback skip appid={} pid={} exe={} reason=already-tried",
                                pRoute->appId, pid, NarrowPath(key));
            return 0;
        }
        if (pid) {
            pRoute->fallbackPids.insert(pid);
            // Associate child process PID with its OnlineFix app for IPC and watcher resolution.
            SteamCapture::AssociateOnlineFixPid(pid, pRoute->appId);
        }
        LOG_ONLINEFIX_INFO("fallback route hit appid={} pid={} launch={} child={}",
                           pRoute->appId, pid, NarrowPath(pRoute->launchExe),
                           NarrowPath(key));
        HookStatus::RecordOnlineFixPayload(pRoute->appId, pid, NarrowPath(key),
                                           "fallback-claimed", "pipewatch-eos");
        return pRoute->appId;
    }

    using CreateProcessW_t = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
        LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
        LPSTARTUPINFOW, LPPROCESS_INFORMATION);
    using CreateProcessAsUserW_t = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR,
        LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
        LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

    CreateProcessW_t       oCreateProcessW       = nullptr;
    CreateProcessAsUserW_t oCreateProcessAsUserW = nullptr;

    // Injects LumaCorePayload.dll into a newly spawned, suspended process.
    // Uses DetourUpdateProcessWithDll to update the PE import directory of the
    // target process directly in memory. When the process's primary thread is resumed,
    // ntdll!LdrpInitializeProcess loads the payload natively before any application
    // code runs. This avoids CreateRemoteThread, which deadlocks on suspended processes
    // waiting on uninitialized loader locks (LdrpInitCompleteEvent).
    static bool InjectPayload(HANDLE hProcess, const char* dllPath) {
        if (!hProcess || !dllPath || !dllPath[0]) return false;

        BOOL isWow64 = FALSE;
        if (IsWow64Process(hProcess, &isWow64) && isWow64) {
            LOG_ONLINEFIX_WARN("InjectPayload: target process is 32-bit (WOW64); "
                               "64-bit payload cannot be loaded");
            return false;
        }

        // Convert path to 8.3 short path to ensure it consists strictly of 7-bit ASCII characters.
        // According to the Microsoft PE/COFF specification, IMAGE_IMPORT_DESCRIPTOR.Name is an
        // 8-bit null-terminated string. If the path contains non-ASCII characters outside the active
        // Windows code page, WideCharToMultiByte replaces them with '?', causing ntdll!LdrpLoadDll
        // to fail with STATUS_DLL_NOT_FOUND (0xC0000135). An NTFS 8.3 short path guarantees 7-bit ASCII.
        char shortPath[MAX_PATH] = {};
        std::wstring wDllPath = WideFromUtf8(dllPath);
        if (!wDllPath.empty()) {
            wchar_t wShort[MAX_PATH] = {};
            if (GetShortPathNameW(wDllPath.c_str(), wShort, MAX_PATH) > 0) {
                if (WideCharToMultiByte(CP_ACP, 0, wShort, -1, shortPath, sizeof(shortPath), nullptr, nullptr) > 0) {
                    dllPath = shortPath;
                }
            }
        }
        if (dllPath != shortPath) {
            if (GetShortPathNameA(dllPath, shortPath, sizeof(shortPath)) > 0) {
                dllPath = shortPath;
            } else if (GetFullPathNameA(dllPath, sizeof(shortPath), shortPath, nullptr) > 0) {
                dllPath = shortPath;
            }
        }

        if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
            LOG_ONLINEFIX_WARN("InjectPayload: payload DLL missing: \"{}\"", dllPath);
            return false;
        }

        if (strchr(dllPath, '?') != nullptr) {
            LOG_ONLINEFIX_WARN("InjectPayload: payload path contains unmappable Unicode characters: \"{}\"", dllPath);
            return false;
        }

        LPCSTR rlpDlls[1] = { dllPath };
        if (!DetourUpdateProcessWithDll(hProcess, rlpDlls, 1)) {
            DWORD err = GetLastError();
            LOG_ONLINEFIX_WARN("InjectPayload: DetourUpdateProcessWithDll failed err={}", err);
            return false;
        }
        return true;
    }

    BOOL LaunchSuspended(HANDLE token, LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
               LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
               LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        auto fwd = [&](DWORD f) {
            return token
                ? oCreateProcessAsUserW(token, app, cmd, pa, ta, inherit, f, env, cwd, si, pi)
                : oCreateProcessW(app, cmd, pa, ta, inherit, f, env, cwd, si, pi);
        };

        QueuedInjection claimed;
        bool hasClaim = ClaimPending(app, cmd, cwd, claimed);
        if (!hasClaim) return fwd(flags);

        AppId_t appId = claimed.appId;
        if (PayloadPath[0] == 0) {
            LOG_ONLINEFIX_WARN("appid={} payload path empty, forwarding without injection", appId);
            return fwd(flags);
        }

        BOOL ok = fwd(flags | CREATE_SUSPENDED);
        if (!ok) {
            LOG_ONLINEFIX_WARN("appid={} spawn failed err={}", appId, GetLastError());
            return ok;
        }

        // Validate target process image path directly from kernel before injecting
        wchar_t realImagePath[MAX_PATH * 2] = {};
        DWORD pathLen = static_cast<DWORD>(std::size(realImagePath));
        if (QueryFullProcessImageNameW(pi->hProcess, 0, realImagePath, &pathLen) && pathLen > 0) {
            std::wstring normReal = NormalizePath(realImagePath);
            bool valid = (normReal == claimed.expectedExePath) ||
                         IsSubpathOf(normReal, claimed.expectedInstallDir);
            if (!valid) {
                LOG_ONLINEFIX_WARN("SECURITY: Aborting payload injection for appid={} pid={}: "
                                   "spawned image \"{}\" is NOT within expected install dir \"{}\"",
                                   appId, pi->dwProcessId, NarrowPath(normReal),
                                   NarrowPath(claimed.expectedInstallDir));
                HookStatus::RecordOnlineFixPayload(appId, pi->dwProcessId,
                                                   NarrowPath(LowerBasename(normReal.c_str())),
                                                   "path-validation-failed", "security-reject");
                // Note: The caller (Steam) owns pi->hProcess and pi->hThread and will close them.
                // Do NOT call CloseHandle here; closing them while returning TRUE to the caller
                // causes ERROR_INVALID_HANDLE in Steam and disastrous handle-recycling bugs.
                // We resume the suspended thread so the non-game process can execute normally.
                if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
                return ok;
            }
        }

        bool injected = (PayloadPath[0] != 0) && InjectPayload(pi->hProcess, PayloadPath);
        // Associate newly spawned primary process PID with its OnlineFix app.
        if (pi && pi->dwProcessId) {
            SteamCapture::AssociateOnlineFixPid(pi->dwProcessId, appId);
        }
        LOG_ONLINEFIX_INFO("appid={} pid={} payload {}", appId, pi->dwProcessId,
                           injected ? "loaded" : "FAILED");
        HookStatus::RecordOnlineFixPayload(appId, pi->dwProcessId,
                                           NarrowPath(claimed.expectedBasename),
                                           injected ? "claimed-loaded" : "claimed-failed",
                                           "createprocess");

        if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
        return ok;
    }

    BOOL WINAPI hkCreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
        LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags, LPVOID env,
        LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        return LaunchSuspended(nullptr, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }

    BOOL WINAPI hkCreateProcessAsUserW(HANDLE token, LPCWSTR app, LPWSTR cmd,
        LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
        LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
    {
        return LaunchSuspended(token, app, cmd, pa, ta, inherit, flags, env, cwd, si, pi);
    }

}

namespace {

    void ResetPayloadLogs() {
        namespace fs = std::filesystem;
        fs::path dir = fs::path(Settings::logDir) / "payload";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }

}

namespace OnlineFixInject {

    void Install() {
        if (PayloadPath[0] == 0) {
            LOG_ONLINEFIX_WARN("payload path not set; injection disabled");
            return;
        }
        if (!Settings::onlineFixInjectEnabled) {
            LOG_ONLINEFIX_INFO("online-fix injection disabled by config");
            return;
        }
        if (GetFileAttributesA(PayloadPath) == INVALID_FILE_ATTRIBUTES) {
            LOG_ONLINEFIX_WARN("payload DLL not found at \"{}\"; injection disabled", PayloadPath);
            return;
        }
        ResetPayloadLogs();
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (!k32) return;
        oCreateProcessW       = reinterpret_cast<CreateProcessW_t>      (GetProcAddress(k32, "CreateProcessW"));
        oCreateProcessAsUserW = reinterpret_cast<CreateProcessAsUserW_t>(GetProcAddress(k32, "CreateProcessAsUserW"));

        LM_TX_BEGIN();
        if (oCreateProcessW)
            DetourAttach(reinterpret_cast<PVOID*>(&oCreateProcessW),
                         reinterpret_cast<PVOID>(hkCreateProcessW));
        if (oCreateProcessAsUserW)
            DetourAttach(reinterpret_cast<PVOID*>(&oCreateProcessAsUserW),
                         reinterpret_cast<PVOID>(hkCreateProcessAsUserW));
        LM_TX_COMMIT();
        LOG_ONLINEFIX_INFO("spawn hooks installed dll=\"{}\"", PayloadPath);
    }

    void Uninstall() {
        LM_TX_BEGIN();
        if (oCreateProcessW) {
            DetourDetach(reinterpret_cast<PVOID*>(&oCreateProcessW),
                         reinterpret_cast<PVOID>(hkCreateProcessW));
            oCreateProcessW = nullptr;
        }
        if (oCreateProcessAsUserW) {
            DetourDetach(reinterpret_cast<PVOID*>(&oCreateProcessAsUserW),
                         reinterpret_cast<PVOID>(hkCreateProcessAsUserW));
            oCreateProcessAsUserW = nullptr;
        }
        LM_TX_COMMIT();

        std::lock_guard lk(g_queueLock);
        g_queue.clear();
        g_pendingRoutes.clear();
    }

    void QueueInjection(const char* exePath, AppId_t realAppId) {
        if (!realAppId || !exePath || !*exePath) return;

        std::wstring wexe = WideFromUtf8(exePath);
        std::wstring normExe = NormalizePath(wexe);
        std::wstring key = LowerBasename(normExe.c_str());
        if (key.empty()) {
            LOG_ONLINEFIX_WARN("queue skipped appid={} exe=\"{}\"", realAppId, exePath);
            return;
        }
        std::wstring installDir = DeriveInstallDir(normExe);

        std::lock_guard lk(g_queueLock);
        uint64_t now = GetTickCount64();
        std::erase_if(g_queue, [now](const QueuedInjection& q) {
            return (now - q.queuedAt) > 60000;
        });
        PrunePendingRoutesLocked(now);

        QueuedInjection q;
        q.appId = realAppId;
        q.expectedExePath = normExe;
        q.expectedInstallDir = installDir;
        q.expectedBasename = key;
        q.queuedAt = now;
        g_queue.push_back(q);

        PendingRoute& route = g_pendingRoutes[realAppId];
        route.appId = realAppId;
        route.launchExe = key;
        route.expectedExePath = normExe;
        route.expectedInstallDir = installDir;
        route.queuedAt = now;
        route.fallbackPids.clear();

        LOG_ONLINEFIX_INFO("queued appid={} exe=\"{}\" installDir=\"{}\"",
                           realAppId, NarrowPath(normExe), NarrowPath(installDir));
        HookStatus::RecordOnlineFixPayload(realAppId, 0, NarrowPath(key), "queued", "manual-route");
    }

    void RecordNoEos(uint32_t pid, const std::string& imageName, AppId_t realAppId) {
        if (!pid || !realAppId) return;
        std::wstring wide = WideFromUtf8(imageName);
        std::wstring key = LowerBasename(wide.c_str());

        std::lock_guard lk(g_queueLock);
        if (!g_pendingRoutes.contains(realAppId))
            return;
        HookStatus::RecordOnlineFixPayload(realAppId, pid, NarrowPath(key), "no-eos", "pipewatch");
    }

    bool TryFallbackInject(uint32_t pid, const std::string& imageName, AppId_t realAppId) {
        if (!pid || !realAppId) return false;
        AppId_t queuedAppId = ClaimFallbackRoute(pid, imageName, realAppId);
        if (!queuedAppId) return false;

        // Security validation: verify child process image path against expected game install directory
        {
            std::lock_guard lk(g_queueLock);
            auto it = g_pendingRoutes.find(queuedAppId);
            if (it != g_pendingRoutes.end() && !it->second.expectedInstallDir.empty()) {
                HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!hProc) {
                    LOG_ONLINEFIX_WARN("SECURITY: Aborting fallback injection for appid={} pid={}: "
                                       "unable to open process for verification (err={})",
                                       queuedAppId, pid, GetLastError());
                    HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                       "fallback-failed", "security-open-failed");
                    return false;
                }

                wchar_t realImagePath[MAX_PATH * 2] = {};
                DWORD pathLen = static_cast<DWORD>(std::size(realImagePath));
                bool queryOk = QueryFullProcessImageNameW(hProc, 0, realImagePath, &pathLen) && pathLen > 0;
                CloseHandle(hProc);

                if (!queryOk) {
                    LOG_ONLINEFIX_WARN("SECURITY: Aborting fallback injection for appid={} pid={}: "
                                       "unable to query process image name (err={})",
                                       queuedAppId, pid, GetLastError());
                    HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                       "fallback-failed", "security-query-failed");
                    return false;
                }

                std::wstring normReal = NormalizePath(realImagePath);
                bool valid = (normReal == it->second.expectedExePath) ||
                             IsSubpathOf(normReal, it->second.expectedInstallDir);
                if (!valid) {
                    LOG_ONLINEFIX_WARN("SECURITY: Aborting fallback injection for appid={} pid={}: "
                                       "process image \"{}\" is outside expected install dir \"{}\"",
                                       queuedAppId, pid, NarrowPath(normReal),
                                       NarrowPath(it->second.expectedInstallDir));
                    HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                       "fallback-failed", "security-reject");
                    return false;
                }
            }
        }

        if (PayloadPath[0] == 0) {
            LOG_ONLINEFIX_WARN("fallback appid={} pid={} payload path empty", queuedAppId, pid);
            HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                "fallback-failed", "payload-empty");
            return false;
        }
        if (!Settings::onlineFixInjectEnabled) {
            LOG_ONLINEFIX_INFO("fallback appid={} pid={} injection disabled by config", queuedAppId, pid);
            HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                "fallback-disabled", "config");
            return false;
        }
        if (GetFileAttributesA(PayloadPath) == INVALID_FILE_ATTRIBUTES) {
            LOG_ONLINEFIX_WARN("fallback appid={} pid={} payload DLL missing path=\"{}\"",
                               queuedAppId, pid, PayloadPath);
            HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                "fallback-failed", "payload-missing");
            return false;
        }

        RemoteTools::LoadResult loaded =
            RemoteTools::LoadLibraryInto(pid, std::filesystem::path(PayloadPath));
        if (loaded.ok) {
            LOG_ONLINEFIX_INFO("fallback appid={} pid={} payload {}",
                               queuedAppId, pid,
                               loaded.alreadyLoaded ? "already-loaded" : "loaded");
            HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                                loaded.alreadyLoaded ? "fallback-already-loaded" : "fallback-loaded",
                                                "pipewatch-eos");
            return true;
        }

        LOG_ONLINEFIX_WARN("fallback appid={} pid={} payload FAILED err={}",
                           queuedAppId, pid, loaded.error);
        HookStatus::RecordOnlineFixPayload(queuedAppId, pid, ImageForLog(imageName),
                                           "fallback-failed", loaded.error);
        return false;
    }

}
