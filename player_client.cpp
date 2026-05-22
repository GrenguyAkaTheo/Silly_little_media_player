#include "player_client.h"
#include <iostream>

PlayerClient::PlayerClient(QObject *parent) : QObject(parent) {
    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::readyRead, this, &PlayerClient::handleReadyRead);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &PlayerClient::handleSocketError);
}

void PlayerClient::sendCommand(CommandType type, int intVal, const QString& pathVal) {
    // 1. Ensure we disconnect from any stale connections before a new transaction
    if (m_socket->isOpen()) {
        m_socket->close();
    }

    // 2. Connect to your daemon's socket path
    m_socket->connectToServer(SOCKET_PATH);

    if (!m_socket->waitForConnected(500)) {
        emit connectionError("Could not connect to media daemon.");
        return;
    }

    // 3. Populate your exact C-struct from protocol.h
    PlayerCommand cmd;
    cmd.type = type;
    cmd.int_value = intVal;

    // Safely copy the string path into the fixed-size char array
    memset(cmd.path_value, 0, sizeof(cmd.path_value));
    if (!pathVal.isEmpty()) {
        std::string stdStr = pathVal.toStdString();
        strncpy(cmd.path_value, stdStr.c_str(), sizeof(cmd.path_value) - 1);
    }

    // 4. Blast the raw struct bytes straight down the wire
    m_socket->write(reinterpret_cast<const char*>(&cmd), sizeof(PlayerCommand));
    m_socket->flush();

    // If it's a fire-and-forget command (like SKIP or PLAY), close it if we don't expect a response
    if (type != CommandType::GET_STATUS) {
        m_socket->disconnectFromServer();
    }
}

void PlayerClient::requestStatus() {
    sendCommand(CommandType::GET_STATUS);
}

void PlayerClient::handleReadyRead() {
    // This triggers when the daemon responds to GET_STATUS
    if (m_socket->bytesAvailable() >= sizeof(PlayerStatusResponse)) {
        PlayerStatusResponse response;
        m_socket->read(reinterpret_cast<char*>(&response), sizeof(PlayerStatusResponse));

        emit statusReceived(response);
        m_socket->disconnectFromServer();
    }
}

void PlayerClient::handleSocketError(QLocalSocket::LocalSocketError error) {
    emit connectionError(m_socket->errorString());
}
