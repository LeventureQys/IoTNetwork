#include "host_app.h"
#include "host_window.h"
#include "sim_backend.h"
#include "win_backend.h"
#include "log.h"
#include "params.h"
#include "pc_event.h"
#include "pc_paths.h"
#include "pc_scenario.h"
#include "scenario_runner.h"

#include <QApplication>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ctime>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

const char *kDefaultConfigRel = "config/pc_config.json";

struct CliOptions {
    const char *config = nullptr;
    const char *runtime_dir = nullptr;
    const char *sim_catalog_dir = nullptr;
    const char *log_dir = nullptr;
    const char *events_jsonl = nullptr;
    const char *scenario = nullptr;
    int duration = 0;
    int backend = -1; /* 0=sim 1=windows；-1=平台默认 */
};

bool parse_backend(const char *value, int *out)
{
    if (value == nullptr)
        return false;
    if (strcmp(value, "sim") == 0) {
        *out = 0;
        return true;
    }
    if (strcmp(value, "windows") == 0) {
        *out = 1;
        return true;
    }
    return false;
}

int parse_cli(int argc, char **argv, CliOptions *opts)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            opts->config = argv[++i];
        else if (strcmp(argv[i], "--runtime-dir") == 0 && i + 1 < argc)
            opts->runtime_dir = argv[++i];
        else if (strcmp(argv[i], "--sim-catalog-dir") == 0 && i + 1 < argc)
            opts->sim_catalog_dir = argv[++i];
        else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc)
            opts->log_dir = argv[++i];
        else if (strcmp(argv[i], "--events-jsonl") == 0 && i + 1 < argc)
            opts->events_jsonl = argv[++i];
        else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc)
            opts->scenario = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            opts->duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sim") == 0)
            opts->backend = 0;
        else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            int parsed = -1;
            if (!parse_backend(argv[++i], &parsed))
                return 1;
            opts->backend = parsed;
        }
    }
    return 0;
}

bool get_exe_dir(char *out, size_t cap)
{
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        return false;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0)
        return false;
    out[n] = '\0';
#endif
    char dir[520];
    pc_path_parent_dir(out, dir, sizeof(dir));
    snprintf(out, cap, "%s", dir);
    return true;
}

const char *find_default_config(char *buffer, size_t cap)
{
    const char *env = getenv("MODUTECH_PC_CONFIG");
    if (env && env[0])
        return env;
    char exe_dir[520];
    if (get_exe_dir(exe_dir, sizeof(exe_dir))) {
        if (snprintf(buffer, cap, "%s/%s", exe_dir, kDefaultConfigRel) < (int)cap)
            return buffer;
    }
    return nullptr;
}

