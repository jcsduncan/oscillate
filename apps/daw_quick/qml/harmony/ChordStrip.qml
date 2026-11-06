pragma ComponentBehavior: Bound
import QtQuick
import "../shell"

// The chord strip over the piano roll's notes (ADR-0084): the chords the
// clip's notes make, read from them, so a moved note renames its chord.
// Click a chord to select it and its notes; double-click to change it in the
// Chord tools.
Item {
    id: strip
    objectName: "chordStrip"
    required property var controller
    required property real keyboardWidth
    required property real gridWidth
    required property real spanTicks
    readonly property HarmonyTheme style: HarmonyTheme {}
    signal chordClicked(real startTicks, real endTicks)
    signal chordOpened()

    function xAt(ticks) { return keyboardWidth + gridWidth * ticks / Math.max(1, spanTicks) }

    Rectangle { anchors.fill: parent; color: strip.style.well }
    Text {
        width: strip.keyboardWidth
        height: parent.height
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: "CHORDS"
        color: strip.style.violet
        font.family: strip.style.mono
        font.pixelSize: strip.style.typeMicro
        font.weight: Font.Bold
        font.letterSpacing: strip.style.captionSpacing
    }
    Text {
        visible: strip.controller !== null && strip.controller.count === 0
        x: strip.keyboardWidth + strip.style.spaceMd
        height: parent.height
        verticalAlignment: Text.AlignVCenter
        text: "No chords yet: write some, or play two or more notes together"
        color: strip.style.faint
        font.family: strip.style.sans
        font.pixelSize: strip.style.typeSmall
    }
    Repeater {
        model: strip.controller ? strip.controller.chords : null
        delegate: Rectangle {
            id: block
            required property int index
            required property var modelData
            readonly property bool lit: strip.controller.selected === modelData.index
            objectName: "chordBlock" + modelData.index
            x: strip.xAt(Number(modelData.startTicks)) + strip.style.stripBlockGap
            y: strip.style.stripBlockMarginY
            width: Math.max(strip.style.stripBlockMinWidth, strip.xAt(Number(modelData.endTicks))
                            - strip.xAt(Number(modelData.startTicks)) - 2 * strip.style.stripBlockGap)
            height: strip.height - 2 * strip.style.stripBlockMarginY
            radius: strip.style.radiusMd
            clip: true
            color: lit ? strip.style.alpha(strip.style.violet, strip.style.stripLitAlpha) : blockArea.containsMouse
                ? strip.style.panel3 : strip.style.panel2
            border.color: activeFocus ? strip.style.ink : lit ? strip.style.violet : strip.style.line2
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: modelData.name + ", " + modelData.roman + ". Enter selects its notes"
            Keys.onReturnPressed: blockArea.choose()
            // Tension, along the bottom edge.
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: strip.style.tensionBarHeight
                color: strip.style.tensionColor(block.modelData.tension)
                opacity: strip.style.tensionBarOpacity
            }
            Row {
                x: strip.style.spaceSm
                anchors.verticalCenter: parent.verticalCenter
                spacing: strip.style.itemGap
                Text {
                    text: block.modelData.name
                    color: block.lit ? strip.style.ink : strip.style.dim
                    font.family: strip.style.sans
                    font.pixelSize: strip.style.typeLabel
                    font.weight: Font.Bold
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: block.modelData.roman + (block.modelData.drone ? " · drone" : "")
                    color: strip.style.violet
                    font.family: strip.style.mono
                    font.pixelSize: strip.style.typeCaption
                    visible: block.width > strip.style.stripRomanMinWidth
                }
            }
            MouseArea {
                id: blockArea
                anchors.fill: parent
                hoverEnabled: true
                function choose() {
                    strip.controller.select(block.modelData.index)
                    strip.chordClicked(Number(block.modelData.startTicks), Number(block.modelData.endTicks))
                }
                onClicked: choose()
                onDoubleClicked: {
                    choose()
                    strip.chordOpened()
                }
                Tip { text: block.modelData.name + " · " + block.modelData.role + ". Double-click to change it" }
            }
        }
    }
}
