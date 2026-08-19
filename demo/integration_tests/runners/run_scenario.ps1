#requires -Version 5.1
<#
.SYNOPSIS
    跨端集成测试 runner（PowerShell 5.1+，无第三方依赖）。
    启动已构建的 provision_pc / provision_device 双进程，以公开 CLI、
    事件 JSONL、scenario 文件与退出码完成协议互操作验收。

.DESCRIPTION
    只消费两端已构建产物（可执行文件、config、share/protocol-contract.json）。
    禁止 include/编译/链接任何端侧源码；本脚本不复制、不引用端侧生产源码。

    场景清单（对应 SubStage_07 任务书与验收文档 E/F 节）：
      a  protocol manifest 一致性（PC/device contract 与 golden 语义比较）
      b  首次配网 -> 会话 + 心跳
      c  优雅退出（host_bye / 资源清理 / 端口可重绑）
      d  断链重连（sock_send_fail 注入 -> 离线 -> 恢复 -> 重连）
      e  close_ap 单次发送失败仍上线
      f  wifi_result fail：PC 明确失败、无 close_ap、设备保持可配网
      g  双向 app_data：512/513/UTF-8 字节语义
      h  帧边界与协议版本拒绝（尽力而为，原始 socket 探针）

    用法示例：
      powershell -ExecutionPolicy Bypass -File run_scenario.ps1 -Scenario all `
          -PcExe out/artifacts/pc/bin/provision_pc.exe `
          -DeviceExe out/artifacts/device/bin/provision_device.exe `
          -RuntimeRoot out/ss07-evidence
#>
param(
    [string]$Scenario = "all",
    [string]$PcExe = "",
    [string]$DeviceExe = "",
    [string]$PcConfig = "",
    [string]$DeviceConfig = "",
    [string]$PcContract = "",
    [string]$DeviceContract = "",
    [string]$RuntimeRoot = "",
    [int]$TimeoutSeconds = 45,
    [string]$QtBin = "",
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RunRoot = if ($RuntimeRoot) { $RuntimeRoot } else { Join-Path $env:TEMP "modutech-integration" }
$script:RunRoot = [System.IO.Path]::GetFullPath($script:RunRoot)
if (-not $QtBin) { $QtBin = if ($env:QT_BIN) { $env:QT_BIN } else { "D:/Devtools/Qt/6.8.3/msvc2022_64/bin" } }
$script:ScenariosDir = Join-Path $PSScriptRoot "..\scenarios"
$script:Golden = Join-Path $PSScriptRoot "..\contracts\protocol-contract.golden.json"

$script:Results = @()

function Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $msg
    Write-Host $line
    Add-Content -LiteralPath $script:RunLog -Value $line -Encoding UTF8
}

function AssertTrue([bool]$cond, [string]$msg) {
    if (-not $cond) { throw "断言失败: $msg" }
}

function Get-JsonSafe([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { throw "文件不存在: $path" }
    return Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Compare-JsonSemantic($x, $y, [string]$path, [System.Collections.ArrayList]$diffs) {
    if ($null -eq $x -or $null -eq $y) { [void]$diffs.Add("$path : 一侧为空"); return }
    if ($x -is [System.Management.Automation.PSCustomObject] -and $y -is [System.Management.Automation.PSCustomObject]) {
        $keys = @(@($x.PSObject.Properties.Name) + @($y.PSObject.Properties.Name) | Sort-Object -Unique)
        foreach ($k in $keys) {
            if ($k -eq "generated_from") { continue }
            if (-not $x.PSObject.Properties[$k]) { [void]$diffs.Add("$path.$k 缺失于 X"); continue }
            if (-not $y.PSObject.Properties[$k]) { [void]$diffs.Add("$path.$k 缺失于 Y"); continue }
            Compare-JsonSemantic $x.$k $y.$k "$path.$k" $diffs
        }
        return
    }
    if ($x -is [System.Collections.IEnumerable] -and $x -isnot [string] -and
        $y -is [System.Collections.IEnumerable] -and $y -isnot [string]) {
        $xa = @($x); $ya = @($y)
        if ($xa.Count -ne $ya.Count) { [void]$diffs.Add("$path 数组长度 $($xa.Count) vs $($ya.Count)"); return }
        for ($i = 0; $i -lt $xa.Count; $i++) {
            Compare-JsonSemantic $xa[$i] $ya[$i] "$path[$i]" $diffs
        }
        return
    }
    if ("$x" -ne "$y") { [void]$diffs.Add("$path : '$x' vs '$y'") }
}

function Sha256Hex([string]$text) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = $sha.ComputeHash($bytes)
    return ([System.BitConverter]::ToString($hash)).Replace("-", "").ToLowerInvariant()
}

function Find-Contract([string]$exePath, [string]$hint) {
    if ($hint -and (Test-Path -LiteralPath $hint)) { return (Resolve-Path -LiteralPath $hint).Path }
    $dir = Split-Path -Parent $exePath
    for ($i = 0; $i -lt 4; $i++) {
        $cand = Join-Path $dir "share\protocol-contract.json"
        if (Test-Path -LiteralPath $cand) { return (Resolve-Path -LiteralPath $cand).Path }
        $dir = Split-Path -Parent $dir
        if (-not $dir) { break }
    }
    return ""
}

function Read-Events([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    $out = @()
    foreach ($line in (Get-Content -LiteralPath $path -Encoding UTF8)) {
        if (-not $line.Trim()) { continue }
        try { $out += ($line | ConvertFrom-Json) } catch { }
    }
    return $out
}

function Wait-Events([string]$path, [string]$name, [string]$role, [int]$count, [int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $evs = Read-Events $path | Where-Object { $_.event -eq $name -and ($role -eq "" -or $_.role -eq $role) }
        if (@($evs).Count -ge $count) { return @($evs) }
        Start-Sleep -Milliseconds 200
    }
    return @($evs)
}

function Wait-AnyEvent([string[]]$paths, [string[]]$names, [int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        foreach ($p in $paths) {
            foreach ($ev in (Read-Events $p)) {
                if ($names -contains $ev.event) { return $ev }
            }
        }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

function Test-PortRebindable([int]$port) {
    $listener = $null
    try {
        $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
        $listener.Start()
        return $true
    } catch {
        return $false
    } finally {
        if ($listener) { try { $listener.Stop() } catch { } }
    }
}

function New-TempRunDir([string]$scenarioName) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $dir = Join-Path $script:RunRoot ("{0}_{1}" -f $stamp, $scenarioName)
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dir "pc-runtime") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dir "device-runtime") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dir "sim-catalog") -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $dir "logs") -Force | Out-Null
    return $dir
}

function Start-SideProcess(
    [string]$exe,
    [string[]]$arguments,
    [string]$stdoutLog,
    [string]$stderrLog,
    [int]$durationSec,
    [string]$workDir)
{
    $full = (Resolve-Path -LiteralPath $exe).Path
    $allArgs = @($arguments)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $full
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($workDir) { $psi.WorkingDirectory = $workDir }
    $psi.EnvironmentVariables["QT_QPA_PLATFORM"] = "offscreen"
    $oldPath = $psi.EnvironmentVariables["PATH"]
    $psi.EnvironmentVariables["PATH"] = "$QtBin;$oldPath"
    $sb = New-Object System.Text.StringBuilder
    foreach ($a in $allArgs) {
        if ($a -match "[\s\""]") {
            [void]$sb.Append('"').Append($a.Replace('"', '\"')).Append('"').Append(' ')
        } else {
            [void]$sb.Append($a).Append(' ')
        }
    }
    $psi.Arguments = $sb.ToString().TrimEnd()
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    if (-not $proc.Start()) { throw "进程启动失败: $full" }
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    return @{ Process = $proc; OutTask = $outTask; ErrTask = $errTask; Stdout = $stdoutLog; Stderr = $stderrLog }
}

function Stop-SideProcess($side, [int]$waitMs) {
    $proc = $side.Process
    if ($proc -and -not $proc.HasExited) {
        if (-not $proc.WaitForExit($waitMs)) {
            Log "  强杀进程 PID=$($proc.Id)"
            $proc.Kill()
            $proc.WaitForExit(10000) | Out-Null
        }
    }
}

function Collect-SideOutputs($side) {
    try { $out = $side.OutTask.Result; if ($out) { [System.IO.File]::WriteAllText($side.Stdout, $out) } } catch { }
    try { $err = $side.ErrTask.Result; if ($err) { [System.IO.File]::WriteAllText($side.Stderr, $err) } } catch { }
}

function Write-Summary([string]$dir, [string]$scenario, [bool]$pass, [string[]]$checks, [string]$note) {
    $sum = [ordered]@{
        scenario = $scenario
        pass = $pass
        timestamp = (Get-Date -Format "o")
        checks = $checks
        note = $note
    }
    $sum | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir "summary.json") -Encoding UTF8
}

# ---------------------------------------------------------------------------
# 场景 a：manifest 一致性
# ---------------------------------------------------------------------------
function Invoke-ScenarioA([string]$dir) {
    $checks = @()
    $pcContract = Find-Contract $PcExe $PcContract
    $devContract = Find-Contract $DeviceExe $DeviceContract
    AssertTrue ($pcContract -ne "") "未找到 PC protocol-contract.json（用 -PcContract 指定）"
    AssertTrue ($devContract -ne "") "未找到 设备 protocol-contract.json（用 -DeviceContract 指定）"
    $golden = Get-JsonSafe $script:Golden
    $pc = Get-JsonSafe $pcContract
    $dev = Get-JsonSafe $devContract

    $d1 = New-Object System.Collections.ArrayList
    Compare-JsonSemantic $pc $golden "pc" $d1
    $d2 = New-Object System.Collections.ArrayList
    Compare-JsonSemantic $dev $golden "device" $d2
    $d3 = New-Object System.Collections.ArrayList
    Compare-JsonSemantic $pc $dev "pc_vs_device" $d3

    $diff = [ordered]@{
        pc_vs_golden = $d1.ToArray()
        device_vs_golden = $d2.ToArray()
        pc_vs_device = $d3.ToArray()
    }
    $diff | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $dir "protocol-diff.json") -Encoding UTF8

    $ok = ($d1.Count -eq 0) -and ($d2.Count -eq 0) -and ($d3.Count -eq 0)
    $checks += "PC contract == golden（忽略 generated_from）: $($d1.Count -eq 0)"
    $checks += "Device contract == golden（忽略 generated_from）: $($d2.Count -eq 0)"
    $checks += "PC contract == Device contract: $($d3.Count -eq 0)"
    Write-Summary $dir "a" $ok $checks "pc=$pcContract device=$devContract"
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 b/f/g/c/e 共用的双进程启动器
# ---------------------------------------------------------------------------
function Start-DualProcess([string]$dir, [string]$scenarioFile, [int]$pcDuration, [int]$devDuration) {
    $pcRuntime = Join-Path $dir "pc-runtime"
    $devRuntime = Join-Path $dir "device-runtime"
    $catalog = Join-Path $dir "sim-catalog"
    $pcEvents = Join-Path $dir "pc-events.jsonl"
    $devEvents = Join-Path $dir "device-events.jsonl"

    $cfgDir = Join-Path $dir "configs"
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null

    if ($PcConfig) {
        Copy-Item -LiteralPath $PcConfig -Destination (Join-Path $cfgDir "pc_config.json") -Force
    } else {
        @{
            host_tcp_port = 5935; host_advertise_ip = "127.0.0.1"
            mcast_group = "224.0.2.1"; mcast_port = 5936
            heartbeat_interval_ms = 2000; heartbeat_dead_ms = 0
            hello_timeout_ms = 5000
            provision_auth_timeout_ms = 3000; provision_wifi_cfg_timeout_ms = 8000
            provision_sta_try_max = 3; provision_handoff_grace_ms = 1000
            discovery_fast_window_ms = 8000; discovery_normal_interval_ms = 1000
            discovery_candidate_timeout_ms = 6000
            reconnect_backoff_base_ms = 1000; reconnect_backoff_cap_ms = 5000
            reconnect_backoff_jitter_ms = 500; reconnect_to_discovery_ms = 30000
            busy_backoff_ms = 6000; host_max_conn = 16; malformed_max_per_conn = 3
            device_rate_limit_per_sec = 50
            target_ssid = "TactileFactory-2.4G"; target_password = "securepass123"
            target_band_2g = 1; duration_s = 0; config_tag = "integration-pc"
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $cfgDir "pc_config.json") -Encoding UTF8
    }
    if ($DeviceConfig) {
        Copy-Item -LiteralPath $DeviceConfig -Destination (Join-Path $cfgDir "device_sim.json") -Force
    } else {
        @{
            power_on_jitter_max_ms = 1000; wifi_retry_max = 5
            wifi_backoff_base_ms = 1000; wifi_backoff_cap_ms = 30000; wifi_backoff_jitter_ms = 5000
            provision_auth_timeout_ms = 3000; provision_wifi_cfg_timeout_ms = 8000
            provision_ap_idle_timeout_ms = 0; provision_ap_backoff_ms = 300000
            provision_pin_fail_max = 5; provision_confirm_window_ms = 12000
            provision_sta_try_max = 3; provision_handoff_grace_ms = 1000
            discovery_fast_window_ms = 8000; discovery_fast_interval_ms = 500
            discovery_normal_interval_ms = 1000; discovery_candidate_timeout_ms = 6000
            heartbeat_interval_ms = 2000; heartbeat_dead_ms = 0
            host_max_conn = 16; busy_backoff_ms = 6000; hello_timeout_ms = 5000
            rssi_sample_interval_ms = 1000; rssi_bad_threshold_dbm = -75; rssi_bad_duration_ms = 6000
            reconnect_backoff_base_ms = 1000; reconnect_backoff_cap_ms = 5000
            reconnect_backoff_jitter_ms = 500; reconnect_to_discovery_ms = 30000
            watchdog_state_timeout_ms = 30000; malformed_max_per_conn = 3
            device_rate_limit_per_sec = 50
            host_tcp_port = 5935; host_virtual_ip = "192.168.1.50"
            mcast_group = "224.0.2.1"; mcast_port = 5936
            device_ap_port_base = 20000; nvs_dir = "run"; use_real_wifi_sta = 0
            device_count = 1
            target_ssid = "TactileFactory-2.4G"; target_password = "securepass123"
            target_band_2g = 1; device_fw_version = "1.0.0"; device_proto_ver = 1
            duration_s = 0; config_tag = "integration-device"
        } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $cfgDir "device_sim.json") -Encoding UTF8
    }
    $pcCfgPath = (Resolve-Path -LiteralPath (Join-Path $cfgDir "pc_config.json")).Path
    $devCfgPath = (Resolve-Path -LiteralPath (Join-Path $cfgDir "device_sim.json")).Path

    $scnCopy = Join-Path $dir "scenario.json"
    Copy-Item -LiteralPath $scenarioFile -Destination $scnCopy -Force

    $emptyCwd = Join-Path $dir "empty-cwd"
    New-Item -ItemType Directory -Path $emptyCwd -Force | Out-Null

    $pcSide = Start-SideProcess $PcExe @(
        "--config", $pcCfgPath,
        "--backend", "sim",
        "--runtime-dir", $pcRuntime,
        "--sim-catalog-dir", $catalog,
        "--log-dir", (Join-Path $dir "logs"),
        "--events-jsonl", $pcEvents,
        "--scenario", $scnCopy,
        "--duration", "$pcDuration"
    ) (Join-Path $dir "pc-stdout.log") (Join-Path $dir "pc-stderr.log") $pcDuration $emptyCwd

    $devSide = Start-SideProcess $DeviceExe @(
        "--config", $devCfgPath,
        "--backend", "sim",
        "--device-index", "0",
        "--fresh",
        "--runtime-dir", $devRuntime,
        "--sim-catalog-dir", $catalog,
        "--log-dir", (Join-Path $dir "logs"),
        "--events-jsonl", $devEvents,
        "--scenario", $scnCopy,
        "--duration", "$devDuration"
    ) (Join-Path $dir "device-stdout.log") (Join-Path $dir "device-stderr.log") $devDuration $emptyCwd

    return @{
        dir = $dir; catalog = $catalog; pcEvents = $pcEvents; devEvents = $devEvents
        pcSide = $pcSide; devSide = $devSide
        pcRuntime = $pcRuntime; devRuntime = $devRuntime
        pcDuration = $pcDuration; devDuration = $devDuration
    }
}

function Finish-DualProcess($ctx, [int]$timeoutSec) {
    $checks = New-Object System.Collections.ArrayList
    $pc = $ctx.pcSide; $dev = $ctx.devSide
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($pc.Process.HasExited -and $dev.Process.HasExited) { break }
        Start-Sleep -Milliseconds 300
    }
    Stop-SideProcess $pc 3000
    Stop-SideProcess $dev 3000
    Collect-SideOutputs $pc
    Collect-SideOutputs $dev

    $pcCode = if ($pc.Process.HasExited) { $pc.Process.ExitCode } else { -999 }
    $devCode = if ($dev.Process.HasExited) { $dev.Process.ExitCode } else { -999 }
    [void]$checks.Add("PC 退出码: $pcCode")
    [void]$checks.Add("Device 退出码: $devCode")
    [void]$checks.Add("PC PID=$($pc.Process.Id) vs Device PID=$($dev.Process.Id)")

    $pcLast = (Read-Events $ctx.pcEvents | Select-Object -Last 1)
    $devLast = (Read-Events $ctx.devEvents | Select-Object -Last 1)
    [void]$checks.Add("PC 最后事件: $($pcLast.event) (期望 shutdown_complete)")
    [void]$checks.Add("Device 最后事件: $($devLast.event) (期望 shutdown_complete)")

    $pcAlive = Get-Process -Id $pc.Process.Id -ErrorAction SilentlyContinue
    $devAlive = Get-Process -Id $dev.Process.Id -ErrorAction SilentlyContinue
    [void]$checks.Add("无残留 PC 进程: $($null -eq $pcAlive)")
    [void]$checks.Add("无残留 Device 进程: $($null -eq $devAlive)")
    [void]$checks.Add("TCP 5935 可重绑: $(Test-PortRebindable 5935)")
    [void]$checks.Add("TCP 20000 可重绑: $(Test-PortRebindable 20000)")

    $catalogFiles = @(Get-ChildItem -LiteralPath $ctx.catalog -Filter *.json -ErrorAction SilentlyContinue)
    [void]$checks.Add("sim-catalog 退出后清空（无残留 device-*.json）: $($catalogFiles.Count -eq 0)")

    return @{
        Checks = $checks.ToArray()
        PcCode = $pcCode; DevCode = $devCode
        PcLast = if ($pcLast) { $pcLast.event } else { "none" }
        DevLast = if ($devLast) { $devLast.event } else { "none" }
    }
}

# ---------------------------------------------------------------------------
# 场景 b：首次配网 -> 会话
# ---------------------------------------------------------------------------
function Invoke-ScenarioB([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "b_first_provision.json"
    $ctx = Start-DualProcess $dir $scn 25 25

    $devReady = Wait-Events $ctx.devEvents "ready" "device" 1 15000
    AssertTrue (@($devReady).Count -ge 1) "设备 ready 事件缺失"
    $pcReady = Wait-Events $ctx.pcEvents "ready" "pc" 1 15000
    AssertTrue (@($pcReady).Count -ge 1) "PC ready 事件缺失"
    [void]$checks.Add("PC ready + 设备 ap_ready 前 ready 就绪")

    $ap = Wait-Events $ctx.devEvents "ap_ready" "device" 1 15000
    AssertTrue (@($ap).Count -ge 1) "ap_ready 缺失"
    [void]$checks.Add("ap_ready 出现: $($ap[0].data.ssid)")

    $auth = Wait-Events $ctx.pcEvents "auth_result" "pc" 1 20000
    AssertTrue (@($auth).Count -ge 1 -and $auth[0].result -eq "ok") "auth_result 非 ok"
    $wifi = Wait-Events $ctx.pcEvents "wifi_result" "pc" 1 20000
    AssertTrue (@($wifi).Count -ge 1 -and $wifi[0].data.status -eq "ok") "wifi_result 非 ok"
    [void]$checks.Add("auth_result=ok / wifi_result=ok（PC 侧）")
    $dauth = Wait-Events $ctx.devEvents "auth_result" "device" 1 20000
    $dwifi = Wait-Events $ctx.devEvents "wifi_result" "device" 1 20000
    AssertTrue (@($dauth).Count -ge 1 -and $dauth[0].data.status -eq "ok") "设备 auth_result 非 ok"
    AssertTrue (@($dwifi).Count -ge 1 -and $dwifi[0].data.status -eq "ok") "设备 wifi_result 非 ok"
    [void]$checks.Add("auth_result=ok / wifi_result=ok（设备侧）")

    $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 25000
    $devOnline = Wait-Events $ctx.devEvents "session_online" "device" 1 25000
    AssertTrue (@($pcOnline).Count -ge 1) "PC session_online 缺失"
    AssertTrue (@($devOnline).Count -ge 1) "设备 session_online 缺失"
    [void]$checks.Add("双方 session_online（PC reconnect=$($pcOnline[0].data.reconnect_count)，设备 connect=$($devOnline[0].data.reconnect_count)）")

    $hb = Wait-AnyEvent @($ctx.pcEvents, $ctx.devEvents) @("ping", "pong") 10000
    AssertTrue ($null -ne $hb) "session_online 后 10 秒内未见 ping/pong"
    [void]$checks.Add("session_online 后 10 秒内出现 ping/pong: $($hb.event)")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "b" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 c：优雅退出
# ---------------------------------------------------------------------------
function Invoke-ScenarioC([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "c_graceful_exit.json"
    $ctx = Start-DualProcess $dir $scn 25 25

    $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
    AssertTrue (@($pcOnline).Count -ge 1) "PC session_online 缺失"
    $bye = Wait-Events $ctx.pcEvents "host_bye_sent" "pc" 1 15000
    AssertTrue (@($bye).Count -ge 1) "host_bye_sent 缺失"
    [void]$checks.Add("host_bye_sent 出现（send_result=$($bye[0].data.send_result)）")

    $devOffline = Wait-Events $ctx.devEvents "session_offline" "device" 1 20000
    AssertTrue (@($devOffline).Count -ge 1) "设备 session_offline 缺失（host_bye/连接关闭路径）"
    $pcOffline = Wait-Events $ctx.pcEvents "session_offline" "pc" 1 20000
    [void]$checks.Add("设备离线（reason=$($devOffline[0].data.reason)），PC 侧 session_offline: $(@($pcOffline).Count -ge 1)")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "c" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 d：断链重连
# ---------------------------------------------------------------------------
function Invoke-ScenarioD([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "d_reconnect.json"
    $ctx = Start-DualProcess $dir $scn 45 45

    $online1pc = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
    AssertTrue (@($online1pc).Count -ge 1) "首次 session_online（PC）缺失"
    $online1dev = Wait-Events $ctx.devEvents "session_online" "device" 1 30000
    AssertTrue (@($online1dev).Count -ge 1) "首次 session_online（设备）缺失"
    $fault = Wait-Events $ctx.devEvents "fault_applied" "device" 1 20000
    AssertTrue (@($fault).Count -ge 1) "fault_applied 缺失"
    AssertTrue ($fault[0].data.action -eq "sock_send_fail") "fault action 不符"
    [void]$checks.Add("fault_applied（$($fault[0].data.action), status=$($fault[0].data.status)）")

    $off1pc = Wait-Events $ctx.pcEvents "session_offline" "pc" 1 20000
    $off1dev = Wait-Events $ctx.devEvents "session_offline" "device" 1 20000
    AssertTrue (@($off1pc).Count -ge 1) "PC session_offline 缺失"
    AssertTrue (@($off1dev).Count -ge 1) "设备 session_offline 缺失"
    [void]$checks.Add("断链后双方 session_offline（pc reason=$($off1pc[0].data.reason) / dev reason=$($off1dev[0].data.reason)）")

    $online2pc = Wait-Events $ctx.pcEvents "session_online" "pc" 2 30000
    $online2dev = Wait-Events $ctx.devEvents "session_online" "device" 2 30000
    AssertTrue (@($online2pc).Count -ge 2) "PC 重连 session_online 缺失"
    AssertTrue (@($online2dev).Count -ge 2) "设备重连 session_online 缺失"
    $rc1 = [int]$online1pc[0].data.reconnect_count
    $rc2 = [int]$online2pc[1].data.reconnect_count
    $dc1 = [int]($online1dev | Select-Object -First 1).data.reconnect_count
    $dc2 = [int]$online2dev[-1].data.reconnect_count
    AssertTrue ($rc2 -gt $rc1) "PC reconnect_count 未增长: $rc1 -> $rc2"
    AssertTrue ($dc2 -gt $dc1) "设备 reconnect_count 未增长: $dc1 -> $dc2"
    [void]$checks.Add("重连后 reconnect_count 增长（PC $rc1->$rc2，设备 $dc1->$dc2）")

    $hb = Wait-AnyEvent @($ctx.pcEvents, $ctx.devEvents) @("ping", "pong") 10000
    AssertTrue ($null -ne $hb) "重连后 10 秒内未见 ping/pong"
    [void]$checks.Add("重连后 10 秒内出现 ping/pong: $($hb.event)")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 20)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "d" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 e：close_ap 单次发送失败仍上线
# ---------------------------------------------------------------------------
function Invoke-ScenarioE([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "e_close_ap_fail.json"
    $ctx = Start-DualProcess $dir $scn 25 25

    $fault = Wait-Events $ctx.pcEvents "fault_applied" "pc" 1 15000
    AssertTrue (@($fault).Count -ge 1) "PC fault_applied 缺失"
    $close = Wait-Events $ctx.pcEvents "close_ap_sent" "pc" 1 25000
    AssertTrue (@($close).Count -ge 1) "close_ap_sent 缺失"
    AssertTrue ($close[0].code -ne 0) "close_ap_sent 应记录发送失败（code=$($close[0].code)）"
    [void]$checks.Add("close_ap_sent 失败一次（send_result=$($close[0].data.send_result)）")

    $wifi = Wait-Events $ctx.pcEvents "wifi_result" "pc" 1 20000
    AssertTrue (@($wifi).Count -ge 1 -and $wifi[0].data.status -eq "ok") "wifi_result 非 ok"
    [void]$checks.Add("close_ap 失败后 wifi_result 仍为 ok（配网结果未被改写）")

    $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
    $devOnline = Wait-Events $ctx.devEvents "session_online" "device" 1 30000
    AssertTrue (@($pcOnline).Count -ge 1) "PC session_online 缺失"
    AssertTrue (@($devOnline).Count -ge 1) "设备 session_online 缺失"
    [void]$checks.Add("设备最终上线（无无限重试）")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "e" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 f：wifi_result fail
# ---------------------------------------------------------------------------
function Invoke-ScenarioF([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "f_wifi_result_fail.json"
    $ctx = Start-DualProcess $dir $scn 25 25

    $wifi = Wait-Events $ctx.pcEvents "wifi_result" "pc" 1 25000
    AssertTrue (@($wifi).Count -ge 1 -and $wifi[0].data.status -eq "fail") "PC wifi_result 非 fail"
    $dwifi = Wait-Events $ctx.devEvents "wifi_result" "device" 1 25000
    AssertTrue (@($dwifi).Count -ge 1 -and $dwifi[0].data.status -eq "fail") "设备 wifi_result 非 fail"
    [void]$checks.Add("双方 wifi_result=fail（pc reason=$($wifi[0].data.reason)，dev reason=$($dwifi[0].data.reason)）")

    $close = @(Read-Events $ctx.pcEvents | Where-Object { $_.event -eq "close_ap_sent" })
    AssertTrue ($close.Count -eq 0) "wifi_result fail 后不应发送 close_ap"
    [void]$checks.Add("无 close_ap_sent 事件")

    $pcOnline = @(Read-Events $ctx.pcEvents | Where-Object { $_.event -eq "session_online" })
    $devOnline = @(Read-Events $ctx.devEvents | Where-Object { $_.event -eq "session_online" })
    AssertTrue ($pcOnline.Count -eq 0 -and $devOnline.Count -eq 0) "wifi 失败后不应出现 session_online"
    [void]$checks.Add("无 session_online（未错误进入会话）")

    $catalogSeen = $false
    for ($i = 0; $i -lt 30; $i++) {
        if (Get-ChildItem -LiteralPath $ctx.catalog -Filter "device-0.json" -ErrorAction SilentlyContinue) { $catalogSeen = $true; break }
        Start-Sleep -Milliseconds 300
    }
    AssertTrue $catalogSeen "设备配网热点 catalog 未发布"
    [void]$checks.Add("设备保持可配网状态（catalog device-0.json 在配网失败后仍存在）")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "f" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 g：双向 app_data
# ---------------------------------------------------------------------------
function Invoke-ScenarioG([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "g_app_data.json"
    $ctx = Start-DualProcess $dir $scn 30 30

    $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
    AssertTrue (@($pcOnline).Count -ge 1) "session_online（PC）缺失"

    function Wait-ScenarioResult([string]$path, [string]$id, [int]$timeoutMs) {
        $deadline = (Get-Date).AddMilliseconds($timeoutMs)
        while ((Get-Date) -lt $deadline) {
            $ev = Read-Events $path | Where-Object { $_.event -eq "scenario_result" -and $_.data.action_id -eq $id } | Select-Object -First 1
            if ($ev) { return $ev }
            Start-Sleep -Milliseconds 200
        }
        return $null
    }
    function Wait-DataEvent([string]$path, [string]$name, [string]$sha, [int]$timeoutMs) {
        $deadline = (Get-Date).AddMilliseconds($timeoutMs)
        while ((Get-Date) -lt $deadline) {
            $ev = Read-Events $path | Where-Object { $_.event -eq $name -and $_.data.sha256 -eq $sha } | Select-Object -First 1
            if ($ev) { return $ev }
            Start-Sleep -Milliseconds 200
        }
        return $null
    }

    $text512pc = "P" * 512
    $text512dev = "D" * 512
    $text513pc = "Q" * 513
    $text513dev = "E" * 513
    $textUtf8Pc = "模组科技网络双向数据一致性测试-UTF8多字节-0123456789-中文编码验证-abcdefghijklmnopqrstuvwxyz"
    $textUtf8Dev = "设备端UTF8回传：压强传感数据帧边界测试-协议互操作-2026-双向发送"

    $r = Wait-ScenarioResult $ctx.pcEvents "g_pc_512" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "ok") "g_pc_512 未 ok"
    $sha = Sha256Hex $text512pc
    $tx = Wait-DataEvent $ctx.pcEvents "app_data_tx" $sha 10000
    $rx = Wait-DataEvent $ctx.devEvents "app_data_rx" $sha 10000
    AssertTrue ($null -ne $tx) "PC app_data_tx(512) sha256 不符"
    AssertTrue ($null -ne $rx) "设备 app_data_rx(512) sha256 不符"
    [void]$checks.Add("PC->设备 512 字节：tx/rx sha256 一致（$($sha.Substring(0,16))...）")

    $r = Wait-ScenarioResult $ctx.devEvents "g_dev_512" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "ok") "g_dev_512 未 ok"
    $sha = Sha256Hex $text512dev
    $tx = Wait-DataEvent $ctx.devEvents "app_data_tx" $sha 10000
    $rx = Wait-DataEvent $ctx.pcEvents "app_data_rx" $sha 10000
    AssertTrue ($null -ne $tx) "设备 app_data_tx(512) sha256 不符"
    AssertTrue ($null -ne $rx) "PC app_data_rx(512) sha256 不符"
    [void]$checks.Add("设备->PC 512 字节：tx/rx sha256 一致（$($sha.Substring(0,16))...）")

    $r = Wait-ScenarioResult $ctx.pcEvents "g_pc_513" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "rejected" -and $r.data.request_bytes -eq 513 -and $r.data.reason -eq "payload_too_large") "g_pc_513 未按 payload_too_large 拒绝"
    $sha513 = Sha256Hex $text513pc
    $badTx = Wait-DataEvent $ctx.pcEvents "app_data_tx" $sha513 3000
    AssertTrue ($null -eq $badTx) "513 字节不应产生 app_data_tx"
    [void]$checks.Add("PC 513 字节：status=rejected / request_bytes=513 / reason=payload_too_large，无 tx")

    $r = Wait-ScenarioResult $ctx.devEvents "g_dev_513" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "rejected" -and $r.data.request_bytes -eq 513 -and $r.data.reason -eq "payload_too_large") "g_dev_513 未按 payload_too_large 拒绝"
    $sha513d = Sha256Hex $text513dev
    $badTx = Wait-DataEvent $ctx.devEvents "app_data_tx" $sha513d 3000
    AssertTrue ($null -eq $badTx) "513 字节不应产生设备 app_data_tx"
    [void]$checks.Add("设备 513 字节：status=rejected / request_bytes=513 / reason=payload_too_large，无 tx")

    $r = Wait-ScenarioResult $ctx.pcEvents "g_pc_utf8" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "ok") "g_pc_utf8 未 ok"
    $sha = Sha256Hex $textUtf8Pc
    $tx = Wait-DataEvent $ctx.pcEvents "app_data_tx" $sha 10000
    $rx = Wait-DataEvent $ctx.devEvents "app_data_rx" $sha 10000
    AssertTrue ($null -ne $tx -and $null -ne $rx) "PC->设备 UTF-8 内容 sha256 不一致"
    [void]$checks.Add("PC->设备 UTF-8 多字节：tx/rx sha256 一致（$($sha.Substring(0,16))...）")

    $r = Wait-ScenarioResult $ctx.devEvents "g_dev_utf8" 20000
    AssertTrue ($null -ne $r -and $r.data.status -eq "ok") "g_dev_utf8 未 ok"
    $sha = Sha256Hex $textUtf8Dev
    $tx = Wait-DataEvent $ctx.devEvents "app_data_tx" $sha 10000
    $rx = Wait-DataEvent $ctx.pcEvents "app_data_rx" $sha 10000
    AssertTrue ($null -ne $tx -and $null -ne $rx) "设备->PC UTF-8 内容 sha256 不一致"
    [void]$checks.Add("设备->PC UTF-8 多字节：tx/rx sha256 一致（$($sha.Substring(0,16))...）")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "g" $ok $checks.ToArray() ""
    return $ok
}

# ---------------------------------------------------------------------------
# 场景 h：帧边界与协议版本（尽力而为）
# ---------------------------------------------------------------------------
function Invoke-ScenarioH([string]$dir) {
    $checks = New-Object System.Collections.ArrayList
    $scn = Join-Path $script:ScenariosDir "h_frame_probe.json"
    $ctx = Start-DualProcess $dir $scn 20 20

    $devReady = Wait-Events $ctx.devEvents "ready" "device" 1 15000
    $pcReady = Wait-Events $ctx.pcEvents "ready" "pc" 1 15000
    AssertTrue (@($devReady).Count -ge 1 -and @($pcReady).Count -ge 1) "ready 缺失"
    $ap = Wait-Events $ctx.devEvents "ap_ready" "device" 1 15000
    AssertTrue (@($ap).Count -ge 1) "ap_ready 缺失"
    Start-Sleep -Seconds 1

    function Send-Frame([System.Net.Sockets.TcpClient]$client, [byte[]]$payload) {
        $len = $payload.Length
        $hdr = [byte[]]@((($len -shr 8) -band 0xFF), ($len -band 0xFF))
        $stream = $client.GetStream()
        $stream.Write($hdr, 0, 2)
        if ($len -gt 0) { $stream.Write($payload, 0, $len) }
        $stream.Flush()
    }
    function Read-Frame([System.Net.Sockets.TcpClient]$client, [int]$timeoutMs) {
        $stream = $client.GetStream()
        $stream.ReadTimeout = $timeoutMs
        $hdr = New-Object byte[] 2
        $got = 0
        while ($got -lt 2) {
            $n = $stream.Read($hdr, $got, 2 - $got)
            if ($n -le 0) { return $null }
            $got += $n
        }
        $len = (($hdr[0] -shl 8) -bor $hdr[1])
        $buf = New-Object byte[] $len
        $got = 0
        while ($got -lt $len) {
            $n = $stream.Read($buf, $got, $len - $got)
            if ($n -le 0) { return $null }
            $got += $n
        }
        return ([System.Text.Encoding]::UTF8.GetString($buf))
    }
    function Try-Connect([string]$ip, [int]$port) {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect($ip, $port)
        return $c
    }

    # --- 设备配网服务（127.0.0.1:20000） ---
    $authBad = [System.Text.Encoding]::UTF8.GetBytes('{"cmd":"auth","pin":"0000"}')
    $full = [byte[]]@(0, $authBad.Length) + $authBad

    # 连接 A：拆包/粘包 —— 一个合法帧分两次写入，仍被完整解析
    $probe = Try-Connect "127.0.0.1" 20000
    $half = [Math]::Floor($full.Length / 2)
    $s = $probe.GetStream()
    $s.Write($full, 0, $half); $s.Flush()
    Start-Sleep -Milliseconds 150
    $s.Write($full, $half, $full.Length - $half); $s.Flush()
    $resp = Read-Frame $probe 5000
    AssertTrue ($null -ne $resp) "拆包发送 auth 后无响应"
    $j = $resp | ConvertFrom-Json
    AssertTrue ($j.cmd -eq "auth_result" -and $j.status -eq "fail") "auth_result 期望 fail（pin=0000），实际 $resp"
    $probe.Close()
    [void]$checks.Add("拆包/粘包帧解析：分两次写出的 auth 帧被正确解析（auth_result fail）")

    # 连接 B：超长帧头（声明 1025 字节负载）被拒绝且不崩溃；随后合法帧仍可解析
    $probe = Try-Connect "127.0.0.1" 20000
    $s = $probe.GetStream()
    $s.Write([byte[]]@(0x04, 0x01), 0, 2); $s.Flush()
    Start-Sleep -Milliseconds 150
    $s.Write($full, 0, $full.Length); $s.Flush()
    $resp = Read-Frame $probe 5000
    AssertTrue ($null -ne $resp) "超长帧后连接应仍可用并响应合法帧"
    $j = $resp | ConvertFrom-Json
    AssertTrue ($j.cmd -eq "auth_result" -and $j.status -eq "fail") "超长帧后 auth_result 期望 fail"
    $probe.Close()
    [void]$checks.Add("1025 字节超长帧头被拒绝（畸形计数），连接未崩溃且继续工作")

    # 0 长度帧 × 3 → 畸形超限，连接被设备关闭且进程存活
    $probe2 = Try-Connect "127.0.0.1" 20000
    $s2 = $probe2.GetStream()
    for ($i = 0; $i -lt 3; $i++) {
        $s2.Write([byte[]]@(0, 0), 0, 2); $s2.Flush()
        Start-Sleep -Milliseconds 200
    }
    $closed = $false
    try {
        $s2.ReadTimeout = 3000
        $b = New-Object byte[] 16
        $n = $s2.Read($b, 0, 16)
        $closed = ($n -le 0)
    } catch { $closed = $true }
    $probe2.Close()
    AssertTrue $closed "连续 3 个 0 长度帧后连接应被设备关闭（malformed_max）"
    [void]$checks.Add("0 长度帧 ×3：设备按 malformed_max 关闭连接且不崩溃")

    # --- PC 业务端口（127.0.0.1:5935）协议版本拒绝 ---
    $hello = '{"cmd":"device_hello","id":"02:00:00:00:00:ff","type":"pressure_sensor","fw_version":"1.0.0","proto_ver":2,"capabilities":["pressure"],"uptime":1}'
    $c = Try-Connect "127.0.0.1" 5935
    Send-Frame $c ([System.Text.Encoding]::UTF8.GetBytes($hello))
    $resp = Read-Frame $c 5000
    AssertTrue ($null -ne $resp) "proto_ver=2 的 device_hello 无响应"
    $j = $resp | ConvertFrom-Json
    AssertTrue ($j.cmd -eq "host_ack" -and $j.status -eq "fail") "proto_ver=2 应被拒绝（host_ack fail），实际 $resp"
    $c.Close()
    [void]$checks.Add("PC 拒绝 proto_ver=2（host_ack status=fail）")

    $helloOk = '{"cmd":"device_hello","id":"02:00:00:00:00:fe","type":"pressure_sensor","fw_version":"1.0.0","proto_ver":1,"capabilities":["pressure"],"uptime":1}'
    $c2 = Try-Connect "127.0.0.1" 5935
    Send-Frame $c2 ([System.Text.Encoding]::UTF8.GetBytes($helloOk))
    $resp = Read-Frame $c2 5000
    AssertTrue ($null -ne $resp) "proto_ver=1 的 device_hello 无响应"
    $j = $resp | ConvertFrom-Json
    AssertTrue ($j.cmd -eq "host_ack" -and $j.status -eq "ok") "proto_ver=1 应被接受（host_ack ok），实际 $resp"
    $c2.Close()
    [void]$checks.Add("PC 接受 proto_ver=1（host_ack status=ok，1/1024 字节帧互操作基线）")

    $fin = Finish-DualProcess $ctx ($TimeoutSeconds + 10)
    $checks.AddRange($fin.Checks)
    $ok = ($fin.PcCode -eq 0) -and ($fin.DevCode -eq 0) -and
          ($fin.PcLast -eq "shutdown_complete") -and ($fin.DevLast -eq "shutdown_complete")
    Write-Summary $dir "h" $ok $checks.ToArray() "尽力而为场景（原始 socket 探针）"
    return $ok
}

# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
$script:RunLog = Join-Path $script:RunRoot "runner.log"

AssertTrue ($PcExe -ne "") "必须提供 -PcExe"
AssertTrue ($DeviceExe -ne "") "必须提供 -DeviceExe"
AssertTrue (Test-Path -LiteralPath $PcExe) "PC 可执行文件不存在: $PcExe"
AssertTrue (Test-Path -LiteralPath $DeviceExe) "设备可执行文件不存在: $DeviceExe"

$scenarioNames = @("a", "b", "c", "d", "e", "f", "g", "h")
$runList = if ($Scenario -eq "all") { $scenarioNames } else { @($Scenario -split "," | ForEach-Object { $_.Trim() }) }

foreach ($name in $runList) {
    if ($scenarioNames -notcontains $name) { throw "未知场景: $name（可选 a,b,c,d,e,f,g,h 或 all）" }
}

$failed = @()
foreach ($name in $runList) {
    $dir = New-TempRunDir $name
    Log "==== 场景 $name @ $dir ===="
    $pass = $false
    try {
        switch ($name) {
            "a" { $pass = Invoke-ScenarioA $dir }
            "b" { $pass = Invoke-ScenarioB $dir }
            "c" { $pass = Invoke-ScenarioC $dir }
            "d" { $pass = Invoke-ScenarioD $dir }
            "e" { $pass = Invoke-ScenarioE $dir }
            "f" { $pass = Invoke-ScenarioF $dir }
            "g" { $pass = Invoke-ScenarioG $dir }
            "h" { $pass = Invoke-ScenarioH $dir }
        }
    } catch {
        Log "  场景 $name 异常: $_"
        $pass = $false
        try {
            foreach ($p in @(Get-Process provision_pc, provision_device -ErrorAction SilentlyContinue)) {
                if ($p.ProcessName -match "provision") { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
            }
        } catch { }
    }
    if ($pass) { Log "  场景 $name 通过" } else { Log "  场景 $name 失败"; $failed += $name }
}

$summary = [ordered]@{
    timestamp = (Get-Date -Format "o")
    passed = ($runList | Where-Object { $_ -notin $failed })
    failed = $failed
    runtime_root = $script:RunRoot
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $script:RunRoot "integration-summary.json") -Encoding UTF8

if ($failed.Count -gt 0) {
    Log "失败场景: $($failed -join ', ')"
    exit 1
}
Log "全部场景通过"
exit 0
