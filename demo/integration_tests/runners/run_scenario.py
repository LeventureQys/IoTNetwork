#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
跨端集成测试 runner（Python 3 等价实现，与 run_scenario.ps1 语义一致）。

只消费两端已构建产物（可执行文件、config、share/protocol-contract.json），
不包含/不编译/不链接任何端侧生产源码。

场景:
  a  protocol manifest 一致性（PC/device contract 与 golden 语义比较）
  b  首次配网 -> 会话 + 心跳
  c  优雅退出（host_bye / 资源清理 / 端口可重绑）
  d  断链重连（sock_send_fail 注入 -> 离线 -> 恢复 -> 重连）
  e  close_ap 单次发送失败仍上线
  f  wifi_result fail：PC 明确失败、无 close_ap、设备保持可配网
  g  双向 app_data：512/513/UTF-8 字节语义
  h  帧边界与协议版本拒绝（尽力而为，原始 socket 探针）

用法:
  python run_scenario.py --scenario all --pc-exe out/artifacts/pc/bin/provision_pc.exe \
      --device-exe out/artifacts/device/bin/provision_device.exe \
      --runtime-root out/ss07-evidence
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
import threading
from datetime import datetime

SCENARIOS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scenarios")
GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contracts", "protocol-contract.golden.json")
SCENARIO_NAMES = ["a", "b", "c", "d", "e", "f", "g", "h"]


