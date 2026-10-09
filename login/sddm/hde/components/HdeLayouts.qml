// HdeLayouts.qml — the keyboard layout of the login screen. SDDM 0.19 and newer hand the theme a `keyboard` object
// (`keyboard.layouts`, `keyboard.currentLayout`); with an older greeter there is nothing to choose, so the chooser is
// not shown at all.
import QtQuick 2.0

Column {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property alias currentIndex: combo.index
    readonly property bool open: combo.open          // the list of layouts is open: the keyboard is its
    readonly property bool available: (typeof keyboard !== "undefined") && keyboard !== null
                                      && keyboard.layouts !== undefined && keyboard.layouts.length > 1
    property var names: []
    function closeCombo() { combo.open = false; }
    spacing: 6

    function refresh() {
        var list = [];        // built here and assigned once: QML only notices a new array, not a push
        if (typeof keyboard !== "undefined" && keyboard && keyboard.layouts) {
            for (var i = 0; i < keyboard.layouts.length; i++) {
                var l = keyboard.layouts[i];
                list.push((l.longName !== undefined && l.longName !== "") ? l.longName
                          : (l.shortName !== undefined ? l.shortName : String(l)));
            }
        }
        root.names = list;
    }

    Component.onCompleted: refresh()
    Connections {
        // guarded: a greeter without the keyboard object (SDDM older than 0.19) has nothing to connect to, and an
        // unguarded `keyboard` here is a ReferenceError in the greeter's log on every one of those machines
        target: (typeof keyboard !== "undefined") ? keyboard : null
        function onLayoutsChanged() { root.refresh(); }
        function onCurrentLayoutChanged() { combo.index = keyboard.currentLayout; }
    }

    HdeCombo {
        id: combo
        visible: root.available
        width: parent.width
        label: "Layout"
        items: root.names
        accent: root.accent
        fontFamily: root.fontFamily
        onPicked: { if (typeof keyboard !== "undefined" && keyboard) keyboard.currentLayout = index; }
    }
}
