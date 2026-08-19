#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "pc_scenario.h"
#include "pc_event.h"
#include "cJSON.h"
#include "host_app.h"
#include "sim_backend.h"
#include "sim_world.h"
#include "params.h"
#include "scenario_runner.h"

namespace {

void write_file(const char *path, const char *content)
{
    std::filesystem::create_directories("run");
    FILE *f = fopen(path, "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

std::string make_scenario_file(const char *path, const std::string &text_513)
{
    std::string content =
        "{\"schema\":1,\"actions\":["
        "{\"id\":\"a1\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":0,"
        "\"action\":\"send_app_data\",\"args\":{\"text\":\"" + text_513 + "\"}},"
        "{\"id\":\"a2\",\"target\":\"device\",\"after_event\":\"null\",\"delay_ms\":0,"
        "\"action\":\"request_stop\",\"args\":{}},"
        "{\"id\":\"a3\",\"target\":\"pc\",\"after_event\":\"ready\",\"delay_ms\":10,"
        "\"action\":\"auto_provision\",\"args\":{\"ssid\":\"TactileFactory-2.4G\","
        "\"password\":\"securepass123\"}}"
        "]}";
    write_file(path, content.c_str());
    return content;
}

} // namespace

void emit_gate_event(const char *name)
{
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = name;
    ev.result = "ok";
    pc_events_emit(&ev);
}

struct ScenarioTimingFixture {
    demo_params_t params;
    net_ctx_t *ctx = nullptr;
    void *user = nullptr;
    HostApp *host = nullptr;
    std::thread host_th;

    void Start()
    {
        params_defaults(&params);
        params.device_ap_port_base = 23000;
        SimWorld::Instance().TargetNetworkSet(params.target_ssid, params.target_password, true);
        user = sim_backend_create("host", &params);
        ASSERT_NE(user, nullptr);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), user, nullptr, &c), DEMO_OK);
        ctx = c;
        host = new HostApp(params, ctx);
        ASSERT_EQ(host->Start(), DEMO_OK);
        host_th = std::thread([this] { host->Run(); });
    }

    void Stop()
    {
        if (host) {
            host->RequestStop();
            if (host_th.joinable())
                host_th.join();
            delete host;
            host = nullptr;
        }
        if (ctx)
            net_ctx_destroy(ctx);
        if (user)
            sim_backend_destroy(user);
        ctx = nullptr;
        user = nullptr;
    }
};

