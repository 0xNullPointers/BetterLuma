cmake_minimum_required(VERSION 3.20)
project(BetterLuma LANGUAGES C CXX)

# Determine release version: taken directly from ENV{RELEASE_VERSION} or ENV{BETTERLUMA_VERSION}
if(DEFINED ENV{RELEASE_VERSION} AND NOT "$ENV{RELEASE_VERSION}" STREQUAL "")
    set(BETTERLUMA_VERSION "$ENV{RELEASE_VERSION}")
elseif(DEFINED ENV{BETTERLUMA_VERSION} AND NOT "$ENV{BETTERLUMA_VERSION}" STREQUAL "")
    set(BETTERLUMA_VERSION "$ENV{BETTERLUMA_VERSION}")
else()
    set(BETTERLUMA_VERSION "debug")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Ensure consistent binary output layout across all generators (VS, Ninja, etc.)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LcLua)
include(LcDetours)
include(LcSpdlog)
include(LcProtobuf)
include(LcTomlplusplus)
include(LcLogMacros)

set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR       "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PROTO_GEN_LITE_DIR  "${CMAKE_CURRENT_BINARY_DIR}/proto_lite")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=${PROTO_GEN_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf full-Message sources (Debug)"
)

add_custom_command(
    OUTPUT "${PROTO_GEN_LITE_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_LITE_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_LITE_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=lite:${PROTO_GEN_LITE_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf lite-MessageLite sources (Release)"
)

add_library(BetterLuma SHARED
    core/entry.cpp
    core/LoaderGate.cpp
    core/Orchestrator.cpp

    runtime/Ticket.cpp
    runtime/Logger.cpp
    runtime/HookStatus.cpp
    runtime/BuildInfo.cpp
    runtime/DirWatch.cpp
    runtime/VehUtil.cpp
    runtime/RuntimeHttp.cpp
    runtime/ManifestFetch.cpp
    runtime/ProtectionProbe.cpp
    runtime/RemoteTools.cpp
    runtime/IpcSpecLoader.cpp
    runtime/BootDiag.cpp
    runtime/HashUtil.cpp
    runtime/CredentialStore.cpp
    runtime/EticketFetcher.cpp
    runtime/TicketProvider.cpp
    runtime/ProcessInspect.cpp
    runtime/IntegrityScanner.cpp
    runtime/LibraryInjector.cpp
    runtime/CloudRedirectHost.cpp

    patterns/ByteScan.cpp
    patterns/PatternSig.cpp
    patterns/PatternFetcher.cpp

    config/LuaState.cpp
    config/LuaBindings.cpp
    config/LuaQuery.cpp
    config/Settings.cpp

    hooks/client/DepotKeys.cpp
    hooks/client/IPCBus.cpp
    hooks/client/CmdUser.cpp
    hooks/client/CmdUtils.cpp
    hooks/client/ManifestBind.cpp
    hooks/client/PacketRouter.cpp
    hooks/client/NetPacket.cpp
    hooks/client/NetPacket_AccessToken.cpp
    hooks/client/NetPacket_UserStats.cpp
    hooks/client/NetPacket_ETicket.cpp
    hooks/client/NetPacket_Manifest.cpp
    hooks/client/NetPacket_FamilySharing.cpp
    hooks/client/NetPacket_OnlineFix.cpp
    hooks/client/NetPacket_SteamStub.cpp
    hooks/client/NetPacket_Cloud.cpp
    hooks/client/PackagePatch.cpp
    hooks/client/RichPresence.cpp
    hooks/client/LicenseHooks.cpp
    hooks/client/StringFind.cpp
    hooks/client/PipeWatch.cpp
    hooks/client/AuthWindow.cpp
    hooks/client/AsyncTicketMap.cpp
    hooks/client/DecryptionKeyHook.cpp
    hooks/client/PendingCallMap.cpp
    hooks/client/OnlineFixInject.cpp
    hooks/client/ProcessExtension.cpp
    hooks/client/SteamStubAuto.cpp
    hooks/client/SteamStubTicket.cpp
    hooks/client/IpcMethodLoader.cpp
    hooks/client/IpcDispatch.cpp
    hooks/client/IpcHooks.cpp
    hooks/client/IpcHandlers_ISteamUser.cpp
    hooks/client/IpcHandlers_ISteamUtils.cpp
    hooks/client/DenuvoAuthenticator.cpp
    hooks/client/GlobalAchievementHooks.cpp

    stats/GlobalAchievementManager.cpp

    hooks/capture/RuntimeCapture.cpp

    hooks/ui/SteamUI.cpp
    hooks/ui/StatusWriter.cpp

    $<$<CONFIG:Debug>:${PROTO_GEN_DIR}/steam_messages.pb.cc>
    $<$<CONFIG:Release>:${PROTO_GEN_LITE_DIR}/steam_messages.pb.cc>
)
set_target_properties(BetterLuma PROPERTIES PREFIX "" OUTPUT_NAME "BetterLuma")

