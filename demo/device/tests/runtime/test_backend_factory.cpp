#include "test_host_fixture.h"

#include "device_backend_factory.h"

#include <cstring>

namespace {

static device_sim_backend_options_t SimOpts()
{
    device_sim_backend_options_t o;
    memset(&o, 0, sizeof(o));
    o.nvs_file = "run/factory_test.nvs.json";
    o.device_index = 0;
    return o;
}

} // namespace

TEST(BackendFactory, SimCreatesFullInstance)
{
    fake_backend_reset();
    device_sim_backend_options_t so = SimOpts();
    device_backend_instance_t inst;
    device_error_t e;
    EXPECT_EQ(device_backend_create_for_host(DEVICE_BACKEND_SIM, &so, nullptr, &inst,
                                             &e),
              DEVICE_OK);
    EXPECT_NE(inst.vtable, nullptr);
    EXPECT_NE(inst.user, nullptr);
    EXPECT_NE(inst.destroy_user, nullptr);
    device_backend_instance_destroy(&inst);
    EXPECT_EQ(inst.vtable, nullptr);
    EXPECT_EQ(inst.user, nullptr);
    EXPECT_EQ(inst.destroy_user, nullptr);
}

TEST(BackendFactory, Esp32NeverSupported)
{
    device_backend_instance_t inst;
    device_error_t e;
    EXPECT_EQ(device_backend_create_for_host(DEVICE_BACKEND_ESP32C2, nullptr, nullptr,
                                             &inst, &e),
              DEVICE_ERR_NOT_SUPPORTED);
    EXPECT_EQ(inst.vtable, nullptr);
}

#ifdef _WIN32
TEST(BackendFactory, LinuxOnWindowsNotSupported)
{
    device_backend_instance_t inst;
    device_error_t e;
    EXPECT_EQ(device_backend_create_for_host(DEVICE_BACKEND_LINUX, nullptr, nullptr,
                                             &inst, &e),
              DEVICE_ERR_NOT_SUPPORTED);
    EXPECT_EQ(inst.vtable, nullptr);
}
#endif

TEST(BackendFactory, NullArgs)
{
    device_sim_backend_options_t so = SimOpts();
    device_error_t e;
    EXPECT_EQ(device_backend_create_for_host(DEVICE_BACKEND_SIM, &so, nullptr, nullptr,
                                             &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(device_backend_create_for_host((device_backend_kind_t)77, &so, nullptr,
                                             nullptr, nullptr),
              DEVICE_ERR_INVALID_ARGUMENT);
}

TEST(BackendFactory, DestroyZeroIdempotent)
{
    device_backend_instance_t inst;
    memset(&inst, 0, sizeof(inst));
    device_backend_instance_destroy(&inst);
    device_backend_instance_destroy(&inst);
    device_backend_instance_destroy(nullptr);
}

TEST(BackendFactory, SimFactoryArgValidation)
{
    device_error_t e;
    device_backend_instance_t inst;
    EXPECT_EQ(device_sim_backend_create(nullptr, &inst, &e),
              DEVICE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(inst.vtable, nullptr);
}
