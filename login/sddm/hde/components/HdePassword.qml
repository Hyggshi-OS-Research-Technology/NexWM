// HdePassword.qml — the password field: the cursor is here most of the time, so it says so with the accent colour,
// warns when Caps Lock is on (SDDM 0.19 and newer tell the theme), and can show what was typed (the eye).
import QtQuick 2.0

Column {
    id: root
    property alias text: input.text
    readonly property string passwordText: input.text
    readonly property bool editing: input.activeFocus   // the keyboard is in this field
    property string hint: ""
    property color accent: "#3584e4"
    property string fontFamily: ""
    property string capsLockMessage: "Caps Lock is on"
    property bool showToggle: true
    property bool revealed: false
    signal accepted()
    spacing: 5

    // forceActiveFocus, not focus = true: the greeter shows its window after the theme has loaded and takes the
    // keyboard for itself on the way, which leaves a field that only asked politely without it (typing then goes
    // nowhere, and Enter is answered by the card instead of by the field: exactly what a login screen must not do).
    function focusField() { input.forceActiveFocus(); }
    function clear() { input.text = ""; root.revealed = false; input.forceActiveFocus(); }
    // SDDM >= 0.19 has a `keyboard` object in the theme; with an older one this is simply false
    function capsLockOn() {
        return (typeof keyboard !== "undefined") && keyboard && keyboard.capsLock === true;
    }

    Text {
        visible: root.hint !== ""
        text: root.hint
        color: "#9fb0c8"
        font.pixelSize: 12
        font.family: root.fontFamily
    }

    Rectangle {
        id: box
        width: parent.width
        height: 40
        radius: 9
        color: "#0f1622"
        border.width: input.activeFocus ? 2 : 1
        border.color: input.activeFocus ? root.accent : "#26334a"
        Behavior on border.color { ColorAnimation { duration: 120 } }

        HdeIcons {
            id: lock
            name: "lock"
            color: input.activeFocus ? root.accent : "#7c8ba3"
            width: 18
            height: 18
            anchors.left: parent.left
            anchors.leftMargin: 11
            anchors.verticalCenter: parent.verticalCenter
        }

        TextInput {
            id: input
            anchors.left: lock.right
            anchors.leftMargin: 9
            anchors.right: toggle.visible ? toggle.left : parent.right
            anchors.rightMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            color: "#eef3fa"
            font.pixelSize: 15
            font.family: root.fontFamily
            echoMode: root.revealed ? TextInput.Normal : TextInput.Password
            passwordCharacter: "•"
            selectionColor: root.accent
            selectedTextColor: "#ffffff"
            clip: true
            onAccepted: root.accepted()

            Text {
                text: "Password"
                color: "#6b7a92"
                font: input.font
                visible: input.text === "" && !input.activeFocus
            }
        }

        // show / hide what was typed
        HdeIcons {
            id: toggle
            visible: root.showToggle
            name: "eye"
            color: mouse.containsMouse || root.revealed ? root.accent : "#7c8ba3"
            width: 18
            height: 18
            anchors.right: parent.right
            anchors.rightMargin: 11
            anchors.verticalCenter: parent.verticalCenter

            MouseArea {
                id: mouse
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: { root.revealed = !root.revealed; input.focus = true; }
            }
        }
    }

    // Caps Lock is on: said in words, in the warning colour
    Row {
        visible: root.capsLockOn()
        spacing: 6
        Text {
            text: "▲"
            color: "#f6c76a"
            font.pixelSize: 11
        }
        Text {
            text: root.capsLockMessage
            color: "#f6c76a"
            font.pixelSize: 11
            font.family: root.fontFamily
        }
    }
}
