// HdePower.qml — what can be done with the computer from the login screen: sleep, restart, shut down (only the ones
// SDDM says this machine supports — sddm.canSuspend, sddm.canReboot, sddm.canPowerOff).
import QtQuick 2.0

Row {
    id: root
    property color accent: "#3584e4"
    property string fontFamily: ""
    spacing: 10

    HdeIconButton {
        visible: sddm && sddm.canSuspend
        sign: "suspend"
        tip: "Sleep"
        accent: root.accent
        fontFamily: root.fontFamily
        onClicked: sddm.suspend()
    }
    HdeIconButton {
        visible: sddm && sddm.canReboot
        sign: "reboot"
        tip: "Restart"
        accent: root.accent
        fontFamily: root.fontFamily
        onClicked: sddm.reboot()
    }
    HdeIconButton {
        visible: sddm && sddm.canPowerOff
        sign: "power"
        tip: "Shut down"
        accent: root.accent
        fontFamily: root.fontFamily
        onClicked: sddm.powerOff()
    }
}
