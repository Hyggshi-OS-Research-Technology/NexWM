// HdeLogo.qml — the HDE logo, drawn with plain Rectangle shapes (no images, no QtGraphicalEffects, so it works in
// the Qt 5 greeter and in the Qt 6 one). A rounded square in the accent colour with the gradient HDE uses, the
// title bar of a window and the H.
import QtQuick 2.0

Item {
    id: root
    width: 64
    height: 64
    property color accent: "#3584e4"

    Rectangle {
        anchors.fill: parent
        radius: width * 0.26
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.lighter(root.accent, 1.25) }
            GradientStop { position: 1.0; color: root.darker(root.accent, 0.55) }
        }
    }

    // the title bar of a window
    Rectangle {
        x: parent.width * 0.16
        y: parent.height * 0.18
        width: parent.width * 0.68
        height: parent.height * 0.10
        color: "#ffffff"
        opacity: 0.22
    }

    // the H
    Rectangle {
        x: parent.width * 0.26
        y: parent.height * 0.36
        width: parent.width * 0.12
        height: parent.height * 0.46
        color: "#ffffff"
        opacity: 0.95
    }
    Rectangle {
        x: parent.width * 0.62
        y: parent.height * 0.36
        width: parent.width * 0.12
        height: parent.height * 0.46
        color: "#ffffff"
        opacity: 0.95
    }
    Rectangle {
        x: parent.width * 0.26
        y: parent.height * 0.36 + parent.height * 0.46 / 2 - parent.height * 0.06
        width: parent.width * 0.48
        height: parent.height * 0.12
        color: "#ffffff"
        opacity: 0.95
    }

    function lighter(c, f) {
        return Qt.rgba(Math.min(1, c.r * f + 0.08), Math.min(1, c.g * f + 0.08), Math.min(1, c.b * f + 0.08), 1);
    }
    function darker(c, f) {
        return Qt.rgba(c.r * f, c.g * f, c.b * f + 0.05, 1);
    }
}
