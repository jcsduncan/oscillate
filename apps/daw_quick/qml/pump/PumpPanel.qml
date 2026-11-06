pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "PumpShapes.js" as Shapes

// The Pump device panel (ADR-0058): a volume shaper drawn, not dialled. The
// shape editor leads, one cycle of volume in coral drawn over the kick in
// slate (or the cycle's input and output), with the volume as heard and an
// amber playhead while it plays; tabs above it pick the slot (A to D) and,
// in Three Band, the band being drawn, and a tool row paints, ramps, stamps
// and transforms it. Beside it the shape library and the slots' pattern;
// on the right the trigger (tempo, the kick in the key, or its notes) with
// what each needs; under it what pumps (full, the lows, or three bands, and
// which part of the stereo image) and the output with its gain reduction.
// The shapes are the effect's document (pump.hpp names the curves); every
// other control is a parameter. Everything reads and writes through
// `controller` (NativeSynthDeviceController for Pump) and its document
// model; nothing about the project or the audio lives in QML.
Rectangle {
    id: panel
    required property var controller
    // The sidechain key, from the effect's context.
    property bool keyed: false
    property string keyName: "the sidechain"
    property PumpTheme theme: PumpTheme {}
    readonly property var doc: controller.document

    readonly property real margin: theme.spaceLg
    readonly property real gap: theme.cardGap
    readonly property int revision: controller.revision
    // The reduction meters' full scale, dB.
    readonly property real reductionRangeDb: 24
    // Learn takes a note still playing after this many polls.
    readonly property int learnHoldPolls: 20

    // Live state (layout in pump.hpp), polled while visible.
    property var tele: ({ live: false })
    readonly property bool playing: tele.live === true
    readonly property real phase: playing ? tele.phase : -1
    readonly property real gainDb: playing ? tele.gain_db : 0
    readonly property real gainMidDb: playing && tele.gain_mid_db !== undefined ? tele.gain_mid_db : 0
    readonly property real gainHighDb: playing && tele.gain_high_db !== undefined ? tele.gain_high_db : 0
    readonly property int playingStep: playing && tele.step !== undefined ? tele.step : -1
    // The slot playing now (0..3), -1 while morphing or stopped.
    readonly property int playingSlot: playing && tele.slot !== undefined ? tele.slot : -1
    readonly property int lastNote: tele.last_note !== undefined ? tele.last_note : -1
    // Rounded so float jitter never repaints the editor's scale.
    readonly property real cycleSeconds: playing ? Math.round(tele.cycle_seconds * 1000) / 1000 : 0
    // The key's last cycle, the heard curve and the before and after change
    // once a cycle or on an edit, not every poll: they are replaced only
    // when their values do.
    property var capturedKick: []
    property var heardCurve: []
    property var inputCycle: []
    property var outputCycle: []
    readonly property bool keyHeard: playing ? tele.keyed === true : keyed

    readonly property int trigger: revision >= 0 ? controller.choice("pump.trigger") : 0
    readonly property int bandMode: revision >= 0 ? controller.choice("pump.band") : 0
    readonly property bool threeBand: bandMode === 2
    readonly property int slotMode: revision >= 0 ? controller.choice("pump.slot") : 0
    readonly property int patternLength: revision >= 0 ? Math.round(controller.value("pump.pattern")) : 1
    // The engine's cycle in the song's meter while playing; a 4/4 guess
    // from the rate before then.
    readonly property real cycleBeats: playing && tele.cycle_beats > 0 ? tele.cycle_beats
        : revision >= 0
        ? Shapes.cycleBeats(controller.choice("pump.rate"), controller.choice("pump.feel")) : 1

    // What the editor draws: a slot (0..3) and, in Three Band, a band.
    property int editSlot: 0
    property int editBand: 0
    readonly property int shownBand: threeBand ? editBand : 0
    readonly property string editCurveId: Shapes.curveIdFor(editSlot, shownBand)
    // Bumped once the followed curves' defaults are refreshed after an edit.
    property int defaultsRevision: 0
    readonly property bool editHas: doc.curves_revision >= 0 && doc.has_curve(editCurveId)
    readonly property bool editFollows: !editHas && editCurveId !== Shapes.curveId
    readonly property string followsLabel: !editFollows ? ""
        : shownBand > 0 && doc.curves_revision >= 0 && doc.has_curve(Shapes.curveIdFor(editSlot, 0))
        ? "= " + Shapes.slotNames[editSlot] + " · TAKES THE SLOT'S SHAPE"
        : "= A · EDIT TO MAKE IT ITS OWN"
    readonly property var points: curve.points
    readonly property int shapeIndex: Shapes.match(points)

    // The kick to draw against: the engine's capture of the last cycle,
    // scaled to its own peak, or a typical kick at 120 BPM before anything
    // plays.
    readonly property var kick: {
        const captured = playing ? capturedKick : []
        let top = 0
        for (let i = 0; i < captured.length; ++i)
            top = Math.max(top, captured[i])
        if (top > 1e-3) {
            const scaled = []
            for (let i = 0; i < captured.length; ++i)
                scaled.push(captured[i] / top)
            return scaled
        }
        return Shapes.previewKick(cycleBeats * 0.5, 256)
    }
    readonly property string kickLabel: keyHeard ? "KICK · " + keyName.toUpperCase()
        : playing ? "INPUT" : "KICK · PREVIEW"

    // The display's readout: the control last touched, or a curve gesture.
    property string gestureName: ""
    property string gestureValue: ""
    // MIDI Learn: armed, waiting for a note on the keying tracks.
    property bool learning: false
    property int learnFrom: -1
    property int learnPolls: 0
    // The library shape Stamp uses: the last tile picked.
    property int stampIndex: 3
    // The name field: "", "save", "export" or "import".
    property string naming: ""

    objectName: "pumpPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.caseRadius
    gradient: Gradient {
        GradientStop { position: 0; color: panel.theme.caseTop }
        GradientStop { position: panel.theme.caseMidStop; color: panel.theme.caseColor }
        GradientStop { position: 1; color: panel.theme.caseBottom }
    }
    border.color: panel.theme.seam
    Accessible.role: Accessible.Pane
    Accessible.name: "Pump volume shaper"

    Component.onCompleted: refreshDefaults()
    Connections {
        target: panel.doc
        function onCurves_revisionChanged() { panel.refreshDefaults() }
    }
    // A slot without its own curve follows A, and a band without its own
    // follows its slot: register what each shows and starts from.
    function refreshDefaults() {
        const a = doc.curve(Shapes.curveId)
        for (let s = 1; s < 4; ++s)
            doc.set_curve_default(Shapes.curveIdFor(s, 0), a)
        for (let s = 0; s < 4; ++s) {
            const main = doc.curve(Shapes.curveIdFor(s, 0))
            for (let b = 1; b < 3; ++b)
                doc.set_curve_default(Shapes.curveIdFor(s, b), main)
        }
        ++defaultsRevision
    }

    function sameList(a, b) {
        if (a.length !== b.length)
            return false
        for (let i = 0; i < a.length; ++i)
            if (a[i] !== b[i])
                return false
        return true
    }
    function listOf(name) {
        return tele.live === true && tele[name] !== undefined ? tele[name] : []
    }
    function poll() {
        tele = controller.telemetry()
        const kick = listOf("kick")
        if (!sameList(kick, capturedKick))
            capturedKick = kick
        // The heard line is the playing slot's low band: shown only over it.
        const heard = playingSlot === editSlot && shownBand === 0 ? listOf("applied") : []
        if (!sameList(heard, heardCurve))
            heardCurve = heard
        const input = listOf("input")
        if (!sameList(input, inputCycle))
            inputCycle = input
        const output = listOf("output")
        if (!sameList(output, outputCycle))
            outputCycle = output
        if (learning && lastNote >= 0) {
            // A new note, or the same one still playing a moment later.
            if (lastNote !== learnFrom || ++learnPolls >= learnHoldPolls)
                learnNote(lastNote)
        }
    }
    // A curve gesture's readout, until a control is touched again.
    function touch(name, value) {
        theme.lastTouched = ""
        gestureName = name
        gestureValue = value
    }
    Connections {
        target: panel.theme
        function onLastTouchedChanged() {
            if (panel.theme.lastTouched !== "") {
                panel.gestureName = ""
                panel.gestureValue = ""
            }
        }
    }
    function readoutName() {
        const id = theme.lastTouched
        if (id === "")
            return gestureName !== "" ? gestureName : "TOUCH A CONTROL"
        const info = controller.parameter(id)
        if (info.name === undefined)
            return ""
        return (info.group === info.name ? info.name : info.group + " · " + info.name).toUpperCase()
    }
    function readoutValue() {
        const id = theme.lastTouched
        if (id === "")
            return gestureName !== "" ? gestureValue : "—"
        if (revision < 0)
            return ""
        if (id === "pump.note_low" || id === "pump.note_high")
            return Shapes.noteName(Math.round(controller.value(id)))
        return controller.display(id)
    }

    // ---- Shape operations, on the curve being edited ---------------------------------

    function setShape(points) {
        doc.set_curve(editCurveId, points)
    }
    function curveName() {
        return Shapes.slotNames[editSlot] + (threeBand ? " · " + Shapes.bandNames[shownBand] : "")
    }
    function fitToKick() {
        const release = Shapes.releaseAt(kick, 0.1)
        if (release < 0)
            return
        setShape(Shapes.fitted(release))
        touch("SHAPE " + curveName(), "Fitted to the kick")
    }
    function invertShape() {
        setShape(Shapes.flipped(points))
        touch("SHAPE " + curveName(), "Inverted")
    }
    function mirrorShape() {
        setShape(Shapes.mirrored(points))
        touch("SHAPE " + curveName(), "Mirrored")
    }
    function shiftShape(direction) {
        setShape(Shapes.shifted(points, direction / curve.cells))
        touch("SHAPE " + curveName(), direction > 0 ? "Shifted a step later" : "Shifted a step earlier")
    }
    function randomShape(seed) {
        setShape(Shapes.randomized(curve.cells, seed))
        touch("SHAPE " + curveName(), "Randomized")
    }
    function copyShape() {
        controller.set_clipboard_text(Shapes.clipboardText(points))
        touch("SHAPE " + curveName(), "Copied")
    }
    function pasteShape() {
        const pasted = Shapes.parsedClipboard(controller.clipboard_text())
        if (pasted === null || pasted.length < 2) {
            touch("PASTE", "The clipboard holds no shape")
            return false
        }
        setShape(pasted)
        touch("SHAPE " + curveName(), "Pasted")
        return true
    }
    // Back to following: the slot's shape for a band, A's for a slot.
    function resetCurve() {
        if (editCurveId === Shapes.curveId || !doc.has_curve(editCurveId))
            return
        doc.remove_curve(editCurveId)
        touch("SHAPE " + curveName(), shownBand > 0 ? "Follows the slot" : "Follows A")
    }
    function selectSlot(slot) {
        editSlot = slot
        touch("EDITING", "Slot " + Shapes.slotNames[slot]
            + (doc.has_curve(Shapes.curveIdFor(slot, 0)) || slot === 0 ? "" : " · follows A"))
    }
    function selectBand(band) {
        editBand = band
        touch("EDITING", Shapes.bandNames[band] + " band of " + Shapes.slotNames[editSlot])
    }
    // MIDI Learn: the keying tracks' note becomes the whole range.
    function learnNote(note) {
        learning = false
        if (note < 0)
            return
        controller.set_value("pump.note_low", note)
        controller.set_value("pump.note_high", note)
        touch("LEARN", "Triggers on " + Shapes.noteName(note))
    }
    function armLearn() {
        if (learning) {
            learning = false
            return
        }
        learning = true
        learnFrom = lastNote
        learnPolls = 0
        touch("LEARN", "Play the kick note")
    }
    // A pattern step's slot, cycled forward or back.
    function cycleStep(step, direction) {
        const id = "pump.step." + (step + 1)
        controller.set_choice(id, (controller.choice(id) + direction + 4) % 4)
        theme.lastTouched = id
    }

    // ---- Presets: the factory library, the user's own, and their files ---------------

    readonly property var userPresets: controller.user_presets !== undefined ? controller.user_presets : []
    function openPresets() {
        if (!controller.editable)
            return
        const items = []
        const categories = controller.preset_categories
        const presets = controller.presets
        for (let c = 0; c < categories.length; ++c) {
            items.push({ label: categories[c], value: -1, header: true })
            for (let i = 0; i < presets.length; ++i)
                if (presets[i].category === categories[c])
                    items.push({ label: presets[i].name, value: presets[i].index })
        }
        items.push({ label: "User", value: -1, header: true })
        for (let u = 0; u < userPresets.length; ++u)
            items.push({ label: userPresets[u].name, value: "user:" + userPresets[u].name })
        items.push({ label: "Save as…", value: "save" })
        items.push({ label: "Import…", value: "import" })
        items.push({ label: "Export…", value: "export" })
        if (userPresets.length > 0)
            items.push({ label: "Delete…", value: "delete" })
        menu.open(display, items, controller.preset_index, theme.accent, value => presetPicked(value))
    }
    function presetPicked(value) {
        if (typeof value === "number") {
            controller.apply_preset(value)
        } else if (value.indexOf("user:") === 0) {
            controller.apply_user_preset(value.substring(5))
        } else if (value === "delete") {
            const items = [{ label: "Delete a user preset", value: "", header: true }]
            for (let u = 0; u < userPresets.length; ++u)
                items.push({ label: userPresets[u].name, value: userPresets[u].name })
            menu.open(display, items, "", theme.warning, name => {
                controller.delete_user_preset(name)
                touch("PRESET", "Deleted " + name)
            })
        } else {
            startNaming(value)
        }
    }
    function startNaming(mode) {
        naming = mode
        nameInput.text = mode === "save" ? controller.preset_name : ""
        nameInput.selectAll()
        nameInput.forceActiveFocus()
    }
    function confirmName(text) {
        const mode = naming
        naming = ""
        const value = text.trim()
        if (value === "")
            return false
        let done = false
        if (mode === "save")
            done = controller.save_user_preset(value)
        else if (mode === "export")
            done = controller.export_preset(value)
        else if (mode === "import")
            done = controller.import_preset(value)
        touch("PRESET", (done ? "" : "Could not ") + (mode === "save" ? "save " : mode + " ") + value)
        return done
    }

    readonly property int linkedCount: controller.linked_count !== undefined ? controller.linked_count : 1

    component Caption: Text {
        color: panel.theme.inkFaint
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.typeCaption
        font.letterSpacing: panel.theme.captionSpacing
    }
    component Heading: Text {
        color: panel.theme.ink
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.typeCaption
        font.bold: true
        font.letterSpacing: panel.theme.headingSpacing
    }
    component Pill: Rectangle {
        id: pill
        property string label: ""
        property bool on: false
        property bool dim: false
        property color ink: panel.theme.shape
        signal activated
        signal rightClicked
        width: pillText.implicitWidth + panel.theme.pillPad
        height: panel.theme.pillHeight
        radius: panel.theme.radiusLg
        color: on ? panel.theme.shapeOn : panel.theme.well
        border.color: on ? ink : panel.theme.wellEdge
        opacity: !panel.controller.editable ? panel.theme.pillDisabledOpacity : dim ? panel.theme.pillDimOpacity : 1
        Accessible.role: Accessible.Button
        Kit.HoverTip { theme: panel.theme }
        Accessible.name: label
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label
            color: pill.on ? pill.ink : panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeCaption
            font.bold: true
            font.letterSpacing: panel.theme.pillSpacing
        }
        MouseArea {
            anchors.fill: parent
            enabled: panel.controller.editable
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton)
                    pill.rightClicked()
                else
                    pill.activated()
            }
        }
    }
    component Cell: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
    }
    component Seg: Kit.Segment {
        controller: panel.controller
        theme: panel.theme
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.theme.spaceXl
        y: 0
        width: panel.width - 2 * panel.theme.spaceXl
        height: panel.theme.headerHeight

        Text {
            id: wordmark
            y: panel.theme.wordmarkY
            text: "PUMP"
            color: panel.theme.wordmark
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.wordmarkSize
            font.weight: Font.Light
            font.letterSpacing: panel.theme.wordmarkSpacing
        }
        Text {
            id: subtitle
            y: wordmark.y + wordmark.height + panel.theme.subtitleGap
            text: "VOLUME SHAPER · SIDECHAIN"
            color: panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.subtitleSpacing
        }
        Rectangle {
            y: subtitle.y + subtitle.height + panel.theme.ruleGap
            width: Math.max(wordmark.implicitWidth, subtitle.implicitWidth)
            height: panel.theme.strokeBold
            radius: height / 2
            color: panel.theme.shape
        }

        Row {
            x: panel.theme.browserX
            y: panel.theme.browserY
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "pumpPresetPrevious"
                direction: -1
            }
            Rectangle {
                id: display
                objectName: "pumpPresetDisplay"
                width: panel.theme.presetWidth
                height: panel.theme.presetHeight
                radius: panel.theme.presetRadius
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.accent : panel.theme.wellEdge
                border.width: activeFocus ? panel.theme.focusStroke : panel.theme.hairline
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Preset"
                Accessible.description: panel.controller.preset_name
                    + (panel.controller.preset_modified ? ", edited" : "")
                Keys.onReturnPressed: panel.openPresets()
                Keys.onSpacePressed: panel.openPresets()
                Text {
                    x: panel.theme.presetTextX
                    y: panel.theme.presetCaptionY
                    text: (panel.controller.preset_category + " · FACTORY").toUpperCase()
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.capsSpacing
                }
                Text {
                    objectName: "pumpPresetName"
                    x: panel.theme.presetTextX
                    y: panel.theme.presetNameY
                    width: panel.theme.presetNameWidth
                    text: panel.controller.preset_name
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
                    elide: Text.ElideRight
                }
                Rectangle {
                    x: panel.theme.presetDividerX
                    y: 0
                    width: panel.theme.hairline
                    height: parent.height
                    color: panel.theme.plateEdge
                }
                Text {
                    objectName: "pumpReadout"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetCaptionY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutName()
                    color: panel.theme.inkFaint
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.capsTightSpacing
                    elide: Text.ElideRight
                }
                Text {
                    objectName: "pumpReadoutValue"
                    x: panel.theme.presetReadoutX
                    y: panel.theme.presetValueY
                    width: display.width - x - panel.theme.presetReadoutRight
                    text: panel.readoutValue()
                    color: panel.theme.shape
                    font.family: panel.theme.mono
                    font.pixelSize: panel.theme.typeLabel
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.presetChevronMargin
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    onClicked: {
                        display.forceActiveFocus()
                        panel.openPresets()
                    }
                }
                // Save as, export and import: a name or path typed in place.
                Rectangle {
                    objectName: "pumpNameField"
                    anchors.fill: parent
                    visible: panel.naming !== ""
                    radius: panel.theme.presetRadius
                    color: panel.theme.well
                    border.color: panel.theme.accent
                    Text {
                        x: panel.theme.presetTextX
                        y: panel.theme.presetCaptionY
                        text: panel.naming === "save" ? "SAVE AS · NAME"
                            : panel.naming === "export" ? "EXPORT · FILE PATH" : "IMPORT · FILE PATH"
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeMicro
                        font.bold: true
                        font.letterSpacing: panel.theme.capsSpacing
                    }
                    TextInput {
                        id: nameInput
                        objectName: "pumpNameInput"
                        x: panel.theme.presetTextX
                        y: panel.theme.presetValueY
                        width: parent.width - panel.theme.nameFieldRight
                        color: panel.theme.ink
                        selectionColor: panel.theme.accentSoft
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeSubtitle
                        clip: true
                        Keys.onReturnPressed: panel.confirmName(text)
                        Keys.onEnterPressed: panel.confirmName(text)
                        Keys.onEscapePressed: panel.naming = ""
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: panel.theme.spaceLg
                        anchors.verticalCenter: parent.verticalCenter
                        text: "↵ OK · ESC"
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                    }
                }
            }
            Kit.PresetArrow {
                controller: panel.controller
                theme: panel.theme
                objectName: "pumpPresetNext"
                direction: 1
            }
        }

        // Linked instances: one shape across every Pump in the group.
        Pill {
            id: linkPill
            objectName: "pumpLink"
            anchors.right: status.left
            anchors.rightMargin: panel.theme.spaceLg
            y: panel.theme.linkY
            visible: panel.linkedCount > 1
            on: true
            ink: panel.theme.time
            label: "LINKED · " + panel.linkedCount
            Accessible.description: "This shape is shared by " + panel.linkedCount + " Pumps"
            onActivated: menu.open(linkPill, [{ label: "Shared by " + panel.linkedCount + " Pumps", value: "",
                                                header: true },
                                              { label: "Unlink this Pump", value: "unlink" }], "",
                                   panel.theme.time, value => {
                                       if (value === "unlink") {
                                           panel.controller.unlink()
                                           panel.touch("LINK", "This Pump has its own shapes")
                                       }
                                   })
        }
        Kit.StatusBadge {
            id: status
            objectName: "pumpStatus"
            controller: panel.controller
            theme: panel.theme
            anchors.right: parent.right
            y: panel.theme.statusY
        }
    }

    // ---- Body: library and pattern | editor | trigger, then band | output ------------

    readonly property real bodyY: theme.bodyY
    readonly property real sideWidth: theme.sideWidth
    readonly property real triggerWidth: theme.triggerWidth
    readonly property real lowerHeight: theme.lowerHeight
    readonly property real upperHeight: height - bodyY - margin - gap - lowerHeight
    readonly property real middleX: margin + sideWidth + gap
    readonly property real middleWidth: width - 2 * margin - sideWidth - triggerWidth - 2 * gap
    readonly property real rightX: middleX + middleWidth + gap
    readonly property real lowerY: bodyY + upperHeight + gap
    readonly property real libraryHeight: theme.libraryHeight

    Kit.Card {
        id: library
        theme: panel.theme
        title: "SHAPES"
        note: curve.tool === "stamp" ? "PICK WHAT STAMP STAMPS" : ""
        x: panel.margin
        y: panel.bodyY
        width: panel.sideWidth
        height: panel.libraryHeight
        Grid {
            x: panel.theme.cardInset
            y: panel.theme.cardBodyY
            columns: 2
            spacing: panel.theme.spaceSm
            Repeater {
                model: Shapes.shapes.length
                delegate: Rectangle {
                    id: tile
                    required property int index
                    readonly property var shape: Shapes.shapes[index]
                    // Points the thumbnail's line is drawn through.
                    readonly property int thumbSamples: 60
                    readonly property bool on: curve.tool === "stamp" ? panel.stampIndex === index
                                                                      : panel.shapeIndex === index
                    objectName: "pumpShape:" + index
                    width: (library.width - 2 * panel.theme.cardInset - panel.theme.spaceSm) / 2
                    height: panel.theme.shapeTileHeight
                    radius: panel.theme.wellRadius
                    color: on ? panel.theme.shapeOn : panel.theme.well
                    border.color: on ? panel.theme.shape : panel.theme.wellEdge
                    Accessible.role: Accessible.Button
                    Kit.HoverTip { theme: panel.theme }
                    Accessible.name: shape.name + " shape"
                    Canvas {
                        x: panel.theme.spaceXs
                        y: panel.theme.spaceXs
                        width: tile.width - 2 * panel.theme.spaceXs
                        height: panel.theme.thumbHeight
                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width, h = height
                            const gains = Shapes.sampled(tile.shape.points, tile.thumbSamples)
                            ctx.beginPath()
                            for (let i = 0; i <= tile.thumbSamples; ++i) {
                                const t = i / tile.thumbSamples
                                const y = h - panel.theme.thumbInset - gains[i] * (h - 2 * panel.theme.thumbInset)
                                if (i === 0) ctx.moveTo(t * w, y); else ctx.lineTo(t * w, y)
                            }
                            ctx.strokeStyle = panel.theme.shape
                            ctx.lineWidth = panel.theme.thumbStroke
                            ctx.stroke()
                            ctx.lineTo(w, 0)
                            ctx.lineTo(0, 0)
                            ctx.closePath()
                            ctx.fillStyle = panel.theme.shapeSoft
                            ctx.fill()
                        }
                    }
                    Text {
                        x: panel.theme.spaceSm
                        y: panel.theme.shapeNameY
                        width: tile.width - 2 * panel.theme.spaceSm
                        text: tile.shape.name
                        color: tile.on ? panel.theme.shape : panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeCaption
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: panel.controller.editable
                        onClicked: {
                            panel.stampIndex = tile.index
                            if (curve.tool === "stamp") {
                                panel.touch("STAMP", tile.shape.name)
                                return
                            }
                            panel.setShape(tile.shape.points)
                            panel.touch("SHAPE " + panel.curveName(), tile.shape.name)
                        }
                    }
                }
            }
        }
    }

    Kit.Card {
        id: patternCard
        objectName: "pumpPatternCard"
        theme: panel.theme
        title: "SLOTS"
        note: "WHICH SHAPE PLAYS"
        x: panel.margin
        y: panel.bodyY + panel.libraryHeight + panel.gap
        width: panel.sideWidth
        height: panel.height - y - panel.margin
        Caption {
            x: panel.theme.spaceLg
            y: panel.theme.playCaptionY
            text: "PLAY"
        }
        Kit.Dropdown {
            x: panel.theme.playDropdownX
            y: panel.theme.playDropdownY
            width: patternCard.width - panel.theme.playDropdownX - panel.theme.spaceLg
            controller: panel.controller
            theme: panel.theme
            parameterId: "pump.slot"
            menu: menu
        }
        Cell {
            x: panel.theme.spaceLg
            y: panel.theme.morphY
            parameterId: "pump.morph"
            diameter: panel.theme.smallKnobDiameter
            inactive: panel.slotMode !== 5
        }
        Caption {
            x: panel.theme.patternX
            y: panel.theme.patternCaptionY
            text: "PATTERN · CYCLES"
        }
        Kit.Stepper {
            x: panel.theme.patternX
            y: panel.theme.patternStepperY
            width: patternCard.width - panel.theme.patternX - panel.theme.spaceLg
            controller: panel.controller
            theme: panel.theme
            parameterId: "pump.pattern"
            plain: true
            opacity: panel.slotMode === 0 ? 1 : panel.theme.stepperInactiveOpacity
        }
        Grid {
            id: stepGrid
            x: panel.theme.spaceLg
            y: panel.theme.stepGridY
            columns: 4
            spacing: panel.theme.stepGap
            Repeater {
                model: 8
                delegate: Rectangle {
                    id: stepChip
                    required property int index
                    readonly property int slot: panel.revision >= 0
                        ? panel.controller.choice("pump.step." + (index + 1)) : 0
                    readonly property bool used: index < panel.patternLength && panel.slotMode === 0
                    readonly property bool now: used && panel.playingStep === index
                    objectName: "pumpStep:" + (index + 1)
                    width: (patternCard.width - 2 * panel.theme.spaceLg
                            - (stepGrid.columns - 1) * stepGrid.spacing) / stepGrid.columns
                    height: panel.theme.stepHeight
                    radius: panel.theme.radiusLg
                    color: now ? panel.theme.shapeOn : panel.theme.well
                    border.color: now ? panel.theme.time : used ? panel.theme.wellEdge : panel.theme.seam
                    border.width: now ? panel.theme.stepNowStroke : panel.theme.hairline
                    opacity: used ? 1 : panel.theme.stepUnusedOpacity
                    Accessible.role: Accessible.Button
                    Kit.HoverTip { theme: panel.theme }
                    Accessible.name: "Pattern step " + (index + 1) + ", slot " + Shapes.slotNames[slot]
                    Text {
                        x: panel.theme.stepNumberX
                        y: panel.theme.stepNumberY
                        text: stepChip.index + 1
                        color: panel.theme.inkFaint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeMicro
                    }
                    Text {
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: panel.theme.stepLetterDrop
                        text: Shapes.slotNames[stepChip.slot]
                        color: stepChip.now ? panel.theme.time : panel.theme.shape
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeSubtitle
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: panel.controller.editable
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => panel.cycleStep(stepChip.index,
                                                            mouse.button === Qt.RightButton ? -1 : 1)
                    }
                }
            }
        }
        Caption {
            x: panel.theme.spaceLg
            y: panel.theme.slotNoteY
            width: patternCard.width - 2 * panel.theme.spaceLg
            wrapMode: Text.WordWrap
            lineHeight: panel.theme.noteLineHeight
            text: panel.slotMode === 0 ? "Each cycle plays its step's slot. Click a step for the next slot."
                : panel.slotMode === 5 ? "Morph blends A into B; automate it to evolve the pump."
                : "One slot plays every cycle; automate Play to switch."
        }
    }

    Kit.Card {
        id: shapeCard
        theme: panel.theme
        title: "SHAPE"
        x: panel.middleX
        y: panel.bodyY
        width: panel.middleWidth
        height: panel.upperHeight

        // Row one: which curve is drawn, the view, the grid.
        Row {
            x: panel.theme.slotTabsX
            y: panel.theme.tabRowY
            spacing: panel.theme.tabGap
            Repeater {
                model: 4
                delegate: Pill {
                    id: slotTab
                    required property int index
                    readonly property bool own: panel.doc.curves_revision >= 0
                        && (index === 0 || panel.doc.has_curve(Shapes.curveIdFor(index, 0)))
                    objectName: "pumpSlot:" + Shapes.slotNames[index]
                    width: panel.theme.slotTabWidth
                    label: Shapes.slotNames[index] + (own ? "" : "=")
                    on: panel.editSlot === index
                    dim: !own
                    ink: panel.playingSlot === index ? panel.theme.time : panel.theme.shape
                    border.color: panel.playingSlot === index ? panel.theme.time
                        : on ? panel.theme.shape : panel.theme.wellEdge
                    Accessible.name: "Edit slot " + Shapes.slotNames[index]
                    onActivated: panel.selectSlot(index)
                }
            }
        }
        Row {
            x: panel.theme.bandTabsX
            y: panel.theme.tabRowY
            spacing: panel.theme.tabGap
            visible: panel.threeBand
            Repeater {
                model: 3
                delegate: Pill {
                    id: bandTab
                    required property int index
                    readonly property bool own: panel.doc.curves_revision >= 0
                        && (index === 0 || panel.doc.has_curve(Shapes.curveIdFor(panel.editSlot, index)))
                    objectName: "pumpBandTab:" + ["low", "mid", "high"][index]
                    width: panel.theme.bandTabWidth
                    label: Shapes.bandNames[index]
                    on: panel.editBand === index
                    dim: !own
                    Accessible.name: "Edit the " + Shapes.bandNames[index].toLowerCase() + " band"
                    onActivated: panel.selectBand(index)
                }
            }
        }
        Pill {
            objectName: "pumpResetCurve"
            x: panel.threeBand ? panel.theme.resetBandX : panel.theme.bandTabsX
            y: panel.theme.tabRowY
            visible: panel.editHas && panel.editCurveId !== Shapes.curveId
            label: panel.shownBand > 0 ? "FOLLOW SLOT" : "FOLLOW A"
            onActivated: panel.resetCurve()
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceLg
            y: panel.theme.tabRowY
            spacing: panel.theme.spaceSm
            Pill {
                objectName: "pumpDisplay"
                label: curve.display === "io" ? "IN · OUT" : "KICK"
                on: curve.display === "io"
                ink: panel.theme.heard
                onActivated: {
                    curve.display = curve.display === "io" ? "kick" : "io"
                    panel.touch("VIEW", curve.display === "io" ? "Input and output" : "The kick")
                }
            }
            Caption {
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: panel.theme.spaceXs
                text: "GRID"
            }
            Row {
                spacing: 0
                Repeater {
                    model: [["1/8", 8], ["1/16", 16], ["1/32", 32], ["OFF", 0]]
                    delegate: Rectangle {
                        id: gridTile
                        required property var modelData
                        required property int index
                        readonly property bool on: curve.grid === modelData[1]
                        objectName: "pumpGrid:" + modelData[1]
                        width: panel.theme.gridTileWidth
                        height: panel.theme.pillHeight
                        color: on ? panel.theme.shapeOn : panel.theme.well
                        border.color: panel.theme.wellEdge
                        Text {
                            anchors.centerIn: parent
                            text: gridTile.modelData[0]
                            color: gridTile.on ? panel.theme.shape : panel.theme.inkDim
                            font.family: panel.theme.sans
                            font.pixelSize: panel.theme.typeCaption
                            font.bold: true
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: curve.grid = gridTile.modelData[1]
                        }
                    }
                }
            }
        }

        // Row two: tools, then what they do to the whole curve.
        Row {
            x: panel.theme.spaceLg
            y: panel.theme.toolRowY
            spacing: panel.theme.spaceXs
            Repeater {
                model: [["select", "SELECT"], ["pencil", "PENCIL"], ["line", "LINE"], ["stamp", "STAMP"]]
                delegate: Pill {
                    id: toolPill
                    required property var modelData
                    objectName: "pumpTool:" + modelData[0]
                    label: modelData[1]
                    on: curve.tool === modelData[0]
                    onActivated: {
                        curve.tool = toolPill.modelData[0]
                        panel.touch("TOOL", toolPill.modelData[1].charAt(0) + toolPill.modelData[1].slice(1).toLowerCase())
                    }
                }
            }
            Item { width: panel.theme.toolGroupGap; height: 1 }
            Pill { objectName: "pumpFlip"; label: "INVERT"; onActivated: panel.invertShape() }
            Pill { objectName: "pumpMirror"; label: "MIRROR"; onActivated: panel.mirrorShape() }
            Pill { objectName: "pumpShiftLeft"; label: "◀"; Accessible.name: "Shift a step earlier"; onActivated: panel.shiftShape(-1) }
            Pill { objectName: "pumpShiftRight"; label: "▶"; Accessible.name: "Shift a step later"; onActivated: panel.shiftShape(1) }
            Pill { objectName: "pumpRandom"; label: "RANDOM"; onActivated: panel.randomShape(Date.now()) }
            Item { width: panel.theme.toolGroupGap; height: 1 }
            Pill { objectName: "pumpCopy"; label: "COPY"; onActivated: panel.copyShape() }
            Pill { objectName: "pumpPaste"; label: "PASTE"; onActivated: panel.pasteShape() }
            Item { width: panel.theme.toolGroupGap; height: 1 }
            Pill { objectName: "pumpFit"; label: "FIT TO KICK"; on: true; onActivated: panel.fitToKick() }
            Pill {
                objectName: "pumpClear"
                label: "CLEAR"
                onActivated: {
                    panel.setShape(Shapes.shapes[0].points)
                    panel.touch("SHAPE " + panel.curveName(), "Classic Pump")
                }
            }
        }
        PumpCurve {
            id: curve
            panel: panel
            x: panel.theme.cardInset
            y: panel.theme.curveY
            width: shapeCard.width - 2 * panel.theme.cardInset
            height: shapeCard.height - panel.theme.curveY - panel.theme.curveFoot
            curveId: panel.editCurveId
            follows: panel.editFollows
            followsLabel: panel.followsLabel
            depthId: ["pump.depth", "pump.depth_mid", "pump.depth_high"][panel.shownBand]
            kick: panel.kick
            input: panel.inputCycle
            output: panel.outputCycle
            applied: panel.heardCurve
            phase: panel.phase
            cycleBeats: panel.cycleBeats
            cycleSeconds: panel.cycleSeconds
            gainDb: [panel.gainDb, panel.gainMidDb, panel.gainHighDb][panel.shownBand]
            stampShape: Shapes.shapes[panel.stampIndex].points
        }
        Caption {
            objectName: "pumpToolHelp"
            x: panel.theme.spaceLg
            y: shapeCard.height - panel.theme.toolHelpBottom
            width: shapeCard.width - 2 * panel.theme.spaceLg
            elide: Text.ElideRight
            text: curve.tool === "pencil" ? "Drag across the grid to paint each step at the pointer's level"
                : curve.tool === "line" ? "Drag to draw a straight ramp · Shift snaps off"
                : curve.tool === "stamp" ? "Drag across steps to stamp the picked library shape into them"
                : "Drag points · drag empty space to select several · double-click adds · right-click removes · Alt-drag bends"
        }
    }

    Kit.Card {
        id: triggerCard
        theme: panel.theme
        title: "TRIGGER"
        x: panel.rightX
        y: panel.bodyY
        width: panel.triggerWidth
        height: panel.upperHeight
        Seg {
            x: panel.theme.spaceLg
            y: panel.theme.cardBodyY
            width: triggerCard.width - 2 * panel.theme.spaceLg
            parameterId: "pump.trigger"
            labels: ["TEMPO", "AUDIO", "MIDI"]
        }
        Rectangle {
            id: source
            objectName: "pumpSource"
            x: panel.theme.spaceLg
            y: panel.theme.sourceY
            width: triggerCard.width - 2 * panel.theme.spaceLg
            height: panel.theme.sourceHeight
            radius: panel.theme.wellRadius
            color: panel.theme.well
            border.color: panel.theme.wellEdge
            opacity: panel.trigger === 0 ? panel.theme.sourceOffOpacity : 1
            Text {
                anchors.verticalCenter: parent.verticalCenter
                x: panel.theme.sourceTextX
                width: source.width - 2 * panel.theme.sourceTextX
                text: panel.keyed ? "Key · " + panel.keyName
                    : panel.trigger === 2 ? "No key · send the kick track here"
                    : "No key · hears its own input"
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
                elide: Text.ElideRight
            }
        }
        Heading {
            x: panel.theme.spaceLg
            y: panel.theme.rateHeadingY
            text: panel.trigger === 0 ? "RATE" : "RATE · CYCLE LENGTH"
        }
        Seg {
            x: panel.theme.spaceLg
            y: panel.theme.rateY
            width: triggerCard.width - 2 * panel.theme.spaceLg
            height: panel.theme.compactSegmentHeight
            parameterId: "pump.rate"
            labels: ["1/16", "1/8", "1/4", "1/2", "BAR", "×2"]
        }
        Seg {
            x: panel.theme.spaceLg
            y: panel.theme.feelY
            width: triggerCard.width - 2 * panel.theme.spaceLg
            height: panel.theme.compactSegmentHeight
            parameterId: "pump.feel"
            labels: ["STRAIGHT", "DOTTED", "TRIPLET"]
        }

        Item {
            objectName: "pumpTempo"
            x: 0
            y: panel.theme.modePageY
            width: triggerCard.width
            height: panel.theme.modePageHeight
            visible: panel.trigger === 0
            Cell {
                x: panel.theme.spaceXl
                y: panel.theme.spaceXs
                parameterId: "pump.swing"
                diameter: panel.theme.largeKnobDiameter
            }
            Caption {
                x: panel.theme.modeNoteX
                y: panel.theme.swingNoteY
                width: triggerCard.width - panel.theme.modeNoteX - panel.theme.spaceLg
                wrapMode: Text.WordWrap
                lineHeight: panel.theme.modeNoteLineHeight
                text: "Each beat restarts the shape, locked to the song. Swing starts every second cycle late."
            }
        }

        Item {
            objectName: "pumpKickAudio"
            x: 0
            y: panel.theme.modePageY
            width: triggerCard.width
            height: panel.theme.modePageHeight
            visible: panel.trigger === 1
            Row {
                x: panel.theme.cardInset
                y: 0
                spacing: panel.theme.spaceLg
                Cell {
                    parameterId: "pump.threshold"
                    diameter: panel.theme.smallKnobDiameter
                }
                Cell {
                    parameterId: "pump.detect_low"
                    label: "Band Low"
                    diameter: panel.theme.smallKnobDiameter
                }
                Cell {
                    parameterId: "pump.detect_high"
                    label: "Band High"
                    diameter: panel.theme.smallKnobDiameter
                }
            }
            // Where the detector listens, on a 20 Hz to 4 kHz scale.
            Rectangle {
                id: detectStrip
                objectName: "pumpDetectBand"
                readonly property real low: panel.revision >= 0 ? panel.controller.value("pump.detect_low") : 30
                readonly property real high: panel.revision >= 0 ? panel.controller.value("pump.detect_high") : 160
                function at(hz) { return Math.log(hz / 20) / Math.log(4000 / 20) }
                x: panel.theme.spaceLg
                y: panel.theme.detectY
                width: triggerCard.width - 2 * panel.theme.spaceLg
                height: panel.theme.detectHeight
                radius: panel.theme.radiusSm
                color: panel.theme.well
                border.color: panel.theme.plateEdge
                Rectangle {
                    x: detectStrip.width * detectStrip.at(detectStrip.low)
                    width: Math.max(panel.theme.detectMinWidth, detectStrip.width * (detectStrip.at(detectStrip.high) - detectStrip.at(detectStrip.low)))
                    height: parent.height
                    radius: panel.theme.radiusSm
                    color: panel.theme.kick
                }
            }
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.detectCaptionY
                text: "20 Hz"
            }
            Caption {
                anchors.right: detectStrip.right
                y: panel.theme.detectCaptionY
                text: "HEARS ONLY THE KICK'S BAND · 4 kHz"
            }
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.lookaheadCaptionY
                text: "LOOKAHEAD"
            }
            Seg {
                x: panel.theme.lookaheadX
                y: panel.theme.lookaheadY
                width: triggerCard.width - panel.theme.lookaheadX - panel.theme.spaceLg
                height: panel.theme.smallSegmentHeight
                parameterId: "pump.lookahead"
                labels: ["OFF", "5", "10", "20"]
            }
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.lengthCaptionY
                text: "LENGTH"
            }
            Seg {
                x: panel.theme.lengthX
                y: panel.theme.lengthY
                width: triggerCard.width - panel.theme.lengthX - panel.theme.spaceLg
                height: panel.theme.smallSegmentHeight
                parameterId: "pump.follow"
                labels: ["RATE", "SPACING"]
            }
            Kit.Toggle {
                x: panel.theme.spaceLg
                y: panel.theme.listenY
                width: triggerCard.width - 2 * panel.theme.spaceLg
                height: panel.theme.listenHeight
                controller: panel.controller
                theme: panel.theme
                parameterId: "pump.listen"
                label: "LISTEN"
                onCaption: "the detector"
                offCaption: "the output"
                lit: panel.theme.time
            }
        }

        Item {
            objectName: "pumpMidi"
            x: 0
            y: panel.theme.modePageY
            width: triggerCard.width
            height: panel.theme.modePageHeight
            visible: panel.trigger === 2
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.spaceXxs
                text: "NOTES · " + (panel.revision >= 0
                    ? Shapes.noteName(Math.round(panel.controller.value("pump.note_low"))) + " TO "
                      + Shapes.noteName(Math.round(panel.controller.value("pump.note_high"))) : "")
            }
            Kit.Stepper {
                x: panel.theme.spaceLg
                y: panel.theme.noteStepperY
                width: panel.theme.noteStepperWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "pump.note_low"
                plain: true
            }
            Kit.Stepper {
                x: panel.theme.noteHighX
                y: panel.theme.noteStepperY
                width: panel.theme.noteStepperWidth
                controller: panel.controller
                theme: panel.theme
                parameterId: "pump.note_high"
                plain: true
            }
            Pill {
                objectName: "pumpLearn"
                x: panel.theme.learnX
                y: panel.theme.noteStepperY
                width: triggerCard.width - panel.theme.learnX - panel.theme.spaceLg
                label: panel.learning ? "PLAY IT…" : "LEARN"
                on: panel.learning
                ink: panel.theme.time
                Accessible.description: "Set the note range from the next note on the key track"
                onActivated: panel.armLearn()
            }
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.lastNoteY
                text: panel.lastNote >= 0 ? "LAST NOTE ON THE KEY · " + Shapes.noteName(panel.lastNote) : "NO NOTE YET"
            }
            Cell {
                x: panel.theme.spaceXl
                y: panel.theme.velocityY
                parameterId: "pump.velocity"
                diameter: panel.theme.knobDiameter
            }
            Caption {
                x: panel.theme.modeNoteX
                y: panel.theme.velocityNoteY
                width: triggerCard.width - panel.theme.modeNoteX - panel.theme.spaceLg
                wrapMode: Text.WordWrap
                lineHeight: panel.theme.modeNoteLineHeight
                text: "Softer notes duck less. Notes are read ahead: on time, no latency."
            }
            Caption {
                x: panel.theme.spaceLg
                y: panel.theme.midiLengthCaptionY
                text: "LENGTH"
            }
            Seg {
                x: panel.theme.lengthX
                y: panel.theme.midiLengthY
                width: triggerCard.width - panel.theme.lengthX - panel.theme.spaceLg
                height: panel.theme.smallSegmentHeight
                parameterId: "pump.follow"
                labels: ["RATE", "SPACING"]
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.depthRowY
            spacing: panel.theme.depthRowGap
            Cell {
                parameterId: "pump.depth"
                diameter: panel.theme.largeKnobDiameter
            }
            Cell {
                parameterId: "pump.nudge"
                diameter: panel.theme.largeKnobDiameter
            }
        }
    }

    Kit.Card {
        id: bandCard
        theme: panel.theme
        title: "BAND"
        note: "WHAT PUMPS"
        x: panel.middleX
        y: panel.lowerY
        width: panel.middleWidth
        height: panel.lowerHeight
        Seg {
            x: panel.theme.bandCardX
            y: panel.theme.cardBodyY
            width: panel.theme.bandColumnWidth
            height: panel.theme.compactSegmentHeight
            parameterId: "pump.band"
            labels: ["FULL", "LOWS ONLY", "THREE BAND"]
        }
        Seg {
            x: panel.theme.bandCardX
            y: panel.theme.stereoY
            width: panel.theme.bandColumnWidth
            height: panel.theme.compactSegmentHeight
            parameterId: "pump.stereo"
            labels: ["STEREO", "MID", "SIDE"]
        }
        Caption {
            x: panel.theme.bandCardX
            y: panel.theme.bandNoteY
            width: panel.theme.bandColumnWidth
            wrapMode: Text.WordWrap
            lineHeight: panel.theme.noteLineHeight
            text: (panel.bandMode === 1 ? "Only the lows pump; the top plays on. "
                   : panel.bandMode === 2 ? "Each band pumps by its own depth and, drawn, its own shape. "
                   : "The whole signal pumps. ")
                + (panel.revision >= 0 && panel.controller.choice("pump.stereo") === 1 ? "The sides stay wide."
                   : panel.revision >= 0 && panel.controller.choice("pump.stereo") === 2 ? "Only the sides pump."
                   : "")
        }
        Row {
            x: panel.theme.bandKnobsX
            y: panel.theme.bandKnobsY
            spacing: panel.theme.bandKnobGap
            Cell {
                parameterId: "pump.crossover"
                label: "Low Split"
                diameter: panel.theme.bandKnobDiameter
                inactive: panel.bandMode === 0
            }
            Cell {
                parameterId: "pump.crossover_high"
                label: "High Split"
                diameter: panel.theme.bandKnobDiameter
                inactive: !panel.threeBand
            }
            Cell {
                parameterId: "pump.depth_mid"
                diameter: panel.theme.bandKnobDiameter
                inactive: !panel.threeBand
            }
            Cell {
                parameterId: "pump.depth_high"
                diameter: panel.theme.bandKnobDiameter
                inactive: !panel.threeBand
            }
            Cell {
                parameterId: "pump.anticlick"
                diameter: panel.theme.bandKnobDiameter
            }
        }
    }

    Kit.Card {
        id: outputCard
        theme: panel.theme
        title: "OUTPUT"
        x: panel.rightX
        y: panel.lowerY
        width: panel.triggerWidth
        height: panel.lowerHeight
        Caption {
            anchors.right: parent.right
            anchors.rightMargin: panel.theme.spaceLg
            y: panel.theme.reductionCaptionY
            text: "GAIN REDUCTION"
        }
        Column {
            x: panel.theme.spaceLg
            y: panel.theme.reductionY
            spacing: panel.theme.reductionGap
            Repeater {
                model: panel.threeBand ? 3 : 1
                delegate: Row {
                    id: meterRow
                    required property int index
                    readonly property real db: [panel.gainDb, panel.gainMidDb, panel.gainHighDb][index]
                    spacing: panel.theme.spaceSm
                    Text {
                        width: panel.theme.reductionLabelWidth
                        visible: panel.threeBand
                        text: Shapes.bandNames[meterRow.index]
                        color: panel.theme.inkFaint
                        font.pixelSize: panel.theme.reductionLabelSize
                        font.bold: true
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Rectangle {
                        width: outputCard.width - 2 * panel.theme.spaceLg
                            - (panel.threeBand ? panel.theme.reductionLabelWidth + meterRow.spacing : 0)
                        height: panel.threeBand ? panel.theme.reductionBandHeight : panel.theme.reductionHeight
                        radius: panel.theme.radiusSm
                        color: panel.theme.well
                        border.color: panel.theme.plateEdge
                        Rectangle {
                            objectName: meterRow.index === 0 ? "pumpReductionMeter" : "pumpReductionMeter:" + meterRow.index
                            anchors.right: parent.right
                            anchors.rightMargin: panel.theme.hairline
                            y: panel.theme.hairline
                            height: parent.height - 2 * panel.theme.hairline
                            width: (parent.width - 2 * panel.theme.hairline)
                                * Math.max(0, Math.min(1, -meterRow.db / panel.reductionRangeDb))
                            radius: panel.theme.radiusXs
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: panel.theme.shape }
                                GradientStop { position: 1; color: panel.theme.heard }
                            }
                        }
                    }
                }
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            y: panel.theme.outputKnobsY
            spacing: panel.theme.outputKnobGap
            Cell {
                parameterId: "pump.mix"
                diameter: panel.theme.outputKnobDiameter
            }
            Cell {
                parameterId: "pump.output"
                diameter: panel.theme.outputKnobDiameter
            }
        }
    }

    Timer {
        objectName: "pumpTelemetryPoll"
        interval: panel.playing || panel.learning ? 33 : 250
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
