// HdeField.qml — a text field of the login screen (the user name), with a label above it and the accent colour when
// it has the focus.
import QtQuick 2.0

Column {
    id: root
    property alias text: input.text
    readonly property bool editing: input.activeFocus   // the keyboard is in this field
    property string label: ""
    property color accent: "#3584e4"
    property string fontFamily: ""
    property string icon: "user"
    signal accepted()
    spacing: 6

    // forceActiveFocus, not focus = true: the greeter shows its window after the theme has loaded and takes the
    // keyboard for itself on the way, which leaves a field that only asked politely without it. This puts the
    // keyboard in the field now, and in it again when the window becomes active.
    function focusField() { input.forceActiveFocus(); input.cursorPosition = input.text.length; }

    Text {
        visible: root.label !== ""
        text: root.label
        color: input.activeFocus ? root.accent : "#9fb0c8"
        font.pixelSize: 12
        font.bold: input.activeFocus
        font.family: root.fontFamily
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    Rectangle {
        id: box
        width: parent.width
        height: 44
        radius: 11
        color: "#0f1622"
        border.width: 1
        border.color: input.activeFocus ? root.accent : "#26334a"
        Behavior on border.color { ColorAnimation { duration: 130 } }
        Behavior on border.width { NumberAnimation { duration: 130 } }

        HdeIcons {
            id: sign
            visible: root.icon !== ""
            name: root.icon
            color: input.activeFocus ? root.accent : "#7c8ba3"
            width: 18
            height: 18
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
        }

        TextInput {
            id: input
            anchors.left: sign.visible ? sign.right : parent.left
            anchors.leftMargin: sign.visible ? 10 : 14
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            color: "#eef3fa"
            font.pixelSize: 15
            font.family: root.fontFamily
            selectionColor: root.accent
            selectedTextColor: "#ffffff"
            clip: true
            onAccepted: root.accepted()

            Text {
                text: root.label !== "" ? root.label : "User"
                color: "#6b7a92"
                font: input.font
                visible: input.text === "" && !input.activeFocus
            }
        }
    }
}
