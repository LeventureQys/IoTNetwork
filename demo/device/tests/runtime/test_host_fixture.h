#ifndef TEST_HOST_FIXTURE_H
#define TEST_HOST_FIXTURE_H

#include "device_host.h"
#include "device_host_internal.h"
#include "fake_backend.h"
#include "cJSON.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

/* 运行时测试公共工具：临时目录、配置写入、host 生命周期、状态等待。 */

static inline std::string TmpDir(const char *tag)
{
    return std::string("ss03_tmp_") + tag;
}

static inline void CleanDir(const std::string &dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

static inline void WriteFile(const std::string &path, const char *content)
{
    FILE *f = fopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fputs(content, f);
    fclose(f);
}

static inline void WriteConfig(const std::string &dir, const char *content)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    WriteFile(dir + "/device_sim.json", content);
}

/* 在 <dir>/run/dev<idx>.nvs.json 预置凭据（模拟端假后端读 NVS 文件；
 * 值经 cJSON 转义，格式与真实 NVS blob 一致） */
static inline void PreseedCreds(const std::string &dir, unsigned int idx,
                                const char *creds_json)
{
    std::error_code ec;
    std::filesystem::create_directories(dir + "/run", ec);
    cJSON *root = cJSON_CreateObject();
    ASSERT_NE(root, nullptr);
    cJSON_AddStringToObject(root, "wifi_creds", creds_json);
    char *s = cJSON_PrintUnformatted(root);
    ASSERT_NE(s, nullptr);
    WriteFile(dir + "/run/dev" + std::to_string(idx) + ".nvs.json", s);
    free(s);
    cJSON_Delete(root);
}

/* config_path 字符串必须存活到 create 之后（host 在 create 内复制）；
 * 由调用方提供的 cfg_path 持有。 */
static inline device_host_options_t MakeOptions(const std::string &dir,
                                                std::string *cfg_path)
{
    device_host_options_t o;
    device_host_options_init(&o);
    *cfg_path = dir + "/device_sim.json";
    o.config_path = cfg_path->c_str();
    return o;
}

static inline device_host_t *CreateHost(const device_host_options_t &o,
                                        device_result_t expect = DEVICE_OK)
{
    device_host_t *h = nullptr;
    device_error_t e;
    device_result_t rc = device_host_create(&o, &h, &e);
    EXPECT_EQ(rc, expect);
    if (expect != DEVICE_OK) {
        EXPECT_EQ(h, nullptr);
        return nullptr;
    }
    EXPECT_NE(h, nullptr);
    return h;
}

static inline void DestroyHost(device_host_t **h)
{
    device_error_t e;
    device_result_t rc = device_host_destroy(h, &e);
    EXPECT_EQ(rc, DEVICE_OK);
    EXPECT_EQ(*h, nullptr);
}

/* 轮询快照直到 device_state == target；超时返回 false */
static inline bool WaitDeviceState(device_host_t *host, int target, int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        device_snapshot_t s;
        if (device_host_get_snapshot(host, &s) == DEVICE_OK && s.device_state == target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static inline bool WaitHostState(device_host_t *host, device_host_state_t target,
                                 int timeout_ms)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        device_host_state_t s;
        if (device_host_get_state(host, &s) == DEVICE_OK && s == target)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

#endif
