pragma ComponentBehavior: Bound
import QtQuick
import "shell"
import "devicekit/MacroPalette.js" as MacroPalette

// The track's eight macros at the head of its device chain (ADR-0059). Turn
// one to move everything mapped to it, heard as it moves and saved as one
// undo step on release. MAP arms a macro: touching any built-in synth or
// effect knob then maps it (or right-click a knob, "Map to Macro N"). The
// selected macro lists its mappings, each with a range to drag and a remove
// button. SAVE RACK keeps the track's whole device set as a rack.
Rectangle {
    id: strip
    required property var bridge
    required property string trackId
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    readonly property var macros: bridge.macros
    readonly property var entries: macros ? macros.macros : []
    property bool expanded: true
    property int selected: 0
    readonly property bool mapping: macros !== null && macros.armed >= 0
    readonly property var selectedEntry: entries.length > selected ? entries[selected] : null
    // Layout: the header row, the knob grid, then the selected macro's mappings.
    readonly property int expandedWidth: 252
    readonly property int collapsedWidth: 30
    readonly property int bodyInsetX: 10
    readonly property int headerHeight: 22
    readonly property int headerButtonHeight: 20
    readonly property int headerButtonPadding: 14
    readonly property int rackRowHeight: 26
    readonly property int knobColumns: 4
    readonly property int knobCellWidth: 56
    readonly property int knobCellHeight: 76
    readonly property int knobRingSize: 44
    readonly property int haloSize: 48
    readonly property int arcSize: 40
    readonly property int arcCountOffset: 13
    readonly property int knobNameY: 48
    readonly property int knobValueY: 61
    readonly property int haloBreathMs: 420
    // Vertical drag: pixels for the whole range, normal and with Shift.
    readonly property real turnPixels: 180
    readonly property real fineTurnPixels: 1600
    readonly property int mappingHeight: 40
    readonly property int mappingLabelRoom: 20
    readonly property int removeSize: 16
    readonly property int rangeY: 18
    readonly property int rangeHeight: 12
    readonly property int rangeLineY: 5
    readonly property int rangeBandY: 4
    readonly property int rangeBandHeight: 4
    readonly property int handleWidth: 10
    readonly property real nudgeStep: 0.01
    readonly property int mappingTextY: 30

    objectName: "macroStrip"
    width: expanded ? expandedWidth : collapsedWidth
    color: Theme.macroStripFill
    clip: true
    onTrackIdChanged: if (macros) macros.track_id = trackId
    Component.onCompleted: if (macros) macros.track_id = trackId
    Behavior on width { enabled: Motion.enabled; NumberAnimation { duration: Motion.normal; easing.type: Easing.OutCubic } }
    Rectangle { anchors.right: parent.right; width: Theme.hairline; height: parent.height; color: strip.lineColor }
    Keys.onEscapePressed: if (strip.mapping) strip.macros.armed = -1

    // Collapsed: a narrow tab that opens the strip again.
    Item {
        visible: !strip.expanded
        anchors.fill: parent
        activeFocusOnTab: !strip.expanded
        Accessible.role: Accessible.Button
        Accessible.name: "Show the macros"
        Keys.onReturnPressed: strip.expanded = true
        Keys.onSpacePressed: strip.expanded = true
        Text {
            anchors.centerIn: parent
            rotation: -90
            text: "MACROS"
            color: strip.dimColor; font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.tracking2xl
        }
        Tip { text: "Macros — Show the track's eight macros" }
        MouseArea { id: showArea; anchors.fill: parent; hoverEnabled: true; onClicked: strip.expanded = true }
    }

    Flickable {
        visible: strip.expanded
        x: strip.bodyInsetX; y: Theme.spaceMd
        width: parent.width - 2 * strip.bodyInsetX; height: parent.height - 2 * Theme.spaceMd
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: body
            width: parent.width
            spacing: Theme.spaceMd

            // MACROS · MAP · SAVE RACK · collapse
            Item {
                width: parent.width; height: strip.headerHeight
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "MACROS"
                    color: strip.accentColor; font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.tracking2xl
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceXs
                    Rectangle {
                        objectName: "macroMapButton"
                        width: mapText.implicitWidth + strip.headerButtonPadding; height: strip.headerButtonHeight; radius: Theme.radiusMd
                        color: strip.mapping ? MacroPalette.color(strip.macros.armed) : "transparent"
                        border.color: activeFocus ? strip.accentColor : Theme.macroButtonEdge
                        activeFocusOnTab: true
                        Accessible.role: Accessible.CheckBox
                        Accessible.checked: strip.mapping
                        Accessible.name: "Map knobs to Macro " + (strip.selected + 1)
                        function toggle() { strip.macros.armed = strip.mapping ? -1 : strip.selected }
                        Keys.onReturnPressed: toggle()
                        Keys.onSpacePressed: toggle()
                        Text {
                            id: mapText
                            anchors.centerIn: parent
                            text: "MAP"
                            color: strip.mapping ? Theme.macroInkDark : strip.textColor
                            font.pixelSize: Theme.typeCaption; font.bold: true
                        }
                        Tip { text: (strip.mapping ? "Mapping Macro " + (strip.macros.armed + 1) : "Map") + " — " + (strip.mapping ? "Touch knobs to map them · Esc or MAP to finish" : "Arm Macro " + (strip.selected + 1) + ", then touch synth or effect knobs to map them") }
                        MouseArea { id: mapArea; anchors.fill: parent; hoverEnabled: true; onClicked: parent.toggle() }
                    }
                    Rectangle {
                        objectName: "macroSaveRackButton"
                        width: rackText.implicitWidth + strip.headerButtonPadding; height: strip.headerButtonHeight; radius: Theme.radiusMd
                        color: "transparent"
                        border.color: activeFocus ? strip.accentColor : Theme.macroButtonEdge
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Save the track's devices and macros as a rack"
                        function open() {
                            rackName.text = ""
                            rackRow.visible = true
                            rackName.forceActiveFocus()
                        }
                        Keys.onReturnPressed: open()
                        Keys.onSpacePressed: open()
                        Text {
                            id: rackText
                            anchors.centerIn: parent
                            text: "SAVE RACK"
                            color: strip.textColor; font.pixelSize: Theme.typeCaption; font.bold: true
                        }
                        Tip { text: "Save rack — Keep this track's MIDI effects, built-in synth, effects and macros as a rack in the browser" }
                        MouseArea { id: saveRackArea; anchors.fill: parent; hoverEnabled: true; onClicked: parent.open() }
                    }
                    Rectangle {
                        width: strip.headerButtonHeight; height: strip.headerButtonHeight; radius: Theme.radiusMd
                        color: "transparent"
                        border.color: activeFocus ? strip.accentColor : Theme.macroButtonEdge
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: "Hide the macros"
                        Keys.onReturnPressed: strip.expanded = false
                        Keys.onSpacePressed: strip.expanded = false
                        Text { anchors.centerIn: parent; text: "‹"; color: strip.dimColor; font.pixelSize: Theme.typeEmphasis }
                        Tip { text: "Hide the macros" }
                        MouseArea { id: hideArea; anchors.fill: parent; hoverEnabled: true; onClicked: strip.expanded = false }
                    }
                }
            }

            // Naming a new rack.
            Rectangle {
                id: rackRow
                visible: false
                onVisibleChanged: if (visible && Motion.enabled) rackReveal.restart()
                NumberAnimation {
                    id: rackReveal
                    target: rackRow; property: "opacity"; from: 0; to: 1
                    duration: Motion.normal
                }
                width: parent.width; height: strip.rackRowHeight; radius: Theme.radiusMd
                color: Theme.rackNameFill
                border.color: strip.accentColor
                TextInput {
                    id: rackName
                    objectName: "macroRackName"
                    x: Theme.spaceMd; width: parent.width - 2 * Theme.spaceMd
                    anchors.verticalCenter: parent.verticalCenter
                    color: strip.textColor; font.pixelSize: Theme.typeBody
                    maximumLength: 64
                    clip: true
                    Accessible.role: Accessible.EditableText
                    Accessible.name: "Rack name; Enter saves, Escape cancels"
                    Keys.onReturnPressed: {
                        if (text.trim().length > 0)
                            strip.bridge.racks.save_rack(strip.trackId, text)
                        rackRow.visible = false
                    }
                    Keys.onEscapePressed: rackRow.visible = false
                    Text {
                        visible: rackName.text.length === 0
                        text: "Rack name"
                        color: strip.dimColor; font.pixelSize: Theme.typeBody
                    }
                }
            }

            Text {
                visible: strip.mapping
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Touch a knob to map it to " + (strip.mapping
                    ? strip.entries[strip.macros.armed].label : "") + " · Esc or MAP to finish"
                color: strip.mapping ? MacroPalette.color(strip.macros.armed) : strip.dimColor
                font.pixelSize: Theme.typeSmall
            }

            Grid {
                columns: strip.knobColumns
                columnSpacing: Theme.spaceXxs
                rowSpacing: Theme.spaceXs
                Repeater {
                    model: strip.entries
                    delegate: MacroKnob {}
                }
            }

            // The selected macro's mappings.
            Column {
                width: parent.width
                spacing: Theme.spaceXs
                visible: strip.selectedEntry !== null
                Text {
                    text: strip.selectedEntry
                        ? strip.selectedEntry.label.toUpperCase() + " · "
                          + strip.selectedEntry.mappings.length
                          + (strip.selectedEntry.mappings.length === 1 ? " MAPPING" : " MAPPINGS")
                        : ""
                    color: strip.selectedEntry ? MacroPalette.color(strip.selected) : strip.dimColor
                    font.pixelSize: Theme.typeCaption; font.bold: true; font.letterSpacing: Theme.trackingMd
                }
                Text {
                    visible: strip.selectedEntry !== null && strip.selectedEntry.mappings.length === 0
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Nothing mapped. Press MAP, then touch a synth or effect knob, or right-click a knob."
                    color: strip.dimColor; font.pixelSize: Theme.typeSmall
                }
                Repeater {
                    model: strip.selectedEntry ? strip.selectedEntry.mappings : []
                    delegate: MappingRow {}
                }
            }
        }
    }

    // One macro: a knob in its colour, its name (double-click to rename) and
    // its value. Drag vertically (Shift fine), scroll, or arrow keys.
    component MacroKnob: Item {
        id: cell
        required property var modelData
        required property int index
        readonly property color tint: MacroPalette.color(index)
        readonly property bool turning: strip.macros.turning === index
        // The arc eases to a new value (undo, a rack, automation) but tracks
        // the pointer exactly while turning.
        property real shownValue: modelData.value
        Behavior on shownValue {
            enabled: !cell.turning && Motion.enabled
            NumberAnimation { duration: Motion.fast; easing.type: Easing.OutCubic }
        }
        onShownValueChanged: arc.requestPaint()
        property real originValue: 0
        property real startY: 0
        objectName: "macroKnob:" + index
        width: strip.knobCellWidth; height: strip.knobCellHeight
        activeFocusOnTab: true
        Accessible.role: Accessible.Dial
        Accessible.name: modelData.label
        Accessible.description: modelData.value_text + ", " + modelData.mappings.length + " mapped"
        Keys.onUpPressed: event => strip.macros.step(index, 1, (event.modifiers & Qt.ShiftModifier) !== 0)
        Keys.onDownPressed: event => strip.macros.step(index, -1, (event.modifiers & Qt.ShiftModifier) !== 0)
        onActiveFocusChanged: if (activeFocus) strip.selected = index

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            y: (strip.haloSize - strip.knobRingSize) / 2
            width: strip.knobRingSize; height: strip.knobRingSize; radius: width / 2
            color: strip.selected === cell.index ? Theme.selection : "transparent"
            border.color: strip.mapping && strip.macros.armed === cell.index ? cell.tint
                : cell.activeFocus ? strip.accentColor : "transparent"
            border.width: Theme.macroRingWidth
        }
        // A soft halo breathes in the macro's colour while it turns.
        Rectangle {
            id: halo
            anchors.horizontalCenter: parent.horizontalCenter
            y: 0
            width: strip.haloSize; height: strip.haloSize; radius: width / 2
            color: "transparent"
            border.color: cell.tint
            border.width: Theme.strokeBold
            opacity: 0
            SequentialAnimation on opacity {
                running: cell.turning && Motion.enabled
                loops: Animation.Infinite
                onRunningChanged: if (!running) halo.opacity = 0
                NumberAnimation { to: Theme.macroHaloHigh; duration: strip.haloBreathMs; easing.type: Easing.InOutSine }
                NumberAnimation { to: Theme.macroHaloLow; duration: strip.haloBreathMs; easing.type: Easing.InOutSine }
            }
        }
        Canvas {
            id: arc
            anchors.horizontalCenter: parent.horizontalCenter
            y: (strip.haloSize - strip.arcSize) / 2
            width: strip.arcSize; height: strip.arcSize
            // A 270° arc from lower left to lower right round a centre cap.
            readonly property real arcRadius: 15
            readonly property real capRadius: 10
            readonly property real startAngle: Math.PI * 0.75
            readonly property real sweepAngle: Math.PI * 1.5
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const start = startAngle
                const sweep = sweepAngle
                ctx.lineWidth = Theme.macroArcWidth
                ctx.lineCap = "round"
                ctx.strokeStyle = Theme.macroTrack
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, arcRadius, start, start + sweep)
                ctx.stroke()
                ctx.strokeStyle = cell.tint
                ctx.globalAlpha = cell.modelData.mappings.length > 0 ? 1 : Theme.macroUnmappedAlpha
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, arcRadius, start, start + sweep * cell.shownValue)
                ctx.stroke()
                ctx.globalAlpha = 1
                ctx.fillStyle = Theme.macroKnobCap
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, capRadius, 0, Math.PI * 2)
                ctx.fill()
            }
            Connections {
                target: cell
                function onModelDataChanged() { arc.requestPaint() }
            }
        }
        Text {
            anchors.horizontalCenter: arc.horizontalCenter
            y: arc.y + strip.arcCountOffset
            text: cell.modelData.mappings.length > 0 ? cell.modelData.mappings.length : ""
            color: cell.tint; font.pixelSize: Theme.typeCaption; font.bold: true
        }
        Tip { text: (cell.modelData.label + "  " + cell.modelData.value_text) + " — " + ((cell.modelData.mappings.length === 0 ? "Nothing mapped yet\n" : cell.modelData.mappings.map(mapping => mapping.label).join("\n") + "\n") + (strip.mapping ? "Click to arm this macro" : "Drag · Shift fine · double-click resets · double-click the name renames")) }
        MouseArea {
            id: knobArea
            anchors.fill: arc
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                cell.forceActiveFocus()
                strip.selected = cell.index
                // While mapping, a macro chooses which one is armed.
                if (strip.mapping) {
                    strip.macros.armed = cell.index
                    return
                }
                cell.originValue = cell.modelData.value
                cell.startY = mouse.y
                strip.macros.begin_turn(cell.index)
            }
            onPositionChanged: mouse => {
                if (!pressed || !cell.turning)
                    return
                const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
                const travel = (cell.startY - mouse.y) / (fine ? strip.fineTurnPixels : strip.turnPixels)
                strip.macros.turn(cell.index, Math.max(0, Math.min(1, cell.originValue + travel)))
            }
            onReleased: if (cell.turning) strip.macros.end_turn(cell.index)
            onCanceled: if (cell.turning) strip.macros.cancel_turn(cell.index)
            onDoubleClicked: if (!strip.mapping) strip.macros.set_value(cell.index, 0)
            onWheel: wheel => {
                if (wheel.angleDelta.y !== 0)
                    strip.macros.step(cell.index, wheel.angleDelta.y > 0 ? 1 : -1,
                                      (wheel.modifiers & Qt.ShiftModifier) !== 0)
                wheel.accepted = true
            }
        }
        Text {
            id: nameText
            visible: !nameEdit.visible
            anchors.horizontalCenter: parent.horizontalCenter
            y: strip.knobNameY
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: cell.turning ? cell.modelData.value_text : cell.modelData.label
            color: cell.turning ? cell.tint : strip.textColor
            font.pixelSize: Theme.typeCaption
            elide: Text.ElideRight
            MouseArea {
                anchors.fill: parent
                onClicked: strip.selected = cell.index
                onDoubleClicked: {
                    nameEdit.text = cell.modelData.name
                    nameEdit.visible = true
                    nameEdit.forceActiveFocus()
                    nameEdit.selectAll()
                }
            }
        }
        TextInput {
            id: nameEdit
            visible: false
            anchors.horizontalCenter: parent.horizontalCenter
            y: strip.knobNameY
            width: parent.width
            horizontalAlignment: TextInput.AlignHCenter
            color: strip.textColor; font.pixelSize: Theme.typeCaption
            maximumLength: 48
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Macro name"
            Keys.onReturnPressed: {
                strip.macros.rename(cell.index, text)
                visible = false
            }
            Keys.onEscapePressed: visible = false
            onActiveFocusChanged: if (!activeFocus) visible = false
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: strip.knobValueY
            text: cell.modelData.value_text
            color: strip.dimColor; font.pixelSize: Theme.typeMicro
            visible: !cell.turning
        }
    }

    // A mapping: what it drives, the range it sweeps (drag either end; the
    // line shows where the macro has it now) and a remove button.
    component MappingRow: Item {
        id: row
        required property var modelData
        required property int index
        readonly property color tint: MacroPalette.color(strip.selected)
        property real shownMinimum: modelData.minimum
        property real shownMaximum: modelData.maximum
        property string dragText: ""
        objectName: "macroMapping:" + index
        width: parent ? parent.width : 0
        height: strip.mappingHeight
        onModelDataChanged: {
            shownMinimum = modelData.minimum
            shownMaximum = modelData.maximum
        }

        Text {
            width: parent.width - strip.mappingLabelRoom
            text: row.modelData.label
            color: strip.textColor; font.pixelSize: Theme.typeSmall
            elide: Text.ElideRight
        }
        Rectangle {
            objectName: "macroMappingRemove:" + row.index
            anchors.right: parent.right
            width: strip.removeSize; height: strip.removeSize; radius: Theme.radiusSm
            color: removeArea.containsMouse ? Theme.removeHoverFill : "transparent"
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Remove " + row.modelData.label + " from the macro"
            Keys.onReturnPressed: strip.macros.remove_mapping(strip.selected, row.index)
            Keys.onSpacePressed: strip.macros.remove_mapping(strip.selected, row.index)
            Text { anchors.centerIn: parent; text: "×"; color: Theme.removeGlyph; font.pixelSize: Theme.typeLabel }
            Tip { text: "Remove the mapping" }
            MouseArea {
                id: removeArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: strip.macros.remove_mapping(strip.selected, row.index)
            }
        }
        Item {
            id: track
            y: strip.rangeY
            width: parent.width; height: strip.rangeHeight
            Rectangle { y: strip.rangeLineY; width: parent.width; height: Theme.strokeBold; radius: height / 2; color: Theme.macroTrack }
            Rectangle {
                y: strip.rangeBandY; height: strip.rangeBandHeight; radius: Theme.radiusXs
                x: Math.min(row.shownMinimum, row.shownMaximum) * track.width
                width: Math.abs(row.shownMaximum - row.shownMinimum) * track.width
                color: row.tint; opacity: Theme.macroRangeOpacity
            }
            Repeater {
                model: [0, 1]
                delegate: Rectangle {
                    id: handle
                    required property int modelData
                    readonly property bool low: modelData === 0
                    x: (low ? row.shownMinimum : row.shownMaximum) * track.width - width / 2
                    y: 0
                    width: strip.handleWidth; height: strip.rangeHeight; radius: Theme.radiusSm
                    color: handleArea.pressed ? Theme.handlePressed : row.tint
                    border.color: Theme.macroInkDark
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Slider
                    Accessible.name: (low ? "Range start of " : "Range end of ") + row.modelData.label
                    Accessible.description: low ? row.modelData.minimum_text : row.modelData.maximum_text
                    function nudge(delta) {
                        const minimum = low ? Math.max(0, Math.min(1, row.modelData.minimum + delta))
                                            : row.modelData.minimum
                        const maximum = low ? row.modelData.maximum
                                            : Math.max(0, Math.min(1, row.modelData.maximum + delta))
                        strip.macros.set_mapping_range(strip.selected, row.index, minimum, maximum)
                    }
                    Keys.onLeftPressed: nudge(-strip.nudgeStep)
                    Keys.onRightPressed: nudge(strip.nudgeStep)
                    Tip { text: ((handle.low ? "From " + row.modelData.minimum_text : "To " + row.modelData.maximum_text)) + " — " + ("Where the macro takes " + row.modelData.label + " at " + (handle.low ? "0%" : "100%") + " · drag, or arrow keys") }
                    MouseArea {
                        id: handleArea
                        anchors.fill: parent
                        anchors.margins: -Theme.handleHitSlop
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: Qt.SizeHorCursor
                        onPositionChanged: mouse => {
                            if (!pressed)
                                return
                            const point = mapToItem(track, mouse.x, mouse.y)
                            const position = Math.max(0, Math.min(1, point.x / track.width))
                            if (handle.low)
                                row.shownMinimum = position
                            else
                                row.shownMaximum = position
                            row.dragText = strip.macros.position_text(strip.selected, row.index, position)
                        }
                        onReleased: {
                            row.dragText = ""
                            strip.macros.set_mapping_range(strip.selected, row.index,
                                                           row.shownMinimum, row.shownMaximum)
                        }
                    }
                }
            }
        }
        Text {
            y: strip.mappingTextY
            width: parent.width
            text: row.dragText !== "" ? row.dragText
                : row.modelData.minimum_text + " → " + row.modelData.maximum_text
                  + "  ·  now " + row.modelData.value_text
            color: strip.dimColor; font.pixelSize: Theme.typeCaption
            elide: Text.ElideRight
        }
    }
}
