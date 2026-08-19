#requires -Version 5.1
<#
.SYNOPSIS
    跨端集成测试 runner（PowerShell 5.1+，无第三方依赖）。
    启动已构建的 provision_pc / provision_device 双进程，以公开 CLI、
    事件 JSONL、scenario 文件与退出码完成协议互操作验收。

.DESCRIPTION
    只消费两端已构建产物（可执行文件、config、share/protocol-contract.json）。
    禁止 include/编译/链接任何端侧源码；本脚本不复制、不引用端侧生产源码。

    beta v1.1 拓扑：PC 发布热点（sim 写 pc-hotspot.json catalog）→ 设备扫描/
    连接固定地址 192.168.137.1:5935 → 握手 → 心跳/应用数据。

    场景清单（对应 beta v1.1 设计文档第 15 节）：
      a  protocol manifest 一致性（三份 schema2 contract 语义比较）
      b  PC hotspot_ready -> 设备扫描/STA/直连 -> 双方 session_online -> ping/pong
      c  PC request_stop -> catalog 删除 -> 设备 session_offline；无残留
      d  sock_send_fail 注入 -> 双方离线 -> 恢复 -> 固定地址重连，reconnect_count 增长
      e  第二设备被拒（host_ack busy / single_device_only），第一设备保持在线
      f  目标 SSID 不存在：无 session_online、无配网/热点事件、设备持续扫描
      g  双向 app_data：512 成功、513 rejected、UTF-8 一致
      h  帧边界与协议版本（拆包/超长/空帧/版本拒绝，原始 socket 探针，PC-only）

    用法示例：
      powershell -ExecutionPolicy Bypass -File run_scenario.ps1 -Scenario all `
          -PcExe out\pc-build\bin\Debug\provision_pc.exe `
          -DeviceExe out\device-build\bin\Debug\provision_device.exe `
          -PcContract demo\pc\share\protocol-contract.json `
          -DeviceContract out\device-build\share\protocol-contract.json `
          -RuntimeRoot out\v11-integration-evidence -TimeoutSeconds 60 `
          -QtBin D:/Devtools/Qt/6.8.3/msvc2022_64/bin
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
    [int]$TimeoutSeconds = 60,
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

function Log([string]$msg) {
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $msg
    if (-not $Quiet) { Write-Host $line }
    Add-Content -LiteralPath $script:RunLog -Value $line -Encoding UTF8
}

function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

