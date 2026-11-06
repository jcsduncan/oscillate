pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "CrateContract.js" as Contract

// The Crate chop sampler panel (ADR-0063, spec 09 §5): the header with the sample, its
// tempo and root, the ERA and TIME choices; the chop-mode bar with CHOP TO
// MIDI over the waveform cut into coloured chops; the pads (bank A/B,
// bottom-left first as on hardware); the selected chop's controls; and the
// CHARACTER section.
//
// Every parameter goes through `controller` (NativeSynthDeviceController):
// crate.* globals and the selected chop's crate.chop.NN.* (by slot, through
// crate.chop_parameter). The sample, chops, peaks and chop actions go
// through `controller.crate` (CrateController, spec 09 §5.1), null for any
// other engine. Pads audition through the host: padPressed / padReleased.
Rectangle {
    id: panel
    required property var controller
    property CrateTheme theme: CrateTheme {}
    // The transport's playhead as a zero-based bar (the bridge's
    // playheadBar), where CHOP TO MIDI writes its clip.
    property real playheadBar: 0
    readonly property var crate: controller.crate !== undefined ? controller.crate : null
    readonly property bool hasSample: crate !== null && crate.has_sample
    readonly property var chops: crate !== null ? crate.chops : []
    readonly property int selected: crate !== null ? crate.selected : -1
    readonly property var selectedChop: selected >= 0 && selected < chops.length ? chops[selected] : null
    readonly property color selectedColour: selectedChop !== null ? Contract.tagColour(selectedChop.tag) : theme.accent
    readonly property int bank: crate !== null ? crate.bank : 0
    // The selected chop's parameter ids by control ({} without a chop).
    readonly property var chopIds: {
        const ids = {}
        if (crate === null || selectedChop === null)
            return ids
        const controls = Contract.chopKnobs.concat(Contract.envelopeKnobs, Contract.filterKnobs,
                                                   ["reverse", "mode", "filter"])
        for (let i = 0; i < controls.length; ++i)
            ids[controls[i]] = crate.chop_parameter(selected, controls[i])
        return ids
    }
    readonly property bool allReversed: {
        if (crate === null || chops.length === 0 || controller.revision < 0)
            return false
        for (let i = 0; i < chops.length; ++i)
            if (controller.choice(crate.chop_parameter(i, "reverse")) !== 1)
                return false
        return true
    }
    // The telemetry frame (crate.hpp layout) while a voice sounds, else [].
    property var frame: []
    readonly property bool live: frame.length >= Contract.T.size
    // Keys held down from the pads, by chop index, so a release lets go of
    // the key the press played.
    property var heldKeys: ({})

    // Asks the host to play `key` at `velocity` on this track until
    // padReleased(key) (the bridge's play_note / release_note).
    signal padPressed(int key, int velocity)
    signal padReleased(int key)

    objectName: "cratePanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    color: theme.caseColor
    border.color: theme.line
    clip: true
    Accessible.role: Accessible.Pane
    Accessible.name: "Crate chop sampler"

    function poll() {
        // Idle slots read -1, so the raw frame is used whenever there is one.
        const state = controller.telemetry()
        frame = state.frame !== undefined && state.frame.length >= Contract.T.size ? state.frame : []
    }
    Timer {
        objectName: "crateTelemetryPoll"
        interval: Contract.pollInterval
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    function slotPlayhead(slot) {
        return live ? Number(frame[Contract.T.playheads + slot]) : -1
    }
    readonly property int lastSlot: live ? Math.round(Number(frame[Contract.T.lastSlot])) : -1

    // Selects chop `index` and plays it until releasePad(index). In the keys
    // layout the pad chooses the chop the keyboard plays and sounds the root.
    function pressPad(index) {
        if (crate === null || index < 0 || index >= chops.length)
            return
        crate.selected = index
        let key = crate.pad_key(index)
        if (key < 0) {
            controller.set_value("crate.keys_chop", chops[index].slot)
            key = Math.round(controller.value("crate.root"))
        }
        const held = heldKeys
        held[index] = key
        heldKeys = held
        padPressed(key, Contract.auditionVelocity)
    }
    function releasePad(index) {
        const held = heldKeys
        if (held[index] === undefined)
            return
        const key = held[index]
        delete held[index]
        heldKeys = held
        padReleased(key)
    }

    function pickMode(index) {
        if (crate === null)
            return
        switch (Contract.modes[index].id) {
        case "transient": crate.chop_transients(crate.sensitivity); break
        case "grid": crate.chop_grid(crate.grid_division); break
        case "equal": crate.chop_equal(crate.equal_count); break
        default: crate.chop_mode = "manual"
        }
    }
    function loadSample(url) {
        if (crate !== null && controller.editable)
            crate.load_sample(String(url))
    }

    // ---- Shared pieces ---------------------------------------------------------------------

    component Caption: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.captionSize
        font.letterSpacing: panel.theme.captionSpacing
    }
    component Mono: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.mono
        font.pixelSize: panel.theme.captionSize
    }
    component Knob: CrateKnob {
        controller: panel.controller
        theme: panel.theme
    }
    // A value read as text, changed by dragging it vertically (Shift fine)
    // and reset with a double-click: SWING and the transient sensitivity.
    component DragValue: Rectangle {
        id: dragValue
        property string caption: ""
        property string valueText: ""
        // The value's position 0..1 when a drag starts.
        property real position: 0
        property real origin: 0
        property real startY: 0
        signal started()
        signal dragged(real position)
        signal finished()
        signal reset()
        implicitWidth: dragRow.implicitWidth + 2 * panel.theme.dragValueInset
        implicitHeight: panel.theme.segHeight
        radius: panel.theme.dragValueRadius
        color: dragArea.containsMouse || dragArea.pressed ? panel.theme.buttonHi : "transparent"
        border.color: activeFocus ? panel.theme.ink : panel.theme.line2
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: caption
        Accessible.description: valueText + ". Drag up or down; double-click resets"
        Row {
            id: dragRow
            anchors.centerIn: parent
            spacing: panel.theme.spaceSm
            Caption { text: dragValue.caption; anchors.verticalCenter: parent.verticalCenter }
            Mono {
                text: dragValue.valueText
                color: panel.theme.ink
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            id: dragArea
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                dragValue.forceActiveFocus()
                dragValue.origin = dragValue.position
                dragValue.startY = mouse.y
                dragValue.started()
            }
            onPositionChanged: mouse => {
                if (!pressed)
                    return
                const fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
                const travel = (dragValue.startY - mouse.y)
                    / (fine ? panel.theme.knobFinePixels : panel.theme.knobDragPixels)
                dragValue.dragged(Math.max(0, Math.min(1, dragValue.origin + travel)))
            }
            onReleased: dragValue.finished()
            onCanceled: dragValue.finished()
            onDoubleClicked: dragValue.reset()
        }
    }

    // ---- Header ----------------------------------------------------------------------------

    Rectangle {
        id: header
        width: parent.width
        height: panel.theme.headerHeight
        color: panel.theme.headerFill
        radius: panel.radius
        // Square off the header's lower corners against the body.
        Rectangle {
            y: parent.height - parent.radius
            width: parent.width
            height: parent.radius
            color: parent.color
        }
        Rectangle {
            y: parent.height - height
            width: parent.width
            height: panel.theme.hairline
            color: panel.theme.line
        }

        Row {
            id: headerLeft
            x: panel.theme.gutter
            height: parent.height
            spacing: panel.theme.headerGap
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.headerItemGap
                Text {
                    id: wordmark
                    objectName: "crateWordmark"
                    text: "CRATE"
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: Font.Bold
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
                Mono {
                    anchors.baseline: wordmark.baseline
                    text: "CHOP SAMPLER"
                    font.letterSpacing: panel.theme.taglineSpacing
                }
            }
            // The sample: its name and what it is.
            Rectangle {
                objectName: "crateSampleBox"
                anchors.verticalCenter: parent.verticalCenter
                width: sampleRow.implicitWidth + 2 * panel.theme.sampleBoxInset
                height: panel.theme.sampleBoxHeight
                radius: panel.theme.radiusLg
                color: panel.theme.caseColor
                border.color: panel.theme.line2
                Row {
                    id: sampleRow
                    x: panel.theme.sampleBoxInset
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.headerItemGap
                    // A record: a ringed disc with a spindle hole.
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.recordIconSize
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.color: panel.theme.inkDim
                        border.width: panel.theme.recordIconStroke
                        Rectangle {
                            anchors.centerIn: parent
                            width: panel.theme.recordIconHole
                            height: width
                            radius: width / 2
                            color: panel.theme.inkDim
                        }
                    }
                    Text {
                        objectName: "crateSampleName"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.hasSample ? panel.crate.sample_name : "No sample"
                        color: panel.hasSample ? panel.theme.ink : panel.theme.inkDim
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.bodySize
                        width: Math.min(implicitWidth, panel.theme.sampleNameWidth)
                        elide: Text.ElideMiddle
                    }
                    Text {
                        objectName: "crateSampleInfo"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.crate !== null && panel.crate.busy
                            ? (panel.crate.message !== "" ? panel.crate.message : "Loading…")
                            : Contract.infoLine(panel.crate)
                        visible: text !== ""
                        color: panel.crate !== null && panel.crate.busy ? panel.theme.accent : panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.bodySize
                    }
                }
            }
            Text {
                objectName: "crateMessage"
                anchors.verticalCenter: parent.verticalCenter
                visible: panel.crate !== null && !panel.crate.busy && panel.crate.message !== ""
                text: panel.crate !== null ? panel.crate.message : ""
                // What room the ERA and TIME choices leave.
                width: Math.max(0, Math.min(implicitWidth, headerRight.x - x - headerLeft.x - panel.theme.gutter))
                elide: Text.ElideRight
                color: panel.theme.warning
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
            }
        }

        Row {
            id: headerRight
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.gutter
            height: parent.height
            spacing: panel.theme.headerItemGap
            Caption { text: "ERA"; anchors.verticalCenter: parent.verticalCenter }
            CrateSegs {
                objectName: "crateEra"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                labels: Contract.eras.map(era => era.label)
                tileNames: Contract.eras.map((era, index) => "crateEra:" + index)
                current: panel.controller.revision >= 0 ? panel.controller.choice("crate.era") : 0
                enabled: panel.crate !== null && panel.controller.editable
                onPicked: index => panel.crate.set_era(index)
            }
            Item { width: panel.theme.spaceXxs; height: 1 }
            Caption { text: "TIME"; anchors.verticalCenter: parent.verticalCenter }
            CrateSegs {
                objectName: "crateTime"
                anchors.verticalCenter: parent.verticalCenter
                controller: panel.controller
                theme: panel.theme
                parameterId: "crate.time"
                labels: Contract.times
                tileNames: Contract.times.map((label, index) => "crateTime:" + index)
            }
        }
    }

    // ---- Waveform and the chop-mode bar -------------------------------------------------------

    Item {
        id: chopSection
        visible: panel.hasSample
        y: header.height
        width: parent.width
        height: panel.theme.modeBarTop + panel.theme.buttonHeight + panel.theme.waveGap + panel.theme.waveHeight
            + panel.theme.waveBottom

        Item {
            id: modeBar
            x: panel.theme.gutter
            y: panel.theme.modeBarTop
            width: parent.width - 2 * panel.theme.gutter
            height: panel.theme.buttonHeight

            Row {
                height: parent.height
                spacing: panel.theme.blockGap
                CrateSegs {
                    objectName: "crateMode"
                    anchors.verticalCenter: parent.verticalCenter
                    controller: panel.controller
                    theme: panel.theme
                    labels: Contract.modes.map(mode => mode.label)
                    tileNames: Contract.modes.map(mode => mode.objectName)
                    current: panel.crate !== null ? Contract.modeIndex(panel.crate.chop_mode) : 0
                    enabled: panel.controller.editable
                    onPicked: index => panel.pickMode(index)
                }
                // The mode's setting.
                DragValue {
                    objectName: "crateSensitivity"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.crate !== null && panel.crate.chop_mode === "transient"
                    caption: "SENS"
                    valueText: panel.crate !== null ? Contract.percent(panel.crate.sensitivity) : ""
                    position: panel.crate !== null ? panel.crate.sensitivity : 0
                    onDragged: position => panel.crate.sensitivity = position
                    onFinished: panel.crate.chop_transients(panel.crate.sensitivity)
                }
                CrateSegs {
                    objectName: "crateGrid"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.crate !== null && panel.crate.chop_mode === "grid"
                    compact: true
                    controller: panel.controller
                    theme: panel.theme
                    labels: Contract.gridLabels
                    tileNames: Contract.gridLabels.map((label, index) => "crateGrid:" + index)
                    current: panel.crate !== null ? Contract.gridDivisions.indexOf(panel.crate.grid_division) : 0
                    enabled: panel.controller.editable
                    onPicked: index => panel.crate.chop_grid(Contract.gridDivisions[index])
                }
                Row {
                    objectName: "crateEqualCount"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: panel.crate !== null && panel.crate.chop_mode === "equal"
                    spacing: panel.theme.spaceXs
                    readonly property int count: panel.crate !== null ? panel.crate.equal_count : 0
                    Kit.Button {
                        objectName: "crateEqualLess"
                        theme: panel.theme
                        label: "−"
                        height: panel.theme.segHeight
                        width: panel.theme.segHeight
                        active: parent.count > Contract.minEqualCount && panel.controller.editable
                        onClicked: panel.crate.chop_equal(parent.count - 1)
                    }
                    Mono {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.equalCountWidth
                        horizontalAlignment: Text.AlignHCenter
                        text: String(parent.count)
                        color: panel.theme.ink
                        font.pixelSize: panel.theme.bodySize
                    }
                    Kit.Button {
                        objectName: "crateEqualMore"
                        theme: panel.theme
                        label: "+"
                        height: panel.theme.segHeight
                        width: panel.theme.segHeight
                        active: parent.count < Contract.maxEqualCount && panel.controller.editable
                        onClicked: panel.crate.chop_equal(parent.count + 1)
                    }
                }
                Text {
                    objectName: "crateModeHint"
                    anchors.verticalCenter: parent.verticalCenter
                    text: Contract.modeHint(panel.crate)
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.bodySize
                }
            }

            Row {
                anchors.right: parent.right
                height: parent.height
                spacing: panel.theme.blockGap
                Mono {
                    objectName: "crateChopCount"
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.chops.length + (panel.chops.length === 1 ? " CHOP" : " CHOPS")
                    font.pixelSize: panel.theme.bodySize
                }
                Kit.Button {
                    objectName: "crateReverseAll"
                    anchors.verticalCenter: parent.verticalCenter
                    theme: panel.theme
                    label: "REVERSE ALL"
                    on: panel.allReversed
                    active: panel.controller.editable && panel.chops.length > 0
                    tip: panel.allReversed ? "Play every chop forwards" : "Play every chop backwards"
                    onClicked: panel.crate.reverse_all(!panel.allReversed)
                }
                Kit.Button {
                    objectName: "crateChopToMidi"
                    anchors.verticalCenter: parent.verticalCenter
                    theme: panel.theme
                    label: "CHOP TO MIDI"
                    prime: true
                    active: panel.controller.editable && panel.chops.length > 0
                    tip: "Write a MIDI clip at the playhead that plays the chops in their original order"
                    onClicked: panel.crate.chop_to_midi(Math.max(0, Math.floor(panel.playheadBar)))
                }
            }
        }

        CrateWaveform {
            id: waveform
            x: panel.theme.gutter
            y: modeBar.y + modeBar.height + panel.theme.waveGap
            width: parent.width - 2 * panel.theme.gutter
            height: panel.theme.waveHeight
            crate: panel.crate
            theme: panel.theme
            frame: panel.frame
        }

        Rectangle {
            y: parent.height - height
            width: parent.width
            height: panel.theme.hairline
            color: panel.theme.line
        }
    }

    // ---- Lower: pads, the selected chop, character ------------------------------------------------

    Item {
        id: lower
        visible: panel.hasSample
        y: chopSection.y + chopSection.height
        width: parent.width
        height: parent.height - y

        // Pads.
        Item {
            id: padsColumn
            width: panel.theme.padsWidth
            height: parent.height
            Rectangle {
                x: parent.width - width
                width: panel.theme.hairline
                height: parent.height
                color: panel.theme.line
            }
            Item {
                id: padsHeader
                x: panel.theme.gutter
                y: panel.theme.lowerTop
                width: parent.width - 2 * panel.theme.gutter
                height: panel.theme.segHeight
                Caption {
                    objectName: "cratePadsTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    readonly property int first: Contract.padKey(panel.bank * Contract.bankSize)
                    text: "PADS · " + Contract.noteName(first) + "–" + Contract.noteName(first + Contract.bankSize - 1)
                }
                Row {
                    anchors.right: parent.right
                    height: parent.height
                    spacing: panel.theme.spaceMd
                    CrateSegs {
                        objectName: "crateBank"
                        anchors.verticalCenter: parent.verticalCenter
                        compact: true
                        controller: panel.controller
                        theme: panel.theme
                        labels: ["A", "B"]
                        tileNames: ["crateBank:0", "crateBank:1"]
                        current: panel.bank
                        enabled: panel.crate !== null
                        onPicked: index => panel.crate.bank = index
                    }
                    DragValue {
                        objectName: "crateSwing"
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property string parameterId: "crate.swing"
                        caption: "SWING"
                        valueText: panel.controller.revision >= 0
                            ? Math.round(panel.controller.value(parameterId)) + "%" : ""
                        position: panel.controller.revision >= 0 ? panel.controller.normalised(parameterId) : 0
                        enabled: panel.controller.editable
                        onStarted: panel.controller.begin_gesture(parameterId)
                        onDragged: position => panel.controller.set_normalised(parameterId, position)
                        onFinished: panel.controller.end_gesture(parameterId)
                        onReset: panel.controller.reset_to_default(parameterId)
                    }
                }
            }
            Grid {
                id: padGrid
                x: panel.theme.gutter
                y: padsHeader.y + padsHeader.height + panel.theme.padsHeaderGap
                width: parent.width - 2 * panel.theme.gutter
                columns: Contract.padColumns
                spacing: panel.theme.padGap
                Repeater {
                    model: Contract.bankSize
                    delegate: Item {
                        id: padCell
                        required property int index
                        // Bottom-left first, as on hardware: grid position
                        // `index` (rows from the top) holds this chop.
                        readonly property int chopIndex: panel.bank * Contract.bankSize
                            + (Contract.padRows - 1 - Math.floor(index / Contract.padColumns)) * Contract.padColumns
                            + index % Contract.padColumns
                        width: (padGrid.width - (Contract.padColumns - 1) * padGrid.spacing) / Contract.padColumns
                        height: panel.theme.padHeight
                        CratePad {
                            anchors.fill: parent
                            theme: panel.theme
                            index: padCell.chopIndex
                            chop: padCell.chopIndex < panel.chops.length ? panel.chops[padCell.chopIndex] : null
                            selected: padCell.chopIndex === panel.selected
                            sounding: chop !== null && panel.slotPlayhead(chop.slot) >= 0
                            last: chop !== null && panel.lastSlot === chop.slot
                            // The release goes to the chop the press started,
                            // even if the pads re-ordered under the pointer.
                            property int pressedChop: -1
                            onPressedPad: {
                                pressedChop = padCell.chopIndex
                                panel.pressPad(pressedChop)
                            }
                            onReleasedPad: {
                                panel.releasePad(pressedChop >= 0 ? pressedChop : padCell.chopIndex)
                                pressedChop = -1
                            }
                        }
                    }
                }
            }
        }

        // The selected chop.
        Item {
            id: chopEditor
            x: padsColumn.width + panel.theme.chopEditorInset
            y: panel.theme.lowerTop
            width: parent.width - padsColumn.width - panel.theme.characterWidth - 2 * panel.theme.chopEditorInset
            height: parent.height - 2 * panel.theme.lowerTop

            Item {
                id: chopTitle
                width: parent.width
                height: panel.theme.buttonHeight
                Row {
                    height: parent.height
                    spacing: panel.theme.spaceLg
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.chopSwatchSize
                        height: width
                        radius: panel.theme.radiusXs
                        color: panel.selectedColour
                    }
                    Text {
                        objectName: "crateChopTitle"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.selectedChop !== null ? "CHOP " + Contract.chopNumber(panel.selected) : "NO CHOP"
                        color: panel.theme.ink
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.titleSize
                        font.weight: Font.DemiBold
                    }
                    // The chop's name: double-click to rename it.
                    Item {
                        id: nameBox
                        anchors.verticalCenter: parent.verticalCenter
                        width: renaming ? panel.theme.chopNameWidth : chopName.width
                        height: chopName.implicitHeight + panel.theme.spaceXs
                        property bool renaming: false
                        Text {
                            id: chopName
                            objectName: "crateChopName"
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !parent.renaming
                            width: Math.min(implicitWidth, panel.theme.chopNameWidth)
                            elide: Text.ElideRight
                            text: Contract.chopName(panel.selectedChop)
                            color: panel.theme.subInk
                            font.family: panel.theme.sans
                            font.pixelSize: panel.theme.nameSize
                            MouseArea {
                                anchors.fill: parent
                                enabled: panel.selectedChop !== null && panel.controller.editable
                                onDoubleClicked: {
                                    nameField.text = panel.selectedChop.name
                                    nameBox.renaming = true
                                    nameField.forceActiveFocus()
                                }
                                Kit.HoverTip { theme: panel.theme; text: "Double-click to rename this chop" }
                            }
                        }
                        Rectangle {
                            visible: parent.renaming
                            anchors.fill: parent
                            radius: panel.theme.radiusMd
                            color: panel.theme.well
                            border.color: panel.selectedColour
                            TextInput {
                                id: nameField
                                objectName: "crateChopNameField"
                                anchors.fill: parent
                                anchors.leftMargin: panel.theme.spaceSm
                                anchors.rightMargin: panel.theme.spaceSm
                                verticalAlignment: TextInput.AlignVCenter
                                color: panel.theme.ink
                                font.family: panel.theme.sans
                                font.pixelSize: panel.theme.nameSize
                                clip: true
                                onAccepted: {
                                    panel.crate.rename_chop(panel.selected, text)
                                    nameBox.renaming = false
                                }
                                Keys.onEscapePressed: nameBox.renaming = false
                                onActiveFocusChanged: if (!activeFocus) nameBox.renaming = false
                            }
                        }
                    }
                    Mono {
                        objectName: "crateChopSpan"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.selectedChop !== null
                            ? Contract.milliseconds(panel.selectedChop.length_seconds) + " · "
                              + Contract.noteName(Contract.padKey(panel.selected))
                            : ""
                    }
                }
                Row {
                    anchors.right: parent.right
                    height: parent.height
                    spacing: panel.theme.spaceLg
                    CrateSegs {
                        objectName: "cratePlayMode"
                        anchors.verticalCenter: parent.verticalCenter
                        controller: panel.controller
                        theme: panel.theme
                        accent: panel.selectedColour
                        parameterId: panel.chopIds.mode !== undefined ? panel.chopIds.mode : ""
                        labels: Contract.playModes
                        tileNames: Contract.playModes.map((label, index) => "cratePlayMode:" + index)
                    }
                    Kit.Button {
                        objectName: "crateReverse"
                        anchors.verticalCenter: parent.verticalCenter
                        theme: panel.theme
                        height: panel.theme.chopButtonHeight
                        label: "REVERSE"
                        accent: panel.selectedColour
                        readonly property string parameterId: panel.chopIds.reverse !== undefined ? panel.chopIds.reverse : ""
                        on: parameterId !== "" && panel.controller.revision >= 0
                            && panel.controller.choice(parameterId) === 1
                        active: parameterId !== "" && panel.controller.editable
                        onClicked: panel.controller.set_choice(parameterId, on ? 0 : 1)
                    }
                }
            }

            // Pitch, fine, gain, pan, start, length.
            Row {
                id: chopKnobs
                y: chopTitle.height + panel.theme.blockGap
                width: parent.width
                readonly property real cell: width / Contract.chopKnobs.length
                Repeater {
                    model: Contract.chopKnobs
                    delegate: Item {
                        id: knobCell
                        required property string modelData
                        width: chopKnobs.cell
                        height: knob.implicitHeight
                        Knob {
                            id: knob
                            anchors.horizontalCenter: parent.horizontalCenter
                            label: knobCell.modelData.toUpperCase()
                            parameterId: panel.chopIds[knobCell.modelData] !== undefined ? panel.chopIds[knobCell.modelData] : ""
                            colour: panel.selectedColour
                            formatter: knobCell.modelData === "pan" ? Contract.formatPan : null
                        }
                    }
                }
            }

            // The envelope and the filter.
            Row {
                y: chopKnobs.y + chopKnobs.height + panel.theme.blockGap
                width: parent.width
                height: parent.height - y
                spacing: panel.theme.blockGap
                Rectangle {
                    id: envelopeCard
                    objectName: "crateEnvelopeCard"
                    width: (parent.width - parent.spacing) / 2
                    height: parent.height
                    radius: panel.theme.cardRadius
                    color: panel.theme.cardFill
                    border.color: panel.theme.line
                    Column {
                        x: panel.theme.cardInset
                        y: panel.theme.envelopeCardTop
                        width: parent.width - 2 * panel.theme.cardInset
                        spacing: panel.theme.spaceSm
                        Item {
                            width: parent.width
                            height: envelopeTitle.implicitHeight
                            Caption { id: envelopeTitle; text: "ENVELOPE"; font.pixelSize: panel.theme.labelSize }
                            Mono {
                                objectName: "crateEnvelopeSummary"
                                anchors.right: parent.right
                                readonly property var ids: panel.chopIds
                                text: ids.attack === undefined || panel.controller.revision < 0 ? ""
                                    : "A " + Math.round(panel.controller.value(ids.attack))
                                      + " · H " + Math.round(panel.controller.value(ids.hold))
                                      + " · D " + Math.round(panel.controller.value(ids.decay))
                                      + " · R " + Math.round(panel.controller.value(ids.release)) + " ms"
                            }
                        }
                        CrateEnvelopeView {
                            width: parent.width
                            height: panel.theme.viewHeight
                            controller: panel.controller
                            theme: panel.theme
                            ids: panel.chopIds
                            colour: panel.selectedColour
                        }
                        Row {
                            id: envelopeRow
                            width: parent.width
                            readonly property real cell: width / Contract.envelopeKnobs.length
                            Repeater {
                                model: Contract.envelopeKnobs
                                delegate: Item {
                                    id: envelopeCell
                                    required property string modelData
                                    width: envelopeRow.cell
                                    height: envelopeKnob.implicitHeight
                                    Knob {
                                        id: envelopeKnob
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        size: "sm"
                                        label: envelopeCell.modelData.toUpperCase()
                                        parameterId: panel.chopIds[envelopeCell.modelData] !== undefined ? panel.chopIds[envelopeCell.modelData] : ""
                                        colour: panel.selectedColour
                                    }
                                }
                            }
                        }
                    }
                }
                Rectangle {
                    objectName: "crateFilterCard"
                    width: (parent.width - parent.spacing) / 2
                    height: parent.height
                    radius: panel.theme.cardRadius
                    color: panel.theme.cardFill
                    border.color: panel.theme.line
                    Column {
                        x: panel.theme.cardInset
                        y: panel.theme.filterCardTop
                        width: parent.width - 2 * panel.theme.cardInset
                        spacing: panel.theme.spaceSm
                        Item {
                            width: parent.width
                            height: filterModes.implicitHeight
                            Caption {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "FILTER"
                                font.pixelSize: panel.theme.labelSize
                            }
                            CrateSegs {
                                id: filterModes
                                objectName: "crateFilterMode"
                                anchors.right: parent.right
                                compact: true
                                controller: panel.controller
                                theme: panel.theme
                                accent: panel.selectedColour
                                parameterId: panel.chopIds.filter !== undefined ? panel.chopIds.filter : ""
                                labels: Contract.filterModes
                                tileNames: Contract.filterModes.map((label, index) => "crateFilterMode:" + index)
                            }
                        }
                        CrateFilterView {
                            width: parent.width
                            height: panel.theme.filterCurveHeight
                            controller: panel.controller
                            theme: panel.theme
                            ids: panel.chopIds
                            colour: panel.selectedColour
                        }
                        Row {
                            id: filterRow
                            width: parent.width
                            readonly property real cell: width / Contract.filterKnobs.length
                            Repeater {
                                model: Contract.filterKnobs
                                delegate: Item {
                                    id: filterCell
                                    required property string modelData
                                    width: filterRow.cell
                                    height: filterKnob.implicitHeight
                                    Knob {
                                        id: filterKnob
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        size: "sm"
                                        label: filterCell.modelData === "resonance" ? "RES" : filterCell.modelData.toUpperCase()
                                        parameterId: panel.chopIds[filterCell.modelData] !== undefined ? panel.chopIds[filterCell.modelData] : ""
                                        colour: panel.selectedColour
                                        formatter: filterCell.modelData === "choke" ? Contract.formatChoke : null
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Character.
        Rectangle {
            id: character
            objectName: "crateCharacter"
            x: parent.width - width
            width: panel.theme.characterWidth
            height: parent.height
            color: panel.theme.characterFill
            radius: panel.radius
            // Square off every corner but the lower right.
            Rectangle {
                width: parent.width
                height: parent.radius
                color: parent.color
            }
            Rectangle {
                width: parent.radius
                height: parent.height
                color: parent.color
            }
            Rectangle {
                width: panel.theme.hairline
                height: parent.height
                color: panel.theme.line
            }
            Item {
                id: characterTitle
                x: panel.theme.gutter
                y: panel.theme.lowerTop
                width: parent.width - 2 * panel.theme.gutter
                height: eraName.implicitHeight
                Caption { text: "CHARACTER"; anchors.baseline: eraName.baseline }
                Text {
                    id: eraName
                    objectName: "crateEraName"
                    anchors.right: parent.right
                    readonly property int era: panel.controller.revision >= 0 ? panel.controller.choice("crate.era") : 0
                    text: Contract.eras[Math.max(0, Math.min(Contract.eras.length - 1, era))].name
                    color: panel.theme.accent
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.bodySize
                }
            }
            Grid {
                id: characterKnobs
                x: panel.theme.gutter
                y: characterTitle.y + characterTitle.height + panel.theme.characterGridTop
                width: parent.width - 2 * panel.theme.gutter
                columns: panel.theme.characterColumns
                rowSpacing: panel.theme.characterRowGap
                readonly property real cell: width / columns
                Repeater {
                    model: Contract.characterKnobs
                    delegate: Item {
                        id: characterCell
                        required property string modelData
                        width: characterKnobs.cell
                        height: characterKnob.implicitHeight
                        Knob {
                            id: characterKnob
                            anchors.horizontalCenter: parent.horizontalCenter
                            parameterId: characterCell.modelData
                            formatter: characterCell.modelData === "crate.bits"
                                ? (value => Math.round(value) + "-bit") : null
                        }
                    }
                }
            }
            Text {
                objectName: "crateEraBlurb"
                x: panel.theme.gutter
                width: parent.width - 2 * panel.theme.gutter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: panel.theme.lowerTop
                text: Contract.eras[Math.max(0, Math.min(Contract.eras.length - 1, eraName.era))].blurb
                wrapMode: Text.WordWrap
                lineHeight: panel.theme.blurbLineHeight
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
            }
        }
    }

    // ---- Empty: drop or choose a sample ------------------------------------------------------------

    Rectangle {
        id: dropZone
        objectName: "crateDropZone"
        visible: !panel.hasSample
        x: panel.theme.gutter
        y: header.height + panel.theme.gutter
        width: parent.width - 2 * panel.theme.gutter
        height: parent.height - y - panel.theme.gutter
        radius: panel.theme.cardRadius
        color: dropArea.pressed ? panel.theme.well : panel.theme.dropFill
        border.color: dropArea.containsMouse || drop.containsDrag ? panel.theme.accent : panel.theme.line2
        border.width: panel.theme.dropEdgeWidth
        activeFocusOnTab: visible
        Accessible.role: Accessible.Button
        Accessible.name: "Drop a sample or click to load"
        Keys.onReturnPressed: dropZone.choose()
        Keys.onSpacePressed: dropZone.choose()
        function choose() {
            if (panel.crate !== null && !panel.crate.busy && panel.controller.editable)
                panel.crate.choose_sample()
        }
        Column {
            anchors.centerIn: parent
            spacing: panel.theme.spaceLg
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: panel.crate !== null && panel.crate.busy ? "Loading…" : "Drop a sample or click to load"
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.heroSize
                font.weight: Font.Medium
            }
            Text {
                objectName: "crateDropHint"
                anchors.horizontalCenter: parent.horizontalCenter
                text: panel.crate !== null && panel.crate.message !== "" ? panel.crate.message
                    : "A break or a loop: Crate cuts it at each hit and lays the chops out on the pads"
                color: panel.crate !== null && panel.crate.message !== "" && !panel.crate.busy
                    ? panel.theme.warning : panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.bodySize
            }
        }
        MouseArea {
            id: dropArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: panel.crate !== null && !panel.crate.busy && panel.controller.editable
            cursorShape: Qt.PointingHandCursor
            onClicked: dropZone.choose()
        }
    }

    // Audio dropped anywhere on the panel loads as the sample.
    Rectangle {
        objectName: "crateDropHighlight"
        visible: drop.containsDrag && panel.hasSample
        anchors.fill: parent
        radius: panel.radius
        color: Qt.rgba(panel.theme.accent.r, panel.theme.accent.g, panel.theme.accent.b, panel.theme.dropWashAlpha)
        border.color: panel.theme.accent
        border.width: panel.theme.strokeBold
        z: 10
        Text {
            anchors.centerIn: parent
            text: "Drop to load this sample"
            color: panel.theme.accent
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.heroSize
        }
    }
    DropArea {
        id: drop
        objectName: "crateDropArea"
        anchors.fill: parent
        z: 11
        onEntered: dragEvent => {
            dragEvent.accepted = dragEvent.hasUrls && dragEvent.urls.length > 0
                && Contract.isAudio(dragEvent.urls[0]) && panel.crate !== null
        }
        onDropped: dropEvent => {
            if (dropEvent.hasUrls && dropEvent.urls.length > 0 && Contract.isAudio(dropEvent.urls[0])) {
                panel.loadSample(dropEvent.urls[0])
                dropEvent.acceptProposedAction()
            }
        }
    }
}
