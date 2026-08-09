/*
 * nex-terminal.cpp - NexTerminal
 * Qt6 terminal emulator spawning a shell over a pty.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nex-terminal.h"

#include <QApplication>
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QTimer>
#include <QClipboard>
#include <QTextStream>
#include <QCloseEvent>

extern "C" {
#include <pty.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
}
#include <cstdio>
#include <cstring>
#include <cstdlib>

/* ─── construction / destruction ──────────────────────────────────────────── */

TerminalWidget::TerminalWidget(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::IBeamCursor);

    QFont f;
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setPointSize(11);
    setFont(f);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(8);
    connect(m_pollTimer, &QTimer::timeout, this, &TerminalWidget::pollPty);
}

TerminalWidget::~TerminalWidget()
{
    shutdown();
}

/* ─── pty + shell lifecycle ───────────────────────────────────────────────── */

bool TerminalWidget::startShell()
{
    if (openpty(&m_master, &m_slave, nullptr, nullptr, nullptr) != 0) {
        fprintf(stderr, "nex-terminal: openpty failed\n");
        return false;
    }
    fcntl(m_master, F_SETFL, O_NONBLOCK);
    /* Ensure the master survives exec in the child (clear close-on-exec). */
    int fdflags = fcntl(m_master, F_GETFD, 0);
    if (fdflags != -1) fcntl(m_master, F_SETFD, fdflags & ~FD_CLOEXEC);

    QString shell = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (shell.isEmpty()) shell = QStringLiteral("/bin/sh");
    QByteArray shellUtf8 = shell.toLocal8Bit();

    m_pid = fork();
    if (m_pid < 0) {
        fprintf(stderr, "nex-terminal: fork failed\n");
        return false;
    }

    if (m_pid == 0) {
        /* ── child: turn the pty slave into a controlling terminal ── */
        if (setsid() < 0) { /* ignore */ }
        dup2(m_slave, 0);
        dup2(m_slave, 1);
        dup2(m_slave, 2);
        if (m_slave > 2) ::close(m_slave);
        ::close(m_master);
#ifdef TIOCSCTTY
        ioctl(0, TIOCSCTTY, 0);
#endif
        struct termios tio;
        if (tcgetattr(0, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cc[VMIN]  = 1;
            tio.c_cc[VTIME] = 0;
            tcsetattr(0, TCSANOW, &tio);
        }
        if (setpgid(0, 0) < 0) { /* running in own process group */ }

        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("SHELL", shellUtf8.constData(), 1);

        const char *home = getenv("HOME");
        if (home && *home) chdir(home);

        execlp(shellUtf8.constData(), shellUtf8.constData(), (char *)NULL);
        _exit(127);
    }

    /* ── parent ── */
    /* Put the child in its own process group so we can signal it as a unit. */
    if (setpgid(m_pid, m_pid) != 0) { /* child already did it; harmless */ }
    ::close(m_slave);
    m_slave = -1;

    /* Initial pty size. */
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = (unsigned short)m_rows;
    ws.ws_col = (unsigned short)m_cols;
    ioctl(m_master, TIOCSWINSZ, &ws);

    m_pollTimer->start();

    return true;
}

void TerminalWidget::shutdown()
{
    if (m_pollTimer) m_pollTimer->stop();
    if (m_pid > 0) {
        /* Terminate the shell's process group, then reap it. */
        ::kill(-m_pid, SIGHUP);
        ::kill(m_pid,  SIGHUP); /* in case the group id differs */
        int st = 0;
        if (waitpid(m_pid, &st, WNOHANG) == 0) {
            ::kill(m_pid, SIGTERM);
            waitpid(m_pid, &st, 0);
        }
        m_pid = -1;
    }
    if (m_master >= 0) { ::close(m_master); m_master = -1; }
    if (m_slave  >= 0) { ::close(m_slave);  m_slave  = -1; }
}

/* ─── screen model ────────────────────────────────────────────────────────── */

void TerminalWidget::ensureCells()
{
    if (m_screen.size() != m_rows) m_screen.resize(m_rows);
    for (int r = 0; r < m_rows; ++r) {
        if (m_screen[r].size() != m_cols) m_screen[r].resize(m_cols);
    }
}

void TerminalWidget::newline()
{
    m_cx = 0;
    m_cy++;
    if (m_cy >= m_rows) {
        m_cy = m_rows - 1;
        scroll();
    }
}

void TerminalWidget::scroll()
{
    if (m_screen.isEmpty()) return;
    m_screen.removeFirst();
    m_screen.append(QVector<TermCell>(m_cols));
    update();
}

void TerminalWidget::writeChar(QChar ch)
{
    if (ch == QLatin1Char('\r')) { m_cx = 0; return; }
    if (ch == QLatin1Char('\n')) { newline(); return; }
    if (ch == QLatin1Char('\b')) { if (m_cx > 0) m_cx--; return; }
    if (ch == QLatin1Char('\t')) {
        int nextTab = ((m_cx / 8) + 1) * 8;
        m_cx = (nextTab < m_cols) ? nextTab : (m_cols - 1);
        return;
    }
    if (ch.unicode() < 0x20) return; /* ignore other control chars */

    ensureCells();
    TermCell &cell = m_screen[m_cy][m_cx];
    cell.ch   = ch;
    cell.fg   = m_fg;
    cell.bg   = m_bg;
    cell.bold = m_bold;

    m_cx++;
    if (m_cx >= m_cols) newline();
}

void TerminalWidget::moveCursor(int dx, int dy)
{
    m_cx += dx;
    m_cy += dy;
    if (m_cx < 0) m_cx = 0;
    if (m_cx >= m_cols) m_cx = m_cols - 1;
    if (m_cy < 0) m_cy = 0;
    if (m_cy >= m_rows) m_cy = m_rows - 1;
}

void TerminalWidget::setCursorPos(int col, int row)
{
    m_cx = (col >= 0 && col < m_cols) ? col : m_cols - 1;
    m_cy = (row >= 0 && row < m_rows) ? row : m_rows - 1;
}

void TerminalWidget::clearScreen()
{
    ensureCells();
    for (int r = 0; r < m_rows; ++r)
        for (int c = 0; c < m_cols; ++c)
            m_screen[r][c] = TermCell();
}

void TerminalWidget::clearLine()
{
    ensureCells();
    for (int c = 0; c < m_cols; ++c)
        m_screen[m_cy][c] = TermCell();
}

/* ─── ANSI escape parser ──────────────────────────────────────────────────── */

void TerminalWidget::feed(const QByteArray &data)
{
    for (int i = 0; i < data.size(); ++i) {
        unsigned char c = (unsigned char)data[i];
        switch (m_state) {
        case NORM:
            if (c == 0x1b) { flushText(); m_state = ESC; }
            else if (c < 0x20 || c == 0x7f) { flushText(); handleControl(c); }
            else m_text.append((char)c);
            break;
        case ESC:
            flushText();
            if (c == '[') { m_state = CSI; m_csi.clear(); }
            else if (c == ']') { m_state = OSC; m_osc.clear(); }
            else m_state = NORM;
            break;
        case CSI:
            if (c >= 0x40 && c <= 0x7e) { m_csi.append((char)c); dispatchCSI(); m_state = NORM; }
            else m_csi.append((char)c);
            break;
        case OSC:
            if (c == 0x07 || c == 0x9c) { m_state = NORM; }
            else if (c == 0x1b) { m_state = NORM; }
            break;
        }
    }
}

void TerminalWidget::flushText()
{
    if (m_text.isEmpty()) return;
    QString s = QString::fromUtf8(m_text);
    m_text.clear();
    for (const QChar &ch : s) writeChar(ch);
}

void TerminalWidget::handleControl(unsigned char c)
{
    switch (c) {
    case '\r': writeChar(QLatin1Char('\r')); break;
    case '\n': newline(); break;
    case '\b': writeChar(QLatin1Char('\b')); break;
    case '\t': writeChar(QLatin1Char('\t')); break;
    case '\a': QApplication::beep(); break;
    default:   break;
    }
    update();
}

void TerminalWidget::dispatchCSI()
{
    QByteArray s = m_csi;
    if (s.isEmpty()) return;

    char final = s.at(s.size() - 1);
    QByteArray body = s.left(s.size() - 1);

    /* Strip private-marker / intermediate lead characters (? > !). */
    if (!body.isEmpty() && (body.at(0) == '?' || body.at(0) == '>' || body.at(0) == '!'))
        body.remove(0, 1);

    QList<int> params;
    for (const QByteArray &part : body.split(';')) {
        if (part.isEmpty()) { params << 1; continue; }
        bool ok = false;
        int v = part.toInt(&ok);
        params << (ok ? v : 0);
    }
    if (params.isEmpty()) params << 0;
    if (params[0] == 0) params[0] = 1;

    int n = params[0];

    switch (final) {
    case 'A': moveCursor(0, -n); break;
    case 'B': moveCursor(0,  n); break;
    case 'C': moveCursor(n, 0);  break;
    case 'D': moveCursor(-n, 0); break;
    case 'G': setCursorPos(params[0] - 1, m_cy); break;
    case 'H':
    case 'f': setCursorPos(params.value(1, 1) - 1, params[0] - 1); break;
    case 'J':
        if (params[0] == 2 || params[0] == 3) { clearScreen(); m_cx = 0; m_cy = 0; }
        break;
    case 'K':
        if (params[0] == 2) clearLine();
        break;
    case 'm': setSGR(params); break;
    case 'h':
    case 'l': /* private modes (cursor hide/show etc.) ignored */ break;
    default:  break;
    }
    update();
}

void TerminalWidget::setSGR(const QList<int> &params)
{
    int i = 0;
    while (i < params.size()) {
        int p = params[i];
        if (p == 0)  { m_fg = QColor("#cdd6f4"); m_bg = QColor("#1e1e2e"); m_bold = false; }
        else if (p == 1)  { m_bold = true; }
        else if (p == 22) { m_bold = false; }
        else if (p >= 30 && p <= 37)  m_fg = colorForIndex(p - 30);
        else if (p >= 90 && p <= 97)  m_fg = colorForIndex(p - 90 + 8);
        else if (p == 39) { m_fg = QColor("#cdd6f4"); }
        else if (p >= 40 && p <= 47)  m_bg = colorForIndex(p - 40);
        else if (p >= 100 && p <= 107) m_bg = colorForIndex(p - 100 + 8);
        else if (p == 49) { m_bg = QColor("#1e1e2e"); }
        else if ((p == 38 || p == 48) && i + 1 < params.size() && params[i + 1] == 5) {
            if (i + 2 < params.size()) {
                QColor col = colorForIndex(params[i + 2]);
                if (p == 38) m_fg = col; else m_bg = col;
                i += 2;
            }
        }
        ++i;
    }
}

QColor TerminalWidget::colorForIndex(int idx) const
{
    static const QColor ansi[16] = {
        QColor("#1e1e2e"), QColor("#e78284"), QColor("#a6e3a1"),
        QColor("#f9e2af"), QColor("#89b4fa"), QColor("#f5c2e7"),
        QColor("#94e2d5"), QColor("#cdd6f4"), QColor("#585b70"),
        QColor("#f38ba8"), QColor("#a6e3a1"), QColor("#f9e2af"),
        QColor("#89b4fa"), QColor("#f5c2e7"), QColor("#94e2d5"), QColor("#ffffff")
    };
    if (idx >= 0 && idx < 16) return ansi[idx];
    if (idx >= 16 && idx <= 231) {
        int v = idx - 16;
        int r = v / 36, g = (v % 36) / 6, b = v % 6;
        int rr = (r == 0) ? 0 : 55 + r * 40;
        int gg = (g == 0) ? 0 : 55 + g * 40;
        int bb = (b == 0) ? 0 : 55 + b * 40;
        return QColor(rr, gg, bb);
    }
    if (idx >= 232 && idx <= 255) {
        int v = 8 + (idx - 232) * 10;
        return QColor(v, v, v);
    }
    return QColor("#cdd6f4");
}

/* ─── pty I/O ─────────────────────────────────────────────────────────────── */

void TerminalWidget::writeToPty(const QByteArray &data)
{
    if (m_master < 0 || data.isEmpty()) return;
    m_writeBuf.append(data);
    m_writePending = true;
    flushWrite();
}

void TerminalWidget::flushWrite()
{
    if (m_master < 0 || m_writeBuf.isEmpty()) { m_writePending = false; return; }
    while (!m_writeBuf.isEmpty()) {
        ssize_t n = ::write(m_master, m_writeBuf.constData(), m_writeBuf.size());
        if (n > 0) {
            m_writeBuf.remove(0, (int)n);
        } else if (n < 0 && errno == EAGAIN) {
            break;
        } else {
            m_writeBuf.clear();
            break;
        }
    }
    m_writePending = !m_writeBuf.isEmpty();
}

void TerminalWidget::handlePtyOutput()
{
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(m_master, buf, sizeof(buf));
        if (n > 0) {
            feed(QByteArray(buf, (int)n));
        } else if (n == 0) {
            /* EOF: the shell closed — tear down cleanly. */
            shutdown();
            return;
        } else {
            break; /* EAGAIN or error */
        }
    }
}

