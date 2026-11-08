pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: arrangement
    // True once the first clips are laid out: later clips (a drop, a paste,
    // an undo) land with a short settle.
    property bool clipsSettled: false
    objectName: "arrangementView"
    required property var bridge
    required property real visibleBars
    required property real viewStartBar
    required property real trackScrollY
    required property int creationHeight
    required property int elapsedRulerHeight
    required property var rowLayout
    required property bool snapToBeat
    // Track id → {open, overlay, keys} (ArrangementWorkspace).
    required property var automationView
    required property bool automationDrawMode
    required property string selectedClipId
    required property int selectedTrack
    required property color canvasColor
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    // The shell's kit theme (devicekit/ShellTheme): track colours and type.
    required property var theme
    readonly property int pianoRollHeight: 0
    // The bar ruler sits level with the track list's header row.
    readonly property int rulerHeight: Theme.timelineHeaderHeight
    readonly property int masterRowHeight: Theme.masterRowHeight
    // Notes being recorded (ADR-0086): their colour, and the keys a lane spans.
    readonly property color recordingNoteColor: Theme.recordingNote
    readonly property int recordingLowestKey: 36
    readonly property int recordingHighestKey: 96
    readonly property int beatsPerBar: 4
    // The finest grid a zoom reaches, as divisions of a bar.
    readonly property int finestGridDivision: 256
    // Bars a phrase groups on the ruler and the lane grid.
    readonly property int barsPerPhrase: 4
    // The ruler labels every bar up to this many bars apart, and at most
    // this many ticks.
    readonly property int maxRulerStepBars: 65536
    readonly property int maxRulerTicks: 400
    // An empty MIDI clip's key span, C3 to C4.
    readonly property var defaultKeySpan: [60, 72]
    // The most repeats of a looping clip's pattern drawn on its card.
    readonly property int maxDrawnPasses: 64
    // Drags shorter than this (px) are clicks: MIDI and grid moves, then
    // audio moves and trims.
    readonly property real clickSlop: 2
    readonly property real audioClickSlop: 4
    // An unsnapped MIDI clip is at least a sixteenth of a bar long.
    readonly property int unsnappedStepsPerBar: 16
    // A trimmed audio clip keeps at least this fraction of the timeline.
    readonly property real minimumClipFraction: 0.001
    // Absorbs rounding when a position lands on a bar or beat.
    readonly property real barEpsilon: 1e-9
    function tint(colour, alpha) {
        return Qt.rgba(colour.r, colour.g, colour.b, alpha)
    }
    property string contextClipId: ""
    property string contextClipKind: "audio"
    property real contextMenuX: 0
    property real contextMenuY: 0
    property string contextClipTimebase: "Elapsed"
    property bool contextClipLoop: false
    property bool levelEditMode: false

    signal zoomInRequested()
    signal zoomOutRequested()
    signal panRequested(int direction)
    // Wheel navigation: zoom by `factor` around `bar`, or scroll by `bars`.
    signal zoomAtRequested(real factor, real bar)
    signal verticalScrollRequested(real pixels)
    signal scrollRequested(real bars)
    signal snapToggled()
    signal clipSelected(string clipId)
    // A group of clips (Shift/Ctrl-click, a drawn box); the last is the
    // clip the inspector shows.
    signal clipsSelected(var clipIds)
    // A double-clicked MIDI clip: open its notes.
    signal clipOpened(string clipId)
    signal trackSelected(int index)
    signal automationDrawToggled()

    function submitViewport() {
        if (bridge && width > 0)
            bridge.set_arrangement_viewport(viewStartBar, visibleBars, Math.ceil(width))
    }
    // The instrument track and zero-based bar under a point of `item`, or
    // null: a MIDI file dropped there plays on that track from that bar.
    function instrumentDropTarget(item, x, y) {
        const point = item.mapToItem(arrangementTrackViewport, x, y)
        if (point.x < 0 || point.x >= arrangementTrackViewport.width
                || point.y < 0 || point.y >= arrangementTrackViewport.height)
            return null
        const at = point.y + trackScrollY
        for (let row = 0; row < rowLayout.length; ++row) {
            const layout = rowLayout[row]
            const track = bridge.tracks[row]
            if (track && track.kind === "instrument" && at >= layout.top && at < layout.top + layout.height)
                return { trackId: track.id,
                         bar: Math.max(0, Math.floor(viewStartBar + point.x * visibleBars / width)) }
        }
        return null
    }
    function createMidiClip(trackId, barIndex) {
        const before = bridge.tracks.find(track => track.id === trackId)
        if (!before)
            return
        const oldIds = before.clips.map(clip => clip.id)
        bridge.add_midi_clip(trackId, barIndex)
        const after = bridge.tracks.find(track => track.id === trackId)
        const created = after ? after.clips.find(clip => !oldIds.includes(clip.id)) : null
        if (created)
            clipSelected(created.id)
    }
    // The viewport re-projects the waveform columns in view, so it is sent
    // once layout settles: a resize or zoom burst becomes one update.
    Component.onCompleted: {
        Qt.callLater(submitViewport)
        Qt.callLater(() => arrangement.clipsSettled = true)
    }
    onViewStartBarChanged: Qt.callLater(submitViewport)
    onVisibleBarsChanged: Qt.callLater(submitViewport)
    onWidthChanged: Qt.callLater(submitViewport)

    // The grid at this zoom, as divisions of a bar: the finest power of two
    // (beats, sixteenths, then down to 1/256) still 12 px wide, as the
    // drawn grid. Zoomed right in, snapping is then a few pixels a step.
    readonly property int gridDivision: {
        let division = 1
        while (division < finestGridDivision && pixelsPerBar / (division * 2) >= Theme.arrangementGridMinSpacing)
            division *= 2
        return division
    }
    function gridStepBars() { return 1 / gridDivision }
    // A clip's start (timeline fraction) moved so its downbeat (or, without
    // a tempo, its start) lands on the grid.
    function snappedClipStart(clip, start) {
        const bars = bridge.timelineBars
        const tempo = bridge.clipTempo.clips[clip.id]
        const offset = tempo && tempo.downbeat >= 0 ? tempo.downbeat * clip.length * bars : 0
        const step = gridStepBars()
        const anchor = start * bars + offset
        return (Math.round(anchor / step) * step - offset) / bars
    }
    // A timeline fraction moved to the nearest grid line.
    function snapToGrid(position) {
        const bars = bridge.timelineBars
        const step = gridStepBars()
        return Math.round(position * bars / step) * step / bars
    }
    function screenDeltaToProject(deltaPixels) {
        return deltaPixels / width * visibleBars / bridge.timelineBars
    }
    function projectDeltaToScreen(delta) {
        return delta * width * bridge.timelineBars / visibleBars
    }
    function selectedClip() {
        for (const track of bridge.tracks) {
            for (const clip of track.clips) {
                if (clip.id === selectedClipId && clip.kind === "midi")
                    return clip
            }
        }
        return null
    }
    function formatElapsed(seconds, includeMillis) {
        if (!Number.isFinite(seconds) || seconds < 0)
            return "--:--"
        const whole = Math.floor(seconds)
        const hours = Math.floor(whole / 3600)
        const minutes = Math.floor(whole / 60) % 60
        const second = whole % 60
        const base = (hours > 0 ? String(hours).padStart(2, "0") + ":" : "")
            + String(minutes).padStart(2, "0") + ":"
            + String(second).padStart(2, "0")
        return includeMillis ? base + "." + String(Math.floor((seconds - whole) * 1000)).padStart(3, "0") : base
    }
    property string contextClipColor: ""
    property string contextClipName: ""
    function clipIsSelected(clipId) {
        return bridge.selectedClipIds.indexOf(clipId) >= 0
    }
    readonly property bool groupSelected: bridge.selectedClipIds.length > 1
    // A press on a clip: Shift or Ctrl adds or removes it from the
    // selection; a plain press on a clip of the selected group keeps the
    // group (to drag it), and on any other clip picks that clip alone.
    function pickClip(clipId, modifiers) {
        const ids = Array.from(bridge.selectedClipIds)
        if ((modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) !== 0) {
            const at = ids.indexOf(clipId)
            if (at >= 0)
                ids.splice(at, 1)
            else
                ids.push(clipId)
            clipsSelected(ids)
        } else if (!(groupSelected && clipIsSelected(clipId))) {
            clipsSelected([clipId])
        }
    }
    // A Ctrl-drag copies rather than moves: the dragged clips show a copy
    // badge and their originals stay drawn where they are.
    property bool copyDragging: false
    // The row of a track, by id, or -1.
    function rowOfTrack(trackId) {
        return bridge.tracks.findIndex(track => track.id === trackId)
    }
    // A selected group follows the clip being dragged: by groupDelta of the
    // timeline and groupRows rows.
    property bool groupDragging: false
    property real groupDelta: 0
    property int groupRows: 0
    property var groupTrackRows: []
    function beginGroupDrag() {
        const rows = []
        for (let row = 0; row < bridge.tracks.length; ++row)
            if (bridge.tracks[row].clips.some(clip => clipIsSelected(clip.id)))
                rows.push(row)
        groupTrackRows = rows
        groupDelta = 0
        groupRows = 0
        groupDragging = true
    }
    function endGroupDrag() {
        groupDragging = false
        groupDelta = 0
        groupRows = 0
        groupTrackRows = []
    }
    // The row under a viewport y (scrolled content), or -1.
    function rowAt(contentY) {
        for (let row = 0; row < rowLayout.length; ++row) {
            const layout = rowLayout[row]
            if (layout && contentY >= layout.top && contentY < layout.top + layout.height)
                return row
        }
        return -1
    }
    // How far a clip in `row` draws when the group moves `rows` rows.
    function rowShiftFor(row, rows) {
        const from = rowLayout[row]
        const to = rowLayout[row + rows]
        return rows !== 0 && from && to ? to.top - from.top : 0
    }
    // Ctrl+A: every audio and MIDI clip on every track. The arrangement
    // takes the keyboard, so the arrow keys then move the selection.
    function selectAllClips() {
        const ids = []
        for (const track of bridge.tracks)
            for (const clip of track.clips)
                ids.push(clip.id)
        clipsSelected(ids)
        forceActiveFocus()
    }
    // Arrow keys: the selection moves one beat, as one undo step.
    function nudgeSelection(direction) {
        if (bridge.selectedClipIds.length > 0)
            bridge.nudge_selected_clips(direction)
    }
    Keys.onLeftPressed: nudgeSelection(-1)
    Keys.onRightPressed: nudgeSelection(1)
    // The selection box drawn on empty lane space, in viewport content
    // coordinates (y includes the track scroll).
    property bool bandActive: false
    property real bandX0: 0
    property real bandY0: 0
    property real bandX1: 0
    property real bandY1: 0
    // Every clip the box touches; with Shift or Ctrl, added to the selection.
    function selectInBand(modifiers) {
        const perBar = visibleBars / Math.max(1, width)
        const from = viewStartBar + Math.min(bandX0, bandX1) * perBar
        const to = viewStartBar + Math.max(bandX0, bandX1) * perBar
        const top = Math.min(bandY0, bandY1)
        const bottom = Math.max(bandY0, bandY1)
        const ids = (modifiers & (Qt.ShiftModifier | Qt.ControlModifier)) !== 0
            ? Array.from(bridge.selectedClipIds) : []
        for (let row = 0; row < bridge.tracks.length; ++row) {
            const layout = rowLayout[row]
            if (!layout || layout.top + layout.height <= top || layout.top >= bottom)
                continue
            for (const clip of bridge.tracks[row].clips) {
                const start = clip.start * bridge.timelineBars
                const end = (clip.start + clip.length) * bridge.timelineBars
                if (start < to && end > from && ids.indexOf(clip.id) < 0)
                    ids.push(clip.id)
            }
        }
        clipsSelected(ids)
    }
    function openClipMenu(clipId, kind, timebase, x, y, looping, color, name) {
        // Right-click picks the clip too, so the outline matches the menu
        // (a clip of the selected group keeps the group, for Merge).
        pickClip(clipId, 0)
        contextClipColor = color || ""
        contextClipName = name || ""
        contextClipId = clipId
        contextClipKind = kind
        contextClipTimebase = timebase
        contextClipLoop = looping === true
        contextMenuX = Math.max(Theme.spaceXs, Math.min(width - Theme.arrangementContextMenuReach, x))
        contextMenuY = Math.max(Theme.spaceXs, Math.min(height - (kind === "audio"
            ? clipContextMenu.height : midiClipContextMenu.height) - Theme.spaceXs, y))
        if (kind === "audio")
            clipContextMenu.forceActiveFocus()
        else
            midiClipContextMenu.forceActiveFocus()
    }
    function hasNote(step, key) {
        const clip = selectedClip()
        return clip !== null && clip.notes.some(note => note.step === step && note.key === key)
    }

    color: canvasColor
    clip: true
    Rectangle {
        width: parent.width
        height: arrangement.rulerHeight
        color: Theme.arrangementRulerGround
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.hairline
            color: arrangement.lineColor
        }
    }
    // Arrangement tools, small at the ruler's right end: clip level editing,
    // bar paging and zoom, and snap.
    component ToolButton: Rectangle {
        id: tool
        property string label: ""
        property bool active: false
        property string accessibleName: label
        property string shortcut: ""
        signal activated()
        width: Math.max(Theme.arrangementToolHeight, toolText.implicitWidth + Theme.spaceLg)
        height: Theme.arrangementToolHeight
        radius: Theme.arrangementToolRadius
        color: active ? Theme.arrangementToolActive : toolTip.hovered ? Theme.arrangementToolHover : Theme.panelRaised
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        Tip { id: toolTip; text: tool.accessibleName; shortcut: tool.shortcut }
        border.color: activeFocus ? arrangement.accentColor : active ? Theme.arrangementToolActiveEdge : Theme.arrangementToolEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName
        Keys.onReturnPressed: activated()
        Keys.onSpacePressed: activated()
        Text {
            id: toolText
            anchors.centerIn: parent
            text: tool.label
            color: tool.active ? arrangement.accentColor : Theme.textDim
            font.pixelSize: Theme.typeSmall
            font.weight: Font.DemiBold
        }
        MouseArea { anchors.fill: parent; onClicked: tool.activated() }
    }
    Row {
        id: arrangementTools
        objectName: "arrangementTools"
        z: 5
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceSm
        y: (arrangement.rulerHeight - Theme.arrangementToolHeight) / 2
        spacing: Theme.arrangementToolSpacing
        ToolButton {
            id: levelEditAction
            objectName: "arrangementLevelEditToggle"
            label: "Level"
            active: arrangement.levelEditMode
            accessibleName: "Edit clip level envelope"
            Accessible.description: arrangement.levelEditMode ? "On" : "Off"
            onActivated: arrangement.levelEditMode = !arrangement.levelEditMode
        }
        Repeater {
            model: ["◀", "−", "+", "▶"]
            delegate: ToolButton {
                required property var modelData
                required property int index
                objectName: "arrangementZoomControl"
                label: modelData
                accessibleName: ["Previous bars", "Zoom out", "Zoom in", "Next bars"][index]
                shortcut: ["", "−", "+", ""][index]
                onActivated: {
                    if (index === 0) arrangement.panRequested(-1)
                    else if (index === 1) arrangement.zoomOutRequested()
                    else if (index === 2) arrangement.zoomInRequested()
                    else arrangement.panRequested(1)
                }
            }
        }
        ToolButton {
            id: snapAction
            objectName: "audioSnapToggle"
            label: arrangement.snapToBeat ? "Snap" : "Free"
            active: arrangement.snapToBeat
            accessibleName: "Snap audio edit to beat"
            Accessible.description: arrangement.snapToBeat ? "On" : "Off"
            onActivated: arrangement.snapToggled()
        }
    }
    Rectangle {
        id: loopRangeBand
        objectName: "arrangementLoopRange"
        readonly property real firstBar: rulerSeek.dragging
            ? Math.floor(arrangement.viewStartBar + Math.min(rulerSeek.pressX, rulerSeek.currentX) * arrangement.visibleBars / Math.max(1, rulerSeek.width))
            : arrangement.bridge.loopTransport.startBar - 1
        readonly property real lastBar: rulerSeek.dragging
            ? Math.floor(arrangement.viewStartBar + Math.max(rulerSeek.pressX, rulerSeek.currentX) * arrangement.visibleBars / Math.max(1, rulerSeek.width)) + 1
            : arrangement.bridge.loopTransport.endBar - 1
        x: (Math.max(firstBar, arrangement.viewStartBar) - arrangement.viewStartBar)
            * arrangement.width / arrangement.visibleBars
        y: 0
        width: Math.max(0, Math.min(lastBar, arrangement.viewStartBar + arrangement.visibleBars)
            - Math.max(firstBar, arrangement.viewStartBar)) * arrangement.width / arrangement.visibleBars
        height: arrangement.rulerHeight
        color: arrangement.tint(arrangement.amberColor, Theme.loopRangeWashAlpha)
        opacity: rulerSeek.dragging ? 1 : Theme.loopRangeRestOpacity
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.strokeBold
            color: arrangement.amberColor
        }
        visible: (rulerSeek.dragging || arrangement.bridge.loopTransport.enabled)
            && lastBar > arrangement.viewStartBar
            && firstBar < arrangement.viewStartBar + arrangement.visibleBars
        z: 1
    }
    MouseArea {
        id: rulerSeek
        // A press that moves this far draws a loop range rather than seeking.
        readonly property real dragThreshold: 8
        objectName: "arrangementRulerSeek"
        y: 0
        // Full width so bars map exactly; the tools sit above it at the end.
        width: parent.width
        height: arrangement.rulerHeight
        z: 3
        property real pressX: 0
        property real currentX: 0
        property bool dragging: false
        Accessible.role: Accessible.Slider
        Accessible.name: "Playback position and loop range"
        Accessible.description: "Click to seek; drag across bars to loop"
        onPressed: mouse => {
            pressX = Math.max(0, Math.min(width - 1, mouse.x))
            currentX = pressX
            dragging = false
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            currentX = Math.max(0, Math.min(width - 1, mouse.x))
            if (Math.abs(currentX - pressX) >= dragThreshold)
                dragging = true
        }
        onReleased: mouse => {
            currentX = Math.max(0, Math.min(width - 1, mouse.x))
            if (dragging) {
                const first = Math.floor(arrangement.viewStartBar + Math.min(pressX, currentX) * arrangement.visibleBars / width)
                const last = Math.floor(arrangement.viewStartBar + Math.max(pressX, currentX) * arrangement.visibleBars / width)
                arrangement.bridge.loopTransport.set_range(first + 1, last + 2)
                arrangement.bridge.loopTransport.set_enabled(true)
                dragging = false
            } else {
                arrangement.bridge.seek_to_bar(
                    arrangement.viewStartBar + currentX / width * arrangement.visibleBars)
            }
        }
        onCanceled: dragging = false
    }
    // Markers (Ctrl+M at the playhead): a flag on the ruler and a faint line
    // down the lanes. Click to jump there, double-click to rename,
    // right-click to remove.
    Repeater {
        model: arrangement.bridge.markers || []
        delegate: Item {
            id: marker
            required property var modelData
            readonly property real at: (Number(modelData.bar) - arrangement.viewStartBar)
                * arrangement.width / arrangement.visibleBars
            objectName: "arrangementMarker"
            visible: at >= -Theme.markerCullMargin && at <= arrangement.width
            x: at
            y: 0
            z: 4
            width: arrangement.markerFlagWidth(modelData.name)
            height: arrangement.rulerHeight
            Rectangle {
                width: Theme.hairline
                height: arrangement.height
                color: arrangement.mainTextColor
                opacity: Theme.markerLineOpacity
            }
            Rectangle {
                id: flag
                width: parent.width
                height: Theme.markerFlagHeight
                radius: Theme.radiusXs
                color: Theme.markerFlagFill
                border.color: Theme.markerFlagEdge
                Text {
                    id: flagText
                    visible: !nameEditor.visible
                    x: Theme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    text: marker.modelData.name || "Marker"
                    color: Theme.markerText
                    font.pixelSize: Theme.typeCaption
                    font.bold: true
                }
                TextInput {
                    id: nameEditor
                    visible: false
                    x: Theme.spaceSm
                    width: Math.max(Theme.markerEditorMinWidth, contentWidth + Theme.spaceXs)
                    anchors.verticalCenter: parent.verticalCenter
                    color: Theme.markerEditText
                    font.pixelSize: Theme.typeCaption
                    font.bold: true
                    selectByMouse: true
                    function finish(commit) {
                        if (!visible) return
                        visible = false
                        if (commit && text.trim().length > 0 && text.trim() !== marker.modelData.name)
                            arrangement.bridge.rename_marker(marker.modelData.id, text.trim())
                    }
                    Keys.onReturnPressed: finish(true)
                    Keys.onEnterPressed: finish(true)
                    Keys.onEscapePressed: finish(false)
                    onActiveFocusChanged: if (!activeFocus) finish(true)
                }
            }
            MouseArea {
                Tip {}
                width: flag.width
                height: flag.height
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: "Marker " + (marker.modelData.name || "") + ". Click to jump, double-click to rename, right-click to remove."
                onClicked: mouse => {
                    if (mouse.button === Qt.RightButton)
                        arrangement.bridge.remove_marker(marker.modelData.id)
                    else
                        arrangement.bridge.seek_to_bar(Number(marker.modelData.bar))
                }
                onDoubleClicked: mouse => {
                    if (mouse.button !== Qt.LeftButton) return
                    nameEditor.text = marker.modelData.name || ""
                    nameEditor.visible = true
                    nameEditor.forceActiveFocus()
                    nameEditor.selectAll()
                }
            }
        }
    }
    // Ruler ticks across the view: bar.beat once a beat is wide enough to
    // label, else every bar, or every 2, 4, 8… bars from bar 1 so labels
    // stay at least 48 px apart at any zoom.
    readonly property real pixelsPerBar: width / Math.max(0.0001, visibleBars)
    readonly property real rulerStep: {
        if (pixelsPerBar / beatsPerBar >= Theme.rulerBeatLabelMinSpacing)
            return 1 / beatsPerBar
        let step = 1
        while (step * pixelsPerBar < Theme.rulerLabelMinSpacing && step < maxRulerStepBars)
            step *= 2
        return step
    }
    // A marker flag's width: its name plus padding, never under the minimum.
    FontMetrics {
        id: markerFlagMetrics
        font.pixelSize: Theme.typeCaption
        font.bold: true
    }
    function markerFlagWidth(name) {
        return Math.max(Theme.markerMinWidth, markerFlagMetrics.advanceWidth(name || "Marker") + Theme.spaceLg)
    }
    // A ruler label shows only where it is whole: clear of the tools at the
    // ruler's right end and of every marker flag.
    function rulerLabelFree(left, right) {
        if (right + Theme.arrangementToolSpacing > arrangementTools.x)
            return false
        for (const marker of arrangement.bridge.markers || []) {
            const flagLeft = (Number(marker.bar) - viewStartBar) * width / visibleBars
            if (flagLeft < right && flagLeft + markerFlagWidth(marker.name) > left)
                return false
        }
        return true
    }
    readonly property var rulerTicks: {
        const step = rulerStep
        const lead = arrangement.bridge.leadBars
        const ticks = []
        // Steps count from bar 1, after any intro's lead bars.
        const first = lead + Math.floor((viewStartBar - lead) / step) * step
        for (let index = 0; ticks.length < maxRulerTicks; ++index) {
            const at = first + index * step
            if (at >= viewStartBar + visibleBars)
                break
            const bar = Math.floor(at + barEpsilon)
            const beat = Math.round((at - bar) * beatsPerBar) + 1
            // Bars before bar 1 (an intro) count up to 0.
            const shown = bar + 1 - lead
            ticks.push({ at: at, step: Math.round((at - lead) / step), bar: bar + 1, shown: shown,
                         onBar: beat === 1, label: beat === 1 ? String(shown) : shown + "." + beat })
        }
        return ticks
    }
    Repeater {
        model: arrangement.rulerTicks
        delegate: Item {
            required property var modelData
            required property int index
            x: (modelData.at - arrangement.viewStartBar) * arrangement.pixelsPerBar
            y: 0
            width: Theme.rulerTickLabelWidth
            height: arrangement.height
            Rectangle {
                x: 0
                y: 0
                width: Theme.hairline
                height: parent.height
                color: parent.modelData.onBar ? Theme.line : Theme.rulerBeatLine
            }
            Text {
                readonly property int bar: parent.modelData.bar
                readonly property bool looped: arrangement.bridge.loopTransport.enabled
                    && bar >= arrangement.bridge.loopTransport.startBar
                    && bar < arrangement.bridge.loopTransport.endBar
                objectName: "arrangementRulerLabel"
                x: Theme.spaceSm
                y: Theme.spaceMd
                visible: arrangement.rulerLabelFree(parent.x + x, parent.x + x + implicitWidth)
                text: parent.modelData.label
                color: looped ? arrangement.amberColor
                    : !parent.modelData.onBar ? arrangement.theme.textFaint
                    : parent.modelData.shown <= 0 ? arrangement.amberColor
                    : (parent.modelData.shown - 1) % barsPerPhrase === 0 ? arrangement.mainTextColor : arrangement.theme.textFaint
                font.pixelSize: parent.modelData.onBar ? Theme.typeBody : Theme.typeSmall
                font.family: arrangement.theme.monoFamily
            }
        }
    }
    // Ctrl+wheel zooms around the pointer; Shift+wheel or a trackpad's
    // sideways swipe scrolls through time. A wheel-only area above
    // everything: it takes no clicks, and passes a plain wheel on.
    MouseArea {
        objectName: "arrangementWheel"
        anchors.fill: parent
        z: 50
        // One wheel notch (an angle delta of 120) zooms by wheelZoomStep,
        // scrolls time by wheelScrollPixels or the tracks by
        // wheelTrackScrollPixels.
        readonly property real wheelNotch: 120
        readonly property real wheelZoomStep: 0.75
        readonly property real wheelScrollPixels: 40
        readonly property real wheelTrackScrollPixels: 60
        acceptedButtons: Qt.NoButton
        onWheel: wheel => {
            const bar = arrangement.viewStartBar
                + wheel.x / Math.max(1, arrangement.width) * arrangement.visibleBars
            if ((wheel.modifiers & Qt.ControlModifier) !== 0) {
                const angle = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
                const pixels = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.pixelDelta.x
                // Angle deltas are reliable for a mouse wheel; pixel deltas also
                // cover smooth scroll devices that report no wheel angle.
                const steps = angle !== 0 ? angle / wheelNotch : pixels / wheelNotch
                if (steps !== 0)
                    arrangement.zoomAtRequested(Math.pow(wheelZoomStep, steps), bar)
                wheel.accepted = true
                return
            }
            const sideways = wheel.pixelDelta.x !== 0 ? -wheel.pixelDelta.x
                : wheel.angleDelta.x !== 0 ? -wheel.angleDelta.x / wheelNotch * wheelScrollPixels
                : (wheel.modifiers & Qt.ShiftModifier) !== 0 ? -wheel.angleDelta.y / wheelNotch * wheelScrollPixels : 0
            if (sideways === 0) {
                // A plain wheel scrolls the tracks, as the track list does.
                const down = wheel.pixelDelta.y !== 0 ? -wheel.pixelDelta.y : -wheel.angleDelta.y / wheelNotch * wheelTrackScrollPixels
                if (down === 0) {
                    wheel.accepted = false
                    return
                }
                arrangement.verticalScrollRequested(down)
                wheel.accepted = true
                return
            }
            arrangement.scrollRequested(sideways / Math.max(1, arrangement.width) * arrangement.visibleBars)
            wheel.accepted = true
        }
    }
    Item {
        id: arrangementTrackViewport
        objectName: "arrangementTrackViewport"
        y: arrangement.rulerHeight
        width: parent.width
        height: Math.max(0, parent.height - y - arrangement.masterRowHeight - arrangement.creationHeight
            - arrangement.elapsedRulerHeight
            - arrangement.pianoRollHeight)
        clip: true
        // The loop range tints the lanes as well as the ruler.
        Rectangle {
            objectName: "arrangementLoopLanes"
            z: 1
            visible: loopRangeBand.visible
            x: loopRangeBand.x
            width: loopRangeBand.width
            height: parent.height
            color: arrangement.tint(arrangement.amberColor, Theme.loopLaneWashAlpha)
        }
        // The selection box being drawn on empty lane space.
        Rectangle {
            objectName: "arrangementSelectionBox"
            z: 20
            visible: arrangement.bandActive
            x: Math.min(arrangement.bandX0, arrangement.bandX1)
            y: Math.min(arrangement.bandY0, arrangement.bandY1) - arrangement.trackScrollY
            width: Math.abs(arrangement.bandX1 - arrangement.bandX0)
            height: Math.abs(arrangement.bandY1 - arrangement.bandY0)
            color: arrangement.tint(arrangement.accentColor, Theme.selectionBoxWashAlpha)
            border.color: arrangement.accentColor
            border.width: Theme.hairline
        }
        // The track model signals only the rows an edit changed: other
        // rows keep their bindings, and no row is rebuilt under a drag.
        Repeater {
            model: arrangement.bridge.trackModel
            delegate: Item {
                id: arrangementRow
                required property int index
                required property var modelData
                // This track's clips, a model of their own.
                required property var clips
                readonly property color trackColor: arrangement.theme.trackColor(modelData, index)
                // A clip being dragged out of this row draws above the others.
                property bool lifted: false
                readonly property bool carriesGroup: arrangement.groupDragging && arrangement.groupRows !== 0
                    && arrangement.groupTrackRows.indexOf(index) >= 0
                z: lifted ? 10 : carriesGroup ? 9 : 0
                x: 0
                y: (arrangement.rowLayout[index] ? arrangement.rowLayout[index].top : index * Theme.laneFallbackHeight)
                    - arrangement.trackScrollY
                width: arrangement.width
                height: arrangement.rowLayout[index] ? arrangement.rowLayout[index].height : Theme.laneFallbackHeight
                objectName: "arrangementRow"
                Canvas {
                    id: rowGrid
                    objectName: "arrangementRowGrid"
                    anchors.fill: parent
                    visible: parent.y < arrangement.height && parent.y + parent.height > 0
                    readonly property real barCount: arrangement.visibleBars
                    readonly property real firstBar: arrangement.viewStartBar
                    readonly property bool selected: arrangement.selectedTrack === index
                    onBarCountChanged: requestPaint()
                    onFirstBarChanged: requestPaint()
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onSelectedChanged: requestPaint()
                    readonly property int division: arrangement.gridDivision
                    onDivisionChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        context.fillStyle = selected ? Theme.laneGroundSelected : Theme.laneGround
                        context.fillRect(0, 0, width, height)
                        if (barCount <= 0 || width <= 0)
                            return
                        // The grid in view, as fine as the zoom allows (the
                        // same divisions a drag snaps to).
                        const perBar = width / barCount
                        const division = Math.max(beatsPerBar, arrangement.gridDivision)
                        const from = Math.floor(firstBar * division)
                        const to = Math.ceil((firstBar + barCount) * division)
                        for (let step = from; step <= to; ++step) {
                            const x = Math.floor((step / division - firstBar) * perBar)
                            const onBeat = step % Math.max(1, division / beatsPerBar) === 0
                            const bar = Math.floor(step / division)
                            context.fillStyle = step % division === 0 ? (bar % barsPerPhrase === 0 ? Theme.laneGridPhraseLine : Theme.line)
                                : onBeat ? Theme.laneGridBeatLine : Theme.laneGridStepLine
                            context.fillRect(x, 0, Theme.hairline, height)
                        }
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.hairline
                    color: arrangement.lineColor
                }
                MouseArea {
                    anchors.fill: parent
                    // A click on empty lane space picks the track and lets go
                    // of the selected clips; a drag draws a box that selects
                    // the clips it touches; a double-click on an instrument
                    // lane makes a clip there.
                    property bool banded: false
                    // A drag this far (x plus y) starts a selection box.
                    readonly property real bandThreshold: 6
                    onPressed: mouse => {
                        // The keyboard leaves the piano roll for the arrangement.
                        arrangement.forceActiveFocus()
                        const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y)
                        arrangement.bandX0 = at.x
                        arrangement.bandY0 = at.y + arrangement.trackScrollY
                        banded = false
                    }
                    onPositionChanged: mouse => {
                        if (!pressed)
                            return
                        const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y)
                        arrangement.bandX1 = at.x
                        arrangement.bandY1 = at.y + arrangement.trackScrollY
                        if (!arrangement.bandActive
                                && Math.abs(arrangement.bandX1 - arrangement.bandX0)
                                   + Math.abs(arrangement.bandY1 - arrangement.bandY0) < bandThreshold)
                            return
                        arrangement.bandActive = true
                        arrangement.selectInBand(mouse.modifiers)
                    }
                    onReleased: {
                        banded = arrangement.bandActive
                        arrangement.bandActive = false
                    }
                    onCanceled: arrangement.bandActive = false
                    onClicked: {
                        if (banded)
                            return;
                        arrangement.trackSelected(index);
                        arrangement.clipSelected("");
                        arrangement.clipsSelected([]);
                    }
                    onDoubleClicked: mouse => {
                        if (modelData.kind !== "instrument")
                            return;
                        arrangement.trackSelected(index);
                        arrangement.createMidiClip(modelData.id,
                            Math.floor(arrangement.viewStartBar + mouse.x * arrangement.visibleBars / width));
                    }
                }
                // An empty audio or instrument lane invites its first clip.
                Canvas {
                    id: emptyLane
                    objectName: "emptyMidiLaneHint"
                    visible: (modelData.kind === "instrument" || modelData.kind === "audio")
                        && arrangementRow.clips.count === 0
                    x: 0
                    y: Theme.spaceSm
                    width: parent.width - Theme.spaceSm
                    height: parent.height - Theme.spaceLg
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        context.strokeStyle = Theme.emptyLaneEdge
                        context.lineWidth = Theme.hairline
                        context.setLineDash(Theme.emptyLaneDash)
                        context.beginPath()
                        context.roundedRect(Theme.hairline / 2, Theme.hairline / 2, width - Theme.hairline, height - Theme.hairline, Theme.radiusLg, Theme.radiusLg)
                        context.stroke()
                    }
                    Text {
                        x: Theme.spaceLg
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - Theme.space2xl
                        elide: Text.ElideRight
                        text: arrangementRow.modelData.kind === "instrument"
                            ? "Draw a clip (double-click), drop MIDI or audio here"
                            : "Drop audio here"
                        color: Theme.textFaint
                        font.pixelSize: Theme.typeBody
                    }
                }
                // Notes being recorded on this track (ADR-0086), growing from
                // their start as the keys are held, over the clips.
                Repeater {
                    model: arrangement.bridge.recording
                        ? arrangement.bridge.recording.heldNotes.filter(
                              note => note.trackId === arrangementRow.modelData.id)
                        : []
                    delegate: Rectangle {
                        required property var modelData
                        readonly property real keyFraction: Math.max(0, Math.min(1,
                            (Number(modelData.key) - arrangement.recordingLowestKey)
                            / (arrangement.recordingHighestKey - arrangement.recordingLowestKey)))
                        z: 20
                        x: arrangement.width * (Number(modelData.startBar) - arrangement.viewStartBar)
                           / arrangement.visibleBars
                        y: Theme.spaceMd + (1 - keyFraction) * (arrangementRow.height - Theme.recordingNoteSpan)
                        width: Math.max(Theme.recordingNoteMinWidth, arrangement.width * (Number(modelData.endBar) - Number(modelData.startBar))
                                           / arrangement.visibleBars)
                        height: Theme.recordingNoteHeight
                        radius: Theme.radiusXs
                        color: arrangement.recordingNoteColor
                    }
                }
                Repeater {
                    model: arrangementRow.clips
                    delegate: Rectangle {
                        id: clipCard
                        objectName: "arrangementClip"
                        required property int index
                        required property var modelData
                        // The columns of this clip's part in view.
                        required property var waveform
                        // Its tempo strip's state (ClipTempoController), or none.
                        required property var tempo
                        // Audio trim edges: up to 10 px, a quarter of a narrow clip.
                        readonly property real trimWidth: Math.max(Theme.clipTrimMinWidth, Math.min(Theme.clipTrimMaxWidth, width * Theme.clipTrimFraction))
                        // The clip's own colour, else its track's.
                        readonly property color clipColor: modelData.color ? modelData.color
                                                                           : arrangementRow.trackColor
                        property real previewStart: modelData.start
                        property real previewEnd: modelData.start + modelData.length
                        property bool gestureActive: false
                        // Dragged onto another audio track: how far down (or up)
                        // its row is, and which track.
                        property real rowShift: 0
                        property string targetTrackId: ""
                        onGestureActiveChanged: arrangementRow.lifted = gestureActive
                        // After a gesture the preview follows the clip again,
                        // including the edit the gesture just made.
                        function resetPreview() {
                            previewStart = Qt.binding(() => clipCard.modelData.start)
                            previewEnd = Qt.binding(() => clipCard.modelData.start + clipCard.modelData.length)
                        }
                        // The clip's lowest and highest keys, so its notes fill the body.
                        readonly property var keySpan: {
                            const keys = modelData.kind === "midi" ? modelData.notes.map(note => note.key) : []
                            return keys.length > 0 ? [Math.min(...keys), Math.max(...keys)] : arrangement.defaultKeySpan
                        }
                        // The notes that play, as fractions of the clip: the
                        // pattern's notes, repeated when it loops; notes kept
                        // after the pattern's end are silent and not drawn.
                        readonly property var playedNotes: {
                            if (modelData.kind !== "midi") return []
                            const span = Number(modelData.lengthTicks)
                            const pattern = Number(modelData.patternTicks)
                            if (!(span > 0) || !(pattern > 0)) return []
                            const passes = modelData.loop ? Math.min(arrangement.maxDrawnPasses, Math.ceil(span / pattern)) : 1
                            const played = []
                            for (let pass = 0; pass < passes; ++pass) {
                                for (const note of modelData.notes) {
                                    const start = Number(note.startTicks)
                                    const tick = pass * pattern + start
                                    if (start >= pattern || tick >= span) continue
                                    const end = Math.min(pass * pattern + pattern,
                                        tick + Number(note.durationTicks), span)
                                    played.push({at: tick / span, size: (end - tick) / span,
                                                 key: Number(note.key)})
                                }
                            }
                            return played
                        }
                        activeFocusOnTab: modelData.kind === "midi" || (modelData.kind === "audio" && arrangement.bridge.audioEditEnabled)
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.kind === "audio" ? "Audio clip. Drag to move, Ctrl-drag to copy, Shift-click to add to the selection, Shift+Alt-drag to slip source audio, drag edges to trim. Arrow keys move one beat." + (modelData.waveformCacheDegraded ? " Waveform cache unavailable; preview remains usable." : "") : modelData.loop ? "Looping MIDI clip. Drag to move on the grid (Alt for free), Ctrl-drag to copy, or drag the right edge to repeat its pattern; turn Loop off to set its length instead." : "MIDI clip. Drag to move on the grid (Alt for free), Ctrl-drag to copy, or drag the right edge to lengthen or shorten it; notes after the end are kept. Turn Loop on to repeat it."
                        // With other clips selected (a group, or a clip this
                        // one is not), the arrow keys move the selection.
                        function nudge(direction) {
                            const picked = arrangement.bridge.selectedClipIds
                            if (picked.length > 1 || (picked.length === 1 && !arrangement.clipIsSelected(modelData.id)))
                                arrangement.nudgeSelection(direction);
                            else if (modelData.kind === "audio")
                                arrangement.bridge.nudge_audio_clip(modelData.id, direction);
                            else
                                arrangement.bridge.nudge_midi_clip(modelData.id, direction);
                        }
                        Keys.onLeftPressed: nudge(-1)
                        Keys.onRightPressed: nudge(1)
                        // A selected clip following the group being dragged.
                        readonly property bool following: arrangement.groupDragging && !gestureActive
                            && arrangement.clipIsSelected(modelData.id)
                        // Carried by a Ctrl-drag: the card draws the copy at the
                        // drop and an outline keeps the original in place.
                        readonly property bool copying: arrangement.copyDragging && (gestureActive || following)
                        x: arrangement.width * ((previewStart + (following ? arrangement.groupDelta : 0))
                            * arrangement.bridge.timelineBars - arrangement.viewStartBar) / arrangement.visibleBars + Theme.spaceXxs
                        y: Theme.spaceSm + rowShift + (following ? arrangement.rowShiftFor(arrangementRow.index, arrangement.groupRows) : 0)
                        width: Math.max(Theme.clipMinWidth, arrangement.projectDeltaToScreen(previewEnd - previewStart) - Theme.spaceXs)
                        height: Math.max(Theme.clipMinHeight, arrangementRow.height - Theme.spaceLg)
                        visible: previewEnd * arrangement.bridge.timelineBars > arrangement.viewStartBar && previewStart * arrangement.bridge.timelineBars < arrangement.viewStartBar + arrangement.visibleBars
                        // The card is the clip's full extent, for laying out its
                        // parts; only the slice in view is drawn (zoomed in, a
                        // clip is millions of pixels wide).
                        color: "transparent"
                        // A clip that arrives (dropped, pasted, undone) settles
                        // into place: a short fall and fade, drawing only.
                        transform: Translate { id: landing }
                        NumberAnimation {
                            id: landingDrop
                            target: landing
                            property: "y"
                            from: -Theme.clipLandingDrop
                            to: 0
                            duration: Motion.normal
                            easing.type: Easing.OutBack
                        }
                        NumberAnimation on opacity {
                            id: landingFade
                            running: false
                            from: 0
                            to: 1
                            duration: Motion.fast
                            easing.type: Motion.easing
                        }
                        Component.onCompleted: {
                            if (Motion.enabled && arrangement.clipsSettled && arrangement.visible) {
                                landingDrop.start()
                                landingFade.start()
                            }
                        }
                        readonly property real shownFrom: Math.max(0, -x - Theme.clipCullMargin)
                        readonly property real shownTo: Math.max(shownFrom, Math.min(width, arrangement.width - x + Theme.clipCullMargin))
                        // While the playhead is inside it, a playing clip glows
                        // with its track's level. Drawing only, off with
                        // reduced motion.
                        Rectangle {
                            objectName: "clipPlayingGlow"
                            // The level (dB) below which the glow rests.
                            readonly property real glowFloorDb: 48
                            // Clips out of view never read the playhead.
                            readonly property bool playing: Motion.enabled && clipCard.visible
                                && arrangement.bridge.playbackActive && !arrangement.bridge.playbackPaused
                                && !arrangementRow.modelData.muted
                                && arrangement.bridge.playheadBar >= clipCard.previewStart * arrangement.bridge.timelineBars
                                && arrangement.bridge.playheadBar < clipCard.previewEnd * arrangement.bridge.timelineBars
                            readonly property real level: {
                                if (!playing)
                                    return 0
                                const reading = arrangement.bridge.meters.channel(arrangementRow.modelData.id)
                                if (!reading || !reading.valid)
                                    return 0
                                const rms = Math.max(reading.leftRms, reading.rightRms)
                                return rms <= 0 ? 0 : Math.max(0, Math.min(1, (20 * Math.log10(rms) + glowFloorDb) / glowFloorDb))
                            }
                            visible: playing
                            z: -2
                            x: clipCard.shownFrom - Theme.clipGlowOutset
                            y: -Theme.clipGlowOutset
                            width: clipCard.shownTo - clipCard.shownFrom + 2 * Theme.clipGlowOutset
                            height: parent.height + 2 * Theme.clipGlowOutset
                            radius: Theme.radiusXl
                            color: "transparent"
                            border.color: clipCard.clipColor
                            border.width: Theme.strokeBold
                            opacity: Theme.clipGlowRestOpacity + Theme.clipGlowLevelOpacity * level
                        }
                        Rectangle {
                            objectName: "clipBody"
                            z: -1
                            x: clipCard.shownFrom
                            width: clipCard.shownTo - clipCard.shownFrom
                            height: parent.height
                            radius: Theme.radiusLg
                            color: arrangement.tint(clipCard.clipColor, Theme.clipBodyWashAlpha)
                            readonly property bool picked: arrangement.selectedClipId === clipCard.modelData.id
                                || arrangement.clipIsSelected(clipCard.modelData.id)
                            border.color: clipCard.gestureActive || clipCard.following ? arrangement.amberColor
                                : picked ? arrangement.mainTextColor
                                : arrangement.tint(clipCard.clipColor, Theme.clipEdgeAlpha)
                            border.width: picked ? Theme.strokeBold : Theme.hairline
                        }
                        // The original, left in place under a Ctrl-drag: an
                        // outline at the clip's saved position and row.
                        Rectangle {
                            objectName: "clipCopyOrigin"
                            visible: clipCard.copying
                            z: -3
                            x: arrangement.projectDeltaToScreen(clipCard.modelData.start - clipCard.previewStart
                                - (clipCard.following ? arrangement.groupDelta : 0))
                            y: Theme.spaceSm - clipCard.y
                            width: clipCard.width
                            height: clipCard.height
                            radius: Theme.radiusLg
                            color: arrangement.tint(clipCard.clipColor, Theme.clipBodyWashAlpha)
                            border.color: clipCard.clipColor
                            border.width: Theme.hairline
                        }
                        // The copy being dropped carries a plus.
                        Rectangle {
                            objectName: "clipCopyBadge"
                            visible: clipCard.copying
                            z: 3
                            x: Math.max(clipCard.shownFrom, 0) + Theme.spaceXs
                            y: Theme.spaceXs
                            width: Theme.clipCopyBadgeSize
                            height: Theme.clipCopyBadgeSize
                            radius: width / 2
                            color: arrangement.accentColor
                            Text {
                                anchors.centerIn: parent
                                text: "+"
                                color: arrangement.mainTextColor
                                font.pixelSize: Theme.typeLabel
                                font.bold: true
                            }
                        }
                        Rectangle {
                            objectName: "clipColorCap"
                            x: clipCard.shownFrom
                            y: 0
                            width: clipCard.shownTo - clipCard.shownFrom
                            height: Theme.clipCapHeight
                            radius: Theme.radiusLg
                            color: clipCard.clipColor
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: Theme.radiusLg
                                color: parent.color
                            }
                        }
                        Text {
                            visible: modelData.kind !== "audio"
                            x: Theme.clipNameX
                            y: Theme.clipNameY
                            width: parent.width - Theme.spaceLg
                            elide: Text.ElideRight
                            // A clock marks a clip locked to seconds.
                            text: (modelData.timebase === "Musical" ? "" : "◷ ")
                                  + arrangementRow.modelData.name
                            color: Theme.canvas
                            font.pixelSize: Theme.typeSmall
                            font.weight: Font.DemiBold
                        }
                        // The clip strip: name, track tempo, Follow, mode.
                        ClipStrip {
                            visible: modelData.kind === "audio"
                            z: 6
                            // Stays in view while the clip's start is off screen.
                            x: Math.max(Theme.spaceSm, clipCard.shownFrom + Theme.spaceSm)
                            y: 1
                            width: Math.max(0, clipCard.shownTo - x - Theme.spaceSm)
                            tempo: clipCard.tempo
                            name: modelData.name || arrangementRow.modelData.name || "Audio"
                            amberColor: arrangement.amberColor
                            accentColor: arrangement.accentColor
                            monoFamily: arrangement.theme.monoFamily
                            editable: arrangement.bridge.audioEditEnabled
                            onTempoRequested: function(chip) {
                                arrangement.clipSelected(modelData.id)
                                const at = chip.mapToItem(arrangement, 0, chip.height + Theme.spaceXs)
                                clipTempoPopover.show(modelData.id, "tempo", at)
                            }
                            onModeRequested: function(chip) {
                                arrangement.clipSelected(modelData.id)
                                const at = chip.mapToItem(arrangement, 0, chip.height + Theme.spaceXs)
                                clipTempoPopover.show(modelData.id, "mode", at)
                            }
                            onFollowToggled: function(follow) {
                                arrangement.clipSelected(modelData.id)
                                arrangement.bridge.clipTempo.set_follow(modelData.id, follow)
                            }
                        }
                        Text {
                            visible: modelData.kind === "audio" && modelData.waveformState === "ready" && modelData.waveformCacheDegraded && parent.width > Theme.clipBadgeMinClipWidth
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spaceMd
                            y: Theme.spaceXs
                            text: "CACHE OFF"
                            color: arrangement.amberColor
                            font.pixelSize: Theme.typeMicro
                            font.bold: true
                        }
                        Canvas {
                            id: clipWaveform
                            objectName: "audioClipWaveform"
                            property var points: clipCard.waveform || []
                            readonly property string waveFill: String(clipCard.clipColor)
                            readonly property string waveEdge: String(Qt.lighter(clipCard.clipColor, Theme.clipWaveEdgeLighten))
                            onWaveFillChanged: requestPaint()
                            readonly property real sourceWindowStart: Math.max(0, Math.min(1,
                                (arrangement.viewStartBar - modelData.start * arrangement.bridge.timelineBars)
                                / Math.max(0.000001, modelData.length * arrangement.bridge.timelineBars)))
                            readonly property real sourceWindowEnd: Math.max(0, Math.min(1,
                                (arrangement.viewStartBar + arrangement.visibleBars
                                 - modelData.start * arrangement.bridge.timelineBars)
                                / Math.max(0.000001, modelData.length * arrangement.bridge.timelineBars)))
                            visible: modelData.kind === "audio" && modelData.waveformState === "ready"
                            x: Theme.spaceXs + Math.max(0, parent.width - Theme.spaceMd) * sourceWindowStart
                            y: Theme.clipWaveTop
                            width: Math.max(0, parent.width - Theme.spaceMd) * Math.max(0, sourceWindowEnd - sourceWindowStart)
                            height: parent.height - Theme.clipWaveBottomGap
                            onPointsChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            onPaint: {
                                const context = getContext("2d");
                                context.clearRect(0, 0, width, height);
                                if (points.length === 0 || width <= 0)
                                    return;
                                const columns = Math.max(1, Math.min(points.length, Math.ceil(width)));
                                const upper = [];
                                const lower = [];
                                for (let column = 0; column < columns; ++column) {
                                    const begin = Math.floor(column * points.length / columns);
                                    const end = Math.max(begin + 1, Math.floor((column + 1) * points.length / columns));
                                    let minimum = 1;
                                    let maximum = -1;
                                    for (let index = begin; index < end; ++index) {
                                        minimum = Math.min(minimum, points[index].min);
                                        maximum = Math.max(maximum, points[index].max);
                                    }
                                    const x = (column + 0.5) * width / columns;
                                    upper.push([x, height * (1 - maximum) / 2]);
                                    lower.push([x, height * (1 - minimum) / 2]);
                                }
                                context.beginPath();
                                context.moveTo(upper[0][0], upper[0][1]);
                                for (let column = 1; column < columns; ++column)
                                    context.lineTo(upper[column][0], upper[column][1]);
                                for (let column = columns - 1; column >= 0; --column)
                                    context.lineTo(lower[column][0], lower[column][1]);
                                context.closePath();
                                context.globalAlpha = Theme.clipWaveFillAlpha;
                                context.fillStyle = waveFill;
                                context.fill();
                                context.globalAlpha = 1;
                                context.strokeStyle = waveEdge;
                                context.lineWidth = Theme.clipWaveEdgeStroke;
                                context.beginPath();
                                context.moveTo(upper[0][0], upper[0][1]);
                                for (let column = 1; column < columns; ++column)
                                    context.lineTo(upper[column][0], upper[column][1]);
                                context.stroke();
                            }
                        }
                        // A stretched clip's source is analysed once, in the
                        // background: greyed, uncovering as the analysis reads it.
                        Rectangle {
                            id: renderVeil
                            objectName: "clipRenderVeil"
                            readonly property var tempo: clipCard.tempo
                            readonly property real progress: tempo ? tempo.renderProgress : 1
                            visible: modelData.kind === "audio" && !!tempo && tempo.rendering
                            z: 4
                            readonly property real from: Math.max(Theme.spaceXs + Math.max(0, parent.width - Theme.spaceMd) * progress, clipCard.shownFrom)
                            x: from
                            y: Theme.clipOverlayTop
                            width: Math.max(0, Math.min(parent.width - Theme.spaceXs, clipCard.shownTo) - from)
                            height: parent.height - Theme.clipOverlayBottomGap
                            color: Theme.clipVeil
                            Accessible.role: Accessible.ProgressBar
                            Accessible.name: "Analysing the audio for stretching, " + Math.round(progress * 100) + "%"
                        }
                        Text {
                            visible: renderVeil.visible && parent.width > Theme.clipVeilLabelMinClipWidth
                            z: 5
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spaceMd
                            y: Theme.clipVeilLabelTop
                            text: "Analysing " + Math.round(renderVeil.progress * 100) + "%"
                            color: arrangement.dimTextColor
                            font.pixelSize: Theme.typeCaption
                            font.family: arrangement.theme.monoFamily
                        }
                        // The selected clip's beat grid, from its track tempo
                        // and downbeat: bar lines brighter.
                        Canvas {
                            id: clipBeatGrid
                            objectName: "clipBeatGrid"
                            readonly property var tempo: clipCard.tempo
                            readonly property real downbeat: clipFlag.dragging ? clipFlag.preview
                                : tempo ? tempo.downbeat : -1
                            readonly property real beat: tempo ? tempo.beat : 0
                            visible: modelData.kind === "audio" && arrangement.selectedClipId === modelData.id
                                     && beat > 0 && downbeat >= 0
                            // Only the part of the clip in view: zoomed in, a whole
                            // clip is millions of pixels wide.
                            readonly property real clipWidth: Math.max(0, parent.width - Theme.spaceMd)
                            readonly property real viewFrom: clipWaveform.sourceWindowStart
                            readonly property real viewTo: clipWaveform.sourceWindowEnd
                            x: Theme.spaceXs + clipWidth * viewFrom
                            y: Theme.clipOverlayTop
                            width: clipWidth * Math.max(0, viewTo - viewFrom)
                            height: parent.height - Theme.clipOverlayBottomGap
                            onDownbeatChanged: requestPaint()
                            onBeatChanged: requestPaint()
                            onWidthChanged: requestPaint()
                            onViewFromChanged: requestPaint()
                            onVisibleChanged: requestPaint()
                            onPaint: {
                                const context = getContext("2d")
                                context.clearRect(0, 0, width, height)
                                if (!visible || beat <= 0 || width <= 0) return
                                const px = beat * clipWidth
                                if (px < Theme.clipBeatGridMinBeatSpacing) return
                                // Beats from the first one in view.
                                let index = Math.ceil((viewFrom - downbeat) / beat)
                                for (let at = downbeat + index * beat; at <= viewTo + arrangement.barEpsilon; at += beat, ++index) {
                                    const x = Math.round((at - viewFrom) * clipWidth) + Theme.hairline / 2
                                    if (index % arrangement.beatsPerBar !== 0 && px < Theme.clipBeatGridMinOffbeatSpacing) continue
                                    context.fillStyle = index % arrangement.beatsPerBar === 0 ? Theme.clipBeatGridBarLine : Theme.clipBeatGridBeatLine
                                    context.fillRect(x, 0, Theme.hairline, height)
                                }
                            }
                        }
                        // The downbeat flag: drag it onto the kick that starts a
                        // bar (it snaps to the grid's beats; Alt places it freely).
                        Item {
                            id: clipFlag
                            objectName: "clipDownbeatFlag"
                            property bool dragging: false
                            property real preview: 0
                            readonly property real at: dragging ? preview : clipBeatGrid.downbeat
                            visible: clipBeatGrid.visible
                            z: 5
                            x: Theme.spaceXs + clipBeatGrid.clipWidth * at - Theme.strokeBold / 2
                            y: Theme.clipOverlayTop
                            width: Theme.clipFlagWidth
                            height: parent.height - Theme.clipOverlayBottomGap
                            Rectangle {
                                width: Theme.strokeBold
                                height: parent.height
                                color: arrangement.accentColor
                            }
                            Rectangle {
                                width: flagText.implicitWidth + Theme.spaceMd
                                height: Theme.clipFlagBadgeHeight
                                radius: Theme.radiusSm
                                color: arrangement.accentColor
                                Text {
                                    id: flagText
                                    anchors.centerIn: parent
                                    text: "1"
                                    color: Theme.canvas
                                    font.pixelSize: Theme.typeSmall
                                    font.bold: true
                                }
                            }
                            MouseArea {
                                width: Theme.clipFlagGrabSize
                                height: Theme.clipFlagGrabSize
                                enabled: arrangement.bridge.audioEditEnabled
                                cursorShape: Qt.SizeHorCursor
                                preventStealing: true
                                Accessible.role: Accessible.Slider
                                Accessible.name: "Downbeat flag: drag onto the first beat of a bar"
                                property real pressX: 0
                                property real pressAt: 0
                                onPressed: mouse => {
                                    pressX = mapToItem(clipCard, mouse.x, mouse.y).x
                                    pressAt = clipBeatGrid.downbeat
                                    clipFlag.preview = pressAt
                                    clipFlag.dragging = true
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed) return
                                    const delta = (mapToItem(clipCard, mouse.x, mouse.y).x - pressX)
                                                  / Math.max(1, clipBeatGrid.width)
                                    let next = Math.max(0, Math.min(1, pressAt + delta))
                                    if ((mouse.modifiers & Qt.AltModifier) === 0 && clipBeatGrid.beat > 0)
                                        next = pressAt + Math.round((next - pressAt) / clipBeatGrid.beat) * clipBeatGrid.beat
                                    clipFlag.preview = Math.max(0, Math.min(1, next))
                                }
                                onReleased: mouse => {
                                    const target = clipFlag.preview
                                    clipFlag.dragging = false
                                    if (Math.abs(target - pressAt) * clipBeatGrid.width >= arrangement.clickSlop)
                                        arrangement.bridge.clipTempo.set_downbeat(modelData.id, target,
                                            (mouse.modifiers & Qt.AltModifier) !== 0)
                                }
                                onCanceled: clipFlag.dragging = false
                            }
                        }
                        Text {
                            visible: modelData.kind === "audio" && modelData.waveformState !== "ready"
                            anchors.centerIn: parent
                            width: parent.width - Theme.spaceLg
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            text: modelData.waveformMessage || "Waveform unavailable"
                            color: modelData.waveformState === "loading" ? Theme.clipWaveLoadingText : Theme.clipWaveErrorText
                            font.pixelSize: Theme.typeSmall
                        }
                        // Chord clips (ADR-0084): the chords the notes make,
                        // named along the clip's foot, repeated as it loops.
                        readonly property var playedChords: {
                            if (modelData.kind !== "midi" || !modelData.chords) return []
                            const span = Number(modelData.lengthTicks)
                            const pattern = Number(modelData.patternTicks)
                            if (!(span > 0) || !(pattern > 0)) return []
                            const passes = modelData.loop ? Math.min(arrangement.maxDrawnPasses, Math.ceil(span / pattern)) : 1
                            const played = []
                            for (let pass = 0; pass < passes; ++pass) {
                                for (const chord of modelData.chords) {
                                    const tick = pass * pattern + Number(chord.startTicks)
                                    if (tick >= span) continue
                                    const end = Math.min(pass * pattern + Number(chord.endTicks), span)
                                    played.push({at: tick / span, size: (end - tick) / span, name: chord.name})
                                }
                            }
                            return played
                        }
                        // Shown chord names keep the clip's foot; the notes draw above it.
                        readonly property bool chordsShown: clipCard.height >= Theme.clipChordMinClipHeight
                            && playedChords.length > 0
                        readonly property real chordRow: chordsShown ? Theme.clipChordBottom : 0
                        Repeater {
                            model: clipCard.chordsShown ? clipCard.playedChords : []
                            delegate: Item {
                                id: chordTag
                                required property var modelData
                                objectName: "arrangementChordName"
                                x: Theme.spaceSm + (clipCard.width - Theme.spaceLg) * modelData.at
                                y: clipCard.height - Theme.clipChordBottom
                                width: (clipCard.width - Theme.spaceLg) * modelData.size
                                height: Theme.clipChordHeight
                                clip: true
                                visible: width >= Theme.clipChordMinWidth
                                Rectangle { width: Theme.hairline; height: parent.height; color: Theme.chordAccent; opacity: Theme.clipChordLineOpacity }
                                Text {
                                    x: Theme.clipChordTextInset
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: chordTag.modelData.name
                                    color: Theme.clipChordText
                                    style: Text.Outline
                                    styleColor: Theme.clipChordTextOutline
                                    font.family: arrangement.theme.monoFamily
                                    font.pixelSize: Theme.typeCaption
                                    font.weight: Font.Bold
                                }
                            }
                        }
                        Repeater {
                            model: clipCard.playedNotes
                            delegate: Rectangle {
                                required property var modelData
                                objectName: "arrangementClipNote"
                                x: Theme.spaceSm + (clipCard.width - Theme.spaceLg) * modelData.at
                                y: Theme.clipNoteTop + (clipCard.height - Theme.clipNoteSpan - clipCard.chordRow) * (clipCard.keySpan[1] === clipCard.keySpan[0] ? 0.5
                                    : (clipCard.keySpan[1] - modelData.key) / (clipCard.keySpan[1] - clipCard.keySpan[0]))
                                width: Math.max(Theme.clipNoteMinWidth, (clipCard.width - Theme.spaceLg) * modelData.size)
                                height: Theme.clipNoteHeight
                                radius: height / 2
                                color: Qt.lighter(clipCard.clipColor, Theme.clipNoteLighten)
                            }
                        }
                        MouseArea {
                            objectName: "midiClipMoveHandle"
                            anchors.fill: parent
                            enabled: modelData.kind === "midi"
                            acceptedButtons: Qt.LeftButton
                            cursorShape: arrangement.copyDragging && pressed ? Qt.DragCopyCursor : Qt.OpenHandCursor
                            property real initialX: 0
                            property real initialStart: 0
                            // Shift-click only picks; a press on a clip of the
                            // selected group drags the whole group. Ctrl-drag
                            // copies (the clip, or the group it belongs to);
                            // a Ctrl-click without a drag picks, as Shift does.
                            property bool toggling: false
                            property bool copyPress: false
                            property bool grouped: false
                            property bool moved: false
                            onPressed: mouse => {
                                toggling = (mouse.modifiers & Qt.ShiftModifier) !== 0
                                copyPress = !toggling && (mouse.modifiers & Qt.ControlModifier) !== 0
                                if (!copyPress)
                                    arrangement.pickClip(modelData.id, mouse.modifiers)
                                if (toggling)
                                    return
                                parent.forceActiveFocus()
                                initialX = mapToItem(arrangement, mouse.x, mouse.y).x
                                initialStart = previewStart
                                grouped = arrangement.groupSelected && arrangement.clipIsSelected(modelData.id)
                                moved = false
                                if (grouped)
                                    arrangement.beginGroupDrag()
                                arrangement.copyDragging = copyPress
                                parent.gestureActive = true
                            }
                            onPositionChanged: mouse => {
                                if (pressed && !toggling)
                                    arrangement.copyDragging = (mouse.modifiers & Qt.ControlModifier) !== 0
                                if (pressed && !toggling && grouped) {
                                    const deltaX = mapToItem(arrangement, mouse.x, mouse.y).x - initialX
                                    let nextStart = initialStart + arrangement.screenDeltaToProject(deltaX)
                                    if (arrangement.snapToBeat && (mouse.modifiers & Qt.AltModifier) === 0)
                                        nextStart = arrangement.snapToGrid(nextStart)
                                    parent.previewStart = Math.max(0, Math.min(1 - modelData.length, nextStart))
                                    parent.previewEnd = parent.previewStart + modelData.length
                                    const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y).y
                                               + arrangement.trackScrollY
                                    const row = arrangement.rowAt(at)
                                    arrangement.groupRows = row >= 0 ? row - arrangementRow.index : 0
                                    arrangement.groupDelta = parent.previewStart - initialStart
                                    parent.rowShift = arrangement.rowShiftFor(arrangementRow.index, arrangement.groupRows)
                                    moved = moved || Math.abs(deltaX) >= arrangement.clickSlop || arrangement.groupRows !== 0
                                } else if (pressed && !toggling) {
                                    const deltaX = mapToItem(arrangement, mouse.x, mouse.y).x - initialX
                                    let nextStart = initialStart + arrangement.screenDeltaToProject(deltaX)
                                    if (arrangement.snapToBeat && (mouse.modifiers & Qt.AltModifier) === 0)
                                        nextStart = arrangement.snapToGrid(nextStart)
                                    parent.previewStart = Math.max(0, Math.min(1 - modelData.length, nextStart))
                                    parent.previewEnd = parent.previewStart + modelData.length
                                    // Onto another instrument track's row?
                                    const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y).y
                                               + arrangement.trackScrollY
                                    const own = arrangement.rowLayout[arrangementRow.index]
                                    let shift = 0
                                    let target = ""
                                    for (let row = 0; row < arrangement.rowLayout.length; ++row) {
                                        const layout = arrangement.rowLayout[row]
                                        const track = arrangement.bridge.tracks[row]
                                        if (!track || at < layout.top || at >= layout.top + layout.height)
                                            continue
                                        if (track.kind === "instrument" && row !== arrangementRow.index) {
                                            shift = layout.top - own.top
                                            target = track.id
                                        }
                                    }
                                    parent.rowShift = shift
                                    parent.targetTrackId = target
                                    moved = moved || Math.abs(deltaX) >= arrangement.clickSlop || target.length > 0
                                }
                            }
                            onReleased: {
                                if (toggling) {
                                    toggling = false
                                    return
                                }
                                const target = parent.previewStart
                                const destination = parent.targetTrackId
                                const rows = grouped ? arrangement.groupRows
                                    : destination.length > 0 ? arrangement.rowOfTrack(destination) - arrangementRow.index : 0
                                const copying = arrangement.copyDragging
                                const group = grouped
                                arrangement.copyDragging = false
                                parent.gestureActive = false
                                clipCard.resetPreview()
                                parent.rowShift = 0
                                parent.targetTrackId = ""
                                if (grouped) {
                                    grouped = false
                                    arrangement.endGroupDrag()
                                }
                                if (copyPress && !moved)
                                    arrangement.pickClip(modelData.id, Qt.ControlModifier)
                                else if (copying && moved)
                                    arrangement.bridge.copy_clips_to(group ? arrangement.bridge.selectedClipIds : [modelData.id],
                                                                     initialStart, target, rows)
                                else if (group && moved)
                                    arrangement.bridge.move_selected_clips(initialStart, target, rows)
                                else if (group)
                                    arrangement.clipsSelected([modelData.id])
                                else if (destination.length > 0)
                                    arrangement.bridge.move_midi_clip_to_track(modelData.id, destination, target)
                                else if (Math.abs(arrangement.projectDeltaToScreen(target - initialStart)) >= arrangement.clickSlop)
                                    arrangement.bridge.move_midi_clip(modelData.id, target)
                                copyPress = false
                            }
                            onDoubleClicked: {
                                arrangement.trackSelected(arrangementRow.index)
                                arrangement.clipOpened(modelData.id)
                            }
                            onCanceled: {
                                parent.gestureActive = false
                                clipCard.resetPreview()
                                parent.rowShift = 0
                                parent.targetTrackId = ""
                                toggling = false
                                copyPress = false
                                arrangement.copyDragging = false
                                if (grouped)
                                    arrangement.endGroupDrag()
                                grouped = false
                            }
                        }
                        // The right edge lengthens or shortens the clip. Its grab
                        // zone reaches a little past the edge and stays usable on
                        // narrow clips (a third of the clip, at least 4 px), so a
                        // zoomed-out clip can still be stretched.
                        Item {
                            id: midiResizeZone
                            visible: modelData.kind === "midi"
                            z: 2
                            readonly property real grab: Math.max(Theme.clipGrabMinWidth, Math.min(Theme.clipGrabMaxWidth, clipCard.width * Theme.clipGrabFraction))
                            x: clipCard.width - grab
                            width: grab + Theme.clipGrabOverhang
                            height: parent.height
                            // No permanent bar: a thin grip just inside the clip's
                            // edge shows on hover and while dragging, faintly on the
                            // selected clip, so the edge reads as part of the clip.
                            Rectangle {
                                readonly property bool lit: resizeArea.containsMouse || resizeArea.pressed
                                x: midiResizeZone.grab - width - Theme.clipGripGap
                                anchors.verticalCenter: parent.verticalCenter
                                width: Theme.clipGripWidth
                                height: Math.max(Theme.clipGripMinHeight, parent.height - Theme.clipGripInset)
                                radius: Theme.clipGripWidth / 2
                                color: lit ? arrangement.accentColor : arrangement.mainTextColor
                                opacity: lit ? 1 : arrangement.selectedClipId === modelData.id ? Theme.clipGripSelectedOpacity : 0
                                Behavior on opacity { NumberAnimation { duration: Motion.hover } }
                            }
                            MouseArea {
                                id: resizeArea
                                objectName: "midiClipRightResizeHandle"
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.SizeHorCursor
                                property real initialX: 0
                                property real initialEnd: 0
                                onPressed: mouse => {
                                    arrangement.clipSelected(modelData.id)
                                    initialX = mapToItem(arrangement, mouse.x, mouse.y).x
                                    initialEnd = clipCard.previewEnd
                                    clipCard.gestureActive = true
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed)
                                        return
                                    const deltaX = mapToItem(arrangement, mouse.x, mouse.y).x - initialX
                                    let end = initialEnd + arrangement.screenDeltaToProject(deltaX)
                                    const snapping = arrangement.snapToBeat && (mouse.modifiers & Qt.AltModifier) === 0
                                    if (snapping)
                                        end = arrangement.snapToGrid(end)
                                    // At least one grid step (or a sixteenth unsnapped) long.
                                    const shortest = (snapping ? arrangement.gridStepBars() : 1 / arrangement.unsnappedStepsPerBar)
                                        / arrangement.bridge.timelineBars
                                    clipCard.previewEnd = Math.min(1, Math.max(clipCard.previewStart + shortest, end))
                                }
                                onReleased: {
                                    const target = clipCard.previewEnd
                                    clipCard.gestureActive = false
                                    clipCard.resetPreview()
                                    if (Math.abs(arrangement.projectDeltaToScreen(target - initialEnd)) >= arrangement.clickSlop)
                                        arrangement.bridge.resize_midi_clip(modelData.id, target)
                                }
                                onCanceled: {
                                    clipCard.gestureActive = false
                                    clipCard.resetPreview()
                                }
                            }
                        }
                        MouseArea {
                            id: audioMoveArea
                            objectName: "audioClipMoveHandle"
                            visible: modelData.kind === "audio"
                            enabled: visible && arrangement.bridge.audioEditEnabled
                            anchors.fill: parent
                            anchors.leftMargin: clipCard.width >= Theme.clipTrimMinClipWidth ? clipCard.trimWidth : 0
                            anchors.rightMargin: clipCard.width >= Theme.clipTrimMinClipWidth ? clipCard.trimWidth : 0
                            cursorShape: slipGesture ? Qt.SizeHorCursor
                                : arrangement.copyDragging && pressed ? Qt.DragCopyCursor : Qt.OpenHandCursor
                            property real initialX: 0
                            property real initialStart: 0
                            property bool slipGesture: false
                            property real slipDeltaFrames: 0
                            // As for MIDI clips: Shift-click picks, a press on the
                            // selected group drags it, Ctrl-drag copies (a
                            // Ctrl-click picks); Shift+Alt slips.
                            property bool toggling: false
                            property bool copyPress: false
                            property bool grouped: false
                            property bool moved: false
                            onPressed: mouse => {
                                const shift = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                                const alt = (mouse.modifiers & Qt.AltModifier) !== 0;
                                slipGesture = shift && alt;
                                toggling = !slipGesture && shift;
                                copyPress = !slipGesture && !shift && (mouse.modifiers & Qt.ControlModifier) !== 0;
                                if (!copyPress)
                                    arrangement.pickClip(modelData.id, slipGesture ? 0 : mouse.modifiers);
                                if (toggling)
                                    return;
                                if (slipGesture)
                                    arrangement.clipSelected(modelData.id);
                                parent.forceActiveFocus();
                                initialX = mapToItem(arrangement, mouse.x, mouse.y).x;
                                initialStart = previewStart;
                                grouped = !slipGesture && arrangement.groupSelected && arrangement.clipIsSelected(modelData.id);
                                moved = false;
                                if (grouped)
                                    arrangement.beginGroupDrag();
                                slipDeltaFrames = 0;
                                arrangement.copyDragging = copyPress;
                                parent.gestureActive = true;
                            }
                            onPositionChanged: mouse => {
                                if (pressed && !toggling && !slipGesture)
                                    arrangement.copyDragging = (mouse.modifiers & Qt.ControlModifier) !== 0;
                                if (pressed && !toggling) {
                                    const deltaX = mapToItem(arrangement, mouse.x, mouse.y).x - initialX;
                                    if (grouped) {
                                        let nextStart = Math.max(0, Math.min(1 - modelData.length,
                                            initialStart + arrangement.screenDeltaToProject(deltaX)));
                                        if (arrangement.snapToBeat && (mouse.modifiers & Qt.AltModifier) === 0)
                                            nextStart = Math.max(0, arrangement.snappedClipStart(modelData, nextStart));
                                        parent.previewStart = nextStart;
                                        parent.previewEnd = nextStart + modelData.length;
                                        const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y).y
                                                   + arrangement.trackScrollY;
                                        const row = arrangement.rowAt(at);
                                        arrangement.groupRows = row >= 0 ? row - arrangementRow.index : 0;
                                        arrangement.groupDelta = nextStart - initialStart;
                                        parent.rowShift = arrangement.rowShiftFor(arrangementRow.index, arrangement.groupRows);
                                        moved = moved || Math.abs(deltaX) >= arrangement.clickSlop || arrangement.groupRows !== 0;
                                    } else if (slipGesture) {
                                        const rawDelta = Math.round(deltaX * modelData.sourceFrameCount
                                                                    / Math.max(1, arrangement.projectDeltaToScreen(modelData.length)));
                                        slipDeltaFrames = Math.max(-modelData.sourceStartFrame,
                                            Math.min(modelData.sourceTotalFrames - modelData.sourceStartFrame
                                                     - modelData.sourceFrameCount, rawDelta));
                                    } else {
                                        // Left of zero is allowed: the drop adds bars before bar 1.
                                        let nextStart = Math.min(1 - modelData.length, initialStart + arrangement.screenDeltaToProject(deltaX));
                                        if (arrangement.snapToBeat && (mouse.modifiers & Qt.AltModifier) === 0)
                                            nextStart = arrangement.snappedClipStart(modelData, nextStart);
                                        parent.previewStart = nextStart;
                                        parent.previewEnd = nextStart + modelData.length;
                                        // Onto another audio track's row?
                                        const at = mapToItem(arrangementTrackViewport, mouse.x, mouse.y).y
                                                   + arrangement.trackScrollY;
                                        const own = arrangement.rowLayout[arrangementRow.index];
                                        let shift = 0;
                                        let target = "";
                                        for (let row = 0; row < arrangement.rowLayout.length; ++row) {
                                            const layout = arrangement.rowLayout[row];
                                            const track = arrangement.bridge.tracks[row];
                                            if (!track || at < layout.top || at >= layout.top + layout.height)
                                                continue;
                                            if (track.kind === "audio" && row !== arrangementRow.index) {
                                                shift = layout.top - own.top;
                                                target = track.id;
                                            }
                                        }
                                        parent.rowShift = shift;
                                        parent.targetTrackId = target;
                                        moved = moved || Math.abs(deltaX) >= arrangement.clickSlop || target.length > 0;
                                    }
                                }
                            }
                            onReleased: {
                                if (toggling) {
                                    toggling = false;
                                    return;
                                }
                                const target = parent.previewStart;
                                const slip = slipGesture;
                                const delta = slipDeltaFrames;
                                const destination = parent.targetTrackId;
                                const rows = grouped ? arrangement.groupRows
                                    : destination.length > 0 ? arrangement.rowOfTrack(destination) - arrangementRow.index : 0;
                                const copying = arrangement.copyDragging;
                                const picking = copyPress && !moved;
                                arrangement.copyDragging = false;
                                copyPress = false;
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                                parent.rowShift = 0;
                                parent.targetTrackId = "";
                                slipGesture = false;
                                slipDeltaFrames = 0;
                                const group = grouped;
                                if (grouped) {
                                    grouped = false;
                                    arrangement.endGroupDrag();
                                }
                                if (picking)
                                    arrangement.pickClip(modelData.id, Qt.ControlModifier);
                                else if (!slip && copying && moved)
                                    arrangement.bridge.copy_clips_to(group ? arrangement.bridge.selectedClipIds : [modelData.id],
                                                                     initialStart, target, rows);
                                else if (group) {
                                    if (moved)
                                        arrangement.bridge.move_selected_clips(initialStart, target, rows);
                                    else
                                        arrangement.clipsSelected([modelData.id]);
                                } else if (slip && Math.abs(delta) >= 1)
                                    arrangement.bridge.slip_audio_clip(modelData.id, delta);
                                else if (!slip && destination.length > 0)
                                    arrangement.bridge.move_audio_clip_to_track(modelData.id, destination, target);
                                else if (!slip && Math.abs(arrangement.projectDeltaToScreen(target - initialStart)) >= arrangement.audioClickSlop)
                                    // The preview already snapped (the downbeat to the grid).
                                    arrangement.bridge.move_audio_clip(modelData.id, target, false);
                            }
                            onCanceled: {
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                                parent.rowShift = 0;
                                parent.targetTrackId = "";
                                slipGesture = false;
                                slipDeltaFrames = 0;
                                toggling = false;
                                copyPress = false;
                                arrangement.copyDragging = false;
                                if (grouped)
                                    arrangement.endGroupDrag();
                                grouped = false;
                            }
                        }
                        Text {
                            visible: audioMoveArea.slipGesture
                            anchors.centerIn: parent
                            text: "SLIP"
                            color: arrangement.amberColor
                            font.pixelSize: Theme.typeLabel
                            font.bold: true
                        }
                        MouseArea {
                            objectName: "audioClipLeftTrimHandle"
                            visible: modelData.kind === "audio" && clipCard.width >= Theme.clipTrimMinClipWidth
                            enabled: visible && arrangement.bridge.audioEditEnabled
                            x: 0
                            y: 0
                            width: clipCard.trimWidth
                            hoverEnabled: true
                            height: parent.height
                            cursorShape: Qt.SizeHorCursor
                            Rectangle {
                                x: Theme.clipGripGap
                                anchors.verticalCenter: parent.verticalCenter
                                width: Theme.clipGripWidth
                                height: Math.max(Theme.clipGripMinHeight, parent.height - Theme.clipGripInset)
                                radius: Theme.clipGripWidth / 2
                                color: arrangement.accentColor
                                visible: parent.containsMouse || parent.pressed
                            }
                            property real initialX: 0
                            property real initialEdge: 0
                            onPressed: mouse => {
                                arrangement.clipSelected(modelData.id);
                                initialX = mapToItem(arrangement, mouse.x, mouse.y).x;
                                initialEdge = previewStart;
                                parent.gestureActive = true;
                            }
                            onPositionChanged: mouse => {
                                if (pressed)
                                    parent.previewStart = Math.max(0, Math.min(Math.max(0, parent.previewEnd - arrangement.minimumClipFraction), initialEdge + arrangement.screenDeltaToProject(mapToItem(arrangement, mouse.x, mouse.y).x - initialX)));
                            }
                            onReleased: {
                                const target = parent.previewStart;
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                                if (Math.abs(arrangement.projectDeltaToScreen(target - initialEdge)) >= arrangement.audioClickSlop)
                                    arrangement.bridge.trim_audio_clip(modelData.id, true, target, arrangement.snapToBeat);
                            }
                            onCanceled: {
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                            }
                        }
                        MouseArea {
                            objectName: "audioClipRightTrimHandle"
                            visible: modelData.kind === "audio" && clipCard.width >= Theme.clipTrimMinClipWidth
                            enabled: visible && arrangement.bridge.audioEditEnabled
                            anchors.right: parent.right
                            width: clipCard.trimWidth
                            hoverEnabled: true
                            height: parent.height
                            cursorShape: Qt.SizeHorCursor
                            Rectangle {
                                x: parent.width - Theme.clipGripWidth - Theme.clipGripGap
                                anchors.verticalCenter: parent.verticalCenter
                                width: Theme.clipGripWidth
                                height: Math.max(Theme.clipGripMinHeight, parent.height - Theme.clipGripInset)
                                radius: Theme.clipGripWidth / 2
                                color: arrangement.accentColor
                                visible: parent.containsMouse || parent.pressed
                            }
                            property real initialX: 0
                            property real initialEdge: 0
                            onPressed: mouse => {
                                arrangement.clipSelected(modelData.id);
                                initialX = mapToItem(arrangement, mouse.x, mouse.y).x;
                                initialEdge = previewEnd;
                                parent.gestureActive = true;
                            }
                            onPositionChanged: mouse => {
                                if (pressed)
                                    parent.previewEnd = Math.min(1, Math.max(Math.min(1, parent.previewStart + arrangement.minimumClipFraction), initialEdge + arrangement.screenDeltaToProject(mapToItem(arrangement, mouse.x, mouse.y).x - initialX)));
                            }
                            onReleased: {
                                const target = parent.previewEnd;
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                                if (Math.abs(arrangement.projectDeltaToScreen(target - initialEdge)) >= arrangement.audioClickSlop)
                                    arrangement.bridge.trim_audio_clip(modelData.id, false, target, arrangement.snapToBeat);
                            }
                            onCanceled: {
                                parent.gestureActive = false;
                                clipCard.resetPreview();
                            }
                        }
                        AudioClipFadeControls {
                            anchors.fill: parent
                            visible: modelData.kind === "audio"
                            clipData: modelData
                            bridge: arrangement.bridge
                            selected: arrangement.selectedClipId === modelData.id
                            accentColor: arrangement.accentColor
                            amberColor: arrangement.amberColor
                        }
                        // Only while level editing this clip: its canvas spans the
                        // whole clip.
                        Loader {
                            anchors.fill: parent
                            z: 10
                            active: modelData.kind === "audio" &&
                                    arrangement.selectedClipId === modelData.id &&
                                    arrangement.levelEditMode
                            sourceComponent: ClipLevelEnvelope {
                                bridge: arrangement.bridge
                                clipData: clipCard.modelData
                                accentColor: arrangement.accentColor
                                lineColor: arrangement.lineColor
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            z: 20
                            visible: modelData.kind === "audio"
                            acceptedButtons: Qt.RightButton
                            // The menu opens once the press is delivered: opening it
                            // within the press would leave this area pressed.
                            onPressed: mouse => {
                                const position = mapToItem(arrangement, mouse.x, mouse.y)
                                const clip = modelData
                                Qt.callLater(() => arrangement.openClipMenu(clip.id, "audio", clip.timebase,
                                                                            position.x, position.y, false,
                                                                            clip.color, clip.name))
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            z: 20
                            visible: modelData.kind === "midi"
                            acceptedButtons: Qt.RightButton
                            onPressed: mouse => {
                                const position = mapToItem(arrangement, mouse.x, mouse.y)
                                const clip = modelData
                                Qt.callLater(() => arrangement.openClipMenu(clip.id, "midi", clip.timebase,
                                                                            position.x, position.y,
                                                                            clip.loop === true, clip.color, ""))
                            }
                        }
                    }
                }
                // With automation shown, the chosen parameter's envelope lies
                // over the clips and takes the pointer: the clips show
                // through, dimmed, and stay put.
                Loader {
                    id: automationOverlay
                    readonly property var shown: arrangement.automationView[arrangementRow.modelData.id] || null
                    z: 4
                    anchors.fill: parent
                    active: shown !== null && shown.open === true
                    sourceComponent: AutomationLane {
                        objectName: "arrangementAutomationOverlay"
                        readonly property var automation: arrangement.bridge.arrangementAutomation.tracks
                            .find(entry => entry.trackId === arrangementRow.modelData.id) || null
                        overlay: true
                        controller: arrangement.bridge.arrangementAutomation
                        trackId: arrangementRow.modelData.id || ""
                        laneKey: automationOverlay.shown.overlay
                        laneData: automation
                            ? automation.lanes.find(lane => lane.key === automationOverlay.shown.overlay) || null
                            : null
                        viewStartBar: arrangement.viewStartBar
                        visibleBars: arrangement.visibleBars
                        snapToBeat: arrangement.snapToBeat
                        drawMode: arrangement.automationDrawMode
                        laneColor: arrangementRow.trackColor
                        lineColor: arrangement.lineColor
                        textColor: arrangement.mainTextColor
                        monoFamily: arrangement.theme.monoFamily
                        onDrawToggled: arrangement.automationDrawToggled()
                    }
                }
                // A browser card dropped on the lane swaps the instrument or
                // adds the effect to this track's chain.
                TrackDeviceDrop {
                    objectName: "arrangementRowDeviceDrop"
                    z: 5
                    anchors.fill: parent
                    bridge: arrangement.bridge
                    track: arrangementRow.modelData
                    trackIndex: arrangementRow.index
                    accentColor: arrangement.accentColor
                }
            }
        }
        // Each track's open automation lanes, under its clips (ADR-0051).
        Repeater {
            model: arrangement.bridge.tracks.length
            delegate: Repeater {
                id: laneGroup
                required property int index
                readonly property var track: arrangement.bridge.tracks[index] || ({})
                readonly property var automation: arrangement.bridge.arrangementAutomation.tracks
                    .find(entry => entry.trackId === track.id) || null
                model: arrangement.rowLayout[index] ? arrangement.rowLayout[index].lanes : []
                delegate: AutomationLane {
                    required property var modelData
                    y: modelData.top - arrangement.trackScrollY
                    width: arrangement.width
                    height: modelData.height
                    visible: y < arrangementTrackViewport.height && y + height > 0
                    controller: arrangement.bridge.arrangementAutomation
                    trackId: laneGroup.track.id || ""
                    laneKey: modelData.key
                    laneData: laneGroup.automation
                        ? laneGroup.automation.lanes.find(lane => lane.key === modelData.key) || null
                        : null
                    viewStartBar: arrangement.viewStartBar
                    visibleBars: arrangement.visibleBars
                    snapToBeat: arrangement.snapToBeat
                    drawMode: arrangement.automationDrawMode
                    laneColor: arrangement.theme.trackColor(laneGroup.track, laneGroup.index)
                    lineColor: arrangement.lineColor
                    textColor: arrangement.mainTextColor
                    monoFamily: arrangement.theme.monoFamily
                }
            }
        }
    }
    // An instrument dropped under the last lane makes a new track.
    DeviceTrackDrop {
        objectName: "arrangementEmptyDrop"
        readonly property real rowsBottom: arrangement.rowLayout.length > 0
            ? arrangement.rowLayout[arrangement.rowLayout.length - 1].bottom
            : 0
        x: 0
        y: arrangementTrackViewport.y + Math.min(arrangementTrackViewport.height,
            Math.max(0, rowsBottom - arrangement.trackScrollY))
        width: parent.width
        height: masterArrangementRow.y - y
        bridge: arrangement.bridge
        accentColor: arrangement.accentColor
    }
    ArrangementCreationLane {
        anchors.bottom: masterArrangementRow.top
        width: parent.width
        height: arrangement.creationHeight
        barCount: arrangement.visibleBars
    }
    Rectangle {
        objectName: "arrangementPlayhead"
        x: (arrangement.bridge.playheadBar - arrangement.viewStartBar)
            * arrangement.width / arrangement.visibleBars
        y: 0
        width: Theme.strokeBold
        height: arrangement.height
        z: 10
        visible: arrangement.bridge.playheadBar >= arrangement.viewStartBar
            && arrangement.bridge.playheadBar <= arrangement.viewStartBar + arrangement.visibleBars
        color: arrangement.amberColor
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Theme.playheadGlowWidth
            height: parent.height
            color: arrangement.tint(arrangement.amberColor, Theme.playheadGlowAlpha)
        }
    }
    Rectangle {
        id: masterArrangementRow
        objectName: "masterArrangementRow"
        z: 3
        anchors.bottom: parent.bottom
        anchors.bottomMargin: arrangement.pianoRollHeight
            + arrangement.elapsedRulerHeight
        width: parent.width
        height: arrangement.masterRowHeight
        color: arrangement.selectedTrack === -2 ? Theme.masterRowGroundSelected : Theme.masterRowGround
        Rectangle { width: parent.width; height: Theme.hairline; color: arrangement.lineColor }
        MouseArea { anchors.fill: parent; onClicked: arrangement.trackSelected(-2) }
    }
    Rectangle {
        id: elapsedRuler
        objectName: "arrangementElapsedRuler"
        z: 4
        anchors.bottom: parent.bottom
        anchors.bottomMargin: arrangement.pianoRollHeight
        width: parent.width
        height: arrangement.elapsedRulerHeight
        visible: arrangement.elapsedRulerHeight > 0
        color: Theme.elapsedRulerGround
        border.color: arrangement.lineColor
        // The bar ruler's ticks, labelled with their elapsed time at least
        // 80 px apart (with milliseconds once beats are labelled).
        Repeater {
            model: arrangement.rulerTicks
            delegate: Item {
                required property var modelData
                required property int index
                readonly property real spacing: arrangement.rulerStep * arrangement.pixelsPerBar
                readonly property int stride: Math.max(1, Math.ceil(Theme.elapsedLabelMinSpacing / Math.max(1, spacing)))
                visible: ((modelData.step % stride) + stride) % stride === 0
                x: (modelData.at - arrangement.viewStartBar) * arrangement.pixelsPerBar
                width: Theme.elapsedTickWidth
                height: elapsedRuler.height
                Rectangle {
                    width: Theme.hairline
                    height: Theme.elapsedTickHeight
                    color: arrangement.dimTextColor
                }
                Text {
                    x: Theme.elapsedTickLabelX
                    anchors.verticalCenter: parent.verticalCenter
                    text: arrangement.bridge.revision >= 0
                        ? arrangement.formatElapsed(arrangement.bridge.elapsed_seconds_at_bar(parent.modelData.at),
                                                    !parent.modelData.onBar || arrangement.pixelsPerBar > Theme.elapsedMillisMinBarWidth)
                        : "--:--"
                    color: arrangement.dimTextColor
                    font.pixelSize: Theme.typeSmall
                    font.family: arrangement.theme.monoFamily
                }
            }
        }
        Rectangle {
            z: 2
            anchors.right: parent.right
            width: Theme.elapsedReadoutWidth
            height: parent.height
            color: Theme.panelRaised
            border.color: arrangement.lineColor
            Text {
                anchors.centerIn: parent
                text: "TIME  " + arrangement.formatElapsed(arrangement.bridge.playheadSeconds, true)
                color: arrangement.accentColor
                font.pixelSize: Theme.typeBody
                font.family: "monospace"
                font.bold: true
            }
        }
    }
    Column {
        visible: arrangement.bridge.tracks.length === 0
        anchors.centerIn: parent
        spacing: Theme.spaceMd
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "YOUR ARRANGEMENT STARTS HERE"
            color: arrangement.dimTextColor
            font.pixelSize: Theme.typeEmphasis
            font.letterSpacing: Theme.emptyArrangementLetterSpacing
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Drop audio or a sound from the browser here, or use + Track"
            color: arrangement.dimTextColor
            opacity: Theme.emptyArrangementHintOpacity
            font.pixelSize: Theme.typeBody
        }
    }
    MouseArea {
        anchors.fill: parent
        z: 29
        visible: arrangement.contextClipId.length > 0
        onClicked: arrangement.contextClipId = ""
    }
    ClipTempoPopover {
        id: clipTempoPopover
        anchors.fill: parent
        controller: arrangement.bridge.clipTempo
        amberColor: arrangement.amberColor
        accentColor: arrangement.accentColor
        lineColor: arrangement.lineColor
        textColor: arrangement.mainTextColor
        dimColor: arrangement.dimTextColor
        monoFamily: arrangement.theme.monoFamily
    }
    AudioClipContextMenu {
        id: clipContextMenu
        z: 30
        visible: arrangement.contextClipId.length > 0 && arrangement.contextClipKind === "audio"
        x: arrangement.contextMenuX
        y: arrangement.contextMenuY
        bridge: arrangement.bridge
        clipId: arrangement.contextClipId
        clipColor: arrangement.contextClipColor
        clipName: arrangement.contextClipName
        timebase: arrangement.contextClipTimebase
        raisedColor: arrangement.raisedColor
        lineColor: arrangement.lineColor
        mainTextColor: arrangement.mainTextColor
        dimTextColor: arrangement.dimTextColor
        onCloseRequested: arrangement.contextClipId = ""
        onClipDeleted: arrangement.clipSelected("")
    }
    MidiClipContextMenu {
        id: midiClipContextMenu
        z: 30
        visible: arrangement.contextClipId.length > 0 && arrangement.contextClipKind === "midi"
        x: arrangement.contextMenuX
        y: arrangement.contextMenuY
        bridge: arrangement.bridge
        clipId: arrangement.contextClipId
        clipColor: arrangement.contextClipColor
        clipName: arrangement.contextClipName
        timebase: arrangement.contextClipTimebase
        looping: arrangement.contextClipLoop
        raisedColor: arrangement.raisedColor
        lineColor: arrangement.lineColor
        mainTextColor: arrangement.mainTextColor
        dimTextColor: arrangement.dimTextColor
        onCloseRequested: arrangement.contextClipId = ""
        onClipDeleted: arrangement.clipSelected("")
        onClipsMerged: arrangement.clipsSelected(Array.from(arrangement.bridge.selectedClipIds))
    }
}
