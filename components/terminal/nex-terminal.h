/*
 * nex-terminal.h - NexTerminal
 * Qt6 terminal emulator for the Nex Desktop Environment.
 *
 * Renders the terminal screen on a QWidget and talks to a shell over a
 * pseudo-terminal (pty) so interactive programs and full-screen TUI apps
 * behave correctly. Supports a practical subset of ANSI escape sequences
 * (colors, cursor movement, clear, SGR bold) plus copy/paste shortcuts.
 */

#ifndef NEX_TERMINAL_H
#define NEX_TERMINAL_H

#include <QWidget>
#include <QVector>
#include <QColor>
#include <QByteArray>
#include <QFont>
#include <sys/types.h>

class TerminalWidget : public QWidget {
public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    /* Spawn the configured shell on the pty. Returns false on failure. */
    bool startShell();
    /* Close the shell and clean up. */
    void shutdown();

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    struct TermCell {
        QChar  ch = QLatin1Char(' ');
        QColor fg;
        QColor bg;
        bool   bold = false;
    };

    enum ParseState { NORM = 0, ESC, CSI, OSC };

    /* ── screen helpers ─────────────────────────────────────────────────── */
    void ensureCells();
    void newline();
    void scroll();
    void writeChar(QChar ch);
    void moveCursor(int dx, int dy);
    void setCursorPos(int col, int row);
    void clearScreen();
    void clearLine();

    /* ── ANSI parsing ───────────────────────────────────────────────────── */
    void feed(const QByteArray &data);
    void flushText();
    void handleControl(unsigned char c);
    void dispatchCSI();

    /* ── pty I/O ────────────────────────────────────────────────────────── */
    void writeToPty(const QByteArray &data);
    void flushWrite();
    void pollPty();
    void handlePtyOutput();

    /* ── palette ────────────────────────────────────────────────────────── */
    void setSGR(const QList<int> &params);
    QColor colorForIndex(int idx) const;
    QFont boldFont() const;

    /* ── clipboard helpers ──────────────────────────────────────────────── */
    void copyLastLine();
    void pasteClipboard();

    /* Screen state */
    QVector<QVector<TermCell>> m_screen;
    int m_rows = 24;
    int m_cols = 80;

    /* Cursor */
    int m_cx = 0;
    int m_cy = 0;
    bool m_cursorOn = true;

    /* Current attributes */
    QColor m_fg = QColor("#cdd6f4");
    QColor m_bg = QColor("#1e1e2e");
    bool m_bold = false;

    /* ANSI parser state */
    ParseState m_state = NORM;
    QByteArray m_text;      /* pending printable (UTF-8) run */
    QByteArray m_csi;       /* pending CSI parameter + final byte */
    QByteArray m_osc;

    /* pty + process */
    pid_t m_pid = -1;
    int m_master = -1;
    int m_slave  = -1;
    class QTimer *m_pollTimer = nullptr;
    bool m_writePending = false;
    QByteArray m_writeBuf;
};

#endif // NEX_TERMINAL_H
