/*
 * nex-fm.cpp - Qt6 File Manager for NexWM / NexDE
 */

#include "nex-fm.h"
#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QClipboard>
#include <QShortcut>
#include <QKeyEvent>
#include <QStorageInfo>
#include <QIcon>
#include <QStackedWidget>
#include <cstdlib>

FileManagerWindow::FileManagerWindow(const QString &initialPath, QWidget *parent)
    : QMainWindow(parent), m_isIconMode(false), m_historyIdx(-1), m_clipboardIsCut(false)
{
    setWindowTitle("NexFM — File Manager");
    resize(960, 600);

    setupUI();

    QString startPath = initialPath;
    if (startPath.isEmpty()) {
        const char *home = getenv("HOME");
        startPath = home ? QString(home) : QDir::homePath();
    }
    navigateTo(startPath);
}

void FileManagerWindow::setupUI()
{
    setStyleSheet(R"(
        QMainWindow {
            background-color: #1e1e2e;
            color: #cdd6f4;
            font-family: 'Segoe UI', 'Ubuntu', sans-serif;
        }
        QToolBar {
            background-color: #181825;
            border-bottom: 1px solid #313244;
            spacing: 6px;
            padding: 6px;
        }
        QToolButton {
            background-color: #313244;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 6px 10px;
            font-weight: bold;
        }
        QToolButton:hover {
            background-color: #45475a;
            color: #ffffff;
        }
        QLineEdit {
            background-color: #181825;
            color: #cdd6f4;
            border: 1px solid #45475a;
            border-radius: 6px;
            padding: 5px 10px;
            font-size: 13px;
        }
        QLineEdit:focus {
            border: 1px solid #5b8dd9;
        }
        QSplitter::handle {
            background-color: #313244;
        }
        QListWidget {
            background-color: #181825;
            color: #cdd6f4;
            border: none;
            font-size: 13px;
        }
        QListWidget::item {
            padding: 8px 12px;
            border-radius: 6px;
        }
        QListWidget::item:hover {
            background-color: #313244;
        }
        QListWidget::item:selected {
            background-color: #5b8dd9;
            color: #ffffff;
        }
        QTreeView, QListView {
            background-color: #1e1e2e;
            color: #cdd6f4;
            border: none;
            font-size: 13px;
            outline: none;
        }
        QTreeView::item, QListView::item {
            padding: 6px 8px;
            border-radius: 4px;
        }
        QTreeView::item:hover, QListView::item:hover {
            background-color: #313244;
        }
        QTreeView::item:selected, QListView::item:selected {
            background-color: #5b8dd9;
            color: #ffffff;
        }
        QHeaderView::section {
            background-color: #181825;
            color: #a6adc8;
            padding: 6px 10px;
            border: 1px solid #313244;
            font-weight: bold;
        }
        QStatusBar {
            background-color: #181825;
            color: #a6adc8;
            border-top: 1px solid #313244;
            font-size: 12px;
        }
        QMenu {
            background-color: #1e1e2e;
            color: #cdd6f4;
            border: 1px solid #45475a;
            padding: 6px;
            border-radius: 6px;
        }
        QMenu::item {
            padding: 6px 22px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background-color: #5b8dd9;
            color: #ffffff;
        }
    )");

    setupToolBar();

    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    setupSidebar();

    // File Model
    m_fileModel = new QFileSystemModel(this);
    m_fileModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    m_fileModel->setReadOnly(false);

    // Tree Details View
    m_treeView = new QTreeView(this);
    m_treeView->setModel(m_fileModel);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setSortingEnabled(true);
    m_treeView->sortByColumn(0, Qt::AscendingOrder);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeView->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_treeView->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_treeView->header()->setSectionResizeMode(3, QHeaderView::Interactive);
    m_treeView->setColumnWidth(1, 90);
    m_treeView->setColumnWidth(2, 110);
    m_treeView->setColumnWidth(3, 140);

    // Icon Grid View
    m_iconView = new QListView(this);
    m_iconView->setModel(m_fileModel);
    m_iconView->setViewMode(QListView::IconMode);
    m_iconView->setIconSize(QSize(48, 48));
    m_iconView->setGridSize(QSize(96, 84));
    m_iconView->setMovement(QListView::Static);
    m_iconView->setResizeMode(QListView::Adjust);
    m_iconView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_iconView->hide();

    m_splitter->addWidget(m_treeView);
    m_splitter->addWidget(m_iconView);
    m_splitter->setSizes({220, 740});

    // Status bar
    m_statusLabel = new QLabel("Ready", this);
    statusBar()->addWidget(m_statusLabel);

    // Signals
    connect(m_treeView, &QTreeView::doubleClicked, this, &FileManagerWindow::onItemDoubleClicked);
    connect(m_treeView, &QTreeView::customContextMenuRequested, this, &FileManagerWindow::showContextMenu);

    connect(m_iconView, &QListView::doubleClicked, this, &FileManagerWindow::onItemDoubleClicked);
    connect(m_iconView, &QListView::customContextMenuRequested, this, &FileManagerWindow::showContextMenu);

    connect(m_pathEdit, &QLineEdit::returnPressed, this, &FileManagerWindow::onAddressEntered);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &FileManagerWindow::filterFiles);
}

