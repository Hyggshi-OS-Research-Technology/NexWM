// HdeIconButton.qml — a small round button with a drawn sign (see HdeIcons.qml), for the power buttons.
import QtQuick 2.0

Item {
    id: root
    property string sign: "power"
    property string tip: ""
    property color accent: "#3584e4"
    property string fontFamily: ""
    signal clicked()
    width: 44
    height: 44

    Rectangle {
        id: circle
        anchors.fill: parent
        radius: width / 2
        color: mouse.containsMouse ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.22) : "#1a2435"
        border.width: 1
        border.color: mouse.containsMouse ? root.accent : "#2b3648"
        Behavior on color { ColorAnimation { duration: 110 } }
    }

    HdeIcons {
        name: root.sign
        background: circle.color
        color: mouse.containsMouse ? "#ffffff" : "#d3ddec"
        width: parent.width * 0.46
        height: width
        anchors.centerIn: parent
    }

    Text {
        id: tipLabel
        text: root.tip
        color: "#eef3fa"
        font.pixelSize: 11
        font.family: root.fontFamily
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: 5
        visible: mouse.containsMouse
        opacity: visible ? 1 : 0
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
