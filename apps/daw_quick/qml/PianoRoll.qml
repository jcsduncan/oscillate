pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Contracts
import "harmony"
import "shell"

Rectangle {
    id: roll
    objectName: "pianoRoll"
    required property var bridge
    required property string selectedClipId
    required property var selectedClip
    required property string drumEngineId
    required property var drumLanes
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    property int lowestKey: 60
    // Pitch rows shown: as many as fit at a comfortable height (at least an
    // octave), and Ctrl+wheel zooms between 12 and 48.
    property int zoomRows: 0
    readonly property real notesHeight: height - rowTop - Theme.spaceSm - velocityHeight - stripHeight
    // Counted from the roll's own height (less room for the lanes under the
    // notes), not from the lanes: the pedal strip sizes itself by the rows.
    readonly property real rowsSpace: Math.max(0, height - rowTop - Theme.pianoRollLaneReserve)
    // Never so many rows that one is under 7 px.
    readonly property int maxRows: Math.max(12, Math.min(48, Math.floor(rowsSpace / Theme.pianoRollMinRowHeight)))
    readonly property int rowCount: zoomRows > 0 ? Math.min(zoomRows, maxRows)
        : Math.max(12, Math.min(36, Math.floor(rowsSpace / Theme.pianoRollComfortRowHeight)))
    readonly property int highestLowKey: 128 - rowCount
    property bool preferPiano: false
    property int gridSteps: 16
    // The note last clicked (the velocity lane edits it), and every
    // selected note: Ctrl+A, Shift/Ctrl-click and drags act on all of them.
    property string selectedNoteId: ""
    property var selectedNoteIds: []
    // While dragging: the grabbed note's own step and key, so the others
    // move by the same amounts.
    property int noteDragFromStep: 0
    property int noteDragFromKey: 60
    function isSelected(id) { return selectedNoteIds.indexOf(id) >= 0 }
    function selectOnly(id) {
        selectedNoteId = id
        selectedNoteIds = id.length > 0 ? [id] : []
    }
    function toggleSelected(id) {
        const ids = selectedNoteIds.slice()
        const at = ids.indexOf(id)
        if (at >= 0) ids.splice(at, 1)
        else ids.push(id)
        selectedNoteIds = ids
        selectedNoteId = at >= 0 ? (ids.length > 0 ? ids[ids.length - 1] : "") : id
    }
    function selectAll() {
        selectedNoteIds = notes.map(note => String(note.id))
        if (selectedNoteIds.length > 0 && !isSelected(selectedNoteId))
            selectedNoteId = selectedNoteIds[0]
    }
    function clearSelection() { selectOnly("") }
    function deleteSelected() {
        if (selectedNoteIds.length > 0)
            bridge.remove_notes(selectedClipId, selectedNoteIds)
        clearSelection()
    }
    property bool noteDragValid: false
    // A Ctrl-drag copies: the originals stay and copies land at the drop.
    property bool noteDragCopy: false
    property int noteDragStep: 0
    property int noteDragKey: 60
    property real noteDragDuration: 0
    property string resizingNoteId: ""
    property int resizingEndStep: 0
    readonly property real barTicks: noteTools.barTicks
    readonly property real gridTicks: barTicks / gridSteps
    // The pattern is the part of the clip that plays. Notes kept after its
    // end (a shortened clip keeps them, as in Live) show greyed out, so the
    // roll spans whole bars to the later of the pattern and the last note.
    readonly property real patternTicks: selectedClip && Number(selectedClip.patternTicks) > 0
        ? Number(selectedClip.patternTicks) : barTicks
    readonly property bool looping: selectedClip ? selectedClip.loop === true : false
    readonly property real contentTicks: notes.reduce(
        (end, note) => Math.max(end, Number(note.startTicks) + Number(note.durationTicks)),
        patternTicks)
    readonly property int viewBars: Math.max(1, Math.min(64, Math.ceil(contentTicks / barTicks)))
    readonly property real viewTicks: viewBars * barTicks
    readonly property int columns: gridSteps * viewBars
    readonly property int patternSteps: Math.max(1, Math.floor(patternTicks / gridTicks))
    readonly property bool drumCapable: drumLanes.length > 0
    readonly property bool drumView: drumCapable && !preferPiano
    // The project key's pitch classes; in-key rows are highlighted, and
    // Fold shows only them (plus any row that holds a note, so nothing hides).
    readonly property var scale: bridge.projectScale
    readonly property bool scaleKnown: scale.length > 0
    property bool foldToScale: false
    // The musical tools (ADR-0059): Scale lock snaps drawn and moved notes
    // into the key; with a chord stamp chosen, a click draws a chord.
    readonly property var noteTools: bridge.noteTools
    property bool toolsOpen: false
    readonly property bool chordStamping: toolsPanel.chordMode && !drumView
    readonly property bool scaleLocked: noteTools !== null && noteTools.scaleLock && scaleKnown
                                        && !drumView
    // The key a drawn or dragged note lands on.
    function drawKey(key) { return scaleLocked ? noteTools.lock_key(key) : key }
    function quantise() {
        noteTools.quantize(selectedClipId, selectedNoteIds, gridSteps, toolsPanel.quantizeStrength,
                           toolsPanel.quantizeSwing, toolsPanel.quantizeEnds)
    }
    readonly property bool folded: foldToScale && scaleKnown && !drumView
    function inScale(key) { return scale.indexOf(((key % 12) + 12) % 12) >= 0 }
    readonly property var visibleRows: {
        if (drumView) return drumLanes.slice().reverse()
        const rows = []
        if (folded) {
            const used = {}
            for (const note of notes)
                used[Number(note.key)] = true
            for (let key = lowestKey; key <= 127 && rows.length < roll.rowCount; ++key) {
                if (inScale(key) || used[key])
                    rows.unshift({key: key, name: noteName(key)})
            }
            return rows
        }
        for (let index = roll.rowCount - 1; index >= 0; --index) {
            const key = lowestKey + index
            rows.push({key: key, name: noteName(key)})
        }
        return rows
    }
    readonly property real keyboardWidth: drumView ? Theme.pianoRollDrumKeyboardWidth : Theme.pianoRollKeyboardWidth
    readonly property real gridWidth: Math.max(1, width - keyboardWidth - chordPanelWidth)
    // Chord clips (ADR-0084): the chord strip over the notes names the chords
    // they make, whenever there are any (or the chord tools are open), and the
    // tools sit to the right of the grid.
    readonly property var harmony: bridge.harmony
    property bool chordsOpen: false
    readonly property bool chordView: !drumView && selectedClip !== null && selectedClip !== undefined
    readonly property bool chordToolsShown: chordView && chordsOpen
    readonly property bool chordStripShown: chordView && harmony !== null
                                            && (chordsOpen || harmony.count > 0)
    readonly property real chordPanelWidth: chordToolsShown ? Math.min(Theme.pianoRollChordPanelMaxWidth, Math.max(Theme.pianoRollChordPanelMinWidth, width * Theme.pianoRollChordPanelFraction)) : 0
    readonly property real headerBottom: Theme.pianoRollHeaderBottom
    readonly property real chordStripHeight: chordStripShown ? Theme.pianoRollChordStripHeight : 0
    readonly property real rowTop: chordStripShown ? headerBottom + chordStripHeight + Theme.spaceXxs : Theme.pianoRollRowTop
    // The pedal strip (ADR-0035) sits under the notes in the piano view.
    readonly property bool pedalsShown: !drumView && selectedClip !== null
        && selectedClip !== undefined
    readonly property real stripHeight: pedalsShown ? pedalStrip.implicitHeight + Theme.spaceXs : 0
    // Velocity sits between the notes and the pedals, as in a score.
    readonly property real velocityHeight: selectedClip ? velocityLane.implicitHeight + Theme.spaceXxs : 0
    readonly property real rowHeight: Math.max(Theme.pianoRollMinNoteRowHeight, notesHeight / visibleRows.length)
    readonly property var notes: selectedClip ? selectedClip.notes || [] : []
    readonly property int unmappedNotes: drumView
        ? notes.filter(note => rowIndexForKey(Number(note.key)) < 0).length : 0
    color: raisedColor
    border.color: lineColor
    height: Theme.pianoRollHeight
    clip: true
    activeFocusOnTab: visible
    Accessible.role: Accessible.Pane
    Accessible.name: (drumView
        ? "Drum lanes for " + drumEngineId + ". Switch to Piano to see all MIDI pitches."
        : "Piano roll. Mouse wheel scrolls pitches, Ctrl+wheel zooms them; select a grid, then Quantise to move notes.")
        + " The clip is " + (patternTicks / barTicks).toFixed(2).replace(/\.?0+$/, "")
        + (looping ? " bars and loops." : " bars; drag its right edge in the arrangement to lengthen it.")

    function noteName(key) {
        return Contracts.note_name(key, Contracts.sharp, Contracts.scientific_middle_c)
    }
    function scrollKeys(delta) {
        if (drumView) return
        lowestKey = Math.max(0, Math.min(highestLowKey, lowestKey + delta))
    }
    onRowCountChanged: lowestKey = Math.max(0, Math.min(highestLowKey, lowestKey))
    function scrollFromWheel(event) {
        const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
        if (drumView || delta === 0) {
            event.accepted = false
            return
        }
        if (event.modifiers & Qt.ControlModifier) {
            // Zoom around the middle row.
            const middle = lowestKey + rowCount / 2
            zoomRows = Math.max(12, Math.min(maxRows, rowCount + (delta > 0 ? -4 : 4)))
            lowestKey = Math.max(0, Math.min(highestLowKey, Math.round(middle - zoomRows / 2)))
        } else {
            scrollKeys(Math.sign(delta) * 3)
        }
        event.accepted = true
    }
    function stepAt(x) {
        return Math.max(0, Math.min(columns - 1,
            Math.floor((x - keyboardWidth) * columns / gridWidth)))
    }
    function xAt(ticks) { return keyboardWidth + gridWidth * ticks / viewTicks }
    // The key the left strip is sounding, or -1.
    property int playingKey: -1
    readonly property string playingTrackId: selectedClipId !== ""
        ? bridge.clip_track_id(selectedClipId) : ""
    // A note being recorded (ADR-0086), drawn growing until it is let go.
    readonly property color recordingNoteColor: Theme.recordingNote
    // The key strip's note a recording is taking (ADR-0086), or -1.
    property int recordingKey: -1
    function playKey(key, x) {
        // Further right on a key plays louder, as on a real keyboard.
        const velocity = Math.round(40 + 87 * Math.max(0, Math.min(1, x / keyboardWidth)))
        if (key !== recordingKey && bridge.recording) {
            if (recordingKey >= 0)
                bridge.recording.note_off(recordingKey)
            recordingKey = key
            if (key >= 0 && playingTrackId !== "")
                bridge.recording.note_on(playingTrackId, key, velocity)
        }
        sound(key, velocity)
    }
    // Sounds `key` until the next call (-1 is silence): the key strip, a
    // note being drawn and a note being dragged all play while held.
    function sound(key, velocity) {
        if (key === playingKey)
            return
        if (playingKey >= 0)
            bridge.release_note(playingKey)
        playingKey = key
        if (key >= 0 && playingTrackId !== "")
            bridge.play_note(playingTrackId, key, velocity)
    }
    function keyAt(y) {
        const index = Math.max(0, Math.min(visibleRows.length - 1,
            Math.floor((y - rowTop) / rowHeight)))
        return visibleRows[index].key
    }
    function rowIndexForKey(key) {
        return visibleRows.findIndex(row => row.key === key)
    }
    function labelForKey(key) {
        const index = rowIndexForKey(key)
        return drumView && index >= 0 ? visibleRows[index].name : noteName(key)
    }
    function noteAt(step, key) {
        const tick = step * gridTicks
        return notes.find(note => note.key === key && note.startTicks === tick) || null
    }
    function focusNotes() {
        if (notes.length === 0) return
        const unmapped = drumView
            ? notes.filter(note => rowIndexForKey(Number(note.key)) < 0) : []
        const pitches = (unmapped.length > 0 ? unmapped : notes)
            .map(note => Number(note.key))
        preferPiano = true
        lowestKey = Math.max(0, Math.min(highestLowKey,
            Math.min(...pitches) - 5))
    }
    Keys.onUpPressed: scrollKeys(1)
    Keys.onDownPressed: scrollKeys(-1)
    onDrumEngineIdChanged: preferPiano = false
    onSelectedClipIdChanged: {
        clearSelection()
        preferPiano = false
    }
    // Notes that went away (deleted, undone) leave the selection.
    onNotesChanged: {
        const present = notes.map(note => String(note.id))
        const kept = selectedNoteIds.filter(id => present.indexOf(id) >= 0)
        if (kept.length !== selectedNoteIds.length)
            selectedNoteIds = kept
        if (selectedNoteId.length > 0 && present.indexOf(selectedNoteId) < 0)
            selectedNoteId = kept.length > 0 ? kept[kept.length - 1] : ""
        trackNoteMotion()
    }
    // Motion (ADR-0059): notes are rebuilt with every edit, so arrivals and
    // removals are found here by id. A new note pops in; a removed one
    // leaves a fading ghost. Another clip, or the first look at one,
    // animates nothing.
    property var noteShapes: ({})
    property string shapesClip: ""
    property var arrivedNotes: ({})
    property int fadingSerial: 0
    ListModel { id: fadingNotes }
    function trackNoteMotion() {
        const clip = selectedClip ? String(selectedClip.id) : ""
        const sameClip = clip === shapesClip && clip !== ""
        const next = {}
        const arrived = {}
        for (const note of notes) {
            const id = String(note.id)
            next[id] = {start: Number(note.startTicks), duration: Number(note.durationTicks),
                        key: Number(note.key)}
            if (sameClip && noteShapes[id] === undefined)
                arrived[id] = true
        }
        if (sameClip && Motion.enabled) {
            for (const id in noteShapes) {
                if (next[id] === undefined && fadingNotes.count < 256) {
                    const shape = noteShapes[id]
                    fadingNotes.append({start: shape.start, duration: shape.duration,
                                        key: shape.key, token: ++fadingSerial})
                }
            }
        }
        noteShapes = next
        shapesClip = clip
        arrivedNotes = arrived
    }
    function dropFading(token) {
        for (let index = 0; index < fadingNotes.count; ++index) {
            if (fadingNotes.get(index).token === token) {
                fadingNotes.remove(index)
                return
            }
        }
    }
    // The bridge's note selection drives copy, cut, paste and duplicate.
    onSelectedNoteIdsChanged: {
        bridge.selectedNoteClipId = selectedNoteIds.length > 0 ? selectedClipId : ""
        bridge.selectedNoteIds = selectedNoteIds
    }
    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: roll.visible && roll.notes.length > 0
        onActivated: roll.selectAll()
    }
    Shortcut {
        sequence: "Escape"
        enabled: roll.visible && roll.selectedNoteIds.length > 0
        onActivated: roll.clearSelection()
    }
    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => roll.scrollFromWheel(event)
    }

    Text {
        x: Theme.pianoRollTitleX; y: Theme.pianoRollTitleY
        text: roll.drumView ? "DRUM GRID · PULSE"
            : "PIANO ROLL  ·  " + roll.noteName(Number(roll.visibleRows[roll.visibleRows.length - 1].key)) + "–"
              + roll.noteName(Number(roll.visibleRows[0].key))
        color: roll.mainTextColor
        font.pixelSize: Theme.typeBody; font.bold: true
    }
    Repeater {
        model: roll.drumCapable ? ["PIANO", "DRUMS"] : []
        delegate: Rectangle {
            id: viewButton
            required property string modelData
            required property int index
            readonly property bool chosen: (index === 1) === roll.drumView
            objectName: index === 1 ? "midiDrumViewButton" : "midiPianoViewButton"
            x: Theme.pianoRollViewButtonX + index * Theme.pianoRollViewButtonStride; y: Theme.spaceMd
            width: Theme.pianoRollViewButtonWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
            color: chosen ? Theme.toggleOn : Theme.toggleOff
            border.color: activeFocus ? roll.accentColor : roll.lineColor
            activeFocusOnTab: roll.visible
            Accessible.role: Accessible.Button
            Accessible.name: "Show MIDI " + modelData.toLowerCase() + " view"
            Keys.onReturnPressed: roll.preferPiano = index === 0
            Keys.onSpacePressed: roll.preferPiano = index === 0
            Text { anchors.centerIn: parent; text: viewButton.modelData;
                color: viewButton.chosen ? roll.accentColor : roll.mainTextColor;
                font.pixelSize: Theme.typeCaption; font.bold: true }
            Tip { text: viewButton.index === 0 ? "Piano view: every MIDI pitch"
                                              : "Drum view: the kit's pads as named lanes" }
            MouseArea { anchors.fill: parent; onClicked: roll.preferPiano = viewButton.index === 0 }
        }
    }
    Text {
        x: Theme.pianoRollGridLabelX; y: Theme.pianoRollHeaderLabelY
        text: "GRID"
        color: roll.dimTextColor
        font.pixelSize: Theme.typeSmall; font.bold: true
    }
    Repeater {
        // The grids come from application::midi_grid_steps_per_bar, the
        // list the bridge accepts; six fit between GRID and Quantise.
        model: roll.noteTools !== null ? roll.noteTools.gridDivisions : []
        delegate: Rectangle {
            id: gridButton
            required property int modelData
            required property int index
            objectName: "pianoGrid" + modelData
            x: Theme.pianoRollGridButtonX + index * Theme.pianoRollGridButtonStride; y: Theme.spaceMd
            width: Theme.pianoRollGridButtonWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
            color: roll.gridSteps === modelData ? Theme.toggleOn : Theme.toggleOff
            border.color: activeFocus ? roll.accentColor : roll.lineColor
            activeFocusOnTab: roll.visible
            Accessible.role: Accessible.Button
            Accessible.name: "Set piano roll grid to one " + modelData + "th note"
            Keys.onReturnPressed: roll.gridSteps = modelData
            Keys.onSpacePressed: roll.gridSteps = modelData
            Text { anchors.centerIn: parent; text: "1/" + gridButton.modelData;
                color: roll.gridSteps === gridButton.modelData ? roll.accentColor : roll.mainTextColor;
                font.pixelSize: Theme.typeCaption; font.bold: true }
            Tip { text: "Grid: 1/" + gridButton.modelData + " notes for drawing, moving and Quantise" }
            MouseArea { anchors.fill: parent; onClicked: roll.gridSteps = gridButton.modelData }
        }
    }
    Rectangle {
        id: quantiseButton
        objectName: "pianoQuantise"
        x: Theme.pianoRollQuantiseX; y: Theme.spaceMd; width: Theme.pianoRollQuantiseWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        color: Theme.noteActionFill
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && roll.notes.length > 0
        Accessible.role: Accessible.Button
        Accessible.name: "Quantise " + (roll.selectedNoteIds.length > 0 ? "the selected notes"
                                                                        : "all notes in the clip")
                         + " to the one " + roll.gridSteps + "th note grid at "
                         + Math.round(toolsPanel.quantizeStrength * 100) + "% strength"
                         + (toolsPanel.quantizeSwing > 0.5
                            ? " and " + Math.round(toolsPanel.quantizeSwing * 100) + "% swing" : "")
                         + ", one undo step"
        Keys.onReturnPressed: roll.quantise()
        Keys.onSpacePressed: roll.quantise()
        Text { anchors.centerIn: parent; text: "QUANTISE"; color: roll.accentColor;
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: quantiseButton.Accessible.name + ". Strength and swing are in Tools" }
        MouseArea {
            anchors.fill: parent
            enabled: roll.notes.length > 0
            onClicked: roll.quantise()
        }
    }
    Rectangle {
        objectName: "pianoFocusNotes"
        x: Theme.pianoRollFindNotesX; y: Theme.spaceMd; width: Theme.pianoRollFindNotesWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        color: Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && roll.notes.length > 0
        Accessible.role: Accessible.Button
        Accessible.name: roll.drumView ? "Show all MIDI notes in the piano view"
            : "Scroll piano roll to the pitches used by this clip"
        Keys.onReturnPressed: roll.focusNotes()
        Keys.onSpacePressed: roll.focusNotes()
        Text { anchors.centerIn: parent; text: roll.drumView ? "ALL NOTES" : "FIND NOTES";
            color: roll.mainTextColor; font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip {}
        MouseArea { anchors.fill: parent; enabled: roll.notes.length > 0;
            onClicked: roll.focusNotes() }
    }
    Rectangle {
        // Loop: off, the clip's right edge sets its length; on, it repeats
        // the pattern (Live's Loop switch).
        id: loopButton
        objectName: "pianoLoopToggle"
        x: Theme.pianoRollLoopX; y: Theme.spaceMd; width: Theme.pianoRollLoopWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        color: roll.looping ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && roll.selectedClipId.length > 0
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: roll.looping
        Accessible.name: roll.looping
            ? "Loop is on: the clip repeats its pattern. Turn off so the clip's right edge sets its length. One undo step."
            : "Loop is off: the clip's right edge sets its length. Turn on to repeat the whole clip. One undo step."
        function activate() { roll.bridge.set_midi_clip_loop(roll.selectedClipId, !roll.looping) }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "↻ LOOP"
            color: roll.looping ? roll.accentColor : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: roll.looping ? "Loop on: the clip repeats its pattern"
                                 : "Loop off: the clip's right edge sets its length" }
        MouseArea { anchors.fill: parent; enabled: roll.selectedClipId.length > 0
            onClicked: loopButton.activate() }
    }
    Rectangle {
        // Fold to the project key's scale (Live's Fold to Scale).
        id: scaleButton
        objectName: "pianoFoldToScale"
        x: Theme.pianoRollFoldX; y: Theme.spaceMd; width: Theme.pianoRollFoldWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        visible: !roll.drumView
        color: roll.folded ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        opacity: roll.scaleKnown ? 1 : Theme.pianoRollUnavailableOpacity
        activeFocusOnTab: roll.visible && roll.scaleKnown
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: roll.folded
        Accessible.name: roll.scaleKnown
            ? "Fold the piano roll to the project key: show only in-key rows and rows with notes"
            : "Set a project key to fold the piano roll to its scale"
        function activate() { if (roll.scaleKnown) roll.foldToScale = !roll.foldToScale }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent
            text: "FOLD · " + (roll.scaleKnown ? roll.bridge.projectKey.split(" ")[0] : "KEY")
            color: roll.folded ? roll.accentColor : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: roll.scaleKnown ? "Fold: show only in-key rows (and rows with notes)"
                                    : "Fold: set a song key first (Tools)" }
        MouseArea { anchors.fill: parent; enabled: roll.scaleKnown; onClicked: scaleButton.activate() }
    }
    Rectangle {
        // Scale lock: drawn and moved notes stay in the key. With no key
        // set it opens the tools, where the key is chosen.
        id: lockButton
        objectName: "pianoScaleLock"
        x: Theme.pianoRollLockX; y: Theme.spaceMd; width: Theme.pianoRollLockWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        visible: !roll.drumView
        color: roll.scaleLocked ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && !roll.drumView
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: roll.scaleLocked
        Accessible.name: roll.scaleKnown
            ? "Scale lock: drawn, moved and transposed notes stay in " + roll.bridge.projectKey
            : "Choose a song key to lock notes to its scale"
        function activate() {
            if (roll.scaleKnown) roll.noteTools.scaleLock = !roll.noteTools.scaleLock
            else roll.toolsOpen = true
        }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "LOCK"
            color: roll.scaleLocked ? roll.accentColor : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: roll.scaleKnown
                  ? "Scale lock " + (roll.scaleLocked ? "on" : "off") + ": drawn and moved notes stay in "
                    + roll.bridge.projectKey
                  : "Scale lock: choose a song key in Tools first" }
        MouseArea { anchors.fill: parent; onClicked: lockButton.activate() }
    }
    Rectangle {
        // Chord stamp: on, a click on the grid draws the chosen chord.
        id: chordButton
        objectName: "pianoChordStamp"
        x: Theme.pianoRollChordX; y: Theme.spaceMd; width: Theme.pianoRollChordWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        visible: !roll.drumView
        color: roll.chordStamping ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && !roll.drumView
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: roll.chordStamping
        Accessible.name: roll.chordStamping
            ? "Chord stamp is on: a click draws a " + toolsPanel.chordShape + " chord. Turn off to draw single notes."
            : "Chord stamp: a click draws a " + toolsPanel.chordShape + " chord"
              + (roll.scaleKnown ? " in " + roll.bridge.projectKey : "")
        function activate() { toolsPanel.chordMode = !toolsPanel.chordMode }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent
            text: roll.chordStamping ? toolsPanel.chordShape.toUpperCase() : "CHORD"
            color: roll.chordStamping ? roll.accentColor : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: (roll.chordStamping ? "Chord stamp on: " : "Chord stamp: ")
                    + "a click draws a " + toolsPanel.chordShape + " chord"
                    + (roll.scaleKnown ? " in " + roll.bridge.projectKey : "")
                    + " · shape and inversion in Tools" }
        MouseArea { anchors.fill: parent; onClicked: chordButton.activate() }
    }
    Rectangle {
        id: toolsButton
        objectName: "pianoToolsButton"
        x: roll.drumView ? Theme.pianoRollDrumToolsX : Theme.pianoRollToolsX; y: Theme.spaceMd; width: Theme.pianoRollToolsWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        color: roll.toolsOpen ? Theme.toggleOn : Theme.toggleOff
        border.color: activeFocus ? roll.accentColor : roll.lineColor
        activeFocusOnTab: roll.visible && roll.selectedClipId.length > 0
        Accessible.role: Accessible.Button
        Accessible.name: "Tools: key and scale lock, chord stamps, strum, humanise, quantise strength and swing, and grooves"
        function activate() { roll.toolsOpen = !roll.toolsOpen }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "TOOLS ▾"
            color: roll.toolsOpen ? roll.accentColor : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: "Tools: key, chords, strum, humanise, quantise and grooves" }
        MouseArea { anchors.fill: parent; enabled: roll.selectedClipId.length > 0
            onClicked: toolsButton.activate() }
    }
    Repeater {
        // Pedal helpers: legato re-pedalling per chord, and clear.
        model: roll.pedalsShown
            ? [{name: "PEDAL/CHORD", object: "pianoPedalPerChord",
                help: "Pedal every chord: press just after it lands, lift as the next lands. One undo step."},
               {name: "CLEAR PEDALS", object: "pianoClearPedals",
                help: "Remove every pedal from this clip. One undo step."}]
            : []
        delegate: Rectangle {
            id: pedalButton
            required property var modelData
            required property int index
            objectName: modelData.object
            x: Theme.pianoRollPedalButtonX + index * Theme.pianoRollPedalButtonStride; y: Theme.spaceMd; width: Theme.pianoRollPedalButtonWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
            color: Theme.pianoRollPedalButtonFill
            border.color: activeFocus ? roll.accentColor : roll.lineColor
            activeFocusOnTab: roll.visible
            Accessible.role: Accessible.Button
            Accessible.name: modelData.help
            function activate() {
                if (index === 0) roll.bridge.noteTools.pedal_per_chord(roll.selectedClipId)
                else roll.bridge.noteTools.clear_pedals(roll.selectedClipId)
            }
            Keys.onReturnPressed: activate()
            Keys.onSpacePressed: activate()
            Text { anchors.centerIn: parent; text: pedalButton.modelData.name
                color: roll.amberColor; font.pixelSize: Theme.typeSmall; font.bold: true }
            Tip { text: pedalButton.modelData.help }
            MouseArea { anchors.fill: parent; onClicked: pedalButton.activate() }
        }
    }
    Text {
        x: roll.drumView ? toolsButton.x + toolsButton.width + Theme.pianoRollHintGap : Theme.pianoRollHintX; y: Theme.pianoRollHeaderLabelY
        text: roll.unmappedNotes > 0
            ? roll.unmappedNotes + " UNMAPPED NOTES · OPEN PIANO TO EDIT"
            : roll.drumView ? "PAD LANES  ·  DRAG: MOVE  ·  DEL: REMOVE"
                            : "WHEEL: PITCH  ·  DRAG: MOVE  ·  DEL: REMOVE"
        color: roll.unmappedNotes > 0 ? roll.amberColor : roll.dimTextColor
        font.pixelSize: Theme.typeCaption
        visible: roll.width > x + (roll.unmappedNotes > 0 ? Theme.pianoRollUnmappedHintRoom : Theme.pianoRollHintRoom)
    }

    // The key strip plays the clip's instrument: press to sound a key,
    // slide for a glissando, release to stop.
    MouseArea {
        objectName: "pianoRollKeys"
        x: 0; y: roll.rowTop
        z: 2
        width: roll.keyboardWidth
        height: roll.visibleRows.length * roll.rowHeight
        enabled: roll.selectedClipId !== ""
        preventStealing: true
        onPressed: mouse => roll.playKey(Number(roll.keyAt(roll.rowTop + mouse.y)), mouse.x)
        onPositionChanged: mouse => {
            if (pressed)
                roll.playKey(Number(roll.keyAt(roll.rowTop + mouse.y)), mouse.x)
        }
        onReleased: roll.playKey(-1, 0)
        onCanceled: roll.playKey(-1, 0)
    }

    Repeater {
        model: roll.visibleRows.length
        delegate: Item {
            id: row
            required property int index
            readonly property var lane: roll.visibleRows[index]
            readonly property int key: Number(lane.key)
            x: 0; y: roll.rowTop + index * roll.rowHeight
            width: roll.width; height: roll.rowHeight
            Rectangle {
                width: roll.keyboardWidth; height: parent.height
                color: row.key === roll.playingKey ? roll.accentColor
                    : roll.drumView ? (row.index % 2 === 0 ? Theme.pianoRollKeyDrumEven : Theme.pianoRollKeyDrumOdd)
                    : row.key % 12 === 0 ? Theme.pianoRollKeyC
                    : [1, 3, 6, 8, 10].includes(row.key % 12) ? Theme.pianoRollKeyBlack : Theme.pianoRollKeyWhite
                border.color: roll.lineColor
                // In-key pitches carry an accent mark at the strip's edge,
                // the tonic a full one.
                Rectangle {
                    visible: roll.scaleKnown && !roll.drumView && roll.inScale(row.key)
                    anchors.right: parent.right
                    width: Theme.pianoRollScaleMarkWidth
                    height: parent.height
                    color: roll.accentColor
                    opacity: ((row.key % 12) === roll.scale[0]) ? Theme.pianoRollTonicMarkOpacity : Theme.pianoRollScaleMarkOpacity
                }
                Text {
                    x: roll.drumView ? Theme.spaceMd : 0
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (roll.drumView ? Theme.spaceLg : 0)
                    text: roll.drumView
                        ? row.lane.name + "  ·  " + row.key
                        : roll.noteName(row.key)
                    horizontalAlignment: roll.drumView ? Text.AlignLeft : Text.AlignHCenter
                    color: roll.mainTextColor; font.pixelSize: Theme.typeCaption
                    elide: Text.ElideRight
                }
            }
            // The row's ground: key colouring only; the grid lines are
            // drawn once over every row (gridLines) and the region after the
            // pattern's end greyed once (pianoPatternGround).
            Rectangle {
                x: roll.keyboardWidth
                width: roll.gridWidth; height: parent.height
                // With a project key, out-of-key rows sit darker.
                readonly property bool outOfKey: roll.scaleKnown && !roll.drumView
                                                 && !roll.inScale(row.key)
                color: outOfKey ? Theme.pianoRollRowOutOfKey : row.key % 12 === 0 ? Theme.pianoRollRowC : Theme.pianoRollRow
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: Theme.hairline
                    color: Theme.pianoRollRowLine
                }
            }
            // One hit area per row: the step is worked out from x, so a fine
            // grid costs nothing per cell. Steps after the pattern's end take
            // no notes.
            MouseArea {
                id: rowArea
                objectName: "pianoRollRow-" + row.key
                readonly property int columns: roll.columns
                x: roll.keyboardWidth
                width: roll.gridWidth; height: parent.height
                scrollGestureEnabled: false
                property int pressedStep: -1
                function stepAt(localX) {
                    return Math.max(0, Math.min(roll.columns - 1,
                                                Math.floor(localX * roll.columns / width)))
                }
                Accessible.role: Accessible.Button
                Accessible.name: "Add " + row.lane.name + " on the 1/" + roll.gridSteps
                                 + " grid: click a step before the clip's end"
                // Drawing a note plays it while the button is down.
                onPressed: mouse => {
                    const step = stepAt(mouse.x)
                    if (step >= roll.patternSteps) {
                        pressedStep = -1
                        mouse.accepted = false
                        return
                    }
                    pressedStep = step
                    const key = roll.drawKey(row.key)
                    if (roll.selectedNoteIds.length <= 1 && roll.noteAt(step, key) === null)
                        roll.sound(key, 100)
                }
                onReleased: roll.sound(-1, 0)
                onCanceled: {
                    pressedStep = -1
                    roll.sound(-1, 0)
                }
                onClicked: {
                    const step = pressedStep
                    pressedStep = -1
                    if (step < 0)
                        return
                    if (roll.selectedNoteIds.length > 1) {
                        // With a set selected, a click on the grid first
                        // lets go of it.
                        roll.clearSelection()
                        return
                    }
                    const key = roll.drawKey(row.key)
                    if (roll.chordStamping) {
                        const drawn = roll.noteTools.stamp_chord(
                            roll.selectedClipId, step, key, roll.gridSteps,
                            toolsPanel.chordShape, toolsPanel.chordInversion,
                            Math.max(1, roll.gridSteps / 4))
                        if (drawn.length > 0) {
                            roll.selectedNoteIds = drawn
                            roll.selectedNoteId = drawn[0]
                        }
                        return
                    }
                    if (roll.noteAt(step, key) === null) {
                        roll.bridge.toggle_note_grid(roll.selectedClipId, step, key, roll.gridSteps)
                        // The drawn note becomes the selection, so the
                        // tools (Quantise, Strum…) act on what was just drawn.
                        const drawn = roll.noteAt(step, key)
                        roll.selectOnly(drawn !== null ? String(drawn.id) : "")
                    }
                }
                onWheel: wheel => roll.scrollFromWheel(wheel)
            }
        }
    }
    // After the pattern's end the grid is greyed and takes no notes.
    Rectangle {
        objectName: "pianoPatternGround"
        readonly property real startX: roll.gridWidth * roll.patternSteps / roll.columns
        visible: roll.patternSteps < roll.columns
        z: 0.5
        x: roll.keyboardWidth + startX
        y: roll.rowTop
        width: roll.gridWidth - startX
        height: roll.visibleRows.length * roll.rowHeight
        color: Theme.pianoRollPatternGround
    }
    // The grid's vertical lines, once over every row: bars brightest, then
    // beats, then steps. A fine grid draws only every stride-th step line,
    // the finest that stays minLineSpacing apart, so 1/128 never turns into
    // a grey wash; beat and bar lines always show.
    Item {
        id: gridLines
        objectName: "pianoGridLines"
        readonly property real minLineSpacing: Theme.pianoRollGridMinLineSpacing
        readonly property real stepWidth: roll.gridWidth / roll.columns
        readonly property int beatSteps: Math.max(1, roll.gridSteps / 4)
        // Grid steps between drawn step lines: a power of two up to a beat.
        readonly property int stride: {
            let every = 1
            while (every < beatSteps && every * stepWidth < minLineSpacing)
                every *= 2
            return every
        }
        // A beat too narrow for its own line leaves only bar lines.
        readonly property int lineSteps: stride >= beatSteps && beatSteps * stepWidth < minLineSpacing
                                         ? roll.gridSteps : stride
        z: 1
        x: roll.keyboardWidth; y: roll.rowTop
        width: roll.gridWidth
        height: roll.visibleRows.length * roll.rowHeight
        Repeater {
            model: Math.ceil(roll.columns / gridLines.lineSteps)
            delegate: Rectangle {
                required property int index
                readonly property int step: index * gridLines.lineSteps
                readonly property bool bar: step % roll.gridSteps === 0
                readonly property bool beat: step % gridLines.beatSteps === 0
                x: gridLines.stepWidth * step
                width: Theme.hairline; height: gridLines.height
                color: bar ? Theme.pianoRollGridBarLine : beat ? Theme.pianoRollGridBeatLine : Theme.pianoRollGridStepLine
            }
        }
    }

    // Ringing tails: how long each note sounds under the pedals.
    Repeater {
        model: roll.pedalsShown ? roll.notes : []
        delegate: Rectangle {
            required property var modelData
            readonly property int rowIndex: roll.rowIndexForKey(Number(modelData.key))
            readonly property real noteEnd: Number(modelData.startTicks)
                                            + Number(modelData.durationTicks)
            readonly property real ringEnd: Number(modelData.soundingEndTicks || noteEnd)
            objectName: "pianoRingingTail-" + String(modelData.id)
            visible: rowIndex >= 0 && ringEnd > noteEnd
            z: 2
            x: roll.xAt(noteEnd)
            y: roll.rowTop + rowIndex * roll.rowHeight + roll.rowHeight * Theme.pianoRollTailTop
            width: roll.gridWidth * Math.min(ringEnd - noteEnd, roll.viewTicks - noteEnd)
                   / roll.viewTicks
            height: roll.rowHeight * Theme.pianoRollTailHeight
            radius: Theme.radiusXs
            color: Theme.pianoRollTail
            opacity: Theme.pianoRollTailOpacity
        }
    }

    Repeater {
        model: roll.notes
        delegate: Rectangle {
            id: noteBlock
            required property var modelData
            readonly property int noteKey: Number(modelData.key)
            readonly property int rowIndex: roll.rowIndexForKey(noteKey)
            readonly property bool inView: rowIndex >= 0
            readonly property string noteId: String(modelData.id)
            // Kept after the pattern's end: silent, shown greyed.
            readonly property bool silent: Number(modelData.startTicks) >= roll.patternTicks
            visible: inView
            opacity: silent ? Theme.pianoRollSilentNoteOpacity : 1
            z: 3
            // A new note pops in from its start.
            transformOrigin: Item.Left
            Component.onCompleted: {
                if (Motion.enabled)
                    Qt.callLater(() => {
                        if (roll.arrivedNotes[noteBlock.noteId] === true)
                            notePop.restart()
                    })
            }
            NumberAnimation {
                id: notePop
                target: noteBlock; property: "scale"; from: Theme.pianoRollNotePopFrom; to: 1
                duration: Motion.fast; easing.type: Easing.OutBack
            }
            x: roll.xAt(Number(modelData.startTicks))
            y: roll.rowTop + rowIndex * roll.rowHeight + Theme.pianoRollNoteInset
            width: Math.max(Theme.pianoRollNoteMinWidth, roll.gridWidth * (roll.resizingNoteId === noteId
                ? roll.resizingEndStep * roll.gridTicks - Number(modelData.startTicks)
                : Number(modelData.durationTicks)) / roll.viewTicks)
            height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
            radius: Theme.radiusSm
            // Louder notes are brighter, so dynamics read at a glance.
            readonly property real loudness: Number(modelData.velocity) / 127
            readonly property bool selected: roll.isSelected(noteId)
            color: selected ? Theme.pianoRollNoteSelected
                : Qt.rgba(Theme.pianoRollNoteBase[0] + Theme.pianoRollNoteLoudnessGain[0] * loudness,
                          Theme.pianoRollNoteBase[1] + Theme.pianoRollNoteLoudnessGain[1] * loudness,
                          Theme.pianoRollNoteBase[2] + Theme.pianoRollNoteLoudnessGain[2] * loudness, 1)
            border.color: selected ? roll.amberColor : Theme.pianoRollNoteEdge
            Text {
                anchors.verticalCenter: parent.verticalCenter; x: Theme.spaceXs
                text: roll.labelForKey(noteBlock.noteKey)
                color: Theme.pianoRollNoteText; font.pixelSize: Theme.typeCaption; font.bold: true
                visible: noteBlock.width > Theme.pianoRollNoteLabelMinWidth
            }
            Tip {
                anchorItem: noteBlock
                text: roll.labelForKey(noteBlock.noteKey) + " · velocity " + noteBlock.modelData.velocity
                      + (noteBlock.silent ? " · after the clip's end, silent" : "")
                      + " — drag moves (Ctrl copies), right edge resizes, double-click removes"
            }
            MouseArea {
                objectName: "pianoRollNote-" + noteBlock.noteId
                anchors.fill: parent
                scrollGestureEnabled: false
                activeFocusOnTab: noteBlock.visible
                property bool moved: false
                property bool copying: false
                property real pressX: 0
                property real pressY: 0
                Accessible.role: Accessible.Button
                Accessible.name: "MIDI note " + roll.labelForKey(noteBlock.noteKey)
                                 + (noteBlock.silent ? ", after the clip's end, silent" : "")
                                 + ". Drag to move, Delete to remove, double click to remove."
                // Delete on a focused note outside the selection removes that note.
                function deleteFocused() {
                    if (!roll.isSelected(noteBlock.noteId))
                        roll.selectOnly(noteBlock.noteId)
                    roll.deleteSelected()
                }
                Keys.onDeletePressed: deleteFocused()
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Backspace) {
                        deleteFocused()
                        event.accepted = true
                    }
                }
                onPressed: mouse => {
                    forceActiveFocus()
                    const point = mapToItem(roll, mouse.x, mouse.y)
                    pressX = point.x; pressY = point.y
                    moved = false
                    roll.noteDragValid = false
                    roll.noteDragDuration = Number(noteBlock.modelData.durationTicks)
                    roll.noteDragFromStep = Math.round(Number(noteBlock.modelData.startTicks) / roll.gridTicks)
                    roll.noteDragFromKey = noteBlock.noteKey
                    // Ctrl decides on release: a drag copies, a click adds or
                    // removes the note from the selection. Shift adds or
                    // removes at once; a plain press on an unselected note
                    // selects only it, on a selected one keeps the set so it
                    // can be dragged together.
                    copying = (mouse.modifiers & Qt.ControlModifier) !== 0
                    // A grabbed note plays while held, at its dragged pitch.
                    roll.sound(noteBlock.noteKey, Number(noteBlock.modelData.velocity))
                    if (copying)
                        return
                    if (mouse.modifiers & Qt.ShiftModifier)
                        roll.toggleSelected(noteBlock.noteId)
                    else if (!noteBlock.selected)
                        roll.selectOnly(noteBlock.noteId)
                    else
                        roll.selectedNoteId = noteBlock.noteId
                }
                onPositionChanged: mouse => {
                    if (!pressed) return
                    const point = mapToItem(roll, mouse.x, mouse.y)
                    const starting = !moved && (Math.abs(point.x - pressX) >= 5
                                                || Math.abs(point.y - pressY) >= 5)
                    moved = moved || starting
                    if (!moved) return
                    if (starting && copying) {
                        // An unselected note is copied alone.
                        if (!noteBlock.selected)
                            roll.selectOnly(noteBlock.noteId)
                        roll.noteDragCopy = true
                    }
                    roll.noteDragValid = point.x >= roll.keyboardWidth
                        && point.x < roll.keyboardWidth + roll.gridWidth
                        && point.y >= roll.rowTop
                        && point.y < roll.rowTop + roll.visibleRows.length * roll.rowHeight
                    if (roll.noteDragValid) {
                        roll.noteDragStep = roll.stepAt(point.x)
                        roll.noteDragKey = roll.drawKey(roll.keyAt(point.y))
                        roll.sound(Number(roll.noteDragKey), Number(noteBlock.modelData.velocity))
                    }
                }
                onReleased: mouse => {
                    if (moved && roll.noteDragValid && copying) {
                        const source = roll.selectedNoteIds.length > 0 ? roll.selectedNoteIds
                                                                       : [noteBlock.noteId]
                        const copies = roll.bridge.copy_notes_grid(
                            roll.selectedClipId, source,
                            roll.noteDragStep - roll.noteDragFromStep,
                            roll.noteDragKey - roll.noteDragFromKey, roll.gridSteps)
                        if (copies.length > 0) {
                            roll.selectedNoteIds = copies
                            roll.selectedNoteId = copies[copies.length - 1]
                        }
                    } else if (moved && roll.noteDragValid) {
                        if (roll.selectedNoteIds.length > 1 && noteBlock.selected && roll.scaleLocked)
                            roll.noteTools.move_notes_in_scale(roll.selectedClipId, roll.selectedNoteIds,
                                                               roll.noteDragStep - roll.noteDragFromStep,
                                                               roll.noteDragFromKey, roll.noteDragKey,
                                                               roll.gridSteps)
                        else if (roll.selectedNoteIds.length > 1 && noteBlock.selected)
                            roll.bridge.move_notes_grid(roll.selectedClipId, roll.selectedNoteIds,
                                                        roll.noteDragStep - roll.noteDragFromStep,
                                                        roll.noteDragKey - roll.noteDragFromKey,
                                                        roll.gridSteps)
                        else
                            roll.bridge.move_note_grid(roll.selectedClipId, noteBlock.noteId,
                                                       roll.noteDragStep, roll.noteDragKey,
                                                       roll.gridSteps)
                    } else if (!moved && copying) {
                        roll.toggleSelected(noteBlock.noteId)
                    } else if (!moved && !(mouse.modifiers & Qt.ShiftModifier)) {
                        // A click (no drag) on a note in a set selects just it.
                        roll.selectOnly(noteBlock.noteId)
                    }
                    roll.noteDragValid = false
                    roll.noteDragCopy = false
                    copying = false
                    roll.sound(-1, 0)
                }
                onDoubleClicked: roll.bridge.remove_note_id(roll.selectedClipId, noteBlock.noteId)
                onCanceled: {
                    roll.noteDragValid = false
                    roll.noteDragCopy = false
                    copying = false
                    roll.sound(-1, 0)
                }
                onWheel: wheel => roll.scrollFromWheel(wheel)
            }
            Rectangle {
                x: parent.width - width
                width: Theme.pianoRollNoteHandleWidth; height: parent.height
                radius: Theme.radiusXs
                color: noteBlock.selected ? Theme.pianoRollNoteHandleSelected : Theme.pianoRollNoteHandle
                opacity: Theme.pianoRollNoteHandleOpacity
                MouseArea {
                    objectName: "pianoNoteResizeHandle-" + noteBlock.noteId
                    anchors.fill: parent
                    scrollGestureEnabled: false
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true
                    onPressed: {
                        forceActiveFocus()
                        roll.selectOnly(noteBlock.noteId)
                        roll.resizingNoteId = noteBlock.noteId
                        roll.resizingEndStep = Math.max(1, Math.min(roll.patternSteps,
                            Math.round((Number(noteBlock.modelData.startTicks)
                                + Number(noteBlock.modelData.durationTicks)) / roll.gridTicks)))
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const point = mapToItem(roll, mouse.x, mouse.y)
                        roll.resizingEndStep = Math.max(
                            Math.floor(Number(noteBlock.modelData.startTicks) / roll.gridTicks) + 1,
                            Math.min(roll.patternSteps, roll.stepAt(point.x) + 1))
                    }
                    onReleased: {
                        const target = roll.resizingEndStep
                        roll.resizingNoteId = ""
                        if (target * roll.gridTicks !== Number(noteBlock.modelData.startTicks)
                                + Number(noteBlock.modelData.durationTicks))
                            roll.bridge.resize_note_grid(roll.selectedClipId, noteBlock.noteId,
                                                         target, roll.gridSteps)
                    }
                    onCanceled: roll.resizingNoteId = ""
                    onWheel: wheel => roll.scrollFromWheel(wheel)
                }
            }
        }
    }
    // Removed notes fade where they were.
    Repeater {
        model: fadingNotes
        delegate: Rectangle {
            id: fading
            required property real start
            required property real duration
            required property int key
            required property int token
            readonly property int rowIndex: roll.rowIndexForKey(key)
            visible: rowIndex >= 0
            z: 3
            x: roll.xAt(start)
            y: roll.rowTop + rowIndex * roll.rowHeight + Theme.pianoRollNoteInset
            width: Math.max(Theme.pianoRollNoteMinWidth, roll.gridWidth * duration / roll.viewTicks)
            height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
            radius: Theme.radiusSm
            color: Theme.pianoRollNoteEdge
            opacity: Theme.pianoRollFadingNoteOpacity
            ParallelAnimation {
                running: true
                NumberAnimation { target: fading; property: "opacity"; to: 0
                                  duration: Motion.slow; easing.type: Easing.OutQuad }
                NumberAnimation { target: fading; property: "scale"; to: Theme.pianoRollFadingNoteScale
                                  duration: Motion.slow; easing.type: Easing.InQuad }
                onFinished: Qt.callLater(() => roll.dropFading(fading.token))
            }
        }
    }
    // The playhead inside the clip while playing; a looping clip wraps it.
    readonly property real playheadTicks: {
        bridge.playheadSample
        bridge.playbackActive
        return selectedClipId !== "" ? bridge.clip_playhead_ticks(selectedClipId) : -1
    }
    Rectangle {
        objectName: "pianoRollPlayhead"
        x: roll.xAt(roll.playheadTicks) - width / 2
        y: roll.rowTop
        width: Theme.strokeBold
        height: roll.visibleRows.length * roll.rowHeight
        z: 3
        visible: roll.playheadTicks >= 0 && roll.playheadTicks <= roll.viewTicks
        color: roll.amberColor
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.playheadGlowWidth
            height: parent.height
            color: Qt.rgba(roll.amberColor.r, roll.amberColor.g, roll.amberColor.b, Theme.playheadGlowAlpha)
        }
    }
    // Notes being recorded, growing from their start until the key is let go.
    readonly property var recordingNotes: {
        bridge.playheadSample
        const recorder = bridge.recording
        if (!recorder || selectedClipId === "" || recorder.heldNotes.length === 0)
            return []
        return recorder.clip_held_notes(selectedClipId)
    }
    Repeater {
        model: roll.recordingNotes
        delegate: Rectangle {
            required property var modelData
            readonly property int rowIndex: roll.rowIndexForKey(Number(modelData.key))
            visible: rowIndex >= 0
            z: 3
            x: roll.xAt(Number(modelData.startTicks))
            y: roll.rowTop + rowIndex * roll.rowHeight + Theme.pianoRollNoteInset
            width: Math.max(Theme.pianoRollRecordingNoteMinWidth, roll.gridWidth * (Number(modelData.endTicks) - Number(modelData.startTicks))
                                / roll.viewTicks)
            height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
            radius: Theme.radiusSm
            color: roll.recordingNoteColor
            border.color: roll.amberColor
        }
    }
    // Chord stamp: the chord a click would draw, under the pointer.
    Item {
        id: stampArea
        x: roll.keyboardWidth; y: roll.rowTop
        width: roll.gridWidth
        height: roll.visibleRows.length * roll.rowHeight
        z: 2
        HoverHandler {
            id: stampHover
            enabled: roll.chordStamping
        }
        readonly property int step: stampHover.hovered
            ? Math.max(0, Math.min(roll.columns - 1,
                                   Math.floor(stampHover.point.position.x * roll.columns / roll.gridWidth)))
            : -1
        readonly property int root: stampHover.hovered
            ? roll.drawKey(Number(roll.keyAt(roll.rowTop + stampHover.point.position.y))) : -1
        readonly property var keys: roll.chordStamping && step >= 0 && step < roll.patternSteps
                                    && root >= 0 && roll.noteTools !== null
            ? roll.noteTools.chord_preview(root, toolsPanel.chordShape, toolsPanel.chordInversion)
            : []
        Repeater {
            model: stampArea.keys
            delegate: Rectangle {
                id: stampGhost
                required property int modelData
                readonly property int rowIndex: roll.rowIndexForKey(modelData)
                visible: rowIndex >= 0
                x: roll.gridWidth * stampArea.step / roll.columns
                y: rowIndex * roll.rowHeight + Theme.pianoRollNoteInset
                width: Math.max(Theme.pianoRollNoteMinWidth, roll.gridWidth * Math.max(1, roll.gridSteps / 4) / roll.columns)
                height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
                radius: Theme.radiusSm
                color: Qt.rgba(roll.accentColor.r, roll.accentColor.g, roll.accentColor.b, Theme.pianoRollStampWashAlpha)
                border.color: roll.accentColor
                Text {
                    anchors.verticalCenter: parent.verticalCenter; x: Theme.spaceXs
                    visible: parent.width > Theme.pianoRollStampLabelMinWidth
                    text: roll.labelForKey(stampGhost.modelData)
                    color: roll.accentColor; font.pixelSize: Theme.typeCaption; font.bold: true
                }
            }
        }
    }
    // The rest of the selection follows the dragged note.
    Repeater {
        model: roll.noteDragValid && roll.selectedNoteIds.length > 1
            ? roll.notes.filter(note => roll.isSelected(String(note.id))) : []
        delegate: Rectangle {
            required property var modelData
            // Under Scale lock the set moves by scale degrees, as the drop will.
            readonly property int key: roll.scaleLocked
                ? roll.noteTools.moved_key_in_scale(Number(modelData.key), roll.noteDragFromKey,
                                                    roll.noteDragKey)
                : Number(modelData.key) + roll.noteDragKey - roll.noteDragFromKey
            readonly property int rowIndex: roll.rowIndexForKey(key)
            visible: rowIndex >= 0
            z: 4
            x: roll.xAt(Number(modelData.startTicks)
                        + (roll.noteDragStep - roll.noteDragFromStep) * roll.gridTicks)
            y: roll.rowTop + rowIndex * roll.rowHeight + Theme.pianoRollNoteInset
            width: Math.max(Theme.pianoRollNoteMinWidth, roll.gridWidth * Number(modelData.durationTicks) / roll.viewTicks)
            height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
            radius: Theme.radiusSm
            color: roll.accentColor; opacity: Theme.pianoRollFollowerOpacity
            border.color: roll.amberColor
        }
    }
    Rectangle {
        visible: roll.noteDragValid && !(roll.selectedNoteIds.length > 1)
        z: 4
        x: roll.keyboardWidth + roll.gridWidth * roll.noteDragStep / roll.columns
        y: roll.rowTop + roll.rowIndexForKey(roll.noteDragKey) * roll.rowHeight + Theme.pianoRollNoteInset
        width: Math.max(Theme.pianoRollNoteMinWidth, roll.gridWidth * roll.noteDragDuration / roll.viewTicks)
        height: roll.rowHeight - 2 * Theme.pianoRollNoteInset
        radius: Theme.radiusSm
        color: roll.accentColor; opacity: Theme.pianoRollDragGhostOpacity
        border.color: roll.amberColor
        Text {
            visible: roll.noteDragCopy
            anchors.verticalCenter: parent.verticalCenter
            x: Theme.pianoRollCopyMarkX
            text: "+"
            color: Theme.pianoRollNoteText; font.pixelSize: Theme.typeBody; font.bold: true
        }
    }
    // Greys everything after the pattern's end, notes, velocities and pedals
    // alike, as Live greys the region outside a clip; it takes no input.
    Rectangle {
        objectName: "pianoPatternEnd"
        visible: roll.patternTicks < roll.viewTicks
        z: 6
        x: roll.xAt(roll.patternTicks)
        y: roll.rowTop
        width: roll.keyboardWidth + roll.gridWidth - x
        height: roll.height - y
        color: Theme.pianoRollPatternEndVeil
        Rectangle { width: Theme.strokeBold; height: parent.height; color: roll.amberColor; opacity: Theme.pianoRollPatternEndLineOpacity }
    }
    VelocityLane {
        id: velocityLane
        visible: roll.selectedClip !== null && roll.selectedClip !== undefined
        x: 0
        y: roll.rowTop + roll.visibleRows.length * roll.rowHeight + Theme.spaceXxs
        width: roll.width
        bridge: roll.bridge
        clipId: roll.selectedClipId
        notes: roll.notes
        drumView: roll.drumView
        selectedNoteId: roll.selectedNoteId
        keyboardWidth: roll.keyboardWidth
        gridWidth: roll.gridWidth
        spanTicks: roll.viewTicks
        lineColor: roll.lineColor
        mainTextColor: roll.mainTextColor
        dimTextColor: roll.dimTextColor
        accentColor: roll.accentColor
        amberColor: roll.amberColor
        // An unselected stem's note becomes the selection; a stem already in
        // the selection keeps it and just becomes the focused note.
        onNoteSelected: noteId => {
            if (roll.isSelected(noteId))
                roll.selectedNoteId = noteId
            else
                roll.selectOnly(noteId)
        }
    }
    PianoRollTools {
        id: toolsPanel
        visible: roll.toolsOpen && roll.selectedClipId.length > 0
        z: 20
        x: Math.max(Theme.spaceXs, Math.min(roll.width - width - Theme.spaceXs, toolsButton.x + toolsButton.width - width))
        y: Theme.pianoRollToolsY
        height: Math.min(Theme.pianoToolsHeight, roll.height - y - Theme.spaceXs)
        clip: true
        bridge: roll.bridge
        noteTools: roll.noteTools
        clipId: roll.selectedClipId
        selectedNoteIds: roll.selectedNoteIds
        gridSteps: roll.gridSteps
        foldToScale: roll.folded
        lineColor: roll.lineColor
        mainTextColor: roll.mainTextColor
        dimTextColor: roll.dimTextColor
        accentColor: roll.accentColor
        amberColor: roll.amberColor
        onFoldToggled: roll.foldToScale = !roll.foldToScale
        onCloseRequested: roll.toolsOpen = false
    }
    PedalStrip {
        id: pedalStrip
        visible: roll.pedalsShown
        x: 0
        y: velocityLane.y + roll.velocityHeight
        width: roll.width
        bridge: roll.bridge
        clipId: roll.selectedClipId
        pedals: roll.selectedClip ? roll.selectedClip.pedals || ({}) : ({})
        keyboardWidth: roll.keyboardWidth
        gridWidth: roll.gridWidth
        spanTicks: roll.viewTicks
        columns: roll.columns
        stepsPerBar: roll.gridSteps
        patternTicks: roll.patternTicks
        lineColor: roll.lineColor
        mainTextColor: roll.mainTextColor
        dimTextColor: roll.dimTextColor
        accentColor: roll.accentColor
        amberColor: roll.amberColor
    }
    // The roll's clip is the one the chord tools read and write.
    Binding {
        target: roll.harmony
        when: roll.harmony !== null
        property: "clipId"
        value: roll.visible && roll.chordView ? roll.selectedClipId : ""
    }
    Rectangle {
        id: chordsButton
        objectName: "pianoChordsButton"
        visible: roll.chordView
        x: toolsButton.x + toolsButton.width + Theme.spaceSm; y: Theme.spaceMd; width: Theme.pianoRollChordsButtonWidth; height: Theme.pianoRollHeaderButtonHeight; radius: Theme.pianoRollHeaderButtonRadius
        color: roll.chordsOpen ? Theme.pianoRollChordsOpenFill : Theme.toggleOff
        border.color: activeFocus ? Theme.chordAccent : roll.lineColor
        activeFocusOnTab: roll.visible && roll.chordView
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: roll.chordsOpen
        Accessible.name: "Chord tools: write chords from a mood, change the selected chord, give them a rhythm"
        function activate() { roll.chordsOpen = !roll.chordsOpen }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        Text { anchors.centerIn: parent; text: "♪ CHORDS"
            color: roll.chordsOpen ? Theme.chordAccent : roll.mainTextColor
            font.pixelSize: Theme.typeSmall; font.bold: true }
        Tip { text: "Chords: write, change and give rhythm to the chords in this clip" }
        MouseArea { anchors.fill: parent; onClicked: chordsButton.activate() }
    }
    ChordStrip {
        visible: roll.chordStripShown
        z: 7
        x: 0
        y: roll.headerBottom
        width: roll.keyboardWidth + roll.gridWidth
        height: roll.chordStripHeight
        controller: roll.harmony
        keyboardWidth: roll.keyboardWidth
        gridWidth: roll.gridWidth
        spanTicks: roll.viewTicks
        onChordClicked: (startTicks, endTicks) => {
            const ids = roll.notes.filter(note => Number(note.startTicks) >= startTicks
                                                  && Number(note.startTicks) < endTicks)
                                  .map(note => String(note.id))
            roll.selectedNoteIds = ids
            roll.selectedNoteId = ids.length > 0 ? ids[0] : ""
        }
        onChordOpened: {
            roll.chordsOpen = true
            chordPanel.tab = 1
        }
    }
    ChordPanel {
        id: chordPanel
        visible: roll.chordToolsShown
        z: 15
        x: roll.width - width
        y: roll.headerBottom
        width: roll.chordPanelWidth
        height: roll.height - y
        controller: roll.harmony
        onCloseRequested: roll.chordsOpen = false
    }
}
