#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include "cJSON.h"
#include "sim_backend.h"
#include "params.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

const char *kCatalogDir = "run/pc_hotspot_catalog_test";

void write_file(const char *dir, const char *name, const char *content)
{
    std::filesystem::create_directories(dir);
    std::string path = std::string(dir) + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

struct CatalogFixture {
    demo_params_t params;
    void *user = nullptr;
    net_ctx_t *ctx = nullptr;

    void Init()
    {
        params_defaults(&params);
        params.host_tcp_port = 55953;
        snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", kCatalogDir);
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
        std::filesystem::remove_all(kCatalogDir);
    }

    std::string FilePath() const { return std::string(kCatalogDir) + "/pc-hotspot.json"; }
};

} // namespace

TEST(SimCatalog, PublishSchema2AndDelete)
{
    std::filesystem::remove_all(kCatalogDir);
    CatalogFixture f;
    f.Init();
    ASSERT_EQ(net_wifi_ap_start(f.ctx, "Modu_PC", "modu_leventure", nullptr), DEMO_OK);

    const std::string path = f.FilePath();
    ASSERT_TRUE(std::filesystem::exists(path));
    FILE *file = fopen(path.c_str(), "rb");
    ASSERT_NE(file, nullptr);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    std::string text((size_t)size, '\0');
    ASSERT_EQ(fread(&text[0], 1, text.size(), file), (size_t)size);
    fclose(file);

    cJSON *root = cJSON_Parse(text.c_str());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(root, "schema")->valueint, 2);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(root, "ssid")->valuestring, "Modu_PC");
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(root, "password")->valuestring,
                 "modu_leventure");
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(root, "logical_gateway")->valuestring,
                 "192.168.137.1");
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(root, "prefix_length")->valueint, 24);
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(root, "tcp_port")->valueint, 55953);
    EXPECT_STREQ(cJSON_GetObjectItemCaseSensitive(root, "loopback_host")->valuestring,
                 "127.0.0.1");
    EXPECT_EQ(cJSON_GetObjectItemCaseSensitive(root, "loopback_port")->valueint, 55953);
    EXPECT_EQ((int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "owner_pid")),
              (int)getpid());
    cJSON_Delete(root);

    ASSERT_EQ(net_wifi_ap_stop(f.ctx), DEMO_OK);
    EXPECT_FALSE(std::filesystem::exists(path));
    f.Cleanup();
}

TEST(SimCatalog, StatusReflectsStartAndStop)
{
    std::filesystem::remove_all(kCatalogDir);
    CatalogFixture f;
    f.Init();
    net_ap_status_t status;
    EXPECT_EQ(net_wifi_ap_status(f.ctx, &status), DEMO_ERR); /* 未启动 */

    ASSERT_EQ(net_wifi_ap_start(f.ctx, "Modu_PC", "modu_leventure", nullptr), DEMO_OK);
    ASSERT_EQ(net_wifi_ap_status(f.ctx, &status), DEMO_OK);
    EXPECT_EQ(status.started, 1);
    EXPECT_STREQ(status.ssid, "Modu_PC");
    EXPECT_STREQ(status.ipv4, "192.168.137.1");
    EXPECT_EQ(status.prefix_length, 24);

    ASSERT_EQ(net_wifi_ap_stop(f.ctx), DEMO_OK);
    EXPECT_EQ(net_wifi_ap_status(f.ctx, &status), DEMO_ERR);
    f.Cleanup();
}

TEST(SimCatalog, ConfigureIpv4UpdatesStatus)
{
    std::filesystem::remove_all(kCatalogDir);
    CatalogFixture f;
    f.Init();
    ASSERT_EQ(net_wifi_ap_start(f.ctx, "Modu_PC", "modu_leventure", nullptr), DEMO_OK);
    ASSERT_EQ(net_wifi_ap_configure_ipv4(f.ctx, "10.0.0.1", 24), DEMO_OK);
    net_ap_status_t status;
    ASSERT_EQ(net_wifi_ap_status(f.ctx, &status), DEMO_OK);
    EXPECT_STREQ(status.ipv4, "10.0.0.1");
    EXPECT_EQ(status.prefix_length, 24);
    f.Cleanup();
}

TEST(SimCatalog, StopDoesNotDeleteForeignOwnedFile)
{
    std::filesystem::remove_all(kCatalogDir);
    write_file(kCatalogDir, "pc-hotspot.json",
               "{\"schema\":2,\"ssid\":\"Modu_PC\",\"password\":\"modu_leventure\","
               "\"logical_gateway\":\"192.168.137.1\",\"prefix_length\":24,"
               "\"tcp_port\":5935,\"loopback_host\":\"127.0.0.1\",\"loopback_port\":5935,"
               "\"owner_pid\":99999999,\"published_at_ms\":1}");
    CatalogFixture f;
    f.Init();
    ASSERT_EQ(net_wifi_ap_stop(f.ctx), DEMO_OK); /* 幂等停止 */
    EXPECT_TRUE(std::filesystem::exists(f.FilePath())); /* owner 非本进程，不删除 */
    f.Cleanup();
}

TEST(SimCatalog, StartRejectsInvalidArgs)
{
    std::filesystem::remove_all(kCatalogDir);
    CatalogFixture f;
    f.Init();
    EXPECT_EQ(net_wifi_ap_start(f.ctx, nullptr, "modu_leventure", nullptr), DEMO_ERR_INVAL);
    EXPECT_EQ(net_wifi_ap_start(f.ctx, "Modu_PC", nullptr, nullptr), DEMO_ERR_INVAL);
    EXPECT_FALSE(std::filesystem::exists(f.FilePath()));
    f.Cleanup();
}
