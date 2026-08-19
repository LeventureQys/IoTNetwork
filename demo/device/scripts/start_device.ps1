# 设备模拟器启动脚本（Windows PowerShell，自包含：只引用 demo/device 内部）。
# 用法：.\start_device.ps1 [-DeviceIndex 0] [-Duration 0] [-Scenario <path>]
param(
    [int]$DeviceIndex = 0,
    [int]$Duration = 0,
    [string]$Scenario = ""
)
$ErrorActionPreference = "Stop"

$DeviceRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $DeviceRoot "build"
$Program = Join-Path $BuildDir "bin\Debug\provision_device.exe"
$Config = Join-Path $DeviceRoot "config\device_sim.json"

if (-not (Test-Path $Program)) {
    cmake -S $DeviceRoot -B $BuildDir -DDEVICE_BUILD_TESTS=OFF
    cmake --build $BuildDir --config Debug --parallel
}

$args = @("--config", $Config, "--backend", "sim",
          "--device-index", "$DeviceIndex", "--fresh")
if ($Duration -gt 0) { $args += @("--duration", "$Duration") }
if ($Scenario -ne "") { $args += @("--scenario", (Resolve-Path $Scenario).Path) }

$env:QT_QPA_PLATFORM = "offscreen"
& $Program @args
exit $LASTEXITCODE
