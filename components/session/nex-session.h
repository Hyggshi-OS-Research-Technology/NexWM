/*
 * nex-session.h - Session & Power Exit Dialog for NexWM / NexDE
 * Part of Nex Desktop Environment
 */

#ifndef NEX_SESSION_H
#define NEX_SESSION_H

#include <QWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>

class SessionWindow : public QWidget {
public:
    explicit SessionWindow(QWidget *parent = nullptr);
    ~SessionWindow() override = default;

private:
    void actionLock();
    void actionLogout();
    void actionReboot();
    void actionShutdown();
};

#endif // NEX_SESSION_H
