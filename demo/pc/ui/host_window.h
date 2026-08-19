#ifndef DEMO_HOST_WINDOW_H
#define DEMO_HOST_WINDOW_H

#include <QMainWindow>

class HostApp;
class HandshakeFlowWidget;
class LogModel;
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

signals:
    void StopRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void Refresh();
    void SendAppData();

private:
    HostApp *host_;
    LogModel *log_model_;
    HandshakeFlowWidget *flow_widget_;
    QListView *log_view_;
    QLabel *summary_;
    QLabel *hotspot_status_;
    QTableWidget *device_table_;
    QLineEdit *msg_edit_;
    QPushButton *msg_send_button_;
    bool stopping_ = false;
};

#endif
