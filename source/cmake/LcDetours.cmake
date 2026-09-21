# Defines a static `detours` target by fetching the upstream Microsoft/Detours
# sources. Idempotent - safe to include() from multiple CMakeLists.
if(TARGET detours)
    return()
endif()

include(LcFetchCache)
include(FetchContent)

FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    # SOURCE_SUBDIR points at a directory with no CMakeLists.txt so that
    # FetchContent_MakeAvailable downloads the source but does NOT add the
    # repo's own (Make-based) build into our project.
    SOURCE_SUBDIR  cmake-noop
)
FetchContent_MakeAvailable(detours)

# Pick the architecture-specific disassembler source.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|aarch64")
        set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolarm64.cpp")
    else()
        set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolx64.cpp")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM|aarch64")
        set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolarm.cpp")
    else()
        set(DETOURS_ARCH_SRC "${detours_SOURCE_DIR}/src/disolx86.cpp")
    endif()
endif()

# Generate patched creatwth.cpp to enable 64-bit -> 32-bit cross-bitness injection
set(DETOURS_CREATWTH_PATCHED "${CMAKE_CURRENT_BINARY_DIR}/detours_creatwth.cpp")
file(READ "${detours_SOURCE_DIR}/src/creatwth.cpp" _creatwth_content)
string(REPLACE "\r\n" "\n" _creatwth_content "${_creatwth_content}")

set(_t1 "#if DETOURS_32BIT\n#define DWORD_XX                        DWORD32")
set(_r1 "#if 1\n#define DWORD_XX                        DWORD32")
string(REPLACE "${_t1}" "${_r1}" _creatwth_content "${_creatwth_content}")

set(_t2 "#elif defined(DETOURS_64BIT)\n    if (bIs32BitProcess || bIs32BitExe) {\n        // Can't detour a 32-bit process with 64-bit code.\n        SetLastError(ERROR_INVALID_HANDLE);\n        return FALSE;\n    }")
set(_r2 "#elif defined(DETOURS_64BIT)\n    if (bIs32BitProcess || bIs32BitExe) {\n        if (!UpdateImports32(hProcess, hModule, rlpDlls, nDlls)) {\n            return FALSE;\n        }\n    }")
string(REPLACE "${_t2}" "${_r2}" _creatwth_content "${_creatwth_content}")

if(NOT _creatwth_content MATCHES "UpdateImports32\\(hProcess")
    message(FATAL_ERROR "Detours patch failed: upstream creatwth.cpp signature changed.")
endif()

file(WRITE "${DETOURS_CREATWTH_PATCHED}" "${_creatwth_content}")

add_library(detours STATIC
    ${detours_SOURCE_DIR}/src/detours.cpp
    ${detours_SOURCE_DIR}/src/modules.cpp
    ${detours_SOURCE_DIR}/src/disasm.cpp
    ${detours_SOURCE_DIR}/src/image.cpp
    ${DETOURS_CREATWTH_PATCHED}
    ${DETOURS_ARCH_SRC}
)
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
set_target_properties(detours PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(detours PRIVATE /w)
else()
    target_compile_options(detours PRIVATE -w)
endif()
