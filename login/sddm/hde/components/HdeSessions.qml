// HdeSessions.qml — which desktop session to start (HDE (X11), HDE (Wayland)).
//
// Reads SDDM's sessionModel (/usr/share/xsessions and /usr/share/wayland-sessions), normalizes HDE session names so
// both "HDE (X11)" and "HDE (Wayland)" are always clearly presented (without exposing internal NexWM or non-HDE
// fallback entries), shows a protocol badge ("X11" / "Wayland") on every entry, and provides a 1-click X11 | Wayland
// quick switcher.
import QtQuick 2.0

Column {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property int currentIndex: 0
    readonly property bool open: combo.open          // the list of sessions is open: the keyboard is its
    property var rawEntries: []
    property var names: ["HDE (X11)", "HDE (Wayland)"]
    property var protocols: ["X11", "Wayland"]       // "Wayland" or "X11" for each displayed item
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
    spacing: 6

    function detectProtocol(nameStr, execStr, fileStr) {
        var n = String(nameStr || "").toLowerCase();
        var e = String(execStr || "").toLowerCase();
        var f = String(fileStr || "").toLowerCase();
        if (n.indexOf("wayland") >= 0 || e.indexOf("wayland") >= 0 || f.indexOf("wayland") >= 0)
            return "Wayland";
        return "X11";
    }

    function isHdeSession(nameStr, execStr, fileStr) {
        var n = String(nameStr || "").toLowerCase();
        var e = String(execStr || "").toLowerCase();
        var f = String(fileStr || "").toLowerCase();
        return n.indexOf("hde") >= 0 || n.indexOf("hyggshi") >= 0 || n.indexOf("nexwm") >= 0 ||
               e.indexOf("hde-start") >= 0 || e.indexOf("hde-session") >= 0 || e.indexOf("nexwm") >= 0 ||
               f.indexOf("hde") >= 0 || f.indexOf("nexwm") >= 0;
    }

    function formatSessionName(rawName, proto) {
        var n = String(rawName || "");
        if (proto === "Wayland") {
            if (n.indexOf("HDE") >= 0 || n.indexOf("NexWM") >= 0)
                return "HDE (Wayland)";
            return n;
        }
        if (n.indexOf("HDE") >= 0 || n.indexOf("NexWM") >= 0)
            return "HDE (X11)";
        return n;
    }

    function syncComboIndex() {
        if (root.modelIndices.length === 0) return;
        if (combo.index >= 0 && combo.index < root.modelIndices.length &&
            root.modelIndices[combo.index] === root.currentIndex) {
            return;
        }
        var mapped = root.modelIndices.indexOf(root.currentIndex);
        if (mapped >= 0) {
            combo.index = mapped;
        } else {
            combo.index = 0;
            if (root.modelIndices[0] >= 0 && root.currentIndex !== root.modelIndices[0]) {
                root.picked(root.modelIndices[0]);
            }
        }
    }

    onCurrentIndexChanged: root.syncComboIndex()

    function rebuildSessionList() {
        if (rawCollector.count > 0 && root.rawEntries.length < rawCollector.count) {
            return;
        }

        var hdeX11Idx = -1;
        var hdeX11Alias = true;
        var hdeWlIdx = -1;
        var hdeWlAlias = true;

        for (var j = 0; j < root.rawEntries.length; j++) {
            var r = root.rawEntries[j];
            if (!r.isHde) continue;
            if (r.proto === "Wayland" || r.displayName === "HDE (Wayland)") {
                if (hdeWlIdx < 0 || (hdeWlAlias && !r.isNexwmAlias)) {
                    hdeWlIdx = r.modelIndex;
                    hdeWlAlias = r.isNexwmAlias;
                }
            } else {
                if (hdeX11Idx < 0 || (hdeX11Alias && !r.isNexwmAlias)) {
                    hdeX11Idx = r.modelIndex;
                    hdeX11Alias = r.isNexwmAlias;
                }
            }
        }

        // Fall back between HDE (X11) and HDE (Wayland) if only one HDE entry is in sessionModel
        if (hdeX11Idx < 0 && hdeWlIdx >= 0) hdeX11Idx = hdeWlIdx;
        if (hdeWlIdx < 0 && hdeX11Idx >= 0) hdeWlIdx = hdeX11Idx;

        root.names = ["HDE (X11)", "HDE (Wayland)"];
        root.protocols = ["X11", "Wayland"];
        if (hdeX11Idx >= 0 && hdeWlIdx >= 0) {
            root.modelIndices = [hdeX11Idx, hdeWlIdx];
            root.syncComboIndex();
        } else if (root.rawEntries.length > 0) {
            var lastIdx = root.rawEntries.length - 1;
            root.modelIndices = [0, lastIdx];
            root.syncComboIndex();
        }
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
                var rawNm = (model.name !== undefined && model.name !== null && model.name !== "")
                            ? String(model.name) : ("Session " + (index + 1));
                var rawEx = "";
                var rawFl = "";
                try { if (model.exec !== undefined && model.exec !== null) rawEx = String(model.exec); } catch (e1) {}
                try { if (model.file !== undefined && model.file !== null) rawFl = String(model.file); } catch (e2) {}
                var pr = root.detectProtocol(rawNm, rawEx, rawFl);
                var disp = root.formatSessionName(rawNm, pr);
                var isAlias = rawNm.toLowerCase().indexOf("nexwm") >= 0 || rawFl.toLowerCase().indexOf("nexwm") >= 0;
                var hdeMatch = root.isHdeSession(rawNm, rawEx, rawFl);
                var next = root.rawEntries.slice();
                next.push({
                    modelIndex: index,
                    rawName: rawNm,
                    displayName: disp,
                    proto: pr,
                    isNexwmAlias: isAlias,
                    isHde: hdeMatch
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
