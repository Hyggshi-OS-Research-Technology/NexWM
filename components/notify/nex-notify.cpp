/*
 * nex-notify.cpp - Qt6 Notification Toast Daemon for NexWM
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QScreen>
#include <QMouseEvent>

class NotificationWidget : public QWidget {
public:
    NotificationWidget(const QString &title, const QString &body, int timeoutMs = 3000, QWidget *parent = nullptr)
        : QWidget(parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground, false);
        setFixedSize(320, 90);

        // Position top-right
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->availableGeometry();
            move(geom.x() + geom.width() - width() - 16, geom.y() + 40);
        }

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 12);
        layout->setSpacing(4);

        auto *titleLabel = new QLabel(title, this);
        titleLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #5b8dd9;");

        auto *bodyLabel = new QLabel(body, this);
        bodyLabel->setWordWrap(true);
        bodyLabel->setStyleSheet("font-size: 12px; color: #cdd6f4;");

        layout->addWidget(titleLabel);
        layout->addWidget(bodyLabel);

        setStyleSheet(R"(
            QWidget {
                background-color: #1e1e2e;
                border: 1px solid #45475a;
                border-radius: 8px;
            }
        )");

        QTimer::singleShot(timeoutMs, this, &NotificationWidget::close);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        (void)event;
        close();
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QString title = (argc > 1) ? QString::fromUtf8(argv[1]) : "Notification";
    QString body  = (argc > 2) ? QString::fromUtf8(argv[2]) : "";
    int timeout   = (argc > 3) ? atoi(argv[3]) : 3000;

    NotificationWidget notify(title, body, timeout);
    notify.show();
    return app.exec();
}
