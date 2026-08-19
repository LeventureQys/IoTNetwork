#ifndef DEMO_DEVICE_WINDOW_H
#define DEMO_DEVICE_WINDOW_H

#include <QMainWindow>

/* 设备顶层 UI 外壳：只持有不透明 device_host_t*，通过 facade 公开接口
 * （get_snapshot / drain_logs / send_app_data / inject_fault / request_stop）工作。
 * 不包含任何 core / backend / runtime 私有头。 */

typedef struct device_host device_host_t;

class LogModel;
class FlowWidget;
class QListView;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;

class DeviceWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit DeviceWindow(device_host_t *host, QWidget *parent = nullptr);
    ~DeviceWindow() override;

signals:
    /* 关闭窗口时发出：请求停止，不自行销毁核心（由入口负责 join/destroy）。 */
    void StopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void Refresh();
    void Inject();
    void SendAppData();

private:
    void ShowError(const QString &message);

    device_host_t *host_;
    LogModel *log_model_;
    FlowWidget *flow_widget_;
    QListView *log_view_;
    QLabel *state_label_;
    QLabel *ap_label_;
    QLabel *session_label_;
    QLabel *error_label_;
    QComboBox *action_box_;
    QLineEdit *argument_edit_;
    QLineEdit *msg_edit_;
    QPushButton *msg_send_button_;
    bool stopping_ = false;
};

#endif
