# MEDIATOR - Portable Build & Flash Script
# Requires: Pico SDK 2.1.0, CMake, Ninja, ARM GCC in your PATH.
# Run this from the "Pico Developer Command Prompt" on Windows.

$ScriptDir = $PSScriptRoot

# Load local overrides if they exist (for custom local paths)
if (Test-Path "$ScriptDir\local_env.ps1") {
    . "$ScriptDir\local_env.ps1"
}

$BuildDir  = "$ScriptDir\build"

# First time setup
if (-not (Test-Path $BuildDir)) {
    Write-Host "Creating build directory..."
    New-Item -ItemType Directory $BuildDir | Out-Null
    Set-Location $BuildDir
    cmake .. -G Ninja
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configuration failed!"; exit 1 }
} else {
    Set-Location $BuildDir
}

# Build
Write-Host "Compiling MEDIATOR..."
ninja
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed!"; exit 1 }

Write-Host "Build OK."
Write-Host "Please put your Pico into BOOTSEL mode (hold the BOOTSEL button while plugging it in)."
Write-Host "Waiting for RPI-RP2 drive to mount..."

# Auto-detect RPI-RP2 drive
$PicoDrive = $null
while ($null -eq $PicoDrive) {
    $Drive = Get-Volume | Where-Object { $_.FileSystemLabel -eq 'RPI-RP2' }
    if ($Drive) { 
        $PicoDrive = $Drive.DriveLetter + ":\" 
    } else {
        Start-Sleep -Milliseconds 300
    }
}

Write-Host "Found Pico drive at $PicoDrive. Flashing firmware..."
Copy-Item -Force "bridge.uf2" $PicoDrive
Write-Host "Flashed successfully! The Pico will now reboot."
