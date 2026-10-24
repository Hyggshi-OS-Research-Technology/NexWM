// HdeBackground.qml — the background of one screen: the picture of the theme (assets/background.png, or any picture
// the configuration names, or a plain colour like "#0e1520"), dimmed with a gradient so that white text stays
// readable whatever the picture is. When no picture is configured the background is a soft HDE gradient drawn
// with plain QML, so the screen always looks complete without shipping any image file.
import QtQuick 2.0

Item {
    id: root
    property string source: "assets/background.png"
    property real dim: 0.30
    property color accent: "#3584e4"
    property bool isColour: source.charAt(0) === "#"

    // the base picture / colour
    Image {
        id: picture
        anchors.fill: parent
        visible: !root.isColour
        source: root.source.indexOf("://") > 0 || root.source.charAt(0) === "/"
                  ? root.source : Qt.resolvedUrl("../" + root.source)
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
        smooth: true
    }

    // plain colour fallback when no picture is configured
    Rectangle {
        anchors.fill: parent
        color: root.isColour ? root.source : "#05070a"
        visible: root.isColour || root.source === ""
    }

    // a faint radial accent glow behind where the card will sit (only when using a picture),
    // drawn with a radial gradient so nothing but QtQuick is needed
    Rectangle {
        anchors.fill: parent
        visible: !root.isColour && root.source !== ""
        gradient: Gradient {
            GradientStop { position: 0.45; color: "#00000000" }
            GradientStop { position: 0.62; color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.18) }
            GradientStop { position: 0.78; color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.06) }
            GradientStop { position: 1.0; color: "#00000000" }
        }
    }

    // the dim: darker at the bottom and the top edges
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(0.02, 0.03, 0.05, root.dim * 0.9) }
            GradientStop { position: 0.45; color: Qt.rgba(0.02, 0.03, 0.05, root.dim * 0.30) }
            GradientStop { position: 1.0; color: Qt.rgba(0.01, 0.02, 0.04, root.dim * 1.30) }
        }
    }

    function lighter(c, f) {
        return Qt.rgba(Math.min(1, c.r * f + 0.06), Math.min(1, c.g * f + 0.06), Math.min(1, c.b * f + 0.06), 1);
    }
    readonly property color accentLighter: lighter(root.accent, 1.30)
}
