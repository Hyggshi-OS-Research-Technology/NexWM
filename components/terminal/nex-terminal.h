/*
 * nex-terminal.h - NexTerminal
 * Qt6 terminal emulator for the Nex Desktop Environment.
 *
 * Renders a terminal screen on QWidget and communicates with an interactive
 * shell through a POSIX pseudo-terminal (PTY).
 */

#ifndef NEX_TERMINAL_H
#define NEX_TERMINAL_H

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QList>
#include <QVector>
#include <QWidget>
#include <QPoint>

#include <sys/types.h>

class QCloseEvent;
class QFocusEvent;
class QKeyEvent;
class QPaintEvent;
class QResizeEvent;
class QScrollBar;
class QTimer;
class QWheelEvent;

class TerminalWidget final : public QWidget {
public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    bool startShell();
    void shutdown();

    int rows() const { return m_rows; }
    int cols() const { return m_cols; }

    // Mandatory startup smoke test for the scrolling frame.
    bool runScrollbackSmokeTest();

    // Mandatory startup smoke test for the scrolling frame.

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct TermCell {
        QChar ch = QLatin1Char(' ');
        QColor fg;
        QColor bg;
        bool bold = false;
    };

    enum ParseState {
        NORM = 0,
        ESC,
        CSI,
        OSC,
        OSC_ESC
    };

    // Screen / cursor --------------------------------------------------------
    void resetTerminal();
    void resetTerminalModes();
    void ensureCells();

    QVector<QVector<TermCell>> &activeScreen();
    const QVector<QVector<TermCell>> &activeScreen() const;
    TermCell blankCell() const;
    void resetScreenBuffer(QVector<QVector<TermCell>> &screen);

    void newline();
    void reverseIndex();
    void scroll();
    void writeChar(QChar ch);
    void moveCursor(int dx, int dy);
    void setCursorPos(int col, int row);
    void saveCursor();
    void restoreCursor();

    void clearScreen(int mode = 2);
    void clearLine(int mode = 2);
    void eraseChars(int count);
    void insertChars(int count);
    void deleteChars(int count);
    void insertLines(int count);
    void deleteLines(int count);

    void setAlternateScreen(bool enabled);

    // ANSI / VT parser -------------------------------------------------------
    void feed(const QByteArray &data);
    void flushText();
    void handleControl(unsigned char c);
    void dispatchCSI();
    void setSGR(const QList<int> &params);
    void handleOSC(const QByteArray &data);
    QColor colorForIndex(int idx) const;

    // PTY --------------------------------------------------------------------
    void writeToPty(const QByteArray &data);
    void flushWrite();
    void pollPty();
    void handlePtyOutput();
    void setPtySize();

    // Rendering / clipboard --------------------------------------------------
    QFont boldFont() const;
    QString lineText(int row) const;
    void copySelectionOrLine();
    void pasteClipboard();

    // Scrollback / scrolling frame
    void setupScrollingFrame();
    void updateScrollBar();
    void scrollToBottom();
    void scrollBy(int lines);
    void handleScrollValueChanged(int value);
    int scrollbackLines() const;
    const QVector<TermCell> &displayRow(int row) const;

    // Screen state
    QVector<QVector<TermCell>> m_screen;
    QVector<QVector<TermCell>> m_altScreen;

    // Scrollback backing store for the terminal scrolling frame.
    QVector<QVector<TermCell>> m_scrollback;
    int m_scrollbackLimit = 10000;
    int m_scrollOffset = 0;
    QScrollBar *m_scrollBar = nullptr;
    bool m_updatingScrollBar = false;

    int m_rows = 24;
    int m_cols = 80;
    bool m_altScreenActive = false;

    // Cursor
    int m_cx = 0;
    int m_cy = 0;
    int m_savedCx = 0;
    int m_savedCy = 0;
    int m_altSavedCx = 0;
    int m_altSavedCy = 0;

    bool m_cursorOn = true;
    bool m_appCursorKeys = false;
    bool m_wrapPending = false;
    bool m_bracketedPaste = false;

    // Current attributes
    QColor m_fg = QColor("#cdd6f4");
    QColor m_bg = QColor("#1e1e2e");
    bool m_bold = false;

    // ANSI parser
    ParseState m_state = NORM;
    QByteArray m_text;
    QByteArray m_csi;
    QByteArray m_osc;

    // PTY / process
    pid_t m_pid = -1;
    int m_master = -1;
    int m_slave = -1;
    QTimer *m_pollTimer = nullptr;

    bool m_writePending = false;
    QByteArray m_writeBuf;
};

#endif // NEX_TERMINAL_H
