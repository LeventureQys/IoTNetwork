#requires -Version 5.1
<#
.SYNOPSIS
    仓库外自包含验收（验收文档 G1/G2）：把 demo/pc 与 demo/device 分别复制到
    系统临时目录，在副本上配置、构建、CTest、offscreen 启动，并扫描构建日志与
    compile_commands.json，确认不出现原仓库路径。

.DESCRIPTION
    原仓库不加入 include/CMake path；副本之外的任何依赖都会表现为
    构建失败、路径泄漏或启动失败。本脚本不修改原仓库任何文件。

    用法:
      powershell -ExecutionPolicy Bypass -File scripts\run_self_contained.ps1 `
          -RepoRoot ..\.. `
          -QtPrefix D:/Devtools/Qt/6.8.3/msvc2022_64 `
          -OutDir out\ss07-selfcontained
#>
param(
    [string]$RepoRoot = "..\..",
    [string]$QtPrefix = "D:/Devtools/Qt/6.8.3/msvc2022_64",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Config = "Debug",
    [string]$OutDir = "out\ss07-selfcontained",
    [int]$Parallel = 8
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot $RepoRoot))
$outRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $OutDir))
New-Item -ItemType Directory -Path $outRoot -Force | Out-Null
$log = Join-Path $outRoot "isolation-run.log"
function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $m
    Write-Host $line
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
}
function RunStep([string]$name, [scriptblock]$body, [string]$outLog) {
    Log "---- $name ----"
    try {
        & $body *> $outLog
        if ($LASTEXITCODE -ne 0) { throw "步骤 $name 退出码 $LASTEXITCODE" }
        Log "  $name 通过"
        return $true
    } catch {
        Log "  $name 失败: $_"
        return $false
    }
}
function Test-RepoPathLeak([string[]]$files, [string]$label) {
    $leaks = @()
    foreach ($f in $files) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        foreach ($line in (Get-Content -LiteralPath $f -Encoding UTF8 -ErrorAction SilentlyContinue)) {
            if ($line -match [regex]::Escape($repo) -or
                $line -match [regex]::Escape($repo.Replace('/', '\')) -or
                $line -match [regex]::Escape($repo.Replace('\', '/'))) {
                $leaks += "$f : $line"
            }
        }
    }
    if ($leaks.Count -eq 0) {
        Log "  [$label] 无原仓库路径泄漏"
        return $true
    }
    Log "  [$label] 检测到原仓库路径泄漏:"
    $leaks | ForEach-Object { Log "    $_" }
    return $false
}

$ok = $true
$tempBase = Join-Path $env:TEMP ("modu_selfcontained_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempBase -Force | Out-Null
Log "临时副本根: $tempBase"

$results = @{}

# ---------- PC 副本 ----------
$pcCopy = Join-Path $tempBase "pc"
$pcOk = $true
$pcOk = RunStep "复制 demo/pc -> $pcCopy" {
    robocopy (Join-Path $repo "demo\pc") $pcCopy /E /XD out build .git /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "robocopy 失败 code=$LASTEXITCODE" }
    $global:LASTEXITCODE = 0
} (Join-Path $outRoot "pc-copy.log")
if ($pcOk) {
    $pcBuild = Join-Path $pcCopy "out\pc-build"
    $pcOk = RunStep "PC configure（副本内）" {
        cmake -S $pcCopy -B $pcBuild -G $Generator -A x64 -DCMAKE_PREFIX_PATH=$QtPrefix -DPC_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    } (Join-Path $outRoot "pc-configure.log")
    if ($pcOk) {
        $pcOk = RunStep "PC build（副本内）" {
            cmake --build $pcBuild --config $Config --parallel $Parallel
        } (Join-Path $outRoot "pc-build.log")
    }
    if ($pcOk) {
        $pcOk = RunStep "PC ctest（副本内）" {
            ctest --test-dir $pcBuild -C $Config --output-on-failure
        } (Join-Path $outRoot "pc-ctest.log")
    }
    if ($pcOk) {
        $pcOk = RunStep "PC offscreen 启动（副本内，空 CWD）" {
            $exe = Join-Path $pcBuild "bin\$Config\provision_pc.exe"
            $env:QT_QPA_PLATFORM = "offscreen"
            $env:PATH = "$QtPrefix\bin;" + $env:PATH
            $cwd = Join-Path $outRoot "pc-empty-cwd"
            New-Item -ItemType Directory -Path $cwd -Force | Out-Null
            $proc = Start-Process -FilePath $exe -ArgumentList @("--backend", "sim", "--duration", "3") `
                -WorkingDirectory $cwd -WindowStyle Hidden -Wait -PassThru `
                -RedirectStandardOutput (Join-Path $outRoot "pc-launch-stdout.log") `
                -RedirectStandardError (Join-Path $outRoot "pc-launch-stderr.log")
            if ($proc.ExitCode -ne 0) { throw "PC offscreen 启动退出码 $($proc.ExitCode)" }
        } (Join-Path $outRoot "pc-launch.log")
    }
    if ($pcOk) {
        $cc = Join-Path $pcBuild "compile_commands.json"
        $pcOk = Test-RepoPathLeak @((Join-Path $outRoot "pc-configure.log"), (Join-Path $outRoot "pc-build.log"), $cc) "PC"
    }
    $results["pc"] = $pcOk
} else { $results["pc"] = $false }