void emit_error(const char *operation, const char *message)
{
    char data[512];
    snprintf(data, sizeof(data), "{\"operation\":\"%s\",\"message\":\"%s\"}",
             operation, message);
    pc_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event = "error";
    ev.result = "fail";
    ev.code = DEMO_ERR;
    ev.data_json = data;
    pc_events_emit(&ev);
    LOG_E("MAIN", "%s：%s", operation, message);
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    CliOptions opts;
    if (parse_cli(argc, argv, &opts) != 0) {
        fprintf(stderr,
                "用法: provision_pc [--config <path>] [--backend sim|windows|--sim] "
                "[--duration <sec>] [--runtime-dir <dir>] "
                "[--sim-catalog-dir <dir>] [--log-dir <dir>] [--events-jsonl <path>] "
                "[--scenario <path>]\n");
        return 1;
    }

#ifdef _WIN32
    int backend_kind = opts.backend >= 0 ? opts.backend : 1; /* Windows 默认 windows */
#else
    int backend_kind = opts.backend >= 0 ? opts.backend : 0; /* Linux 默认 sim */
    if (backend_kind == 1) {
        fprintf(stderr, "警告：Linux 下真实 WiFi 后端不可用，已回退到模拟模式（--sim）\n");
        backend_kind = 0;
    }
#endif

    /* CLI 相对路径按启动 CWD 规范化为绝对路径 */
    char cli_runtime[520] = {0}, cli_catalog[520] = {0}, cli_log[520] = {0};
    char cli_events[520] = {0}, cli_scenario[520] = {0}, cli_config[520] = {0};
    if (opts.runtime_dir && pc_path_absolute(opts.runtime_dir, cli_runtime, sizeof(cli_runtime)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --runtime-dir：%s\n", opts.runtime_dir);
        return 1;
    }
    if (opts.sim_catalog_dir && pc_path_absolute(opts.sim_catalog_dir, cli_catalog, sizeof(cli_catalog)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --sim-catalog-dir：%s\n", opts.sim_catalog_dir);
        return 1;
    }
    if (opts.log_dir && pc_path_absolute(opts.log_dir, cli_log, sizeof(cli_log)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --log-dir：%s\n", opts.log_dir);
        return 1;
    }
    if (opts.events_jsonl && pc_path_absolute(opts.events_jsonl, cli_events, sizeof(cli_events)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --events-jsonl：%s\n", opts.events_jsonl);
        return 1;
    }
    if (opts.scenario && pc_path_absolute(opts.scenario, cli_scenario, sizeof(cli_scenario)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --scenario：%s\n", opts.scenario);
        return 1;
    }
    if (opts.config && pc_path_absolute(opts.config, cli_config, sizeof(cli_config)) != DEMO_OK) {
        fprintf(stderr, "无法解析 --config：%s\n", opts.config);
        return 1;
    }

    /* 配置定位：--config → MODUTECH_PC_CONFIG → 可执行部署目录 config/pc_config.json → 内置默认 */
    char default_config[520] = {0};
    const char *config_path = nullptr;
    const char *deployment_config = find_default_config(default_config, sizeof(default_config));
    if (cli_config[0])
        config_path = cli_config;
    else if (deployment_config)
        config_path = deployment_config;

    demo_params_t params;
    bool config_missing = false;
    if (config_path) {
        int rc = params_load(&params, config_path);
        if (rc == DEMO_OK) {
            FILE *probe = fopen(config_path, "rb");
            if (probe) {
                fclose(probe);
            } else {
                config_missing = true;
                fprintf(stderr, "警告：配置文件不存在，使用内置默认参数：%s\n", config_path);
                config_path = nullptr;
            }
        } else {
            fprintf(stderr, "配置文件解析失败：%s\n", config_path);
            return 1;
        }
    } else {
        params_defaults(&params);
        fprintf(stderr, "警告：未找到配置文件，使用内置默认参数\n");
        config_missing = true;
    }
    if (config_missing)
        config_path = nullptr;

    /* 配置校验：非法配置启动失败（退出码 1），不创建热点/TCP */
    {
        char validate_error[256];
        if (params_validate(&params, validate_error, sizeof(validate_error)) != DEMO_OK) {
            fprintf(stderr, "配置非法：%s\n", validate_error);
            return 1;
        }
    }

    /* 配置内相对路径以配置目录解析 */
    char cfg_dir[520] = {0};
    if (config_path)
        pc_path_parent_dir(config_path, cfg_dir, sizeof(cfg_dir));

    char cfg_runtime[520] = {0}, cfg_catalog[520] = {0}, cfg_log[520] = {0};
    char cfg_scenario[520] = {0};
    if (config_path) {
        if (params.runtime_dir[0])
            pc_path_resolve(cfg_dir, params.runtime_dir, cfg_runtime, sizeof(cfg_runtime));
        if (params.sim_catalog_dir[0])
            pc_path_resolve(cfg_dir, params.sim_catalog_dir, cfg_catalog, sizeof(cfg_catalog));
        if (params.log_dir[0])
            pc_path_resolve(cfg_dir, params.log_dir, cfg_log, sizeof(cfg_log));
        if (params.scenario_path[0])
            pc_path_resolve(cfg_dir, params.scenario_path, cfg_scenario, sizeof(cfg_scenario));
    }

    /* CLI 覆盖配置；未指定时用 exe 目录下的默认（不依赖 CWD，不向上搜索） */
    char exe_dir[520] = {0};
    get_exe_dir(exe_dir, sizeof(exe_dir));
    char def_runtime[520] = {0}, def_log[520] = {0};
    snprintf(def_runtime, sizeof(def_runtime), "%s/run", exe_dir);
    snprintf(def_log, sizeof(def_log), "%s/logs", exe_dir);

    char runtime_dir[520], log_dir[520], catalog_dir[520], events_path[520], scenario_path[520];
    snprintf(runtime_dir, sizeof(runtime_dir), "%s",
             cli_runtime[0] ? cli_runtime : (cfg_runtime[0] ? cfg_runtime : def_runtime));
    snprintf(log_dir, sizeof(log_dir), "%s",
             cli_log[0] ? cli_log : (cfg_log[0] ? cfg_log : def_log));
    char def_catalog[520];
    snprintf(def_catalog, sizeof(def_catalog), "%s/ap_catalog", runtime_dir);
    snprintf(catalog_dir, sizeof(catalog_dir), "%s",
             cli_catalog[0] ? cli_catalog : (cfg_catalog[0] ? cfg_catalog : def_catalog));
    snprintf(events_path, sizeof(events_path), "%s", cli_events);
    snprintf(scenario_path, sizeof(scenario_path), "%s",
             cli_scenario[0] ? cli_scenario : cfg_scenario);

    if (opts.duration > 0)
        params.duration_s = opts.duration;

    /* 事件通道：显式指定才创建；不可写则启动失败 */
    if (events_path[0]) {
        if (pc_events_open(events_path) != DEMO_OK) {
            fprintf(stderr, "无法创建事件文件：%s\n", events_path);
            return 1;
        }
    }

    /* scenario：只允许 sim；解析失败启动失败 */
    pc_scenario_t scenario;
    memset(&scenario, 0, sizeof(scenario));
    bool has_scenario = scenario_path[0] != '\0';
    if (has_scenario) {
        if (pc_scenario_validate_backend(backend_kind == 0) != DEMO_OK) {
            emit_error("scenario", "scenario 只允许 sim 后端");
            return 1;
        }
        if (pc_scenario_load(scenario_path, &scenario) != DEMO_OK) {
            emit_error("scenario", "scenario 文件非法");
            return 1;
        }
    }

    log_init(LOG_INFO);
    std::error_code fs_error;
    std::filesystem::create_directories(runtime_dir, fs_error);
    std::filesystem::create_directories(log_dir, fs_error);
    if (fs_error) {
        emit_error("runtime_dirs", "无法创建运行/日志目录");
        return 1;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif
    char log_path[520];
    std::strftime(log_path, sizeof(log_path), "/provision_pc_%Y%m%d_%H%M%S.log", &local_time);
    char full_log_path[1040];
    snprintf(full_log_path, sizeof(full_log_path), "%s%s", log_dir, log_path);
    if (log_set_file(full_log_path) == DEMO_OK)
        LOG_I("MAIN", "诊断日志文件：%s", full_log_path);
    else
        fprintf(stderr, "无法创建诊断日志文件：%s\n", full_log_path);

    if (config_path)
        LOG_I("MAIN", "配置文件：%s", config_path);
    else
        LOG_W("MAIN", "配置文件缺失，使用内置默认参数");

    /* 后端创建（无任意局域网 IP 获取，无配网路径） */
    void *backend = nullptr;
    const net_backend_t *table = nullptr;
    if (backend_kind == 0) {
        snprintf(params.runtime_dir, sizeof(params.runtime_dir), "%s", runtime_dir);
        snprintf(params.sim_catalog_dir, sizeof(params.sim_catalog_dir), "%s", catalog_dir);
        backend = sim_backend_create("host", &params);
        table = sim_backend_table();
        if (!backend) {
            emit_error("backend", "sim 后端创建失败");
            return 2;
        }
    } else {
        backend = win_backend_create(&params);
        table = win_backend_table();
        if (!backend) {
            emit_error("backend", "windows 后端创建失败");
            return 2;
        }
    }

    net_ctx_t *net = nullptr;
    int create_rc = net_ctx_create(table, backend, config_path, &net);
    if (create_rc != DEMO_OK) {
        emit_error("net_ctx", "网络抽象层初始化失败");
        if (backend_kind == 0)
            sim_backend_destroy(backend);
        else
            win_backend_destroy(backend);
        return 2;
    }

    HostApp *host = new HostApp(params, net);
    if (host->Start() != DEMO_OK) {
        LOG_E("MAIN", "上位机启动失败（热点或 TCP 端口异常）");
        emit_error("host_start", "上位机启动失败（热点或 TCP 端口异常）");
        delete host;
        net_ctx_destroy(net);
        if (backend_kind == 0) sim_backend_destroy(backend); else win_backend_destroy(backend);
        return 2;
    }
    LOG_I("MAIN", "运行模式：%s", backend_kind == 0 ? "模拟" : "Windows真实");
    LOG_I("MAIN", "热点：SSID=%s，IP=%s/%d，TCP=0.0.0.0:%d",
          params.pc_ap_ssid, params.pc_ap_ip, params.pc_ap_prefix_length,
          params.host_tcp_port);

    {
        char data[128];
        snprintf(data, sizeof(data), "{\"backend\":\"%s\",\"pid\":%ld}",
                 backend_kind == 0 ? "sim" : "windows", (long)getpid());
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "ready";
        ev.result = "ok";
        ev.code = DEMO_OK;
        ev.data_json = data;
        pc_events_emit(&ev);
    }

    std::thread host_thread([host] { host->Run(); });

    PcScenarioRunner scenario_runner(host, net, &scenario);
    std::thread scenario_thread;
    if (has_scenario)
        scenario_thread = std::thread([&scenario_runner] { scenario_runner.Run(); });

    int result = 0;
    {
        QApplication app(argc, argv);
        HostWindow window(host);
        if (params.duration_s > 0)
            QTimer::singleShot(params.duration_s * 1000, &window, &QWidget::close);
        QObject::connect(&window, &HostWindow::StopRequested, [&]() {
            host->RequestStop();
            app.quit();
        });
        window.show();
        result = app.exec();
        host->RequestStop();
    }

    scenario_runner.RequestStop();
    if (scenario_thread.joinable())
        scenario_thread.join();
    if (host_thread.joinable())
        host_thread.join();
    delete host;
    net_ctx_destroy(net);
    if (backend_kind == 0) sim_backend_destroy(backend); else win_backend_destroy(backend);

    {
        char data[64];
        snprintf(data, sizeof(data), "{\"exit_code\":%d}", result);
        pc_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event = "shutdown_complete";
        ev.result = "ok";
        ev.code = result;
        ev.data_json = data;
        pc_events_emit(&ev);
    }
    pc_events_close();
    log_close_file();
    return result;
}