void FileManagerWindow::setupToolBar()
{
    QToolBar *tb = addToolBar("Navigation");
    tb->setMovable(false);

    m_actBack = tb->addAction(QIcon::fromTheme("go-previous"), "Back", this, &FileManagerWindow::actionBack);
    m_actForward = tb->addAction(QIcon::fromTheme("go-next"), "Forward", this, &FileManagerWindow::actionForward);
    m_actUp = tb->addAction(QIcon::fromTheme("go-up"), "Up", this, &FileManagerWindow::actionUp);
    m_actHome = tb->addAction(QIcon::fromTheme("user-home"), "Home", this, &FileManagerWindow::actionHome);
    m_actRefresh = tb->addAction(QIcon::fromTheme("view-refresh"), "Refresh", this, &FileManagerWindow::actionRefresh);

    tb->addSeparator();

    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText("Enter path...");
    tb->addWidget(m_pathEdit);

    tb->addSeparator();

    tb->addAction(QIcon::fromTheme("folder-new"), "+ Folder", this, &FileManagerWindow::actionNewFolder);
    tb->addAction(QIcon::fromTheme("document-new"), "+ File", this, &FileManagerWindow::actionNewFile);

    tb->addSeparator();

    m_actViewMode = tb->addAction(QIcon::fromTheme("view-grid"), "Grid View", this, &FileManagerWindow::toggleViewMode);

    tb->addSeparator();

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("🔍 Filter...");
    m_searchEdit->setMaximumWidth(160);
    tb->addWidget(m_searchEdit);
}

void FileManagerWindow::setupSidebar()
{
    m_sidebar = new QListWidget(this);
    m_sidebar->setMaximumWidth(220);

    const char *home = getenv("HOME");
    QString homeDir = home ? QString(home) : QDir::homePath();

    struct Place {
        QString name;
        QString path;
        QString icon;
    };

    QList<Place> places = {
        {"Home", homeDir, "user-home"},
        {"Desktop", homeDir + "/Desktop", "user-desktop"},
        {"Documents", homeDir + "/Documents", "folder-documents"},
        {"Downloads", homeDir + "/Downloads", "folder-download"},
        {"Music", homeDir + "/Music", "folder-music"},
        {"Pictures", homeDir + "/Pictures", "folder-pictures"},
        {"Videos", homeDir + "/Videos", "folder-videos"},
        {"Root (/)", "/", "drive-harddisk"},
        {"Tmp (/tmp)", "/tmp", "folder-temp"}
    };

    for (const auto &p : places) {
        if (QDir(p.path).exists()) {
            auto *item = new QListWidgetItem(QIcon::fromTheme(p.icon), p.name, m_sidebar);
            item->setData(Qt::UserRole, p.path);
        }
    }

    m_splitter->addWidget(m_sidebar);
    connect(m_sidebar, &QListWidget::itemClicked, this, &FileManagerWindow::onSidebarClicked);
}

