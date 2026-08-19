#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include "sim_nvs.h"

namespace {

std::string MakeTempDir(const char *prefix)
{
    static int counter = 0;
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                (std::string(prefix) + "_" + std::to_string(counter++));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

std::string MakeNvsFile(const std::string &dir, const char *name)
{
    return dir + "/" + name;
}

} // namespace

TEST(SimNvs, SetGetRoundtrip)
{
    std::string dir = MakeTempDir("simnvs");
    sim_nvs_t *nvs = sim_nvs_open(MakeNvsFile(dir, "dev0.nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    const char *data = "{\"ssid\":\"TestWifi\",\"confirmed\":0}";
    EXPECT_EQ(sim_nvs_set(nvs, "wifi_creds", (const uint8_t *)data, (int)strlen(data)), DEMO_OK);
    uint8_t buf[128];
    int len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "wifi_creds", buf, len, &len), DEMO_OK);
    EXPECT_EQ(len, (int)strlen(data));
    EXPECT_EQ(memcmp(buf, data, (size_t)len), 0);
    sim_nvs_close(nvs);
}

TEST(SimNvs, KeyNotFound)
{
    std::string dir = MakeTempDir("simnvs");
    sim_nvs_t *nvs = sim_nvs_open(MakeNvsFile(dir, "dev0.nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    uint8_t buf[64];
    int len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "no_such_key", buf, len, &len), DEMO_ERR);
    EXPECT_EQ(len, 0);
    sim_nvs_close(nvs);
}

TEST(SimNvs, OverwriteAndMultiKey)
{
    std::string dir = MakeTempDir("simnvs");
    sim_nvs_t *nvs = sim_nvs_open(MakeNvsFile(dir, "dev0.nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    const char *v1 = "value-one";
    const char *v2 = "value-two-longer";
    EXPECT_EQ(sim_nvs_set(nvs, "k1", (const uint8_t *)v1, (int)strlen(v1)), DEMO_OK);
    EXPECT_EQ(sim_nvs_set(nvs, "k2", (const uint8_t *)v2, (int)strlen(v2)), DEMO_OK);
    EXPECT_EQ(sim_nvs_set(nvs, "k1", (const uint8_t *)v2, (int)strlen(v2)), DEMO_OK);
    uint8_t buf[128];
    int len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "k1", buf, len, &len), DEMO_OK);
    EXPECT_EQ(len, (int)strlen(v2));
    EXPECT_EQ(memcmp(buf, v2, (size_t)len), 0);
    len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "k2", buf, len, &len), DEMO_OK);
    EXPECT_EQ(len, (int)strlen(v2));
    sim_nvs_close(nvs);
}

TEST(SimNvs, Erase)
{
    std::string dir = MakeTempDir("simnvs");
    sim_nvs_t *nvs = sim_nvs_open(MakeNvsFile(dir, "dev0.nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    const char *v = "data";
    EXPECT_EQ(sim_nvs_set(nvs, "k", (const uint8_t *)v, (int)strlen(v)), DEMO_OK);
    EXPECT_EQ(sim_nvs_erase(nvs, "k"), DEMO_OK);
    uint8_t buf[64];
    int len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "k", buf, len, &len), DEMO_ERR);
    EXPECT_EQ(len, 0);
    sim_nvs_close(nvs);
}

TEST(SimNvs, CorruptedFile)
{
    std::string dir = MakeTempDir("simnvs");
    std::string path = MakeNvsFile(dir, "dev0.nvs.json");
    {
        FILE *fp = fopen(path.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("### not json ###", fp);
        fclose(fp);
    }
    sim_nvs_t *nvs = sim_nvs_open(path.c_str());
    ASSERT_NE(nvs, nullptr);
    uint8_t buf[64];
    int len = (int)sizeof(buf);
    /* 不崩溃且返回错误（键不存在路径） */
    int rc = sim_nvs_get(nvs, "k", buf, len, &len);
    EXPECT_TRUE(rc == DEMO_ERR);
    EXPECT_EQ(len, 0);
    /* 覆盖后可恢复 */
    EXPECT_EQ(sim_nvs_set(nvs, "k", (const uint8_t *)"ok", 2), DEMO_OK);
    len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "k", buf, len, &len), DEMO_OK);
    EXPECT_EQ(len, 2);
    sim_nvs_close(nvs);
}

TEST(SimNvs, PersistAcrossInstances)
{
    std::string dir = MakeTempDir("simnvs");
    std::string path = MakeNvsFile(dir, "dev0.nvs.json");
    {
        sim_nvs_t *nvs = sim_nvs_open(path.c_str());
        ASSERT_NE(nvs, nullptr);
        const char *v = "persist-me";
        EXPECT_EQ(sim_nvs_set(nvs, "persist", (const uint8_t *)v, (int)strlen(v)), DEMO_OK);
        sim_nvs_close(nvs);
    }
    {
        sim_nvs_t *nvs = sim_nvs_open(path.c_str());
        ASSERT_NE(nvs, nullptr);
        uint8_t buf[64];
        int len = (int)sizeof(buf);
        EXPECT_EQ(sim_nvs_get(nvs, "persist", buf, len, &len), DEMO_OK);
        EXPECT_EQ(memcmp(buf, "persist-me", 10), 0);
        sim_nvs_close(nvs);
    }
}

