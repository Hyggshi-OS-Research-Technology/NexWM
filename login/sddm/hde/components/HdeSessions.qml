// HdeSessions.qml — which desktop session to start (HDE (X11), HDE (Wayland), ...).
//
// Reads SDDM's sessionModel (/usr/share/xsessions and /usr/share/wayland-sessions), normalizes HDE session names so
// both "HDE (X11)" and "HDE (Wayland)" are always clearly presented (without exposing internal NexWM entries),
// shows a protocol badge ("X11" / "Wayland") on every entry, and provides a 1-click X11 | Wayland quick switcher.
import QtQuick 2.0

Column {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property int currentIndex: 0
    readonly property bool open: combo.open          // the list of sessions is open: the keyboard is its
    property var rawEntries: []
    property var names: []
    property var protocols: []                       // "Wayland" or "X11" for each displayed item
    property var modelIndices: []                    // index in SDDM's sessionModel for each displayed item
    readonly property string currentName: combo.currentText
    readonly property string currentProtocol: (combo.index >= 0 && combo.index < protocols.length)
                                              ? protocols[combo.index] : ""
    readonly property int selectedModelIndex: (combo.index >= 0 && combo.index < modelIndices.length)
                                              ? modelIndices[combo.index] : currentIndex
    readonly property bool hasX11: protocols.indexOf("X11") >= 0
    readonly property bool hasWayland: protocols.indexOf("Wayland") >= 0
    signal picked(int index)
    function closeCombo() { combo.open = false; }
    spacing: 5

    function detectProtocol(nameStr, execStr, fileStr, typeStr) {
        var n = String(nameStr || "").toLowerCase();
        var e = String(execStr || "").toLowerCase();
        var f = String(fileStr || "").toLowerCase();
        var t = String(typeStr || "").toLowerCase();
        if (n.indexOf("wayland") >= 0 || e.indexOf("wayland") >= 0 ||
            f.indexOf("wayland") >= 0 || t.indexOf("wayland") >= 0)
            return "Wayland";
        return "X11";
    }

    function formatSessionName(rawName, proto) {
        var n = String(rawName || "");
        if (proto === "Wayland") {
            if (n === "HDE" || n === "HDE (Wayland)" || n === "NexWM" || n === "NexWM (Wayland)")
                return "HDE (Wayland)";
            return n;
        }
        if (n === "HDE" || n === "HDE (X11)" || n === "NexWM" || n === "NexWM (X11)")
            return "HDE (X11)";
        return n;
    }

    function syncComboIndex() {
        if (root.modelIndices.length === 0) return;
        var mapped = root.modelIndices.indexOf(root.currentIndex);
        if (mapped >= 0 && combo.index !== mapped) {
            combo.index = mapped;
        } else if (mapped < 0 && (combo.index < 0 || combo.index >= root.names.length)) {
            combo.index = 0;
        }
    }

    onCurrentIndexChanged: root.syncComboIndex()

    function rebuildSessionList() {
        var outNames = [];
        var outProtos = [];
        var outIndices = [];
        var outAliases = [];

        for (var j = 0; j < root.rawEntries.length; j++) {
            var r = root.rawEntries[j];
            var existing = outNames.indexOf(r.displayName);
            if (existing >= 0) {
                // Prefer the primary hde.desktop / hde-wayland.desktop entry over a legacy nexwm alias
                if (outAliases[existing] && !r.isNexwmAlias) {
                    outIndices[existing] = r.modelIndex;
                    outAliases[existing] = false;
                }
                continue;
            }
            outNames.push(r.displayName);
            outProtos.push(r.proto);
            outIndices.push(r.modelIndex);
            outAliases.push(r.isNexwmAlias);
        }

        // Ensure both HDE (X11) and HDE (Wayland) are always visible in the login chooser
        if (outNames.indexOf("HDE (X11)") < 0) {
            outNames.unshift("HDE (X11)");
            outProtos.unshift("X11");
            outIndices.unshift(outIndices.length > 0 ? outIndices[0] : 0);
        }
        if (outNames.indexOf("HDE (Wayland)") < 0) {
            var insertAt = outNames.indexOf("HDE (X11)") + 1;
            outNames.splice(insertAt, 0, "HDE (Wayland)");
            outProtos.splice(insertAt, 0, "Wayland");
            outIndices.splice(insertAt, 0, outIndices.length > 0 ? outIndices[0] : 0);
        }

        root.names = outNames;
        root.protocols = outProtos;
        root.modelIndices = outIndices;
        root.syncComboIndex();
    }

    // Switch directly to HDE (X11) or HDE (Wayland)
    function selectProtocol(targetProto) {
        if (protocols.length === 0) return;
        var targetLabel = (targetProto === "Wayland") ? "HDE (Wayland)" : "HDE (X11)";
        var best = names.indexOf(targetLabel);
        if (best < 0) {
            for (var i = 0; i < protocols.length; i++) {
                if (protocols[i] === targetProto) {
                    best = i;
                    break;
                }
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

    Repeater {
        id: rawCollector
        model: (typeof sessionModel !== "undefined" && sessionModel !== null) ? sessionModel : []
        delegate: Item {
            visible: false
            width: 0
            height: 0
            Component.onCompleted: {
                var rawNm = (model.name !== undefined && model.name !== "") ? String(model.name) : ("Session " + (index + 1));
                var rawEx = (model.exec !== undefined) ? String(model.exec) : "";
                var rawFl = (model.file !== undefined) ? String(model.file) : "";
                var rawTp = (model.type !== undefined) ? String(model.type) : "";
                var pr = root.detectProtocol(rawNm, rawEx, rawFl, rawTp);
                var disp = root.formatSessionName(rawNm, pr);
                var isAlias = rawNm.toLowerCase().indexOf("nexwm") >= 0 || rawFl.toLowerCase().indexOf("nexwm") >= 0;
                var next = root.rawEntries.slice();
                next.push({
                    modelIndex: index,
                    rawName: rawNm,
                    displayName: disp,
                    proto: pr,
                    isNexwmAlias: isAlias
                });
                next.sort(function (a, b) { return a.modelIndex - b.modelIndex; });
                root.rawEntries = next;
                root.rebuildSessionList();
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
        onPicked: {
            var modelIdx = (index >= 0 && index < root.modelIndices.length) ? root.modelIndices[index] : index;
            root.picked(modelIdx);
        }
    }

    Component.onCompleted: {
        root.rebuildSessionList();
    }
}
