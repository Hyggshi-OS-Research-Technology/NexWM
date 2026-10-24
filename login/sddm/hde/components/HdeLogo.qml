// HdeLogo.qml — the HDE logo, drawn with plain Rectangle shapes (no images, no QtGraphicalEffects, so it works in
// the Qt 5 greeter and in the Qt 6 one). A rounded square in the accent colour with a gradient, the title bar of a
// window and the H — the mark HDE uses everywhere.
import QtQuick 2.0

Item {
    id: root
    width: 64
    height: 64
    property color accent: "#3584e4"

    // the rounded square behind
    Rectangle {
        anchors.fill: parent
        radius: width * 0.22
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.lighter(root.accent, 1.20) }
            GradientStop { position: 1.0; color: root.darker(root.accent, 0.60) }
        }
    }

    // the title bar of a window (faint white)
    Rectangle {
        x: parent.width * 0.16
        y: parent.height * 0.20
        width: parent.width * 0.68
        height: parent.height * 0.10
        color: "#ffffff"
        opacity: 0.25
        radius: 2
    }

    // the H
    Rectangle {
        x: parent.width * 0.26
        y: parent.height * 0.36
        width: parent.width * 0.12
        height: parent.height * 0.46
        color: "#ffffff"
        opacity: 0.95
        radius: 1
    }
    Rectangle {
        x: parent.width * 0.62
        y: parent.height * 0.36
        width: parent.width * 0.12
        height: parent.height * 0.46
        color: "#ffffff"
        opacity: 0.95
        radius: 1
    }
    Rectangle {
        x: parent.width * 0.26
        y: parent.height * 0.36 + parent.height * 0.46 / 2 - parent.height * 0.06
        width: parent.width * 0.48
        height: parent.height * 0.12
        color: "#ffffff"
        opacity: 0.95
        radius: 2
    }

    function lighter(c, f) {
        return Qt.rgba(Math.min(1, c.r * f + 0.06), Math.min(1, c.g * f + 0.06), Math.min(1, c.b * f + 0.06), 1);
    }
    function darker(c, f) {
        return Qt.rgba(c.r * f, c.g * f, c.b * f + 0.05, 1);
    }
}
