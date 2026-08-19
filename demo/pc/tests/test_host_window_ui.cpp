#include <gtest/gtest.h>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include "host_window.h"
#include "handshake_flow_widget.h"
#include "host_app.h"
#include "params.h"

namespace {

/* 单一进程仅允许一个 QApplication：按需创建一次并复用。 */
QApplication &ui_app()
{
    static int argc = 1;
    static char argv0[] = "pc_ui_tests";
    static char *argv[] = {argv0, nullptr};
    static QApplication app(argc, argv);
    return app;
}

} // namespace

TEST(UiLegacyText, NoOldProvisionText)
{
    ui_app();
    demo_params_t params;
    params_defaults(&params);
    net_backend_t be{};
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(&be, nullptr, nullptr, &ctx), DEMO_OK);
    {
        HostApp host(params, ctx);
        HostWindow window(&host);

        const QStringList forbidden = {
            QStringLiteral("下发 WiFi"), QStringLiteral("设备 AP"),
            QStringLiteral("mDNS"), QStringLiteral("host_announce"),
            QStringLiteral("配网"), QStringLiteral("SoftAP")
        };
        for (QLabel *label : window.findChildren<QLabel *>()) {
            for (const QString &word : forbidden)
                EXPECT_FALSE(label->text().contains(word)) << label->text().toStdString();
        }
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            for (const QString &word : forbidden)
                EXPECT_FALSE(button->text().contains(word)) << button->text().toStdString();
        }
    }
    net_ctx_destroy(ctx);
}

TEST(UiFlowWidget, SixNodesOnly)
{
    ui_app();
    HandshakeFlowWidget widget;
    ASSERT_EQ(widget.nodeCount(), 6);
    const QStringList titles = widget.nodeTitles();
    EXPECT_EQ(titles[0], QStringLiteral("启动"));
    EXPECT_EQ(titles[1], QStringLiteral("热点启动"));
    EXPECT_EQ(titles[2], QStringLiteral("固定 IP"));
    EXPECT_EQ(titles[3], QStringLiteral("TCP 监听"));
    EXPECT_EQ(titles[4], QStringLiteral("设备握手"));
    EXPECT_EQ(titles[5], QStringLiteral("会话在线"));
}

TEST(UiStopButton, TextIsStopServiceAndHotspot)
{
    ui_app();
    demo_params_t params;
    params_defaults(&params);
    net_backend_t be{};
    net_ctx_t *ctx = nullptr;
    ASSERT_EQ(net_ctx_create(&be, nullptr, nullptr, &ctx), DEMO_OK);
    {
        HostApp host(params, ctx);
        HostWindow window(&host);
        bool found = false;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("停止服务并关闭热点"))
                found = true;
        }
        EXPECT_TRUE(found);
    }
    net_ctx_destroy(ctx);
}
