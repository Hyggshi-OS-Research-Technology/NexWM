/*
 * nex-session.cpp - Qt6 Power & Session Management Dialog for NexWM
 * Part of Nex Desktop Environment
 */

#include "nex-session.h"
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QProcess>
#include <cstdlib>

SessionWindow::SessionWindow(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground, false);
    resize(420, 240);

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect geom = screen->geometry();
        move(geom.x() + (geom.width() - width()) / 2, geom.y() + (geom.height() - height()) / 2);
    }

    setStyleSheet(R"(
        QWidget {
            background-color: #1e1e2e;
            color: #cdd6f4;
            border-radius: 8px;
            font-size: 14px;
        }
        QLabel {
            font-size: 18px;
            font-weight: bold;
            color: #5b8dd9;
        }
        QPushButton {
            background-color: #313244;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 10px 14px;
            font-size: 14px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #5b8dd9;
            color: #ffffff;
        }
        QPushButton#shutdownBtn {
            background-color: #e78284;
            color: #11111b;
        }
        QPushButton#shutdownBtn:hover {
            background-color: #ea999c;
        }
        QPushButton#cancelBtn {
            background-color: #45475a;
        }
    )");

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(16);

    auto *titleLabel = new QLabel("NexDE Power & Session Options", this);
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    auto *grid = new QHBoxLayout();
    grid->setSpacing(10);

    auto *btnLock = new QPushButton("🔒 Lock", this);
    auto *btnLogout = new QPushButton("🚪 Log Out", this);
    auto *btnReboot = new QPushButton("🔄 Reboot", this);
    auto *btnShutdown = new QPushButton("⏻ Power Off", this);
    btnShutdown->setObjectName("shutdownBtn");

    grid->addWidget(btnLock);
    grid->addWidget(btnLogout);
    grid->addWidget(btnReboot);
    grid->addWidget(btnShutdown);

    mainLayout->addLayout(grid);

    auto *btnCancel = new QPushButton("Cancel", this);
    btnCancel->setObjectName("cancelBtn");
    mainLayout->addWidget(btnCancel);

    connect(btnLock, &QPushButton::clicked, this, &SessionWindow::actionLock);
    connect(btnLogout, &QPushButton::clicked, this, &SessionWindow::actionLogout);
    connect(btnReboot, &QPushButton::clicked, this, &SessionWindow::actionReboot);
    connect(btnShutdown, &QPushButton::clicked, this, &SessionWindow::actionShutdown);
    connect(btnCancel, &QPushButton::clicked, this, &SessionWindow::close);
}

void SessionWindow::actionLock()
{
    QProcess::startDetached("xset", {"s", "activate"});
    close();
}

void SessionWindow::actionLogout()
{
    QProcess::execute("nexwmctl", {"quit"});
    close();
}

void SessionWindow::actionReboot()
{
    QProcess::startDetached("systemctl", {"reboot"});
    close();
}

void SessionWindow::actionShutdown()
{
    QProcess::startDetached("systemctl", {"poweroff"});
    close();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    SessionWindow win;
    win.show();
    return app.exec();
}
