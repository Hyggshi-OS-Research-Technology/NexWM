// HdeBackground.qml — the background of one screen: the picture of the theme (assets/background.png, or any picture
// the configuration names, or a plain colour like "#0e1520"), dimmed with a gradient so that white text stays
// readable whatever the picture is.
import QtQuick 2.0

Item {
    id: root
    property string source: "assets/background.png"
    property real dim: 0.30
    property color accent: "#3584e4"
    property bool isColour: source.charAt(0) === "#"

    Rectangle {
        anchors.fill: parent
        color: root.isColour ? root.source : "#0b0f18"
    }

    Image {
        id: picture
        anchors.fill: parent
        visible: !root.isColour
        source: root.isColour ? "" : (root.source.indexOf("://") > 0 || root.source.charAt(0) === "/"
                                      ? root.source : Qt.resolvedUrl("../" + root.source))
        fillMode: Image.PreserveAspectCrop
        asynchronous: false
        cache: true
        smooth: true
    }

    // the dim: darker at the bottom and the top edges, a hint of the accent in the middle
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0.04, 0.05, 0.08, root.dim * 0.9) }
            GradientStop { position: 0.45; color: Qt.rgba(0.04, 0.05, 0.08, root.dim * 0.35) }
            GradientStop { position: 1.0; color: Qt.rgba(0.03, 0.04, 0.06, root.dim * 1.25) }
        }
    }
}
