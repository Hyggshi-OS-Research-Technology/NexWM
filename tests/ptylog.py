#!/usr/bin/env python3
"""tests/ptylog.py — run a program on a pseudo-terminal (a pty) and copy what it writes into a log file.

Why: SDDM's greeter replaces Qt's message handler with its own and only writes the theme's QML output (console.log,
warnings, errors) to stderr when the process is on a terminal. Started with its stderr redirected to a file — which is
what a test does — the greeter sends everything to journald instead (an SDDM built with journald support: Fedora; on a
machine with no journal, such as the CI container, it is simply lost) or to a log file of its own, and the log the test
is reading stays empty. A pty *is* a terminal, so with one the greeter writes to stderr again, and this copies it.

    python3 tests/ptylog.py LOGFILE PROGRAM [ARG...]      exit status = the program's own

The program runs in a session of its own with the pty as its terminal (its own process group, so a terminal signal
would reach it the way it does in a shell). SIGTERM, SIGINT, SIGHUP and SIGQUIT sent to this process are forwarded to
that group — so killing what the test started stops the greeter too, not just the wrapper around it — and a program
still there a second after SIGTERM is killed, which keeps a test run from leaving a greeter behind.
"""
import os
import pty
import select
import signal
import sys
import time

EXIT_CANNOT_RUN = 127
FORWARD = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT)
GRACE = 1.0            # seconds between SIGTERM and SIGKILL for a program that ignores it


def usage():
    sys.stderr.write(__doc__)
    return 2


def main():
    if len(sys.argv) < 3:
        return usage()
    log_path, argv = sys.argv[1], sys.argv[2:]

    master, slave = pty.openpty()
    child = os.fork()
    if child == 0:                                     # the program: the pty becomes its terminal
        os.close(master)
        try:
            os.setsid()
            import fcntl
            import termios
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        except OSError:
            pass                                       # no controlling terminal: still a terminal to write to
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        if slave > 2:
            os.close(slave)
        try:
            os.execvp(argv[0], argv)
        except OSError as e:
            os.write(2, ("ptylog: cannot run %s: %s\n" % (argv[0], e)).encode())
            os._exit(EXIT_CANNOT_RUN)

    os.close(slave)
    log = open(log_path, "wb", buffering=0)
    asked_to_stop = []                                 # the wrapper was signalled: it has happened
    killed = []                                        # and the program did not go on its own: it was killed

    def forward(signum, _frame):
        asked_to_stop.append(signum)
        try:
            os.killpg(child, signum)
        except OSError:
            pass

    for s in FORWARD:
        signal.signal(s, forward)

    status = None
    asked_at = None
    while True:
        try:
            ready, _, _ = select.select([master], [], [], 0.2)
        except InterruptedError:
            ready = []
        chunk = b""
        if ready:
            try:
                chunk = os.read(master, 65536)         # EIO on Linux once the program is gone
            except OSError:
                chunk = b""
            if chunk:
                log.write(chunk)
        if status is None:
            pid, st = os.waitpid(child, os.WNOHANG)
            if pid == child:
                status = st
                continue                                   # keep draining what the pty still holds
        elif not chunk:
            break                                          # the program is gone and there is nothing left
        if asked_to_stop:
            now = time.time()
            if asked_at is None:
                asked_at = now
            elif not killed and now - asked_at > GRACE:
                killed.append(True)
                try:
                    os.killpg(child, signal.SIGKILL)
                except OSError:
                    pass

    log.close()
    os.close(master)
    if status is None:                                      # only reachable if the loop left early
        _, status = os.waitpid(child, 0)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return os.WEXITSTATUS(status)


if __name__ == "__main__":
    sys.exit(main())
