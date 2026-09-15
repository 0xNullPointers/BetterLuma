// BetterLumaCore - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "hooks/client/IPCBus.h"
#include "hooks/client/CmdUser.h"
#include "hooks/client/CmdUtils.h"
#include "hooks/Macros.h"
#include "hooks/client/PipeWatch.h"
#include "core/entry.h"
#include "runtime/LcFnvHash.h"
#include "runtime/IpcSpecLoader.h"
#include "hooks/capture/SteamCapture.h"
#include <map>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <cctype>

// ── pipe retriever, needed by LM_BIND macro (fn##_t expansion) ──
using GetPipeClient_t = CSteamPipeClient*(*)(void* pEngine, HSteamPipe hSteamPipe);
GetPipeClient_t oGetPipeClient = nullptr;

namespace IPCBus::Registry {

    static CSteamPipeClient* PipeForHandle(void* pServer, HSteamPipe handle) {
        return oGetPipeClient ? oGetPipeClient(pServer, handle) : nullptr;
    }

    static constexpr uint64_t BuildKey(EIPCInterface iface, uint32_t funcHash) {
        return (static_cast<uint64_t>(iface) << 32) | funcHash;
    }

    static std::map<uint64_t, IpcHandlerEntry> s_table;

    void Add(const IpcHandlerEntry* entries, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            auto entry = entries[i];
            if (IpcSpecLoader::IsLoaded()) {
                auto specHash = IpcSpecLoader::ResolveHash(entry.name);
                if (specHash)
                    entry.funcHash = *specHash;
            }
            s_table.emplace(BuildKey(entry.interfaceID, entry.funcHash), entry);
        }
    }

    const IpcHandlerEntry* Lookup(EIPCInterface iface, uint32_t funcHash) {
        auto it = s_table.find(BuildKey(iface, funcHash));
        return (it != s_table.end()) ? &it->second : nullptr;
    }

    void Clear() {
        s_table.clear();
    }

    // internal pipes (engine-side, appid=0) get passthrough treatment
    static bool IsInternal(const CSteamPipeClient* pipe) {
        return !pipe || ((pipe->m_hSteamPipe & 0xFFFF) <= 2);
    }

    struct CallFrame {
        CSteamPipeClient*       pipe    = nullptr;
        const IpcHandlerEntry*  handler = nullptr;
        bool                    statsCall = false;
        bool                    Good() const { return pipe && handler; }
    };

    static CallFrame SetupFrame(void* pServer, HSteamPipe hPipe, CUtlBuffer* pRead) {
        CallFrame f;
        f.pipe = PipeForHandle(pServer, hPipe);
        if (!f.pipe) return f;

        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const auto* raw = pRead->Base();
            const auto ec = static_cast<EIPCCommand>(raw[OFFSET_CMD]);

            LOG_IPCRTR_INFO("\"cmd\" \"{}\" \"pipe\" \"0x{:08X}\" \"size\" {}",
                EIPCCommandName(ec), f.pipe->m_hSteamPipe, pRead->TellPut());

            if (ec == EIPCCommand::Handshake) {
                LOG_IPCRTR_INFO("\"evt\" \"handshake\" \"pipe\" \"{}\"", f.pipe->DebugString());
                if (!IsInternal(f.pipe))
                    PipeWatch::OnHandshake(f.pipe, pRead);
            } else if (ec == EIPCCommand::InterfaceCall) {
                if (IsInternal(f.pipe)) {
                    LOG_IPCRTR_INFO("\"cmd\" \"InterfaceCall\" \"pipe\" \"0x{:08X}\" \"action\" \"passthrough\"",
                        f.pipe->m_hSteamPipe);
                    return CallFrame{f.pipe, nullptr, false};
                }
                PipeWatch::TouchPipe(f.pipe);
                const auto iface = static_cast<EIPCInterface>(raw[OFFSET_INTERFACE_ID]);
                const uint32_t fHash = *reinterpret_cast<const uint32_t*>(raw + OFFSET_FUNC_HASH);
                f.statsCall = (iface == EIPCInterface::IClientUserStats);
                f.handler = Lookup(iface, fHash);
                if (f.handler) {
                    LOG_IPCRTR_INFO("\"cmd\" \"InterfaceCall\" \"name\" \"{}\" \"pipe\" \"{}\" \"realAppId\" {} \"AppId\" {}",
                        f.handler->name, f.pipe->DebugString(),
                        SteamCapture::ResolveAppId(), SteamCapture::GetAppIDForCurrentPipe());
                } else {
                    LOG_IPCRTR_INFO("\"cmd\" \"InterfaceCall\" \"iface\" \"{}\" \"hash\" \"0x{:08X}\" \"pipe\" \"{}\" \"realAppId\" {} \"AppId\" {}",
                        EIPCInterfaceName(iface), fHash, f.pipe->DebugString(),
                        SteamCapture::ResolveAppId(), SteamCapture::GetAppIDForCurrentPipe());
                }
            } else {
                LOG_IPCRTR_INFO("\"cmd\" \"{}\" \"pipe\" \"{}\"", EIPCCommandName(ec), f.pipe->DebugString());
            }
        }
        return f;
    }

} // namespace IPCBus::Registry