TEST(Scenario, ShortDelayRunsWithin500ms)
{
    /* 回归：delay_ms=50 的短延迟 action 必须在 500ms 内执行
     * （修复前延迟相对 runner 进度起算且逐 action 累积，出现秒级漂移） */
    pc_events_close();
    const char *events_path = "run/test_scenario_short_delay.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);

    ScenarioTimingFixture f;
    f.Start();

    const char *scenario_path = "run/test_scenario_short_delay.json";
    write_file(scenario_path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"sd1\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":50,"
               "\"action\":\"request_stop\",\"args\":{}}]}");
    pc_scenario_t scenario;
    ASSERT_EQ(pc_scenario_load(scenario_path, &scenario), DEMO_OK);

    PcScenarioRunner runner(f.host, f.ctx, &scenario);
    const auto t0 = std::chrono::steady_clock::now();
    runner.Run();
    const auto t1 = std::chrono::steady_clock::now();
    const long elapsed_ms =
        (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_LT(elapsed_ms, 500) << "delay_ms=50 的 action 应在 500ms 内执行，实际 "
                               << elapsed_ms << "ms";

    f.Stop();
    std::remove(scenario_path);
    std::remove(events_path);
    pc_events_close();
}

TEST(Scenario, DelayAnchoredToEventArrival)
{
    /* 回归：delay_ms 相对 after_event 首次到达时刻起算（与设备侧引擎一致）。
     * 事件先于 runner 到达时，触发时刻（到达+300ms）早已过去，runner 应立即执行；
     * 修复前会从 runner 轮到时重新等待整个 delay_ms（约 300ms+）。 */
    pc_events_close();
    const char *events_path = "run/test_scenario_anchor.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);
    emit_gate_event("ready");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    ScenarioTimingFixture f;
    f.Start();

    const char *scenario_path = "run/test_scenario_anchor.json";
    write_file(scenario_path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"ga1\",\"target\":\"pc\",\"after_event\":\"ready\","
               "\"delay_ms\":300,\"action\":\"request_stop\",\"args\":{}}]}");
    pc_scenario_t scenario;
    ASSERT_EQ(pc_scenario_load(scenario_path, &scenario), DEMO_OK);

    PcScenarioRunner runner(f.host, f.ctx, &scenario);
    const auto t0 = std::chrono::steady_clock::now();
    runner.Run();
    const auto t1 = std::chrono::steady_clock::now();
    const long elapsed_ms =
        (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_LT(elapsed_ms, 250) << "事件先到达时不应再等待整个 delay_ms，实际 "
                               << elapsed_ms << "ms";

    f.Stop();
    std::remove(scenario_path);
    std::remove(events_path);
    pc_events_close();
}

TEST(Scenario, DelayCountedFromLaterArrival)
{
    /* 事件在 runner 等待期间到达：触发 = 到达时刻 + delay_ms（约 200+300ms） */
    pc_events_close();
    const char *events_path = "run/test_scenario_later.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);

    ScenarioTimingFixture f;
    f.Start();

    const char *scenario_path = "run/test_scenario_later.json";
    write_file(scenario_path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"gl1\",\"target\":\"pc\",\"after_event\":\"session_online\","
               "\"delay_ms\":300,\"action\":\"request_stop\",\"args\":{}}]}");
    pc_scenario_t scenario;
    ASSERT_EQ(pc_scenario_load(scenario_path, &scenario), DEMO_OK);

    PcScenarioRunner runner(f.host, f.ctx, &scenario);
    const auto t0 = std::chrono::steady_clock::now();
    std::thread runner_th([&runner] { runner.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    emit_gate_event("session_online");
    runner_th.join();
    const auto t1 = std::chrono::steady_clock::now();
    const long elapsed_ms =
        (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    EXPECT_GE(elapsed_ms, 350) << "应等待事件到达 + delay_ms，实际 " << elapsed_ms << "ms";
    EXPECT_LT(elapsed_ms, 2500) << "不应出现秒级额外漂移，实际 " << elapsed_ms << "ms";

    f.Stop();
    std::remove(scenario_path);
    std::remove(events_path);
    pc_events_close();
}

TEST(Scenario, ValidateBackendGate)
{
    EXPECT_EQ(pc_scenario_validate_backend(1), DEMO_OK); /* sim 允许 */
    EXPECT_NE(pc_scenario_validate_backend(0), DEMO_OK); /* 非 sim 拒绝 */
}

TEST(Scenario, LoadValid)
{
    const char *path = "run/test_scenario_valid.json";
    write_file(path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"x1\",\"target\":\"pc\",\"after_event\":\"session_online\","
               "\"delay_ms\":100,\"action\":\"inject_fault\","
               "\"args\":{\"name\":\"wifi_disconnect\",\"argument_json\":\"{}\"}}]}");
    pc_scenario_t s;
    EXPECT_EQ(pc_scenario_load(path, &s), DEMO_OK);
    EXPECT_EQ(s.schema, 1);
    EXPECT_EQ(s.action_count, 1);
    EXPECT_STREQ(s.actions[0].id, "x1");
    EXPECT_EQ(s.actions[0].kind, PC_SCENARIO_ACTION_INJECT_FAULT);
    EXPECT_EQ(s.actions[0].delay_ms, 100);
    remove(path);
}

TEST(Scenario, LoadInvalidSchema)
{
    const char *path = "run/test_scenario_bad1.json";
    write_file(path, "{\"schema\":2,\"actions\":[]}");
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load(path, &s), DEMO_OK);
    remove(path);
}

TEST(Scenario, LoadDuplicateIdRejected)
{
    const char *path = "run/test_scenario_bad2.json";
    write_file(path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"dup\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":0,"
               "\"action\":\"request_stop\",\"args\":{}},"
               "{\"id\":\"dup\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":0,"
               "\"action\":\"request_stop\",\"args\":{}}]}");
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load(path, &s), DEMO_OK);
    remove(path);
}

TEST(Scenario, LoadDelayOutOfRangeRejected)
{
    const char *path = "run/test_scenario_bad3.json";
    write_file(path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"d1\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":60001,"
               "\"action\":\"request_stop\",\"args\":{}}]}");
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load(path, &s), DEMO_OK);
    remove(path);
}

TEST(Scenario, LoadRequestStopArgsMustBeEmpty)
{
    const char *path = "run/test_scenario_bad4.json";
    write_file(path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"r1\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":0,"
               "\"action\":\"request_stop\",\"args\":{\"x\":1}}]}");
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load(path, &s), DEMO_OK);
    remove(path);
}

TEST(Scenario, LoadMissingFile)
{
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load("run/not_exist_scenario.json", &s), DEMO_OK);
}

