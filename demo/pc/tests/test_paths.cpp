#include <gtest/gtest.h>
#include <cstring>
#include "pc_paths.h"
#include "params.h"

TEST(Paths, ResolveAgainstBase)
{
    char out[520];
    EXPECT_EQ(pc_path_resolve("C:/cfg", "sim_catalog", out, sizeof(out)), DEMO_OK);
    EXPECT_STREQ(out, "C:/cfg/sim_catalog");
    EXPECT_EQ(pc_path_resolve("C:/cfg/", "sim_catalog", out, sizeof(out)), DEMO_OK);
    EXPECT_STREQ(out, "C:/cfg/sim_catalog");
}

TEST(Paths, ResolveAbsoluteKeepsPath)
{
    char out[520];
    EXPECT_EQ(pc_path_resolve("C:/cfg", "C:/abs/x", out, sizeof(out)), DEMO_OK);
    EXPECT_STREQ(out, "C:/abs/x");
    EXPECT_EQ(pc_path_resolve("C:/cfg", "/root/x", out, sizeof(out)), DEMO_OK);
    EXPECT_STREQ(out, "/root/x");
}

TEST(Paths, ResolveEmptyInput)
{
    char out[520];
    out[0] = 'x';
    EXPECT_NE(pc_path_resolve("C:/cfg", "", out, sizeof(out)), DEMO_OK);
    EXPECT_STREQ(out, "");
}

TEST(Paths, ParentDir)
{
    char out[520];
    pc_path_parent_dir("C:/a/b/c.json", out, sizeof(out));
    EXPECT_STREQ(out, "C:/a/b");
    pc_path_parent_dir("config/pc_config.json", out, sizeof(out));
    EXPECT_STREQ(out, "config");
    pc_path_parent_dir("pc_config.json", out, sizeof(out));
    EXPECT_STREQ(out, "");
    pc_path_parent_dir("C:/a", out, sizeof(out));
    EXPECT_STREQ(out, "C:");
}

TEST(Paths, AbsoluteResolvesAgainstCwd)
{
    char out[1040];
    EXPECT_EQ(pc_path_absolute("run/x", out, sizeof(out)), DEMO_OK);
    /* 规范化后必须以 '/' 或盘符开头（绝对路径）且不以 run/x 相对形式出现 */
    EXPECT_TRUE(out[0] == '/' || (out[1] == ':' && out[2] == '/'));
    EXPECT_NE(strstr(out, "run/x"), nullptr);
}

TEST(Paths, NormalizeBackslashes)
{
    char out[520];
    pc_path_normalize("C:\\cfg\\sim_catalog\\", out, sizeof(out));
    EXPECT_STREQ(out, "C:/cfg/sim_catalog");
}

TEST(Paths, ConfigRelativePathResolutionFlow)
{
    /* 模拟入口行为：配置目录 C:/cfg，配置内相对路径 sim_catalog_dir=sim_catalog */
    demo_params_t params;
    params_defaults(&params);
    snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "sim_catalog");
    char cfg_dir[520], resolved[520];
    pc_path_parent_dir("C:/cfg/pc_config.json", cfg_dir, sizeof(cfg_dir));
    EXPECT_STREQ(cfg_dir, "C:/cfg");
    EXPECT_EQ(pc_path_resolve(cfg_dir, params.sim_catalog_dir, resolved, sizeof(resolved)),
              DEMO_OK);
    EXPECT_STREQ(resolved, "C:/cfg/sim_catalog");
}
