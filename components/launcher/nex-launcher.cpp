/*
 * nex-launcher.cpp - Spotlight-style Qt6 Application Launcher for NexWM / NexDE
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QKeyEvent>
#include <QScreen>
#include <QIcon>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QPushButton>
#include <cstdlib>

struct AppItem {
    QString name;
    QString exec;
    QString icon;
    QString comment;
    QString category;
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
            val.remove(QRegularExpression("%[fFuUiDcK]"));
            app.exec = val.trimmed();
        }
        else if (key == "Icon") app.icon = val;
        else if (key == "Comment") app.comment = val;
        else if (key == "Categories") app.category = val;
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

/* Custom Item Delegate for two-line rendering (Name + Comment) */
class AppItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();

        bool isSelected = option.state & QStyle::State_Selected;

        // Background
        QRect rect = option.rect;
        if (isSelected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#5b8dd9"));
            painter->drawRoundedRect(rect.adjusted(2, 2, -2, -2), 6, 6);
        } else if (option.state & QStyle::State_MouseOver) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#313244"));
            painter->drawRoundedRect(rect.adjusted(2, 2, -2, -2), 6, 6);
        }

        // Icon
        QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        int iconSize = 32;
        QRect iconRect(rect.left() + 10, rect.top() + (rect.height() - iconSize) / 2, iconSize, iconSize);
        if (!icon.isNull()) {
            icon.paint(painter, iconRect);
        } else {
            // Draw initial avatar icon
            QString name = index.data(Qt::DisplayRole).toString();
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#45475a"));
            painter->drawRoundedRect(iconRect, 6, 6);
            painter->setPen(QColor("#cdd6f4"));
            painter->setFont(QFont("Monospace", 12, QFont::Bold));
            painter->drawText(iconRect, Qt::AlignCenter, name.left(1).toUpper());
        }

        // Text
        QString name = index.data(Qt::DisplayRole).toString();
        QString comment = index.data(Qt::UserRole + 1).toString();

        int textLeft = iconRect.right() + 12;
        int textWidth = rect.right() - textLeft - 10;

        // Title
        painter->setFont(QFont("Sans-Serif", 11, QFont::Bold));
        painter->setPen(isSelected ? QColor("#ffffff") : QColor("#cdd6f4"));
        QRect titleRect(textLeft, rect.top() + (comment.isEmpty() ? 12 : 6), textWidth, 20);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, name);

        // Subtitle / Comment
        if (!comment.isEmpty()) {
            painter->setFont(QFont("Sans-Serif", 9));
            painter->setPen(isSelected ? QColor("#e0e0e0") : QColor("#a6adc8"));
            QRect subRect(textLeft, titleRect.bottom() + 1, textWidth, 16);
            painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, comment);
        }

        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(0, 52);
    }
};

class LauncherWindow : public QWidget {
public:
    LauncherWindow(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground, false);
        resize(640, 440);

        // Center on primary screen
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            QRect geom = screen->geometry();
            move(geom.x() + (geom.width() - width()) / 2, geom.y() + (geom.height() - height()) / 3);
        }

        m_allApps = scanApps();

        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(16, 16, 16, 12);
        mainLayout->setSpacing(10);

        // Search Bar Container
        auto *searchContainer = new QHBoxLayout();
        searchContainer->setSpacing(8);

        m_searchEdit = new QLineEdit(this);
        m_searchEdit->setPlaceholderText("🔍 Search applications, settings, utilities...");
        searchContainer->addWidget(m_searchEdit);
        mainLayout->addLayout(searchContainer);

        // Category Filter Buttons
        auto *catLayout = new QHBoxLayout();
        catLayout->setSpacing(6);

        QStringList categories = {"All", "System", "Settings", "Utilities", "Development", "Office"};
        for (const QString &cat : categories) {
            auto *btn = new QPushButton(cat, this);
            btn->setCheckable(true);
            btn->setFocusPolicy(Qt::NoFocus);
            if (cat == "All") btn->setChecked(true);
            m_catButtons.append(btn);
            catLayout->addWidget(btn);
            connect(btn, &QPushButton::clicked, this, [this, cat]() {
                for (auto *b : m_catButtons) b->setChecked(b->text() == cat);
                m_currentCategory = (cat == "All") ? "" : cat;
                filterList(m_searchEdit->text());
            });
        }
        mainLayout->addLayout(catLayout);

        // Application List Widget
        m_listWidget = new QListWidget(this);
        m_listWidget->setItemDelegate(new AppItemDelegate(m_listWidget));
        m_listWidget->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        mainLayout->addWidget(m_listWidget);

        // Footer Hint Bar
        auto *footerLayout = new QHBoxLayout();
        auto *hintLabel = new QLabel("<b>Enter</b> Launch &nbsp;•&nbsp; <b>Esc</b> Close &nbsp;•&nbsp; <b>↑↓</b> Navigate", this);
        hintLabel->setStyleSheet("color: #a6adc8; font-size: 11px;");
        footerLayout->addWidget(hintLabel, 0, Qt::AlignRight);
        mainLayout->addLayout(footerLayout);

        // Theme Stylesheet
        setStyleSheet(R"(
            QWidget {
                background-color: #1e1e2e;
                color: #cdd6f4;
                border-radius: 12px;
                font-family: 'Segoe UI', 'Ubuntu', sans-serif;
            }
            QLineEdit {
                background-color: #181825;
                color: #cdd6f4;
                border: 2px solid #313244;
                border-radius: 8px;
                padding: 10px 14px;
                font-size: 15px;
                selection-background-color: #5b8dd9;
            }
            QLineEdit:focus {
                border: 2px solid #5b8dd9;
            }
            QPushButton {
                background-color: #181825;
                color: #a6adc8;
                border: 1px solid #313244;
                border-radius: 6px;
                padding: 4px 10px;
                font-size: 12px;
                font-weight: bold;
            }
            QPushButton:checked {
                background-color: #5b8dd9;
                color: #ffffff;
                border: 1px solid #5b8dd9;
            }
            QPushButton:hover:!checked {
                background-color: #313244;
                color: #cdd6f4;
            }
            QListWidget {
                background-color: #181825;
                border: 1px solid #313244;
                border-radius: 8px;
                padding: 4px;
                outline: none;
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
            bool matchesCategory = m_currentCategory.isEmpty() ||
                app.category.contains(m_currentCategory, Qt::CaseInsensitive);

            bool matchesQuery = query.isEmpty() ||
                app.name.contains(query, Qt::CaseInsensitive) ||
                app.comment.contains(query, Qt::CaseInsensitive) ||
                app.exec.contains(query, Qt::CaseInsensitive);

            if (matchesCategory && matchesQuery) {
                auto *item = new QListWidgetItem(app.name, m_listWidget);
                item->setData(Qt::UserRole, app.exec);
                item->setData(Qt::UserRole + 1, app.comment);

                QIcon icon = QIcon::fromTheme(app.icon);
                if (!icon.isNull()) {
                    item->setIcon(icon);
                }
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
    QList<QPushButton*> m_catButtons;
    QString m_currentCategory;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    LauncherWindow win;
    win.show();
    return app.exec();
}