void FileManagerWindow::toggleViewMode()
{
    m_isIconMode = !m_isIconMode;
    if (m_isIconMode) {
        m_treeView->hide();
        m_iconView->show();
        m_actViewMode->setText("List View");
    } else {
        m_iconView->hide();
        m_treeView->show();
        m_actViewMode->setText("Grid View");
    }
}

void FileManagerWindow::navigateTo(const QString &path)
{
    QFileInfo fi(path);
    QString cleanPath = fi.canonicalFilePath();
    if (cleanPath.isEmpty()) cleanPath = path;

    if (!QDir(cleanPath).exists()) return;

    QModelIndex rootIdx = m_fileModel->setRootPath(cleanPath);
    m_treeView->setRootIndex(rootIdx);
    m_iconView->setRootIndex(rootIdx);
    m_pathEdit->setText(cleanPath);

    // Navigation history
    if (m_historyIdx < 0 || m_history[m_historyIdx] != cleanPath) {
        while (m_history.size() > m_historyIdx + 1) {
            m_history.removeLast();
        }
        m_history.append(cleanPath);
        m_historyIdx = m_history.size() - 1;
    }

    m_actBack->setEnabled(m_historyIdx > 0);
    m_actForward->setEnabled(m_historyIdx < m_history.size() - 1);

    updateStatusBar();
}

void FileManagerWindow::onAddressEntered()
{
    navigateTo(m_pathEdit->text().trimmed());
}

void FileManagerWindow::onSidebarClicked(QListWidgetItem *item)
{
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();
    if (!path.isEmpty()) navigateTo(path);
}

