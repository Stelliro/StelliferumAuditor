@echo off
setlocal EnableExtensions
REM ============================================================================
REM  sfa — Stelliferum Auditor simple terminal launcher (Windows)
REM  Finds a prebuilt binary so you do NOT need a full path or rebuild.
REM ============================================================================
REM  Usage:
REM    sfa              → interactive terminal
REM    sfa help
REM    sfa pull
REM    sfa pipeline
REM    sfa push
REM    sfa run cycle
REM ============================================================================

set "ROOT=%~dp0"
set "EXE="

REM 1) Same folder as this script (release zip / copy)
if exist "%ROOT%StelliferumAuditor.exe" set "EXE=%ROOT%StelliferumAuditor.exe"

REM 2) Standard Windows Release build output
if not defined EXE if exist "%ROOT%build\bin\Release\StelliferumAuditor.exe" set "EXE=%ROOT%build\bin\Release\StelliferumAuditor.exe"
if not defined EXE if exist "%ROOT%build\bin\StelliferumAuditor.exe" set "EXE=%ROOT%build\bin\StelliferumAuditor.exe"

if not defined EXE (
  echo [sfa] No StelliferumAuditor binary found.
  echo        Put StelliferumAuditor.exe next to sfa.cmd, or build once:
  echo          build.bat
  echo        Expected:
  echo          %ROOT%StelliferumAuditor.exe
  echo          %ROOT%build\bin\Release\StelliferumAuditor.exe
  exit /b 1
)

REM Always run from project root so config/ is found
pushd "%ROOT%" >nul

if "%~1"=="" (
  "%EXE%" shell
) else (
  "%EXE%" %*
)

set "ERR=%ERRORLEVEL%"
popd >nul
exit /b %ERR%
