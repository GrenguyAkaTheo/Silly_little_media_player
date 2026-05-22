#include "main_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QSplitter>
#include <QTableView>
#include <QTabWidget>
#include <QListView>
#include <QLabel>
#include <QPushButton>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // 1. Initialize the socket client
    m_client = new PlayerClient(this);

    // 2. Create the Main Horizontal Splitter as the base
    QSplitter* mainSplitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(mainSplitter);

    // 3. Left Side: Tracklist
    QTableView* trackTable = new QTableView(mainSplitter);
    trackTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    trackTable->setAlternatingRowColors(true);
    mainSplitter->addWidget(trackTable);

    // 4. Right Side: The Sidebar (Vertical Splitter)
    QSplitter* sidebarSplitter = new QSplitter(Qt::Vertical, mainSplitter);
    mainSplitter->addWidget(sidebarSplitter);

    // -- Sidebar Top: Unified Now Playing View
    QWidget* nowPlayingWidget = new QWidget(sidebarSplitter);
    QVBoxLayout* nowPlayingLayout = new QVBoxLayout(nowPlayingWidget);

    // 1. Track Title
    m_trackTitleLabel = new QLabel("Waiting for daemon...", nowPlayingWidget);
    m_trackTitleLabel->setAlignment(Qt::AlignCenter);

    // 2. Album Art
    QLabel* albumArtLabel = new QLabel("Album Art Here", nowPlayingWidget);
    albumArtLabel->setAlignment(Qt::AlignCenter);
    albumArtLabel->setMinimumSize(200, 200); // Prevents the image area from collapsing
    // albumArtLabel->setScaledContents(true); // Uncomment this later when loading actual images

    // 3. Progress Bar & Time Labels (Horizontal Row)
    QHBoxLayout* progressLayout = new QHBoxLayout();

    // Create the time labels (You will need to add these to main_window.h as member variables)
    m_currentTimeLabel = new QLabel("0:00", nowPlayingWidget);
    m_totalTimeLabel = new QLabel("0:00", nowPlayingWidget);

    m_timeProgressBar = new QProgressBar(nowPlayingWidget);
    m_timeProgressBar->setTextVisible(false); // Removes the percentage text
    m_timeProgressBar->setFixedHeight(8);     // Makes the progress bar thinner

    // Add them to the horizontal layout
    progressLayout->addWidget(m_currentTimeLabel);
    progressLayout->addWidget(m_timeProgressBar, 1); // '1' stretch factor keeps the bar expanding
    progressLayout->addWidget(m_totalTimeLabel);

    // 4. Playback Controls (Vertical Stack)
    // Changed from QHBoxLayout to QVBoxLayout to stack them top-to-bottom
    QVBoxLayout* controlsLayout = new QVBoxLayout();

    m_playPauseBtn = new QPushButton("Play/Pause", nowPlayingWidget);
    m_skipBtn = new QPushButton("Skip", nowPlayingWidget);

    controlsLayout->addWidget(m_playPauseBtn);
    controlsLayout->addWidget(m_skipBtn);

    // 5. Stack everything in the main vertical layout
    nowPlayingLayout->addWidget(m_trackTitleLabel);
    nowPlayingLayout->addWidget(albumArtLabel, 1); // Pushes the lower controls to the bottom
    nowPlayingLayout->addLayout(progressLayout);   // Nests the horizontal time/progress row
    nowPlayingLayout->addLayout(controlsLayout);   // Nests the vertical buttons

    sidebarSplitter->addWidget(nowPlayingWidget);

    sidebarSplitter->addWidget(nowPlayingWidget);

    // -- Sidebar Bottom: Playlist Manager Container
    QWidget* managerContainer = new QWidget(sidebarSplitter);
    QVBoxLayout* managerLayout = new QVBoxLayout(managerContainer);
    managerLayout->setContentsMargins(0, 0, 0, 0);

    QListView* playlistList = new QListView(managerContainer);
    managerLayout->addWidget(playlistList);

    QHBoxLayout* listButtonLayout = new QHBoxLayout();
    listButtonLayout->addWidget(new QPushButton("New"));
    listButtonLayout->addWidget(new QPushButton("Rename"));
    listButtonLayout->addWidget(new QPushButton("Remove"));
    managerLayout->addLayout(listButtonLayout);

    sidebarSplitter->addWidget(managerContainer);

    // 5. Set relative widths (Left 70%, Right 30%)
    mainSplitter->setStretchFactor(0, 7);
    mainSplitter->setStretchFactor(1, 3);

    // 6. Wire the buttons to the socket commands
    connect(m_skipBtn, &QPushButton::clicked, this, &MainWindow::onSkipClicked);
    connect(m_playPauseBtn, &QPushButton::clicked, this, &MainWindow::onPlayPauseClicked);

    // 7. Wire the client's status response to our UI updater
    connect(m_client, &PlayerClient::statusReceived, this, &MainWindow::updateUI);

    // 8. Start the heartbeat timer (polls every 500ms)
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, m_client, &PlayerClient::requestStatus);
    m_statusTimer->start(500);
}

void MainWindow::onSkipClicked() {
    m_client->sendCommand(CommandType::SKIP);
}

void MainWindow::onPlayPauseClicked() {
    m_client->sendCommand(CommandType::PLAY);
}

void MainWindow::updateUI(const PlayerStatusResponse& status) {
    m_trackTitleLabel->setText(QString::fromUtf8(status.track_name));
    m_timeProgressBar->setMaximum(status.duration_seconds);
    m_timeProgressBar->setValue(status.elapsed_seconds);

    // Calculate current time
    int currentMins = status.elapsed_seconds / 60;
    int currentSecs = status.elapsed_seconds % 60;
    m_currentTimeLabel->setText(QString::asprintf("%d:%02d", currentMins, currentSecs));

    // Calculate total time
    int totalMins = status.duration_seconds / 60;
    int totalSecs = status.duration_seconds % 60;
    m_totalTimeLabel->setText(QString::asprintf("%d:%02d", totalMins, totalSecs));
}
