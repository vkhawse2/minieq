# Test-MiniEQLink.ps1 -- MiniEQ device-link diagnostic.
#
# Read-only by default: inspects the registry paths that connect a render
# device to the MiniEQ APO and reports exactly what is (or isn't) there.
#
# If run elevated, it also offers a self-cleaning WRITE test: it writes a
# temporary value into the device's FxProperties key, reads it back and
# deletes it again. Nothing is left behind either way.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File Test-MiniEQLink.ps1

$ErrorActionPreference = 'SilentlyContinue'

$MiniEqClsid   = '{5E52BF50-F229-46A0-8B7D-AA805D47FB60}'
$SfxSlot      = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5'
$FriendlyName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'
$RenderRoot   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$ApoRegPath   = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$MiniEqClsid"

function Is-Elevated {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Host ''
Write-Host '=== MiniEQ device-link diagnostic ===' -ForegroundColor Cyan
if (-not [Environment]::Is64BitProcess) {
    Write-Host 'WARNING: 32-bit PowerShell sees a redirected registry view. Re-run with 64-bit PowerShell.' -ForegroundColor Yellow
}
$elevated = Is-Elevated
Write-Host ("Elevated: " + $elevated)
Write-Host ''

# 0. Is the COM class registered? (This is what the audio engine actually
#    CoCreates when it builds the APO graph. A missing/wrong InprocServer32
#    entry means the DLL is never even loaded -- check this before anything
#    else when the trace log stays empty.)
Write-Host '--- 0. COM class (CLSID\InprocServer32) ---'
$ClsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$MiniEqClsid\InprocServer32"
$cls = Get-ItemProperty -Path $ClsidPath -ErrorAction SilentlyContinue
if ($cls) {
    $dllPath = $cls.'(default)'
    Write-Host ("FOUND  " + $ClsidPath) -ForegroundColor Green
    Write-Host ("  DLL path: " + $dllPath)
    if ($dllPath -and (Test-Path $dllPath)) {
        Write-Host '  DLL file exists' -ForegroundColor Green
    } else {
        Write-Host '  !! DLL file MISSING at the registered path' -ForegroundColor Red
    }
    $tm = $cls.ThreadingModel
    Write-Host ("  ThreadingModel: " + $tm)
    if ($tm -ne 'Both') {
        Write-Host '  !! expected ThreadingModel=Both' -ForegroundColor Yellow
    }
} else {
    Write-Host ("MISSING " + $ClsidPath) -ForegroundColor Red
    Write-Host '  The audio engine cannot CoCreate the APO at all.' -ForegroundColor Red
}
Write-Host ''

# 1. Is the APO itself registered with the audio engine?
Write-Host '--- 1. APO registration (AudioEngine\AudioProcessingObjects) ---'
$apo = Get-ItemProperty -Path $ApoRegPath -ErrorAction SilentlyContinue
if ($apo) {
    Write-Host ("FOUND  " + $ApoRegPath) -ForegroundColor Green
    Write-Host ("  FriendlyName: " + $apo.FriendlyName)
    Write-Host ("  Flags:        " + $apo.Flags + "  (MiniEQ expects 1 = APO_FLAG_INPLACE per Microsoft docs)")
} else {
    Write-Host ("MISSING " + $ApoRegPath) -ForegroundColor Red
    Write-Host '  The MSI did not register the APO. Reinstall MiniEQ-0.1.0-x64.msi.'
}
Write-Host ''

# 2. Walk every render endpoint.
Write-Host '--- 2. Render endpoints and their FxProperties ---'
$renderKeys = Get-ChildItem -Path $RenderRoot -ErrorAction SilentlyContinue
if (-not $renderKeys) {
    Write-Host 'Could not enumerate render endpoints (need admin to read HKLM).' -ForegroundColor Red
    exit 1
}
$devices = @()
$idx = 0
foreach ($k in $renderKeys) {
    $idx++
    $guid = $k.PSChildName
    $props = Get-ItemProperty -Path ($k.PSPath + '\Properties') -ErrorAction SilentlyContinue
    $name = $props.$FriendlyName
    if (-not $name) { $name = '(no friendly name)' }
    $fxPath = $k.PSPath + '\FxProperties'
    $fx = Get-ItemProperty -Path $fxPath -ErrorAction SilentlyContinue
    $sfx = $null
    $fxExists = (Test-Path $fxPath)
    if ($fxExists) { $sfx = $fx.$SfxSlot }

    $suspicious = $guid -like '*.*'  # e.g. "{0.0.0.00000000}.{guid}" -- wrong-path artifact
    $devices += [pscustomobject]@{
        Index = $idx; Guid = $guid; Name = $name
        FxExists = $fxExists; SfxSlot = $sfx; Suspicious = $suspicious
    }

    $tag = ''
    if ($suspicious) { $tag = '  <-- SUSPICIOUS: not a bare endpoint GUID (leftover of a bad attach?)' }
    Write-Host ("[$idx] $name") -ForegroundColor White
    Write-Host ("    key : $guid")
    if ($suspicious) { Write-Host $tag -ForegroundColor Yellow }
    Write-Host ("    FxProperties present: $fxExists")
    if ($fxExists) {
        if ($sfx) {
            $ours = ($sfx -eq $MiniEqClsid)
            $color = 'Green'; if (-not $ours) { $color = 'Yellow' }
            Write-Host ("    SFX slot (,5): $sfx" + $(if ($ours) { '  <-- MiniEQ is linked here' } else { '  <-- another APO owns this slot' })) -ForegroundColor $color
        } else {
            Write-Host '    SFX slot (,5): (empty -- MiniEQ not attached)' -ForegroundColor DarkGray
        }
    }
}
Write-Host ''

# 3. Optional live write test (elevated only).
if ($elevated -and $devices.Count -gt 0) {
    $answer = Read-Host 'Run the live write test? It writes a temporary value to one device and deletes it again (y/N)'
    if ($answer -match '^[Yy]') {
        $sel = Read-Host ("Device number to test [1-$($devices.Count)]")
        $dev = $devices | Where-Object { $_.Index -eq [int]$sel }
        if (-not $dev) { Write-Host 'Bad selection.' -ForegroundColor Red; exit 1 }
        if ($dev.Suspicious) {
            Write-Host 'That key name is not a real endpoint GUID; pick a proper device.' -ForegroundColor Red
            exit 1
        }
        $fxReg = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$($dev.Guid)\FxProperties"
        $testName = 'MiniEQ_LinkTest'
        $testVal  = 'connection-probe'
        try {
            if (-not (Test-Path $fxReg)) { New-Item -Path $fxReg -Force | Out-Null }
            New-ItemProperty -Path $fxReg -Name $testName -Value $testVal -PropertyType String -Force | Out-Null
            $back = (Get-ItemProperty -Path $fxReg -Name $testName -ErrorAction Stop).$testName
            Remove-ItemProperty -Path $fxReg -Name $testName -Force -ErrorAction Stop
            if ($back -eq $testVal) {
                Write-Host 'WRITE TEST: PASS -- the device link path is writable and readable.' -ForegroundColor Green
            } else {
                Write-Host 'WRITE TEST: FAIL -- value read back did not match.' -ForegroundColor Red
            }
        } catch {
            Write-Host ("WRITE TEST: FAIL -- " + $_.Exception.Message) -ForegroundColor Red
        }
    }
} elseif (-not $elevated) {
    Write-Host 'Tip: re-run this script as Administrator to also test writing to the device key.' -ForegroundColor DarkGray
}

Write-Host ''
Write-Host 'Done. Paste the output above back to the developer.' -ForegroundColor Cyan
