#pragma once
#include <QObject>
#include <QLocalSocket>
#include "protocol.h"

class PlayerClient : public QObject {
    Q_OBJECT
public:
    explicit PlayerClient(QObject *parent = nullptr);

    // Core control functions the GUI buttons will call
    void sendCommand(CommandType type, int intVal = 0, const QString& pathVal = "");
    void requestStatus();

signals:
    // Signals to pass data up to your GUI window
    void statusReceived(const PlayerStatusResponse& status);
    void connectionError(const QString& error);

private slots:
    void handleReadyRead();
    void handleSocketError(QLocalSocket::LocalSocketError socketError);

private:
    QLocalSocket* m_socket;
};