namespace {
    using namespace IPCBus::Registry;

    // RAII guard: enters stats scope on construction, leaves on destruction.
    // only activates when statsCall is true - no-ops otherwise.
    struct StatsGuard {
        bool m_active;
        HSteamPipe m_pipe;
        StatsGuard(bool doActivate, HSteamPipe pipe, AppId_t appId = 0) : m_active(doActivate), m_pipe(pipe) {
            if (m_active) {
                SteamCapture::SetUserStatsContext(true);
                SteamCapture::EnterStatsScope(m_pipe, appId);
            }
        }
        ~StatsGuard() {
            if (m_active) {
                SteamCapture::LeaveStatsScope();
                SteamCapture::SetUserStatsContext(false);
            }
        }
    };

    // Type-safe enum matching Valve's ERemoteStorageFileRoot
    enum ERemoteStorageFileRoot : uint32_t {
        k_ERemoteStorageFileRootInvalid = static_cast<uint32_t>(-1),
        k_ERemoteStorageFileRootDefault = 0,
        k_ERemoteStorageFileRootGameInstall = 1,
        k_ERemoteStorageFileRootWinMyDocuments = 2,
        k_ERemoteStorageFileRootWinAppDataLocal = 3,
        k_ERemoteStorageFileRootWinAppDataRoaming = 4,
    };

    LM_HOOK(IClientRemoteStorage_FileExists, bool,
            void* pThis, AppId_t appId, uint32_t fileRoot, const char* pchFile)
    {
        if (!pchFile || !*pchFile) {
            return false;
        }

        AppId_t targetAppId = appId;
        const AppId_t real = SteamCapture::ActiveRouteRealAppId();
        if (real && (appId == kOnlineFixAppId || SteamCapture::IsOnlineFixApp(appId))) {
            targetAppId = real;
            LOG_IPCRTR_INFO("\"evt\" \"RemoteStorage\" \"fn\" \"FileExists\" \"redirect\" \"{}->{}\" \"file\" \"{}\"",
                            appId, targetAppId, pchFile);
        }

        if (!oIClientRemoteStorage_FileExists) {
            LOG_IPCRTR_WARN("\"evt\" \"RemoteStorage\" \"fn\" \"FileExists\" \"err\" \"null-trampoline\"");
            return false;
        }

        return oIClientRemoteStorage_FileExists(pThis, targetAppId, fileRoot, pchFile);
    }

    // Validates that the relative save path extracted from IPC is safe and does not contain
    // directory traversal, drive designators, stream colons, or reserved device names.
    // Allows subdirectories (e.g. "saves/slot1.sav" or "Profiles\Player\Save.dat").
    static bool IsValidSaveFilename(std::string_view filename) {
        if (filename.empty() || filename.size() >= 260)
            return false;

        // Cannot start with a path separator (no absolute / UNC paths)
        if (filename.front() == '/' || filename.front() == '\\')
            return false;

        // Reject drive designators, alternate data streams, and directory traversal sequences
        if (filename.find(':') != std::string_view::npos ||
            filename.find("..") != std::string_view::npos) {
            return false;
        }

        static constexpr std::string_view kReservedNames[] = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };

