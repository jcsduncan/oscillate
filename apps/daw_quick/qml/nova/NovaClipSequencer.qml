pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// The clip sequencer (NOVA-ARP-02/03): four clips of up to four bars on a
// 1/16 piano grid. Click a cell to add a note or remove the one under it;
// the wheel scrolls the keys. The clip playing with the transport is
// nova.seq.clip, and that is the clip shown. Drag ⇲ to the arrangement to
// make a MIDI clip of it (the panel reports sequenceDragged). From the
// keyboard the grid takes a cursor: the arrows move it (the keys scroll to
// follow), Space or Return adds or removes the note under it; the root
// steps with the arrow keys, and Return on ⇲ sends the clip.
NovaCard {
    id: card
    readonly property var controller: panel.controller
    readonly property int clipIndex: controller.revision >= 0 ? controller.choice("nova.seq.clip") : 0
    readonly property string clipId: "clip." + (clipIndex + 1)
    readonly property var sequence: panel.doc.sequences_revision >= 0 ? panel.doc.sequence(clipId) : ({ length: 0, notes: [] })
    readonly property real length: sequence.length > 0 ? sequence.length : 4
    readonly property int root: controller.revision >= 0 ? Math.round(controller.value("nova.seq.root")) : 24
    readonly property bool playing: panel.live && Math.round(panel.at(Contract.T.clip)) === clipIndex
    property int lowKey: 48
    // Notes in the sequencer's accent (time: amber).
    readonly property color accent: panel.accentFor("nova.seq.on")
    readonly property int rows: 24
    // Sixteenths in a beat and beats in a bar, for the grid and its scale.
    readonly property int stepsPerBeat: 4
    readonly property int beatsPerBar: 4
    // A note added from the grid: a sixteenth, at this velocity.
    readonly property real noteLength: 1 / stepsPerBeat
    readonly property int noteVelocity: 100
    readonly property int highestKey: 127
    // The keyboard cursor: its column and key.
    property int cursorColumn: 0
    property int cursorKey: lowKey + rows / 2

    function toggleNote(column, key) {
        panel.doc.toggle_sequence_note(clipId, column / stepsPerBeat, key, noteLength, noteVelocity, length)
    }
    // Moves the cursor's key, scrolling the keys to keep it shown.
    function moveCursorKey(direction) {
        cursorKey = Math.max(0, Math.min(highestKey, cursorKey + direction))
        if (cursorKey < lowKey)
            lowKey = cursorKey
        else if (cursorKey > lowKey + rows - 1)
            lowKey = cursorKey - rows + 1
    }

    objectName: "novaClipSequencer"
    title: "CLIP SEQUENCER"
    sub: "PLAYS WITH THE TRANSPORT OR FROM ITS TRIGGER NOTES"
    ledParameter: "nova.seq.on"
    dimWhenOff: false

    Row {
        anchors.right: parent.right
        anchors.rightMargin: card.panel.theme.cardInsetX
        y: card.panel.theme.cardHeaderControlsY
        spacing: card.panel.theme.spaceMd
        NovaSegs {
            panel: card.panel
            parameterId: "nova.seq.clip"
            labels: ["CLIP 1", "2", "3", "4"]
        }
        NovaSegs {
            objectName: "novaClipLength"
            panel: card.panel
            labels: ["1 BAR", "2", "4"]
            current: card.length > 8 ? 2 : card.length > 4 ? 1 : 0
            onPicked: index => card.panel.doc.set_sequence_length(card.clipId, [4, 8, 16][index])
        }
        NovaPill {
            panel: card.panel
            parameterId: "nova.seq.trigger"
            text: "TRIGGER: NOTE " + Contract.noteName(card.root) + "–" + Contract.noteName(card.root + 3)
        }
        Rectangle {
            width: rootRow.implicitWidth + 2 * card.panel.theme.seqRootInset
            height: card.panel.theme.pillHeight
            radius: card.panel.theme.radiusLg
            color: card.panel.theme.wellFill
            border.color: activeFocus ? card.accent : card.panel.theme.line2
            border.width: activeFocus ? card.panel.theme.strokeBold : card.panel.theme.hairline
            activeFocusOnTab: card.controller.editable
            Accessible.role: Accessible.SpinBox
            Accessible.name: "Trigger root"
            Accessible.description: Contract.noteName(card.root)
            Keys.onLeftPressed: card.controller.step("nova.seq.root", -1, false)
            Keys.onDownPressed: card.controller.step("nova.seq.root", -1, false)
            Keys.onRightPressed: card.controller.step("nova.seq.root", 1, false)
            Keys.onUpPressed: card.controller.step("nova.seq.root", 1, false)
            Row {
                id: rootRow
                x: card.panel.theme.seqRootInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: card.panel.theme.spaceSm
                Text {
                    objectName: "novaSeqRootDown"
                    text: "◀"
                    color: card.panel.theme.dim
                    font.pixelSize: card.panel.theme.typeSmall
                    MouseArea {
                        anchors.centerIn: parent
                        width: Math.max(parent.width, card.panel.theme.minHitSize)
                        height: Math.max(parent.height, card.panel.theme.minHitSize)
                        enabled: card.controller.editable
                        onClicked: card.controller.step("nova.seq.root", -1, false)
                    }
                }
                Text {
                    objectName: "novaSeqRoot"
                    text: "ROOT " + Contract.noteName(card.root)
                    color: card.panel.theme.text
                    font.family: card.panel.theme.mono
                    font.pixelSize: card.panel.theme.typeSmall
                }
                Text {
                    text: "▶"
                    color: card.panel.theme.dim
                    font.pixelSize: card.panel.theme.typeSmall
                    MouseArea {
                        anchors.centerIn: parent
                        width: Math.max(parent.width, card.panel.theme.minHitSize)
                        height: Math.max(parent.height, card.panel.theme.minHitSize)
                        enabled: card.controller.editable
                        onClicked: card.controller.step("nova.seq.root", 1, false)
                    }
                }
            }
        }
        NovaPill { panel: card.panel; text: "FOLLOW KEY"; parameterId: "nova.seq.follow" }
        Rectangle {
            objectName: "novaClipExport"
            width: exportText.implicitWidth + card.panel.theme.seqExportPad
            height: card.panel.theme.pillHeight
            radius: card.panel.theme.radiusLg
            color: exportArea.pressed ? card.panel.theme.soft(card.panel.theme.am) : card.panel.theme.wellFill
            border.color: activeFocus ? card.panel.theme.am : card.panel.theme.accentSoft
            border.width: activeFocus ? card.panel.theme.strokeBold : card.panel.theme.hairline
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Send the clip to the arrangement"
            Keys.onReturnPressed: card.panel.sequenceDragged(card.clipId)
            Keys.onSpacePressed: card.panel.sequenceDragged(card.clipId)
            Text {
                id: exportText
                anchors.centerIn: parent
                text: "⇲ TO ARRANGEMENT"
                color: card.panel.theme.am
                font.family: card.panel.theme.sans
                font.pixelSize: card.panel.theme.typeSmall
                font.weight: Font.Bold
            }
            MouseArea {
                id: exportArea
                anchors.fill: parent
                anchors.topMargin: -card.panel.theme.hitOutset(parent.height)
                anchors.bottomMargin: -card.panel.theme.hitOutset(parent.height)
                onReleased: card.panel.sequenceDragged(card.clipId)
            }
        }
    }

    // The grid in its frame: a tick at each bar; LIVE while the clip plays.
    Kit.PlotFrame {
        id: gridFrame
        theme: card.panel.theme
        live: card.panel.live
        x: card.panel.theme.cardInsetX
        y: card.panel.theme.cardBodyTop
        width: card.width - 2 * card.panel.theme.cardInsetX
        height: card.height - y - card.panel.theme.cardInsetX
        xUnit: "bar"
        xTicks: {
            const ticks = []
            const bars = Math.round(card.length / card.beatsPerBar)
            for (let b = 0; b < bars && grid.width > 0; ++b)
                ticks.push({ at: (grid.keysWidth + b * card.beatsPerBar * card.stepsPerBeat * grid.columnWidth) / grid.width,
                             label: String(b + 1) })
            return ticks
        }
        Rectangle {
            id: grid
            objectName: "novaClipGrid"
            anchors.fill: parent
            radius: card.panel.theme.displayRadius
            color: card.panel.theme.wellFill
            border.color: activeFocus ? card.accent : card.panel.theme.line
            border.width: activeFocus ? card.panel.theme.strokeBold : card.panel.theme.hairline
            clip: true
            activeFocusOnTab: card.controller.editable
            Accessible.role: Accessible.Table
            Accessible.name: "Clip notes"
            Accessible.description: Contract.noteName(card.cursorKey) + ", step " + (card.cursorColumn + 1)
                + ". Arrow keys move, Space adds or removes a note"
            Keys.onLeftPressed: card.cursorColumn = Math.max(0, card.cursorColumn - 1)
            Keys.onRightPressed: card.cursorColumn = Math.min(grid.columns - 1, card.cursorColumn + 1)
            Keys.onUpPressed: card.moveCursorKey(1)
            Keys.onDownPressed: card.moveCursorKey(-1)
            Keys.onSpacePressed: card.toggleNote(card.cursorColumn, card.cursorKey)
            Keys.onReturnPressed: card.toggleNote(card.cursorColumn, card.cursorKey)
            readonly property real keysWidth: 44
            // The note names' inset and baseline in a row; a note's inset in its
            // cell and the least width it is drawn at.
            readonly property real keyLabelX: 6
            readonly property real keyLabelBaseline: 0.72
            readonly property real noteInsetX: 1
            readonly property real noteInsetY: 2
            readonly property real noteMinWidth: 3
            readonly property int columns: Math.round(card.length * 4)
            readonly property real rowHeight: height / card.rows
            readonly property real columnWidth: (width - keysWidth) / columns
            Canvas {
                id: canvas
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const t = card.panel.theme
                    const kw = grid.keysWidth, rh = grid.rowHeight, cw = grid.columnWidth
                    for (let r = 0; r < card.rows; ++r) {
                        const key = card.lowKey + card.rows - 1 - r
                        const black = [1, 3, 6, 8, 10].indexOf(key % 12) >= 0
                        ctx.fillStyle = black ? t.keyRowBlack : t.keyRowWhite
                        ctx.fillRect(kw, r * rh, width - kw, rh)
                        ctx.fillStyle = key % 12 === 0 ? t.dim : t.faint
                        ctx.font = t.typeCaption + "px \"" + t.mono + "\""
                        if (!black)
                            ctx.fillText(Contract.noteName(key), grid.keyLabelX, r * rh + rh * grid.keyLabelBaseline)
                    }
                    for (let c = 0; c <= grid.columns; ++c) {
                        ctx.fillStyle = c % 16 === 0 ? t.line2 : c % 4 === 0 ? t.gridLine2 : t.gridLine
                        ctx.fillRect(kw + c * cw, 0, t.hairline, height)
                    }
                    const notes = card.sequence.notes
                    for (let i = 0; i < notes.length; ++i) {
                        const r = card.lowKey + card.rows - 1 - notes[i].key
                        if (r < 0 || r >= card.rows)
                            continue
                        ctx.fillStyle = t.soft(card.accent, t.seqNoteMinAlpha + t.seqNoteVelocityAlpha * notes[i].velocity / 127)
                        ctx.fillRect(kw + notes[i].start * 4 * cw + grid.noteInsetX, r * rh + grid.noteInsetY,
                                     Math.max(grid.noteMinWidth, notes[i].length * 4 * cw - 2 * grid.noteInsetX), rh - 2 * grid.noteInsetY)
                    }
                    if (card.playing) {
                        const x = kw + (card.panel.at(Contract.T.clipPlayhead) % card.length) * 4 * cw
                        ctx.fillStyle = t.am
                        ctx.fillRect(x - t.strokeBold / 2, 0, t.strokeBold, height)
                    }
                }
            }
            Connections {
                target: card
                function onSequenceChanged() { canvas.requestPaint() }
                function onLowKeyChanged() { canvas.requestPaint() }
                function onLengthChanged() { canvas.requestPaint() }
                // The playhead leaves (or reaches) this clip.
                function onPlayingChanged() { canvas.requestPaint() }
            }
            Connections {
                target: card.panel
                enabled: card.playing && card.visible
                function onFrameChanged() { canvas.requestPaint() }
            }
            // The keyboard cursor, while the grid has the keys.
            Rectangle {
                visible: grid.activeFocus && card.cursorKey >= card.lowKey && card.cursorKey < card.lowKey + card.rows
                x: grid.keysWidth + Math.min(card.cursorColumn, grid.columns - 1) * grid.columnWidth
                y: (card.lowKey + card.rows - 1 - card.cursorKey) * grid.rowHeight
                width: grid.columnWidth
                height: grid.rowHeight
                color: "transparent"
                border.color: card.accent
                border.width: card.panel.theme.strokeBold
            }
            MouseArea {
                anchors.fill: parent
                enabled: card.controller.editable
                onClicked: mouse => {
                    if (mouse.x < grid.keysWidth)
                        return
                    const column = Math.floor((mouse.x - grid.keysWidth) / grid.columnWidth)
                    const key = card.lowKey + card.rows - 1 - Math.floor(mouse.y / grid.rowHeight)
                    card.cursorColumn = column
                    card.cursorKey = key
                    card.toggleNote(column, key)
                }
                onWheel: wheel => {
                    card.lowKey = Math.max(0, Math.min(card.highestKey + 1 - card.rows, card.lowKey + (wheel.angleDelta.y > 0 ? 1 : -1)))
                    wheel.accepted = true
                }
            }
        }
    }
}
