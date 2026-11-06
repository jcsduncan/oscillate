pragma ComponentBehavior: Bound
import QtQuick
import "clay"
import "devicekit" as Kit
import "shell"

// Clay, the Notes tab of an audio clip (ADR-0081):
// detect the notes in the clip, then move them like MIDI with eight tools,
// fix them with the macros, or turn them into something new (To MIDI,
// Harmonise, Fit to song key, Follow the bass, Variation). Every drag and
// every macro release is one undo step; the clip plays its rendered take.
Rectangle {
    id: editor
    objectName: "clayEditor"
    required property var clay
    property var bridge: null
    property string clipId: ""
    readonly property ClayTheme style: ClayTheme {}
    property string tool: "main"
    property bool showOriginal: false
    property bool auditionOnClick: false
    property int variationSeed: 1
    property bool variationApplied: false
    readonly property real sideWidth: style.sidePanelWidth
    // The newest history entries the side panel lists.
    readonly property int historyShown: 12

    readonly property var selectedNotes: clay ? clay.notes.filter(n => n.selected) : []
    readonly property var scopeNotes: selectedNotes.length > 0 ? selectedNotes : (clay ? clay.notes : [])
    readonly property bool polyphonic: clay ? clay.mode === "polyphonic" : false

    // The clip shown follows the dock's selection while the tab is up.
    function syncClip() {
        if (clay && visible && clipId.length > 0 && clay.clipId !== clipId)
            clay.open_clip(clipId)
    }
    onClipIdChanged: syncClip()
    onVisibleChanged: syncClip()
    Component.onCompleted: syncClip()
    Connections {
        target: editor.clay
        function onAuditionReady(path) {
            if (editor.bridge && editor.bridge.browserPreview)
                editor.bridge.browserPreview.preview_sample(path)
        }
        function onHistoryChanged() {
            const history = editor.clay.history
            const last = history.length > 0 ? history[history.length - 1].label : ""
            if (!last.startsWith("Variation"))
                editor.variationApplied = false
        }
    }

    function keyOptions() {
        const names = clay ? clay.tonicNames : []
        const list = [{ value: "chromatic", label: "Chromatic" }]
        for (let minor = 1; minor >= 0; --minor) {
            for (let t = 0; t < names.length; ++t)
                list.push({ value: t + (minor ? "m" : ""), label: names[t] + (minor ? " minor" : " major") })
        }
        return list
    }
    function keyValue(tonic, minor) { return tonic < 0 ? "chromatic" : tonic + (minor ? "m" : "") }
    function parseKey(value) {
        return value === "chromatic" ? { tonic: -1, minor: false }
            : { tonic: parseInt(value), minor: String(value).endsWith("m") }
    }
    function trackOptions(withNew, notesOnly) {
        const list = withNew ? [{ value: "", label: "New Nova track" }] : []
        for (const t of (clay ? clay.instrumentTracks : [])) {
            if (!notesOnly || t.hasNotes)
                list.push({ value: t.id, label: t.name + " · " + t.engine })
        }
        return list
    }
    function quantizeOptions() {
        const list = (clay ? clay.divisions : []).map(d => ({ value: d, label: d }))
        const grooves = editor.bridge && editor.bridge.noteTools ? editor.bridge.noteTools.grooves : []
        for (const g of grooves)
            list.push({ value: g.name, label: "Groove · " + g.name })
        return list
    }
    function centsLabel(c) { return (c >= 0 ? "+" : "−") + Math.abs(Math.round(c)) + " ct" }
    function loopSelection() {
        if (!editor.bridge || scopeNotes.length === 0)
            return
        const bar = clay.beatSeconds * clay.beatsPerBar
        let t0 = Infinity, t1 = -Infinity
        for (const n of scopeNotes) {
            t0 = Math.min(t0, n.start)
            t1 = Math.max(t1, n.end)
        }
        const first = Math.floor(t0 / bar) + 1
        const last = Math.max(first + 1, Math.ceil(t1 / bar) + 1)
        editor.bridge.loopTransport.set_range(first, last)
        editor.bridge.loopTransport.set_enabled(true)
        editor.bridge.seek_to_bar(first - 1)
        if (!editor.bridge.playbackActive)
            editor.bridge.toggle_playback()
    }

    color: style.panel
    clip: true

    // ---- Toolbar ----
    Rectangle {
        id: toolbar
        width: parent.width
        height: toolFlow.implicitHeight + 2 * editor.style.spaceMd
        color: editor.style.panel2
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: editor.style.hairline; color: editor.style.line }
        Flow {
            id: toolFlow
            x: editor.style.spaceLg; y: editor.style.spaceMd
            width: parent.width - 2 * editor.style.spaceLg
            spacing: editor.style.toolbarGroupGap
            Row {
                spacing: editor.style.spaceSm
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "DETECT AS"
                    color: editor.style.faint
                    font.family: editor.style.mono; font.pixelSize: editor.style.labelSize
                    font.weight: Font.DemiBold; font.letterSpacing: editor.style.labelSpacing
                }
                Kit.SegmentedControl {
                    theme: editor.style
                    namePrefix: "clayMode"
                    current: editor.clay ? editor.clay.mode : ""
                    enabled: editor.clay && editor.clay.open && !editor.clay.detecting
                    options: [
                        { id: "melodic", label: "Melodic", tip: "One voice: a vocal, bass or lead" },
                        { id: "polyphonic", label: "Polyphonic", tip: "Stacked notes: chords, pads, piano" },
                        { id: "percussive", label: "Percussive", tip: "Hits, each at its pitch" }
                    ]
                    onPicked: value => editor.clay.detect(value)
                }
            }
            Row {
                visible: editor.clay ? editor.clay.detecting : false
                spacing: editor.style.spaceMd
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: editor.style.progressWidth; height: editor.style.progressHeight
                    radius: editor.style.radiusSm
                    color: editor.style.well
                    border.color: editor.style.line
                    Rectangle {
                        width: parent.width * (editor.clay ? editor.clay.progress : 0)
                        height: parent.height; radius: editor.style.radiusSm
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: editor.style.clay3 }
                            GradientStop { position: 1; color: editor.style.clay2 }
                        }
                    }
                }
                Text {
                    objectName: "clayStage"
                    anchors.verticalCenter: parent.verticalCenter
                    text: editor.clay ? editor.clay.stage : ""
                    color: editor.style.dim
                    font.family: editor.style.mono; font.pixelSize: editor.style.typeSmall
                }
                Kit.Button {
                    theme: editor.style
                    label: "Cancel"
                    onClicked: editor.clay.cancel_detect()
                }
            }
            Row {
                spacing: editor.style.spaceSm
                visible: editor.clay ? editor.clay.hasTake : false
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "KEY"
                    color: editor.style.faint
                    font.family: editor.style.mono; font.pixelSize: editor.style.labelSize
                    font.weight: Font.DemiBold; font.letterSpacing: editor.style.labelSpacing
                }
                ClayPicker {
                    objectName: "clayKey"
                    style: editor.style
                    width: editor.style.keyPickerWidth
                    tip: "The take's key, for snapping and the macros"
                    options: editor.keyOptions()
                    value: editor.clay ? editor.keyValue(editor.clay.keyTonic, editor.clay.keyMinor) : "chromatic"
                    onChosen: value => {
                        const key = editor.parseKey(value)
                        if (key.tonic < 0)
                            editor.clay.set_chromatic()
                        else
                            editor.clay.set_key(key.tonic, key.minor)
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: editor.clay ? editor.clay.keyFromAnalysis : false
                    text: "from analysis"
                    color: editor.style.faint
                    font.family: editor.style.mono; font.pixelSize: editor.style.typeSmall
                }
            }
            Row {
                spacing: editor.style.spaceSm
                visible: editor.clay ? editor.clay.hasTake : false
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "SNAP"
                    color: editor.style.faint
                    font.family: editor.style.mono; font.pixelSize: editor.style.labelSize
                    font.weight: Font.DemiBold; font.letterSpacing: editor.style.labelSpacing
                }
                Kit.SegmentedControl {
                    theme: editor.style
                    accent: editor.style.blue
                    namePrefix: "claySnap"
                    current: editor.clay ? editor.clay.snap : "scale"
                    options: [
                        { id: "scale", label: "Scale", tip: "Pitch drags land on the key's notes" },
                        { id: "chromatic", label: "Chromatic", tip: "Pitch drags land on semitones" },
                        { id: "off", label: "Off", tip: "Pitch drags move freely" }
                    ]
                    onPicked: value => editor.clay.snap = value
                }
            }
            Row {
                spacing: editor.style.spaceXs
                visible: editor.clay ? editor.clay.hasTake : false
                Kit.Button { theme: editor.style; label: "↶"; tip: "Undo"; shortcut: "Ctrl+Z"; onClicked: editor.clay.undo() }
                Kit.Button { theme: editor.style; label: "↷"; tip: "Redo"; shortcut: "Ctrl+Shift+Z"; onClicked: editor.clay.redo() }
            }
            Row {
                spacing: editor.style.toolButtonSpacing
                visible: editor.clay ? editor.clay.hasTake : false
                Repeater {
                    model: [
                        { id: "main", label: "Main", tip: "Move pitch and time · double-click splits" },
                        { id: "pitch", label: "Pitch", tip: "Pitch centre in cents · double-click snaps to the scale" },
                        { id: "modulation", label: "Mod", tip: "Vibrato depth: drag up for more, down for less" },
                        { id: "drift", label: "Drift", tip: "Slow pitch wander: drag down to steady it" },
                        { id: "formant", label: "Formant", tip: "Vowel colour, apart from pitch" },
                        { id: "amplitude", label: "Level", tip: "Note level in dB" },
                        { id: "time", label: "Time", tip: "Drag an edge to stretch, the middle to move" },
                        { id: "separation", label: "Split", tip: "Click splits · Shift-click joins with the next" }
                    ]
                    delegate: Rectangle {
                        id: toolButton
                        required property var modelData
                        required property int index
                        readonly property bool lit: editor.tool === modelData.id
                        objectName: "clayTool:" + modelData.id
                        width: toolRow.implicitWidth + editor.style.toolButtonPad
                        height: editor.style.controlHeight
                        radius: editor.style.radius
                        color: lit ? editor.style.toolLitFill : toolArea.containsMouse ? editor.style.panel3 : "transparent"
                        border.color: lit ? editor.style.toolLitEdge : "transparent"
                        activeFocusOnTab: visible
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: modelData.label + " tool"
                        Accessible.checked: lit
                        Keys.onReturnPressed: editor.tool = modelData.id
                        Keys.onSpacePressed: editor.tool = modelData.id
                        Row {
                            id: toolRow
                            anchors.centerIn: parent
                            spacing: editor.style.toolButtonInnerGap
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: toolButton.modelData.label
                                color: toolButton.lit ? editor.style.ink : editor.style.dim
                                font.family: editor.style.sans; font.pixelSize: editor.style.textSize
                                font.weight: Font.DemiBold
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: editor.style.shortcutBoxSize; height: editor.style.shortcutBoxSize
                                radius: editor.style.radiusSm
                                color: "transparent"
                                border.color: editor.style.line2
                                Text {
                                    anchors.centerIn: parent
                                    text: String(toolButton.index + 1)
                                    color: editor.style.faint
                                    font.family: editor.style.mono; font.pixelSize: editor.style.typeMicro
                                font.weight: Font.DemiBold
                                }
                            }
                        }
                        MouseArea {
                            id: toolArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: editor.tool = toolButton.modelData.id
                            Tip { text: toolButton.modelData.tip; shortcut: String(toolButton.index + 1) }
                        }
                    }
                }
            }
        }
    }

    // ---- The canvas and its footer ----
    Item {
        id: canvasColumn
        y: toolbar.height
        width: parent.width - editor.sideWidth
        height: parent.height - y
        ClayCanvas {
            id: noteCanvas
            objectName: "clayCanvas"
            width: parent.width
            height: parent.height - footer.height
            style: editor.style
            clay: editor.clay
            tool: editor.tool
            showOriginal: editor.showOriginal
            auditionOnClick: editor.auditionOnClick
            playheadSeconds: editor.bridge ? editor.bridge.playheadSeconds : -1
            onToolRequested: name => editor.tool = name
            onAuditionRequested: key => editor.clay.audition_note(key)
            onPlayRequested: editor.clay.audition_selection()
        }
        // Before detection: what Clay does, and the three ways to start.
        Rectangle {
            objectName: "clayEmpty"
            visible: editor.clay ? editor.clay.open && !editor.clay.hasTake && !editor.clay.detecting : false
            anchors.centerIn: noteCanvas
            width: Math.min(noteCanvas.width - 2 * editor.style.emptyCardPad, editor.style.emptyCardMaxWidth)
            height: emptyColumn.implicitHeight + 2 * editor.style.spaceXl
            radius: editor.style.emptyCardRadius
            color: editor.style.panel
            border.color: editor.style.line2
            Column {
                id: emptyColumn
                x: editor.style.emptyCardPad; y: editor.style.spaceXl
                width: parent.width - 2 * editor.style.emptyCardPad
                spacing: editor.style.emptyCardGap
                Text {
                    text: "Find the notes in " + (editor.clay ? editor.clay.clipName : "this clip")
                    color: editor.style.ink
                    font.family: editor.style.sans; font.pixelSize: editor.style.typeTitle; font.weight: Font.Bold
                    width: parent.width; elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Clay finds every note in the audio, then lets you move it like MIDI: its pitch, wobble, drift, formant, level and timing. Nothing is destroyed: the edits live on the clip, and export plays what you hear."
                    color: editor.style.dim
                    font.family: editor.style.sans; font.pixelSize: editor.style.typeLabel
                    lineHeight: editor.style.bodyLineHeight
                }
                Row {
                    spacing: editor.style.spaceMd
                    Repeater {
                        model: [
                            { id: "melodic", label: "Melodic" },
                            { id: "polyphonic", label: "Polyphonic" },
                            { id: "percussive", label: "Percussive" }
                        ]
                        delegate: Kit.Button {
                            required property var modelData
                            required property int index
                            objectName: "clayDetect:" + modelData.id
                            theme: editor.style
                            prime: index === 0
                            label: "Detect · " + modelData.label
                            onClicked: editor.clay.detect(modelData.id)
                        }
                    }
                }
            }
        }
        Rectangle {
            id: footer
            y: parent.height - height
            width: parent.width
            height: editor.style.footerHeight
            color: editor.style.panel2
            Rectangle { width: parent.width; height: editor.style.hairline; color: editor.style.line }
            Row {
                x: editor.style.footerInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: editor.style.spaceSm
                Kit.Button {
                    objectName: "clayPlaySelection"
                    theme: editor.style
                    prime: true
                    label: "▶ Play"
                    tip: editor.selectedNotes.length > 0 ? "Play the selection" : "Play the take"
                    shortcut: "Space"
                    enabled: editor.clay ? editor.clay.hasTake : false
                    onClicked: editor.clay.audition_selection()
                }
                Kit.Button {
                    theme: editor.style
                    label: "Solo note"
                    tip: "Play the selected note alone"
                    enabled: editor.selectedNotes.length > 0
                    onClicked: editor.clay.audition_note(editor.selectedNotes[0].key)
                }
                Kit.Button {
                    theme: editor.style
                    label: "Loop"
                    tip: "Loop the selection's bars in the song"
                    enabled: editor.bridge !== null && editor.scopeNotes.length > 0
                    onClicked: editor.loopSelection()
                }
                Kit.SegmentedControl {
                    theme: editor.style
                    accent: editor.style.teal
                    namePrefix: "clayAB"
                    current: editor.clay && editor.clay.abOriginal ? "a" : "b"
                    options: [
                        { id: "a", label: "A", tip: "Auditions play the original" },
                        { id: "b", label: "B", tip: "Auditions play the edit" }
                    ]
                    onPicked: value => editor.clay.abOriginal = value === "a"
                }
                Kit.Button {
                    theme: editor.style
                    label: "Audition on click"
                    on: editor.auditionOnClick
                    onClicked: editor.auditionOnClick = !editor.auditionOnClick
                }
                Kit.Button {
                    objectName: "clayShowOriginal"
                    theme: editor.style
                    label: "Original curves"
                    on: editor.showOriginal
                    onClicked: editor.showOriginal = !editor.showOriginal
                }
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: editor.style.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                spacing: editor.style.spaceLg
                Text {
                    objectName: "clayStatus"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(0, footer.width - editor.style.footerButtonsWidth))
                    elide: Text.ElideRight
                    text: editor.clay ? editor.clay.status : ""
                    color: editor.style.dim
                    font.family: editor.style.mono; font.pixelSize: editor.style.typeSmall
                }
                Row {
                    id: renderRow
                    objectName: "clayRenderState"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: editor.clay ? editor.clay.renderState !== "none" : false
                    spacing: editor.style.spaceSm
                    readonly property bool rendering: editor.clay ? editor.clay.renderState === "rendering" : false
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: editor.style.renderDotSize; height: editor.style.renderDotSize
                        radius: editor.style.radiusMd
                        color: renderRow.rendering ? editor.style.amber : editor.style.green
                        SequentialAnimation on opacity {
                            running: renderRow.rendering && Motion.enabled
                            loops: Animation.Infinite
                            NumberAnimation { to: editor.style.pulseLowOpacity; duration: editor.style.pulseDuration }
                            NumberAnimation { to: 1; duration: editor.style.pulseDuration }
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: renderRow.rendering ? "rendering…" : "playback has the edit"
                        color: renderRow.rendering ? editor.style.amber : editor.style.dim
                        font.family: editor.style.mono; font.pixelSize: editor.style.typeSmall
                    }
                }
            }
        }
    }

    // ---- The side panel ----
    Rectangle {
        x: canvasColumn.width
        y: toolbar.height
        width: editor.sideWidth
        height: parent.height - y
        color: editor.style.panel
        Rectangle { width: editor.style.hairline; height: parent.height; color: editor.style.line }
        Flickable {
            id: side
            objectName: "claySide"
            x: editor.style.hairline
            width: parent.width - editor.style.hairline
            height: parent.height
            contentHeight: sideColumn.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: sideColumn
                width: side.width

                // Inspector.
                ClaySection {
                    style: editor.style
                    title: editor.selectedNotes.length === 1 ? "NOTE"
                        : editor.selectedNotes.length > 1 ? editor.selectedNotes.length + " NOTES" : "TAKE"
                    Text {
                        objectName: "clayInspector"
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: editor.style.ink
                        font.family: editor.style.mono; font.pixelSize: editor.style.typeBody
                        lineHeight: editor.style.inspectorLineHeight
                        text: {
                            const sel = editor.selectedNotes
                            if (!editor.clay || !editor.clay.hasTake)
                                return "No notes yet: detect them first."
                            if (sel.length === 1) {
                                const n = sel[0]
                                const nearest = Math.round(n.centre / noteCanvas.centsPerSemitone) * noteCanvas.centsPerSemitone
                                return noteCanvas.noteName(n.centre) + "  " + editor.centsLabel(n.centre - nearest)
                                    + "\n" + Math.round((n.end - n.start) * 1000) + " ms · "
                                    + (n.gainDb >= 0 ? "+" : "") + n.gainDb.toFixed(1) + " dB"
                                    + "\nmod ×" + n.modulation.toFixed(2) + " · drift ×" + n.drift.toFixed(2)
                                    + " · formant " + editor.centsLabel(n.formant)
                            }
                            const all = editor.clay.notes
                            return all.length + " notes · " + editor.clay.mode
                                + (sel.length > 1 ? " · " + sel.length + " selected" : "")
                                + "\nClick a note, or drag a box, to select."
                        }
                    }
                }

                // Macros.
                ClaySection {
                    style: editor.style
                    title: "MACROS"
                    hint: editor.selectedNotes.length > 0 ? "on the selection" : "on every note"
                    visible: editor.clay ? editor.clay.hasTake : false
                    Kit.Slider {
                        id: correctCentre
                        objectName: "clayCorrectCentre"
                        theme: editor.style
                        width: parent.width
                        label: "PITCH"
                        tip: "Correct pitch: pull each note's centre onto the key"
                        readout: Math.round(value * 100) + " %"
                        onStarted: editor.clay.begin_macro()
                        onMoved: editor.clay.preview_correct_pitch(correctCentre.value, correctDrift.value)
                        onReleased: editor.clay.commit_macro("Correct pitch")
                    }
                    Kit.Slider {
                        id: correctDrift
                        objectName: "clayCorrectDrift"
                        theme: editor.style
                        width: parent.width
                        label: "DRIFT"
                        tip: "Correct pitch drift: steady the slow wander inside each note"
                        readout: Math.round(value * 100) + " %"
                        onStarted: editor.clay.begin_macro()
                        onMoved: editor.clay.preview_correct_pitch(correctCentre.value, correctDrift.value)
                        onReleased: editor.clay.commit_macro("Correct pitch")
                    }
                    Row {
                        spacing: editor.style.spaceSm
                        width: parent.width
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: editor.style.sliderLabelWidth
                            text: "GRID"
                            color: editor.style.faint
                            font.family: editor.style.mono; font.pixelSize: editor.style.labelSize
                            font.weight: Font.DemiBold; font.letterSpacing: editor.style.labelSpacing
                        }
                        ClayPicker {
                            id: quantizeGrid
                            objectName: "clayQuantizeGrid"
                            style: editor.style
                            width: parent.width - editor.style.sliderLabelWidth - editor.style.spaceSm
                            tip: "Quantize to"
                            options: editor.quantizeOptions()
                            value: "1/16"
                            onChosen: value => quantizeGrid.value = value
                        }
                    }
                    Kit.Slider {
                        id: quantizeAmount
                        objectName: "clayQuantize"
                        theme: editor.style
                        width: parent.width
                        label: "TIME"
                        tip: "Quantize time: move each note's start toward the grid or groove"
                        readout: Math.round(value * 100) + " %"
                        onStarted: editor.clay.begin_macro()
                        onMoved: editor.clay.preview_quantize(quantizeGrid.value, quantizeAmount.value)
                        onReleased: editor.clay.commit_macro("Quantize time")
                    }
                    Kit.Slider {
                        id: levelAmount
                        objectName: "clayLevel"
                        theme: editor.style
                        width: parent.width
                        label: "LEVEL"
                        tip: "Level out: bring loud and quiet notes together"
                        readout: Math.round(value * 100) + " %"
                        onStarted: editor.clay.begin_macro()
                        onMoved: editor.clay.preview_level(levelAmount.value)
                        onReleased: editor.clay.commit_macro("Level out")
                    }
                }

                // Creative tools.
                ClaySection {
                    style: editor.style
                    title: "TO MIDI"
                    ours: true
                    visible: editor.clay ? editor.clay.hasTake : false
                    ClayPicker {
                        id: midiTarget
                        objectName: "clayMidiTarget"
                        style: editor.style
                        width: parent.width
                        tip: "Send the notes to"
                        options: editor.trackOptions(true, false)
                        value: ""
                        onChosen: value => midiTarget.value = value
                    }
                    Kit.Button {
                        objectName: "clayToMidi"
                        theme: editor.style
                        prime: true
                        label: "Send notes to MIDI"
                        tip: "A MIDI clip with the notes, their velocities and pitch bends"
                        onClicked: editor.clay.to_midi(midiTarget.value)
                    }
                }
                ClaySection {
                    style: editor.style
                    title: "HARMONISE"
                    ours: true
                    visible: editor.clay ? editor.clay.hasTake : false
                    ClayPicker {
                        id: harmonyInterval
                        objectName: "clayHarmonyInterval"
                        style: editor.style
                        width: parent.width
                        tip: "Interval, in the key"
                        options: [
                            { value: 2, label: "3rd above" },
                            { value: 4, label: "5th above" },
                            { value: 7, label: "Octave above" },
                            { value: -2, label: "3rd below" },
                            { value: -3, label: "4th below" },
                            { value: -7, label: "Octave below" }
                        ]
                        value: 2
                        onChosen: value => harmonyInterval.value = value
                    }
                    Kit.Slider {
                        id: harmonyFormant
                        theme: editor.style
                        width: parent.width
                        label: "FORMANT"
                        from: -400; to: 400; step: 10
                        readout: editor.centsLabel(value)
                        tip: "Each harmony's vowel colour, so it sounds like another singer"
                    }
                    Kit.Slider {
                        id: harmonyLevel
                        theme: editor.style
                        width: parent.width
                        label: "LEVEL"
                        from: -24; to: 0; step: 0.5
                        value: -6
                        readout: value.toFixed(1) + " dB"
                        tip: "The harmony's level against the note"
                    }
                    Kit.Button {
                        objectName: "clayHarmonise"
                        theme: editor.style
                        label: "Add harmony"
                        tip: editor.selectedNotes.length > 0 ? "Harmonise the selected notes" : "Select notes to harmonise"
                        enabled: editor.selectedNotes.length > 0
                        onClicked: editor.clay.harmonise(harmonyInterval.value, harmonyFormant.value, harmonyLevel.value)
                    }
                }
                ClaySection {
                    style: editor.style
                    title: "FIT TO SONG KEY"
                    ours: true
                    visible: editor.clay ? editor.clay.hasTake : false
                    ClayPicker {
                        id: fitKey
                        objectName: "clayFitKey"
                        style: editor.style
                        width: parent.width
                        tip: "Retune the take into"
                        options: editor.keyOptions().filter(o => o.value !== "chromatic")
                        value: editor.clay && editor.clay.songKeyTonic >= 0
                            ? editor.keyValue(editor.clay.songKeyTonic, editor.clay.songKeyMinor) : "0"
                        onChosen: value => fitKey.value = value
                    }
                    Row {
                        spacing: editor.style.spaceSm
                        Kit.Button {
                            id: followQuality
                            property bool chosen: true
                            theme: editor.style
                            label: "Follow quality"
                            tip: "Minor thirds become major too (and back)"
                            on: chosen
                            onClicked: chosen = !chosen
                        }
                        Kit.Button {
                            objectName: "clayFit"
                            theme: editor.style
                            label: "Fit"
                            prime: true
                            onClicked: {
                                const key = editor.parseKey(fitKey.value)
                                editor.clay.fit_to_key(key.tonic, key.minor, followQuality.chosen)
                            }
                        }
                        Kit.Button {
                            objectName: "clayFlipQuality"
                            visible: editor.polyphonic && editor.clay.keyTonic >= 0
                            theme: editor.style
                            label: editor.clay && editor.clay.keyMinor ? "Minor → Major" : "Major → Minor"
                            tip: "Move only the third: the chord changes quality"
                            onClicked: editor.clay.fit_to_key(editor.clay.keyTonic, !editor.clay.keyMinor, true)
                        }
                    }
                }
                ClaySection {
                    style: editor.style
                    title: "FOLLOW THE BASS"
                    ours: true
                    visible: editor.clay ? editor.clay.hasTake : false
                    ClayPicker {
                        id: followTrack
                        objectName: "clayFollowTrack"
                        style: editor.style
                        width: parent.width
                        tip: "Follow the notes of"
                        options: editor.trackOptions(false, true)
                        value: options.length > 0 ? options[0].value : ""
                        enabled: options.length > 0
                        onChosen: value => followTrack.value = value
                    }
                    Kit.Slider {
                        id: followTime
                        theme: editor.style
                        width: parent.width
                        label: "TIMING"
                        readout: Math.round(value * 100) + " %"
                        tip: "How far the notes also move to the bass notes' timing"
                    }
                    Row {
                        spacing: editor.style.spaceSm
                        Kit.Button {
                            id: stayInKey
                            property bool chosen: true
                            theme: editor.style
                            label: "Stay in key"
                            on: chosen
                            onClicked: chosen = !chosen
                        }
                        Kit.Button {
                            theme: editor.style
                            label: "Follow"
                            prime: true
                            enabled: followTrack.options.length > 0
                            onClicked: editor.clay.follow_notes(followTrack.value, followTime.value, stayInKey.chosen)
                        }
                    }
                }
                ClaySection {
                    style: editor.style
                    title: "VARIATION"
                    ours: true
                    visible: editor.clay ? editor.clay.hasTake : false
                    Kit.Slider {
                        id: variationAmount
                        theme: editor.style
                        width: parent.width
                        label: "AMOUNT"
                        value: 0.4
                        readout: Math.round(value * 100) + " %"
                        tip: "How much pitch, timing and level may change"
                    }
                    Row {
                        spacing: editor.style.spaceSm
                        Kit.Button {
                            theme: editor.style
                            label: "‹"
                            tip: "The previous seed"
                            onClicked: {
                                editor.variationSeed = Math.max(1, editor.variationSeed - 1)
                                if (editor.variationApplied) {
                                    editor.clay.undo()
                                    editor.clay.variation(editor.variationSeed, variationAmount.value)
                                }
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "SEED " + editor.variationSeed
                            color: editor.style.ink
                            font.family: editor.style.mono; font.pixelSize: editor.style.typeBody; font.weight: Font.DemiBold
                        }
                        Kit.Button {
                            theme: editor.style
                            label: "›"
                            tip: "The next seed (replaces the last variation)"
                            onClicked: {
                                editor.variationSeed += 1
                                if (editor.variationApplied) {
                                    editor.clay.undo()
                                    editor.clay.variation(editor.variationSeed, variationAmount.value)
                                }
                            }
                        }
                        Kit.Button {
                            objectName: "clayVary"
                            theme: editor.style
                            prime: true
                            label: "Vary"
                            onClicked: {
                                editor.clay.variation(editor.variationSeed, variationAmount.value)
                                editor.variationApplied = true
                            }
                        }
                    }
                }

                // History.
                ClaySection {
                    style: editor.style
                    title: "HISTORY"
                    visible: editor.clay ? editor.clay.hasTake : false
                    Repeater {
                        model: editor.clay ? editor.clay.history.slice().reverse().slice(0, editor.historyShown) : []
                        delegate: Rectangle {
                            id: historyRow
                            required property var modelData
                            required property int index
                            objectName: "clayHistory:" + index
                            width: parent ? parent.width : 0
                            height: editor.style.historyRowHeight
                            radius: editor.style.historyRowRadius
                            color: index === 0 ? editor.style.historyCurrentFill : "transparent"
                            Text {
                                x: editor.style.spaceMd
                                anchors.verticalCenter: parent.verticalCenter
                                text: historyRow.modelData.label
                                color: historyRow.index === 0 ? editor.style.ink : editor.style.dim
                                font.family: editor.style.sans; font.pixelSize: editor.style.typeBody
                            }
                        }
                    }
                    Text {
                        visible: editor.clay ? editor.clay.history.length === 0 : true
                        text: "Edits appear here, newest first."
                        color: editor.style.faint
                        font.family: editor.style.sans; font.pixelSize: editor.style.typeBody
                    }
                    Kit.Button {
                        theme: editor.style
                        label: "Remove the take"
                        tip: "The clip plays its source again (undo brings the take back)"
                        onClicked: editor.clay.remove_take()
                    }
                }
            }
        }
    }

    // Before a clip: say so.
    Text {
        visible: editor.clay ? !editor.clay.open : true
        anchors.centerIn: parent
        text: "Select an audio clip to edit its notes."
        color: editor.style.dim
        font.family: editor.style.sans; font.pixelSize: editor.style.typeLabel
    }
}