function Assert([System.Collections.ArrayList]$list, [string]$name, [bool]$cond, [string]$evidence) {
    $status = if ($cond) { "pass" } else { "fail" }
    [void]$list.Add([ordered]@{ name = $name; status = $status; evidence = $evidence })
    if (-not $cond) { throw "断言失败: $name （$evidence）" }
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
    $evs = @()
    while ((Get-Date) -lt $deadline) {
        $evs = @(Read-Events $path | Where-Object { $_.event -eq $name -and ($role -eq "" -or $_.role -eq $role) })
        if ($evs.Count -ge $count) { return $evs }
        Start-Sleep -Milliseconds 200
    }
    return $evs
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

function Wait-ScenarioResult([string]$path, [string]$id, [int]$timeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $ev = Read-Events $path | Where-Object { $_.event -eq "scenario_result" -and $_.data.action_id -eq $id } | Select-Object -First 1
        if ($ev) { return $ev }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

# 寻找完整 ping/pong 回环：设备 ping(tx,seq) → PC ping(rx,seq) → PC pong(tx,seq) → 设备 pong(rx,seq)。
# $which: "first"（最早完整回环）或 "last"（最晚完整回环）。返回该回环的 ping 事件（含 seq）。
# 说明：设备首个 ping 可能与 hello 同段到达 PC，PC 在 pending→online 转换时会丢弃该段内
# 后续帧（端侧行为，良性——心跳 2 秒重发），因此必须按任意完整回环匹配，而非仅首个 ping。
function Find-PingPongPair($pcAll, $devAll, [string]$which) {
    $devPings = @($devAll | Where-Object { $_.event -eq "ping" -and $_.data.direction -eq "tx" })
    $pcPings = @($pcAll | Where-Object { $_.event -eq "ping" -and $_.data.direction -eq "rx" })
    $pcPongs = @($pcAll | Where-Object { $_.event -eq "pong" -and $_.data.direction -eq "tx" })
    $devPongs = @($devAll | Where-Object { $_.event -eq "pong" -and $_.data.direction -eq "rx" })
    $found = @()
    foreach ($p in $devPings) {
        $seq = [int]$p.data.sequence
        if (@($pcPings | Where-Object { [int]$_.data.sequence -eq $seq }).Count -ge 1 -and
            @($pcPongs | Where-Object { [int]$_.data.sequence -eq $seq }).Count -ge 1 -and
            @($devPongs | Where-Object { [int]$_.data.sequence -eq $seq }).Count -ge 1) {
            $found += $p
        }
    }
    if ($found.Count -eq 0) { return $null }
    if ($which -eq "last") { return $found[-1] }
    return $found[0]
}

# 轮询等待完整 ping/pong 回环出现（$timeoutMs 内）；$which: first/last。
# $afterTimeMs >= 0 时只接受 ping 时刻不早于 $afterTimeMs-2000 的回环（用于重连场景）。
function Wait-PingPongPair([string]$pcPath, [string]$devPath, [int]$timeoutMs, [string]$which, [double]$afterTimeMs) {
    if ($null -eq $afterTimeMs -or $afterTimeMs -lt 0) { $afterTimeMs = -1 }
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    while ((Get-Date) -lt $deadline) {
        $pcAll = Read-Events $pcPath
        $devAll = Read-Events $devPath
        $pair = Find-PingPongPair $pcAll $devAll $which
        if ($null -ne $pair -and ($afterTimeMs -lt 0 -or $pair.time_ms -ge ($afterTimeMs - 2000))) {
            return $pair
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

function Write-Summary([string]$dir, [string]$scenario, [bool]$pass, [System.Collections.ArrayList]$assertions, [string]$note) {
    $sum = [ordered]@{
        scenario = $scenario
        pass = $pass
        timestamp = (Get-Date -Format "o")
        assertions = $assertions.ToArray()
        note = $note
    }
    Write-Utf8NoBom (Join-Path $dir "summary.json") ($sum | ConvertTo-Json -Depth 8)
}

# ---------------------------------------------------------------------------
# 配置生成（beta v1.1 字段）
# ---------------------------------------------------------------------------
function New-Configs([string]$dir, [int]$pcDuration, [int]$devDuration, [string]$deviceSsid) {
    $cfgDir = Join-Path $dir "configs"
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null

    if ($PcConfig) {
        Copy-Item -LiteralPath $PcConfig -Destination (Join-Path $cfgDir "pc_config.json") -Force
    } else {
        $pc = [ordered]@{
            host_tcp_port = 5935
            pc_ap_ssid = "Modu_PC"
            pc_ap_password = "modu_leventure"
            pc_ap_ip = "192.168.137.1"
            pc_ap_prefix_length = 24
            heartbeat_interval_ms = 2000
            heartbeat_dead_ms = 0
            hello_timeout_ms = 5000
            malformed_max_per_conn = 3
            device_rate_limit_per_sec = 50
            duration_s = $pcDuration
            config_tag = "integration-pc"
        }
        Write-Utf8NoBom (Join-Path $cfgDir "pc_config.json") ($pc | ConvertTo-Json -Depth 5)
    }
    if ($DeviceConfig) {
        Copy-Item -LiteralPath $DeviceConfig -Destination (Join-Path $cfgDir "device_sim.json") -Force
    } else {
        $dev = [ordered]@{
            power_on_jitter_max_ms = 500
            wifi_retry_max = 5
            wifi_backoff_base_ms = 1000
            wifi_backoff_cap_ms = 5000
            wifi_backoff_jitter_ms = 500
            heartbeat_interval_ms = 2000
            heartbeat_dead_ms = 0
            busy_backoff_ms = 60000
            hello_timeout_ms = 5000
            reconnect_backoff_base_ms = 1000
            reconnect_backoff_cap_ms = 5000
            reconnect_backoff_jitter_ms = 500
            malformed_max_per_conn = 3
            device_rate_limit_per_sec = 50
            host_tcp_port = 5935
            pc_ap_ssid = $deviceSsid
            pc_ap_password = "modu_leventure"
            pc_host_ip = "192.168.137.1"
            nvs_dir = "run"
            use_real_wifi_sta = 0
            device_fw_version = "1.1.0"
            device_proto_ver = 1
            duration_s = $devDuration
            config_tag = "integration-device"
        }
        Write-Utf8NoBom (Join-Path $cfgDir "device_sim.json") ($dev | ConvertTo-Json -Depth 5)
    }
    $pcCfgPath = (Resolve-Path -LiteralPath (Join-Path $cfgDir "pc_config.json")).Path
    $devCfgPath = (Resolve-Path -LiteralPath (Join-Path $cfgDir "device_sim.json")).Path
    return @{ PcCfg = $pcCfgPath; DevCfg = $devCfgPath; CfgDir = $cfgDir }
}

function Copy-Scenario([string]$dir, [string]$scenarioFile) {
    $scnCopy = Join-Path $dir "scenario.json"
    Copy-Item -LiteralPath $scenarioFile -Destination $scnCopy -Force
    return $scnCopy
}

function Get-EmptyCwd([string]$dir) {
    $emptyCwd = Join-Path $dir "empty-cwd"
    New-Item -ItemType Directory -Path $emptyCwd -Force | Out-Null
    return $emptyCwd
}

# 启动 PC（sim），等待 hotspot_ready 事件后再返回
function Start-Pc([string]$dir, [string]$scnCopy, [int]$pcDuration, [string]$logPrefix) {
    $catalog = Join-Path $dir "sim-catalog"
    $pcEvents = Join-Path $dir "pc-events.jsonl"
    $pcRuntime = Join-Path $dir "pc-runtime"
    $cfg = New-Configs $dir $pcDuration 1 "Modu_PC"
    $emptyCwd = Get-EmptyCwd $dir

    $pcSide = Start-SideProcess $PcExe @(
        "--config", $cfg.PcCfg,
        "--backend", "sim",
        "--runtime-dir", $pcRuntime,
        "--sim-catalog-dir", $catalog,
        "--log-dir", (Join-Path $dir "logs"),
        "--events-jsonl", $pcEvents,
        "--scenario", $scnCopy,
        "--duration", "$pcDuration"
    ) (Join-Path $dir "pc-stdout.log") (Join-Path $dir "pc-stderr.log") $pcDuration $emptyCwd

    $hot = Wait-Events $pcEvents "hotspot_ready" "pc" 1 15000
    if (@($hot).Count -lt 1) {
        Stop-SideProcess $pcSide 2000
        Collect-SideOutputs $pcSide
        throw "PC 未在 15 秒内产生 hotspot_ready（log=$logPrefix）"
    }
    return @{
        dir = $dir; catalog = $catalog; pcEvents = $pcEvents
        pcSide = $pcSide; pcRuntime = $pcRuntime
        pcDuration = $pcDuration
        DevSide = $null; DevEvents = ""
        Dev1Side = $null; Dev1Events = ""; Dev2Side = $null; Dev2Events = ""; Dev1Id = ""
    }
}

# 双进程：PC 先启动并等待 hotspot_ready，再启动设备（共享 sim-catalog-dir）
function Start-DualProcess([string]$dir, [string]$scenarioFile, [int]$pcDuration, [int]$devDuration, [string]$deviceSsid) {
    if (-not $deviceSsid) { $deviceSsid = "Modu_PC" }
    $scnCopy = Copy-Scenario $dir $scenarioFile
    $ctx = Start-Pc $dir $scnCopy $pcDuration "dual"

    $devEvents = Join-Path $dir "device-events.jsonl"
    $devRuntime = Join-Path $dir "device-runtime"
    $catalog = $ctx.catalog
    $cfg = New-Configs $dir $pcDuration $devDuration $deviceSsid
    $emptyCwd = Get-EmptyCwd $dir

    $devSide = Start-SideProcess $DeviceExe @(
        "--config", $cfg.DevCfg,
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

    $ctx.DevSide = $devSide
    $ctx.DevEvents = $devEvents
    $ctx.DevRuntime = $devRuntime
    $ctx.DevDuration = $devDuration
    return $ctx
}

# 三进程（场景 e）：PC → 设备1（等待 session_online）→ 设备2
function Start-TripleProcess([string]$dir, [string]$scenarioFile, [int]$pcDuration, [int]$devDuration) {
    $scnCopy = Copy-Scenario $dir $scenarioFile
    $ctx = Start-Pc $dir $scnCopy $pcDuration "triple"

    $catalog = $ctx.catalog
    $emptyCwd = Get-EmptyCwd $dir
    $cfg = New-Configs $dir $pcDuration $devDuration "Modu_PC"

    $dev1Events = Join-Path $dir "dev1-events.jsonl"
    $dev1Side = Start-SideProcess $DeviceExe @(
        "--config", $cfg.DevCfg, "--backend", "sim", "--device-index", "0", "--fresh",
        "--runtime-dir", (Join-Path $dir "device1-runtime"),
        "--sim-catalog-dir", $catalog, "--log-dir", (Join-Path $dir "logs"),
        "--events-jsonl", $dev1Events, "--scenario", $scnCopy, "--duration", "$devDuration"
    ) (Join-Path $dir "dev1-stdout.log") (Join-Path $dir "dev1-stderr.log") $devDuration $emptyCwd

    # 等待设备1在线后再启动设备2（保证设备2被 busy 拒绝而非抢线）
    $on1 = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
    if (@($on1).Count -lt 1) {
        Stop-SideProcess $dev1Side 2000
        Stop-SideProcess $ctx.pcSide 2000
        Collect-SideOutputs $dev1Side
        Collect-SideOutputs $ctx.pcSide
        throw "设备1 未在 30 秒内上线"
    }
    $dev1Id = $on1[0].device_id

    $dev2Events = Join-Path $dir "dev2-events.jsonl"
    $dev2Side = Start-SideProcess $DeviceExe @(
        "--config", $cfg.DevCfg, "--backend", "sim", "--device-index", "1", "--fresh",
        "--runtime-dir", (Join-Path $dir "device2-runtime"),
        "--sim-catalog-dir", $catalog, "--log-dir", (Join-Path $dir "logs"),
        "--events-jsonl", $dev2Events, "--scenario", $scnCopy, "--duration", "$devDuration"
    ) (Join-Path $dir "dev2-stdout.log") (Join-Path $dir "dev2-stderr.log") $devDuration $emptyCwd

    $ctx.Dev1Side = $dev1Side; $ctx.Dev1Events = $dev1Events; $ctx.Dev1Id = $dev1Id
    $ctx.Dev2Side = $dev2Side; $ctx.Dev2Events = $dev2Events
    return $ctx
}

# 结束双进程：等待退出 → 停止/收集输出 → 返回事实
function Finish-Processes($ctx, [string[]]$sideKeys, [string[]]$eventKeys, [string[]]$names, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $allExited = $true
        foreach ($k in $sideKeys) { if (-not $ctx.$k.Process.HasExited) { $allExited = $false; break } }
        if ($allExited) { break }
        Start-Sleep -Milliseconds 300
    }
    foreach ($k in $sideKeys) { Stop-SideProcess $ctx.$k 3000 }
    foreach ($k in $sideKeys) { Collect-SideOutputs $ctx.$k }

    $facts = [ordered]@{}
    for ($i = 0; $i -lt $sideKeys.Count; $i++) {
        $sk = $sideKeys[$i]; $ek = $eventKeys[$i]; $nm = $names[$i]
        $p = $ctx.$sk.Process
        $code = if ($p.HasExited) { $p.ExitCode } else { -999 }
        $last = (Read-Events $ctx.$ek | Select-Object -Last 1)
        $alive = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
        $facts["${nm}Code"] = $code
        $facts["${nm}Last"] = if ($last) { $last.event } else { "none" }
        $facts["${nm}Alive"] = ($null -ne $alive)
        $facts["${nm}Pid"] = $p.Id
    }
    $catalogFiles = @(Get-ChildItem -LiteralPath $ctx.catalog -Filter *.json -ErrorAction SilentlyContinue)
    $facts.CatalogFiles = $catalogFiles.Count
    return $facts
}

function Add-CleanupAssertions([System.Collections.ArrayList]$asserts, $facts, [string[]]$names, [bool]$checkPort5935) {
    foreach ($nm in $names) {
        $code = $facts["${nm}Code"]
        $last = $facts["${nm}Last"]
        $alive = $facts["${nm}Alive"]
        $procId = $facts["${nm}Pid"]
        Assert $asserts "$nm 退出码为 0" ($code -eq 0) "exit=$code"
        Assert $asserts "$nm 最后事件为 shutdown_complete" ($last -eq "shutdown_complete") "last=$last"
        Assert $asserts "无残留 $nm 进程" (-not $alive) "pid=$procId"
    }
    Assert $asserts "无残留 catalog 文件（pc-hotspot.json）" ($facts.CatalogFiles -eq 0) "files=$($facts.CatalogFiles)"
    if ($checkPort5935) {
        Assert $asserts "TCP 5935 可重绑（无端口残留）" (Test-PortRebindable 5935) "rebind=OK"
    }
}

# ---------------------------------------------------------------------------
# 场景 a：manifest 一致性
# ---------------------------------------------------------------------------
function Invoke-ScenarioA([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    try {
        $pcContract = Find-Contract $PcExe $PcContract
        $devContract = Find-Contract $DeviceExe $DeviceContract
        Assert $asserts "找到 PC protocol-contract.json" ($pcContract -ne "") "-PcContract 或 exe 旁 share/ 未找到"
        Assert $asserts "找到设备 protocol-contract.json" ($devContract -ne "") "-DeviceContract 或 exe 旁 share/ 未找到"
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
        Write-Utf8NoBom (Join-Path $dir "protocol-diff.json") ($diff | ConvertTo-Json -Depth 8)

        Assert $asserts "三份契约 schema_version 均为 2" `
            ($pc.schema_version -eq 2 -and $dev.schema_version -eq 2 -and $golden.schema_version -eq 2) `
            "pc=$($pc.schema_version) dev=$($dev.schema_version) golden=$($golden.schema_version)"
        Assert $asserts "PC contract == golden（忽略 generated_from）" ($d1.Count -eq 0) "diffs=$($d1.Count)"
        Assert $asserts "Device contract == golden（忽略 generated_from）" ($d2.Count -eq 0) "diffs=$($d2.Count)"
        Assert $asserts "PC contract == Device contract" ($d3.Count -eq 0) "diffs=$($d3.Count)"
        $pass = $true
    } catch {
        Log "  场景 a 异常: $_"
    }
    Write-Summary $dir "a" $pass $asserts "pc=$pcContract device=$devContract golden=$($script:Golden)"
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 b：热点 → 设备扫描/STA/直连 → 会话 → ping/pong
# ---------------------------------------------------------------------------
function Invoke-ScenarioB([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "b_hotspot_session.json"
        $ctx = Start-DualProcess $dir $scn 25 25 "Modu_PC"

        # 先等待关键事件就位，再读完整事件流断言顺序（避免读盘竞态）
        $pcReady = Wait-Events $ctx.pcEvents "ready" "pc" 1 15000
        Assert $asserts "PC ready 出现" (@($pcReady).Count -ge 1) $(if (@($pcReady).Count -ge 1) { "seq=$($pcReady[0].seq)" } else { "缺失" })
        $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 25000
        Assert $asserts "PC session_online 出现" (@($pcOnline).Count -ge 1) $(if (@($pcOnline).Count -ge 1) { "seq=$($pcOnline[0].seq)" } else { "缺失" })
        $devOnline = Wait-Events $ctx.DevEvents "session_online" "device" 1 25000
        Assert $asserts "设备 session_online 出现" (@($devOnline).Count -ge 1) $(if (@($devOnline).Count -ge 1) { "seq=$($devOnline[0].seq)" } else { "缺失" })

        # PC 顺序：hotspot_ready → tcp_listening → ready
        $pcEvs = Read-Events $ctx.pcEvents
        $hs = $pcEvs | Where-Object { $_.event -eq "hotspot_ready" } | Select-Object -First 1
        $tc = $pcEvs | Where-Object { $_.event -eq "tcp_listening" } | Select-Object -First 1
        $rd = $pcEvs | Where-Object { $_.event -eq "ready" } | Select-Object -First 1
        Assert $asserts "PC hotspot_ready 存在" ($null -ne $hs) $(if ($null -ne $hs) { "seq=$($hs.seq)" } else { "缺失" })
        Assert $asserts "PC tcp_listening 存在" ($null -ne $tc) $(if ($null -ne $tc) { "seq=$($tc.seq)" } else { "缺失" })
        Assert $asserts "PC ready 存在" ($null -ne $rd) $(if ($null -ne $rd) { "seq=$($rd.seq)" } else { "缺失" })
        Assert $asserts "PC 事件顺序 hotspot_ready→tcp_listening→ready" `
            ($null -ne $hs -and $null -ne $tc -and $null -ne $rd -and $hs.seq -lt $tc.seq -and $tc.seq -lt $rd.seq) `
            "seq hs=$($hs.seq) tc=$($tc.seq) rd=$($rd.seq)"
        Assert $asserts "hotspot_ready 数据 ssid=Modu_PC/ip=192.168.137.1/prefix=24" `
            ($null -ne $hs -and $hs.data.ssid -eq "Modu_PC" -and $hs.data.ip -eq "192.168.137.1" -and $hs.data.prefix_length -eq 24) `
            "data=$($hs.data | ConvertTo-Json -Compress)"

        # 设备顺序：ready → wifi_scan_started → wifi_target_found → wifi_connected → tcp_connecting → session_online
        $devEvs = Read-Events $ctx.DevEvents
        $order = @("ready", "wifi_scan_started", "wifi_target_found", "wifi_connected", "tcp_connecting", "session_online")
        $prevSeq = -1; $orderOk = $true; $seqs = @()
        foreach ($n in $order) {
            $ev = $devEvs | Where-Object { $_.event -eq $n } | Select-Object -First 1
            if ($null -eq $ev -or $ev.seq -le $prevSeq) { $orderOk = $false }
            $seqs += $(if ($ev) { "$n=$($ev.seq)" } else { "$n=缺失" })
            if ($ev) { $prevSeq = $ev.seq }
        }
        Assert $asserts "设备事件顺序 ready→scan→found→connected→connecting→online" $orderOk ($seqs -join " ")
        $wc = $devEvs | Where-Object { $_.event -eq "wifi_connected" } | Select-Object -First 1
        Assert $asserts "wifi_connected 数据 ssid=Modu_PC 且含 device_ip" `
            ($null -ne $wc -and $wc.data.ssid -eq "Modu_PC" -and $wc.data.device_ip) "data=$($wc.data | ConvertTo-Json -Compress)"
        $tc2 = $devEvs | Where-Object { $_.event -eq "tcp_connecting" } | Select-Object -First 1
        Assert $asserts "tcp_connecting 数据 host=192.168.137.1/port=5935" `
            ($null -ne $tc2 -and $tc2.data.host -eq "192.168.137.1" -and $tc2.data.port -eq 5935) "data=$($tc2.data | ConvertTo-Json -Compress)"

        # ping/pong：设备 session_online 后 10 秒内出现完整回环对
        $do = $devOnline[0]
        $pairEv = Wait-PingPongPair $ctx.pcEvents $ctx.DevEvents 12000 "first"
        $inWindow = $false
        if ($null -ne $pairEv -and $null -ne $do) {
            $inWindow = (($pairEv.time_ms - $do.time_ms) -le 10000)
        }
        Assert $asserts "session_online 后 10 秒内出现完整 ping/pong 回环（设备tx→PC rx→PC pong→设备pong rx）" `
            ($null -ne $pairEv -and $inWindow) `
            $(if ($null -ne $pairEv) { "pair_seq=$($pairEv.data.sequence) pair_time=$($pairEv.time_ms) online_time=$($do.time_ms)" } else { "无完整回环" })

        $facts = Finish-Processes $ctx @("pcSide", "DevSide") @("pcEvents", "DevEvents") @("PC", "Device") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC", "Device") $true
        $pass = $true
    } catch {
        Log "  场景 b 异常: $_"
        Log "  STACK: $($_.ScriptStackTrace)"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.DevSide) { Stop-SideProcess $ctx.DevSide 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.DevSide) { Collect-SideOutputs $ctx.DevSide }
    }
    Write-Summary $dir "b" $pass $asserts ""
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 c：PC 优雅退出 → catalog 删除 → 设备离线
# ---------------------------------------------------------------------------
function Invoke-ScenarioC([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "c_pc_stop.json"
        $ctx = Start-DualProcess $dir $scn 25 25 "Modu_PC"

        $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
        Assert $asserts "PC session_online 出现" (@($pcOnline).Count -ge 1) $(if (@($pcOnline).Count -ge 1) { "seq=$($pcOnline[0].seq)" } else { "缺失" })
        $devOnline = Wait-Events $ctx.DevEvents "session_online" "device" 1 30000
        Assert $asserts "设备 session_online 出现" (@($devOnline).Count -ge 1) $(if (@($devOnline).Count -ge 1) { "seq=$($devOnline[0].seq)" } else { "缺失" })

        $stop = Wait-ScenarioResult $ctx.pcEvents "c_pc_stop" 20000
        Assert $asserts "PC request_stop 动作执行（scenario_result ok）" ($null -ne $stop -and $stop.data.status -eq "ok") `
            $(if ($null -ne $stop) { "result=$($stop.data | ConvertTo-Json -Compress)" } else { "scenario_result 缺失" })

        # PC request_stop 后 catalog 删除
        $catGone = $false
        for ($i = 0; $i -lt 25; $i++) {
            if (-not (Test-Path -LiteralPath (Join-Path $ctx.catalog "pc-hotspot.json"))) { $catGone = $true; break }
            Start-Sleep -Milliseconds 200
        }
        Assert $asserts "PC 停止后 pc-hotspot.json catalog 被删除" $catGone "catalog 仍存在"

        $devOff = Wait-Events $ctx.DevEvents "session_offline" "device" 1 20000
        Assert $asserts "设备 session_offline 出现" (@($devOff).Count -ge 1) `
            $(if (@($devOff).Count -ge 1) { "reason=$($devOff[0].data.reason)" } else { "缺失" })

        # 设备不产生任何配网/热点事件（v1.1 无 AP 路径）
        $devAll = Read-Events $ctx.DevEvents
        $forbidden = @("ap_ready", "auth_result", "wifi_result", "close_ap_sent", "host_announce", "host_bye")
        $bad = @($devAll | Where-Object { $forbidden -contains $_.event })
        $badNames = @($bad | ForEach-Object { $_.event }) -join ","
        Assert $asserts "设备事件流不含配网/热点事件（ap_ready/auth/wifi_result/close_ap/host_announce/host_bye）" ($bad.Count -eq 0) "found=$badNames"

        $facts = Finish-Processes $ctx @("pcSide", "DevSide") @("pcEvents", "DevEvents") @("PC", "Device") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC", "Device") $true
        $pass = $true
    } catch {
        Log "  场景 c 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.DevSide) { Stop-SideProcess $ctx.DevSide 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.DevSide) { Collect-SideOutputs $ctx.DevSide }
    }
    Write-Summary $dir "c" $pass $asserts "注：sim 后端 STA 状态在 catalog 删除后仍保持有效，设备按设计文档 9.2 进入 HEAL 的 TCP 退避重连（不产生扫描事件），不属端侧缺陷"
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 d：TCP 故障注入 → 双方离线 → 恢复 → 固定地址重连
# ---------------------------------------------------------------------------
function Invoke-ScenarioD([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "d_reconnect.json"
        $ctx = Start-DualProcess $dir $scn 40 40 "Modu_PC"

        $on1pc = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
        $on1dev = Wait-Events $ctx.DevEvents "session_online" "device" 1 30000
        Assert $asserts "首次 session_online（PC）" (@($on1pc).Count -ge 1) $(if (@($on1pc).Count -ge 1) { "seq=$($on1pc[0].seq)" } else { "缺失" })
        Assert $asserts "首次 session_online（设备）" (@($on1dev).Count -ge 1) $(if (@($on1dev).Count -ge 1) { "seq=$($on1dev[0].seq)" } else { "缺失" })

        $fault = Wait-ScenarioResult $ctx.DevEvents "d_fault" 20000
        Assert $asserts "设备注入 sock_send_fail 成功" ($null -ne $fault -and $fault.data.status -eq "ok" -and $fault.data.action -eq "inject_fault") `
            $(if ($null -ne $fault) { "result=$($fault.data | ConvertTo-Json -Compress)" } else { "scenario_result 缺失" })

        $off1pc = Wait-Events $ctx.pcEvents "session_offline" "pc" 1 25000
        $off1dev = Wait-Events $ctx.DevEvents "session_offline" "device" 1 25000
        Assert $asserts "注入后 PC session_offline" (@($off1pc).Count -ge 1) $(if (@($off1pc).Count -ge 1) { "seq=$($off1pc[0].seq)" } else { "缺失" })
        Assert $asserts "注入后设备 session_offline" (@($off1dev).Count -ge 1) $(if (@($off1dev).Count -ge 1) { "seq=$($off1dev[0].seq)" } else { "缺失" })

        $restore = Wait-ScenarioResult $ctx.DevEvents "d_restore" 15000
        Assert $asserts "设备恢复 sock_send_fail（count=0）" ($null -ne $restore -and $restore.data.status -eq "ok") `
            $(if ($null -ne $restore) { "result=$($restore.data | ConvertTo-Json -Compress)" } else { "scenario_result 缺失" })

        # 30 秒内固定地址重连
        $on2pc = Wait-Events $ctx.pcEvents "session_online" "pc" 2 30000
        $on2dev = Wait-Events $ctx.DevEvents "session_online" "device" 2 30000
        Assert $asserts "PC 重连 session_online（第 2 次）" (@($on2pc).Count -ge 2) "count=$(@($on2pc).Count)"
        Assert $asserts "设备重连 session_online（第 2 次）" (@($on2dev).Count -ge 2) "count=$(@($on2dev).Count)"

        $rc1 = [int]$on1pc[0].data.reconnect_count
        $rc2 = [int]$on2pc[1].data.reconnect_count
        $dc1 = [int]$on1dev[0].data.reconnect_count
        $dc2 = [int]$on2dev[1].data.reconnect_count
        Assert $asserts "PC reconnect_count 增长" ($rc2 -gt $rc1) "pc $rc1 -> $rc2"
        Assert $asserts "设备 reconnect_count 增长" ($dc2 -gt $dc1) "dev $dc1 -> $dc2"

        $devAll = Read-Events $ctx.DevEvents
        $pcAll = Read-Events $ctx.pcEvents
        $pairEv = Wait-PingPongPair $ctx.pcEvents $ctx.DevEvents 15000 "last" $on2dev[1].time_ms
        $afterReconnect = $null -ne $pairEv
        Assert $asserts "重连后存在完整 ping/pong 回环" $afterReconnect `
            $(if ($null -ne $pairEv) { "pair_seq=$($pairEv.data.sequence) pair_time=$($pairEv.time_ms) online2_time=$($on2dev[1].time_ms)" } else { "无完整回环" })

        $facts = Finish-Processes $ctx @("pcSide", "DevSide") @("pcEvents", "DevEvents") @("PC", "Device") ($TimeoutSeconds + 15)
        Add-CleanupAssertions $asserts $facts @("PC", "Device") $true
        $pass = $true
    } catch {
        Log "  场景 d 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.DevSide) { Stop-SideProcess $ctx.DevSide 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.DevSide) { Collect-SideOutputs $ctx.DevSide }
    }
    Write-Summary $dir "d" $pass $asserts ""
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 e：第二设备被拒（single_device_only），第一设备保持在线
# ---------------------------------------------------------------------------
function Invoke-ScenarioE([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "e_second_device.json"
        $ctx = Start-TripleProcess $dir $scn 25 25

        $rej = Wait-Events $ctx.pcEvents "single_device_rejected" "pc" 1 30000
        Assert $asserts "PC 发出 single_device_rejected" (@($rej).Count -ge 1) $(if (@($rej).Count -ge 1) { "seq=$($rej[0].seq)" } else { "缺失" })
        Assert $asserts "single_device_rejected reason=single_device_only" `
            (@($rej).Count -ge 1 -and $rej[0].data.reason -eq "single_device_only") `
            $(if (@($rej).Count -ge 1) { "reason=$($rej[0].data.reason)" } else { "缺失" })

        $pcAll = Read-Events $ctx.pcEvents
        $dev2All = Read-Events $ctx.Dev2Events
        $dev1All = Read-Events $ctx.Dev1Events

        $dev1Online = @($pcAll | Where-Object { $_.event -eq "session_online" -and $_.device_id -eq $ctx.Dev1Id })
        Assert $asserts "设备1（$($ctx.Dev1Id)）在 PC 侧上线" ($dev1Online.Count -ge 1) "count=$($dev1Online.Count)"
        $firstRejSeq = ($rej | Select-Object -First 1).seq
        $firstOfflineForDev1 = $pcAll | Where-Object { $_.event -eq "session_offline" -and $_.device_id -eq $ctx.Dev1Id } | Select-Object -First 1
        $kept = $true
        if ($null -ne $firstOfflineForDev1) { $kept = ($firstOfflineForDev1.seq -gt $firstRejSeq) }
        $offEvidence = if ($null -ne $firstOfflineForDev1) { "first_offline_seq=$($firstOfflineForDev1.seq) reject_seq=$firstRejSeq" } else { "设备1 无提前离线（reject_seq=$firstRejSeq）" }
        Assert $asserts "拒绝发生时设备1仍在线（PC 侧无提前 session_offline）" $kept $offEvidence

        $dev2Online = @($dev2All | Where-Object { $_.event -eq "session_online" })
        Assert $asserts "设备2 从未 session_online（busy 拒绝）" ($dev2Online.Count -eq 0) "count=$($dev2Online.Count)"
        $dev2Off = @($dev2All | Where-Object { $_.event -eq "session_offline" })
        Assert $asserts "设备2 收到拒绝后 session_offline（busy 循环）" ($dev2Off.Count -ge 1) "count=$($dev2Off.Count)"

        $facts = Finish-Processes $ctx @("pcSide", "Dev1Side", "Dev2Side") @("pcEvents", "Dev1Events", "Dev2Events") @("PC", "Device1", "Device2") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC", "Device1", "Device2") $true
        $pass = $true
    } catch {
        Log "  场景 e 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.Dev1Side) { Stop-SideProcess $ctx.Dev1Side 2000 }
        if ($ctx.Dev2Side) { Stop-SideProcess $ctx.Dev2Side 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.Dev1Side) { Collect-SideOutputs $ctx.Dev1Side }
        if ($ctx.Dev2Side) { Collect-SideOutputs $ctx.Dev2Side }
    }
    Write-Summary $dir "e" $pass $asserts ""
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 f：目标 SSID 不存在 → 无会话、无配网/热点事件、持续扫描
# ---------------------------------------------------------------------------
function Invoke-ScenarioF([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "f_no_target.json"
        $ctx = Start-DualProcess $dir $scn 18 18 "Modu_Other"   # 目标 SSID 与 PC 发布的不一致

        $devScan = Wait-Events $ctx.DevEvents "wifi_scan_started" "device" 1 15000
        Assert $asserts "设备持续 wifi_scan_started" (@($devScan).Count -ge 1) "count=$(@($devScan).Count)"

        $devAll = Read-Events $ctx.DevEvents
        $pcAll = Read-Events $ctx.pcEvents
        $never = @("wifi_target_found", "wifi_connected", "tcp_connecting", "wifi_connect_failed", "session_online")
        foreach ($n in $never) {
            $cnt = @($devAll | Where-Object { $_.event -eq $n })
            Assert $asserts "设备无 $n" ($cnt.Count -eq 0) "count=$($cnt.Count)"
        }
        $pcOnline = @($pcAll | Where-Object { $_.event -eq "session_online" })
        Assert $asserts "PC 无 session_online" ($pcOnline.Count -eq 0) "count=$($pcOnline.Count)"
        $forbidden = @("ap_ready", "auth_result", "wifi_result", "close_ap_sent", "host_announce", "host_bye")
        $bad = @($devAll | Where-Object { $forbidden -contains $_.event })
        $badNames = @($bad | ForEach-Object { $_.event }) -join ","
        Assert $asserts "设备事件流不含任何配网/热点事件" ($bad.Count -eq 0) "found=$badNames"

        $devStop = Wait-ScenarioResult $ctx.DevEvents "f_dev_stop" 25000
        Assert $asserts "设备 request_stop 动作执行（扫描约 15 秒后自停）" ($null -ne $devStop -and $devStop.data.status -eq "ok") `
            $(if ($null -ne $devStop) { "result=$($devStop.data | ConvertTo-Json -Compress)" } else { "scenario_result 缺失" })

        $facts = Finish-Processes $ctx @("pcSide", "DevSide") @("pcEvents", "DevEvents") @("PC", "Device") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC", "Device") $true
        $pass = $true
    } catch {
        Log "  场景 f 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.DevSide) { Stop-SideProcess $ctx.DevSide 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.DevSide) { Collect-SideOutputs $ctx.DevSide }
    }
    Write-Summary $dir "f" $pass $asserts "注：配置校验固定密码/SSID 前缀，'错误密码'分支无法经配置注入（密码被校验为产品固定值），本场景采用'目标 SSID 不存在'分支；密码不符语义由端侧 sim 单测覆盖"
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 g：双向 app_data（512/513/UTF-8）
# ---------------------------------------------------------------------------
function Invoke-ScenarioG([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "g_app_data.json"
        $ctx = Start-DualProcess $dir $scn 25 25 "Modu_PC"

        $pcOnline = Wait-Events $ctx.pcEvents "session_online" "pc" 1 30000
        $devOnline = Wait-Events $ctx.DevEvents "session_online" "device" 1 30000
        Assert $asserts "session_online（PC）" (@($pcOnline).Count -ge 1) $(if (@($pcOnline).Count -ge 1) { "seq=$($pcOnline[0].seq)" } else { "缺失" })
        Assert $asserts "session_online（设备）" (@($devOnline).Count -ge 1) $(if (@($devOnline).Count -ge 1) { "seq=$($devOnline[0].seq)" } else { "缺失" })

        # 从场景文件读取文本（避免与脚本硬编码漂移）
        $scnJson = Get-JsonSafe (Join-Path $dir "scenario.json")
        $textOf = @{}
        foreach ($a in $scnJson.actions) {
            if ($null -ne $a.args -and $a.args.PSObject.Properties['text'] -and $a.args.text) {
                $textOf[$a.id] = $a.args.text
            }
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

        foreach ($pair in @(
            @{ id = "g_pc_512";  sender = "pc";     recver = "device"; expect = "ok" },
            @{ id = "g_dev_512"; sender = "device"; recver = "pc";     expect = "ok" },
            @{ id = "g_pc_513";  sender = "pc";     recver = "device"; expect = "rejected" },
            @{ id = "g_dev_513"; sender = "device"; recver = "pc";     expect = "rejected" },
            @{ id = "g_pc_utf8"; sender = "pc";     recver = "device"; expect = "ok" },
            @{ id = "g_dev_utf8";sender = "device"; recver = "pc";     expect = "ok" }
        )) {
            $id = $pair.id
            $text = $textOf[$id]
            $sha = Sha256Hex $text
            $senderPath = if ($pair.sender -eq "pc") { $ctx.pcEvents } else { $ctx.DevEvents }
            $recvPath = if ($pair.recver -eq "pc") { $ctx.pcEvents } else { $ctx.DevEvents }
            $r = Wait-ScenarioResult $senderPath $id 20000
            $rEv = $(if ($null -ne $r) { "result=$($r.data | ConvertTo-Json -Compress)" } else { "scenario_result 缺失" })
            if ($pair.expect -eq "ok") {
                Assert $asserts "$id scenario_result=ok" ($null -ne $r -and $r.data.status -eq "ok") $rEv
                $tx = Wait-DataEvent $senderPath "app_data_tx" $sha 10000
                $rx = Wait-DataEvent $recvPath "app_data_rx" $sha 10000
                Assert $asserts "$id 发送端 app_data_tx sha256 一致" ($null -ne $tx) "sha=$($sha.Substring(0,16))..."
                Assert $asserts "$id 接收端 app_data_rx sha256 一致" ($null -ne $rx) "sha=$($sha.Substring(0,16))..."
            } else {
                Assert $asserts "$id scenario_result=rejected/payload_too_large/request_bytes=$($text.Length)" `
                    ($null -ne $r -and $r.data.status -eq "rejected" -and $r.data.reason -eq "payload_too_large" -and $r.data.request_bytes -eq $text.Length) $rEv
                $badTx = Wait-DataEvent $senderPath "app_data_tx" $sha 3000
                Assert $asserts "$id 无 app_data_tx（超限不发送）" ($null -eq $badTx) "意外 tx"
            }
        }

        $facts = Finish-Processes $ctx @("pcSide", "DevSide") @("pcEvents", "DevEvents") @("PC", "Device") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC", "Device") $true
        $pass = $true
    } catch {
        Log "  场景 g 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        if ($ctx.DevSide) { Stop-SideProcess $ctx.DevSide 2000 }
        Collect-SideOutputs $ctx.pcSide
        if ($ctx.DevSide) { Collect-SideOutputs $ctx.DevSide }
    }
    Write-Summary $dir "g" $pass $asserts ""
    return $pass
}

# ---------------------------------------------------------------------------
# 场景 h：帧边界与协议版本（PC-only 原始 socket 探针）
# ---------------------------------------------------------------------------
function Invoke-ScenarioH([string]$dir) {
    $asserts = New-Object System.Collections.ArrayList
    $pass = $false
    $ctx = $null
    try {
        $scn = Join-Path $script:ScenariosDir "h_frame_probe.json"
        $ctx = Start-Pc $dir (Copy-Scenario $dir $scn) 20 "h"

        $pcEvs = Read-Events $ctx.pcEvents
        $hs = $pcEvs | Where-Object { $_.event -eq "hotspot_ready" } | Select-Object -First 1
        $tc = $pcEvs | Where-Object { $_.event -eq "tcp_listening" } | Select-Object -First 1
        $rd = $pcEvs | Where-Object { $_.event -eq "ready" } | Select-Object -First 1
        Assert $asserts "PC 事件顺序 hotspot_ready→tcp_listening→ready" `
            ($null -ne $hs -and $null -ne $tc -and $null -ne $rd -and $hs.seq -lt $tc.seq -and $tc.seq -lt $rd.seq) `
            "seq hs=$($hs.seq) tc=$($tc.seq) rd=$($rd.seq)"

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
        function Close-Conn($c) { try { $c.Close() } catch { } }

        $helloA1 = '{"cmd":"device_hello","id":"02:00:00:00:00:a1","fw_version":"1.1.0","proto_ver":1,"uptime":1}'
        $helloA2 = '{"cmd":"device_hello","id":"02:00:00:00:00:a2","fw_version":"1.1.0","proto_ver":1,"uptime":1}'
        $helloV2 = '{"cmd":"device_hello","id":"02:00:00:00:00:a3","fw_version":"1.1.0","proto_ver":2,"uptime":1}'
        $helloA4 = '{"cmd":"device_hello","id":"02:00:00:00:00:a4","fw_version":"1.1.0","proto_ver":1,"uptime":1}'

        # --- 1) 拆包/粘包：一帧分两次写入，仍被完整解析 ---
        $payload = [System.Text.Encoding]::UTF8.GetBytes($helloA1)
        $full = [byte[]]@(0, $payload.Length) + $payload
        $probe = Try-Connect "127.0.0.1" 5935
        $half = [Math]::Floor($full.Length / 2)
        $s = $probe.GetStream()
        $s.Write($full, 0, $half); $s.Flush()
        Start-Sleep -Milliseconds 150
        $s.Write($full, $half, $full.Length - $half); $s.Flush()
        $resp = Read-Frame $probe 5000
        Assert $asserts "拆包帧（分两次写入）被完整解析" ($null -ne $resp) $(if ($null -ne $resp) { "resp=$resp" } else { "无响应" })
        $j = $resp | ConvertFrom-Json
        Assert $asserts "拆包 hello → host_ack ok" ($j.cmd -eq "host_ack" -and $j.status -eq "ok") "resp=$resp"
        Close-Conn $probe
        Start-Sleep -Milliseconds 500

        # --- 2) 1025 超长帧头被拒绝且连接仍可用 ---
        $probe = Try-Connect "127.0.0.1" 5935
        $s = $probe.GetStream()
        $s.Write([byte[]]@(0x04, 0x01), 0, 2); $s.Flush()
        Start-Sleep -Milliseconds 150
        Send-Frame $probe ([System.Text.Encoding]::UTF8.GetBytes($helloA2))
        $resp = Read-Frame $probe 5000
        Assert $asserts "超长帧头后连接仍可响应合法帧" ($null -ne $resp) $(if ($null -ne $resp) { "resp=$resp" } else { "无响应" })
        $j = $resp | ConvertFrom-Json
        Assert $asserts "超长帧后合法 hello → host_ack ok（畸形计数 1/3 未达上限）" ($j.cmd -eq "host_ack" -and $j.status -eq "ok") "resp=$resp"
        Close-Conn $probe
        Start-Sleep -Milliseconds 500

        # --- 3) 0 长度帧 × 3 → malformed_max 关闭连接 ---
        $probe = Try-Connect "127.0.0.1" 5935
        $s = $probe.GetStream()
        for ($i = 0; $i -lt 3; $i++) {
            $s.Write([byte[]]@(0, 0), 0, 2); $s.Flush()
            Start-Sleep -Milliseconds 200
        }
        $closed = $false
        try {
            $s.ReadTimeout = 3000
            $b = New-Object byte[] 16
            $n = $s.Read($b, 0, 16)
            $closed = ($n -le 0)
        } catch { $closed = $true }
        Close-Conn $probe
        Assert $asserts "0 长度帧 ×3 后连接被 PC 关闭（malformed_max）" $closed "连接仍打开"
        Start-Sleep -Milliseconds 500

        # --- 4) proto_ver=2 被拒 ---
        $probe = Try-Connect "127.0.0.1" 5935
        Send-Frame $probe ([System.Text.Encoding]::UTF8.GetBytes($helloV2))
        $resp = Read-Frame $probe 5000
        Assert $asserts "proto_ver=2 hello 有响应" ($null -ne $resp) $(if ($null -ne $resp) { "resp=$resp" } else { "无响应" })
        $j = $resp | ConvertFrom-Json
        Assert $asserts "proto_ver=2 → host_ack fail/unsupported_protocol" `
            ($j.cmd -eq "host_ack" -and $j.status -eq "fail" -and $j.reason -eq "unsupported_protocol") "resp=$resp"
        $closed = $false
        try {
            $probe.GetStream().ReadTimeout = 3000
            $b = New-Object byte[] 16
            $n = $probe.GetStream().Read($b, 0, 16)
            $closed = ($n -le 0)
        } catch { $closed = $true }
        Assert $asserts "proto_ver=2 拒绝后连接被关闭" $closed "连接仍打开"
        Close-Conn $probe
        Start-Sleep -Milliseconds 500

        # --- 5) proto_ver=1 接受；在线连接粘包：一次写入两帧 ping → 两个 pong ---
        $probe = Try-Connect "127.0.0.1" 5935
        Send-Frame $probe ([System.Text.Encoding]::UTF8.GetBytes($helloA4))
        $resp = Read-Frame $probe 5000
        Assert $asserts "proto_ver=1 hello 有响应" ($null -ne $resp) $(if ($null -ne $resp) { "resp=$resp" } else { "无响应" })
        $j = $resp | ConvertFrom-Json
        Assert $asserts "proto_ver=1 → host_ack ok" ($j.cmd -eq "host_ack" -and $j.status -eq "ok") "resp=$resp"

        $ping1 = '{"cmd":"ping","seq":1}'
        $ping2 = '{"cmd":"ping","seq":2}'
        $b1 = [System.Text.Encoding]::UTF8.GetBytes($ping1)
        $b2 = [System.Text.Encoding]::UTF8.GetBytes($ping2)
        $two = [byte[]]@(0, $b1.Length) + $b1 + [byte[]]@(0, $b2.Length) + $b2
        $s = $probe.GetStream()
        $s.Write($two, 0, $two.Length); $s.Flush()   # 一次写入两帧（粘包）
        $r1 = Read-Frame $probe 5000
        $r2 = Read-Frame $probe 5000
        $j1 = if ($r1) { $r1 | ConvertFrom-Json } else { $null }
        $j2 = if ($r2) { $r2 | ConvertFrom-Json } else { $null }
        $seqs = @()
        if ($j1 -and $j1.cmd -eq "pong") { $seqs += [int]$j1.seq }
        if ($j2 -and $j2.cmd -eq "pong") { $seqs += [int]$j2.seq }
        Assert $asserts "粘包：一次写入 2 帧 ping → 2 个 pong 回显（seq=1,2）" `
            ($seqs.Count -eq 2 -and $seqs -contains 1 -and $seqs -contains 2) "pongs=$($seqs -join ',')"
        Close-Conn $probe

        $facts = Finish-Processes $ctx @("pcSide") @("pcEvents") @("PC") ($TimeoutSeconds + 10)
        Add-CleanupAssertions $asserts $facts @("PC") $true
        $pass = $true
    } catch {
        Log "  场景 h 异常: $_"
    }
    if ($ctx) {
        Stop-SideProcess $ctx.pcSide 2000
        Collect-SideOutputs $ctx.pcSide
    }
    Write-Summary $dir "h" $pass $asserts "PC-only 原始 socket 探针（无真实设备，避免占用唯一连接槽）"
    return $pass
}

# ---------------------------------------------------------------------------
# 主入口
# ---------------------------------------------------------------------------
New-Item -ItemType Directory -Path $script:RunRoot -Force | Out-Null
$script:RunLog = Join-Path $script:RunRoot "runner.log"

if ($PcExe -eq "") { throw "必须提供 -PcExe" }
if ($DeviceExe -eq "") { throw "必须提供 -DeviceExe" }
if (-not (Test-Path -LiteralPath $PcExe)) { throw "PC 可执行文件不存在: $PcExe" }
if (-not (Test-Path -LiteralPath $DeviceExe)) { throw "设备可执行文件不存在: $DeviceExe" }

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
    passed = @($runList | Where-Object { $_ -notin $failed })
    failed = $failed
    runtime_root = $script:RunRoot
}
Write-Utf8NoBom (Join-Path $script:RunRoot "integration-summary.json") ($summary | ConvertTo-Json -Depth 4)

if ($failed.Count -gt 0) {
    Log "失败场景: $($failed -join ', ')"
    exit 1
}
Log "全部场景通过"
exit 0
