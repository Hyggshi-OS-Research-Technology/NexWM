// HdeClock.qml — the clock of the login screen: the time, the date and (if wanted) the name of the computer.
// A Column lays its children out itself, so the labels inside it have no anchors (see the QML rules for Positioners).
import QtQuick 2.0

Column {
    id: root
    property bool use24h: true
    property bool showSeconds: false
    property bool showDate: true
    property string host: ""
    property string fontFamily: ""
    property color textColor: "#eef3fa"
    property color subColor: "#9fb0c8"
    property string time: timeText()
    property string date: dateText()
    property real clockSize: 34
    property real dateSize: 13
    spacing: 3

    function timeText() {
        var d = new Date();
        var h = d.getHours();
        var m = ("0" + d.getMinutes()).slice(-2);
        var s = ("0" + d.getSeconds()).slice(-2);
        var out;
        if (root.use24h) {
            out = ("0" + h).slice(-2) + ":" + m;
        } else {
            var ampm = h < 12 ? "AM" : "PM";
            var h12 = h % 12;
            if (h12 === 0) h12 = 12;
            out = h12 + ":" + m + " " + ampm;
        }
        return root.showSeconds ? out + ":" + s : out;
    }
    function dateText() {
        return new Date().toLocaleDateString(Qt.locale(), "dddd, d MMMM yyyy");
    }

    Timer {
        interval: 1000
        running: true
        repeat: true
        onTriggered: { root.time = root.timeText(); root.date = root.dateText(); }
    }

    Text {
        text: root.time
        color: root.textColor
        font.pixelSize: root.clockSize
        font.bold: true
        font.family: root.fontFamily
    }
    Text {
        visible: root.showDate
        text: root.date
        color: root.subColor
        font.pixelSize: root.dateSize
        font.family: root.fontFamily
    }
    Text {
        visible: root.host !== ""
        text: root.host
        color: root.subColor
        font.pixelSize: 12
        font.family: root.fontFamily
    }
}
