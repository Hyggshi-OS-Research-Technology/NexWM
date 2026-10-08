// Main.qml — the HDE login screen for SDDM (login/sddm/hde/).
//
// Written to run in both greeters SDDM can start: the Qt 5 one (sddm-greeter) and the Qt 6 one (sddm-greeter-qt6),
// which is what Fedora ships. It uses nothing but the core QtQuick types — no QtGraphicalEffects (gone in Qt 6), no
// QtQuick.Controls and not even SDDM's own component module: every widget of the screen is drawn in components/, so
// the login screen looks the same everywhere. The only picture it needs is the background.
//
// Everything it shows can be set in theme.conf ([General]): the background, the accent colour, the clock, the user
// list or a plain user name field, the session and the keyboard layout choosers, the power buttons, the greeting.
//
// The context objects come from the greeter: sddm (the proxy: login(...), powerOff(), ...), userModel, sessionModel,
// keyboard (SDDM >= 0.19; guarded below) and config (theme.conf).
import QtQuick 2.0
import "components"

// The theme needs nothing but QtQuick: no SDDM component module (the greeter's own widgets are drawn here instead, so
// the login screen looks the same in every greeter and on every distribution), no QtGraphicalEffects (gone in Qt 6).
Rectangle {
    id: root
    width: 1280
    height: 800

    // ------------------------------------------------------------------ reading theme.conf (group [General])
    // The values arrive as QSettings gives them (a bool for "true"/"false", a string for the rest, nothing at all for
    // a key that is missing), so they all go through these helpers: a theme.conf that only names a few keys keeps the
    // defaults of the theme.
    function cfg(key, def) {
        if (typeof config === "undefined" || config === null) return def;
        var v;
        if (typeof config.value === "function") v = config.value(key, def);   // SDDM >= 0.17
        else v = config[key];
        return (v === undefined || v === null || v === "") ? def : v;
    }
    function cfgString(key, def) { return String(cfg(key, def)); }
    function cfgBool(key, def) {
        var v = cfg(key, def);
        if (v === true || v === false) return v;
        var s = String(v).toLowerCase();
        return s === "true" || s === "yes" || s === "1" || s === "on";
    }
    function cfgNumber(key, def) {
        var v = cfg(key, def);
        var n = Number(v);
        return isNaN(n) ? def : n;
    }

    // ------------------------------------------------------------------ configuration (theme.conf, group [General])
    readonly property string accent:     cfgString("accent", "#3584e4")
    readonly property string background: cfgString("background", "assets/background.png")
    readonly property real   dim:        cfgNumber("dim", 0.30)
    readonly property string greeting:   cfgString("greeting", "")
    readonly property string fontFamily: cfgString("fontFamily", "")
    readonly property bool   showLogo:   cfgBool("showLogo", true)
    readonly property bool   showClock:  cfgBool("showClock", true)
    readonly property bool   clock24h:   cfgBool("clock24h", true)
    readonly property bool   clockSeconds: cfgBool("clockSeconds", false)
    readonly property bool   clockDate:  cfgBool("clockDate", true)
    readonly property string clockPosition: cfgString("clockPosition", "top-right")
    readonly property bool   showHost:   cfgBool("showHost", true)
    readonly property bool   showSessions: cfgBool("showSession", true)
    readonly property bool   showLayouts: cfgBool("showLayout", true)
    readonly property bool   showPower:  cfgBool("showPower", true)
    readonly property bool   showUserList: cfgBool("showUserList", true)
    readonly property string userMode:   cfgString("userMode", "user") === "username" ? "username" : "user"
    readonly property string title:      cfgString("title", "Hyggshi Desktop Environment")
    readonly property string loginHint:  cfgString("loginHint", "")
    readonly property string capsLockMessage: cfgString("capsLockMessage", "Caps Lock is on")
    readonly property string footer:     cfgString("footer", "HDE · Hyggshi OS")
    readonly property int    cardWidth:  cfgNumber("cardWidth", 400)
    readonly property string buttonStyle: cfgString("buttonStyle", "wide") === "compact" ? "compact" : "wide"
    readonly property int    margin:     Math.round(Math.min(width, height) * 0.045)

    // the user the password will be checked for, the session to start, and what the greeter told us
    property string userName: ""
    property int sessionIndex: (typeof sessionModel !== "undefined" && sessionModel.count > 0)
                               ? Math.max(0, sessionModel.lastIndex) : -1
    property string message: ""
    property bool messageIsError: false
    property bool busy: false

    color: "#0b0f18"

    // ------------------------------------------------------------------ the screens (a background for each of them)
    Repeater {
        model: screenModel
        delegate: HdeBackground {
            x: model.geometry.x
            y: model.geometry.y
            width: model.geometry.width
            height: model.geometry.height
            source: root.background
            dim: root.dim
            accent: root.accent
        }
    }

    // the interface lives on the primary screen
    Item {
        id: screen
        readonly property var g: (typeof screenModel !== "undefined" && typeof screenModel.geometry === "function")
                                 ? screenModel.geometry(screenModel.primary !== undefined ? screenModel.primary : 0)
                                 : null
        x: g ? g.x : 0
        y: g ? g.y : 0
        width: g ? g.width : root.width
        height: g ? g.height : root.height

        // -------------------------------------------------------------- the clock (time, date, computer name)
        HdeClock {
            id: clock
            visible: root.showClock
            use24h: root.clock24h
            showSeconds: root.clockSeconds
            showDate: root.clockDate
            host: root.showHost ? sddm.hostName : ""
            fontFamily: root.fontFamily
            y: root.margin
            x: root.clockPosition === "top-left" ? root.margin
             : root.clockPosition === "top-center" ? Math.round((screen.width - width) / 2)
             : screen.width - width - root.margin
        }

        // -------------------------------------------------------------- the login card
        Rectangle {
            id: card
            width: Math.min(root.cardWidth, screen.width - 2 * root.margin)
            height: cardColumn.implicitHeight + 56
            // on a small screen the card shrinks instead of running off the edges
            scale: Math.min(1, (screen.height - 2 * root.margin) / Math.max(1, height))
            x: Math.round((screen.width - width) / 2)
            y: Math.round((screen.height - height) / 2) - Math.round(screen.height * 0.02)
            radius: 18
            color: "#141b29"
            border.width: 1
            border.color: "#26334a"

            // a click on the card itself closes an open list (the lists close by themselves when the cursor moves on)
            MouseArea {
                anchors.fill: parent
                z: -1
                onPressed: root.closeChoosers()
            }

            // a soft accent line on top of the card
            Rectangle {
                width: parent.width - 48
                height: 3
                radius: 1.5
                color: root.accent
                opacity: 0.9
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 1
            }

            Column {
                id: cardColumn
                width: parent.width - 48
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                HdeLogo {
                    visible: root.showLogo
                    width: 62
                    height: 62
                    accent: root.accent
                    anchors.horizontalCenter: parent.horizontalCenter
                }

                Text {
                    width: parent.width
                    text: root.title
                    color: "#f2f5fa"
                    font.pixelSize: 19
                    font.bold: true
                    font.family: root.fontFamily
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    visible: root.greeting !== ""
                    text: root.greeting
                    color: "#9fb0c8"
                    font.pixelSize: 13
                    font.family: root.fontFamily
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                // the users of this computer (avatars and real names)
                HdeUsers {
                    id: users
                    visible: root.showUserList && root.userMode === "user"
                    width: parent.width
                    accent: root.accent
                    fontFamily: root.fontFamily
                    onSelected: function (name) {
                        if (name && name !== "") root.userName = name;
                        root.message = "";
                        field.focusField();
                    }
                }

                // ... or a plain user name field (userMode=username)
                HdeField {
                    id: nameField
                    visible: root.userMode === "username"
                    width: parent.width
                    label: "User"
                    accent: root.accent
                    fontFamily: root.fontFamily
                    icon: "user"
                    onAccepted: root.tryLogin()
                }

                HdePassword {
                    id: field
                    width: parent.width
                    accent: root.accent
                    fontFamily: root.fontFamily
                    hint: root.loginHint
                    capsLockMessage: root.capsLockMessage
                    onAccepted: root.tryLogin()
                }

                // what went wrong (or the information the greeter sent)
                HdeMessage {
                    id: messageLine
                    width: parent.width
                    text: root.message
                    isError: root.messageIsError
                    fontFamily: root.fontFamily
                }

                // the session and the keyboard layout
                Row {
                    width: parent.width
                    spacing: 10
                    anchors.horizontalCenter: parent.horizontalCenter

                    HdeSessions {
                        id: sessions
                        visible: root.showSessions && (typeof sessionModel !== "undefined") && sessionModel.count > 0
                        width: (parent.width - 10) / 2
                        accent: root.accent
                        fontFamily: root.fontFamily
                        currentIndex: root.sessionIndex
                        onPicked: function (index) { root.sessionIndex = index; field.focusField(); }
                    }

                    HdeLayouts {
                        visible: root.showLayouts
                        width: (parent.width - 10) / 2
                        accent: root.accent
                        fontFamily: root.fontFamily
                    }
                }

                // the button that logs in
                HdeButton {
                    id: loginButton
                    width: root.buttonStyle === "wide" ? parent.width : Math.min(170, parent.width)
                    text: root.busy ? "Signing in…" : "Sign in"
                    accent: root.accent
                    fontFamily: root.fontFamily
                    busy: root.busy
                    enabled: !root.busy && root.userName !== ""
                    anchors.horizontalCenter: parent.horizontalCenter
                    onClicked: root.tryLogin()
                }
            }
        }

        // -------------------------------------------------------------- sleep / restart / shut down
        HdePower {
            id: power
            visible: root.showPower
            accent: root.accent
            fontFamily: root.fontFamily
            x: screen.width - width - 20
            y: screen.height - height - 24
        }

        // -------------------------------------------------------------- the footer
        Text {
            text: root.footer
            visible: root.footer !== ""
            color: "#7c8ba3"
            font.pixelSize: 11
            font.family: root.fontFamily
            x: root.margin
            y: screen.height - height - root.margin
        }
    }

    // ------------------------------------------------------------------ logging in
    function tryLogin() {
        if (root.busy) return;
        var user = root.userName;
        if (root.userMode === "username") user = nameField.text;
        if (!user || user === "") {
            root.message = root.userMode === "username" ? "Please type a user name." : "Please choose a user.";
            root.messageIsError = true;
            messageLine.shake();
            return;
        }
        root.userName = user;
        root.busy = true;
        root.message = "";
        sddm.login(user, field.passwordText, root.sessionIndex);
    }

    function closeChoosers() {
        if (root.showSessions) sessions.closeCombo();
        if (root.showLayouts) layouts.closeCombo();
    }

    function showGreeterMessage(text, isError) {
        root.busy = false;
        root.message = text;
        root.messageIsError = isError;
        field.clear();
        messageLine.shake();
    }

    Connections {
        target: sddm

        function onLoginSucceeded() {
            root.busy = false;
            root.message = "";
        }
        function onLoginFailed() {
            root.showGreeterMessage("Wrong password, or this user cannot log in.", true);
        }
        function onInformationMessage(message) {
            root.showGreeterMessage(message, true);
        }
    }

    Component.onCompleted: {
        // the user who logged in last (or the first one), like every login screen does
        if (root.userMode === "user" && root.showUserList && users.count > 0) {
            var last = (typeof userModel !== "undefined" && userModel.lastUser !== "") ? userModel.lastUser : users.currentName;
            root.userName = last;
        }
        if (root.userMode === "username") nameField.focusField();
        else field.focusField();
    }

    // Enter anywhere in the card logs in (the password field handles its own Enter)
    Keys.onReturnPressed: root.tryLogin()
    Keys.onEnterPressed: root.tryLogin()
    focus: true
}