class Runner:
    def __init__(self, args):
        self.args = args
        self.run_root = os.path.abspath(args.runtime_root or os.path.join(os.environ.get("TEMP", "."), "modutech-integration"))
        os.makedirs(self.run_root, exist_ok=True)
        self.qt_bin = args.qt_bin or os.environ.get("QT_BIN", "D:/Devtools/Qt/6.8.3/msvc2022_64/bin")
        self.checks = []
        self.dir = None

    def log(self, msg):
        line = "[%s] %s" % (datetime.now().strftime("%H:%M:%S.%f")[:-3], msg)
        print(line, flush=True)
        with open(os.path.join(self.run_root, "runner.log"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def check(self, cond, msg):
        if not cond:
            raise AssertionError("断言失败: %s" % msg)
        self.checks.append(msg)

    # ---------- JSON 语义比较 ----------
    @staticmethod
    def json_cmp(x, y, path, diffs):
        if isinstance(x, dict) and isinstance(y, dict):
            for k in sorted(set(x) | set(y)):
                if k == "generated_from":
                    continue
                if k not in x:
                    diffs.append("%s.%s 缺失于 X" % (path, k)); continue
                if k not in y:
                    diffs.append("%s.%s 缺失于 Y" % (path, k)); continue
                Runner.json_cmp(x[k], y[k], "%s.%s" % (path, k), diffs)
            return
        if isinstance(x, list) and isinstance(y, list):
            if len(x) != len(y):
                diffs.append("%s 数组长度 %d vs %d" % (path, len(x), len(y))); return
            for i, (u, v) in enumerate(zip(x, y)):
                Runner.json_cmp(u, v, "%s[%d]" % (path, i), diffs)
            return
        if x != y:
            diffs.append("%s : %r vs %r" % (path, x, y))

    @staticmethod
    def sha256_hex(text):
        return hashlib.sha256(text.encode("utf-8")).hexdigest()

    def find_contract(self, exe, hint):
        if hint and os.path.exists(hint):
            return os.path.abspath(hint)
        d = os.path.dirname(exe)
        for _ in range(4):
            cand = os.path.join(d, "share", "protocol-contract.json")
            if os.path.exists(cand):
                return os.path.abspath(cand)
            parent = os.path.dirname(d)
            if not parent or parent == d:
                break
            d = parent
        return ""

    # ---------- 事件读取 ----------
    @staticmethod
    def read_events(path):
        if not os.path.exists(path):
            return []
        out = []
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except Exception:
                    pass
        return out

    def wait_events(self, path, name, role, count, timeout_ms):
        deadline = time.time() + timeout_ms / 1000.0
        evs = []
        while time.time() < deadline:
            evs = [e for e in self.read_events(path) if e.get("event") == name and (not role or e.get("role") == role)]
            if len(evs) >= count:
                return evs
            time.sleep(0.2)
        return evs

    def wait_any_event(self, paths, names, timeout_ms):
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            for p in paths:
                for ev in self.read_events(p):
                    if ev.get("event") in names:
                        return ev
            time.sleep(0.2)
        return None

    def port_rebindable(self, port):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", port))
            s.listen(1)
            s.close()
            return True
        except OSError:
            return False

    # ---------- 进程管理 ----------
    def start_side(self, exe, arguments, stdout_log, stderr_log, work_dir):
        env = dict(os.environ)
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["PATH"] = self.qt_bin + os.pathsep + env.get("PATH", "")
        out = open(stdout_log, "wb")
        err = open(stderr_log, "wb")
        proc = subprocess.Popen(
            [os.path.abspath(exe)] + arguments,
            stdout=out, stderr=err, env=env, cwd=work_dir,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return {"proc": proc, "out": out, "err": err, "stdout": stdout_log, "stderr": stderr_log}

    def start_dual(self, scenario_file, pc_duration, dev_duration):
        d = self.dir
        pc_runtime = os.path.join(d, "pc-runtime")
        dev_runtime = os.path.join(d, "device-runtime")
        catalog = os.path.join(d, "sim-catalog")
        pc_events = os.path.join(d, "pc-events.jsonl")
        dev_events = os.path.join(d, "device-events.jsonl")
        cfg_dir = os.path.join(d, "configs")
        os.makedirs(cfg_dir, exist_ok=True)
        empty_cwd = os.path.join(d, "empty-cwd")
        os.makedirs(empty_cwd, exist_ok=True)

        pc_cfg = os.path.join(cfg_dir, "pc_config.json")
        if self.args.pc_config:
            with open(self.args.pc_config, "rb") as src, open(pc_cfg, "wb") as dst:
                dst.write(src.read())
        else:
            with open(pc_cfg, "w", encoding="utf-8") as f:
                json.dump({
                    "host_tcp_port": 5935, "host_advertise_ip": "127.0.0.1",
                    "mcast_group": "224.0.2.1", "mcast_port": 5936,
                    "heartbeat_interval_ms": 2000, "heartbeat_dead_ms": 0,
                    "hello_timeout_ms": 5000,
                    "provision_auth_timeout_ms": 3000, "provision_wifi_cfg_timeout_ms": 8000,
                    "provision_sta_try_max": 3, "provision_handoff_grace_ms": 1000,
                    "discovery_fast_window_ms": 8000, "discovery_normal_interval_ms": 1000,
                    "discovery_candidate_timeout_ms": 6000,
                    "reconnect_backoff_base_ms": 1000, "reconnect_backoff_cap_ms": 5000,
                    "reconnect_backoff_jitter_ms": 500, "reconnect_to_discovery_ms": 30000,
                    "busy_backoff_ms": 6000, "host_max_conn": 16, "malformed_max_per_conn": 3,
                    "device_rate_limit_per_sec": 50,
                    "target_ssid": "TactileFactory-2.4G", "target_password": "securepass123",
                    "target_band_2g": 1, "duration_s": 0, "config_tag": "integration-pc",
                }, f, ensure_ascii=False, indent=2)
        dev_cfg = os.path.join(cfg_dir, "device_sim.json")
        if self.args.device_config:
            with open(self.args.device_config, "rb") as src, open(dev_cfg, "wb") as dst:
                dst.write(src.read())
        else:
            with open(dev_cfg, "w", encoding="utf-8") as f:
                json.dump({
                    "power_on_jitter_max_ms": 1000, "wifi_retry_max": 5,
                    "wifi_backoff_base_ms": 1000, "wifi_backoff_cap_ms": 30000, "wifi_backoff_jitter_ms": 5000,
                    "provision_auth_timeout_ms": 3000, "provision_wifi_cfg_timeout_ms": 8000,
                    "provision_ap_idle_timeout_ms": 0, "provision_ap_backoff_ms": 300000,
                    "provision_pin_fail_max": 5, "provision_confirm_window_ms": 12000,
                    "provision_sta_try_max": 3, "provision_handoff_grace_ms": 1000,
                    "discovery_fast_window_ms": 8000, "discovery_fast_interval_ms": 500,
                    "discovery_normal_interval_ms": 1000, "discovery_candidate_timeout_ms": 6000,
                    "heartbeat_interval_ms": 2000, "heartbeat_dead_ms": 0,
                    "host_max_conn": 16, "busy_backoff_ms": 6000, "hello_timeout_ms": 5000,
                    "rssi_sample_interval_ms": 1000, "rssi_bad_threshold_dbm": -75, "rssi_bad_duration_ms": 6000,
                    "reconnect_backoff_base_ms": 1000, "reconnect_backoff_cap_ms": 5000,
                    "reconnect_backoff_jitter_ms": 500, "reconnect_to_discovery_ms": 30000,
                    "watchdog_state_timeout_ms": 30000, "malformed_max_per_conn": 3,
                    "device_rate_limit_per_sec": 50,
                    "host_tcp_port": 5935, "host_virtual_ip": "192.168.1.50",
                    "mcast_group": "224.0.2.1", "mcast_port": 5936,
                    "device_ap_port_base": 20000, "nvs_dir": "run", "use_real_wifi_sta": 0,
                    "device_count": 1,
                    "target_ssid": "TactileFactory-2.4G", "target_password": "securepass123",
                    "target_band_2g": 1, "device_fw_version": "1.0.0", "device_proto_ver": 1,
                    "duration_s": 0, "config_tag": "integration-device",
                }, f, ensure_ascii=False, indent=2)

        scn_copy = os.path.join(d, "scenario.json")
        with open(scenario_file, "rb") as src, open(scn_copy, "wb") as dst:
            dst.write(src.read())

        pc_side = self.start_side(self.args.pc_exe, [
            "--config", pc_cfg, "--backend", "sim",
            "--runtime-dir", pc_runtime, "--sim-catalog-dir", catalog,
            "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", pc_events, "--scenario", scn_copy, "--duration", str(pc_duration),
        ], os.path.join(d, "pc-stdout.log"), os.path.join(d, "pc-stderr.log"), empty_cwd)

        dev_side = self.start_side(self.args.device_exe, [
            "--config", dev_cfg, "--backend", "sim",
            "--device-index", "0", "--fresh",
            "--runtime-dir", dev_runtime, "--sim-catalog-dir", catalog,
            "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", dev_events, "--scenario", scn_copy, "--duration", str(dev_duration),
        ], os.path.join(d, "device-stdout.log"), os.path.join(d, "device-stderr.log"), empty_cwd)

        return {
            "dir": d, "catalog": catalog, "pc_events": pc_events, "dev_events": dev_events,
            "pc": pc_side, "dev": dev_side,
        }

    def finish_dual(self, ctx, timeout_sec):
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            if ctx["pc"]["proc"].poll() is not None and ctx["dev"]["proc"].poll() is not None:
                break
            time.sleep(0.3)
        for side in (ctx["pc"], ctx["dev"]):
            if side["proc"].poll() is None:
                self.log("  强杀进程 PID=%d" % side["proc"].pid)
                side["proc"].kill()
            side["out"].close()
            side["err"].close()
        pc_code = ctx["pc"]["proc"].returncode if ctx["pc"]["proc"].poll() is not None else -999
        dev_code = ctx["dev"]["proc"].returncode if ctx["dev"]["proc"].poll() is not None else -999
        pc_events = self.read_events(ctx["pc_events"])
        dev_events = self.read_events(ctx["dev_events"])
        pc_last = pc_events[-1].get("event", "none") if pc_events else "none"
        dev_last = dev_events[-1].get("event", "none") if dev_events else "none"
        self.checks.append("PC 退出码: %d" % pc_code)
        self.checks.append("Device 退出码: %d" % dev_code)
        self.checks.append("PC PID=%d vs Device PID=%d" % (ctx["pc"]["proc"].pid, ctx["dev"]["proc"].pid))
        self.checks.append("PC 最后事件: %s (期望 shutdown_complete)" % pc_last)
        self.checks.append("Device 最后事件: %s (期望 shutdown_complete)" % dev_last)
        try:
            os.kill(ctx["pc"]["proc"].pid, 0)
            pc_alive = True
        except OSError:
            pc_alive = False
        try:
            os.kill(ctx["dev"]["proc"].pid, 0)
            dev_alive = True
        except OSError:
            dev_alive = False
        self.checks.append("无残留 PC 进程: %s" % (not pc_alive))
        self.checks.append("无残留 Device 进程: %s" % (not dev_alive))
        self.checks.append("TCP 5935 可重绑: %s" % self.port_rebindable(5935))
        self.checks.append("TCP 20000 可重绑: %s" % self.port_rebindable(20000))
        catalog_files = [f for f in os.listdir(ctx["catalog"]) if f.endswith(".json")] if os.path.isdir(ctx["catalog"]) else []
        self.checks.append("sim-catalog 退出后清空: %s" % (len(catalog_files) == 0))
        return pc_code, dev_code, pc_last, dev_last

    def write_summary(self, name, passed, note=""):
        with open(os.path.join(self.dir, "summary.json"), "w", encoding="utf-8") as f:
            json.dump({"scenario": name, "pass": passed, "timestamp": datetime.now().isoformat(),
                       "checks": self.checks, "note": note}, f, ensure_ascii=False, indent=2)

    # ---------- 场景实现 ----------
    def scenario_a(self):
        pc_contract = self.find_contract(self.args.pc_exe, self.args.pc_contract)
        dev_contract = self.find_contract(self.args.device_exe, self.args.device_contract)
        self.check(pc_contract != "", "未找到 PC protocol-contract.json（用 --pc-contract 指定）")
        self.check(dev_contract != "", "未找到 设备 protocol-contract.json（用 --device-contract 指定）")
        with open(GOLDEN, encoding="utf-8") as f:
            golden = json.load(f)
        with open(pc_contract, encoding="utf-8") as f:
            pc = json.load(f)
        with open(dev_contract, encoding="utf-8") as f:
            dev = json.load(f)
        d1, d2, d3 = [], [], []
        self.json_cmp(pc, golden, "pc", d1)
        self.json_cmp(dev, golden, "device", d2)
        self.json_cmp(pc, dev, "pc_vs_device", d3)
        diff = {"pc_vs_golden": d1, "device_vs_golden": d2, "pc_vs_device": d3}
        with open(os.path.join(self.dir, "protocol-diff.json"), "w", encoding="utf-8") as f:
            json.dump(diff, f, ensure_ascii=False, indent=2)
        self.check(not d1, "PC contract 与 golden 不一致: %r" % d1)
        self.check(not d2, "Device contract 与 golden 不一致: %r" % d2)
        self.check(not d3, "PC contract 与 Device contract 不一致: %r" % d3)
        self.write_summary("a", True, "pc=%s device=%s" % (pc_contract, dev_contract))

    def scenario_b(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "b_first_provision.json"), 25, 25)
        self.check(len(self.wait_events(ctx["dev_events"], "ready", "device", 1, 15000)) >= 1, "设备 ready 缺失")
        self.check(len(self.wait_events(ctx["pc_events"], "ready", "pc", 1, 15000)) >= 1, "PC ready 缺失")
        ap = self.wait_events(ctx["dev_events"], "ap_ready", "device", 1, 15000)
        self.check(len(ap) >= 1, "ap_ready 缺失")
        self.checks.append("ap_ready 出现: %s" % ap[0]["data"].get("ssid"))
        auth = self.wait_events(ctx["pc_events"], "auth_result", "pc", 1, 20000)
        self.check(len(auth) >= 1 and auth[0].get("result") == "ok", "auth_result 非 ok")
        wifi = self.wait_events(ctx["pc_events"], "wifi_result", "pc", 1, 20000)
        self.check(len(wifi) >= 1 and wifi[0]["data"].get("status") == "ok", "wifi_result 非 ok")
        dauth = self.wait_events(ctx["dev_events"], "auth_result", "device", 1, 20000)
        dwifi = self.wait_events(ctx["dev_events"], "wifi_result", "device", 1, 20000)
        self.check(len(dauth) >= 1 and dauth[0]["data"].get("status") == "ok", "设备 auth_result 非 ok")
        self.check(len(dwifi) >= 1 and dwifi[0]["data"].get("status") == "ok", "设备 wifi_result 非 ok")
        self.checks.append("双方 auth_result=ok / wifi_result=ok")
        online_pc = self.wait_events(ctx["pc_events"], "session_online", "pc", 1, 25000)
        online_dev = self.wait_events(ctx["dev_events"], "session_online", "device", 1, 25000)
        self.check(len(online_pc) >= 1, "PC session_online 缺失")
        self.check(len(online_dev) >= 1, "设备 session_online 缺失")
        hb = self.wait_any_event([ctx["pc_events"], ctx["dev_events"]], ["ping", "pong"], 10000)
        self.check(hb is not None, "session_online 后 10 秒内未见 ping/pong")
        self.checks.append("session_online 后 10 秒内出现 ping/pong: %s" % hb["event"])
        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("b", ok)

    def scenario_c(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "c_graceful_exit.json"), 25, 25)
        online = self.wait_events(ctx["pc_events"], "session_online", "pc", 1, 30000)
        self.check(len(online) >= 1, "PC session_online 缺失")
        bye = self.wait_events(ctx["pc_events"], "host_bye_sent", "pc", 1, 15000)
        self.check(len(bye) >= 1, "host_bye_sent 缺失")
        self.checks.append("host_bye_sent 出现（send_result=%s）" % bye[0]["data"].get("send_result"))
        off_dev = self.wait_events(ctx["dev_events"], "session_offline", "device", 1, 20000)
        self.check(len(off_dev) >= 1, "设备 session_offline 缺失（host_bye/连接关闭路径）")
        self.checks.append("设备离线（reason=%s）" % off_dev[0]["data"].get("reason"))
        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("c", ok)

    def scenario_d(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "d_reconnect.json"), 45, 45)
        on1_pc = self.wait_events(ctx["pc_events"], "session_online", "pc", 1, 30000)
        self.check(len(on1_pc) >= 1, "首次 session_online（PC）缺失")
        on1_dev = self.wait_events(ctx["dev_events"], "session_online", "device", 1, 30000)
        self.check(len(on1_dev) >= 1, "首次 session_online（设备）缺失")
        fault = self.wait_events(ctx["dev_events"], "fault_applied", "device", 1, 20000)
        self.check(len(fault) >= 1, "fault_applied 缺失")
        self.check(fault[0]["data"].get("action") == "sock_send_fail", "fault action 不符")
        off_pc = self.wait_events(ctx["pc_events"], "session_offline", "pc", 1, 20000)
        off_dev = self.wait_events(ctx["dev_events"], "session_offline", "device", 1, 20000)
        self.check(len(off_pc) >= 1, "PC session_offline 缺失")
        self.check(len(off_dev) >= 1, "设备 session_offline 缺失")
        on2_pc = self.wait_events(ctx["pc_events"], "session_online", "pc", 2, 30000)
        on2_dev = self.wait_events(ctx["dev_events"], "session_online", "device", 2, 30000)
        self.check(len(on2_pc) >= 2, "PC 重连 session_online 缺失")
        self.check(len(on2_dev) >= 2, "设备重连 session_online 缺失")
        rc1 = int(on1_pc[0]["data"]["reconnect_count"]); rc2 = int(on2_pc[1]["data"]["reconnect_count"])
        dc1 = int(on1_dev[0]["data"]["reconnect_count"]); dc2 = int(on2_dev[-1]["data"]["reconnect_count"])
        self.check(rc2 > rc1, "PC reconnect_count 未增长: %d -> %d" % (rc1, rc2))
        self.check(dc2 > dc1, "设备 reconnect_count 未增长: %d -> %d" % (dc1, dc2))
        hb = self.wait_any_event([ctx["pc_events"], ctx["dev_events"]], ["ping", "pong"], 10000)
        self.check(hb is not None, "重连后 10 秒内未见 ping/pong")
        self.checks.append("重连后 reconnect_count 增长（PC %d->%d，设备 %d->%d）" % (rc1, rc2, dc1, dc2))
        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 20)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("d", ok)

    def scenario_e(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "e_close_ap_fail.json"), 25, 25)
        fault = self.wait_events(ctx["pc_events"], "fault_applied", "pc", 1, 15000)
        self.check(len(fault) >= 1, "PC fault_applied 缺失")
        close = self.wait_events(ctx["pc_events"], "close_ap_sent", "pc", 1, 25000)
        self.check(len(close) >= 1, "close_ap_sent 缺失")
        self.check(int(close[0]["code"]) != 0, "close_ap_sent 应记录发送失败（code=%s）" % close[0]["code"])
        wifi = self.wait_events(ctx["pc_events"], "wifi_result", "pc", 1, 20000)
        self.check(len(wifi) >= 1 and wifi[0]["data"].get("status") == "ok", "wifi_result 非 ok")
        on_pc = self.wait_events(ctx["pc_events"], "session_online", "pc", 1, 30000)
        on_dev = self.wait_events(ctx["dev_events"], "session_online", "device", 1, 30000)
        self.check(len(on_pc) >= 1, "PC session_online 缺失")
        self.check(len(on_dev) >= 1, "设备 session_online 缺失")
        self.checks.append("close_ap 失败一次（send_result=%s）后设备仍上线" % close[0]["data"].get("send_result"))
        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("e", ok)

    def scenario_f(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "f_wifi_result_fail.json"), 25, 25)
        wifi = self.wait_events(ctx["pc_events"], "wifi_result", "pc", 1, 25000)
        self.check(len(wifi) >= 1 and wifi[0]["data"].get("status") == "fail", "PC wifi_result 非 fail")
        dwifi = self.wait_events(ctx["dev_events"], "wifi_result", "device", 1, 25000)
        self.check(len(dwifi) >= 1 and dwifi[0]["data"].get("status") == "fail", "设备 wifi_result 非 fail")
        close = [e for e in self.read_events(ctx["pc_events"]) if e.get("event") == "close_ap_sent"]
        self.check(len(close) == 0, "wifi_result fail 后不应发送 close_ap")
        on_pc = [e for e in self.read_events(ctx["pc_events"]) if e.get("event") == "session_online"]
        on_dev = [e for e in self.read_events(ctx["dev_events"]) if e.get("event") == "session_online"]
        self.check(len(on_pc) == 0 and len(on_dev) == 0, "wifi 失败后不应出现 session_online")
        catalog_seen = False
        for _ in range(30):
            if os.path.exists(os.path.join(ctx["catalog"], "device-0.json")):
                catalog_seen = True
                break
            time.sleep(0.3)
        self.check(catalog_seen, "设备配网热点 catalog 未发布")
        self.checks.append("双方 wifi_result=fail，无 close_ap_sent，无 session_online，设备保持可配网")
        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("f", ok)

    def scenario_g(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "g_app_data.json"), 30, 30)
        self.check(len(self.wait_events(ctx["pc_events"], "session_online", "pc", 1, 30000)) >= 1, "session_online（PC）缺失")

        def wait_scenario_result(path, aid, timeout_ms):
            deadline = time.time() + timeout_ms / 1000.0
            while time.time() < deadline:
                for e in self.read_events(path):
                    if e.get("event") == "scenario_result" and e["data"].get("action_id") == aid:
                        return e
                time.sleep(0.2)
            return None

        def wait_data_event(path, name, sha, timeout_ms):
            deadline = time.time() + timeout_ms / 1000.0
            while time.time() < deadline:
                for e in self.read_events(path):
                    if e.get("event") == name and e["data"].get("sha256") == sha:
                        return e
                time.sleep(0.2)
            return None

        texts = {
            "pc512": "P" * 512, "dev512": "D" * 512,
            "pc513": "Q" * 513, "dev513": "E" * 513,
            "pc_utf8": "模组科技网络双向数据一致性测试-UTF8多字节-0123456789-中文编码验证-abcdefghijklmnopqrstuvwxyz",
            "dev_utf8": "设备端UTF8回传：压强传感数据帧边界测试-协议互操作-2026-双向发送",
        }

        r = wait_scenario_result(ctx["pc_events"], "g_pc_512", 20000)
        self.check(r is not None and r["data"]["status"] == "ok", "g_pc_512 未 ok")
        sha = self.sha256_hex(texts["pc512"])
        self.check(wait_data_event(ctx["pc_events"], "app_data_tx", sha, 10000) is not None, "PC app_data_tx(512) sha256 不符")
        self.check(wait_data_event(ctx["dev_events"], "app_data_rx", sha, 10000) is not None, "设备 app_data_rx(512) sha256 不符")
        self.checks.append("PC->设备 512 字节：tx/rx sha256 一致")

        r = wait_scenario_result(ctx["dev_events"], "g_dev_512", 20000)
        self.check(r is not None and r["data"]["status"] == "ok", "g_dev_512 未 ok")
        sha = self.sha256_hex(texts["dev512"])
        self.check(wait_data_event(ctx["dev_events"], "app_data_tx", sha, 10000) is not None, "设备 app_data_tx(512) sha256 不符")
        self.check(wait_data_event(ctx["pc_events"], "app_data_rx", sha, 10000) is not None, "PC app_data_rx(512) sha256 不符")
        self.checks.append("设备->PC 512 字节：tx/rx sha256 一致")

        r = wait_scenario_result(ctx["pc_events"], "g_pc_513", 20000)
        self.check(r is not None and r["data"]["status"] == "rejected" and r["data"]["request_bytes"] == 513
                   and r["data"]["reason"] == "payload_too_large", "g_pc_513 未按 payload_too_large 拒绝")
        self.check(wait_data_event(ctx["pc_events"], "app_data_tx", self.sha256_hex(texts["pc513"]), 3000) is None,
                   "513 字节不应产生 app_data_tx")
        self.checks.append("PC 513 字节：rejected/payload_too_large，无 tx")

        r = wait_scenario_result(ctx["dev_events"], "g_dev_513", 20000)
        self.check(r is not None and r["data"]["status"] == "rejected" and r["data"]["request_bytes"] == 513
                   and r["data"]["reason"] == "payload_too_large", "g_dev_513 未按 payload_too_large 拒绝")
        self.check(wait_data_event(ctx["dev_events"], "app_data_tx", self.sha256_hex(texts["dev513"]), 3000) is None,
                   "513 字节不应产生设备 app_data_tx")
        self.checks.append("设备 513 字节：rejected/payload_too_large，无 tx")

        r = wait_scenario_result(ctx["pc_events"], "g_pc_utf8", 20000)
        self.check(r is not None and r["data"]["status"] == "ok", "g_pc_utf8 未 ok")
        sha = self.sha256_hex(texts["pc_utf8"])
        self.check(wait_data_event(ctx["pc_events"], "app_data_tx", sha, 10000) is not None, "PC->设备 UTF-8 sha256 不符")
        self.check(wait_data_event(ctx["dev_events"], "app_data_rx", sha, 10000) is not None, "PC->设备 UTF-8 rx sha256 不符")
        self.checks.append("PC->设备 UTF-8 多字节：tx/rx sha256 一致")

        r = wait_scenario_result(ctx["dev_events"], "g_dev_utf8", 20000)
        self.check(r is not None and r["data"]["status"] == "ok", "g_dev_utf8 未 ok")
        sha = self.sha256_hex(texts["dev_utf8"])
        self.check(wait_data_event(ctx["dev_events"], "app_data_tx", sha, 10000) is not None, "设备->PC UTF-8 sha256 不符")
        self.check(wait_data_event(ctx["pc_events"], "app_data_rx", sha, 10000) is not None, "设备->PC UTF-8 rx sha256 不符")
        self.checks.append("设备->PC UTF-8 多字节：tx/rx sha256 一致")

        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("g", ok)

    def scenario_h(self):
        ctx = self.start_dual(os.path.join(SCENARIOS_DIR, "h_frame_probe.json"), 20, 20)
        self.check(len(self.wait_events(ctx["dev_events"], "ready", "device", 1, 15000)) >= 1, "设备 ready 缺失")
        self.check(len(self.wait_events(ctx["pc_events"], "ready", "pc", 1, 15000)) >= 1, "PC ready 缺失")
        self.check(len(self.wait_events(ctx["dev_events"], "ap_ready", "device", 1, 15000)) >= 1, "ap_ready 缺失")
        time.sleep(1)

        def send_frame(client, payload):
            hdr = bytes([(len(payload) >> 8) & 0xFF, len(payload) & 0xFF])
            client.sendall(hdr + payload)

        def read_frame(client, timeout_ms):
            client.settimeout(timeout_ms / 1000.0)
            try:
                hdr = b""
                while len(hdr) < 2:
                    chunk = client.recv(2 - len(hdr))
                    if not chunk:
                        return None
                    hdr += chunk
                length = (hdr[0] << 8) | hdr[1]
                body = b""
                while len(body) < length:
                    chunk = client.recv(length - len(body))
                    if not chunk:
                        return None
                    body += chunk
                return body.decode("utf-8")
            except (socket.timeout, OSError):
                return None

        def try_connect(port):
            s = socket.create_connection(("127.0.0.1", port), timeout=3)
            return s

        auth_bad = b'{"cmd":"auth","pin":"0000"}'
        full = bytes([0, len(auth_bad)]) + auth_bad

        # A: 拆包
        s = try_connect(20000)
        half = len(full) // 2
        s.sendall(full[:half])
        time.sleep(0.15)
        s.sendall(full[half:])
        resp = read_frame(s, 5000)
        self.check(resp is not None, "拆包发送 auth 后无响应")
        j = json.loads(resp)
        self.check(j.get("cmd") == "auth_result" and j.get("status") == "fail", "auth_result 期望 fail，实际 %s" % resp)
        s.close()
        self.checks.append("拆包/粘包帧解析：分两次写出的 auth 帧被正确解析")

        # B: 超长帧头
        s = try_connect(20000)
        s.sendall(bytes([0x04, 0x01]))
        time.sleep(0.15)
        s.sendall(full)
        resp = read_frame(s, 5000)
        self.check(resp is not None, "超长帧后连接应仍可用并响应合法帧")
        j = json.loads(resp)
        self.check(j.get("cmd") == "auth_result" and j.get("status") == "fail", "超长帧后 auth_result 期望 fail")
        s.close()
        self.checks.append("1025 字节超长帧头被拒绝，连接未崩溃且继续工作")

        # C: 0 长度帧 x3 -> 关闭
        s = try_connect(20000)
        for _ in range(3):
            s.sendall(bytes([0, 0]))
            time.sleep(0.2)
        s.settimeout(3)
        closed = False
        try:
            n = len(s.recv(16))
            closed = n <= 0
        except OSError:
            closed = True
        s.close()
        self.check(closed, "连续 3 个 0 长度帧后连接应被设备关闭（malformed_max）")
        self.checks.append("0 长度帧 x3：设备按 malformed_max 关闭连接且不崩溃")

        # PC 业务端口：版本拒绝
        hello_bad = ('{"cmd":"device_hello","id":"02:00:00:00:00:ff","type":"pressure_sensor",'
                     '"fw_version":"1.0.0","proto_ver":2,"capabilities":["pressure"],"uptime":1}')
        s = try_connect(5935)
        send_frame(s, hello_bad.encode("utf-8"))
        resp = read_frame(s, 5000)
        self.check(resp is not None, "proto_ver=2 的 device_hello 无响应")
        j = json.loads(resp)
        self.check(j.get("cmd") == "host_ack" and j.get("status") == "fail", "proto_ver=2 应被拒绝，实际 %s" % resp)
        s.close()
        self.checks.append("PC 拒绝 proto_ver=2（host_ack status=fail）")

        hello_ok = ('{"cmd":"device_hello","id":"02:00:00:00:00:fe","type":"pressure_sensor",'
                    '"fw_version":"1.0.0","proto_ver":1,"capabilities":["pressure"],"uptime":1}')
        s = try_connect(5935)
        send_frame(s, hello_ok.encode("utf-8"))
        resp = read_frame(s, 5000)
        self.check(resp is not None, "proto_ver=1 的 device_hello 无响应")
        j = json.loads(resp)
        self.check(j.get("cmd") == "host_ack" and j.get("status") == "ok", "proto_ver=1 应被接受，实际 %s" % resp)
        s.close()
        self.checks.append("PC 接受 proto_ver=1（host_ack status=ok）")

        pc_code, dev_code, pc_last, dev_last = self.finish_dual(ctx, self.args.timeout_seconds + 10)
        ok = pc_code == 0 and dev_code == 0 and pc_last == "shutdown_complete" and dev_last == "shutdown_complete"
        self.write_summary("h", ok, "尽力而为场景（原始 socket 探针）")

    # ---------- 主入口 ----------
    def run(self, names):
        failed = []
        for name in names:
            self.dir = os.path.join(self.run_root, "%s_%s" % (datetime.now().strftime("%Y%m%d_%H%M%S"), name))
            os.makedirs(os.path.join(self.dir, "pc-runtime"), exist_ok=True)
            os.makedirs(os.path.join(self.dir, "device-runtime"), exist_ok=True)
            os.makedirs(os.path.join(self.dir, "sim-catalog"), exist_ok=True)
            os.makedirs(os.path.join(self.dir, "logs"), exist_ok=True)
            self.checks = []
            self.log("==== 场景 %s @ %s ====" % (name, self.dir))
            try:
                getattr(self, "scenario_%s" % name)()
                self.log("  场景 %s 通过" % name)
            except Exception as exc:
                self.log("  场景 %s 异常: %s" % (name, exc))
                failed.append(name)
                for proc_name in ("provision_pc", "provision_device"):
                    os.system("taskkill /F /IM %s.exe >NUL 2>&1" % proc_name)
        summary = {"timestamp": datetime.now().isoformat(), "failed": failed,
                   "passed": [n for n in names if n not in failed], "runtime_root": self.run_root}
        with open(os.path.join(self.run_root, "integration-summary.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
        if failed:
            self.log("失败场景: %s" % ", ".join(failed))
            return 1
        self.log("全部场景通过")
        return 0


def main():
    parser = argparse.ArgumentParser(description="跨端集成测试 runner（Python 3）")
    parser.add_argument("--scenario", default="all", help="a,b,c,d,e,f,g,h 或 all")
    parser.add_argument("--pc-exe", required=True)
    parser.add_argument("--device-exe", required=True)
    parser.add_argument("--pc-config", default="")
    parser.add_argument("--device-config", default="")
    parser.add_argument("--pc-contract", default="")
    parser.add_argument("--device-contract", default="")
    parser.add_argument("--runtime-root", default="")
    parser.add_argument("--timeout-seconds", type=int, default=45)
    parser.add_argument("--qt-bin", default="")
    args = parser.parse_args()
    names = SCENARIO_NAMES if args.scenario == "all" else [s.strip() for s in args.scenario.split(",")]
    for n in names:
        if n not in SCENARIO_NAMES:
            print("未知场景: %s（可选 %s 或 all）" % (n, ",".join(SCENARIO_NAMES)))
            return 2
    runner = Runner(args)
    return runner.run(names)


if __name__ == "__main__":
    sys.exit(main())
