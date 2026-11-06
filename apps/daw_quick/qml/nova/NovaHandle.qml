pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// A modulation source's ✥ handle (NOVA-UI-06): drag it onto any knob to
// route the source there at 30 %. The panel draws the ghost, lights the knob
// under the pointer and makes the route on release.
Rectangle {
    id: handle
    required property var panel
    required property string source
    property real glyphSize: panel.theme.typeBody

    objectName: "novaHandle:" + source
    width: panel.theme.handleSize
    height: panel.theme.handleSize
    radius: panel.theme.radiusMd
    color: area.pressed || area.containsMouse ? panel.theme.soft(panel.theme.vi, 2 * panel.theme.softAlpha) : panel.theme.viSoft
    Accessible.role: Accessible.Button
    Kit.HoverTip { theme: handle.panel.theme }
    Accessible.name: "Drag " + panel.sourceName(source) + " onto a knob to modulate it"

    Text {
        anchors.centerIn: parent
        text: "✥"
        color: handle.panel.theme.vi
        font.pixelSize: handle.glyphSize
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: mouse => {
            const at = mapToItem(handle.panel, mouse.x, mouse.y)
            handle.panel.beginSourceDrag(handle.source, at.x, at.y)
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const at = mapToItem(handle.panel, mouse.x, mouse.y)
            handle.panel.moveSourceDrag(at.x, at.y)
        }
        onReleased: mouse => {
            const at = mapToItem(handle.panel, mouse.x, mouse.y)
            handle.panel.endSourceDrag(at.x, at.y)
        }
        onCanceled: handle.panel.endSourceDrag(-1, -1)
    }
}
