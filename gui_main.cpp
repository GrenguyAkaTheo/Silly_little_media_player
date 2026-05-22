#include <QApplication>
#include "main_window.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    app.setStyle("Fusion");
    app.setStyleSheet(
        "QWidget { background-color: #1e1e2e; color: #cdd6f4; }"
        "QProgressBar { border: 1px solid #313244; background: #181825; }"
        "QProgressBar::chunk { background-color: #cba6f7; }"
        "QPushButton { background-color: #313244; border: none; padding: 5px; }"
        "QPushButton:hover { background-color: #45475a; }"
    );

    MainWindow window;
    window.resize(1024, 600);
    window.setWindowTitle("Geniusnt Player GUI");
    window.show();

    return app.exec();
}
