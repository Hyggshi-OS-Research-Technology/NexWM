/*
 * nex-notify.cpp - Qt6 Toast Notification Daemon for NexWM / NexDE
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QScreen>
#include <QMouseEvent>
#include <QProgressBar>
#include <QIcon>
#include <cstdlib>

class NotificationWidget : public QWidget {
public:
    NotificationWidget(const QString &title, const QString &body, int timeoutMs = 3500, const QString &iconName = "dialog-information", QWidget *parent = nullptr)
        : QWidget(parent), m_remainingMs(timeoutMs), m_totalMs(timeoutMs) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setFixedSize(360, 96);

        // Position top-right
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->availableGeometry();
            move(geom.x() + geom.width() - width() - 16, geom.y() + 48);
        }

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        auto *contentContainer = new QWidget(this);
        auto *contentLayout = new QHBoxLayout(contentContainer);
        contentLayout->setContentsMargins(14, 12, 14, 12);
        contentLayout->setSpacing(12);

        // Icon
        auto *iconLabel = new QLabel(contentContainer);
        QIcon icon = QIcon::fromTheme(iconName, QIcon::fromTheme("dialog-information"));
        if (!icon.isNull()) {
            iconLabel->setPixmap(icon.pixmap(32, 32));
        } else {
            iconLabel->setFixedSize(32, 32);
            iconLabel->setStyleSheet("background-color: #5b8dd9; border-radius: 16px;");
        }
        contentLayout->addWidget(iconLabel);

        // Text Layout
        auto *textLayout = new QVBoxLayout();
        textLayout->setSpacing(2);

        auto *titleLabel = new QLabel(title, contentContainer);
        titleLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #5b8dd9;");

        auto *bodyLabel = new QLabel(body, contentContainer);
        bodyLabel->setWordWrap(true);
        bodyLabel->setStyleSheet("font-size: 12px; color: #cdd6f4;");

        textLayout->addWidget(titleLabel);
        textLayout->addWidget(bodyLabel);
        contentLayout->addLayout(textLayout, 1);

        mainLayout->addWidget(contentContainer, 1);

        // Timer Progress Bar
        m_progressBar = new QProgressBar(this);
        m_progressBar->setFixedHeight(3);
        m_progressBar->setTextVisible(false);
        m_progressBar->setRange(0, m_totalMs);
        m_progressBar->setValue(m_totalMs);
        m_progressBar->setStyleSheet(R"(
            QProgressBar {
                background-color: #313244;
                border: none;
                border-bottom-left-radius: 8px;
                border-bottom-right-radius: 8px;
            }
            QProgressBar::chunk {
                background-color: #5b8dd9;
            }
        )");
        mainLayout->addWidget(m_progressBar);

        setStyleSheet(R"(
            QWidget {
                background-color: #1e1e2e;
                border: 1px solid #45475a;
                border-radius: 8px;
                font-family: 'Segoe UI', 'Ubuntu', sans-serif;
            }
        )");

        // Countdown timer
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this]() {
            m_remainingMs -= 50;
            if (m_remainingMs <= 0) {
                close();
            } else {
                m_progressBar->setValue(m_remainingMs);
            }
        });
        m_timer->start(50);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        Q_UNUSED(event);
        close();
    }

private:
    int m_remainingMs;
    int m_totalMs;
    QTimer *m_timer;
    QProgressBar *m_progressBar;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QString title    = (argc > 1) ? QString::fromUtf8(argv[1]) : "Notification";
    QString body     = (argc > 2) ? QString::fromUtf8(argv[2]) : "";
    int timeout      = (argc > 3) ? atoi(argv[3]) : 3500;
    QString iconName = (argc > 4) ? QString::fromUtf8(argv[4]) : "dialog-information";

    NotificationWidget notify(title, body, timeout, iconName);
    notify.show();
    return app.exec();
}
