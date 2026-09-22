// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#include "EpicOnlineBridge.h"
#include "EpicOnlineTypes.h"
#include "LcPayloadLogging.h"

#include <atomic>
#include <cstring>
#include <detours.h>

namespace {
    std::atomic_bool g_installed{false};

    EOS_Connect_Login_t          oLogin          = nullptr;
    EOS_Connect_CreateDeviceId_t oCreateDeviceId = nullptr;
    EOS_Connect_DeleteDeviceId_t oDeleteDeviceId = nullptr;
    EOS_IPOContainer_Add_t       oIPOAdd         = nullptr;
    EOS_Lobby_OpFn_t             oCreateLobby    = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobby      = nullptr;
    EOS_Lobby_OpFn_t             oJoinLobbyById  = nullptr;

    struct LoginCtx {
        EOS_HConnect            handle;
        EOS_Connect_OnLoginCb   cb;
        void*                   cbData;
        std::string             displayName;
    };

    std::string SteamPersonaName() {
        HMODULE sa = GetModuleHandleW(L"steam_api64.dll");
        if (!sa) sa = GetModuleHandleW(L"steam_api.dll");

        auto pFriends = sa ? reinterpret_cast<void* (*)()>(GetProcAddress(sa, "SteamFriends")) : nullptr;
        if (!pFriends && sa) {
            for (const char* v : { "SteamAPI_SteamFriends_v017", "SteamAPI_SteamFriends_v016", "SteamAPI_SteamFriends_v015" }) {
                pFriends = reinterpret_cast<void* (*)()>(GetProcAddress(sa, v));
                if (pFriends) break;
            }
        }
        auto pName    = sa ? reinterpret_cast<const char* (*)(void*)>(GetProcAddress(sa, "SteamAPI_ISteamFriends_GetPersonaName")) : nullptr;

        void* friends = pFriends ? pFriends() : nullptr;
        const char* name = (pName && friends) ? pName(friends) : nullptr;
        return (name && *name) ? name : "Unknown Player";
    }

    void EOS_CALL OnLoginDone(const EOS_Connect_LoginCallbackInfo* info) {
        auto* ctx = static_cast<LoginCtx*>(info->ClientData);
        EOS_Connect_LoginCallbackInfo out = *info;
        out.ClientData = ctx->cbData;
        if (ctx->cb) ctx->cb(&out);
        delete ctx;
    }

    void EOS_CALL OnCreateDeviceIdDone(const EOS_Connect_CreateDeviceIdCallbackInfo* info) {
        auto* ctx = static_cast<LoginCtx*>(info->ClientData);
        const bool ready = info->ResultCode == EOS_Success
                        || info->ResultCode == EOS_DuplicateNotAllowed;
        if (!ready) {
            EOS_Connect_LoginCallbackInfo fail = {};
            fail.ResultCode = info->ResultCode;
            fail.ClientData = ctx->cbData;
            if (ctx->cb) ctx->cb(&fail);
            delete ctx;
            return;
        }

        EOS_Connect_Credentials   creds{ 1, nullptr, EOS_ECT_DEVICEID_ACCESS_TOKEN };
        EOS_Connect_UserLoginInfo who  { 1, ctx->displayName.c_str() };
        EOS_Connect_LoginOptions  opts { 2, &creds, &who };
        oLogin(ctx->handle, &opts, ctx, OnLoginDone);
    }

    void EOS_CALL hkLogin(EOS_HConnect h, const EOS_Connect_LoginOptions*,
                          void* cbData, EOS_Connect_OnLoginCb cb)
    {
        auto* ctx = new LoginCtx{ h, cb, cbData, SteamPersonaName() };
        EOS_Connect_CreateDeviceIdOptions create{ 1, "PC" };
        oCreateDeviceId(h, &create, ctx, OnCreateDeviceIdDone);
    }

    EOS_EResult EOS_CALL hkIPOAdd(EOS_HIntegratedPlatformOptionsContainer, const void*) {
        return EOS_Success;
    }

    const void* MakePresenceStrippedCopy(const void* opts, size_t flagOffset, int32_t minApiVer,
                                         void* outBuf, size_t outBufSize)
    {
        if (!opts) return nullptr;
        if (*reinterpret_cast<const int32_t*>(opts) < minApiVer) return opts;

        auto* flag = reinterpret_cast<const EOS_Bool*>(
            reinterpret_cast<uintptr_t>(opts) + flagOffset);
        if (!*flag) return opts;

        const uintptr_t addr = reinterpret_cast<uintptr_t>(opts);
        const size_t bytesInFirstPage = 4096 - (addr & 0xFFF);
        size_t copyBytes = outBufSize;

        if (bytesInFirstPage < outBufSize) {
            MEMORY_BASIC_INFORMATION mbi{};
            const void* nextExpectedPage = reinterpret_cast<const void*>(addr + bytesInFirstPage);
            if (VirtualQuery(nextExpectedPage, &mbi, sizeof(mbi)) != 0 &&
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                // Next page is committed and accessible
            } else {
                copyBytes = bytesInFirstPage;
            }
        }

        if (copyBytes < flagOffset + sizeof(EOS_Bool)) {
            return opts;
        }

        std::memset(outBuf, 0, outBufSize);
        std::memcpy(outBuf, opts, copyBytes);

        auto* copyFlag = reinterpret_cast<EOS_Bool*>(
            reinterpret_cast<uintptr_t>(outBuf) + flagOffset);
        *copyFlag = 0;

        return outBuf;
    }