        // Validate each component between separators
        size_t start = 0;
        while (start < filename.size()) {
            size_t end = filename.find_first_of("/\\", start);
            if (end == std::string_view::npos)
                end = filename.size();

            std::string_view comp = filename.substr(start, end - start);
            if (comp.empty())
                return false; // consecutive slashes like "foo//bar" not allowed

            // Windows forbids leading/trailing dots and spaces in path components
            if (comp.front() == '.' || comp.back() == '.' || comp.back() == ' ')
                return false;

            for (char c : comp) {
                // Reject non-printable ASCII or control characters, and Windows illegal filename characters
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7F ||
                    c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
                    return false;
                }
            }

            // Reject Windows reserved DOS device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9)
            auto baseName = comp;
            auto dotPos = baseName.find('.');
            if (dotPos != std::string_view::npos) {
                baseName = baseName.substr(0, dotPos);
            }

            for (const auto& reserved : kReservedNames) {
                if (baseName.size() == reserved.size()) {
                    bool match = true;
                    for (size_t i = 0; i < baseName.size(); ++i) {
                        if (toupper(static_cast<unsigned char>(baseName[i])) != reserved[i]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return false;
                }
            }

            start = end + 1;
        }

        return true;
    }

    // Resolves and canonicalizes target save file path, verifying it strictly resides
    // within the intended userdata/<steamId32>/<appId>/remote/ directory.
    static bool ResolveSecureSavePath(const char* steamRoot, DWORD steamId32, AppId_t appId,
                                      const char* filename, char* outPath, size_t outPathSize) {
        if (!steamRoot || !steamRoot[0] || !steamId32 || !appId || !filename || !filename[0])
            return false;

        if (!IsValidSaveFilename(filename))
            return false;

        std::error_code ec;
        std::filesystem::path root(steamRoot);
        std::filesystem::path baseDir = root / "userdata" / std::to_string(steamId32) / std::to_string(appId) / "remote";
        std::filesystem::path targetFile = baseDir / filename;

        std::filesystem::path canonicalBase = std::filesystem::weakly_canonical(baseDir, ec);
        if (ec) return false;

        // Strictly reject symlinks, junctions, and non-regular files to prevent arbitrary file read/traversal
        std::filesystem::path canonicalTarget;
        if (std::filesystem::exists(targetFile, ec)) {
            if (std::filesystem::is_symlink(targetFile, ec))
                return false;

            // For existing files, use canonical to strictly resolve all path components and symlinks
            canonicalTarget = std::filesystem::canonical(targetFile, ec);
            if (ec) return false;

            // Ensure the resolved canonical target is a regular file (not directory, junction, or device)
            if (!std::filesystem::is_regular_file(canonicalTarget, ec))
                return false;
        } else {
            // Target does not exist yet (e.g. FileExists probing for uncreated saves)
            canonicalTarget = std::filesystem::weakly_canonical(targetFile, ec);
            if (ec) return false;
        }

        // Verify that canonicalTarget is strictly a child under canonicalBase
        auto b = canonicalBase.begin();
        auto t = canonicalTarget.begin();
        while (b != canonicalBase.end() && t != canonicalTarget.end()) {
            if (_wcsicmp(b->c_str(), t->c_str()) != 0)
                return false;
            ++b;
            ++t;
        }

        if (b != canonicalBase.end() || t == canonicalTarget.end())
            return false;

        std::string safeStr = canonicalTarget.string();
        // Strip extended path prefix if present to ensure standard Win32 ANSI API compatibility
        if (safeStr.rfind(R"(\\?\)", 0) == 0) {
            safeStr.erase(0, 4);
        }

        if (safeStr.empty() || safeStr.size() >= outPathSize)
            return false;

        strcpy_s(outPath, outPathSize, safeStr.c_str());
        return true;
    }

