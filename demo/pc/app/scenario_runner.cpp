#include "scenario_runner.h"
#include "host_app.h"
#include "host_registry.h"
#include "log.h"
#include "pc_event.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

PcScenarioRunner::PcScenarioRunner(HostApp *host, net_ctx_t *net,
                                   const pc_scenario_t *scenario)
    : host_(host), net_(net)
{
    scenario_ = *scenario;
}

PcScenarioRunner::~PcScenarioRunner()
{
    RequestStop();
}

void PcScenarioRunner::RequestStop()
{
    stop_ = true;
}

bool PcScenarioRunner::WaitForEvent(const char *event, int timeout_ms)
{
    if (event == nullptr || strcmp(event, "null") == 0)
        return true;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms <= 0 ? 60000 : timeout_ms);
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
        if (pc_events_was_seen(event))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void PcScenarioRunner::EmitResult(const pc_scenario_action_t &action, const char *status,
                                  int request_bytes, const char *reason)
{
    static const char *kNames[] = {"auto_provision", "send_app_data", "inject_fault",
                                   "request_stop"};
    const char *name = (action.kind >= 0 && action.kind < 4) ? kNames[action.kind] : "unknown";
    char data[512];
    snprintf(data, sizeof(data),
             "{\"action_id\":\"%s\",\"action\":\"%s\",\"status\":\"%s\","
             "\"request_bytes\":%d,\"reason\":\"%s\"}",
             action.id, name, status, request_bytes,
             reason && reason[0] ? reason : "");
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = "scenario_result";
    ev.result = strcmp(status, "ok") == 0 ? "ok" : "fail";
    ev.code = DEMO_OK;
    ev.data_json = data;
    pc_events_emit(&ev);
    LOG_I("SCN", "scenario action=%s id=%s status=%s", name, action.id, status);
}

void PcScenarioRunner::RunAction(const pc_scenario_action_t &action)
{
    switch (action.kind) {
    case PC_SCENARIO_ACTION_AUTO_PROVISION: {
        host_->SetTargetNetwork(action.arg_ssid, action.arg_password);
        host_->ProvisionAllDevices();
        EmitResult(action, "ok", 0, "");
        break;
    }
    case PC_SCENARIO_ACTION_SEND_APP_DATA: {
        size_t len = action.arg_text_len > 0 ? (size_t)action.arg_text_len
                                             : strlen(action.arg_text);
        if (len > 512) {
            EmitResult(action, "rejected", (int)len, "payload_too_large");
            break;
        }
        std::string device_id;
        {
            const auto entries = host_->registry().Snapshot();
            for (const auto &entry : entries) {
                if (entry.state == "online" && entry.conn) {
                    device_id = entry.id;
                    break;
                }
            }
        }
        if (device_id.empty()) {
            EmitResult(action, "failed", (int)len, "no_online_device");
            break;
        }
        int rc = host_->SendAppDataToDevice(device_id, action.arg_text);
        if (rc == DEMO_OK)
            EmitResult(action, "ok", (int)len, "");
        else
            EmitResult(action, "failed", (int)len, "queue_rejected");
        break;
    }
    case PC_SCENARIO_ACTION_INJECT_FAULT: {
        const char *arg = action.arg_fault_json[0] ? action.arg_fault_json : nullptr;
        int rc = net_inject(net_, action.arg_fault_name, arg);
        {
            char data[256];
            snprintf(data, sizeof(data),
                     "{\"action\":\"%s\",\"status\":\"%s\"}",
                     action.arg_fault_name, rc == DEMO_OK ? "ok" : "fail");
            pc_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.event = "fault_applied";
            ev.result = rc == DEMO_OK ? "ok" : "fail";
            ev.code = rc;
            ev.data_json = data;
            pc_events_emit(&ev);
        }
        EmitResult(action, rc == DEMO_OK ? "ok" : "failed", 0,
                   rc == DEMO_OK ? "" : "inject_failed");
        break;
    }
    case PC_SCENARIO_ACTION_REQUEST_STOP: {
        host_->RequestStop();
        EmitResult(action, "ok", 0, "");
        break;
    }
    }
}

/* 等待单调时钟到达 trigger_at_ms（毫秒级；stop 后 ≤5ms 内退出）。 */
void PcScenarioRunner::WaitUntilMs(uint64_t trigger_at_ms)
{
    while (!stop_) {
        const uint64_t now = pc_events_now_ms();
        if (now >= trigger_at_ms)
            return;
        const uint64_t remain = trigger_at_ms - now;
        std::this_thread::sleep_for(
            std::chrono::milliseconds((long)(remain > 5 ? 5 : remain)));
    }
}

void PcScenarioRunner::Run()
{
    for (int i = 0; i < scenario_.action_count && !stop_; ++i) {
        const pc_scenario_action_t &action = scenario_.actions[i];
        if (strcmp(action.target, "pc") != 0)
            continue; /* 目标不匹配：忽略 */
        const char *gate = action.after_event;
        if (gate == nullptr || strcmp(gate, "null") == 0) {
            /* 无门控：delay 从本 action 轮到时起算 */
            WaitUntilMs(pc_events_now_ms() + (uint64_t)action.delay_ms);
        } else {
            /* 门控：delay_ms 相对事件首次到达时刻起算（与设备侧引擎一致），
             * 避免前序 action 阻塞造成的秒级漂移。 */
            uint64_t arrival = pc_events_last_seen_ms(gate);
            if (arrival == 0 && pc_events_was_seen(gate))
                arrival = pc_events_now_ms(); /* 事件在时钟首毫秒内到达（误差 <1ms） */
            if (arrival == 0) {
                if (!WaitForEvent(gate, 60000)) {
                    LOG_W("SCN", "scenario action %s 等待事件 %s 超时，跳过", action.id,
                          gate);
                    EmitResult(action, "failed", 0, "after_event_timeout");
                    continue;
                }
                arrival = pc_events_last_seen_ms(gate);
                if (arrival == 0)
                    arrival = pc_events_now_ms();
            }
            WaitUntilMs(arrival + (uint64_t)action.delay_ms);
        }
        if (stop_)
            break;
        RunAction(action);
    }
    LOG_I("SCN", "scenario 执行完毕");
}