set(BUILD_STAMP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildStamp.h")

# Ensure build stamp header exists at configure time
get_filename_component(_STAMP_DIR "${BUILD_STAMP_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${_STAMP_DIR}")
if(NOT EXISTS "${BUILD_STAMP_HEADER}")
    string(TIMESTAMP _INIT_STAMP "%b %d %Y %H:%M:%S")
    file(WRITE "${BUILD_STAMP_HEADER}" "// Auto-generated by CMake. Do not edit directly.\n#pragma once\n\n#define BETTERLUMA_VERSION_STRING \"${BETTERLUMA_VERSION}\"\n#define BETTERLUMA_BUILD_STAMP_STRING \"${_INIT_STAMP}\"\n")
endif()

add_custom_target(GenerateBuildStamp
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_FILE=${BUILD_STAMP_HEADER}
        -DVERSION=${BETTERLUMA_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateBuildStamp.cmake
    COMMENT "Generating fresh BetterLuma build stamp and version"
    BYPRODUCTS "${BUILD_STAMP_HEADER}"
)

add_dependencies(BetterLuma GenerateBuildStamp)

target_include_directories(BetterLuma PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    $<$<CONFIG:Debug>:${PROTO_GEN_DIR}>
    $<$<CONFIG:Release>:${PROTO_GEN_LITE_DIR}>
)

target_link_libraries(BetterLuma PRIVATE
    lua_static
    detours
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
    winhttp
    bcrypt
)

target_compile_definitions(BetterLuma PRIVATE
    $<$<CONFIG:Debug>:BETTERLUMA_LOGGING_ENABLED>
)

# ---------------------------------------------------------------------------
# BetterLumaPayload - DLL injected into a game launched with -onlinefix
# ---------------------------------------------------------------------------
add_library(BetterLumaPayload SHARED
    payload/BetterLumaPayload/BetterLumaPayloadEntry.cpp
    payload/BetterLumaPayload/LcPayloadLogging.cpp
    payload/BetterLumaPayload/EpicOnlineBridge.cpp
    payload/BetterLumaPayload/PayloadPropagator.cpp
)
set_target_properties(BetterLumaPayload PROPERTIES PREFIX "" OUTPUT_NAME "BetterLumaPayload")

target_include_directories(BetterLumaPayload PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/payload/BetterLumaPayload
)
target_link_libraries(BetterLumaPayload PRIVATE detours psapi)
target_compile_definitions(BetterLumaPayload PRIVATE
    $<$<CONFIG:Debug>:BETTERLUMA_PAYLOAD_LOGGING_ENABLED>
)

add_library(dwmapi SHARED
    proxy/LcDwmProxy.cpp
)
set_target_properties(dwmapi PROPERTIES PREFIX "" OUTPUT_NAME "dwmapi")

# ---------------------------------------------------------------------------
# xinput1_4.dll hijack - secondary loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(xinput1_4 SHARED
    proxy/LcXInputProxy.cpp
    proxy/LcXInputProxy.def
)
set_target_properties(xinput1_4 PROPERTIES PREFIX "" OUTPUT_NAME "xinput1_4")

# ---------------------------------------------------------------------------
# MSVC Compiler & Linker Configuration
# ---------------------------------------------------------------------------
if(MSVC)
    set(LC_COMPILE_OPTIONS
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
        /guard:cf-
        /GF
        /Gy
        /Gw
    )

    set(LC_COMMON_LINK_OPTIONS
        "/PDBALTPATH:%_PDB%"
        "/Brepro"
        "/DYNAMICBASE"
        "/HIGHENTROPYVA"
        "/NXCOMPAT"
    )

    # Release-specific link options:
    set(LC_RELEASE_LINK_OPTIONS
        "/OPT:REF"
        "/OPT:NOICF"
    )

    foreach(_tgt IN ITEMS BetterLuma BetterLumaPayload dwmapi xinput1_4)
        target_compile_options(${_tgt} PRIVATE ${LC_COMPILE_OPTIONS})
        target_link_options(${_tgt} PRIVATE
            ${LC_COMMON_LINK_OPTIONS}
            $<$<CONFIG:Release>:${LC_RELEASE_LINK_OPTIONS}>
        )
    endforeach()
endif()
