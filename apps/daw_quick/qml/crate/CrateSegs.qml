pragma ComponentBehavior: Bound
import QtQuick

// Joined segment buttons: a dark well with the chosen segment filled in
// the accent. Bound to a choice parameter they pick its
// value; unbound they show `labels` with `current` and report `picked`.
Rectangle {
    id: segs
    required property var controller
    required property CrateTheme theme
    property string parameterId: ""
    property var labels: []
    property int current: 0
    property color accent: theme.accent
    // objectNames for the tiles; default "<objectName>:<index>".
    property var tileNames: []
    property bool compact: false
    readonly property bool bound: parameterId !== ""
    readonly property int selected: bound
        ? (controller.revision >= 0 ? controller.choice(parameterId) : 0) : current
    readonly property bool enabledToPick: bound ? controller.editable : enabled
    signal picked(int index)

    implicitWidth: row.implicitWidth + 2 * theme.segInset
    implicitHeight: compact ? theme.segCompactHeight : theme.segWellHeight
    radius: compact ? theme.segCompactRadius : theme.segRadius
    color: theme.well
    border.color: theme.line2
    opacity: enabledToPick ? 1 : theme.knobDisabledOpacity
    Accessible.role: Accessible.PageTabList

    function pick(index) {
        if (!enabledToPick)
            return
        if (bound)
            controller.set_choice(parameterId, index)
        picked(index)
    }

    Row {
        id: row
        x: segs.theme.segInset
        y: segs.theme.segInset
        height: segs.height - 2 * segs.theme.segInset
        spacing: segs.theme.spaceXxs
        Repeater {
            model: segs.labels.length
            delegate: Rectangle {
                id: tile
                required property int index
                readonly property bool on: index === segs.selected
                objectName: segs.tileNames.length > index ? segs.tileNames[index] : segs.objectName + ":" + index
                // Never narrower than the kit's least hit size.
                width: Math.max(segs.theme.minHitSize,
                                label.implicitWidth + (segs.compact ? segs.theme.segTilePadCompact : segs.theme.segTilePad))
                height: row.height
                radius: segs.compact ? segs.theme.segTileRadiusCompact : segs.theme.segTileRadius
                color: on ? segs.accent : tileArea.containsMouse ? segs.theme.buttonHi : "transparent"
                activeFocusOnTab: segs.enabledToPick
                border.color: activeFocus ? segs.theme.ink : "transparent"
                Accessible.role: Accessible.PageTab
                Accessible.name: segs.labels[index]
                Accessible.checked: on
                Accessible.onPressAction: segs.pick(index)
                Keys.onSpacePressed: segs.pick(index)
                Keys.onReturnPressed: segs.pick(index)
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: segs.labels[tile.index]
                    color: tile.on ? segs.theme.onAccent : segs.theme.segIdleInk
                    font.family: segs.theme.sans
                    font.pixelSize: segs.compact ? segs.theme.labelSize : segs.theme.captionSize
                    font.weight: Font.DemiBold
                    font.letterSpacing: segs.compact ? 0 : segs.theme.segLetterSpacing
                }
                MouseArea {
                    id: tileArea
                    anchors.fill: parent
                    // A compact tile's pointer area reaches past the well
                    // to the kit's least hit size; its neighbours sit beside it.
                    anchors.topMargin: -segs.theme.hitOutset(parent.height)
                    anchors.bottomMargin: -segs.theme.hitOutset(parent.height)
                    hoverEnabled: true
                    onClicked: segs.pick(tile.index)
                }
            }
        }
    }
}
