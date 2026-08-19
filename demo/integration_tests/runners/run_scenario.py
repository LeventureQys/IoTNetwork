#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
跨端集成测试 runner（Python 3 等价实现，与 run_scenario.ps1 语义一致）。

只消费两端已构建产物（可执行文件、config、share/protocol-contract.json），
不包含/不编译/不链接任何端侧生产源码。

beta v1.1 拓扑：PC 发布热点（sim 写 pc-hotspot.json catalog）→ 设备扫描/
连接固定地址 192.168.137.1:5935 → 握手 → 心跳/应用数据。

场景:
  a  protocol manifest 一致性（三份 schema2 contract 语义比较）
  b  PC hotspot_ready -> 设备扫描/STA/直连 -> 双方 session_online -> ping/pong
  c  PC request_stop -> catalog 删除 -> 设备 session_offline；无残留
  d  sock_send_fail 注入 -> 双方离线 -> 恢复 -> 固定地址重连，reconnect_count 增长
  e  第二设备被拒（single_device_only），第一设备保持在线
  f  目标 SSID 不存在：无 session_online、无配网/热点事件、设备持续扫描
  g  双向 app_data：512 成功、513 rejected、UTF-8 一致
  h  帧边界与协议版本（拆包/超长/空帧/版本拒绝，原始 socket 探针，PC-only）

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
import struct
import subprocess
import sys
import time
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
        self.dir = None

    # ---------- 基础设施 ----------
    def log(self, msg):
        line = "[%s] %s" % (datetime.now().strftime("%H:%M:%S.%f")[:-3], msg)
        print(line, flush=True)
        with open(os.path.join(self.run_root, "runner.log"), "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def assert_true(self, asserts, name, cond, evidence):
        status = "pass" if cond else "fail"
        asserts.append({"name": name, "status": status, "evidence": evidence})
        if not cond:
            raise AssertionError("断言失败: %s （%s）" % (name, evidence))

    def write_utf8_no_bom(self, path, text):
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)

    def write_summary(self, scenario, passed, asserts, note=""):
        summary = {
            "scenario": scenario,
            "pass": passed,
            "timestamp": datetime.now().isoformat(),
            "assertions": asserts,
            "note": note,
        }
        self.write_utf8_no_bom(os.path.join(self.dir, "summary.json"), json.dumps(summary, ensure_ascii=False, indent=2))

    def load_json(self, path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

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
        d = os.path.dirname(os.path.abspath(exe))
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

    def wait_scenario_result(self, path, action_id, timeout_ms):
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            for ev in self.read_events(path):
                if ev.get("event") == "scenario_result" and ev.get("data", {}).get("action_id") == action_id:
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

    def stop_side(self, side, wait_s=3.0):
        proc = side["proc"]
        try:
            proc.wait(timeout=wait_s)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        finally:
            for f in (side["out"], side["err"]):
                try:
                    f.close()
                except Exception:
                    pass

    def new_configs(self, pc_duration, dev_duration, device_ssid="Modu_PC"):
        cfg_dir = os.path.join(self.dir, "configs")
        os.makedirs(cfg_dir, exist_ok=True)
        pc_cfg = os.path.join(cfg_dir, "pc_config.json")
        dev_cfg = os.path.join(cfg_dir, "device_sim.json")
        if self.args.pc_config:
            with open(self.args.pc_config, "rb") as src, open(pc_cfg, "wb") as dst:
                dst.write(src.read())
        else:
            self.write_utf8_no_bom(pc_cfg, json.dumps({
                "host_tcp_port": 5935,
                "pc_ap_ssid": "Modu_PC", "pc_ap_password": "modu_leventure",
                "pc_ap_ip": "192.168.137.1", "pc_ap_prefix_length": 24,
                "heartbeat_interval_ms": 2000, "heartbeat_dead_ms": 0,
                "hello_timeout_ms": 5000, "malformed_max_per_conn": 3,
                "device_rate_limit_per_sec": 50, "duration_s": pc_duration,
                "config_tag": "integration-pc",
            }, ensure_ascii=False, indent=2))
        if self.args.device_config:
            with open(self.args.device_config, "rb") as src, open(dev_cfg, "wb") as dst:
                dst.write(src.read())
        else:
            self.write_utf8_no_bom(dev_cfg, json.dumps({
                "power_on_jitter_max_ms": 500, "wifi_retry_max": 5,
                "wifi_backoff_base_ms": 1000, "wifi_backoff_cap_ms": 5000, "wifi_backoff_jitter_ms": 500,
                "heartbeat_interval_ms": 2000, "heartbeat_dead_ms": 0,
                "busy_backoff_ms": 60000, "hello_timeout_ms": 5000,
                "reconnect_backoff_base_ms": 1000, "reconnect_backoff_cap_ms": 5000,
                "reconnect_backoff_jitter_ms": 500, "malformed_max_per_conn": 3,
                "device_rate_limit_per_sec": 50, "host_tcp_port": 5935,
                "pc_ap_ssid": device_ssid, "pc_ap_password": "modu_leventure",
                "pc_host_ip": "192.168.137.1", "nvs_dir": "run",
                "use_real_wifi_sta": 0, "device_fw_version": "1.1.0",
                "device_proto_ver": 1, "duration_s": dev_duration,
                "config_tag": "integration-device",
            }, ensure_ascii=False, indent=2))
        return pc_cfg, dev_cfg

    def start_pc(self, scenario_file, pc_duration):
        d = self.dir
        catalog = os.path.join(d, "sim-catalog")
        pc_events = os.path.join(d, "pc-events.jsonl")
        pc_runtime = os.path.join(d, "pc-runtime")
        empty_cwd = os.path.join(d, "empty-cwd")
        os.makedirs(empty_cwd, exist_ok=True)
        scn_copy = os.path.join(d, "scenario.json")
        with open(scenario_file, "rb") as src, open(scn_copy, "wb") as dst:
            dst.write(src.read())
        pc_cfg, _ = self.new_configs(pc_duration, 1, "Modu_PC")
        pc_side = self.start_side(self.args.pc_exe, [
            "--config", pc_cfg, "--backend", "sim",
            "--runtime-dir", pc_runtime, "--sim-catalog-dir", catalog,
            "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", pc_events, "--scenario", scn_copy,
            "--duration", str(pc_duration),
        ], os.path.join(d, "pc-stdout.log"), os.path.join(d, "pc-stderr.log"), empty_cwd)
        hot = self.wait_events(pc_events, "hotspot_ready", "pc", 1, 15000)
        if len(hot) < 1:
            self.stop_side(pc_side)
            raise AssertionError("PC 未在 15 秒内产生 hotspot_ready")
        return {
            "pcSide": pc_side, "pcEvents": pc_events, "catalog": catalog,
            "pcDuration": pc_duration,
        }

    def start_dual(self, scenario_file, pc_duration, dev_duration, device_ssid="Modu_PC"):
        d = self.dir
        ctx = self.start_pc(scenario_file, pc_duration)
        dev_events = os.path.join(d, "device-events.jsonl")
        dev_runtime = os.path.join(d, "device-runtime")
        empty_cwd = os.path.join(d, "empty-cwd")
        pc_cfg, dev_cfg = self.new_configs(pc_duration, dev_duration, device_ssid)
        dev_side = self.start_side(self.args.device_exe, [
            "--config", dev_cfg, "--backend", "sim", "--device-index", "0", "--fresh",
            "--runtime-dir", dev_runtime, "--sim-catalog-dir", ctx["catalog"],
            "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", dev_events, "--scenario", scenario_file,
            "--duration", str(dev_duration),
        ], os.path.join(d, "device-stdout.log"), os.path.join(d, "device-stderr.log"), empty_cwd)
        ctx["DevSide"] = dev_side
        ctx["DevEvents"] = dev_events
        ctx["DevDuration"] = dev_duration
        return ctx

    def start_triple(self, scenario_file, pc_duration, dev_duration):
        d = self.dir
        ctx = self.start_pc(scenario_file, pc_duration)
        empty_cwd = os.path.join(d, "empty-cwd")
        _, dev_cfg = self.new_configs(pc_duration, dev_duration, "Modu_PC")
        dev1_events = os.path.join(d, "dev1-events.jsonl")
        dev1_side = self.start_side(self.args.device_exe, [
            "--config", dev_cfg, "--backend", "sim", "--device-index", "0", "--fresh",
            "--runtime-dir", os.path.join(d, "device1-runtime"),
            "--sim-catalog-dir", ctx["catalog"], "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", dev1_events, "--scenario", scenario_file,
            "--duration", str(dev_duration),
        ], os.path.join(d, "dev1-stdout.log"), os.path.join(d, "dev1-stderr.log"), empty_cwd)
        on1 = self.wait_events(ctx["pcEvents"], "session_online", "pc", 1, 30000)
        if len(on1) < 1:
            self.stop_side(dev1_side)
            self.stop_side(ctx["pcSide"])
            raise AssertionError("设备1 未在 30 秒内上线")
        dev1_id = on1[0].get("device_id", "")
        dev2_events = os.path.join(d, "dev2-events.jsonl")
        dev2_side = self.start_side(self.args.device_exe, [
            "--config", dev_cfg, "--backend", "sim", "--device-index", "1", "--fresh",
            "--runtime-dir", os.path.join(d, "device2-runtime"),
            "--sim-catalog-dir", ctx["catalog"], "--log-dir", os.path.join(d, "logs"),
            "--events-jsonl", dev2_events, "--scenario", scenario_file,
            "--duration", str(dev_duration),
        ], os.path.join(d, "dev2-stdout.log"), os.path.join(d, "dev2-stderr.log"), empty_cwd)
        ctx["Dev1Side"] = dev1_side
        ctx["Dev1Events"] = dev1_events
        ctx["Dev1Id"] = dev1_id
        ctx["Dev2Side"] = dev2_side
        ctx["Dev2Events"] = dev2_events
        return ctx

    def finish_processes(self, ctx, side_keys, event_keys, names, timeout_s):
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if all(ctx[k]["proc"].poll() is not None for k in side_keys):
                break
            time.sleep(0.3)
        for k in side_keys:
            self.stop_side(ctx[k], wait_s=3.0)
        facts = {}
        for sk, ek, nm in zip(side_keys, event_keys, names):
            proc = ctx[sk]["proc"]
            code = proc.returncode if proc.returncode is not None else -999
            last = self.read_events(ctx[ek])
            last_ev = last[-1].get("event", "none") if last else "none"
            alive = proc.poll() is None
            facts["%sCode" % nm] = code
            facts["%sLast" % nm] = last_ev
            facts["%sAlive" % nm] = alive
            facts["%sPid" % nm] = proc.pid
        catalog_files = [f for f in os.listdir(ctx["catalog"]) if f.endswith(".json")] if os.path.isdir(ctx["catalog"]) else []
        facts["CatalogFiles"] = len(catalog_files)
        return facts

    def add_cleanup_assertions(self, asserts, facts, names, check_port5935=True):
        for nm in names:
            self.assert_true(asserts, "%s 退出码为 0" % nm, facts["%sCode" % nm] == 0, "exit=%s" % facts["%sCode" % nm])
            self.assert_true(asserts, "%s 最后事件为 shutdown_complete" % nm,
                             facts["%sLast" % nm] == "shutdown_complete", "last=%s" % facts["%sLast" % nm])
            self.assert_true(asserts, "无残留 %s 进程" % nm, not facts["%sAlive" % nm], "pid=%s" % facts["%sPid" % nm])
        self.assert_true(asserts, "无残留 catalog 文件（pc-hotspot.json）", facts["CatalogFiles"] == 0,
                         "files=%s" % facts["CatalogFiles"])
        if check_port5935:
            self.assert_true(asserts, "TCP 5935 可重绑（无端口残留）", self.port_rebindable(5935), "rebind=OK")

    # ---------- ping/pong 回环 ----------
    @staticmethod
    def find_ping_pong_pair(pc_all, dev_all, which="first"):
        dev_pings = [e for e in dev_all if e.get("event") == "ping" and e.get("data", {}).get("direction") == "tx"]
        pc_pings = [e for e in pc_all if e.get("event") == "ping" and e.get("data", {}).get("direction") == "rx"]
        pc_pongs = [e for e in pc_all if e.get("event") == "pong" and e.get("data", {}).get("direction") == "tx"]
        dev_pongs = [e for e in dev_all if e.get("event") == "pong" and e.get("data", {}).get("direction") == "rx"]
        pc_ping_seqs = {e["data"]["sequence"] for e in pc_pings}
        pc_pong_seqs = {e["data"]["sequence"] for e in pc_pongs}
        dev_pong_seqs = {e["data"]["sequence"] for e in dev_pongs}
        matched = [p for p in dev_pings
                   if p["data"]["sequence"] in pc_ping_seqs
                   and p["data"]["sequence"] in pc_pong_seqs
                   and p["data"]["sequence"] in dev_pong_seqs]
        if not matched:
            return None
        return matched[-1] if which == "last" else matched[0]

    def wait_ping_pong_pair(self, pc_path, dev_path, timeout_ms, which="first", after_time_ms=None):
        deadline = time.time() + timeout_ms / 1000.0
        while time.time() < deadline:
            pair = self.find_ping_pong_pair(self.read_events(pc_path), self.read_events(dev_path), which)
            if pair is not None and (after_time_ms is None or pair["time_ms"] >= after_time_ms - 2000):
                return pair
            time.sleep(0.2)
        return None

    # ---------- 场景实现 ----------
    def scenario_a(self):
        asserts = []
        passed = False
        try:
            pc_contract = self.find_contract(self.args.pc_exe, self.args.pc_contract)
            dev_contract = self.find_contract(self.args.device_exe, self.args.device_contract)
            self.assert_true(asserts, "找到 PC protocol-contract.json", pc_contract != "", "-PcContract 或 exe 旁 share/ 未找到")
            self.assert_true(asserts, "找到设备 protocol-contract.json", dev_contract != "", "-DeviceContract 或 exe 旁 share/ 未找到")
            golden = self.load_json(GOLDEN)
            pc = self.load_json(pc_contract)
            dev = self.load_json(dev_contract)
            d1, d2, d3 = [], [], []
            self.json_cmp(pc, golden, "pc", d1)
            self.json_cmp(dev, golden, "device", d2)
            self.json_cmp(pc, dev, "pc_vs_device", d3)
            diff = {"pc_vs_golden": d1, "device_vs_golden": d2, "pc_vs_device": d3}
            self.write_utf8_no_bom(os.path.join(self.dir, "protocol-diff.json"), json.dumps(diff, ensure_ascii=False, indent=2))
            self.assert_true(asserts, "三份契约 schema_version 均为 2",
                             pc.get("schema_version") == 2 and dev.get("schema_version") == 2 and golden.get("schema_version") == 2,
                             "pc=%s dev=%s golden=%s" % (pc.get("schema_version"), dev.get("schema_version"), golden.get("schema_version")))
            self.assert_true(asserts, "PC contract == golden（忽略 generated_from）", len(d1) == 0, "diffs=%d" % len(d1))
            self.assert_true(asserts, "Device contract == golden（忽略 generated_from）", len(d2) == 0, "diffs=%d" % len(d2))
            self.assert_true(asserts, "PC contract == Device contract", len(d3) == 0, "diffs=%d" % len(d3))
            passed = True
        except Exception as e:
            self.log("  场景 a 异常: %s" % e)
        self.write_summary("a", passed, asserts, "pc=%s device=%s" % (pc_contract if "pc_contract" in dir() else "", dev_contract if "dev_contract" in dir() else ""))
        return passed

    def scenario_b(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "b_hotspot_session.json")
            ctx = self.start_dual(scn, 25, 25, "Modu_PC")
            self.assert_true(asserts, "PC ready 出现", len(self.wait_events(ctx["pcEvents"], "ready", "pc", 1, 15000)) >= 1, "缺失")
            self.assert_true(asserts, "PC session_online 出现", len(self.wait_events(ctx["pcEvents"], "session_online", "pc", 1, 25000)) >= 1, "缺失")
            dev_online = self.wait_events(ctx["DevEvents"], "session_online", "device", 1, 25000)
            self.assert_true(asserts, "设备 session_online 出现", len(dev_online) >= 1, "缺失")

            pc_evs = self.read_events(ctx["pcEvents"])
            hs = next((e for e in pc_evs if e.get("event") == "hotspot_ready"), None)
            tc = next((e for e in pc_evs if e.get("event") == "tcp_listening"), None)
            rd = next((e for e in pc_evs if e.get("event") == "ready"), None)
            self.assert_true(asserts, "PC 事件顺序 hotspot_ready→tcp_listening→ready",
                             hs and tc and rd and hs["seq"] < tc["seq"] < rd["seq"],
                             "seq hs=%s tc=%s rd=%s" % (hs and hs.get("seq"), tc and tc.get("seq"), rd and rd.get("seq")))
            self.assert_true(asserts, "hotspot_ready 数据 ssid=Modu_PC/ip=192.168.137.1/prefix=24",
                             hs and hs["data"].get("ssid") == "Modu_PC" and hs["data"].get("ip") == "192.168.137.1" and hs["data"].get("prefix_length") == 24,
                             "data=%s" % json.dumps(hs["data"], ensure_ascii=False) if hs else "缺失")

            dev_evs = self.read_events(ctx["DevEvents"])
            order = ["ready", "wifi_scan_started", "wifi_target_found", "wifi_connected", "tcp_connecting", "session_online"]
            prev = -1
            order_ok = True
            seqs = []
            for n in order:
                ev = next((e for e in dev_evs if e.get("event") == n), None)
                if ev is None or ev["seq"] <= prev:
                    order_ok = False
                seqs.append("%s=%s" % (n, ev.get("seq") if ev else "缺失"))
                if ev:
                    prev = ev["seq"]
            self.assert_true(asserts, "设备事件顺序 ready→scan→found→connected→connecting→online", order_ok, " ".join(seqs))
            wc = next((e for e in dev_evs if e.get("event") == "wifi_connected"), None)
            self.assert_true(asserts, "wifi_connected 数据 ssid=Modu_PC 且含 device_ip",
                             wc and wc["data"].get("ssid") == "Modu_PC" and wc["data"].get("device_ip"),
                             "data=%s" % json.dumps(wc["data"], ensure_ascii=False) if wc else "缺失")
            tc2 = next((e for e in dev_evs if e.get("event") == "tcp_connecting"), None)
            self.assert_true(asserts, "tcp_connecting 数据 host=192.168.137.1/port=5935",
                             tc2 and tc2["data"].get("host") == "192.168.137.1" and tc2["data"].get("port") == 5935,
                             "data=%s" % json.dumps(tc2["data"], ensure_ascii=False) if tc2 else "缺失")

            do = dev_online[0]
            pair = self.wait_ping_pong_pair(ctx["pcEvents"], ctx["DevEvents"], 12000, "first")
            in_window = pair is not None and (pair["time_ms"] - do["time_ms"]) <= 10000
            self.assert_true(asserts, "session_online 后 10 秒内出现完整 ping/pong 回环",
                             pair is not None and in_window,
                             "pair_seq=%s pair_time=%s online_time=%s" % (pair["data"]["sequence"], pair["time_ms"], do["time_ms"]) if pair else "无完整回环")

            facts = self.finish_processes(ctx, ["pcSide", "DevSide"], ["pcEvents", "DevEvents"], ["PC", "Device"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device"])
            passed = True
        except Exception as e:
            self.log("  场景 b 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("b", passed, asserts)
        return passed

    def scenario_c(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "c_pc_stop.json")
            ctx = self.start_dual(scn, 25, 25, "Modu_PC")
            self.assert_true(asserts, "PC session_online 出现", len(self.wait_events(ctx["pcEvents"], "session_online", "pc", 1, 30000)) >= 1, "缺失")
            self.assert_true(asserts, "设备 session_online 出现", len(self.wait_events(ctx["DevEvents"], "session_online", "device", 1, 30000)) >= 1, "缺失")
            stop = self.wait_scenario_result(ctx["pcEvents"], "c_pc_stop", 20000)
            self.assert_true(asserts, "PC request_stop 动作执行（scenario_result ok）",
                             stop is not None and stop["data"].get("status") == "ok",
                             "result=%s" % json.dumps(stop["data"], ensure_ascii=False) if stop else "scenario_result 缺失")
            cat_gone = False
            for _ in range(25):
                if not os.path.exists(os.path.join(ctx["catalog"], "pc-hotspot.json")):
                    cat_gone = True
                    break
                time.sleep(0.2)
            self.assert_true(asserts, "PC 停止后 pc-hotspot.json catalog 被删除", cat_gone, "catalog 仍存在")
            dev_off = self.wait_events(ctx["DevEvents"], "session_offline", "device", 1, 20000)
            self.assert_true(asserts, "设备 session_offline 出现",
                             len(dev_off) >= 1, "reason=%s" % dev_off[0]["data"].get("reason") if dev_off else "缺失")
            dev_all = self.read_events(ctx["DevEvents"])
            forbidden = {"ap_ready", "auth_result", "wifi_result", "close_ap_sent", "host_announce", "host_bye"}
            bad = [e for e in dev_all if e.get("event") in forbidden]
            self.assert_true(asserts, "设备事件流不含配网/热点事件", len(bad) == 0, "found=%s" % ",".join(e["event"] for e in bad))
            facts = self.finish_processes(ctx, ["pcSide", "DevSide"], ["pcEvents", "DevEvents"], ["PC", "Device"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device"])
            passed = True
        except Exception as e:
            self.log("  场景 c 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("c", passed, asserts,
                           "注：sim 后端 STA 状态在 catalog 删除后仍保持有效，设备按设计文档 9.2 进入 HEAL 的 TCP 退避重连（不产生扫描事件），不属端侧缺陷")
        return passed

    def scenario_d(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "d_reconnect.json")
            ctx = self.start_dual(scn, 40, 40, "Modu_PC")
            on1pc = self.wait_events(ctx["pcEvents"], "session_online", "pc", 1, 30000)
            on1dev = self.wait_events(ctx["DevEvents"], "session_online", "device", 1, 30000)
            self.assert_true(asserts, "首次 session_online（PC）", len(on1pc) >= 1, "缺失")
            self.assert_true(asserts, "首次 session_online（设备）", len(on1dev) >= 1, "缺失")
            fault = self.wait_scenario_result(ctx["DevEvents"], "d_fault", 20000)
            self.assert_true(asserts, "设备注入 sock_send_fail 成功",
                             fault is not None and fault["data"].get("status") == "ok" and fault["data"].get("action") == "inject_fault",
                             "result=%s" % json.dumps(fault["data"], ensure_ascii=False) if fault else "scenario_result 缺失")
            self.assert_true(asserts, "注入后 PC session_offline", len(self.wait_events(ctx["pcEvents"], "session_offline", "pc", 1, 25000)) >= 1, "缺失")
            self.assert_true(asserts, "注入后设备 session_offline", len(self.wait_events(ctx["DevEvents"], "session_offline", "device", 1, 25000)) >= 1, "缺失")
            restore = self.wait_scenario_result(ctx["DevEvents"], "d_restore", 15000)
            self.assert_true(asserts, "设备恢复 sock_send_fail（count=0）",
                             restore is not None and restore["data"].get("status") == "ok",
                             "result=%s" % json.dumps(restore["data"], ensure_ascii=False) if restore else "scenario_result 缺失")
            on2pc = self.wait_events(ctx["pcEvents"], "session_online", "pc", 2, 30000)
            on2dev = self.wait_events(ctx["DevEvents"], "session_online", "device", 2, 30000)
            self.assert_true(asserts, "PC 重连 session_online（第 2 次）", len(on2pc) >= 2, "count=%d" % len(on2pc))
            self.assert_true(asserts, "设备重连 session_online（第 2 次）", len(on2dev) >= 2, "count=%d" % len(on2dev))
            rc1, rc2 = on1pc[0]["data"].get("reconnect_count", 0), on2pc[1]["data"].get("reconnect_count", 0)
            dc1, dc2 = on1dev[0]["data"].get("reconnect_count", 0), on2dev[1]["data"].get("reconnect_count", 0)
            self.assert_true(asserts, "PC reconnect_count 增长", rc2 > rc1, "pc %s -> %s" % (rc1, rc2))
            self.assert_true(asserts, "设备 reconnect_count 增长", dc2 > dc1, "dev %s -> %s" % (dc1, dc2))
            pair = self.wait_ping_pong_pair(ctx["pcEvents"], ctx["DevEvents"], 15000, "last", on2dev[1]["time_ms"])
            after = pair is not None
            self.assert_true(asserts, "重连后存在完整 ping/pong 回环", after,
                             "pair_seq=%s pair_time=%s online2=%s" % (pair["data"]["sequence"], pair["time_ms"], on2dev[1]["time_ms"]) if pair else "无完整回环")
            facts = self.finish_processes(ctx, ["pcSide", "DevSide"], ["pcEvents", "DevEvents"], ["PC", "Device"], self.args.timeout_seconds + 15)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device"])
            passed = True
        except Exception as e:
            self.log("  场景 d 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("d", passed, asserts)
        return passed

    def scenario_e(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "e_second_device.json")
            ctx = self.start_triple(scn, 25, 25)
            rej = self.wait_events(ctx["pcEvents"], "single_device_rejected", "pc", 1, 30000)
            self.assert_true(asserts, "PC 发出 single_device_rejected", len(rej) >= 1, "缺失")
            self.assert_true(asserts, "single_device_rejected reason=single_device_only",
                             len(rej) >= 1 and rej[0]["data"].get("reason") == "single_device_only",
                             "reason=%s" % rej[0]["data"].get("reason") if rej else "缺失")
            pc_all = self.read_events(ctx["pcEvents"])
            dev2_all = self.read_events(ctx["Dev2Events"])
            dev1_online = [e for e in pc_all if e.get("event") == "session_online" and e.get("device_id") == ctx["Dev1Id"]]
            self.assert_true(asserts, "设备1（%s）在 PC 侧上线" % ctx["Dev1Id"], len(dev1_online) >= 1, "缺失")
            first_rej_seq = rej[0]["seq"]
            first_off = next((e for e in pc_all if e.get("event") == "session_offline" and e.get("device_id") == ctx["Dev1Id"]), None)
            kept = first_off is None or first_off["seq"] > first_rej_seq
            self.assert_true(asserts, "拒绝发生时设备1仍在线（PC 侧无提前 session_offline）", kept,
                             "first_offline_seq=%s reject_seq=%s" % (first_off.get("seq") if first_off else "无", first_rej_seq))
            dev2_online = [e for e in dev2_all if e.get("event") == "session_online"]
            dev2_off = [e for e in dev2_all if e.get("event") == "session_offline"]
            self.assert_true(asserts, "设备2 从未 session_online（busy 拒绝）", len(dev2_online) == 0, "count=%d" % len(dev2_online))
            self.assert_true(asserts, "设备2 收到拒绝后 session_offline（busy 循环）", len(dev2_off) >= 1, "count=%d" % len(dev2_off))
            facts = self.finish_processes(ctx, ["pcSide", "Dev1Side", "Dev2Side"], ["pcEvents", "Dev1Events", "Dev2Events"], ["PC", "Device1", "Device2"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device1", "Device2"])
            passed = True
        except Exception as e:
            self.log("  场景 e 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("e", passed, asserts)
        return passed

    def scenario_f(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "f_no_target.json")
            ctx = self.start_dual(scn, 18, 18, "Modu_Other")  # 目标 SSID 与 PC 发布的不一致
            self.assert_true(asserts, "设备持续 wifi_scan_started",
                             len(self.wait_events(ctx["DevEvents"], "wifi_scan_started", "device", 1, 15000)) >= 1, "缺失")
            dev_all = self.read_events(ctx["DevEvents"])
            pc_all = self.read_events(ctx["pcEvents"])
            never = {"wifi_target_found", "wifi_connected", "tcp_connecting", "wifi_connect_failed", "session_online"}
            for n in never:
                cnt = len([e for e in dev_all if e.get("event") == n])
                self.assert_true(asserts, "设备无 %s" % n, cnt == 0, "count=%d" % cnt)
            self.assert_true(asserts, "PC 无 session_online",
                             len([e for e in pc_all if e.get("event") == "session_online"]) == 0, "出现 session_online")
            forbidden = {"ap_ready", "auth_result", "wifi_result", "close_ap_sent", "host_announce", "host_bye"}
            bad = [e for e in dev_all if e.get("event") in forbidden]
            self.assert_true(asserts, "设备事件流不含任何配网/热点事件", len(bad) == 0, "found=%s" % ",".join(e["event"] for e in bad))
            dev_stop = self.wait_scenario_result(ctx["DevEvents"], "f_dev_stop", 25000)
            self.assert_true(asserts, "设备 request_stop 动作执行（扫描约 15 秒后自停）",
                             dev_stop is not None and dev_stop["data"].get("status") == "ok",
                             "result=%s" % json.dumps(dev_stop["data"], ensure_ascii=False) if dev_stop else "scenario_result 缺失")
            facts = self.finish_processes(ctx, ["pcSide", "DevSide"], ["pcEvents", "DevEvents"], ["PC", "Device"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device"])
            passed = True
        except Exception as e:
            self.log("  场景 f 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("f", passed, asserts,
                           "注：配置校验固定密码/SSID 前缀，'错误密码'分支无法经配置注入（密码被校验为产品固定值），本场景采用'目标 SSID 不存在'分支；密码不符语义由端侧 sim 单测覆盖")
        return passed

    def scenario_g(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "g_app_data.json")
            ctx = self.start_dual(scn, 25, 25, "Modu_PC")
            self.assert_true(asserts, "session_online（PC）", len(self.wait_events(ctx["pcEvents"], "session_online", "pc", 1, 30000)) >= 1, "缺失")
            self.assert_true(asserts, "session_online（设备）", len(self.wait_events(ctx["DevEvents"], "session_online", "device", 1, 30000)) >= 1, "缺失")
            scn_json = self.load_json(os.path.join(self.dir, "scenario.json"))
            text_of = {a["id"]: a["args"]["text"] for a in scn_json["actions"] if a["args"].get("text")}

            def wait_data_event(path, name, sha, timeout_ms):
                deadline = time.time() + timeout_ms / 1000.0
                while time.time() < deadline:
                    for ev in self.read_events(path):
                        if ev.get("event") == name and ev.get("data", {}).get("sha256") == sha:
                            return ev
                    time.sleep(0.2)
                return None

            pairs = [
                ("g_pc_512", "pc", "device", "ok"),
                ("g_dev_512", "device", "pc", "ok"),
                ("g_pc_513", "pc", "device", "rejected"),
                ("g_dev_513", "device", "pc", "rejected"),
                ("g_pc_utf8", "pc", "device", "ok"),
                ("g_dev_utf8", "device", "pc", "ok"),
            ]
            for aid, sender, recver, expect in pairs:
                text = text_of[aid]
                sha = self.sha256_hex(text)
                sender_path = ctx["pcEvents"] if sender == "pc" else ctx["DevEvents"]
                recv_path = ctx["pcEvents"] if recver == "pc" else ctx["DevEvents"]
                r = self.wait_scenario_result(sender_path, aid, 20000)
                r_ev = "result=%s" % json.dumps(r["data"], ensure_ascii=False) if r else "scenario_result 缺失"
                if expect == "ok":
                    self.assert_true(asserts, "%s scenario_result=ok" % aid, r is not None and r["data"].get("status") == "ok", r_ev)
                    tx = wait_data_event(sender_path, "app_data_tx", sha, 10000)
                    rx = wait_data_event(recv_path, "app_data_rx", sha, 10000)
                    self.assert_true(asserts, "%s 发送端 app_data_tx sha256 一致" % aid, tx is not None, "sha=%s..." % sha[:16])
                    self.assert_true(asserts, "%s 接收端 app_data_rx sha256 一致" % aid, rx is not None, "sha=%s..." % sha[:16])
                else:
                    self.assert_true(asserts, "%s scenario_result=rejected/payload_too_large/request_bytes=%d" % (aid, len(text)),
                                     r is not None and r["data"].get("status") == "rejected" and r["data"].get("reason") == "payload_too_large" and r["data"].get("request_bytes") == len(text), r_ev)
                    bad_tx = wait_data_event(sender_path, "app_data_tx", sha, 3000)
                    self.assert_true(asserts, "%s 无 app_data_tx（超限不发送）" % aid, bad_tx is None, "意外 tx")
            facts = self.finish_processes(ctx, ["pcSide", "DevSide"], ["pcEvents", "DevEvents"], ["PC", "Device"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC", "Device"])
            passed = True
        except Exception as e:
            self.log("  场景 g 异常: %s" % e)
        if ctx:
            for k in ("DevSide", "Dev1Side", "Dev2Side"):
                if ctx.get(k):
                    self.stop_side(ctx[k])
            self.stop_side(ctx["pcSide"])
        self.write_summary("g", passed, asserts)
        return passed

    # ---------- 帧工具（场景 h） ----------
    @staticmethod
    def send_frame(sock, payload):
        sock.sendall(struct.pack(">H", len(payload)) + payload)

    @staticmethod
    def read_frame(sock, timeout_s=5.0):
        sock.settimeout(timeout_s)
        try:
            hdr = b""
            while len(hdr) < 2:
                chunk = sock.recv(2 - len(hdr))
                if not chunk:
                    return None
                hdr += chunk
            length = struct.unpack(">H", hdr)[0]
            body = b""
            while len(body) < length:
                chunk = sock.recv(length - len(body))
                if not chunk:
                    return None
                body += chunk
            return body.decode("utf-8")
        except (socket.timeout, OSError):
            return None

    @staticmethod
    def conn_closed(sock, timeout_s=3.0):
        sock.settimeout(timeout_s)
        try:
            data = sock.recv(16)
            return len(data) == 0
        except (socket.timeout, OSError):
            return True

    def scenario_h(self):
        asserts = []
        passed = False
        ctx = None
        try:
            scn = os.path.join(SCENARIOS_DIR, "h_frame_probe.json")
            ctx = self.start_pc(scn, 20)
            pc_evs = self.read_events(ctx["pcEvents"])
            hs = next((e for e in pc_evs if e.get("event") == "hotspot_ready"), None)
            tc = next((e for e in pc_evs if e.get("event") == "tcp_listening"), None)
            rd = next((e for e in pc_evs if e.get("event") == "ready"), None)
            self.assert_true(asserts, "PC 事件顺序 hotspot_ready→tcp_listening→ready",
                             hs and tc and rd and hs["seq"] < tc["seq"] < rd["seq"],
                             "seq hs=%s tc=%s rd=%s" % (hs and hs.get("seq"), tc and tc.get("seq"), rd and rd.get("seq")))

            hello_a1 = '{"cmd":"device_hello","id":"02:00:00:00:00:a1","fw_version":"1.1.0","proto_ver":1,"uptime":1}'
            hello_a2 = '{"cmd":"device_hello","id":"02:00:00:00:00:a2","fw_version":"1.1.0","proto_ver":1,"uptime":1}'
            hello_v2 = '{"cmd":"device_hello","id":"02:00:00:00:00:a3","fw_version":"1.1.0","proto_ver":2,"uptime":1}'
            hello_a4 = '{"cmd":"device_hello","id":"02:00:00:00:00:a4","fw_version":"1.1.0","proto_ver":1,"uptime":1}'

            def connect():
                s = socket.create_connection(("127.0.0.1", 5935), timeout=5)
                return s

            # 1) 拆包：一帧分两次写入
            payload = hello_a1.encode("utf-8")
            full = struct.pack(">H", len(payload)) + payload
            half = len(full) // 2
            s = connect()
            s.sendall(full[:half])
            time.sleep(0.15)
            s.sendall(full[half:])
            resp = self.read_frame(s)
            self.assert_true(asserts, "拆包帧（分两次写入）被完整解析", resp is not None, "无响应")
            j = json.loads(resp) if resp else {}
            self.assert_true(asserts, "拆包 hello → host_ack ok", j.get("cmd") == "host_ack" and j.get("status") == "ok", "resp=%s" % resp)
            s.close()
            time.sleep(0.5)

            # 2) 1025 超长帧头被拒且连接仍可用
            s = connect()
            s.sendall(b"\x04\x01")
            time.sleep(0.15)
            self.send_frame(s, hello_a2.encode("utf-8"))
            resp = self.read_frame(s)
            self.assert_true(asserts, "超长帧头后连接仍可响应合法帧", resp is not None, "无响应")
            j = json.loads(resp) if resp else {}
            self.assert_true(asserts, "超长帧后合法 hello → host_ack ok", j.get("cmd") == "host_ack" and j.get("status") == "ok", "resp=%s" % resp)
            s.close()
            time.sleep(0.5)

            # 3) 0 长度帧 × 3 → malformed_max 关闭
            s = connect()
            for _ in range(3):
                s.sendall(b"\x00\x00")
                time.sleep(0.2)
            closed = self.conn_closed(s)
            s.close()
            self.assert_true(asserts, "0 长度帧 ×3 后连接被 PC 关闭（malformed_max）", closed, "连接仍打开")
            time.sleep(0.5)

            # 4) proto_ver=2 被拒
            s = connect()
            self.send_frame(s, hello_v2.encode("utf-8"))
            resp = self.read_frame(s)
            self.assert_true(asserts, "proto_ver=2 hello 有响应", resp is not None, "无响应")
            j = json.loads(resp) if resp else {}
            self.assert_true(asserts, "proto_ver=2 → host_ack fail/unsupported_protocol",
                             j.get("cmd") == "host_ack" and j.get("status") == "fail" and j.get("reason") == "unsupported_protocol", "resp=%s" % resp)
            closed = self.conn_closed(s)
            s.close()
            self.assert_true(asserts, "proto_ver=2 拒绝后连接被关闭", closed, "连接仍打开")
            time.sleep(0.5)

            # 5) proto_ver=1 接受；粘包：一次写入两帧 ping → 两个 pong
            s = connect()
            self.send_frame(s, hello_a4.encode("utf-8"))
            resp = self.read_frame(s)
            self.assert_true(asserts, "proto_ver=1 hello 有响应", resp is not None, "无响应")
            j = json.loads(resp) if resp else {}
            self.assert_true(asserts, "proto_ver=1 → host_ack ok", j.get("cmd") == "host_ack" and j.get("status") == "ok", "resp=%s" % resp)
            p1 = '{"cmd":"ping","seq":1}'.encode("utf-8")
            p2 = '{"cmd":"ping","seq":2}'.encode("utf-8")
            s.sendall(struct.pack(">H", len(p1)) + p1 + struct.pack(">H", len(p2)) + p2)
            r1 = self.read_frame(s)
            r2 = self.read_frame(s)
            j1 = json.loads(r1) if r1 else {}
            j2 = json.loads(r2) if r2 else {}
            seqs = []
            if j1.get("cmd") == "pong":
                seqs.append(j1.get("seq"))
            if j2.get("cmd") == "pong":
                seqs.append(j2.get("seq"))
            self.assert_true(asserts, "粘包：一次写入 2 帧 ping → 2 个 pong 回显（seq=1,2）",
                             len(seqs) == 2 and 1 in seqs and 2 in seqs, "pongs=%s" % seqs)
            s.close()

            facts = self.finish_processes(ctx, ["pcSide"], ["pcEvents"], ["PC"], self.args.timeout_seconds + 10)
            self.add_cleanup_assertions(asserts, facts, ["PC"])
            passed = True
        except Exception as e:
            self.log("  场景 h 异常: %s" % e)
        if ctx:
            self.stop_side(ctx["pcSide"])
        self.write_summary("h", passed, asserts, "PC-only 原始 socket 探针（无真实设备，避免占用唯一连接槽）")
        return passed

    # ---------- 主入口 ----------
    def run(self, scenario):
        names = SCENARIO_NAMES if scenario == "all" else [s.strip() for s in scenario.split(",")]
        for n in names:
            if n not in SCENARIO_NAMES:
                raise SystemExit("未知场景: %s（可选 a,b,c,d,e,f,g,h 或 all）" % n)
        failed = []
        for name in names:
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            self.dir = os.path.join(self.run_root, "%s_%s" % (stamp, name))
            os.makedirs(self.dir, exist_ok=True)
            os.makedirs(os.path.join(self.dir, "sim-catalog"), exist_ok=True)
            os.makedirs(os.path.join(self.dir, "logs"), exist_ok=True)
            self.log("==== 场景 %s @ %s ====" % (name, self.dir))
            try:
                passed = getattr(self, "scenario_%s" % name)()
            except Exception as e:
                self.log("  场景 %s 异常: %s" % (name, e))
                passed = False
                try:
                    for proc in self._leftover_procs():
                        proc.kill()
                except Exception:
                    pass
            if passed:
                self.log("  场景 %s 通过" % name)
            else:
                self.log("  场景 %s 失败" % name)
                failed.append(name)
        summary = {
            "timestamp": datetime.now().isoformat(),
            "passed": [n for n in names if n not in failed],
            "failed": failed,
            "runtime_root": self.run_root,
        }
        self.write_utf8_no_bom(os.path.join(self.run_root, "integration-summary.json"), json.dumps(summary, ensure_ascii=False, indent=2))
        if failed:
            self.log("失败场景: %s" % ",".join(failed))
            return 1
        self.log("全部场景通过")
        return 0

    def _leftover_procs(self):
        import psutil  # noqa: F401  # 延迟导入；无 psutil 时返回空
        out = []
        try:
            import psutil
            for p in psutil.process_iter(["name"]):
                if p.info["name"] and "provision" in p.info["name"].lower():
                    out.append(p)
        except ImportError:
            pass
        return out


def main():
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:
            pass
    parser = argparse.ArgumentParser(description="跨端集成测试 runner（beta v1.1）")
    parser.add_argument("--scenario", default="all", help="a,b,c,d,e,f,g,h 或 all")
    parser.add_argument("--pc-exe", required=True)
    parser.add_argument("--device-exe", required=True)
    parser.add_argument("--pc-contract", default="")
    parser.add_argument("--device-contract", default="")
    parser.add_argument("--pc-config", default="")
    parser.add_argument("--device-config", default="")
    parser.add_argument("--runtime-root", default="")
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument("--qt-bin", default="")
    args = parser.parse_args()
    runner = Runner(args)
    sys.exit(runner.run(args.scenario))


if __name__ == "__main__":
    main()