    void EOS_CALL hkCreateLobby(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        alignas(8) uint8_t buf[256];
        const void* effectiveOpts = MakePresenceStrippedCopy(
            opts, offsetof(EOS_Lobby_CreateLobbyOptions_Partial, bPresenceEnabled), 2, buf, sizeof(buf));
        oCreateLobby(h, effectiveOpts, cd, cb);
    }
    void EOS_CALL hkJoinLobby(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        alignas(8) uint8_t buf[256];
        const void* effectiveOpts = MakePresenceStrippedCopy(
            opts, offsetof(EOS_Lobby_JoinLobbyOptions_Partial, bPresenceEnabled), 2, buf, sizeof(buf));
        oJoinLobby(h, effectiveOpts, cd, cb);
    }
    void EOS_CALL hkJoinLobbyById(EOS_HLobby h, const void* opts, void* cd, void* cb) {
        alignas(8) uint8_t buf[256];
        const void* effectiveOpts = MakePresenceStrippedCopy(
            opts, offsetof(EOS_Lobby_JoinLobbyByIdOptions_Partial, bPresenceEnabled), 1, buf, sizeof(buf));
        oJoinLobbyById(h, effectiveOpts, cd, cb);
    }

    void EOS_CALL hkDeleteDeviceId(EOS_HConnect, const void*, void* cbData, EOS_Connect_OnDeleteDeviceIdCb cb) {
        PayloadLog::Write("EOS_Connect_DeleteDeviceId suppressed (preserving device credentials)");
        if (cb) {
            EOS_Connect_DeleteDeviceIdCallbackInfo info{ EOS_Success, cbData };
            cb(&info);
        }
    }

    template <typename Fn>
    bool Resolve(HMODULE m, const char* name, Fn& slot, int paramBytes = -1) {
        slot = reinterpret_cast<Fn>(GetProcAddress(m, name));
#if !defined(_WIN64)
        if (!slot && paramBytes >= 0) {
            std::string decorated = "_" + std::string(name) + "@" + std::to_string(paramBytes);
            slot = reinterpret_cast<Fn>(GetProcAddress(m, decorated.c_str()));
        }
#endif
        if (!slot) PayloadLog::Write(std::string("missing EOS export: ") + name);
        return slot != nullptr;
    }
}

namespace EosBridge {
    void InstallOn(HMODULE eos) {
        bool expected = false;
        if (!eos || !g_installed.compare_exchange_strong(expected, true)) return;

        bool ok = Resolve(eos, "EOS_Connect_Login",                          oLogin, 16)
                & Resolve(eos, "EOS_Connect_CreateDeviceId",                 oCreateDeviceId, 16)
                & Resolve(eos, "EOS_IntegratedPlatformOptionsContainer_Add", oIPOAdd, 8)
                & Resolve(eos, "EOS_Lobby_CreateLobby",                      oCreateLobby, 16)
                & Resolve(eos, "EOS_Lobby_JoinLobby",                        oJoinLobby, 16)
                & Resolve(eos, "EOS_Lobby_JoinLobbyById",                    oJoinLobbyById, 16);
        if (!ok) { g_installed.store(false); return; }

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&oLogin),         reinterpret_cast<PVOID>(hkLogin));
        DetourAttach(reinterpret_cast<PVOID*>(&oIPOAdd),        reinterpret_cast<PVOID>(hkIPOAdd));
        DetourAttach(reinterpret_cast<PVOID*>(&oCreateLobby),   reinterpret_cast<PVOID>(hkCreateLobby));
        DetourAttach(reinterpret_cast<PVOID*>(&oJoinLobby),     reinterpret_cast<PVOID>(hkJoinLobby));
        DetourAttach(reinterpret_cast<PVOID*>(&oJoinLobbyById), reinterpret_cast<PVOID>(hkJoinLobbyById));
        if (Resolve(eos, "EOS_Connect_DeleteDeviceId", oDeleteDeviceId, 16)) {
            DetourAttach(reinterpret_cast<PVOID*>(&oDeleteDeviceId), reinterpret_cast<PVOID>(hkDeleteDeviceId));
        }
        // retry commit up to 3 times with backoff - detours can transiently fail
        // if steam is modifying the same code page during startup
        LONG err = NO_ERROR;
        for (int retry = 0; retry < 3; ++retry) {
            err = DetourTransactionCommit();
            if (err == NO_ERROR) break;
            if (retry < 2) Sleep(20u << retry); // 20ms, 40ms
        }
        if (err != NO_ERROR) {
            PayloadLog::Write("DetourTransactionCommit failed after retries err=" + std::to_string(err));
            g_installed.store(false);
            return;
        }
        PayloadLog::Write("EOS hooks installed");
    }
}
