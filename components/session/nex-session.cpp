/*
 * nex-session.cpp - Qt6 Power & Session Management Dialog for NexWM / NexDE
 */

#include "nex-session.h"
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QProcess>
#include <QIcon>
#include <QToolButton>
#include <cstdlib>

SessionWindow::SessionWindow(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground, false);
    resize(460, 220);

    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect geom = screen->geometry();
        move(geom.x() + (geom.width() - width()) / 2, geom.y() + (geom.height() - height()) / 2);
    }

    setStyleSheet(R"(
        QWidget {
            background-color: #1e1e2e;
            color: #cdd6f4;
            border-radius: 12px;
            font-family: 'Segoe UI', 'Ubuntu', sans-serif;
        }
        QLabel#titleLabel {
            font-size: 18px;
            font-weight: bold;
            color: #5b8dd9;
        }
        QToolButton {
            background-color: #181825;
            color: #cdd6f4;
            border: 1px solid #313244;
            border-radius: 8px;
            padding: 12px 6px;
            font-size: 13px;
            font-weight: bold;
        }
        QToolButton:hover {
            background-color: #5b8dd9;
            color: #ffffff;
            border: 1px solid #5b8dd9;
        }
        QToolButton#shutdownBtn:hover {
            background-color: #e78284;
            color: #11111b;
            border: 1px solid #e78284;
        }
        QPushButton#cancelBtn {
            background-color: #313244;
            color: #a6adc8;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 6px 16px;
            font-size: 13px;
        }
        QPushButton#cancelBtn:hover {
            background-color: #45475a;
            color: #ffffff;
        }
    )");

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 16);
    mainLayout->setSpacing(16);

    auto *titleLabel = new QLabel("NexDE Power & Session Options", this);
    titleLabel->setObjectName("titleLabel");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    auto *grid = new QHBoxLayout();
    grid->setSpacing(12);

    struct SessionOption {
        const char *label;
        const char *icon;
        void (SessionWindow::*slot)();
        bool isShutdown;
    };

    SessionOption options[] = {
        {"Lock", "system-lock-screen", &SessionWindow::actionLock, false},
        {"Log Out", "system-log-out", &SessionWindow::actionLogout, false},
        {"Reboot", "system-reboot", &SessionWindow::actionReboot, false},
        {"Power Off", "system-shutdown", &SessionWindow::actionShutdown, true}
    };

    for (const auto &opt : options) {
        auto *btn = new QToolButton(this);
        btn->setText(opt.label);
        btn->setIcon(QIcon::fromTheme(opt.icon));
        btn->setIconSize(QSize(32, 32));
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        if (opt.isShutdown) {
            btn->setObjectName("shutdownBtn");
        }

        connect(btn, &QToolButton::clicked, this, opt.slot);
        grid->addWidget(btn);
    }

    mainLayout->addLayout(grid);

    auto *footer = new QHBoxLayout();
    auto *btnCancel = new QPushButton("Cancel (Esc)", this);
    btnCancel->setObjectName("cancelBtn");
    footer->addStretch();
    footer->addWidget(btnCancel);
    footer->addStretch();
    mainLayout->addLayout(footer);

    connect(btnCancel, &QPushButton::clicked, this, &SessionWindow::close);
}

void SessionWindow::actionLock()
{
    QProcess::startDetached("xset", {"s", "activate"});
    close();
}

void SessionWindow::actionLogout()
{
    QProcess::startDetached("nexwmctl", {"quit"});
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

void SessionWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
    } else {
        QWidget::keyPressEvent(event);
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    SessionWindow win;
    win.show();
    return app.exec();
}
