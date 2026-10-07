// HdeMessage.qml — the line under the password field: what the greeter said (wrong password, information) or what the
// theme wants to tell the user. A wrong password also shakes it, so the answer is noticed without reading.
import QtQuick 2.0

Item {
    id: root
    property string text: ""
    property bool isError: true
    property string fontFamily: ""
    implicitHeight: text !== "" ? label.implicitHeight + 4 : 0
    height: implicitHeight
    clip: true

    function shake() {
        if (root.text !== "") shakeAnim.restart();
    }

    Text {
        id: label
        anchors.fill: parent
        text: root.text
        color: root.isError ? "#ff8f8f" : "#9fd8a7"
        font.pixelSize: 12
        font.family: root.fontFamily
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    SequentialAnimation {
        id: shakeAnim
        NumberAnimation { target: label; property: "x"; to: -6; duration: 45 }
        NumberAnimation { target: label; property: "x"; to: 6; duration: 45 }
        NumberAnimation { target: label; property: "x"; to: -4; duration: 45 }
        NumberAnimation { target: label; property: "x"; to: 0; duration: 60 }
    }
}