# ---------- 设备副本 ----------
$devCopy = Join-Path $tempBase "device"
$devOk = $true
$devOk = RunStep "复制 demo/device -> $devCopy" {
    robocopy (Join-Path $repo "demo\device") $devCopy /E /XD out build .git /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "robocopy 失败 code=$LASTEXITCODE" }
    $global:LASTEXITCODE = 0
} (Join-Path $outRoot "device-copy.log")
if ($devOk) {
    $devBuild = Join-Path $devCopy "out\device-build"
    $devOk = RunStep "设备 configure（副本内）" {
        cmake -S $devCopy -B $devBuild -G $Generator -A x64 -DCMAKE_PREFIX_PATH=$QtPrefix -DDEVICE_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    } (Join-Path $outRoot "device-configure.log")
    if ($devOk) {
        $devOk = RunStep "设备 build（副本内）" {
            cmake --build $devBuild --config $Config --parallel $Parallel
        } (Join-Path $outRoot "device-build.log")
    }
    if ($devOk) {
        $devOk = RunStep "设备 ctest（副本内）" {
            ctest --test-dir $devBuild -C $Config --output-on-failure
        } (Join-Path $outRoot "device-ctest.log")
    }
    if ($devOk) {
        $devOk = RunStep "设备 offscreen 启动（副本内，空 CWD）" {
            $exe = Join-Path $devBuild "bin\$Config\provision_device.exe"
            $env:QT_QPA_PLATFORM = "offscreen"
            $env:PATH = "$QtPrefix\bin;" + $env:PATH
            $cwd = Join-Path $outRoot "device-empty-cwd"
            $catalog = Join-Path $outRoot "device-launch-catalog"
            New-Item -ItemType Directory -Path $cwd -Force | Out-Null
            New-Item -ItemType Directory -Path $catalog -Force | Out-Null
            $proc = Start-Process -FilePath $exe `
                -ArgumentList @("--backend", "sim", "--device-index", "0", "--fresh", "--duration", "3", "--sim-catalog-dir", $catalog) `
                -WorkingDirectory $cwd -WindowStyle Hidden -Wait -PassThru `
                -RedirectStandardOutput (Join-Path $outRoot "device-launch-stdout.log") `
                -RedirectStandardError (Join-Path $outRoot "device-launch-stderr.log")
            if ($proc.ExitCode -ne 0) { throw "设备 offscreen 启动退出码 $($proc.ExitCode)" }
        } (Join-Path $outRoot "device-launch.log")
    }
    if ($devOk) {
        $cc = Join-Path $devBuild "compile_commands.json"
        $devOk = Test-RepoPathLeak @((Join-Path $outRoot "device-configure.log"), (Join-Path $outRoot "device-build.log"), $cc) "Device"
    }
    $results["device"] = $devOk
} else { $results["device"] = $false }

$ok = $results["pc"] -and $results["device"]
$summary = [ordered]@{ timestamp = (Get-Date -Format "o"); pc = $results["pc"]; device = $results["device"]; temp_root = $tempBase }
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outRoot "summary.json") -Encoding UTF8
Log "==== 自包含验收结果: PC=$($results['pc']) Device=$($results['device']) ===="
Remove-Item -Recurse -Force $tempBase -ErrorAction SilentlyContinue
if (-not $ok) { exit 1 }
exit 0
