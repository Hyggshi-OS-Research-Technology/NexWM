// HdeCombo.qml — a chooser of the login screen (the session, the keyboard layout): a field showing what is chosen,
// a list that opens under it, and the arrow keys / Enter to walk through it and pick.
//
// It is drawn here instead of using SDDM's own ComboBox for two reasons: that one shows nothing in the field while
// the list is closed, and this way the theme does not need SDDM's component module at all (it needs nothing but
// QtQuick, which every greeter has).
import QtQuick 2.0

Item {
    id: root
    property string label: ""
    property var items: []                   // the texts to choose between
    property int index: 0
    property color accent: "#3584e4"
    property string fontFamily: ""
    property bool open: false
    readonly property string currentText: (index >= 0 && index < items.length) ? items[index] : ""
    signal picked(int index)
    signal closed()
    implicitHeight: (label !== "" ? labelLabel.height + 5 : 0) + field.height
    height: implicitHeight
    z: open ? 20 : 0                          // so the open list is above the rest of the card

    function select(i) {
        if (i < 0 || i >= items.length) return;
        root.index = i;
        root.picked(i);
    }
    function close() {
        if (!root.open) return;
        root.open = false;
        root.closed();
    }
    function toggle() {
        root.open = !root.open;
        if (!root.open) root.closed();
    }

    Text {
        id: labelLabel
        text: root.label
        color: "#9fb0c8"
        font.pixelSize: 12
        font.family: root.fontFamily
    }

    Rectangle {
        id: field
        anchors.top: labelLabel.bottom
        anchors.topMargin: root.label !== "" ? 5 : 0
        width: parent.width
        height: 36
        radius: 9
        color: "#0f1622"
        border.width: root.open || focus ? 2 : 1
        border.color: root.open || focus ? root.accent : "#26334a"
        Behavior on border.color { ColorAnimation { duration: 120 } }
        focus: root.open

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 11
            anchors.right: chevron.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            text: root.currentText
            color: "#eef3fa"
            font.pixelSize: 13
            font.family: root.fontFamily
            elide: Text.ElideRight
        }

        HdeIcons {
            id: chevron
            name: "chevron"
            color: root.open ? root.accent : "#9fb0c8"
            width: 14
            height: 14
            rotation: root.open ? 180 : 0
            anchors.right: parent.right
            anchors.rightMargin: 11
            anchors.verticalCenter: parent.verticalCenter
            Behavior on rotation { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: { root.toggle(); field.focus = root.open; }
        }

        Keys.onUpPressed: root.select(root.index - 1)
        Keys.onDownPressed: root.select(root.index + 1)
        Keys.onReturnPressed: root.close()
        Keys.onEnterPressed: root.close()
        Keys.onEscapePressed: root.close()
    }

    // the list, under the field
    Rectangle {
        id: dropdown
        anchors.top: field.bottom
        anchors.topMargin: 3
        width: field.width
        height: root.open ? Math.min(190, list.implicitHeight + 8) : 0
        visible: height > 0
        radius: 9
        color: "#141b29"
        border.width: 1
        border.color: "#26334a"
        clip: true

        Flickable {
            anchors.fill: parent
            anchors.margins: 4
            contentHeight: list.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: list
                width: parent.width
                Repeater {
                    model: root.items
                    delegate: Rectangle {
                        width: list.width
                        height: 30
                        radius: 6
                        color: index === root.index ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.25)
                             : rowMouse.containsMouse ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.14)
                             : "transparent"
                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            verticalAlignment: Text.AlignVCenter
                            text: modelData
                            elide: Text.ElideRight
                            color: index === root.index ? "#ffffff" : "#dbe4f2"
                            font.pixelSize: 13
                            font.family: root.fontFamily
                        }
                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { root.select(index); root.close(); }
                        }
                    }
                }
            }
        }

    }

    // the list closes when the field loses the cursor (the login screen gives it to the password field when the user
    // clicks there) - and the screen calls close() for a click on the card itself
    Connections {
        target: field
        function onActiveFocusChanged() {
            if (!field.activeFocus && root.open && root.visible) root.close();
        }
    }
}
