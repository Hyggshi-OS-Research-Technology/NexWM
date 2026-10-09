#!/usr/bin/env python3
"""tests/sddm-qml-test.py — HDE's login screen (login/sddm/hde) in a real QML engine, with the greeter made here.

What this adds to tests/sddm-test.sh: SDDM's own greeter cannot be told to hand over *no users* (that depends on
/etc/sddm.conf.d and on the accounts of the machine), and that is the case the login screen has to survive: SDDM
leaves accounts out of its list by uid range ([Users] MinimumUid / MaximumUid) and by HideUsers / HideShells, and a
login screen without a user list and without a field to type a user name in is one that nobody can log in on (the
button stays grey). It loads Main.qml the way sddm-greeter does — QQmlEngine, a QQuickView, the greeter's context
objects (sddm, userModel, sessionModel, screenModel, config) as stand-ins — and checks what the card decides:

    python3 tests/sddm-qml-test.py [theme dir]     (default: login/sddm/hde of this checkout)

It also writes a picture of each case (shot-sddm-*.png in $HDE_TEST_OUT, default /tmp/hde-sddm): the login screen as it
really renders, which is what the CI job uploads as an artifact and what a reader who cannot log out of their own
session can look at.

  1. one user from the greeter: the tiles are shown, no name field, the keyboard in the password field
  2. an empty user list:        a field to type a user name in, the keyboard in it, and typing a name reaches
                                sddm.login() with what was typed
  3. no user model at all:      the same (a greeter that has none, or an older SDDM)

Needs PySide6 (Qt 6): `pip install PySide6-Essentials`. Without it the test says so and exits 77 (skipped), which is
what tests/sddm-test.sh looks for. No display, no SDDM, no root: it runs anywhere Python does, which is why the same
theme can be checked on the Qt 6 side while the greeter in CI renders it on the Qt 5 side.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
THEME = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "login", "sddm", "hde"))
MAIN = os.path.join(THEME, "Main.qml")
OUT = os.environ.get("HDE_TEST_OUT", "/tmp/hde-sddm")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")        # no display: the greeter is looked at, not shown
os.environ.setdefault("QT_QUICK_BACKEND", "software")        # what the CI greeter renders with too

try:
    from PySide6.QtCore import (QAbstractListModel, QEventLoop, QModelIndex, QObject, Property, Qt, QTimer, Signal,
                                Slot, qInstallMessageHandler, QUrl)
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtQml import QQmlEngine
    from PySide6.QtQuick import QQuickView
except ImportError as e:
    print("SKIP: sddm-qml: no PySide6 here (%s): pip install PySide6-Essentials" % e)
    sys.exit(77)

messages = []      # what the QML engine said (console.log of the theme, warnings, errors)
KEEP = []          # shiboken does not keep the Python stand-ins alive for the engine: hold on to them here
FAILURES = []
PASSES = 0


def say(line):
    print(line)
    try:
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, "results.txt"), "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


def pass_(what):
    global PASSES
    PASSES += 1
    say("PASS: sddm-qml: %s" % what)


def fail(what):
    FAILURES.append(what)
    say("FAIL: sddm-qml: %s" % what)


def check(ok, what):
    (pass_ if ok else fail)(what)


def qml_message(mode, context, message):
    messages.append(message)


class Config(QObject):
    """theme.conf the way SDDM hands it to a theme: config.value(key, default)"""

    VALUES = {"accent": "#3584e4", "background": "assets/background.png"}

    @Slot(str, "QVariant", result="QVariant")
    def value(self, key, default):
        return self.VALUES.get(key, default)


class Sddm(QObject):
    """the greeter's proxy: the properties and the methods a theme uses, and what login() was called with"""

    loginSucceeded = Signal()
    loginFailed = Signal()
    informationMessage = Signal(str)

    def __init__(self):
        super().__init__()
        self.login_call = None

    hostName = Property(str, lambda self: "hde-test")
    canSuspend = Property(bool, lambda self: True)
    canReboot = Property(bool, lambda self: True)
    canPowerOff = Property(bool, lambda self: True)

    @Slot(str, str, int)
    def login(self, user, password, session):
        self.login_call = (user, password, session)

    @Slot()
    def suspend(self):
        pass

    @Slot()
    def reboot(self):
        pass

    @Slot()
    def powerOff(self):
        pass