void FileManagerWindow::onItemDoubleClicked(const QModelIndex &index)
{
    QString path = m_fileModel->filePath(index);
    QFileInfo fi(path);

    if (fi.isDir()) {
        navigateTo(path);
    } else if (fi.isExecutable() && !fi.isDir()) {
        QProcess::startDetached(path, {});
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void FileManagerWindow::actionBack()
{
    if (m_historyIdx > 0) {
        m_historyIdx--;
        QString path = m_history[m_historyIdx];
        QModelIndex rootIdx = m_fileModel->setRootPath(path);
        m_treeView->setRootIndex(rootIdx);
        m_iconView->setRootIndex(rootIdx);
        m_pathEdit->setText(path);
        m_actBack->setEnabled(m_historyIdx > 0);
        m_actForward->setEnabled(m_historyIdx < m_history.size() - 1);
        updateStatusBar();
    }
}

void FileManagerWindow::actionForward()
{
    if (m_historyIdx < m_history.size() - 1) {
        m_historyIdx++;
        QString path = m_history[m_historyIdx];
        QModelIndex rootIdx = m_fileModel->setRootPath(path);
        m_treeView->setRootIndex(rootIdx);
        m_iconView->setRootIndex(rootIdx);
        m_pathEdit->setText(path);
        m_actBack->setEnabled(m_historyIdx > 0);
        m_actForward->setEnabled(m_historyIdx < m_history.size() - 1);
        updateStatusBar();
    }
}

void FileManagerWindow::actionUp()
{
    QString curr = m_pathEdit->text();
    QDir dir(curr);
    if (dir.cdUp()) {
        navigateTo(dir.absolutePath());
    }
}

void FileManagerWindow::actionHome()
{
    const char *home = getenv("HOME");
    navigateTo(home ? QString(home) : QDir::homePath());
}

void FileManagerWindow::actionRefresh()
{
    navigateTo(m_pathEdit->text());
}

void FileManagerWindow::actionNewFolder()
{
    QString currDir = m_pathEdit->text();
    bool ok;
    QString name = QInputDialog::getText(this, "New Folder", "Folder Name:", QLineEdit::Normal, "New Folder", &ok);
    if (ok && !name.isEmpty()) {
        QDir dir(currDir);
        if (!dir.mkdir(name)) {
            QMessageBox::warning(this, "Error", "Failed to create directory!");
        }
    }
}

void FileManagerWindow::actionNewFile()
{
    QString currDir = m_pathEdit->text();
    bool ok;
    QString name = QInputDialog::getText(this, "New File", "File Name:", QLineEdit::Normal, "new_file.txt", &ok);
    if (ok && !name.isEmpty()) {
        QString fullPath = currDir + "/" + name;
        QFile file(fullPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.close();
        } else {
            QMessageBox::warning(this, "Error", "Failed to create file!");
        }
    }
}

void FileManagerWindow::actionRename()
{
    QModelIndex idx = m_isIconMode ? m_iconView->currentIndex() : m_treeView->currentIndex();
    if (!idx.isValid()) return;

    QString oldPath = m_fileModel->filePath(idx);
    QFileInfo fi(oldPath);

    bool ok;
    QString newName = QInputDialog::getText(this, "Rename", "New Name:", QLineEdit::Normal, fi.fileName(), &ok);
    if (ok && !newName.isEmpty() && newName != fi.fileName()) {
        QString newPath = fi.absolutePath() + "/" + newName;
        if (!QFile::rename(oldPath, newPath)) {
            QMessageBox::warning(this, "Error", "Failed to rename file!");
        }
    }
}

void FileManagerWindow::actionDelete()
{
    QModelIndex idx = m_isIconMode ? m_iconView->currentIndex() : m_treeView->currentIndex();
    if (!idx.isValid()) return;

    QString path = m_fileModel->filePath(idx);
    QFileInfo fi(path);

    int res = QMessageBox::question(this, "Confirm Delete",
                                    QString("Are you sure you want to delete '%1'?").arg(fi.fileName()),
                                    QMessageBox::Yes | QMessageBox::No);
    if (res == QMessageBox::Yes) {
        if (fi.isDir()) {
            QDir(path).removeRecursively();
        } else {
            QFile::remove(path);
        }
    }
}

void FileManagerWindow::actionCopy()
{
    QModelIndex idx = m_isIconMode ? m_iconView->currentIndex() : m_treeView->currentIndex();
    if (!idx.isValid()) return;

    m_clipboardPaths = { m_fileModel->filePath(idx) };
    m_clipboardIsCut = false;
    statusBar()->showMessage("Copied to clipboard", 2000);
}

void FileManagerWindow::actionCut()
{
    QModelIndex idx = m_isIconMode ? m_iconView->currentIndex() : m_treeView->currentIndex();
    if (!idx.isValid()) return;

    m_clipboardPaths = { m_fileModel->filePath(idx) };
    m_clipboardIsCut = true;
    statusBar()->showMessage("Cut to clipboard", 2000);
}

void FileManagerWindow::actionPaste()
{
    if (m_clipboardPaths.isEmpty()) return;

    QString targetDir = m_pathEdit->text();
    for (const QString &src : m_clipboardPaths) {
        QFileInfo fi(src);
        QString dest = targetDir + "/" + fi.fileName();

        if (m_clipboardIsCut) {
            QFile::rename(src, dest);
        } else {
            if (fi.isDir()) {
                QProcess::execute("cp", {"-r", src, dest});
            } else {
                QFile::copy(src, dest);
            }
        }
    }

    if (m_clipboardIsCut) m_clipboardPaths.clear();
    statusBar()->showMessage("Pasted successfully", 2000);
}

void FileManagerWindow::actionProperties()
{
    QModelIndex idx = m_isIconMode ? m_iconView->currentIndex() : m_treeView->currentIndex();
    if (!idx.isValid()) return;

    QString path = m_fileModel->filePath(idx);
    QFileInfo fi(path);

    QString infoStr = QString(
        "<b>Name:</b> %1<br>"
        "<b>Path:</b> %2<br>"
        "<b>Type:</b> %3<br>"
        "<b>Size:</b> %4 bytes<br>"
        "<b>Created:</b> %5<br>"
        "<b>Last Modified:</b> %6<br>"
        "<b>Writable:</b> %7<br>"
        "<b>Executable:</b> %8"
    ).arg(fi.fileName())
     .arg(fi.absoluteFilePath())
     .arg(fi.isDir() ? "Directory" : (fi.isExecutable() ? "Executable Application" : "File"))
     .arg(fi.size())
     .arg(fi.birthTime().toString("yyyy-MM-dd HH:mm:ss"))
     .arg(fi.lastModified().toString("yyyy-MM-dd HH:mm:ss"))
     .arg(fi.isWritable() ? "Yes" : "No")
     .arg(fi.isExecutable() ? "Yes" : "No");

    QMessageBox::information(this, "File Properties", infoStr);
}

void FileManagerWindow::filterFiles(const QString &query)
{
    if (query.isEmpty()) {
        m_fileModel->setNameFilters({});
        m_fileModel->setNameFilterDisables(false);
    } else {
        m_fileModel->setNameFilters({ QString("*%1*").arg(query) });
        m_fileModel->setNameFilterDisables(false);
    }
}

void FileManagerWindow::updateStatusBar()
{
    QString currPath = m_pathEdit->text();
    QDir dir(currPath);
    int count = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();

    QStorageInfo storage(currPath);
    double freeGB = (double)storage.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);

    m_statusLabel->setText(QString("%1 items | Free space: %2 GB")
                           .arg(count)
                           .arg(freeGB, 0, 'f', 2));
}

void FileManagerWindow::showContextMenu(const QPoint &pos)
{
    QWidget *activeView = m_isIconMode ? static_cast<QWidget*>(m_iconView) : static_cast<QWidget*>(m_treeView);
    QModelIndex idx = m_isIconMode ? m_iconView->indexAt(pos) : m_treeView->indexAt(pos);
    QMenu menu(this);

    if (idx.isValid()) {
        menu.addAction(QIcon::fromTheme("document-open"), "Open", [this, idx]() { onItemDoubleClicked(idx); });
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-copy"), "Copy", this, &FileManagerWindow::actionCopy);
        menu.addAction(QIcon::fromTheme("edit-cut"), "Cut", this, &FileManagerWindow::actionCut);
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("edit-rename"), "Rename", this, &FileManagerWindow::actionRename);
        menu.addAction(QIcon::fromTheme("edit-delete"), "Delete", this, &FileManagerWindow::actionDelete);
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("dialog-information"), "Properties", this, &FileManagerWindow::actionProperties);
    } else {
        menu.addAction(QIcon::fromTheme("folder-new"), "New Folder", this, &FileManagerWindow::actionNewFolder);
        menu.addAction(QIcon::fromTheme("document-new"), "New File", this, &FileManagerWindow::actionNewFile);
        if (!m_clipboardPaths.isEmpty()) {
            menu.addSeparator();
            menu.addAction(QIcon::fromTheme("edit-paste"), "Paste", this, &FileManagerWindow::actionPaste);
        }
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme("view-refresh"), "Refresh", this, &FileManagerWindow::actionRefresh);
    }

    menu.exec(activeView->mapToGlobal(pos));
}

void FileManagerWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete) {
        actionDelete();
    } else if (event->key() == Qt::Key_F5) {
        actionRefresh();
    } else if (event->key() == Qt::Key_F2) {
        actionRename();
    } else if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C) actionCopy();
        else if (event->key() == Qt::Key_X) actionCut();
        else if (event->key() == Qt::Key_V) actionPaste();
        else if (event->key() == Qt::Key_N) actionNewFolder();
        else QMainWindow::keyPressEvent(event);
    } else {
        QMainWindow::keyPressEvent(event);
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QString initialPath = (argc > 1) ? QString(argv[1]) : QString();

    FileManagerWindow win(initialPath);
    win.show();
    return app.exec();
}