void TerminalWidget::pollPty()
{
    if (m_master < 0) return;
    if (m_writePending) flushWrite();

    struct pollfd fds;
    fds.fd = m_master;
    fds.events = POLLIN;
    fds.revents = 0;
    int rc = poll(&fds, 1, 0);
    if (rc > 0 && (fds.revents & (POLLIN | POLLHUP)))
        handlePtyOutput();
    update();
}

/* ─── events ──────────────────────────────────────────────────────────────── */

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_bg);

    if (m_screen.isEmpty()) return;

    const int cw = qMax(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
    const int ch = qMax(1, fontMetrics().height());
    int maxRows = qMin(m_rows, height() / ch);
    int maxCols = qMin(m_cols, width() / cw);

    for (int r = 0; r < maxRows; ++r) {
        const auto &row = m_screen[r];
        for (int c = 0; c < maxCols; ++c) {
            const TermCell &cell = row[c];
            int x = c * cw;
            int y = r * ch;
            if (cell.bg != m_bg) {
                p.fillRect(x, y, cw, ch, cell.bg);
            }
            QChar gc = cell.ch;
            if (gc.unicode() == 0) continue;
            if (gc.isSpace() && cell.fg == m_bg) continue;
            p.setFont((m_bold || cell.bold) ? boldFont() : font());
            p.setPen(cell.fg);
            p.drawText(QRect(x, y, cw, ch), Qt::AlignLeft | Qt::AlignVCenter, QString(gc));
        }
    }

    /* Cursor block */
    if (m_cursorOn && m_cx < maxCols && m_cy < maxRows) {
        int x = m_cx * cw;
        int y = m_cy * ch;
        p.fillRect(x, y, cw, ch, m_fg);
        QChar gc = m_screen[m_cy][m_cx].ch;
        if (gc.unicode() == 0) gc = QLatin1Char(' ');
        p.setPen(m_bg);
        p.drawText(QRect(x, y, cw, ch), Qt::AlignLeft | Qt::AlignVCenter, QString(gc));
    }
}

