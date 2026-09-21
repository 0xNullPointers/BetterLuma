@echo off
setlocal EnableDelayedExpansion

set "SOURCE_DIR=%~dp0source"
set "BUILD_DIR=%~dp0build"
set "BUILD_DIR_X86=%~dp0build_x86"
set "OUT_DIR=%~dp0Releases"
set "LOG_FILE=%~dp0build_log.txt"
> "%LOG_FILE%" echo BetterLuma build started %DATE% %TIME%

:: --- Argument parsing ----------------------------------------------------
:: --no-pause      skip the trailing 'pause'
:: --debug-only    build Debug only
:: --release-only  build Release only
:: --clean         remove build dir before building
set "NO_PAUSE=0"
set "BUILD_RELEASE=1"
set "BUILD_DEBUG=1"
set "DO_CLEAN=0"
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-pause"     ( set "NO_PAUSE=1"      & shift & goto parse_args )
if /I "%~1"=="--debug-only"   ( set "BUILD_RELEASE=0" & shift & goto parse_args )
if /I "%~1"=="--release-only" ( set "BUILD_DEBUG=0"   & shift & goto parse_args )
if /I "%~1"=="--clean"        ( set "DO_CLEAN=1"      & shift & goto parse_args )
echo [WARN] Unknown argument: %~1
shift
goto parse_args
:args_done

echo.
echo ============================================================
echo  BetterLuma Build
echo  Source  : %SOURCE_DIR%
echo  Build x64 : %BUILD_DIR%
echo  Build x86 : %BUILD_DIR_X86%
echo  Output  : %OUT_DIR%
echo  Release : %BUILD_RELEASE%   Debug: %BUILD_DEBUG%
if "%DO_CLEAN%"=="1" ( echo  Clean  : YES ) else ( echo  Clean  : NO incremental )
echo ============================================================
echo.

:: --- Clean build only when --clean is passed -------------------------------
if "%DO_CLEAN%"=="1" (
    if exist "%BUILD_DIR%\NUL" (
        echo [STEP] Cleaning build directory...
        >> "%LOG_FILE%" echo.
        >> "%LOG_FILE%" echo [STEP] Cleaning build directory...
        rmdir /S /Q "%BUILD_DIR%" >> "%LOG_FILE%" 2>&1
        if exist "%BUILD_DIR%\NUL" (
            echo [WARN] First clean attempt failed, using PowerShell fallback...
            >> "%LOG_FILE%" echo [WARN] First clean attempt failed, using PowerShell fallback...
            powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=[IO.Path]::GetFullPath('%BUILD_DIR%'); $root=[IO.Path]::GetFullPath('%~dp0'); if(-not $p.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing to delete outside BetterLuma folder'}; for($i=1; $i -le 5 -and (Test-Path -LiteralPath $p); $i++){ try { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction Stop } catch { Write-Host ('delete attempt '+$i+' failed: '+$_.Exception.Message); Start-Sleep -Milliseconds 500 } }; if(Test-Path -LiteralPath $p){ exit 1 }" >> "%LOG_FILE%" 2>&1
            if exist "%BUILD_DIR%\NUL" (
                echo [ERROR] Failed to delete %BUILD_DIR% [file in use]
                echo [ERROR] See %LOG_FILE% for details.
                if "%NO_PAUSE%"=="0" pause
                exit /b 1
            )
        )
    )
    if exist "%BUILD_DIR_X86%\NUL" (
        rmdir /S /Q "%BUILD_DIR_X86%" >> "%LOG_FILE%" 2>&1
    )
) else (
    echo [INFO] Incremental build. Pass --clean for full clean.
)

:: --- Locate cmake ---------------------------------------------------------
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if !errorlevel! neq 0 (
    set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
    if not exist "!CMAKE_EXE!" (
        echo [ERROR] cmake not found in PATH or Visual Studio 2022 installations.
        if "%NO_PAUSE%"=="0" pause
        exit /b 1
    )
    echo [INFO] Using cmake from Visual Studio: !CMAKE_EXE!
)

:: --- Pick generator -------------------------------------------------------
set "GENERATOR=Visual Studio 17 2022"
set "GEN_ARGS=-A x64"
set "GEN_ARGS_X86=-A Win32"
where ninja >nul 2>&1
if !errorlevel! == 0 (
    where cl >nul 2>&1
    if !errorlevel! == 0 (
        set "GENERATOR=Ninja Multi-Config"
        set "GEN_ARGS="
        set "GEN_ARGS_X86="
        echo [INFO] Using Ninja Multi-Config generator
    ) else (
        echo [INFO] Ninja found but cl.exe not in PATH, using Visual Studio 17 2022 generator
    )
) else (
    echo [INFO] Using Visual Studio 17 2022 generator
)

:: --- Configure ------------------------------------------------------------
echo [STEP] Configuring x64...
>> "%LOG_FILE%" echo.
>> "%LOG_FILE%" echo [STEP] Configuring x64...
mkdir "%BUILD_DIR%" 2>nul
"!CMAKE_EXE!" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "!GENERATOR!" !GEN_ARGS! >> "%LOG_FILE%" 2>&1
if !errorlevel! neq 0 (
    echo [ERROR] Configure x64 failed.
    type "%LOG_FILE%"
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo [STEP] Configuring x86 Payload32...
>> "%LOG_FILE%" echo.
>> "%LOG_FILE%" echo [STEP] Configuring x86 Payload32...
mkdir "%BUILD_DIR_X86%" 2>nul
"!CMAKE_EXE!" -S "%SOURCE_DIR%" -B "%BUILD_DIR_X86%" -G "!GENERATOR!" !GEN_ARGS_X86! -DBETTERLUMA_PAYLOAD_ONLY=ON >> "%LOG_FILE%" 2>&1
if !errorlevel! neq 0 (
    echo [ERROR] Configure x86 failed.
    type "%LOG_FILE%"
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

:: --- Build ----------------------------------------------------------------
set "BUILD_FAILED=0"

if "%BUILD_RELEASE%"=="1" (
    echo.
    echo [STEP] Building Release x64...
    >> "%LOG_FILE%" echo.
    >> "%LOG_FILE%" echo [STEP] Building Release x64...
    "!CMAKE_EXE!" --build "%BUILD_DIR%" --config Release --parallel >> "%LOG_FILE%" 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] Release x64 build failed.
        set "BUILD_FAILED=1"
    )

    echo [STEP] Building Release x86 Payload...
    >> "%LOG_FILE%" echo.
    >> "%LOG_FILE%" echo [STEP] Building Release x86 Payload...
    "!CMAKE_EXE!" --build "%BUILD_DIR_X86%" --config Release --parallel >> "%LOG_FILE%" 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] Release x86 build failed.
        set "BUILD_FAILED=1"
    )
)

