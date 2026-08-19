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
        snap.device_state = 5; /* SESSION */
        snap.rssi_dbm = -63;
        snap.uptime_seconds = 120;
        snap.provision_port = 20007;
        snap.session_online = 1;
        snprintf(snap.device_id, sizeof(snap.device_id), "02:00:00:00:00:08");
        snprintf(snap.ap_ssid, sizeof(snap.ap_ssid), "Modu_0008");
        snprintf(snap.ap_password, sizeof(snap.ap_password), "modutech_leventure");
        snprintf(snap.provision_pin, sizeof(snap.provision_pin), "5935");
        snprintf(snap.backend_name, sizeof(snap.backend_name), "sim");
        fake_host_set_snapshot(&snap);

        DeviceWindow window(fake_host_instance());
        window.show();
        QMetaObject::invokeMethod(&window, "Refresh", Qt::DirectConnection);

        bool foundState = false, foundAp = false, foundSession = false;
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("会话在线")))
                foundState = true;
            if (label->text().contains(QStringLiteral("Modu_0008")))
                foundAp = true;
            if (label->text().contains(QStringLiteral("-63")) &&
                label->text().contains(QStringLiteral("120")))
                foundSession = true;
        }
        QVERIFY(foundState);
        QVERIFY(foundAp);
        QVERIFY(foundSession);
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