TEST(Scenario, LoadUnknownActionRejected)
{
    const char *path = "run/test_scenario_bad5.json";
    write_file(path,
               "{\"schema\":1,\"actions\":["
               "{\"id\":\"u1\",\"target\":\"pc\",\"after_event\":\"null\",\"delay_ms\":0,"
               "\"action\":\"explode\",\"args\":{}}]}");
    pc_scenario_t s;
    EXPECT_NE(pc_scenario_load(path, &s), DEMO_OK);
    remove(path);
}

TEST(Scenario, Send513RejectedWithoutTxEvent)
{
    /* 513 字节：status=rejected、request_bytes=513、reason=payload_too_large，
     * 且不得出现对应 app_data_tx；目标 device 的 action 被忽略（无 scenario_result）。 */
    pc_events_close();
    const char *events_path = "run/test_scenario_513.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);

    demo_params_t params;
    params_defaults(&params);
    params.device_ap_port_base = 23000;
    SimWorld::Instance().TargetNetworkSet(params.target_ssid, params.target_password, true);
    void *user = sim_backend_create("host", &params);
    ASSERT_NE(user, nullptr);
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(sim_backend_table(), user, nullptr, &ctx), DEMO_OK);
    HostApp host(params, ctx);
    ASSERT_EQ(host.Start(), DEMO_OK);
    std::thread host_th([&host] { host.Run(); });

    pc_event_t ready;
    memset(&ready, 0, sizeof(ready));
    ready.event = "ready";
    ready.result = "ok";
    pc_events_emit(&ready);

    const char *scenario_path = "run/test_scenario_513.json";
    std::string text(513, 'a');
    make_scenario_file(scenario_path, text);
    pc_scenario_t scenario;
    ASSERT_EQ(pc_scenario_load(scenario_path, &scenario), DEMO_OK);

    PcScenarioRunner runner(&host, ctx, &scenario);
    std::thread runner_th([&runner] { runner.Run(); });
    runner_th.join();
    host.RequestStop();
    if (host_th.joinable())
        host_th.join();
    net_ctx_destroy(ctx);
    sim_backend_destroy(user);
    pc_events_close();

    std::ifstream in(events_path);
    std::string line;
    int result_count = 0;
    bool found_rejected_513 = false;
    bool found_ok = false;
    bool found_app_data_tx = false;
    bool found_device_target_result = false;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        cJSON *root = cJSON_Parse(line.c_str());
        ASSERT_NE(root, nullptr) << line;
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        ASSERT_TRUE(cJSON_IsString(event)) << line;
        if (strcmp(event->valuestring, "app_data_tx") == 0)
            found_app_data_tx = true;
        if (strcmp(event->valuestring, "scenario_result") == 0) {
            result_count++;
            const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            ASSERT_TRUE(cJSON_IsObject(data)) << line;
            const cJSON *action_id = cJSON_GetObjectItemCaseSensitive(data, "action_id");
            ASSERT_TRUE(cJSON_IsString(action_id)) << line;
            if (strcmp(action_id->valuestring, "a1") == 0) {
                const cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
                const cJSON *rb = cJSON_GetObjectItemCaseSensitive(data, "request_bytes");
                const cJSON *reason = cJSON_GetObjectItemCaseSensitive(data, "reason");
                EXPECT_TRUE(cJSON_IsString(status) &&
                            strcmp(status->valuestring, "rejected") == 0) << line;
                EXPECT_TRUE(cJSON_IsNumber(rb) && rb->valueint == 513) << line;
                EXPECT_TRUE(cJSON_IsString(reason) &&
                            strcmp(reason->valuestring, "payload_too_large") == 0) << line;
                found_rejected_513 = true;
            } else if (strcmp(action_id->valuestring, "a3") == 0) {
                const cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
                EXPECT_TRUE(cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
                    << line;
                found_ok = true;
            } else if (strcmp(action_id->valuestring, "a2") == 0) {
                found_device_target_result = true;
            }
        }
        cJSON_Delete(root);
    }
    EXPECT_TRUE(found_rejected_513);
    EXPECT_TRUE(found_ok);
    EXPECT_FALSE(found_device_target_result);
    EXPECT_FALSE(found_app_data_tx);
    EXPECT_EQ(result_count, 2);
    std::remove(events_path);
    std::remove(scenario_path);
    pc_events_close();
}