class UserModel(QAbstractListModel):
    """SDDM's user model: name / realName / icon per row, and the last user the greeter remembers"""

    Roles = {Qt.UserRole + 1: b"name", Qt.UserRole + 2: b"realName", Qt.UserRole + 3: b"icon"}

    def __init__(self, users, last_user):
        super().__init__()
        self.users = users
        self.last_user = last_user

    def roleNames(self):
        return self.Roles

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.users)

    def data(self, index, role=Qt.DisplayRole):
        u = self.users[index.row()]
        return {Qt.UserRole + 1: u["name"], Qt.UserRole + 2: u["realName"],
                Qt.UserRole + 3: u.get("icon", "")}.get(role)

    count = Property(int, lambda self: len(self.users))
    lastUser = Property(str, lambda self: self.last_user)
    lastIndex = Property(int, lambda self: next((i for i, u in enumerate(self.users)
                                                 if u["name"] == self.last_user), 0))


class SessionModel(QAbstractListModel):
    """the sessions of the machine (SDDM reads /usr/share/xsessions and /usr/share/wayland-sessions)"""

    Roles = {Qt.UserRole + 1: b"name", Qt.UserRole + 2: b"exec"}

    def __init__(self):
        super().__init__()
        self.sessions = [{"name": "NexWM (Wayland)", "exec": "/usr/bin/hde-start"},
                         {"name": "HDE", "exec": "/usr/bin/startplasma"}]

    def roleNames(self):
        return self.Roles

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.sessions)

    def data(self, index, role=Qt.DisplayRole):
        s = self.sessions[index.row()]
        return {Qt.UserRole + 1: s["name"], Qt.UserRole + 2: s["exec"]}.get(role)

    count = Property(int, lambda self: len(self.sessions))
    lastIndex = Property(int, lambda self: 0)


class ScreenModel(QAbstractListModel):
    """one 1280x800 screen, the size the CI greeter runs on"""

    Roles = {Qt.UserRole + 1: b"geometry"}
    GEOMETRY = {"x": 0, "y": 0, "width": 1280, "height": 800}

    def roleNames(self):
        return self.Roles

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else 1

    def data(self, index, role=Qt.DisplayRole):
        return self.GEOMETRY if role == Qt.UserRole + 1 else None

    count = Property(int, lambda self: 1)
    primary = Property(int, lambda self: 0)

    @Slot(int, result="QVariant")
    def geometry(self, screen):
        return self.GEOMETRY


def text_fields(item, found=None):
    """every TextInput of the card, in the order the file builds them (a hidden field is still built)"""
    if found is None:
        found = []
    if "TextInput" in item.metaObject().className():
        found.append(item)
    for child in item.childItems():
        text_fields(child, found)
    return found


def placeholder(field):
    """the text drawn inside a field while it is empty: "User name" or "Password" — that is how a field is named"""
    for child in field.childItems():
        try:
            text = child.property("text")
        except Exception:
            text = None
        if text:
            return str(text)
    return ""


def errors_in(msgs):
    """the messages of the QML engine that mean the theme is broken (a login screen that half-loads is worse than one
    that does not load at all: log in on it and the password goes nowhere)"""
    bad = []
    for m in msgs:
        first = m.strip().splitlines()[0]
        for pattern in ("is not a type", "Cannot find", "ReferenceError", "TypeError", "SyntaxError",
                        "Unable to assign", "Cannot assign", "is not a function", "Unable to determine",
                        "Undefined value", "unable to load", "was not found"):
            if pattern in first:
                bad.append(first)
                break
    return bad


