// HdeButton.qml — a button of the login screen (Sign in): the accent colour, a lighter tone under the mouse, and a
// spinner when a login is on its way.
import QtQuick 2.0

Rectangle {
    id: root
    property string text: ""
    property color accent: "#3584e4"
    property string fontFamily: ""
    property bool busy: false
    signal clicked()
    implicitWidth: label.implicitWidth + 44
    height: 40
    radius: 9
    color: !root.enabled ? "#233046"
                  : mouse.pressed ? Qt.darker(accent, 1.35)
                  : mouse.containsMouse ? Qt.lighter(accent, 1.12) : accent
    Behavior on color { ColorAnimation { duration: 110 } }

    Text {
        id: label
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: root.busy ? 11 : 0
        text: root.text
        color: root.enabled ? "#ffffff" : "#8b9ab2"
        font.pixelSize: 15
        font.bold: true
        font.family: root.fontFamily
    }

    // the spinner: a small ring that turns while SDDM checks the password
    Item {
        visible: root.busy
        width: 16
        height: 16
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: label.left
        anchors.rightMargin: 8

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: "#ffffff"
            opacity: 0.35
        }
        Rectangle {
            width: 2
            height: 6
            color: "#ffffff"
            x: parent.width / 2 - width / 2
            y: 0
            transformOrigin: Item.Bottom
            RotationAnimation on rotation {
                running: root.busy
                loops: Animation.Infinite
                from: 0
                to: 360
                duration: 900
            }
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        enabled: root.enabled
        onClicked: root.clicked()
    }
}
