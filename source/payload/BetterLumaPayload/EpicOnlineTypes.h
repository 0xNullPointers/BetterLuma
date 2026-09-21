// BetterLuma - Steam client hook layer.
// Modified from LumaCore, 2026.
// Distributed under the GNU General Public License v3 or later.
// Original work and copyright: see README.md.

#pragma once

#include <cstdint>

using EOS_EResult = int32_t;
using EOS_Bool    = int32_t;
using EOS_HConnect = void*;
using EOS_HLobby   = void*;
using EOS_HIntegratedPlatformOptionsContainer = void*;
using EOS_ProductUserId    = void*;
using EOS_ContinuanceToken = void*;

constexpr EOS_EResult EOS_Success             = 0;
constexpr EOS_EResult EOS_DuplicateNotAllowed = 24;
constexpr int32_t EOS_ECT_DEVICEID_ACCESS_TOKEN = 10;

#pragma pack(push, 8)

struct EOS_Connect_Credentials {
    int32_t  ApiVersion;
    const char* Token;
    int32_t  Type;
};
struct EOS_Connect_UserLoginInfo {
    int32_t  ApiVersion;
    const char* DisplayName;
};
struct EOS_Connect_LoginOptions {
    int32_t  ApiVersion;
    const EOS_Connect_Credentials*   Credentials;
    const EOS_Connect_UserLoginInfo* UserLoginInfo;
};
struct EOS_Connect_LoginCallbackInfo {
    EOS_EResult ResultCode;
    void*       ClientData;
    EOS_ProductUserId    LocalUserId;
    EOS_ContinuanceToken ContinuanceToken;
};
struct EOS_Connect_CreateDeviceIdOptions {
    int32_t  ApiVersion;
    const char* DeviceModel;
};
struct EOS_Connect_CreateDeviceIdCallbackInfo {
    EOS_EResult ResultCode;
    void*       ClientData;
};
struct EOS_Connect_DeleteDeviceIdCallbackInfo {
    EOS_EResult ResultCode;
    void*       ClientData;
};

struct EOS_Lobby_CreateLobbyOptions_Partial {
    int32_t           ApiVersion;
    EOS_ProductUserId LocalUserId;
    uint32_t          MaxLobbyMembers;
    int32_t           PermissionLevel;
    EOS_Bool          bPresenceEnabled;
};
struct EOS_Lobby_JoinLobbyOptions_Partial {
    int32_t           ApiVersion;
    void*             LobbyDetailsHandle;
    EOS_ProductUserId LocalUserId;
    EOS_Bool          bPresenceEnabled;
};
struct EOS_Lobby_JoinLobbyByIdOptions_Partial {
    int32_t           ApiVersion;
    const char*       LobbyId;
    EOS_ProductUserId LocalUserId;
    EOS_Bool          bPresenceEnabled;
};

#pragma pack(pop)

#if defined(_WIN32) && !defined(_WIN64)
#define EOS_CALL __stdcall
#else
#define EOS_CALL
#endif

using EOS_Connect_OnLoginCb          = void(EOS_CALL *)(const EOS_Connect_LoginCallbackInfo*);
using EOS_Connect_OnCreateDeviceIdCb = void(EOS_CALL *)(const EOS_Connect_CreateDeviceIdCallbackInfo*);
using EOS_Connect_OnDeleteDeviceIdCb = void(EOS_CALL *)(const EOS_Connect_DeleteDeviceIdCallbackInfo*);

using EOS_Connect_Login_t          = void(EOS_CALL *)(EOS_HConnect, const EOS_Connect_LoginOptions*, void*, EOS_Connect_OnLoginCb);
using EOS_Connect_CreateDeviceId_t = void(EOS_CALL *)(EOS_HConnect, const EOS_Connect_CreateDeviceIdOptions*, void*, EOS_Connect_OnCreateDeviceIdCb);
using EOS_Connect_DeleteDeviceId_t = void(EOS_CALL *)(EOS_HConnect, const void*, void*, EOS_Connect_OnDeleteDeviceIdCb);
using EOS_IPOContainer_Add_t       = EOS_EResult(EOS_CALL *)(EOS_HIntegratedPlatformOptionsContainer, const void*);
using EOS_Lobby_OpFn_t             = void(EOS_CALL *)(EOS_HLobby, const void*, void*, void*);