def run(case, user_model, expect_users, expect_field, expect_message):
    """start the theme the way the greeter does, let it settle, and check what the card decided"""
    global messages
    messages = []
    engine = QQmlEngine()
    view = QQuickView(engine, None)
    KEEP.extend([engine, view])
    context = view.rootContext()
    sddm = Sddm()
    stubs = [Config(), sddm, SessionModel(), ScreenModel()]
    KEEP.extend(stubs)
    for name, obj in zip(["config", "sddm", "sessionModel", "screenModel"], stubs):
        context.setContextProperty(name, obj)
    if user_model is not None:
        KEEP.append(user_model)
        context.setContextProperty("userModel", user_model)

    say("INFO: sddm-qml: case %s" % case)
    view.setSource(QUrl.fromLocalFile(MAIN))
    root = view.rootObject()
    if not root:
        fail("%s: the theme did not load at all (%s)" % (case, " | ".join(messages[-4:])))
        return
    KEEP.append(root)
    view.resize(1280, 800)
    view.show()

    settle = QEventLoop()                      # the theme's own timers look after the keyboard: give them the time
    timer = QTimer()
    timer.setSingleShot(True)
    timer.setInterval(1500)
    timer.timeout.connect(settle.quit)
    KEEP.extend([settle, timer])
    timer.start()
    settle.exec()

    # a picture of the screen as it is now: the card, the users (or the field to type a name in), the accent, the
    # wallpaper — the thing a reader wants to see and cannot, since this machine's own login screen is not this theme
    shot = os.path.join(OUT, "shot-sddm-%s.png" % case.split(" ")[0])
    saved = False
    try:
        grabbed = view.grabWindow()                              # offscreen + software: this is the rendered theme
        saved = bool(grabbed) and grabbed.save(shot, "PNG")
    except Exception as e:                                       # no grabWindow in this PySide6, or no software render
        say("INFO: sddm-qml: no picture of %s (%s)" % (case, e))
    if saved:
        pass_("%s: a picture of the login screen was saved (%s)" % (case, shot))
    else:
        fail("%s: the login screen could not be saved as a picture (%s)" % (case, shot))

    errors = errors_in(messages)
    if errors:
        for e in errors:
            fail("%s: the QML engine says: %s" % (case, e))
    else:
        pass_("%s: the theme runs without a QML error (%d engine messages)" % (case, len(messages)))

    fields = {placeholder(f): f for f in text_fields(root)}
    for line in ("users=%s showNameField=%s message=%r" % (root.property("haveUserTiles"),
                                                           root.property("showNameField"),
                                                           root.property("message")),
                 "fields: " + (", ".join("%r%s" % (n, "" if f.property("visible") else " (hidden)")
                                         for n, f in fields.items()) or "none"),
                 "log: " + (" | ".join(m for m in messages if "hde-login" in m) or "nothing")):
        say("INFO: sddm-qml: %s" % line)

    name_field = fields.get("User name")
    password_field = fields.get("Password")
    check(root.property("haveUserTiles") == expect_users,
          "%s: the user tiles are %s" % (case, "shown" if expect_users else "not shown"))
    check(root.property("showNameField") == expect_field,
          "%s: a field to type a user name in is %s" % (case, "on the card" if expect_field else "not on the card"))
    check(password_field is not None and password_field.property("visible"),
          "%s: the password field is on the card" % case)
    check((name_field is not None and name_field.property("visible")) == expect_field,
          "%s: the user name field is %s" % (case, "visible" if expect_field else "hidden"))
    check(any("the greeter listed no users" in m for m in messages) if not expect_users
          else any("the greeter listed 1 user" in m for m in messages),
          "%s: the theme says in the greeter's log what it was given" % case)

    if expect_field:
        check(expect_message in (root.property("message") or ""),
              "%s: the card says why there is a field to type a name in (%r)" % (case, root.property("message")))
        check(name_field is not None and name_field.property("focus") and name_field.property("activeFocus"),
              "%s: the keyboard goes into the user name field" % case)
        if name_field is not None:
            name_field.setProperty("text", "hyggshi")       # what typing does, without a keyboard
            QGuiApplication.processEvents()
            check(root.property("userName") == "hyggshi",
                  "%s: a typed name reaches the login screen (userName=%r)" % (case, root.property("userName")))
            root.tryLogin()
            QGuiApplication.processEvents()
            check(sddm.login_call is not None and sddm.login_call[0] == "hyggshi",
                  "%s: signing in asks the greeter to log in %r" % (case, sddm.login_call and sddm.login_call[0]))
    else:
        check(password_field is not None and password_field.property("focus")
              and password_field.property("activeFocus"),
              "%s: the keyboard goes into the password field" % case)


def main():
    qInstallMessageHandler(qml_message)
    app = QGuiApplication([])                                    # noqa: F841 — the engine needs one
    run("1 (one user from the greeter)", UserModel([{"name": "runner", "realName": "CI Runner"}], "runner"),
        expect_users=True, expect_field=False, expect_message="")
    run("2 (an empty user list)", UserModel([], ""),
        expect_users=False, expect_field=True, expect_message="no user list")
    run("3 (no user model at all)", None,
        expect_users=False, expect_field=True, expect_message="no user list")
    print("")
    if FAILURES:
        say("== sddm-qml-test: %d check(s) FAILED (%d passed)" % (len(FAILURES), PASSES))
        sys.exit(1)
    say("== sddm-qml-test: all %d checks passed" % PASSES)


main()
