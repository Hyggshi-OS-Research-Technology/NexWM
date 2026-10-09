// HdeUsers.qml — the users of this computer as tiles: the avatar (the picture the user set, from the user model) or a
// drawn placeholder, and the real name. Clicking one chooses it (the login screen then puts the cursor in the
// password field), and the chosen user is the one the password is checked for.
//
// The avatars come from SDDM's userModel (the `icon` role is already "file:///home/user/.face" when there is one), so
// nothing here reads /home: the greeter shows the list without the permissions other themes ask for.
import QtQuick 2.0

Item {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    property int currentIndex: 0
    // `userModel` is always there in SDDM's greeter, but a theme has to survive being loaded without it (typeof: a
    // plain `userModel ? ...` throws a ReferenceError when the name does not exist at all)
    readonly property bool haveModel: (typeof userModel !== "undefined") && userModel !== null
    readonly property int count: haveModel ? userModel.count : 0
    readonly property string currentName: (count > 0 && tileRepeater.itemAt(currentIndex))
                                           ? tileRepeater.itemAt(currentIndex).userName : ""
    signal selected(string name)

    implicitHeight: count > 0 ? Math.min(2, Math.ceil(count / 4)) * 90 + 6 : 0
    height: implicitHeight

    function choose(index) {
        if (index < 0 || index >= count) return;
        root.currentIndex = index;
        var tile = tileRepeater.itemAt(index);
        if (tile) root.selected(tile.userName);
    }

    Grid {
        id: grid
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        columns: Math.min(4, Math.max(1, root.count))
        spacing: 6

        Repeater {
            id: tileRepeater
            model: root.haveModel ? userModel : []

            delegate: Item {
                id: tile
                width: Math.max(84, grid.width / grid.columns - grid.spacing)
                height: 84
                readonly property string userName: model.name
                readonly property bool chosen: index === root.currentIndex

                Rectangle {
                    anchors.fill: parent
                    radius: 12
                    color: tile.chosen ? Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16) : "transparent"
                    border.width: tile.chosen ? 1 : 0
                    border.color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.55)
                }

                // the avatar (the picture of the user), or a drawn placeholder
                Rectangle {
                    id: avatarFrame
                    width: 46
                    height: 46
                    radius: width / 2
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: 6
                    color: "#1d2637"
                    border.width: tile.chosen ? 2 : 1
                    border.color: tile.chosen ? root.accent : "#2b3648"
                    clip: true

                    Image {
                        id: avatar
                        anchors.fill: parent
                        source: model.icon ? model.icon : ""
                        visible: source !== "" && status === Image.Ready
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                    }
                    HdeIcons {
                        visible: !avatar.visible
                        name: "user"
                        color: "#8fa2bd"
                        width: parent.width * 0.62
                        height: width
                        anchors.centerIn: parent
                    }
                }
                // the tick on the chosen user
                HdeIcons {
                    visible: tile.chosen
                    name: "check"
                    color: "#ffffff"
                    weight: 2
                    width: 14
                    height: 14
                    x: avatarFrame.x + avatarFrame.width - 13
                    y: avatarFrame.y + avatarFrame.height - 12
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: avatarFrame.bottom
                    anchors.topMargin: 4
                    width: parent.width - 8
                    text: (model.realName && model.realName !== "") ? model.realName : model.name
                    color: tile.chosen ? "#f4f8ff" : "#c3d0e2"
                    font.pixelSize: 12
                    font.family: root.fontFamily
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.choose(index)
                }
            }
        }
    }

    onCountChanged: {
        if (root.currentIndex >= root.count) root.currentIndex = Math.max(0, root.count - 1);
    }
    Component.onCompleted: {
        // the user who logged in last, when the model knows it
        if (root.haveModel && userModel.lastIndex >= 0 && userModel.lastIndex < root.count)
            root.currentIndex = userModel.lastIndex;
        if (root.count > 0) root.selected(root.currentName);
    }
}
