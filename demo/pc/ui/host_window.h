#ifndef DEMO_HOST_WINDOW_H
#define DEMO_HOST_WINDOW_H

#include <QMainWindow>

class HostApp;
class IHostDataSink;
class HandshakeFlowWidget;
class LogModel;
class SerialDataModel;
class QListView;
class QLabel;
class QPushButton;
class QTableWidget;
class QLineEdit;

class HostWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit HostWindow(HostApp *host, QWidget *parent = nullptr);
    ~HostWindow() override;

    /* main 在窗口创建后经此访问器把 SerialDataModel 接到 HostApp */
    IHostDataSink *serial_model() const;

signals:
    void StopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void Refresh();
    void SendAppData();
    void SendSerialFrame();

private:
    HostApp *host_;
    LogModel *log_model_;
    HandshakeFlowWidget *flow_widget_;
    QListView *log_view_;
    QLabel *summary_;
    QLabel *hotspot_status_;
    SerialDataModel *serial_model_;
    QLabel *serial_stats_;
    QTableWidget *device_table_;
    QLineEdit *msg_edit_;
    QLineEdit *serial_edit_;
    QPushButton *serial_send_button_;
    QPushButton *msg_send_button_;
    bool stopping_ = false;
};

#endif
