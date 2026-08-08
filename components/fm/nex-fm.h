/*
 * nex-fm.h - NexFM File Manager Header
 * Part of Nex Desktop Environment
 */

#ifndef NEX_FM_H
#define NEX_FM_H

#include <QMainWindow>
#include <QFileSystemModel>
#include <QTreeView>
#include <QListView>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QToolBar>
#include <QAction>
#include <QSplitter>
#include <QMenu>
#include <QStatusBar>
#include <QModelIndex>
#include <QStringList>
#include <QFileInfo>

class FileManagerWindow : public QMainWindow {
public:
    explicit FileManagerWindow(const QString &initialPath = QString(), QWidget *parent = nullptr);
    ~FileManagerWindow() override = default;

private slots:
    void navigateTo(const QString &path);
    void onAddressEntered();
    void onSidebarClicked(QListWidgetItem *item);
    void onItemDoubleClicked(const QModelIndex &index);
    void showContextMenu(const QPoint &pos);
    
    /* File Actions */
    void actionBack();
    void actionForward();
    void actionUp();
    void actionHome();
    void actionRefresh();
    void actionNewFolder();
    void actionNewFile();
    void actionRename();
    void actionDelete();
    void actionCopy();
    void actionCut();
    void actionPaste();
    void actionProperties();
    void filterFiles(const QString &query);
    void updateStatusBar();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setupUI();
    void setupSidebar();
    void setupToolBar();
    void setupContextMenu();

    QFileSystemModel *m_fileModel;
    QTreeView *m_treeView;
    QSplitter *m_splitter;
    QListWidget *m_sidebar;
    
    /* Navigation history */
    QStringList m_history;
    int m_historyIdx;

    /* Toolbar controls */
    QLineEdit *m_pathEdit;
    QLineEdit *m_searchEdit;
    QAction *m_actBack;
    QAction *m_actForward;
    QAction *m_actUp;
    QAction *m_actHome;
    QAction *m_actRefresh;

    /* Status bar */
    QLabel *m_statusLabel;

    /* Clipboard state */
    QStringList m_clipboardPaths;
    bool m_clipboardIsCut;
};

#endif // NEX_FM_H