if "%BUILD_DEBUG%"=="1" (
    echo.
    echo [STEP] Building Debug x64...
    >> "%LOG_FILE%" echo.
    >> "%LOG_FILE%" echo [STEP] Building Debug x64...
    "!CMAKE_EXE!" --build "%BUILD_DIR%" --config Debug --parallel >> "%LOG_FILE%" 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] Debug x64 build failed.
        set "BUILD_FAILED=1"
    )

    echo [STEP] Building Debug x86 Payload...
    >> "%LOG_FILE%" echo.
    >> "%LOG_FILE%" echo [STEP] Building Debug x86 Payload...
    "!CMAKE_EXE!" --build "%BUILD_DIR_X86%" --config Debug --parallel >> "%LOG_FILE%" 2>&1
    if !errorlevel! neq 0 (
        echo [WARN] Debug x86 build failed.
        set "BUILD_FAILED=1"
    )
)

:: --- Copy DLLs to Releases ------------------------------------------------
echo.
echo [STEP] Copying DLLs to %OUT_DIR%...

if "%BUILD_RELEASE%"=="1" (
    if exist "%BUILD_DIR%\Release\BetterLuma.dll" (
        mkdir "%OUT_DIR%\Release" 2>nul
        copy /Y "%BUILD_DIR%\Release\BetterLuma.dll" "%OUT_DIR%\Release\" >nul
        if exist "%BUILD_DIR%\Release\dwmapi.dll" (
            copy /Y "%BUILD_DIR%\Release\dwmapi.dll" "%OUT_DIR%\Release\" >nul
        )
        if exist "%BUILD_DIR%\Release\xinput1_4.dll" (
            copy /Y "%BUILD_DIR%\Release\xinput1_4.dll" "%OUT_DIR%\Release\" >nul
        )
        if exist "%BUILD_DIR%\Release\BetterLumaPayload.dll" (
            copy /Y "%BUILD_DIR%\Release\BetterLumaPayload.dll" "%OUT_DIR%\Release\" >nul
        )
        if exist "%BUILD_DIR_X86%\Release\BetterLumaPayload32.dll" (
            copy /Y "%BUILD_DIR_X86%\Release\BetterLumaPayload32.dll" "%OUT_DIR%\Release\" >nul
        )
        echo [OK] Release DLLs copied to %OUT_DIR%\Release
    ) else (
        echo [SKIP] Release BetterLuma.dll not produced.
    )
)

if "%BUILD_DEBUG%"=="1" (
    if exist "%BUILD_DIR%\Debug\BetterLuma.dll" (
        mkdir "%OUT_DIR%\Debug" 2>nul
        copy /Y "%BUILD_DIR%\Debug\BetterLuma.dll" "%OUT_DIR%\Debug\" >nul
        if exist "%BUILD_DIR%\Debug\dwmapi.dll" (
            copy /Y "%BUILD_DIR%\Debug\dwmapi.dll" "%OUT_DIR%\Debug\" >nul
        )
        if exist "%BUILD_DIR%\Debug\xinput1_4.dll" (
            copy /Y "%BUILD_DIR%\Debug\xinput1_4.dll" "%OUT_DIR%\Debug\" >nul
        )
        if exist "%BUILD_DIR%\Debug\BetterLumaPayload.dll" (
            copy /Y "%BUILD_DIR%\Debug\BetterLumaPayload.dll" "%OUT_DIR%\Debug\" >nul
        )
        if exist "%BUILD_DIR_X86%\Debug\BetterLumaPayload32.dll" (
            copy /Y "%BUILD_DIR_X86%\Debug\BetterLumaPayload32.dll" "%OUT_DIR%\Debug\" >nul
        )
        echo [OK] Debug DLLs copied to %OUT_DIR%\Debug
    ) else (
        echo [SKIP] Debug BetterLuma.dll not produced.
    )
)

echo.
echo ============================================================
echo  Done. DLLs are in:
if "%BUILD_RELEASE%"=="1" echo    %OUT_DIR%\Release
if "%BUILD_DEBUG%"=="1"   echo    %OUT_DIR%\Debug
echo ============================================================
echo.
>> "%LOG_FILE%" echo.
>> "%LOG_FILE%" echo Done. Build failed flag: %BUILD_FAILED%

if "%NO_PAUSE%"=="0" pause
endlocal
exit /b %BUILD_FAILED%
