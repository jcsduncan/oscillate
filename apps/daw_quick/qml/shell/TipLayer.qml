import QtQuick

// Draws the shell's tooltip (Tips) above everything, below the pointer's
// control, kept inside the window. Place one, filling the window, last.
Item {
    id: layer
    property color background: Theme.canvas
    property color border: Theme.tipBorder
    property color textColor: Theme.tipText
    property color dimTextColor: Theme.tipTextDim
    enabled: false // Never takes input.

    readonly property point at: {
        if (!Tips.shown || !Tips.item || !Tips.item.visible)
            return Qt.point(-1, -1)
        // Re-evaluate when the layer or the item moves.
        void (width + height + Tips.item.width + Tips.item.height)
        return Tips.item.mapToItem(layer, Tips.item.width / 2, Tips.item.height)
    }

    Rectangle {
        id: bubble
        objectName: "shellTip"
        visible: opacity > 0
        opacity: layer.at.x >= 0 ? 1 : 0
        Behavior on opacity {
            enabled: Motion.enabled
            NumberAnimation { duration: Motion.fast; easing.type: Motion.easing }
        }
        readonly property bool below: layer.at.y + Theme.tipGap + height <= layer.height - Theme.spaceXs
        x: Math.max(Theme.spaceXs, Math.min(layer.width - width - Theme.spaceXs, layer.at.x - width / 2))
        y: below ? layer.at.y + Theme.tipGap
                 : Math.max(Theme.spaceXs, layer.at.y - (Tips.item ? Tips.item.height : 0) - height - Theme.tipGap)
        width: Math.min(Theme.tipMaxWidth, row.implicitWidth + Theme.tipPaddingX)
        height: row.implicitHeight + Theme.tipPaddingY
        radius: Theme.shellTipRadius
        color: layer.background
        border.color: layer.border
        Row {
            id: row
            anchors.centerIn: parent
            spacing: Theme.spaceMd
            Text {
                text: Tips.text
                color: layer.textColor
                font.pixelSize: Theme.typeBody
                width: Math.min(implicitWidth, Theme.tipTextMaxWidth)
                wrapMode: Text.Wrap
            }
            Rectangle {
                visible: Tips.shortcut.length > 0
                anchors.verticalCenter: parent.verticalCenter
                width: keys.implicitWidth + Theme.tipKeyPaddingX
                height: Theme.tipKeyHeight
                radius: Theme.radiusSm
                color: Theme.tipKeyFill
                border.color: layer.border
                Text {
                    id: keys
                    anchors.centerIn: parent
                    text: Tips.shortcut
                    color: layer.dimTextColor
                    font.pixelSize: Theme.typeSmall
                    font.family: Theme.monoSystem
                }
            }
        }
    }
}
