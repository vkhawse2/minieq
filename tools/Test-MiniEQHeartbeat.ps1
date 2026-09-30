# Test-MiniEQHeartbeat.ps1 -- MiniEQ APO heartbeat diagnostic.
#
# Read-only: opens each render endpoint's MiniEQ status channel
# (Global\MiniEQ_Status_<sanitized-endpoint-id>) and samples the APOProcess
# call counter twice, ~1.5 s apart.
#
#   counter advancing  -> the APO is REALLY processing audio on that device
#   channel present but counter stale -> APO locked for the device but starved
#   channel absent     -> the APO never locked for that device (not attached,
#                         enhancements off, exclusive-mode holder, ...)
#
# Works without elevation: the APO creates the channel with a DACL that
# grants Everyone read access.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File Test-MiniEQHeartbeat.ps1

$ErrorActionPreference = 'SilentlyContinue'

$FriendlyName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
$RenderRoot   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MiniEQMap {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr OpenFileMappingW(uint dwDesiredAccess, bool bInheritHandle, string lpName);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr MapViewOfFile(IntPtr hFileMappingObject, uint dwDesiredAccess,
        uint dwFileOffsetHigh, uint dwFileOffsetLow, UIntPtr dwNumberOfBytesToMap);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool UnmapViewOfFile(IntPtr lpBaseAddress);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool CloseHandle(IntPtr hObject);
}
"@

$FILE_MAP_READ = 4

function Get-MiniEQStatus([string]$statusName) {
    $h = [MiniEQMap]::OpenFileMappingW($FILE_MAP_READ, $false, $statusName)
    if ($h -eq [IntPtr]::Zero) { return $null }
    $v = [MiniEQMap]::MapViewOfFile($h, $FILE_MAP_READ, 0, 0, [UIntPtr]64)
    if ($v -eq [IntPtr]::Zero) { [MiniEQMap]::CloseHandle($h) | Out-Null; return $null }
    # Layout of MiniEQApoStatus (shared/settings_channel.h):
    #   +0  uint32 structSize | +4 uint32 version | +8 int64 processCalls
    #   +32 int32 locked | +36 int32 channels | +40 int32 sampleRate | +44 int32 initOk
    $st = [pscustomobject]@{
        ProcessCalls = [Runtime.InteropServices.Marshal]::ReadInt64($v, 8)
        Locked       = [Runtime.InteropServices.Marshal]::ReadInt32($v, 32)
        Channels     = [Runtime.InteropServices.Marshal]::ReadInt32($v, 36)
        SampleRate   = [Runtime.InteropServices.Marshal]::ReadInt32($v, 40)
        InitOk       = [Runtime.InteropServices.Marshal]::ReadInt32($v, 44)
    }
    [MiniEQMap]::UnmapViewOfFile($v) | Out-Null
    [MiniEQMap]::CloseHandle($h) | Out-Null
    return $st
}

Write-Host ''
Write-Host '=== MiniEQ heartbeat diagnostic ===' -ForegroundColor Cyan
if (-not [Environment]::Is64BitProcess) {
    Write-Host 'WARNING: 32-bit PowerShell. Offsets assume the 64-bit layout; re-run with 64-bit PowerShell.' -ForegroundColor Yellow
}
Write-Host ''

$renderKeys = Get-ChildItem -Path $RenderRoot -ErrorAction SilentlyContinue
if (-not $renderKeys) {
    Write-Host 'Could not enumerate render endpoints (need admin to read HKLM).' -ForegroundColor Red
    exit 1
}

$probes = @()
foreach ($k in $renderKeys) {
    $guid = $k.PSChildName
    if ($guid -like '*.*') { continue }  # not a bare endpoint GUID
    $props = Get-ItemProperty -Path ($k.PSPath + '\Properties') -ErrorAction SilentlyContinue
    $name = $props.$FriendlyName
    if (-not $name) { $name = '(no friendly name)' }
    # Same derivation as MiniEQ_StatusNameForEndpoint: "Global\MiniEQ_Status_"
    # + the full device id "{0.0.0.00000000}.<guid>" with [^0-9A-Za-z] -> "_".
    $devId = '{0.0.0.00000000}.' + $guid
    $san = ($devId -replace '[^0-9A-Za-z]', '_')
    $statusName = 'Global\MiniEQ_Status_' + $san
    $first = Get-MiniEQStatus $statusName
    $probes += [pscustomobject]@{ Name = $name; StatusName = $statusName; First = $first }
}

Start-Sleep -Milliseconds 1500

foreach ($p in $probes) {
    Write-Host $p.Name -ForegroundColor White
    if ($null -eq $p.First) {
        Write-Host '  status channel: ABSENT -- APO never locked for this device' -ForegroundColor DarkGray
        continue
    }
    $second = Get-MiniEQStatus $p.StatusName
    if ($null -eq $second) {
        Write-Host '  status channel vanished between samples (device re-enumerated?)' -ForegroundColor Yellow
        continue
    }
    $c1 = $p.First.ProcessCalls
    $c2 = $second.ProcessCalls
    $info = "  locked=$($p.First.Locked) initOk=$($p.First.InitOk) ch=$($p.First.Channels) rate=$($p.First.SampleRate) calls: $c1 -> $c2"
    if ($c2 -gt $c1) {
        Write-Host ($info + '  LIVE -- APO is processing audio') -ForegroundColor Green
    } elseif ($c2 -eq $c1 -and $c1 -gt 0) {
        Write-Host ($info + '  STALE -- APO locked before, but no buffers right now') -ForegroundColor Yellow
    } else {
        Write-Host ($info + '  QUIET -- channel exists, APOProcess never called') -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Cyan
