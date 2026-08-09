/*
 * nex-terminal.cpp - NexTerminal
 * Qt6 terminal emulator using a POSIX pseudo-terminal (PTY).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nex-terminal.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <QTextStream>
#include <QTimer>

extern "C" {
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
const QColor kDefaultFg("#cdd6f4");
const QColor kDefaultBg("#1e1e2e");

static int positiveParam(const QList<int> &p, int index, int fallback = 1)
{
    if (index < 0 || index >= p.size() || p[index] <= 0)
        return fallback;
    return p[index];
}

static void setCloseOnExec(int fd, bool enabled)
{
    if (fd < 0)
        return;
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        return;
    ::fcntl(fd, F_SETFD, enabled ? (flags | FD_CLOEXEC)
                                 : (flags & ~FD_CLOEXEC));
}
} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setCursor(Qt::IBeamCursor);

    QFont f(QStringLiteral("monospace"));
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setPointSize(11);
    setFont(f);

    resetTerminal();
    setupScrollingFrame();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(8);
    connect(m_pollTimer, &QTimer::timeout, this, &TerminalWidget::pollPty);
}

TerminalWidget::~TerminalWidget()
{
    shutdown();
}

bool TerminalWidget::startShell()
{
    if (m_master >= 0 || m_pid > 0)
        return true;

    if (openpty(&m_master, &m_slave, nullptr, nullptr, nullptr) != 0) {
        std::perror("nex-terminal: openpty");
        m_master = m_slave = -1;
        return false;
    }

    const int flags = ::fcntl(m_master, F_GETFL);
    if (flags >= 0)
        ::fcntl(m_master, F_SETFL, flags | O_NONBLOCK);

    // The parent never execs the master. Keeping it non-CLOEXEC is unnecessary.
    setCloseOnExec(m_master, true);

    QString shell = QString::fromLocal8Bit(qgetenv("SHELL")).trimmed();
    if (shell.isEmpty())
        shell = QStringLiteral("/bin/sh");

    const QByteArray shellPath = shell.toLocal8Bit();

    m_pid = ::fork();
    if (m_pid < 0) {
        std::perror("nex-terminal: fork");
        ::close(m_master);
        ::close(m_slave);
        m_master = m_slave = -1;
        m_pid = -1;
        return false;
    }

    if (m_pid == 0) {
        // Child: make the PTY slave our controlling terminal.
        if (::setsid() < 0)
            _exit(1);

#ifdef TIOCSCTTY
        (void)::ioctl(m_slave, TIOCSCTTY, 0);
#endif

        if (::dup2(m_slave, STDIN_FILENO) < 0 ||
            ::dup2(m_slave, STDOUT_FILENO) < 0 ||
            ::dup2(m_slave, STDERR_FILENO) < 0) {
            _exit(1);
        }

        if (m_slave > STDERR_FILENO)
            ::close(m_slave);
        ::close(m_master);

        // Let the shell configure canonical/raw mode itself.
        struct termios tio {};
        if (::tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_iflag |= ICRNL | IXON;
            tio.c_oflag |= OPOST | ONLCR;
            tio.c_lflag |= ECHO | ECHOE | ECHOK | ICANON | ISIG | IEXTEN;
            (void)::tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }

        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "NexTerminal", 1);
        ::setenv("TERM_PROGRAM_VERSION", "1.0", 1);
        ::setenv("SHELL", shellPath.constData(), 1);

        if (const char *home = ::getenv("HOME"); home && *home)
            (void)::chdir(home);

        // Interactive mode is required for prompts, job control and readline.
        ::execl(shellPath.constData(), shellPath.constData(), "-i",
                static_cast<char *>(nullptr));
        _exit(127);
    }

    ::close(m_slave);
    m_slave = -1;

    setPtySize();
    m_pollTimer->start();
    setFocus(Qt::OtherFocusReason);
    return true;
}

void TerminalWidget::shutdown()
{
    if (m_pollTimer)
        m_pollTimer->stop();

    if (m_pid > 0) {
        const pid_t pid = m_pid;
        m_pid = -1;

        // Do not use waitpid() indefinitely from the GUI thread.
        (void)::kill(-pid, SIGHUP);
        (void)::kill(pid, SIGHUP);

        int status = 0;
        pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == 0) {
            (void)::kill(-pid, SIGTERM);
            (void)::kill(pid, SIGTERM);

            for (int i = 0; i < 20; ++i) {
                result = ::waitpid(pid, &status, WNOHANG);
                if (result == pid || (result < 0 && errno == ECHILD))
                    break;
                usleep(10000);
            }

            if (result == 0) {
                (void)::kill(-pid, SIGKILL);
                (void)::kill(pid, SIGKILL);
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    }

    if (m_master >= 0) {
        ::close(m_master);
        m_master = -1;
    }
    if (m_slave >= 0) {
        ::close(m_slave);
        m_slave = -1;
    }

    m_writeBuf.clear();
    m_writePending = false;
}

void TerminalWidget::resetTerminal()
{
    m_screen.clear();
    m_altScreen.clear();
    m_scrollback.clear();
    m_scrollOffset = 0;

    m_cx = 0;
    m_cy = 0;
    m_savedCx = 0;
    m_savedCy = 0;
    m_altSavedCx = 0;
    m_altSavedCy = 0;

    m_cursorOn = true;
    m_appCursorKeys = false;
    m_altScreenActive = false;
    m_wrapPending = false;

    m_fg = kDefaultFg;
    m_bg = kDefaultBg;
    m_bold = false;

    m_state = NORM;
    m_text.clear();
    m_csi.clear();
    m_osc.clear();

    ensureCells();
}

TerminalWidget::TermCell TerminalWidget::blankCell() const
{
    TermCell cell;
    cell.ch = QLatin1Char(' ');
    cell.fg = m_fg;
    cell.bg = m_bg;
    cell.bold = false;
    return cell;
}

void TerminalWidget::ensureCells()
{
    m_rows = qMax(2, m_rows);
    m_cols = qMax(2, m_cols);

    QVector<QVector<TermCell>> &screen =
        m_altScreenActive ? m_altScreen : m_screen;

    if (screen.size() != m_rows)
        screen.resize(m_rows);

    for (auto &row : screen) {
        const int oldSize = row.size();
        if (oldSize != m_cols)
            row.resize(m_cols);

        for (int c = oldSize; c < m_cols; ++c)
            row[c] = blankCell();
    }

    // Newly-created rows need the same defaults too.
    for (int r = 0; r < screen.size(); ++r) {
        if (screen[r].size() != m_cols)
            screen[r].resize(m_cols);
        for (int c = 0; c < screen[r].size(); ++c) {
            if (!screen[r][c].fg.isValid())
                screen[r][c].fg = m_fg;
            if (!screen[r][c].bg.isValid())
                screen[r][c].bg = m_bg;
        }
    }
}

QVector<QVector<TerminalWidget::TermCell>> &TerminalWidget::activeScreen()
{
    return m_altScreenActive ? m_altScreen : m_screen;
}

const QVector<QVector<TerminalWidget::TermCell>> &
TerminalWidget::activeScreen() const
{
    return m_altScreenActive ? m_altScreen : m_screen;
}

void TerminalWidget::newline()
{
    m_cx = 0;
    m_wrapPending = false;
    ++m_cy;

    if (m_cy >= m_rows) {
        m_cy = m_rows - 1;
        scroll();
    }
}

void TerminalWidget::reverseIndex()
{
    m_wrapPending = false;
    if (m_cy > 0) {
        --m_cy;
        return;
    }

    auto &screen = activeScreen();
    if (screen.isEmpty())
        return;

    screen.removeLast();
    screen.prepend(QVector<TermCell>(m_cols));
    for (TermCell &cell : screen.front())
        cell = blankCell();
}

void TerminalWidget::scroll()
{
    auto &screen = activeScreen();
    if (screen.isEmpty())
        return;

    if (!m_altScreenActive) {
        const bool wasAtBottom = (m_scrollOffset == 0);

        m_scrollback.append(screen.front());

        if (m_scrollback.size() > m_scrollbackLimit) {
            const int removed =
                m_scrollback.size() - m_scrollbackLimit;
            m_scrollback.remove(0, removed);

            // Preserve the user's viewport when possible.
            if (!wasAtBottom)
                m_scrollOffset = qMax(0, m_scrollOffset - removed);
        }

        if (wasAtBottom)
            m_scrollOffset = 0;
    }

    screen.removeFirst();
    screen.append(QVector<TermCell>(m_cols));
    for (TermCell &cell : screen.back())
        cell = blankCell();

    updateScrollBar();
    update();
}

void TerminalWidget::writeChar(QChar ch)
{
    if (m_rows <= 0 || m_cols <= 0)
        return;

    ensureCells();

    if (m_wrapPending) {
        m_cx = 0;
        ++m_cy;
        m_wrapPending = false;
        if (m_cy >= m_rows) {
            m_cy = m_rows - 1;
            scroll();
        }
    }

    if (ch == QLatin1Char('\r')) {
        m_cx = 0;
        m_wrapPending = false;
        return;
    }
    if (ch == QLatin1Char('\n')) {
        newline();
        return;
    }
    if (ch == QLatin1Char('\b')) {
        if (m_cx > 0)
            --m_cx;
        m_wrapPending = false;
        return;
    }
    if (ch == QLatin1Char('\t')) {
        const int nextTab = ((m_cx / 8) + 1) * 8;
        m_cx = qMin(nextTab, m_cols - 1);
        m_wrapPending = false;
        return;
    }

    if (ch.unicode() < 0x20 || ch.unicode() == 0x7f)
        return;

    TermCell &cell = activeScreen()[m_cy][m_cx];
    cell.ch = ch;
    cell.fg = m_fg;
    cell.bg = m_bg;
    cell.bold = m_bold;

    if (m_cx == m_cols - 1) {
        // VT-style pending wrap: don't move until another printable arrives.
        m_wrapPending = true;
    } else {
        ++m_cx;
    }
}

void TerminalWidget::moveCursor(int dx, int dy)
{
    m_wrapPending = false;
    m_cx = qBound(0, m_cx + dx, m_cols - 1);
    m_cy = qBound(0, m_cy + dy, m_rows - 1);
}

void TerminalWidget::setCursorPos(int col, int row)
{
    m_wrapPending = false;
    m_cx = qBound(0, col, m_cols - 1);
    m_cy = qBound(0, row, m_rows - 1);
}

void TerminalWidget::saveCursor()
{
    m_savedCx = m_cx;
    m_savedCy = m_cy;
}

void TerminalWidget::restoreCursor()
{
    setCursorPos(m_savedCx, m_savedCy);
}

void TerminalWidget::clearScreen(int mode)
{
    ensureCells();
    auto &screen = activeScreen();

    if (mode == 2 || mode == 3) {
        for (auto &row : screen)
            for (TermCell &cell : row)
                cell = blankCell();
        setCursorPos(0, 0);
        return;
    }

    if (mode == 0) {
        for (int r = m_cy; r < m_rows; ++r) {
            const int start = (r == m_cy) ? m_cx : 0;
            for (int c = start; c < m_cols; ++c)
                screen[r][c] = blankCell();
        }
    } else if (mode == 1) {
        for (int r = 0; r <= m_cy; ++r) {
            const int end = (r == m_cy) ? m_cx : m_cols - 1;
            for (int c = 0; c <= end; ++c)
                screen[r][c] = blankCell();
        }
    }
}

void TerminalWidget::clearLine(int mode)
{
    ensureCells();
    auto &screen = activeScreen();

    int first = 0;
    int last = m_cols - 1;

    if (mode == 0)
        first = m_cx;
    else if (mode == 1)
        last = m_cx;
    else if (mode != 2)
        return;

    for (int c = first; c <= last; ++c)
        screen[m_cy][c] = blankCell();

    m_wrapPending = false;
}

void TerminalWidget::eraseChars(int count)
{
    count = qMax(1, count);
    ensureCells();
    auto &row = activeScreen()[m_cy];
    const int end = qMin(m_cols, m_cx + count);
    for (int c = m_cx; c < end; ++c)
        row[c] = blankCell();
}

void TerminalWidget::insertChars(int count)
{
    count = qMax(1, count);
    ensureCells();
    auto &row = activeScreen()[m_cy];
    count = qMin(count, m_cols - m_cx);

    for (int c = m_cols - 1; c >= m_cx + count; --c)
        row[c] = row[c - count];
    for (int c = m_cx; c < m_cx + count; ++c)
        row[c] = blankCell();
}

void TerminalWidget::deleteChars(int count)
{
    count = qMax(1, count);
    ensureCells();
    auto &row = activeScreen()[m_cy];
    count = qMin(count, m_cols - m_cx);

    for (int c = m_cx; c < m_cols - count; ++c)
        row[c] = row[c + count];
    for (int c = m_cols - count; c < m_cols; ++c)
        row[c] = blankCell();
}

void TerminalWidget::insertLines(int count)
{
    count = qMax(1, count);
    auto &screen = activeScreen();
    count = qMin(count, m_rows - m_cy);

    while (count-- > 0) {
        screen.insert(m_cy, QVector<TermCell>(m_cols));
        for (TermCell &cell : screen[m_cy])
            cell = blankCell();
        screen.removeLast();
    }
}

void TerminalWidget::deleteLines(int count)
{
    count = qMax(1, count);
    auto &screen = activeScreen();
    count = qMin(count, m_rows - m_cy);

    while (count-- > 0) {
        screen.remove(m_cy);
        screen.append(QVector<TermCell>(m_cols));
        for (TermCell &cell : screen.back())
            cell = blankCell();
    }
}

void TerminalWidget::resetScreenBuffer(QVector<QVector<TermCell>> &screen)
{
    screen.clear();
    screen.resize(m_rows);
    for (auto &row : screen) {
        row.resize(m_cols);
        for (TermCell &cell : row)
            cell = blankCell();
    }
}

void TerminalWidget::setAlternateScreen(bool enabled)
{
    if (enabled == m_altScreenActive)
        return;

    if (enabled) {
        m_altSavedCx = m_cx;
        m_altSavedCy = m_cy;
        m_altScreen.clear();
        resetScreenBuffer(m_altScreen);
        m_altScreenActive = true;
        m_cx = m_cy = 0;
        m_wrapPending = false;
    } else {
        m_altScreenActive = false;
        m_cx = m_altSavedCx;
        m_cy = m_altSavedCy;
        m_wrapPending = false;
        ensureCells();
    }
    update();
}

void TerminalWidget::resetTerminalModes()
{
    m_cursorOn = true;
    m_appCursorKeys = false;
    m_wrapPending = false;
}

void TerminalWidget::feed(const QByteArray &data)
{
    for (int i = 0; i < data.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(data.at(i));

        switch (m_state) {
        case NORM:
            if (c == 0x1b) {
                flushText();
                m_state = ESC;
            } else if (c < 0x20 || c == 0x7f) {
                flushText();
                handleControl(c);
            } else {
                m_text.append(static_cast<char>(c));
            }
            break;

        case ESC:
            flushText();
            if (c == '[') {
                m_csi.clear();
                m_state = CSI;
            } else if (c == ']') {
                m_osc.clear();
                m_state = OSC;
            } else if (c == '7') {
                saveCursor();
                m_state = NORM;
            } else if (c == '8') {
                restoreCursor();
                m_state = NORM;
            } else if (c == 'D') {
                newline();
                m_state = NORM;
            } else if (c == 'M') {
                reverseIndex();
                m_state = NORM;
            } else if (c == 'E') {
                newline();
                m_state = NORM;
            } else if (c == 'c') {
                resetTerminal();
                m_state = NORM;
            } else {
                // Charset selection and other single-byte ESC commands are
                // harmless to ignore while returning to normal state.
                m_state = NORM;
            }
            break;

        case CSI:
            if ((c >= 0x40 && c <= 0x7e)) {
                m_csi.append(static_cast<char>(c));
                dispatchCSI();
                m_csi.clear();
                m_state = NORM;
            } else if (c >= 0x20 && c <= 0x3f) {
                m_csi.append(static_cast<char>(c));
                if (m_csi.size() > 128) {
                    m_csi.clear();
                    m_state = NORM;
                }
            } else {
                m_csi.clear();
                m_state = NORM;
            }
            break;

        case OSC:
            if (c == 0x07 || c == 0x9c) {
                handleOSC(m_osc);
                m_osc.clear();
                m_state = NORM;
            } else if (c == 0x1b) {
                m_state = OSC_ESC;
            } else {
                if (m_osc.size() < 4096)
                    m_osc.append(static_cast<char>(c));
            }
            break;

        case OSC_ESC:
            if (c == '\\') {
                handleOSC(m_osc);
                m_osc.clear();
                m_state = NORM;
            } else {
                // It wasn't ST; keep the byte as OSC payload.
                m_osc.append(static_cast<char>(0x1b));
                if (m_osc.size() < 4096)
                    m_osc.append(static_cast<char>(c));
                m_state = OSC;
            }
            break;
        }
    }

    if (m_state == NORM)
        flushText();

    update();
}

void TerminalWidget::flushText()
{
    if (m_text.isEmpty())
        return;

    const QString text = QString::fromUtf8(m_text.constData(), m_text.size());
    m_text.clear();

    for (const QChar ch : text)
        writeChar(ch);
}

void TerminalWidget::handleControl(unsigned char c)
{
    switch (c) {
    case '\r':
        writeChar(QLatin1Char('\r'));
        break;
    case '\n':
        newline();
        break;
    case '\b':
        writeChar(QLatin1Char('\b'));
        break;
    case '\t':
        writeChar(QLatin1Char('\t'));
        break;
    case '\a':
        QApplication::beep();
        break;
    case 0x0e:
    case 0x0f:
        // Shift Out / Shift In: not needed for UTF-8 terminal output.
        break;
    default:
        break;
    }
}

void TerminalWidget::dispatchCSI()
{
    if (m_csi.isEmpty())
        return;

    const char final = m_csi.at(m_csi.size() - 1);
    QByteArray body = m_csi.left(m_csi.size() - 1);

    bool privateMode = false;
    char prefix = 0;
    if (!body.isEmpty() &&
        (body.at(0) == '?' || body.at(0) == '>' || body.at(0) == '!')) {
        privateMode = true;
        prefix = body.at(0);
        body.remove(0, 1);
    }

    // Ignore intermediate bytes for the practical subset implemented here.
    while (!body.isEmpty() &&
           (static_cast<unsigned char>(body.at(0)) >= 0x20 &&
            static_cast<unsigned char>(body.at(0)) <= 0x2f))
        body.remove(0, 1);

    QList<int> params;
    if (!body.isEmpty()) {
        const QList<QByteArray> parts = body.split(';');
        for (const QByteArray &part : parts) {
            if (part.isEmpty()) {
                params << 0;
                continue;
            }
            bool ok = false;
            const int value = part.toInt(&ok);
            params << (ok ? value : 0);
        }
    }

    auto param = [&params](int index, int fallback = 0) {
        return (index >= 0 && index < params.size()) ? params[index] : fallback;
    };

    switch (final) {
    case 'A':
        moveCursor(0, -positiveParam(params, 0));
        break;
    case 'B':
    case 'e':
        moveCursor(0, positiveParam(params, 0));
        break;
    case 'C':
    case 'a':
        moveCursor(positiveParam(params, 0), 0);
        break;
    case 'D':
        moveCursor(-positiveParam(params, 0), 0);
        break;
    case 'E':
        setCursorPos(0, m_cy + positiveParam(params, 0));
        break;
    case 'F':
        setCursorPos(0, m_cy - positiveParam(params, 0));
        break;
    case 'G':
    case '`':
        setCursorPos(positiveParam(params, 0) - 1, m_cy);
        break;
    case 'd':
        setCursorPos(m_cx, positiveParam(params, 0) - 1);
        break;
    case 'H':
    case 'f':
        setCursorPos(positiveParam(params, 1) - 1,
                      positiveParam(params, 0) - 1);
        break;

    case 'J':
        clearScreen(param(0, 0));
        break;
    case 'K':
        clearLine(param(0, 0));
        break;
    case 'P':
        eraseChars(positiveParam(params, 0));
        break;
    case '@':
        insertChars(positiveParam(params, 0));
        break;
    case 'X':
        eraseChars(positiveParam(params, 0));
        break;
    case 'L':
        insertLines(positiveParam(params, 0));
        break;
    case 'M':
        deleteLines(positiveParam(params, 0));
        break;
    case 'S':
        for (int i = 0; i < positiveParam(params, 0); ++i)
            scroll();
        break;
    case 'T':
        for (int i = 0; i < positiveParam(params, 0); ++i)
            reverseIndex();
        break;

    case 'm':
        setSGR(params);
        break;
    case 's':
        saveCursor();
        break;
    case 'u':
        restoreCursor();
        break;

    case 'h':
    case 'l':
        if (privateMode) {
            const bool set = (final == 'h');
            for (const int mode : params) {
                switch (mode) {
                case 1:   m_appCursorKeys = set; break; // DECCKM
                case 25:  m_cursorOn = set; break;      // DECTCEM
                case 2004: m_bracketedPaste = set; break;
                case 47:
                case 1047:
                case 1049:
                    setAlternateScreen(set);
                    break;
                default:
                    break;
                }
            }
        }
        break;

    case 'q':
        // DECSCUSR: accept any cursor style, but keep a block cursor.
        if (privateMode || prefix == 0)
            m_cursorOn = true;
        break;

    case 'c':
        // DA response is intentionally omitted; most shells do not require it.
        break;

    default:
        break;
    }
}

void TerminalWidget::setSGR(const QList<int> &params)
{
    if (params.isEmpty()) {
        m_fg = kDefaultFg;
        m_bg = kDefaultBg;
        m_bold = false;
        return;
    }

    for (int i = 0; i < params.size(); ++i) {
        const int p = params[i];

        if (p == 0) {
            m_fg = kDefaultFg;
            m_bg = kDefaultBg;
            m_bold = false;
        } else if (p == 1) {
            m_bold = true;
        } else if (p == 22) {
            m_bold = false;
        } else if (p == 39) {
            m_fg = kDefaultFg;
        } else if (p == 49) {
            m_bg = kDefaultBg;
        } else if (p >= 30 && p <= 37) {
            m_fg = colorForIndex(p - 30);
        } else if (p >= 90 && p <= 97) {
            m_fg = colorForIndex(p - 90 + 8);
        } else if (p >= 40 && p <= 47) {
            m_bg = colorForIndex(p - 40);
        } else if (p >= 100 && p <= 107) {
            m_bg = colorForIndex(p - 100 + 8);
        } else if ((p == 38 || p == 48) && i + 1 < params.size()) {
            const int mode = params[++i];

            if (mode == 5 && i + 1 < params.size()) {
                const QColor color = colorForIndex(params[++i]);
                if (p == 38) m_fg = color;
                else m_bg = color;
            } else if (mode == 2 && i + 3 < params.size()) {
                const int r = qBound(0, params[++i], 255);
                const int g = qBound(0, params[++i], 255);
                const int b = qBound(0, params[++i], 255);
                const QColor color(r, g, b);
                if (p == 38) m_fg = color;
                else m_bg = color;
            }
        }
    }
}

QColor TerminalWidget::colorForIndex(int idx) const
{
    static const QColor ansi[16] = {
        QColor("#1e1e2e"), QColor("#f38ba8"), QColor("#a6e3a1"),
        QColor("#f9e2af"), QColor("#89b4fa"), QColor("#f5c2e7"),
        QColor("#94e2d5"), QColor("#cdd6f4"), QColor("#585b70"),
        QColor("#f38ba8"), QColor("#a6e3a1"), QColor("#f9e2af"),
        QColor("#89b4fa"), QColor("#f5c2e7"), QColor("#94e2d5"),
        QColor("#ffffff")
    };

    if (idx >= 0 && idx < 16)
        return ansi[idx];

    if (idx >= 16 && idx <= 231) {
        const int v = idx - 16;
        const int r = v / 36;
        const int g = (v % 36) / 6;
        const int b = v % 6;

        const int rr = (r == 0) ? 0 : 55 + r * 40;
        const int gg = (g == 0) ? 0 : 55 + g * 40;
        const int bb = (b == 0) ? 0 : 55 + b * 40;
        return QColor(rr, gg, bb);
    }

    if (idx >= 232 && idx <= 255) {
        const int v = 8 + (idx - 232) * 10;
        return QColor(v, v, v);
    }

    return kDefaultFg;
}

void TerminalWidget::handleOSC(const QByteArray &data)
{
    const int separator = data.indexOf(';');
    if (separator < 0)
        return;

    bool ok = false;
    const int command = data.left(separator).toInt(&ok);
    if (!ok)
        return;

    const QString value =
        QString::fromUtf8(data.constData() + separator + 1,
                          data.size() - separator - 1);

    if (command == 0 || command == 2) {
        setWindowTitle(value);
    }
}

void TerminalWidget::writeToPty(const QByteArray &data)
{
    if (m_master < 0 || data.isEmpty())
        return;

    m_writeBuf.append(data);
    m_writePending = true;
    flushWrite();
}

void TerminalWidget::flushWrite()
{
    if (m_master < 0 || m_writeBuf.isEmpty()) {
        m_writePending = false;
        return;
    }

    while (!m_writeBuf.isEmpty()) {
        const ssize_t n =
            ::write(m_master, m_writeBuf.constData(),
                    static_cast<size_t>(m_writeBuf.size()));

        if (n > 0) {
            m_writeBuf.remove(0, static_cast<int>(n));
            continue;
        }

        if (n < 0 && (errno == EINTR))
            continue;

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;

        m_writeBuf.clear();
        break;
    }

    m_writePending = !m_writeBuf.isEmpty();
}

void TerminalWidget::handlePtyOutput()
{
    if (m_master < 0)
        return;

    char buffer[16384];

    for (;;) {
        const ssize_t n = ::read(m_master, buffer, sizeof(buffer));

        if (n > 0) {
            feed(QByteArray(buffer, static_cast<int>(n)));
            continue;
        }

        if (n == 0) {
            shutdown();
            return;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        shutdown();
        return;
    }
}

void TerminalWidget::pollPty()
{
    if (m_master < 0)
        return;

    if (m_writePending)
        flushWrite();

    struct pollfd fd {};
    fd.fd = m_master;
    fd.events = POLLIN | POLLPRI | POLLHUP | POLLERR;

    const int rc = ::poll(&fd, 1, 0);
    if (rc > 0) {
        if (fd.revents & (POLLIN | POLLPRI | POLLHUP | POLLERR))
            handlePtyOutput();
    }
}


void TerminalWidget::setupScrollingFrame()
{
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setObjectName(QStringLiteral("terminalScrollBar"));
    m_scrollBar->setSingleStep(1);
    m_scrollBar->setMinimum(0);
    m_scrollBar->setMaximum(0);
    m_scrollBar->hide();

    connect(m_scrollBar, &QScrollBar::valueChanged,
            this, &TerminalWidget::handleScrollValueChanged);
}

int TerminalWidget::scrollbackLines() const
{
    return m_scrollback.size();
}

void TerminalWidget::updateScrollBar()
{
    if (!m_scrollBar)
        return;

    const int maxOffset = qMax(0, scrollbackLines());
    m_scrollOffset = qBound(0, m_scrollOffset, maxOffset);

    m_updatingScrollBar = true;
    m_scrollBar->setPageStep(qMax(1, m_rows));
    m_scrollBar->setRange(0, maxOffset);
    m_scrollBar->setValue(m_scrollOffset);
    m_scrollBar->setVisible(maxOffset > 0 && !m_altScreenActive);
    m_updatingScrollBar = false;
}

void TerminalWidget::scrollToBottom()
{
    m_scrollOffset = 0;
    updateScrollBar();
    update();
}

void TerminalWidget::scrollBy(int lines)
{
    if (m_altScreenActive || lines == 0)
        return;

    const int maxOffset = qMax(0, scrollbackLines());
    m_scrollOffset = qBound(0, m_scrollOffset + lines, maxOffset);
    updateScrollBar();
    update();
}

void TerminalWidget::handleScrollValueChanged(int value)
{
    if (m_updatingScrollBar)
        return;

    m_scrollOffset = qMax(0, value);
    update();
}

const QVector<TerminalWidget::TermCell> &TerminalWidget::displayRow(int row) const
{
    static const QVector<TermCell> emptyRow;

    if (row < 0 || row >= m_rows)
        return emptyRow;

    const auto &screen = activeScreen();
    const int total = m_scrollback.size() + screen.size();
    const int first = qMax(0, total - m_rows - m_scrollOffset);
    const int logical = first + row;

    if (logical < 0 || logical >= total)
        return emptyRow;

    if (logical < m_scrollback.size())
        return m_scrollback[logical];

    const int screenRow = logical - m_scrollback.size();
    if (screenRow >= 0 && screenRow < screen.size())
        return screen[screenRow];

    return emptyRow;
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta != 0 && !m_altScreenActive) {
        const int steps = qMax(1, qAbs(delta) / 120);
        scrollBy(delta > 0 ? steps : -steps);
        event->accept();
        return;
    }

    QWidget::wheelEvent(event);
}

bool TerminalWidget::runScrollbackSmokeTest()
{
    if (!m_scrollBar)
        return false;

    m_scrollback.clear();
    m_scrollOffset = 0;
    updateScrollBar();

    if (m_scrollBar->maximum() != 0)
        return false;

    // Force enough history to make the ScrollingFrame meaningful.
    for (int i = 0; i < 32; ++i) {
        QVector<TermCell> row(m_cols);
        for (TermCell &cell : row)
            cell = blankCell();

        if (!row.isEmpty())
            row[0].ch = QChar::fromLatin1('A' + (i % 26));

        m_scrollback.append(row);
    }

    updateScrollBar();

    if (m_scrollBar->maximum() != m_scrollback.size())
        return false;

    scrollBy(5);
    if (m_scrollOffset != 5)
        return false;

    scrollToBottom();
    if (m_scrollOffset != 0)
        return false;

    m_scrollback.clear();
    updateScrollBar();

    return m_scrollBar->maximum() == 0 &&
           m_scrollOffset == 0;
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), m_bg);

    const int cw = qMax(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
    const int ch = qMax(1, fontMetrics().height());
    const int maxRows = qMin(m_rows, height() / ch + 1);
    const int maxCols = qMin(m_cols, width() / cw + 1);

    for (int r = 0; r < maxRows; ++r) {
        const auto &row = displayRow(r);

        for (int c = 0; c < maxCols && c < row.size(); ++c) {
            const TermCell &cell = row[c];
            const int x = c * cw;
            const int y = r * ch;

            if (cell.bg.isValid() && cell.bg != m_bg)
                painter.fillRect(x, y, cw, ch, cell.bg);

            if (cell.ch == QLatin1Char(' '))
                continue;

            painter.setFont(cell.bold ? boldFont() : font());
            painter.setPen(cell.fg.isValid() ? cell.fg : m_fg);
            painter.drawText(QRect(x, y, cw, ch),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QString(cell.ch));
        }
    }

    if (m_cursorOn && hasFocus() && m_scrollOffset == 0 &&
        m_cy >= 0 && m_cy < maxRows &&
        m_cx >= 0 && m_cx < maxCols) {

        const int x = m_cx * cw;
        const int y = m_cy * ch;
        const auto &screen = activeScreen();
        if (m_cy >= screen.size() || m_cx >= screen[m_cy].size())
            return;
        const TermCell &cell = screen[m_cy][m_cx];

        painter.fillRect(x, y, cw, ch, m_fg);

        QChar chv = cell.ch;
        if (chv == QLatin1Char('\0'))
            chv = QLatin1Char(' ');

        painter.setFont(cell.bold ? boldFont() : font());
        painter.setPen(m_bg);
        painter.drawText(QRect(x, y, cw, ch),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString(chv));
    }
}

QFont TerminalWidget::boldFont() const
{
    QFont result = font();
    result.setBold(true);
    return result;
}

void TerminalWidget::setPtySize()
{
    if (m_master < 0)
        return;

    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(qBound(1, m_rows, 65535));
    ws.ws_col = static_cast<unsigned short>(qBound(1, m_cols, 65535));

    (void)::ioctl(m_master, TIOCSWINSZ, &ws);
    if (m_pid > 0)
        (void)::kill(-m_pid, SIGWINCH);
}

void TerminalWidget::resizeEvent(QResizeEvent *)
{
    const int cw = qMax(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
    const int ch = qMax(1, fontMetrics().height());

    const int newCols = qMax(2, width() / cw);
    const int newRows = qMax(2, height() / ch);

    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;
        ensureCells();

        m_cx = qBound(0, m_cx, m_cols - 1);
        m_cy = qBound(0, m_cy, m_rows - 1);

        setPtySize();
    }

    if (m_scrollBar) {
        const int barWidth = qMax(12, m_scrollBar->sizeHint().width());
        m_scrollBar->setGeometry(qMax(0, width() - barWidth), 0,
                                 barWidth, height());
        m_scrollBar->raise();
        updateScrollBar();
    }

    update();
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) {
        copySelectionOrLine();
        return;
    }

    if (event->matches(QKeySequence::Paste)) {
        pasteClipboard();
        return;
    }

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();

    if (!m_altScreenActive && mods.testFlag(Qt::ShiftModifier) &&
        (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        const int amount = qMax(1, m_rows - 1);
        scrollBy(key == Qt::Key_PageUp ? amount : -amount);
        return;
    }

    if (!m_altScreenActive && mods.testFlag(Qt::ShiftModifier) &&
        (key == Qt::Key_Home || key == Qt::Key_End)) {
        if (key == Qt::Key_Home) {
            m_scrollOffset = scrollbackLines();
            updateScrollBar();
            update();
        } else {
            scrollToBottom();
        }
        return;
    }

    QByteArray out;
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool alt = mods.testFlag(Qt::AltModifier);

    if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z) {
        out.append(static_cast<char>(key - Qt::Key_A + 1));
    } else if (ctrl && key == Qt::Key_Space) {
        out.append('\0');
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        out.append('\r');
    } else if (key == Qt::Key_Backspace) {
        out.append('\x7f');
    } else if (key == Qt::Key_Tab) {
        out.append('\t');
    } else if (key == Qt::Key_Escape) {
        out.append('\x1b');
    } else if (key == Qt::Key_Up) {
        out.append(m_appCursorKeys ? "\x1bOA" : "\x1b[A");
    } else if (key == Qt::Key_Down) {
        out.append(m_appCursorKeys ? "\x1bOB" : "\x1b[B");
    } else if (key == Qt::Key_Right) {
        out.append(m_appCursorKeys ? "\x1bOC" : "\x1b[C");
    } else if (key == Qt::Key_Left) {
        out.append(m_appCursorKeys ? "\x1bOD" : "\x1b[D");
    } else if (key == Qt::Key_Home) {
        out.append("\x1b[H");
    } else if (key == Qt::Key_End) {
        out.append("\x1b[F");
    } else if (key == Qt::Key_PageUp) {
        out.append("\x1b[5~");
    } else if (key == Qt::Key_PageDown) {
        out.append("\x1b[6~");
    } else if (key == Qt::Key_Delete) {
        out.append("\x1b[3~");
    } else if (key == Qt::Key_Insert) {
        out.append("\x1b[2~");
    } else if (key == Qt::Key_F1) {
        out.append("\x1bOP");
    } else if (key == Qt::Key_F2) {
        out.append("\x1bOQ");
    } else if (key == Qt::Key_F3) {
        out.append("\x1bOR");
    } else if (key == Qt::Key_F4) {
        out.append("\x1bOS");
    } else {
        QString text = event->text();
        if (!text.isEmpty()) {
            if (alt)
                out.append('\x1b');
            out.append(text.toUtf8());
        }
    }

    if (!out.isEmpty())
        writeToPty(out);

    event->accept();
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
    update();
}

void TerminalWidget::closeEvent(QCloseEvent *event)
{
    shutdown();
    event->accept();
}

QString TerminalWidget::lineText(int row) const
{
    if (row < 0 || row >= activeScreen().size())
        return {};

    QString result;
    const auto &line = activeScreen()[row];
    for (const TermCell &cell : line)
        result.append(cell.ch);
    return result.trimmed();
}

void TerminalWidget::copySelectionOrLine()
{
    // Selection support can be added later without changing PTY behavior.
    // For now copy the current terminal line exactly as displayed.
    const QString text = lineText(m_cy);
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

void TerminalWidget::pasteClipboard()
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty())
        return;

    // Bracketed paste is used by many modern shells/editors.
    if (m_bracketedPaste)
        text = QStringLiteral("\x1b[200~") + text +
               QStringLiteral("\x1b[201~");

    writeToPty(text.toUtf8());
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NexTerminal"));
    app.setApplicationDisplayName(QStringLiteral("NexTerminal"));

    TerminalWidget terminal;
    terminal.resize(900, 600);
    terminal.setWindowTitle(QStringLiteral("NexTerminal"));

    // Mandatory startup smoke test: fail fast if the scrolling frame is broken.
    if (!terminal.runScrollbackSmokeTest()) {
        QTextStream(stderr)
            << "nex-terminal: ScrollingFrame smoke test FAILED\\n";
        return EXIT_FAILURE;
    }

    if (!terminal.startShell()) {
        QTextStream(stderr)
            << "nex-terminal: unable to start a shell\n";
        return EXIT_FAILURE;
    }

    terminal.show();
    terminal.setFocus(Qt::OtherFocusReason);

    const int rc = app.exec();
    terminal.shutdown();
    return rc;
}
