/* 设备 Qt 外壳测试（facade fake）：窗口构造/关闭、快照刷新、日志 drain、
 * 命令错误提示。不要求真实网络。offscreen 平台运行。 */
#include "device_window.h"
#include "fake_host.h"
#include "common.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

class DeviceWindowTest : public QObject {
    Q_OBJECT

private slots:
    void init() { fake_host_init(); }

    void constructsAndShows()
    {
        DeviceWindow window(nullptr);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window) || window.isVisible());
        QVERIFY(window.windowTitle().contains("TactileSense"));
    }

    void snapshotRefreshUpdatesLabels()
    {
        device_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        snap.host_state = DEVICE_HOST_RUNNING;
        snap.device_state = 4; /* SESSION */
        snap.rssi_dbm = -63;
        snap.uptime_seconds = 120;
        snap.session_online = 1;
        snprintf(snap.device_id, sizeof(snap.device_id), "02:00:00:00:00:08");
        snprintf(snap.target_ssid, sizeof(snap.target_ssid), "Modu_PC");
        snprintf(snap.pc_host_ip, sizeof(snap.pc_host_ip), "192.168.137.1");
        snap.pc_host_port = 5935;
        snprintf(snap.backend_name, sizeof(snap.backend_name), "sim");
        fake_host_set_snapshot(&snap);

        DeviceWindow window(fake_host_instance());
        window.show();
        QMetaObject::invokeMethod(&window, "Refresh", Qt::DirectConnection);

        bool foundState = false, foundTarget = false, foundSession = false;
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("会话在线")))
                foundState = true;
            if (label->text().contains(QStringLiteral("Modu_PC")) &&
                label->text().contains(QStringLiteral("192.168.137.1")))
                foundTarget = true;
            if (label->text().contains(QStringLiteral("-63")) &&
                label->text().contains(QStringLiteral("120")))
                foundSession = true;
        }
        QVERIFY(foundState);
        QVERIFY(foundTarget);
        QVERIFY(foundSession);
    }

    /* 六节点 + 无旧配网/发现/AP 文案（任务书第 9 节用例 12） */
    void sixNodesNoLegacyText()
    {
        device_snapshot_t snap;
        memset(&snap, 0, sizeof(snap));
        snap.host_state = DEVICE_HOST_RUNNING;
        snap.device_state = 1; /* WIFI_SCAN */
        snprintf(snap.device_id, sizeof(snap.device_id), "02:00:00:00:00:01");
        snprintf(snap.target_ssid, sizeof(snap.target_ssid), "Modu_PC");
        snprintf(snap.pc_host_ip, sizeof(snap.pc_host_ip), "192.168.137.1");
        snap.pc_host_port = 5935;
        fake_host_set_snapshot(&snap);

        DeviceWindow window(fake_host_instance());
        window.show();
        QMetaObject::invokeMethod(&window, "Refresh", Qt::DirectConnection);

        bool foundOldText = false;
        const QString kNodes[6] = {
            QStringLiteral("启动"), QStringLiteral("扫描PC热点"),
            QStringLiteral("连接热点"), QStringLiteral("连接PC"),
            QStringLiteral("会话在线"), QStringLiteral("异常重连")
        };
        bool nodeFound[6] = {false, false, false, false, false, false};
        for (auto *label : window.findChildren<QLabel *>()) {
            const QString t = label->text();
            if (t.contains(QStringLiteral("配网")) ||
                t.contains(QStringLiteral("发现")) ||
                t.contains(QStringLiteral("密码")) ||
                t.contains(QStringLiteral("PIN")))
                foundOldText = true;
            for (int i = 0; i < 6; i++) {
                if (t == kNodes[i])
                    nodeFound[i] = true;
            }
        }
        for (int i = 0; i < 6; i++)
            QVERIFY(nodeFound[i]); /* 六节点逐一出现（FlowWidget） */
        QVERIFY(!foundOldText);
    }

    void logDrainPushesToModel()
    {
        DeviceWindow window(fake_host_instance());
        window.show();
        device_log_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.sequence = 1;
        rec.level = LOG_INFO;
        snprintf(rec.module, sizeof(rec.module), "HOST");
        snprintf(rec.message, sizeof(rec.message), "hello from fake host");
        fake_host_push_log(&rec);

        QMetaObject::invokeMethod(&window, "Refresh", Qt::DirectConnection);
        auto *view = window.findChild<QListView *>();
        QVERIFY(view != nullptr);
        QAbstractItemModel *model = view->model();
        QVERIFY(model != nullptr);
        QCOMPARE(model->rowCount(), 1);
        QVERIFY(model->data(model->index(0, 0)).toString().contains("hello from fake host"));
    }

    void sendAppDataOk()
    {
        DeviceWindow window(fake_host_instance());
        window.show();
        auto *edit = window.findChild<QLineEdit *>(QStringLiteral("msg_edit"));
        QVERIFY(edit != nullptr);
        edit->setText(QStringLiteral("hello"));

        QList<QPushButton *> buttons = window.findChildren<QPushButton *>();
        QPushButton *send = nullptr;
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("发送消息")))
                send = b;
        }
        QVERIFY(send != nullptr);
        QTest::mouseClick(send, Qt::LeftButton);

        QCOMPARE(fake_host_send_calls(), 1);
        QCOMPARE(fake_host_last_app_data_len(), strlen("hello"));
        QCOMPARE(QString::fromUtf8(fake_host_last_app_data()), QStringLiteral("hello"));
    }

    void sendAppDataErrorShowsHint()
    {
        fake_host_set_send_app_data_result(DEVICE_ERR_INVALID_STATE);
        DeviceWindow window(fake_host_instance());
        window.show();
        auto *edit = window.findChild<QLineEdit *>(QStringLiteral("msg_edit"));
        edit->setText(QStringLiteral("nope"));
        QList<QPushButton *> buttons = window.findChildren<QPushButton *>();
        QPushButton *send = nullptr;
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("发送消息")))
                send = b;
        }
        QTest::mouseClick(send, Qt::LeftButton);
        bool found = false;
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->isVisible() && label->text().contains(QStringLiteral("发送失败")))
                found = true;
        }
        QVERIFY(found);
    }

    void injectFaultOk()
    {
        DeviceWindow window(fake_host_instance());
        window.show();
        auto *combo = window.findChild<QComboBox *>();
        auto *arg = window.findChild<QLineEdit *>(QStringLiteral("argument_edit"));
        QVERIFY(combo != nullptr && arg != nullptr);
        combo->setCurrentIndex(3); /* rssi_set */
        arg->setText(QStringLiteral("-80"));
        QList<QPushButton *> buttons = window.findChildren<QPushButton *>();
        QPushButton *inject = nullptr;
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("注入故障")))
                inject = b;
        }
        QTest::mouseClick(inject, Qt::LeftButton);
        QCOMPARE(fake_host_inject_calls(), 1);
        QCOMPARE(QString::fromUtf8(fake_host_last_inject_action()),
                 QStringLiteral("rssi_set"));
        QCOMPARE(QString::fromUtf8(fake_host_last_inject_argument()),
                 QStringLiteral("-80"));
    }

    void injectFaultErrorShowsHint()
    {
        fake_host_set_inject_result(DEVICE_ERR_INVALID_STATE);
        DeviceWindow window(fake_host_instance());
        window.show();
        QList<QPushButton *> buttons = window.findChildren<QPushButton *>();
        QPushButton *inject = nullptr;
        for (auto *b : buttons) {
            if (b->text().contains(QStringLiteral("注入故障")))
                inject = b;
        }
        QTest::mouseClick(inject, Qt::LeftButton);
        bool found = false;
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->isVisible() && label->text().contains(QStringLiteral("故障注入失败")))
                found = true;
        }
        QVERIFY(found);
    }

    void closeEmitsStopRequested()
    {
        DeviceWindow window(nullptr);
        window.show();
        QSignalSpy spy(&window, &DeviceWindow::StopRequested);
        window.close();
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(DeviceWindowTest)
#include "test_device_window.moc"
