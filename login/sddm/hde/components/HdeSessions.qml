// HdeSessions.qml — which desktop session to start (HDE, HDE (Wayland), NexWM, NexWM (Wayland), ...).
//
// Reads SDDM's sessionModel (/usr/share/xsessions and /usr/share/wayland-sessions), detects whether each session runs
// on X11 or Wayland, shows a protocol badge ("X11" / "Wayland") on every entry, and provides a 1-click X11 | Wayland
// quick switcher bar above the dropdown so the user can switch display servers or desktop sessions effortlessly.
import QtQuick 2.0

Column {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property alias currentIndex: combo.index
    readonly property bool open: combo.open          // the list of sessions is open: the keyboard is its
    property var names: []
    property var protocols: []                       // "Wayland" or "X11" for each session
    readonly property string currentName: combo.currentText
    readonly property string currentProtocol: (combo.index >= 0 && combo.index < protocols.length)
                                              ? protocols[combo.index] : ""
    readonly property bool hasX11: protocols.indexOf("X11") >= 0
    readonly property bool hasWayland: protocols.indexOf("Wayland") >= 0
    signal picked(int index)
    function closeCombo() { combo.open = false; }
    spacing: 5

    function detectProtocol(nameStr, execStr) {
        var n = String(nameStr || "").toLowerCase();
        var e = String(execStr || "").toLowerCase();
        if (n.indexOf("wayland") >= 0 || e.indexOf("wayland") >= 0)
            return "Wayland";
        return "X11";
    }

    // Switch directly to an X11 or Wayland session while keeping the same WM family (NexWM vs HDE) when possible
    function selectProtocol(targetProto) {
        if (protocols.length === 0) return;
        var curNameLower = String(root.currentName || "").toLowerCase();
        var wantNexwm = curNameLower.indexOf("nexwm") >= 0;
        var best = -1;
        for (var i = 0; i < protocols.length; i++) {
            if (protocols[i] !== targetProto) continue;
            if (best < 0) best = i;
            var candLower = String(names[i] || "").toLowerCase();
            if (wantNexwm && candLower.indexOf("nexwm") >= 0) {
                best = i;
                break;
            }
            if (!wantNexwm && candLower.indexOf("hde") >= 0 && candLower.indexOf("nexwm") < 0) {
                best = i;
                break;
            }
        }
        if (best >= 0 && best !== combo.index) {
            combo.select(best);
        }
    }

    function cycleNext() {
        if (names.length <= 1) return;
        var nextIdx = (combo.index + 1) % names.length;
        combo.select(nextIdx);
    }

    // Walk sessionModel once and collect session names + display server protocols ("X11" / "Wayland")
    Repeater {
        model: (typeof sessionModel !== "undefined" && sessionModel !== null) ? sessionModel : []
        delegate: Item {
            visible: false
            width: 0
            height: 0
            Component.onCompleted: {
                var nm = (model.name !== undefined && model.name !== "") ? String(model.name) : ("Session " + (index + 1));
                var ex = (model.exec !== undefined) ? String(model.exec) : "";
                var proto = root.detectProtocol(nm, ex);
                root.names = root.names.concat([nm]);
                root.protocols = root.protocols.concat([proto]);
            }
        }
    }

    // Header row: "Session" label on the left + interactive X11 | Wayland quick-switch pills on the right
    Item {
        width: parent.width
        height: 18

        Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: "Session"
            color: "#9fb0c8"
            font.pixelSize: 12
            font.family: root.fontFamily
        }

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            visible: root.protocols.length > 0

            Rectangle {
                width: x11Lbl.implicitWidth + 10
                height: 18
                radius: 5
                visible: root.hasX11
                color: root.currentProtocol === "X11"
                       ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.30)
                       : "#111826"
                border.width: 1
                border.color: root.currentProtocol === "X11" ? root.accent : "#26334a"

                Text {
                    id: x11Lbl
                    anchors.centerIn: parent
                    text: "X11"
                    color: root.currentProtocol === "X11" ? "#ffffff" : "#8fa2bd"
                    font.pixelSize: 10
                    font.bold: root.currentProtocol === "X11"
                    font.family: root.fontFamily
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selectProtocol("X11")
                }
            }

            Rectangle {
                width: wlLbl.implicitWidth + 10
                height: 18
                radius: 5
                visible: root.hasWayland
                color: root.currentProtocol === "Wayland"
                       ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.30)
                       : "#111826"
                border.width: 1
                border.color: root.currentProtocol === "Wayland" ? root.accent : "#26334a"

                Text {
                    id: wlLbl
                    anchors.centerIn: parent
                    text: "Wayland"
                    color: root.currentProtocol === "Wayland" ? "#ffffff" : "#8fa2bd"
                    font.pixelSize: 10
                    font.bold: root.currentProtocol === "Wayland"
                    font.family: root.fontFamily
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.selectProtocol("Wayland")
                }
            }
        }
    }

    HdeCombo {
        id: combo
        width: parent.width
        label: ""
        items: root.names
        badges: root.protocols
        accent: root.accent
        fontFamily: root.fontFamily
        onPicked: root.picked(index)
    }

    Component.onCompleted: {
        if (typeof sessionModel !== "undefined" && sessionModel !== null && combo.index === 0)
            combo.index = Math.max(0, sessionModel.lastIndex);
    }
}
