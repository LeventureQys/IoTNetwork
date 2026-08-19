#include <gtest/gtest.h>
#include <cstring>

#include "device_host.h"

namespace {

static char g_argv_buf[32][512];
static char *g_argv[40];
static int g_argc = 0;

/* 用独立生命周期构造 argv（options 只借用指针） */
static void SetupArgv(std::initializer_list<const char *> args)
{
    g_argc = 0;
    g_argv[g_argc++] = (char *)"provision_device";
    for (const char *a : args) {
        int idx = g_argc;
        snprintf(g_argv_buf[idx], sizeof(g_argv_buf[idx]), "%s", a);
        g_argv[idx] = g_argv_buf[idx];
        g_argc = idx + 1;
    }
    g_argv[g_argc] = nullptr;
}

} // namespace

TEST(HostOptions, InitDefaults)
{
    device_host_options_t o;
    device_host_options_init(&o);
    EXPECT_EQ(o.backend, DEVICE_BACKEND_SIM);
    EXPECT_EQ(o.device_index, 0u);
    EXPECT_EQ(o.fresh, 0);
    EXPECT_EQ(o.duration_seconds, 0u);
    EXPECT_EQ(o.config_path, nullptr);
    EXPECT_EQ(o.runtime_dir, nullptr);
}

TEST(HostOptions, ParseNoArgs)
{
    SetupArgv({});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.backend, DEVICE_BACKEND_SIM);
    EXPECT_EQ(o.device_index, 0u);
}

TEST(HostOptions, ParseBasicFlags)
{
    SetupArgv({"--config", "cfg.json", "--device-index", "7", "--fresh",
               "--duration", "30"});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_STREQ(o.config_path, "cfg.json");
    EXPECT_EQ(o.device_index, 7u);
    EXPECT_EQ(o.fresh, 1);
    EXPECT_EQ(o.duration_seconds, 30u);
}

TEST(HostOptions, ParseBackendValues)
{
    SetupArgv({"--backend", "sim"});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.backend, DEVICE_BACKEND_SIM);

    SetupArgv({"--backend", "LINUX"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.backend, DEVICE_BACKEND_LINUX);

    SetupArgv({"--backend", "Esp32c2"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.backend, DEVICE_BACKEND_ESP32C2);

    SetupArgv({"--backend", "wifi"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(HostOptions, ParseIndexRange)
{
    device_host_options_t o;
    device_error_t e;
    SetupArgv({"--device-index", "0"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    SetupArgv({"--device-index", "15"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.device_index, 15u);
    SetupArgv({"--device-index", "16"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    SetupArgv({"--device-index", "-1"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    SetupArgv({"--device-index", "abc"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(HostOptions, ParseDurationValidation)
{
    device_host_options_t o;
    device_error_t e;
    SetupArgv({"--duration", "0"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    SetupArgv({"--duration", "120"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_EQ(o.duration_seconds, 120u);
    SetupArgv({"--duration", "-5"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    SetupArgv({"--duration", "x"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(HostOptions, ParseAllPaths)
{
    SetupArgv({"--runtime-dir", "rt", "--sim-catalog-dir", "cat",
               "--log-dir", "logs", "--events-jsonl", "ev.jsonl",
               "--scenario", "sc.json"});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e), DEVICE_OK);
    EXPECT_STREQ(o.runtime_dir, "rt");
    EXPECT_STREQ(o.sim_catalog_dir, "cat");
    EXPECT_STREQ(o.log_dir, "logs");
    EXPECT_STREQ(o.events_jsonl_path, "ev.jsonl");
    EXPECT_STREQ(o.scenario_path, "sc.json");
}

TEST(HostOptions, UnknownOptionRejected)
{
    SetupArgv({"--bogus", "1"});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(e.code, DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_NE(strlen(e.message), 0u);
}

TEST(HostOptions, MissingValueRejected)
{
    SetupArgv({"--config"});
    device_host_options_t o;
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    SetupArgv({"--duration"});
    EXPECT_EQ(device_host_options_parse_argv(&o, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(HostOptions, NullOptionsRejected)
{
    SetupArgv({});
    device_error_t e;
    EXPECT_EQ(device_host_options_parse_argv(nullptr, g_argc, g_argv, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
}
