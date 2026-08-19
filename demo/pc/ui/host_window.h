#ifndef DEMO_HOST_WINDOW_H
#define DEMO_HOST_WINDOW_H

#include <QMainWindow>
#include <functional>

class HostApp;
class HandshakeFlowWidget;
class LogModel;
class QListView;
class QLabel;
class QPushButton;
class QTableWidget;
class QComboBox;
class QLineEdit;

class HostWindow : public QMainWindow {
    Q_OBJECT
public:
    HostWindow(HostApp *host, const char *target_ssid, const char *target_password,
                std::function<void()> provision, QWidget *parent = nullptr);
    ~HostWindow() override;

signals:
    void StopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void Refresh();
    void StartProvision();
    void ScanTargetWifi();
    void SendAppData();

private:
    HostApp *host_;
    std::function<void()> provision_;
    LogModel *log_model_;
    HandshakeFlowWidget *flow_widget_;
    QListView *log_view_;
    QLabel *summary_;
    QTableWidget *device_table_;
    QPushButton *provision_button_;
    QPushButton *wifi_scan_button_;
    QComboBox *target_ssid_;
    QLineEdit *target_password_;
    QLineEdit *msg_edit_;
    QPushButton *msg_send_button_;
    bool stopping_ = false;
};

#endif
