/* 设备 Qt 最薄启动入口：只做 QApplication、解析 CLI、create/start host、
 * 构造窗口、exec、request_stop/join/destroy 与退出码映射。
 * 不得包含配置解析、filesystem、thread、backend 或 core 私有头。 */
#include "device_host.h"
#include "device_window.h"

#include <QApplication>
#include <QTimer>
#include <QWidget>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

static int print_error(const char *stage, device_result_t rc, const device_error_t *error)
{
    fprintf(stderr, "%s失败（code=%d）：%s\n", stage, (int)rc,
            error != NULL && error->message[0] != '\0' ? error->message : "未知错误");
    return 1;
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    QApplication app(argc, argv);

    device_host_options_t options;
    device_error_t error;
    device_host_options_init(&options);
    device_result_t rc = device_host_options_parse_argv(&options, argc, argv, &error);
    if (rc != DEVICE_OK)
        return print_error("参数解析", rc, &error);

    device_host_t *host = NULL;
    rc = device_host_create(&options, &host, &error);
    if (rc != DEVICE_OK)
        return print_error("设备创建", rc, &error);

    rc = device_host_start(host, &error);
    if (rc != DEVICE_OK) {
        device_host_destroy(&host, NULL);
        return print_error("设备启动", rc, &error);
    }

    DeviceWindow window(host);
    if (options.duration_seconds > 0)
        QTimer::singleShot(static_cast<int>(options.duration_seconds) * 1000,
                           &window, &QWidget::close);
    QObject::connect(&window, &DeviceWindow::StopRequested, &app, &QApplication::quit);

    window.show();
    int code = app.exec();

    device_host_request_stop(host);
    rc = device_host_join(host, DEVICE_JOIN_WAIT_FOREVER, &error);
    if (rc != DEVICE_OK)
        fprintf(stderr, "设备停止等待失败（code=%d）：%s\n", (int)rc, error.message);
    device_host_destroy(&host, NULL);
    return code;
}