TEST(SimNvs, IllegalBase64Fails)
{
    std::string dir = MakeTempDir("simnvs");
    std::string path = MakeNvsFile(dir, "dev0.nvs.json");
    /* 直接构造含非法 base64 值的存储文件 */
    {
        FILE *fp = fopen(path.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("{\"bad\":\"!!!\"}", fp);
        fclose(fp);
    }
    sim_nvs_t *nvs = sim_nvs_open(path.c_str());
    ASSERT_NE(nvs, nullptr);
    uint8_t buf[64];
    int len = (int)sizeof(buf);
    EXPECT_EQ(sim_nvs_get(nvs, "bad", buf, len, &len), DEMO_ERR);
    EXPECT_EQ(len, 0);
    sim_nvs_close(nvs);
}

TEST(SimNvs, BufferTooSmallReportsNeededLength)
{
    std::string dir = MakeTempDir("simnvs");
    sim_nvs_t *nvs = sim_nvs_open(MakeNvsFile(dir, "dev0.nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    const char *data = "0123456789";
    EXPECT_EQ(sim_nvs_set(nvs, "k", (const uint8_t *)data, 10), DEMO_OK);
    uint8_t small[5];
    int len = 5;
    EXPECT_EQ(sim_nvs_get(nvs, "k", small, len, &len), DEMO_ERR); /* 不截断 */
    EXPECT_EQ(len, 10);                                            /* 报所需长度 */
    uint8_t exact[10];
    len = 10;
    EXPECT_EQ(sim_nvs_get(nvs, "k", exact, len, &len), DEMO_OK);
    EXPECT_EQ(len, 10);
    EXPECT_EQ(memcmp(exact, data, 10), 0);
    /* 容量 0 同样报所需长度 */
    len = 0;
    EXPECT_EQ(sim_nvs_get(nvs, "k", nullptr, len, &len), DEMO_ERR);
    EXPECT_EQ(len, 10);
    sim_nvs_close(nvs);
}

TEST(SimNvs, DirectoryNotWritable)
{
    std::string dir = MakeTempDir("simnvs");
    /* 用"父路径是普通文件"制造不可写目录（跨平台确定性） */
    std::string blocker = dir + "/blocker";
    {
        FILE *fp = fopen(blocker.c_str(), "wb");
        ASSERT_NE(fp, nullptr);
        fputs("x", fp);
        fclose(fp);
    }
    sim_nvs_t *nvs = sim_nvs_open((blocker + "/nvs.json").c_str());
    ASSERT_NE(nvs, nullptr);
    EXPECT_EQ(sim_nvs_set(nvs, "k", (const uint8_t *)"v", 1), DEMO_ERR);
    sim_nvs_close(nvs);
}

TEST(SimNvs, Base64EncodeDecodeRoundtrip)
{
    const char *input = "hello world";
    char enc[64];
    size_t enc_len = 0;
    EXPECT_EQ(sim_nvs_base64_encode((const uint8_t *)input, strlen(input),
                                    enc, sizeof(enc), &enc_len),
              DEMO_OK);
    EXPECT_EQ(enc_len, 16u); /* base64 编码长度：4*ceil(11/3) */
    EXPECT_STREQ(enc, "aGVsbG8gd29ybGQ=");
    uint8_t dec[64];
    size_t dec_len = 0;
    EXPECT_EQ(sim_nvs_base64_decode(enc, dec, sizeof(dec), &dec_len), DEMO_OK);
    EXPECT_EQ(dec_len, strlen(input));
    EXPECT_EQ(memcmp(dec, input, dec_len), 0);
}

TEST(SimNvs, Base64StrictValidation)
{
    uint8_t dec[16];
    size_t dec_len = 0;
    /* 长度非 4 倍数 */
    EXPECT_EQ(sim_nvs_base64_decode("aGVsbG8", dec, sizeof(dec), &dec_len), DEMO_ERR);
    /* 非法字符 */
    EXPECT_EQ(sim_nvs_base64_decode("a@@=", dec, sizeof(dec), &dec_len), DEMO_ERR);
    /* '=' 位置非法（组内第 1 个字符） */
    EXPECT_EQ(sim_nvs_base64_decode("=AAA", dec, sizeof(dec), &dec_len), DEMO_ERR);
    EXPECT_EQ(sim_nvs_base64_decode("A===AAAA", dec, sizeof(dec), &dec_len), DEMO_ERR);
    /* padding 数量非法 */
    EXPECT_EQ(sim_nvs_base64_decode("====", dec, sizeof(dec), &dec_len), DEMO_ERR);
    /* 缓冲不足：报所需长度 */
    EXPECT_EQ(sim_nvs_base64_decode("aGVsbG8=", dec, 2, &dec_len), DEMO_ERR);
    EXPECT_EQ(dec_len, 5u);
    /* 合法 */
    EXPECT_EQ(sim_nvs_base64_decode("AA==", dec, sizeof(dec), &dec_len), DEMO_OK);
    EXPECT_EQ(dec_len, 1u);
    EXPECT_EQ(sim_nvs_base64_decode("AAA=", dec, sizeof(dec), &dec_len), DEMO_OK);
    EXPECT_EQ(dec_len, 2u);
    /* 空输入合法（0 字节） */
    EXPECT_EQ(sim_nvs_base64_decode("", dec, sizeof(dec), &dec_len), DEMO_OK);
    EXPECT_EQ(dec_len, 0u);
}
