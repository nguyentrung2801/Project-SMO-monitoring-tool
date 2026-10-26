param(
    [ValidateSet("info", "build", "upload", "monitor", "clean", "devices")]
    [string]$Task = "info"
)

$ErrorActionPreference = "Stop"

$pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (!(Test-Path $pio)) {
    Write-Error "Khong tim thay PlatformIO Core tai: $pio. Hay cai extension 'PlatformIO IDE' trong VS Code truoc."
}

$env:PYTHONIOENCODING = "utf-8"
$env:PLATFORMIO_SETTING_ENABLE_TELEMETRY = "No"

switch ($Task) {
    "info" {
        & $pio --version
        & $pio project config
    }
    "build" {
        & $pio run
    }
    "upload" {
        & $pio run --target upload
    }
    "monitor" {
        & $pio device monitor
    }
    "clean" {
        & $pio run --target clean
    }
    "devices" {
        & $pio device list
    }
}