    struct VerifiedSaveFile {
        HANDLE hFile = INVALID_HANDLE_VALUE;
        DWORD fileSize = 0;

        ~VerifiedSaveFile() {
            if (hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(hFile);
                hFile = INVALID_HANDLE_VALUE;
            }
        }
    };

    // Atomically opens target save file and verifies its physical location via kernel handle
    // to eliminate TOCTOU symlink/junction swap races.
    static bool OpenAndVerifySecureSaveFile(const char* targetPath, const char* steamRoot,
                                           DWORD steamId32, AppId_t appId,
                                           VerifiedSaveFile& out) {
        if (!targetPath || !targetPath[0] || !steamRoot || !steamRoot[0])
            return false;

        // Open with FILE_FLAG_OPEN_REPARSE_POINT to prevent following a symlink or junction at target
        HANDLE h = CreateFileA(targetPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h, &info)) {
            CloseHandle(h);
            return false;
        }

        // Must be a regular file, strictly forbidding directories or reparse points
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(h);
            return false;
        }

        // Query kernel for the physical canonical path of the open file object
        char finalPath[MAX_PATH * 2]{};
        DWORD len = GetFinalPathNameByHandleA(h, finalPath, sizeof(finalPath), FILE_NAME_NORMALIZED);
        if (len == 0 || len >= sizeof(finalPath)) {
            CloseHandle(h);
            return false;
        }

        // Build canonical base path
        std::error_code ec;
        std::filesystem::path baseDir = std::filesystem::path(steamRoot) / "userdata" /
                                       std::to_string(steamId32) / std::to_string(appId) / "remote";
        std::filesystem::path canonicalBase = std::filesystem::weakly_canonical(baseDir, ec);
        if (ec) {
            CloseHandle(h);
            return false;
        }

        std::filesystem::path resolvedPath(finalPath);
        std::string resolvedStr = resolvedPath.string();
        if (resolvedStr.rfind(R"(\\?\)", 0) == 0) {
            resolvedStr.erase(0, 4);
            resolvedPath = resolvedStr;
        }

        // Verify resolved path strictly resides inside canonicalBase
        auto b = canonicalBase.begin();
        auto t = resolvedPath.begin();
        while (b != canonicalBase.end() && t != resolvedPath.end()) {
            if (_wcsicmp(b->c_str(), t->c_str()) != 0) {
                CloseHandle(h);
                return false;
            }
            ++b;
            ++t;
        }
        if (b != canonicalBase.end() || t == resolvedPath.end()) {
            CloseHandle(h);
            return false;
        }

        out.hFile = h;
        out.fileSize = info.nFileSizeLow;
        return true;
    }

    LM_HOOK(IPCProcessMessage, bool,
              void* pServer, HSteamPipe hPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        if (pRead->TellPut() >= IPC_HEADER_SIZE) {
            const auto* raw = pRead->Base();
            if (raw[OFFSET_CMD] == static_cast<uint8_t>(EIPCCommand::InterfaceCall)) {
                const auto iface = static_cast<EIPCInterface>(raw[OFFSET_INTERFACE_ID]);
                if (iface == EIPCInterface::IClientNetworkingSocketsSerialized)
                    SteamCapture::NotifyNetworkingSocketsUsed();
                if (iface == EIPCInterface::IClientRemoteStorage) {
                    uint32_t fHash = *reinterpret_cast<const uint32_t*>(raw + OFFSET_FUNC_HASH);
                    // Resolve targeted OnlineFix AppID for pipe process before active route fallback.
                    AppId_t real = 0;
                    if (auto* pClient = PipeForHandle(pServer, hPipe)) {
                        real = SteamCapture::GetOnlineFixAppForPid(pClient->m_clientPID);
                    }
                    if (!real) real = SteamCapture::ActiveRouteRealAppId();
                    if (real && (fHash == 0x376E83D6 || fHash == 0xC69A678D || fHash == 0xA0F6FDBD)) {
                        DWORD steamId32 = 0;
                        HKEY hKey;
                        if (RegOpenKeyExA(HKEY_CURRENT_USER,
                                "Software\\Valve\\Steam\\ActiveProcess", 0,
                                KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
                            DWORD cb = sizeof(steamId32);
                            RegQueryValueExA(hKey, "ActiveUser", nullptr, nullptr,
                                             reinterpret_cast<LPBYTE>(&steamId32), &cb);
                            RegCloseKey(hKey);
                        }
                        if (steamId32) {
                            const uint8_t* reqData = raw + IPC_HEADER_SIZE;
                            const uint32_t reqLen = pRead->TellPut() - IPC_HEADER_SIZE;
                            char filenameBuf[256]{};
                            {
                                const char* p = reinterpret_cast<const char*>(reqData);
                                const char* end = p + reqLen;
                                const char* best = nullptr;
                                int bestLen = 0;
                                while (p < end) {
                                    if (*p >= 0x20 && *p < 0x7F) {
                                        const char* start = p;
                                        while (p < end && *p >= 0x20 && *p < 0x7F) ++p;
                                        int len = static_cast<int>(p - start);
                                        if (len > bestLen) { bestLen = len; best = start; }
                                    } else { ++p; }
                                }
                                if (best && bestLen > 0 && bestLen < 256)
                                    memcpy(filenameBuf, best, bestLen);
                            }
                            if (filenameBuf[0]) {
                                char savePath[512];
                                if (!ResolveSecureSavePath(SteamInstallPath, steamId32, real, filenameBuf, savePath, sizeof(savePath))) {
                                    LOG_IPCRTR_WARN("\"evt\" \"SaveInject\" \"err\" \"path-rejected\" \"file\" \"{}\"", filenameBuf);
                                } else {
                                    auto ensureCap = [&](uint32_t need) -> bool {
                                        if (pWrite->m_Memory.m_nAllocationCount >= need) return true;
                                        if (!pWrite->m_PutOverflowFunc) return false;
                                        return (pWrite->*pWrite->m_PutOverflowFunc)(need);
                                    };

                                if (fHash == 0x376E83D6) {  // FileExists
                                    VerifiedSaveFile vsf;
                                    if (OpenAndVerifySecureSaveFile(savePath, SteamInstallPath, steamId32, real, vsf)) {
                                        if (ensureCap(14)) {
                                            uint8_t frame[14] = {};
                                            uint32_t len = 14; memcpy(frame, &len, 4);
                                            uint32_t result = 0; memcpy(frame + 4, &result, 4);
                                            frame[8] = 1;
                                            memcpy(pWrite->Base(), frame, 14);
                                            pWrite->m_Put = 14;
                                            LOG_IPCRTR_INFO("\"evt\" \"SaveInject\" \"fn\" \"FileExists\" \"path\" \"{}\" \"result\" \"found\"", savePath);
                                            return true;
                                        }
                                        LOG_IPCRTR_INFO("\"evt\" \"SaveInject\" \"fn\" \"FileExists\" \"path\" \"{}\" \"result\" \"cap-grow-failed\"", savePath);
                                    } else {
                                        LOG_IPCRTR_INFO("\"evt\" \"SaveInject\" \"fn\" \"FileExists\" \"path\" \"{}\" \"result\" \"not-found\"", savePath);
                                    }
                                } else if (fHash == 0xC69A678D) {  // GetFileSize
                                    VerifiedSaveFile vsf;
                                    if (OpenAndVerifySecureSaveFile(savePath, SteamInstallPath, steamId32, real, vsf)) {
                                        DWORD fileSize = vsf.fileSize;
                                        if (fileSize != INVALID_FILE_SIZE && ensureCap(14)) {
                                            uint8_t frame[14] = {};
                                            uint32_t len = 14; memcpy(frame, &len, 4);
                                            uint32_t result = 0; memcpy(frame + 4, &result, 4);
                                            memcpy(frame + 8, &fileSize, 4);
                                            memcpy(pWrite->Base(), frame, 14);
                                            pWrite->m_Put = 14;
                                            LOG_IPCRTR_INFO("\"evt\" \"SaveInject\" \"fn\" \"GetFileSize\" \"path\" \"{}\" \"size\" {}", savePath, fileSize);
                                            return true;
                                        }
                                    }
                                } else if (fHash == 0xA0F6FDBD) {  // FileRead
                                    VerifiedSaveFile vsf;
                                    if (OpenAndVerifySecureSaveFile(savePath, SteamInstallPath, steamId32, real, vsf)) {
                                        DWORD fileSize = vsf.fileSize;
                                        if (fileSize != INVALID_FILE_SIZE && fileSize < 1 * 1024 * 1024
                                            && ensureCap(14u + fileSize)) {
                                            uint8_t frame[14] = {};
                                            uint32_t totalLen = 14 + fileSize; memcpy(frame, &totalLen, 4);
                                            uint32_t result = 0; memcpy(frame + 4, &result, 4);
                                            memcpy(frame + 8, &fileSize, 4);
                                            memcpy(pWrite->Base(), frame, 14);
                                            DWORD read = 0;
                                            ReadFile(vsf.hFile, pWrite->Base() + 14, fileSize, &read, nullptr);
                                            pWrite->m_Put = 14 + read;
                                            LOG_IPCRTR_INFO("\"evt\" \"SaveInject\" \"fn\" \"FileRead\" \"path\" \"{}\" \"size\" {} \"read\" {}", savePath, fileSize, read);
                                            return true;
                                        }
                                    } else {
                                        LOG_IPCRTR_WARN("\"evt\" \"SaveInject\" \"fn\" \"FileRead\" \"err\" \"open-or-verify-failed\" \"path\" \"{}\"", savePath);
                                    }
                                }
                                }
                            }
                        }
                    }
                }
            }
        }

        auto f = SetupFrame(pServer, hPipe, pRead);
        AppId_t statsAppId = 0;
        if (f.statsCall && f.pipe) {
            statsAppId = SteamCapture::GetOnlineFixAppForPid(f.pipe->m_clientPID);
        }
        StatsGuard guard(f.statsCall, hPipe, statsAppId);

        const bool ok = oIPCProcessMessage(pServer, hPipe, pRead, pWrite);

        if (!ok || !f.handler) return ok;

        AppId_t appId = SteamCapture::ResolveAppId();
        if (!LuaLoader::HasDepot(appId)) {
            LOG_IPCRTR_INFO("\"cmd\" \"{}\" \"appId\" {} \"action\" \"skip-nodepot\" \"pipe\" \"{}\"",
                f.handler->name, appId, f.pipe ? f.pipe->DebugString() : "null");
            return ok;
        }

        f.handler->handler(f.pipe, pRead, pWrite);
        return ok;
    }

} // namespace

namespace IPCBus {

    void RegisterHandlers(const IpcHandlerEntry* entries, size_t count) {
        Registry::Add(entries, count);
    }

    void Install() {
        LM_BIND(GetPipeClient);
        CmdUser::Register();

        LM_TX_BEGIN();
        LM_INSTALL(IPCProcessMessage);
        {
            static constexpr StringXRefSig kFileExistsSigs[] = {
                {"IClientRemoteStorage::FileExists", ""},
            };
            LM_INSTALL_STR(IClientRemoteStorage_FileExists, kFileExistsSigs, 1);
        }
        LM_TX_COMMIT();

        LOG_IPCRTR_INFO("\"event\" \"install\" \"hook\" \"0x{:X}\"",
                       reinterpret_cast<uintptr_t>(oIPCProcessMessage));
    }

    void Uninstall() {
        LM_TX_BEGIN();
        LM_REMOVE(IPCProcessMessage);
        LM_REMOVE(IClientRemoteStorage_FileExists);
        LM_TX_COMMIT();
        oGetPipeClient = nullptr;
        Registry::Clear();
        PipeWatch::Reset();
    }

}
