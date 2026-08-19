#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "cJSON.h"
#include "host_app.h"
#include "sim_backend.h"
#include "params.h"
#include "pc_event.h"
#include "log.h"

namespace {

std::vector<std::string> ReadEventNames(const char *path)
{
    std::vector<std::string> names;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        cJSON *root = cJSON_Parse(line.c_str());
        if (!root)
            continue;
        const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (cJSON_IsString(event))
            names.emplace_back(event->valuestring);
        cJSON_Delete(root);
    }
    return names;
}

struct SimHostFixture {
    demo_params_t params;
    std::string catalog_dir;
    void *user = nullptr;
    net_ctx_t *ctx = nullptr;

    void Init(int port)
    {
        params_defaults(&params);
        params.host_tcp_port = port;
        catalog_dir = "run/sim_host_app_catalog";
        std::filesystem::remove_all(catalog_dir);
        snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s",
                 catalog_dir.c_str());
        user = sim_backend_create("host", &params);
        ASSERT_NE(user, nullptr);
        net_ctx_t *c = nullptr;
        ASSERT_EQ(net_ctx_create(sim_backend_table(), user, nullptr, &c), DEMO_OK);
        ctx = c;
    }

    void Cleanup()
    {
        if (ctx)
            net_ctx_destroy(ctx);
        if (user)
            sim_backend_destroy(user);
        ctx = nullptr;
        user = nullptr;
        std::filesystem::remove_all(catalog_dir);
    }
};

} // namespace

TEST(HostAppSim, HotspotLifecyclePublishesCatalogAndEvents)
{
    pc_events_close();
    const char *events_path = "run/test_host_app_events.jsonl";
    std::remove(events_path);
    ASSERT_EQ(pc_events_open(events_path), DEMO_OK);

    SimHostFixture f;
    f.Init(55951);
    {
        HostApp host(f.params, f.ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);

        const std::string hotspot_file = f.catalog_dir + "/pc-hotspot.json";
        ASSERT_TRUE(std::filesystem::exists(hotspot_file));

        auto names = ReadEventNames(events_path);
        auto hotspot_it = std::find(names.begin(), names.end(), "hotspot_ready");
        auto tcp_it = std::find(names.begin(), names.end(), "tcp_listening");
        ASSERT_NE(hotspot_it, names.end());
        ASSERT_NE(tcp_it, names.end());
        EXPECT_LT(std::distance(names.begin(), hotspot_it),
                  std::distance(names.begin(), tcp_it));

        host.RequestStop();
        EXPECT_FALSE(std::filesystem::exists(hotspot_file));
    }
    f.Cleanup();
    pc_events_close();
    std::remove(events_path);
}

TEST(HostAppSim, AppDataValidation)
{
    SimHostFixture f;
    f.Init(55952);
    {
        HostApp host(f.params, f.ctx);
        ASSERT_EQ(host.Start(), DEMO_OK);
        EXPECT_EQ(host.SendAppDataToDevice("02:00:00:00:00:01", ""), DEMO_ERR);
        EXPECT_EQ(host.SendAppDataToDevice("02:00:00:00:00:01", std::string(513, 'a')),
                  DEMO_ERR);
        EXPECT_EQ(host.SendAppDataToDevice("", "x"), DEMO_ERR);
        EXPECT_EQ(host.SendAppDataToDevice("02:00:00:00:00:01", std::string(512, 'b')),
                  DEMO_OK);
        EXPECT_EQ(host.OnlineCount(), (size_t)0);
        host.RequestStop();
    }
    f.Cleanup();
}
