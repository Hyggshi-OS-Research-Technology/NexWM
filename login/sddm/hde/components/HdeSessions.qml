// HdeSessions.qml — which desktop session to start (HDE, HDE (Wayland), NexWM, ...). The list is the one the greeter
// provides (SDDM reads /usr/share/xsessions and /usr/share/wayland-sessions), so it shows exactly the sessions this
// system installed.
import QtQuick 2.0

Column {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property alias currentIndex: combo.index
    readonly property bool open: combo.open          // the list of sessions is open: the keyboard is its
    property var names: []
    signal picked(int index)
    function closeCombo() { combo.open = false; }
    spacing: 6

    // the names of the sessions, in the order of the model (the same trick as the layouts below: a hidden repeater
    // walks the model once and collects the texts)
    Repeater {
        model: sessionModel
        delegate: Item {
            visible: false
            width: 0
            height: 0
            // a new array each time: QML only notices a change when the property is assigned
            Component.onCompleted: root.names = root.names.concat([model.name])
        }
    }

    HdeCombo {
        id: combo
        width: parent.width
        label: "Session"
        items: root.names
        accent: root.accent
        fontFamily: root.fontFamily
        onPicked: root.picked(index)
    }

    Component.onCompleted: {
        if (typeof sessionModel !== "undefined" && combo.index === 0)
            combo.index = Math.max(0, sessionModel.lastIndex);
    }
}
