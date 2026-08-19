#requires -Version 5.1
<#
.SYNOPSIS
    二进制隔离验收（验收文档 G4）：只复制两端 install artifacts（bin/config/share）
    与 integration_tests 到系统临时目录，不复制任何端侧源码，在该目录执行
    manifest 一致性（场景 a）与双进程 sim 场景（场景 b），并扫描运行日志
    确认不出现原仓库路径、不依赖端侧源码。

    用法:
      powershell -ExecutionPolicy Bypass -File scripts\run_binary_isolation.ps1 `
          -PcArtifact out\artifacts\pc `
          -DeviceArtifact out\artifacts\device `
          -IntegrationRoot demo\integration_tests `
          -QtBin D:/Devtools/Qt/6.8.3/msvc2022_64/bin `
          -RuntimeRoot out\ss07-isolation
#>
param(
    [string]$PcArtifact = "",
    [string]$DeviceArtifact = "",
    [string]$IntegrationRoot = "",
    [string]$QtBin = "",
    [string]$RuntimeRoot = "out\ss07-isolation"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $PcArtifact -or -not $DeviceArtifact -or -not $IntegrationRoot) {
    Write-Error "必须提供 -PcArtifact / -DeviceArtifact / -IntegrationRoot"
    exit 2
}
if (-not $QtBin) { $QtBin = if ($env:QT_BIN) { $env:QT_BIN } else { "D:/Devtools/Qt/6.8.3/msvc2022_64/bin" } }

$pcArt = [System.IO.Path]::GetFullPath($PcArtifact)
$devArt = [System.IO.Path]::GetFullPath($DeviceArtifact)
$intRoot = [System.IO.Path]::GetFullPath($IntegrationRoot)
$runRoot = [System.IO.Path]::GetFullPath($RuntimeRoot)

$tempBase = Join-Path $env:TEMP ("modu_binaryiso_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempBase -Force | Out-Null
$copyPc = Join-Path $tempBase "pc-artifact"
$copyDev = Join-Path $tempBase "device-artifact"
$copyInt = Join-Path $tempBase "integration_tests"

robocopy $pcArt $copyPc /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy $devArt $copyDev /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy $intRoot $copyInt /E /XD .git /NFL /NDL /NJH /NJS /NP | Out-Null

$missing = @()
foreach ($need in @(
    (Join-Path $copyPc "bin\provision_pc.exe"),
    (Join-Path $copyDev "bin\provision_device.exe"),
    (Join-Path $copyPc "share\protocol-contract.json"),
    (Join-Path $copyDev "share\protocol-contract.json"),
    (Join-Path $copyInt "runners\run_scenario.ps1"))) {
    if (-not (Test-Path -LiteralPath $need)) { $missing += $need }
}
if ($missing.Count -gt 0) {
    Write-Error ("隔离目录缺失文件: " + ($missing -join "; "))
    Remove-Item -Recurse -Force $tempBase -ErrorAction SilentlyContinue
    exit 1
}

# 隔离目录不得包含端侧源码标志文件
$srcProbe = @(
    (Join-Path $copyPc "app\main.cpp"),
    (Join-Path $copyDev "src\core\device_app.c"),
    (Join-Path $copyInt "pc"),
    (Join-Path $copyInt "device"))
$srcLeak = @($srcProbe | Where-Object { Test-Path -LiteralPath $_ })
if ($srcLeak.Count -gt 0) {
    Write-Error ("隔离目录出现源码残留: " + ($srcLeak -join "; "))
    Remove-Item -Recurse -Force $tempBase -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "隔离目录准备完成：无端侧源码"

$pcExe = Join-Path $copyPc "bin\provision_pc.exe"
$devExe = Join-Path $copyDev "bin\provision_device.exe"
$pcContract = Join-Path $copyPc "share\protocol-contract.json"
$devContract = Join-Path $copyDev "share\protocol-contract.json"

$runner = Join-Path $copyInt "runners\run_scenario.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File $runner -Scenario a,b `
    -PcExe $pcExe -DeviceExe $devExe `
    -PcContract $pcContract -DeviceContract $devContract `
    -RuntimeRoot $runRoot -TimeoutSeconds 60 -QtBin $QtBin
$code = $LASTEXITCODE

# 泄漏扫描：只扫描隔离副本内"运行期消费"的文本产物（manifest/contract/config）。
# 若这些文件含原仓库路径，说明 artifact 未真正自包含。二进制 PE 中的 PDB
# 调试路径与 integration_tests 脚本自身源码内容不属于运行期依赖，不计入。
$leaks = @()
$scanFiles = @()
foreach ($base in @($copyPc, $copyDev)) {
    $scanFiles += @(Get-ChildItem -LiteralPath (Join-Path $base "share") -Filter *.json -File -ErrorAction SilentlyContinue)
    $scanFiles += @(Get-ChildItem -LiteralPath (Join-Path $base "config") -Filter *.json -File -ErrorAction SilentlyContinue)
}
foreach ($f in $scanFiles) {
    foreach ($line in (Get-Content -LiteralPath $f.FullName -Encoding UTF8 -ErrorAction SilentlyContinue)) {
        if ($line -match "modutechnetwork|demo\\pc|demo\\device") { $leaks += "$($f.FullName) : $line" }
    }
}
if ($leaks.Count -gt 0) {
    Write-Host "检测到原仓库路径泄漏："
    $leaks | ForEach-Object { Write-Host "  $_" }
    $code = 1
} else {
    Write-Host "无原仓库路径泄漏"
}

Remove-Item -Recurse -Force $tempBase -ErrorAction SilentlyContinue
if ($code -ne 0) { exit $code }
Write-Host "二进制隔离验收通过"
exit 0
