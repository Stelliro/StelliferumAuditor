@echo off
REM ============================================================================
REM STELLIFERUM AUDITOR - Build Script for Windows
REM ============================================================================
REM
REM This script automates the CMake build process for Windows systems.
REM It handles dependency detection, configuration, and compilation.
REM
REM Usage:
REM   build.bat              - Build release version
REM   build.bat debug        - Build debug version
REM   build.bat clean        - Clean build artifacts
REM   build.bat rebuild      - Clean and rebuild
REM
REM =======================7=====================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Color codes for output
set "GREEN=[92m"
set "YELLOW=[93m"
set "RED=[91m"
set "RESET=[0m"

REM Configuration
set "BUILD_DIR=build"
set "BUILD_TYPE=Release"
set "GENERATOR=Visual Studio 17 2022"
set "VERBOSE="

REM Kill any running instance before building
taskkill /F /IM StelliferumAuditor.exe >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo %YELLOW%[*] Killed running StelliferumAuditor.exe%RESET%
    timeout /t 1 /nobreak >nul
)

REM Parse command line arguments
if "%1"=="debug" (
    set "BUILD_TYPE=Debug"
    echo %YELLOW%[*] Building in DEBUG mode%RESET%
)
if "%1"=="clean" (
    echo %YELLOW%[*] Cleaning build artifacts...%RESET%
    if exist "%BUILD_DIR%" (
        rmdir /s /q "%BUILD_DIR%"
        echo %GREEN%[✓] Build directory removed%RESET%
    )
    exit /b 0
)
if "%1"=="rebuild" (
    echo %YELLOW%[*] Performing clean rebuild...%RESET%
    if exist "%BUILD_DIR%" (
        rmdir /s /q "%BUILD_DIR%"
    )
)
if "%1"=="verbose" (
    set "VERBOSE=--verbose"
)

REM Check for required tools
echo %YELLOW%[*] Checking for required tools...%RESET%

where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo %RED%[!] CMake not found in PATH%RESET%
    echo %YELLOW%[*] Please install CMake from https://cmake.org%RESET%
    exit /b 1
)
for /f "tokens=*" %%i in ('cmake --version ^| findstr /R "cmake version"') do (
    echo %GREEN%[✓] Found CMake: %%i%RESET%
)

REM Check for MSBuild (Visual Studio)
set "MSBUILD_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
set "VCVARS_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS_PATH%" (
    echo %YELLOW%[*] Setting up Visual Studio environment...%RESET%
    call "%VCVARS_PATH%" x64 >nul 2>&1
)
if exist "%MSBUILD_PATH%" (
    echo %GREEN%[✓] MSBuild found%RESET%
    set "PATH=%PATH%;C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin"
) else (
    where msbuild >nul 2>&1
    if %ERRORLEVEL% NEQ 0 (
        echo %RED%[!] MSBuild not found - Visual Studio 2022 required%RESET%
        exit /b 1
    )
    echo %GREEN%[✓] MSBuild found in PATH%RESET%
)

REM Check for Git (needed for raylib fetch)
where git >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo %RED%[!] Git not found - required for fetching dependencies%RESET%
    exit /b 1
)
echo %GREEN%[✓] Git found%RESET%

REM Create build directory
if not exist "%BUILD_DIR%" (
    echo %YELLOW%[*] Creating build directory...%RESET%
    mkdir "%BUILD_DIR%"
)

REM Configure project
echo %YELLOW%[*] Configuring project with CMake...%RESET%
cd "%BUILD_DIR%"
cmake .. -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -Wno-dev %VERBOSE%
if %ERRORLEVEL% NEQ 0 (
    echo %RED%[!] CMake configuration failed%RESET%
    cd ..
    exit /b 1
)
echo %GREEN%[✓] Configuration complete%RESET%

REM Build project
echo %YELLOW%[*] Building project (%BUILD_TYPE%)...%RESET%
cmake --build . --config %BUILD_TYPE% %VERBOSE% --parallel %NUMBER_OF_PROCESSORS%
if %ERRORLEVEL% NEQ 0 (
    echo %RED%[!] Build failed%RESET%
    cd ..
    exit /b 1
)
echo %GREEN%[✓] Build successful%RESET%

cd ..

REM Display output
echo.
echo %GREEN%=============================================%RESET%
echo %GREEN%     BUILD COMPLETE%RESET%
echo %GREEN%=============================================%RESET%
echo %YELLOW%[*] Build Type: %BUILD_TYPE%%RESET%
echo %YELLOW%[*] Output Binary: %BUILD_DIR%\bin\StelliferumAuditor.exe%RESET%
echo.
echo %YELLOW%[*] Next steps:%RESET%
echo    1. Run: %BUILD_DIR%\bin\StelliferumAuditor.exe
echo    2. Or install with: cmake --install build --config %BUILD_TYPE%
echo.

exit /b 0
