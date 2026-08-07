# ============================================================================
# STELLIFERUM AUDITOR - PowerShell Build Script
# ============================================================================
# 
# Cross-platform build helper for CMake projects
# 
# Usage:
#   .\build.ps1                      - Build release version
#   .\build.ps1 -Config Debug        - Build debug version
#   .\build.ps1 -Clean               - Clean build artifacts
#   .\build.ps1 -Rebuild             - Clean and rebuild
#   .\build.ps1 -Verbose             - Show detailed build output
#
# ============================================================================

param (
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    
    [switch]$Clean,
    [switch]$Rebuild,
    [switch]$Verbose,
    [switch]$Install,
    [switch]$Help
)

# Constants
$BuildDir = "build"
$ProjectName = "StelliferumAuditor"

# Color output
$colors = @{
    success = 10
    warning = 14
    error   = 12
    info    = 11
}

function Write-colored {
    param (
        [string]$Text,
        [int]$Color
    )
    Write-Host $Text -ForegroundColor $Color
}

function Show-Help {
    Write-Host @"
╔════════════════════════════════════════════════════════════╗
║  STELLIFERUM AUDITOR - Build Script                         ║
╚════════════════════════════════════════════════════════════╝

Usage:
  .\build.ps1 [-Config {Debug|Release}] [-Clean] [-Rebuild] [-Verbose]

Options:
  -Config <Debug|Release>   Build configuration (default: Release)
  -Clean                    Remove build artifacts
  -Rebuild                  Clean and rebuild
  -Verbose                  Show detailed output
  -Install                  Install after build
  -Help                     Show this message

Examples:
  .\build.ps1
  .\build.ps1 -Config Debug
  .\build.ps1 -Rebuild -Verbose
  .\build.ps1 -Config Release -Install
"@
}

if ($Help) {
    Show-Help
    exit 0
}

# Check tools
Write-colored "`n[*] Checking environment..." $colors.info

$toolsFound = $true

# Check CMake
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-colored "[!] CMake not found" $colors.error
    Write-colored "    Install from: https://cmake.org" $colors.warning
    $toolsFound = $false
} else {
    $cmakeVer = cmake --version | Select-Object -First 1
    Write-colored "[✓] $cmakeVer" $colors.success
}

# Check Git (for dependencies)
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-colored "[!] Git not found (required for fetching raylib)" $colors.error
    $toolsFound = $false
} else {
    Write-colored "[✓] Git found" $colors.success
}

# Check Visual Studio on Windows
if ($IsWindows -or $PSVersionTable.Platform -eq "Win32NT") {
    if (-not (Get-Command devenv -ErrorAction SilentlyContinue)) {
        Write-colored "[!] Visual Studio development environment not found" $colors.error
        $toolsFound = $false
    } else {
        Write-colored "[✓] Visual Studio found" $colors.success
    }
}

if (-not $toolsFound) {
    Write-colored "[!] Missing required tools. Exiting." $colors.error
    exit 1
}

# Clean phase
if ($Clean -or $Rebuild) {
    Write-colored "`n[*] Cleaning build artifacts..." $colors.info
    if (Test-Path $BuildDir) {
        Remove-Item -Path $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-colored "[✓] Build directory removed" $colors.success
    }
    if ($Clean) {
        exit 0
    }
}

# Build phase
Write-colored "`n[*] Creating build directory..." $colors.info
$null = New-Item -Path $BuildDir -ItemType Directory -Force

Write-colored "[*] Configuring CMake ($Config)..." $colors.info
Push-Location $BuildDir

$generator = "Visual Studio 17 2022"
if ($PSVersionTable.OS -like "*Linux*") {
    $generator = "Unix Makefiles"
}
elseif ($PSVersionTable.OS -like "*Darwin*") {
    $generator = "Xcode"
}

$configCmd = @("cmake", "..", "-G", $generator, "-DCMAKE_BUILD_TYPE=$Config")
if ($Verbose) { $configCmd += "--debug-output" }

& $configCmd[0] $configCmd[1..($configCmd.Count-1)]
if ($LASTEXITCODE -ne 0) {
    Write-colored "[!] CMake configuration failed" $colors.error
    Pop-Location
    exit 1
}
Write-colored "[✓] CMake configuration successful" $colors.success

Write-colored "`n[*] Building project..." $colors.info
$buildCmd = @("cmake", "--build", ".", "--config", $Config, "--parallel", [Environment]::ProcessorCount)
if ($Verbose) { $buildCmd += "--verbose" }

& $buildCmd[0] $buildCmd[1..($buildCmd.Count-1)]
if ($LASTEXITCODE -ne 0) {
    Write-colored "[!] Build failed" $colors.error
    Pop-Location
    exit 1
}
Write-colored "[✓] Build successful" $colors.success

# Install phase
if ($Install) {
    Write-colored "`n[*] Installing..." $colors.info
    cmake --install . --config $Config
    if ($LASTEXITCODE -eq 0) {
        Write-colored "[✓] Installation successful" $colors.success
    } else {
        Write-colored "[!] Installation failed" $colors.error
    }
}

Pop-Location

# Summary
Write-colored "`n╔════════════════════════════════════════════════════╗" $colors.success
Write-colored "║  BUILD COMPLETE                                      ║" $colors.success
Write-colored "╚════════════════════════════════════════════════════╝" $colors.success
Write-colored `n"Output: $BuildDir\bin\$ProjectName.exe" $colors.info
Write-colored "Config: $Config" $colors.info
Write-colored `n"Run: .\$BuildDir\bin\$ProjectName.exe`n" $colors.success

exit 0
