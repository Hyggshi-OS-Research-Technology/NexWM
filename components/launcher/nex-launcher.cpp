/*
 * nex-launcher.cpp - Qt6 Application Launcher for NexWM
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QKeyEvent>
#include <QScreen>
#include <QRegularExpression>
#include <cstdlib>

struct AppItem {
    QString name;
    QString exec;
    QString icon;
    QString comment;
};

static void parseDesktopFile(const QString &path, QList<AppItem> &apps) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    AppItem app;
    bool inDesktopEntry = false;
    bool noDisplay = false;
    bool isApp = true;

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        if (line.startsWith('[')) {
            inDesktopEntry = (line == "[Desktop Entry]");
            continue;
        }
        if (!inDesktopEntry) continue;

        int eqPos = line.indexOf('=');
        if (eqPos == -1) continue;

        QString key = line.left(eqPos).trimmed();
        QString val = line.mid(eqPos + 1).trimmed();

        if (key.contains('[')) continue; // skip localized keys

        if (key == "Name") app.name = val;
        else if (key == "Exec") {
            // Strip placeholders %u %f %U %F
            val.remove(QRegularExpression("%[fFuUiDcK]"));
            app.exec = val.trimmed();
        }
        else if (key == "Icon") app.icon = val;
        else if (key == "Comment") app.comment = val;
        else if (key == "NoDisplay" && val.toLower() == "true") noDisplay = true;
        else if (key == "Type" && val != "Application") isApp = false;
    }
    file.close();

    if (isApp && !noDisplay && !app.name.isEmpty() && !app.exec.isEmpty()) {
        apps.append(app);
    }
}

static QList<AppItem> scanApps() {
    QList<AppItem> apps;
    QStringList dirs = {
        "/usr/share/applications",
        "/usr/local/share/applications"
    };
    const char *home = getenv("HOME");
    if (home) dirs.append(QString("%1/.local/share/applications").arg(home));

    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;

        for (const QString &entry : dir.entryList({"*.desktop"}, QDir::Files)) {
            parseDesktopFile(dir.filePath(entry), apps);
        }
    }
    return apps;
}

class LauncherWindow : public QWidget {
public:
    LauncherWindow(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground, false);
        resize(480, 360);

        // Center on current screen
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->geometry();
            move(geom.x() + (geom.width() - width()) / 2, geom.y() + (geom.height() - height()) / 3);
        }

        m_allApps = scanApps();

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        m_searchEdit = new QLineEdit(this);
        m_searchEdit->setPlaceholderText("Search applications...");
        layout->addWidget(m_searchEdit);

        m_listWidget = new QListWidget(this);
        layout->addWidget(m_listWidget);

        setStyleSheet(R"(
            QWidget {
                background-color: #1e1e2e;
                color: #cdd6f4;
                border-radius: 8px;
                font-size: 14px;
            }
            QLineEdit {
                background-color: #313244;
                color: #cdd6f4;
                border: 1px solid #45475a;
                border-radius: 6px;
                padding: 8px 12px;
                font-size: 15px;
            }
            QListWidget {
                background-color: #181825;
                border: 1px solid #313244;
                border-radius: 6px;
                padding: 4px;
            }
            QListWidget::item {
                padding: 8px 12px;
                border-radius: 4px;
            }
            QListWidget::item:selected {
                background-color: #5b8dd9;
                color: #ffffff;
            }
        )");

        connect(m_searchEdit, &QLineEdit::textChanged, this, &LauncherWindow::filterList);
        connect(m_listWidget, &QListWidget::itemActivated, this, &LauncherWindow::launchCurrent);

        filterList("");
        m_searchEdit->setFocus();
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            close();
        } else if (event->key() == Qt::Key_Down) {
            int idx = m_listWidget->currentRow() + 1;
            if (idx < m_listWidget->count()) m_listWidget->setCurrentRow(idx);
        } else if (event->key() == Qt::Key_Up) {
            int idx = m_listWidget->currentRow() - 1;
            if (idx >= 0) m_listWidget->setCurrentRow(idx);
        } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            launchCurrent();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    void filterList(const QString &query) {
        m_listWidget->clear();
        for (const AppItem &app : m_allApps) {
            if (query.isEmpty() || app.name.contains(query, Qt::CaseInsensitive) ||
                app.comment.contains(query, Qt::CaseInsensitive)) {
                auto *item = new QListWidgetItem(app.name, m_listWidget);
                item->setData(Qt::UserRole, app.exec);
                if (!app.comment.isEmpty()) item->setToolTip(app.comment);
            }
        }
        if (m_listWidget->count() > 0) m_listWidget->setCurrentRow(0);
    }

    void launchCurrent() {
        QListWidgetItem *item = m_listWidget->currentItem();
        if (!item) return;

        QString exec = item->data(Qt::UserRole).toString();
        if (!exec.isEmpty()) {
            QProcess::startDetached("/bin/sh", {"-c", exec});
        }
        close();
    }

    QList<AppItem> m_allApps;
    QLineEdit *m_searchEdit;
    QListWidget *m_listWidget;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    LauncherWindow win;
    win.show();
    return app.exec();
}