QFont TerminalWidget::boldFont() const
{
    QFont bf = font();
    bf.setBold(true);
    return bf;
}

void TerminalWidget::resizeEvent(QResizeEvent *)
{
    const int cw = qMax(1, fontMetrics().horizontalAdvance(QLatin1Char('M')));
    const int ch = qMax(1, fontMetrics().height());
    m_cols = qMax(2, width() / cw);
    m_rows = qMax(2, height() / ch);
    ensureCells();

    if (m_master >= 0) {
        struct winsize ws;
        memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)m_rows;
        ws.ws_col = (unsigned short)m_cols;
        ioctl(m_master, TIOCSWINSZ, &ws);
        if (m_pid > 0)
            ::kill(-m_pid, SIGWINCH);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy)) { copyLastLine(); return; }
    if (event->matches(QKeySequence::Paste)) { pasteClipboard(); return; }

    QByteArray out;
    int key = event->key();
    bool ctrl = event->modifiers() & Qt::ControlModifier;
    bool alt   = event->modifiers() & Qt::AltModifier;

    if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z) {
        out.append(char(key - Qt::Key_A + 1));
    } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        out.append("\r");
    } else if (key == Qt::Key_Backspace) {
        out.append("\x7f");
    } else if (key == Qt::Key_Tab) {
        out.append("\t");
    } else if (key == Qt::Key_Escape) {
        out.append("\x1b");
    } else if (key == Qt::Key_Up) {
        out.append(alt ? "\x1b[1;3A" : "\x1b[A");
    } else if (key == Qt::Key_Down) {
        out.append(alt ? "\x1b[1;3B" : "\x1b[B");
    } else if (key == Qt::Key_Right) {
        out.append(alt ? "\x1b[1;3C" : "\x1b[C");
    } else if (key == Qt::Key_Left) {
        out.append(alt ? "\x1b[1;3D" : "\x1b[D");
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
    } else {
        QString text = event->text();
        if (!text.isEmpty() && !ctrl)
            out.append(text.toUtf8());
    }

    if (!out.isEmpty()) writeToPty(out);
}

void TerminalWidget::closeEvent(QCloseEvent *event)
{
    shutdown();
    event->accept();
}

void TerminalWidget::copyLastLine()
{
    if (m_screen.isEmpty()) return;
    const auto &row = m_screen[m_cy];
    QString line;
    for (const TermCell &cell : row) {
        if (cell.ch.unicode() != 0 && !cell.ch.isSpace())
            line.append(cell.ch);
    }
    line = line.trimmed();
    if (!line.isEmpty())
        QApplication::clipboard()->setText(line);
}

void TerminalWidget::pasteClipboard()
{
    QString text = QApplication::clipboard()->text();
    if (!text.isEmpty()) writeToPty(text.toUtf8());
}

/* ─── application entry point ─────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NexTerminal"));
    app.setApplicationDisplayName(QStringLiteral("Terminal"));

    TerminalWidget term;
    term.resize(900, 600);
    term.setWindowTitle(QStringLiteral("Terminal"));
    term.setStyleSheet(QStringLiteral("background-color: #1e1e2e; color: #cdd6f4;"));

    if (!term.startShell()) {
        QTextStream(stderr) << "nex-terminal: unable to start a shell\n";
        return 1;
    }

    term.show();
    term.setFocus();
    return app.exec();
}
