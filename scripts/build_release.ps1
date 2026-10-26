param(
    [string]$Environment = "esp32-c3-mini",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$releaseDir = Join-Path $projectRoot "release\smo"
$buildDir = Join-Path $projectRoot ".pio\build\$Environment"

$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
$python = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
$esptool = Join-Path $env:USERPROFILE ".platformio\packages\tool-esptoolpy\esptool.py"
$bootApp0 = Join-Path $env:USERPROFILE ".platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

foreach ($tool in @($pio, $python, $esptool, $bootApp0)) {
    if (!(Test-Path -LiteralPath $tool)) {
        throw "Required build tool was not found: $tool"
    }
}

if (!$SkipBuild) {
    Write-Host "Building SMO Monitor ($Environment)..." -ForegroundColor Cyan
    & $pio run -e $Environment
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO build failed for environment: $Environment"
    }
}

$bootloader = Join-Path $buildDir "bootloader.bin"
$partitions = Join-Path $buildDir "partitions.bin"
$firmware = Join-Path $buildDir "firmware.bin"

foreach ($file in @($bootloader, $partitions, $firmware)) {
    if (!(Test-Path -LiteralPath $file)) {
        throw "Missing build output: $file"
    }
}

New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

$appOutput = Join-Path $releaseDir "SMO_Monitor.bin"
$mergedOutput = Join-Path $releaseDir "SMO_Monitor_merged.bin"
Copy-Item -LiteralPath $firmware -Destination $appOutput -Force

Write-Host "Creating merged ESP32-C3 image..." -ForegroundColor Cyan
& $python $esptool --chip esp32c3 merge_bin `
    -o $mergedOutput `
    --flash_mode dio `
    --flash_freq 80m `
    --flash_size 4MB `
    0x0 $bootloader `
    0x8000 $partitions `
    0xe000 $bootApp0 `
    0x10000 $firmware

if ($LASTEXITCODE -ne 0) {
    throw "Failed to create merged firmware image"
}

$flashScript = Join-Path $releaseDir "flash_smo.bat"
@"
@echo off
set PORT=%~1
if "%PORT%"=="" set /p PORT=Enter COM port, for example COM3: 
if "%PORT%"=="" exit /b 1

where esptool.exe >nul 2>nul
if errorlevel 1 (
  echo esptool.exe was not found in PATH.
  echo For an existing matching installation, flash SMO_Monitor.bin at 0x10000.
  pause
  exit /b 1
)

esptool.exe --chip esp32c3 --port %PORT% --baud 460800 write_flash 0x10000 "%~dp0SMO_Monitor.bin"
pause
"@ | Set-Content -Path $flashScript -Encoding ASCII

$readme = Join-Path $releaseDir "README_FLASH.txt"
@"
SMO Monitor firmware release
======================================

Complete image for fresh installation (clears NVS)
--------------------------
File: SMO_Monitor_merged.bin
Flash address: 0x0

This file contains the bootloader, partition table, boot_app0 and application.
LittleFS is NOT included; blank boards also need pio run -t uploadfs.
Its padding covers the NVS region and overwrites saved settings and test state.
Do NOT use the merged image to update a device whose state must be preserved.
Use Espressif Flash Download Tool for a fresh installation only.

Application-only image
----------------------
File: SMO_Monitor.bin
Flash address: 0x10000

Use the application-only image only when the board already has the matching
bootloader and partition table.
This preserves NVS (Wi-Fi, test information, configuration, test state and ACKs).
Update an existing matching installation using:

    flash_smo.bat COM3

Target
------
Chip: ESP32-C3
Flash size: 4 MB
Flash mode: DIO
PlatformIO environment: $Environment

Important
---------
Application-only updates and standard PlatformIO uploads with the same partition
layout preserve NVS and LittleFS. Do not erase flash or flash a merged image when
you want to keep data. A full erase removes settings, test state, ACKs and history.
"@ | Set-Content -Path $readme -Encoding ASCII

Write-Host "Release created successfully:" -ForegroundColor Green
Write-Host "  $appOutput"
Write-Host "  $mergedOutput"
