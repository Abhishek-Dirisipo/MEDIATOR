# MEDIATOR - Build & Flash Script
# Run this from PowerShell whenever you make code changes.
# Does NOT source pico-env.ps1 (that script overwrites SDK path with v1.5.1)

$env:PICO_SDK_PATH = "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\pico-sdk-2.1.0"
$env:PATH = "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\gcc-arm-none-eabi\bin;" +
            "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\cmake\bin;" +
            "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\ninja;" +
            "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\python;" +
            "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\picotool-bin;" +
            $env:PATH

$BuildDir  = "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\MEDIATOR\build"
$SourceDir = "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\MEDIATOR"
$Picotool  = "E:\Personal\Hardware Hacking - Raspberry pi pico\PICO 2 W\picotool-bin\picotool\picotool.exe"

# First time / clean build: uncomment these two lines:
# Remove-Item -Recurse -Force $BuildDir -ErrorAction SilentlyContinue
# New-Item -ItemType Directory $BuildDir | Out-Null

# Incremental build (only recompiles changed files)
Set-Location $BuildDir
ninja
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed!"; exit 1 }
Write-Host "Build OK. Put Pico into BOOTSEL mode (hold BOOTSEL, plug in), then press Enter..."
Read-Host

# Wait for the Pico to appear as a USB mass storage device
Write-Host "Waiting for Pico drive..."
while (!(Test-Path "F:\")) { Start-Sleep -Milliseconds 300 }
Copy-Item -Force "bridge.uf2" "F:\"
Write-Host "Flashed! Pico will reboot automatically."
