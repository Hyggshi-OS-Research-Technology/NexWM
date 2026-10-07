// HdeIcons.qml — the small signs of the theme, drawn with Rectangle shapes only (a circle with a gap, a crescent, a
// padlock, ...): no images to ship, no icon theme to depend on, and it works with the software renderer SDDM falls
// back to when there is no GPU. `name` picks the sign:
//
//   user    a head and shoulders (the placeholder for a user without an avatar)
//   power   the power sign: a ring with a gap and a bar on top
//   reboot  the same ring with an arrow
//   suspend a crescent moon
//   lock    a padlock
//   chevron a small chevron pointing down (the combos)
//   eye     an eye (show the password)
//   check   a tick (the chosen user)
import QtQuick 2.0

Item {
    id: root
    width: 24
    height: 24
    property string name: "user"
    property color color: "#e6ecf5"
    property color background: "#00000000"      // what is behind the sign (the crescent needs it)
    property real weight: 2

    // user: head + shoulders
    Rectangle {
        visible: root.name === "user"
        width: parent.width * 0.34
        height: width
        radius: width / 2
        color: root.color
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.14
    }
    Rectangle {
        visible: root.name === "user"
        width: parent.width * 0.62
        height: parent.height * 0.40
        radius: width / 2
        color: root.color
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.58
    }

    // power: a ring (a rounded rectangle with a hole = a transparent middle) with a bar on top
    Rectangle {
        visible: root.name === "power" || root.name === "reboot"
        anchors.fill: parent
        anchors.topMargin: parent.height * 0.16
        radius: width / 2
        color: "transparent"
        border.width: root.weight
        border.color: root.color
    }
    Rectangle {
        visible: root.name === "power" || root.name === "reboot"
        width: root.weight
        height: parent.height * 0.42
        color: root.color
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0
    }
    // reboot: the ring is open at the top right, with an arrow head there
    Rectangle {
        visible: root.name === "reboot"
        width: parent.width * 0.26
        height: root.weight
        color: root.color
        rotation: -40
        transformOrigin: Item.Right
        x: parent.width * 0.60
        y: parent.height * 0.20
    }

    // suspend: a crescent (a circle with a circle of the background colour on top of it, offset)
    Rectangle {
        visible: root.name === "suspend"
        width: parent.width * 0.74
        height: width
        radius: width / 2
        color: root.color
        anchors.centerIn: parent
    }
    Rectangle {
        visible: root.name === "suspend"
        width: parent.width * 0.66
        height: width
        radius: width / 2
        color: root.background
        x: parent.width * 0.38
        y: parent.height * 0.12
    }

    // lock: the body and the shackle
    Rectangle {
        visible: root.name === "lock"
        width: parent.width * 0.66
        height: parent.height * 0.52
        radius: 2
        color: root.color
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.42
    }
    Rectangle {
        visible: root.name === "lock"
        width: parent.width * 0.40
        height: parent.height * 0.40
        radius: width / 2
        color: "transparent"
        border.width: root.weight
        border.color: root.color
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.10
    }

    // chevron: a ∨ of two arms (rotate the whole sign for an ∧)
    Item {
        id: chevron
        visible: root.name === "chevron"
        readonly property real arm: Math.min(parent.width, parent.height) * 0.42
        readonly property real angle: 30
        readonly property real dx: arm * Math.cos(angle * Math.PI / 180)
        readonly property real dy: arm * Math.sin(angle * Math.PI / 180)
        anchors.fill: parent
        Rectangle {
            width: chevron.arm
            height: root.weight
            color: root.color
            rotation: chevron.angle
            transformOrigin: Item.Left
            x: parent.width / 2 - chevron.dx
            y: parent.height * 0.62 - chevron.dy
        }
        Rectangle {
            width: chevron.arm
            height: root.weight
            color: root.color
            rotation: -chevron.angle
            transformOrigin: Item.Left
            x: parent.width / 2
            y: parent.height * 0.62
        }
    }

    // eye: an outline with a pupil
    Rectangle {
        visible: root.name === "eye"
        width: parent.width * 0.80
        height: parent.height * 0.52
        radius: height / 2
        color: "transparent"
        border.width: root.weight
        border.color: root.color
        anchors.centerIn: parent
    }
    Rectangle {
        visible: root.name === "eye"
        width: parent.width * 0.22
        height: width
        radius: width / 2
        color: root.color
        anchors.centerIn: parent
    }

    // check: a tick (two bars)
    Rectangle {
        visible: root.name === "check"
        width: parent.width * 0.42
        height: root.weight
        color: root.color
        rotation: 45
        transformOrigin: Item.Right
        x: parent.width * 0.24
        y: parent.height * 0.62
    }
    Rectangle {
        visible: root.name === "check"
        width: parent.width * 0.30
        height: root.weight
        color: root.color
        rotation: -55
        transformOrigin: Item.Left
        x: parent.width * 0.62
        y: parent.height * 0.40
    }
}
