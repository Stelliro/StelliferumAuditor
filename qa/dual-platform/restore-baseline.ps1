#Requires -Version 5.1
<#
.SYNOPSIS
  Upload qa/dual-platform/baseline/* (6 economy/trader files) to live FTP via native CLI.

.DESCRIPTION
  Uses ProcessStartInfo.ArgumentList (no path-space splitting). Credentials only from config/ftp.ini.
  Does not rename sfa verbs or change FTP ABI — wraps --ftp-upload only.

.PARAMETER Tag
  Log name suffix: qa/dual-platform/restore-<Tag>.log

.PARAMETER SkipVerify
  Skip optional types.xml re-download size check.

.EXAMPLE
  .\qa\dual-platform\restore-baseline.ps1 -Tag pre-alpine
#>
param(
    [Parameter(Mandatory = $false)]
    [string]$Tag = "manual",

    [switch]$SkipVerify
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path (Join-Path $Root "config\server_paths.ini"))) {
    $Root = (Get-Location).Path
}
Set-Location $Root

$Exe = Join-Path $Root "build\bin\Release\StelliferumAuditor.exe"
if (-not (Test-Path $Exe)) {
    $Exe = Join-Path $Root "build\bin\StelliferumAuditor.exe"
}
if (-not (Test-Path $Exe)) {
    Write-Error "StelliferumAuditor.exe not found under build/bin"
    exit 2
}

$RemoteRoot = "/104.192.226.196_2322"
$Pairs = @(
    @{ Local = "qa/dual-platform/baseline/types.xml";                  Remote = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/db/types.xml" }
    @{ Local = "qa/dual-platform/baseline/cfgspawnabletypes.xml";      Remote = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/cfgspawnabletypes.xml" }
    @{ Local = "qa/dual-platform/baseline/cfgeconomycore.xml";         Remote = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/cfgeconomycore.xml" }
    @{ Local = "qa/dual-platform/baseline/cfglimitsdefinitionuser.xml"; Remote = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/cfglimitsdefinitionuser.xml" }
    @{ Local = "qa/dual-platform/baseline/cfgrandompresets.xml";       Remote = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/cfgrandompresets.xml" }
    @{ Local = "qa/dual-platform/baseline/TraderConfig.txt";           Remote = "$RemoteRoot/profiles/Trader/TraderConfig.txt" }
)

$LogDir = Join-Path $Root "qa\dual-platform"
$LogPath = Join-Path $LogDir ("restore-{0}.log" -f $Tag)
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add(("=== restore-baseline tag={0} when={1} ===" -f $Tag, (Get-Date -Format o)))
$lines.Add(("exe={0}" -f $Exe))
$lines.Add("cwd=$Root")
$lines.Add("note=credentials from config/ftp.ini only; no secrets logged")

function Invoke-AuditorArgs {
    param([string[]]$Argv, [int]$TimeoutSec = 180)
    # Prefer ProcessStartInfo.ArgumentList when available (.NET Core / PS 6+).
    # On Windows PowerShell 5.1 (.NET Framework) ArgumentList is null — fall back to
    # carefully quoted Arguments string, or call-operator array splat (no path split
    # when paths are relative and space-free as used here).
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $Root
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $usedArgList = $false
    try {
        if ($null -ne $psi.ArgumentList) {
            foreach ($a in $Argv) { [void]$psi.ArgumentList.Add([string]$a) }
            $usedArgList = $true
        }
    } catch {
        $usedArgList = $false
    }
    if (-not $usedArgList) {
        # Quote any arg with whitespace or quotes; relative --local paths stay space-free.
        $quoted = foreach ($a in $Argv) {
            if ($a -match '[\s"]') {
                '"' + ($a -replace '(\\*)"', '$1$1\"') + '"'
            } else {
                $a
            }
        }
        $psi.Arguments = ($quoted -join ' ')
    }
    $p = [System.Diagnostics.Process]::Start($psi)
    if (-not $p) {
        return [pscustomobject]@{ Exit = -2; Out = "failed to start process" }
    }
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        try { $p.Kill() } catch {}
        return [pscustomobject]@{ Exit = -1; Out = "TIMEOUT after ${TimeoutSec}s" }
    }
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    return [pscustomobject]@{ Exit = $p.ExitCode; Out = ($stdout + $stderr) }
}

$fail = 0
foreach ($pair in $Pairs) {
    $localRel = $pair.Local
    $remote = $pair.Remote
    $localFs = Join-Path $Root ($localRel -replace '/', '\')
    if (-not (Test-Path $localFs)) {
        $lines.Add("ec=2 size=0 local=$localRel remote=$remote ERROR=missing_local")
        $fail++
        continue
    }
    $size = (Get-Item $localFs).Length
    if ($size -le 0) {
        $lines.Add("ec=2 size=0 local=$localRel remote=$remote ERROR=zero_byte_local")
        $fail++
        continue
    }
    $argv = @("--ftp-upload", "--local", $localRel, "--remote", $remote)
    $r = Invoke-AuditorArgs -Argv $argv -TimeoutSec 180
    $lines.Add(("ec={0} size={1} local={2} remote={3}" -f $r.Exit, $size, $localRel, $remote))
    if ($r.Exit -ne 0) { $fail++ }
}

if (-not $SkipVerify) {
    $verifyRel = "qa/dual-platform/_restore-verify-types.xml"
    $verifyFs = Join-Path $Root ($verifyRel -replace '/', '\')
    $baselineTypes = Join-Path $Root "qa\dual-platform\baseline\types.xml"
    $remoteTypes = "$RemoteRoot/mpmissions/dayzOffline.chernarusplus/db/types.xml"
    if (Test-Path $verifyFs) { Remove-Item $verifyFs -Force -ErrorAction SilentlyContinue }
    $dl = Invoke-AuditorArgs -Argv @("--ftp-download", "--remote", $remoteTypes, "--local", $verifyRel) -TimeoutSec 180
    $baseSz = if (Test-Path $baselineTypes) { (Get-Item $baselineTypes).Length } else { 0 }
    $gotSz = if (Test-Path $verifyFs) { (Get-Item $verifyFs).Length } else { 0 }
    $match = ($baseSz -gt 0 -and $baseSz -eq $gotSz)
    $lines.Add(("verify_types ec={0} baseline_size={1} remote_size={2} size_match={3}" -f $dl.Exit, $baseSz, $gotSz, $match))
    if (Test-Path $verifyFs) { Remove-Item $verifyFs -Force -ErrorAction SilentlyContinue }
    if ($dl.Exit -ne 0 -or -not $match) { $fail++ }
}

$lines.Add(("ALL_OK={0} fail_count={1}" -f ($fail -eq 0), $fail))
$lines | Set-Content -Path $LogPath -Encoding utf8
Write-Host ("Wrote {0}" -f $LogPath)
$lines | ForEach-Object { Write-Host $_ }
exit $(if ($fail -eq 0) { 0 } else { 1 })
