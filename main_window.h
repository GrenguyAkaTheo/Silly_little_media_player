#pragma once
#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include "player_client.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);

private slots:
    // Slot to handle the data coming back from the socket
    void updateUI(const PlayerStatusResponse& status);

    // Slots for button clicks
    void onSkipClicked();
    void onPlayPauseClicked();

private:
    PlayerClient* m_client;
    QTimer* m_statusTimer;

    // UI Elements
    QLabel* m_trackTitleLabel;
    QProgressBar* m_timeProgressBar;
    QPushButton* m_playPauseBtn;
    QPushButton* m_skipBtn;
    QLabel* m_currentTimeLabel;
    QLabel* m_totalTimeLabel;
};
