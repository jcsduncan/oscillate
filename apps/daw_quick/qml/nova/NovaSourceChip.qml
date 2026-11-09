pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// A modulation source's tab on the MODULATION card (NOVA-UI-06/07,
// ADR-0076 revision 2): drag it onto any knob to route the source there at
// 30 % (the panel draws the ghost, lights the knob under the pointer and
// makes the route on release); click it to select it (panel.selectSource):
// an envelope or LFO opens its editor on the card, a macro shows the
// MACROS card. Envelopes are amber, the other sources violet, a selected
// tab filled; the raised count is the routes the source feeds.
Rectangle {
    id: chip
    required property var panel
    required property string source
    // What a click selects ("env.1", "lfo.3", "macros"), or "" for none.
    property string focusKey: ""
    property string text: panel.sourceName(source).toUpperCase()
    readonly property color accent: panel.sourceAccent(source)
    readonly property bool selected: focusKey !== "" && panel.modTab === focusKey
    readonly property int count: panel.doc.routes_revision >= 0 ? panel.doc.routes_from(source) : 0
    // A press becomes a drag once it moves this far.
    readonly property real dragThreshold: 4
    readonly property real fontSize: panel.theme.typeCaption
    // The route count, raised and smaller, as a superscript.
    readonly property real countSize: panel.theme.chipCountSize
    readonly property real countRise: 1
    readonly property real countOpacity: panel.theme.chipCountOpacity
    readonly property real fillAlpha: panel.theme.chipFillAlpha
    readonly property real edgeAlpha: panel.theme.chipEdgeAlpha

    objectName: "novaSource:" + source
    readonly property real padding: panel.theme.spaceMd
    readonly property real chipHeight: panel.theme.chipHeight
    readonly property real countGap: panel.theme.chipCountGap
    implicitWidth: row.implicitWidth + 2 * padding
    implicitHeight: chipHeight
    radius: panel.theme.chipRadius
    color: selected ? accent : panel.theme.soft(accent, fillAlpha)
    // While focused, a bold edge in the text colour (a selected tab is
    // already filled in the accent).
    border.color: activeFocus ? panel.theme.text : selected ? accent : panel.theme.soft(accent, edgeAlpha)
    border.width: activeFocus ? panel.theme.strokeBold : panel.theme.hairline
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: text + (focusKey !== "" ? ": click to edit, " : ": ") + "drag onto a knob to modulate it"
    Accessible.checked: selected
    Keys.onSpacePressed: activate()
    Keys.onReturnPressed: activate()
    Kit.HoverTip { theme: chip.panel.theme }

    function activate() {
        if (focusKey !== "")
            panel.selectSource(focusKey)
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: chip.countGap
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: chip.text
            color: chip.selected ? chip.panel.theme.bg : chip.accent
            font.family: chip.panel.theme.mono
            font.pixelSize: chip.fontSize
            font.weight: Font.DemiBold
            font.letterSpacing: chip.panel.theme.chipLetterSpacing
        }
        Text {
            visible: chip.count > 0
            y: label.y - chip.countRise
            text: chip.count
            color: chip.selected ? chip.panel.theme.bg : chip.accent
            opacity: chip.countOpacity
            font.family: chip.panel.theme.mono
            font.pixelSize: chip.countSize
        }
    }
    MouseArea {
        id: area
        property point pressedAt: Qt.point(0, 0)
        property bool dragging: false
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        cursorShape: dragging ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        onPressed: mouse => {
            pressedAt = Qt.point(mouse.x, mouse.y)
            dragging = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            const at = mapToItem(chip.panel, mouse.x, mouse.y)
            if (!dragging) {
                if (Math.abs(mouse.x - pressedAt.x) + Math.abs(mouse.y - pressedAt.y) < chip.dragThreshold)
                    return
                dragging = true
                chip.panel.beginSourceDrag(chip.source, at.x, at.y)
            }
            chip.panel.moveSourceDrag(at.x, at.y)
        }
        onReleased: mouse => {
            if (dragging) {
                dragging = false
                const at = mapToItem(chip.panel, mouse.x, mouse.y)
                chip.panel.endSourceDrag(at.x, at.y)
            } else {
                chip.activate()
            }
        }
        onCanceled: {
            if (dragging)
                chip.panel.endSourceDrag(-1, -1)
            dragging = false
        }
    }
}
