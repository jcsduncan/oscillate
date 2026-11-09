pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: trackPanel
    // The instrument track whose header is under a point of `item`, or
    // null: a MIDI file dropped there plays on that track from bar 1.
    function instrumentDropTarget(item, x, y) {
        const point = item.mapToItem(trackList, x, y)
        if (point.x < 0 || point.x >= trackList.width || point.y < 0 || point.y >= trackList.height)
            return null
        const at = point.y + trackList.contentY
        for (let row = 0; row < rowLayout.length; ++row) {
            const layout = rowLayout[row]
            const track = bridge.tracks[row]
            if (track && track.kind === "instrument" && at >= layout.top && at < layout.top + layout.height)
                return { trackId: track.id, bar: 0 }
        }
        return null
    }
    required property var bridge
    required property int selectedTrack
    required property int pianoRollHeight
    required property int creationHeight
    required property int elapsedRulerHeight
    required property var rowLayout
    required property real rowsContentHeight
    // The workspace's layout ({rows, returnsTop, gapTop, height}), the same
    // without a drop slot open, and the drag's target (or null).
    required property var trackLayout
    required property var baseTrackLayout
    required property var trackDrop
    // Track id → {open, keys}: the automation lanes shown under each track.
    required property var automationView
    required property bool automationDrawMode
    // The tempo lane's header row under the creation row (ADR-0112).
    required property int tempoLaneHeight
    required property bool tempoLaneExpanded
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    // The shell's kit theme (devicekit/ShellTheme): track colours and type.
    required property var theme
    // True once the first rows are laid out: later rows are new tracks.
    property bool rowsSettled: false
    Component.onCompleted: Qt.callLater(() => trackPanel.rowsSettled = true)
    // The track whose name should start editing (set by the context menu).
    property string renamingTrackId: ""
    // Tracks selected together (Ctrl-click adds or removes one, Shift-click a
    // run), for sending them to a bus; a plain click selects one again.
    property var selectedTrackIds: []
    property int selectionAnchor: -1
    function trackIdAt(index) {
        const tracks = bridge.tracks
        return index >= 0 && index < tracks.length ? tracks[index].id : ""
    }
    function selectTrack(index, modifiers) {
        const id = trackIdAt(index)
        if (id === "")
            return
        if (modifiers & Qt.ControlModifier) {
            const ids = selectedTrackIds.length > 0 ? selectedTrackIds.slice()
                                                    : [trackIdAt(selectedTrack)].filter(i => i !== "")
            const at = ids.indexOf(id)
            if (at >= 0)
                ids.splice(at, 1)
            else
                ids.push(id)
            selectedTrackIds = ids
            selectionAnchor = index
            if (at < 0)
                trackSelected(index)
        } else if ((modifiers & Qt.ShiftModifier) && (selectionAnchor >= 0 || selectedTrack >= 0)) {
            // A run as the list shows it, top down.
            const tracks = bridge.tracks
            const from = tracks[selectionAnchor >= 0 ? selectionAnchor : selectedTrack]
            const to = tracks[index]
            const low = Math.min(from ? from.displayIndex : to.displayIndex, to.displayIndex)
            const high = Math.max(from ? from.displayIndex : to.displayIndex, to.displayIndex)
            selectedTrackIds = tracks.filter(track => track.displayIndex >= low && track.displayIndex <= high)
                .sort((a, b) => a.displayIndex - b.displayIndex)
                .map(track => track.id)
            trackSelected(index)
        } else {
            selectedTrackIds = [id]
            selectionAnchor = index
            trackSelected(index)
        }
    }
    // Selected elsewhere (a clip, the mixer): that track alone again.
    onSelectedTrackChanged: {
        const id = trackIdAt(selectedTrack)
        if (id === "") {
            // No track selected (master or none): no rows stay highlighted.
            selectedTrackIds = []
            selectionAnchor = -1
        } else if (!selectedTrackIds.includes(id)) {
            selectedTrackIds = [id]
            selectionAnchor = selectedTrack
        }
    }
    function isBus(track) { return track.kind === "group" || track.kind === "return" }
    // Header button tooltips go through the shell's one tooltip (shell/Tips).
    function showTip(item, text, shortcut) { Tips.request(item, text, shortcut || "") }
    function hideTip(text) {
        if (Tips.text === text)
            Tips.release(Tips.item)
    }
    function openTrackMenu(track, index, point) {
        // A right-click inside the selection acts on all of it.
        if (!trackPanel.selectedTrackIds.includes(track.id))
            trackPanel.selectTrack(index, Qt.NoModifier)
        trackMenu.trackIds = trackPanel.selectedTrackIds.length > 1 ? trackPanel.selectedTrackIds : [track.id]
        colourPopover.close()
        trackMenu.track = track
        trackMenu.x = Math.max(Theme.spaceXs, Math.min(trackPanel.width - trackMenu.width - Theme.spaceXs, point.x))
        trackMenu.y = Math.max(Theme.spaceXs, Math.min(trackPanel.height - trackMenu.height - Theme.spaceXs, point.y))
        trackMenu.visible = true
        trackMenu.forceActiveFocus()
    }
    // The control that opened the palette, duck list or parameter chooser:
    // Escape (or a keyboard choice) hands focus back to it.
    property Item popupOpener: null
    function rememberOpener() { popupOpener = trackPanel.Window.activeFocusItem }
    function restoreOpener() {
        const opener = popupOpener
        popupOpener = null
        if (opener && opener.visible)
            opener.forceActiveFocus()
    }
    // A focused popup takes these keys before the shell's bare-key shortcuts
    // (Space plays, Escape lets go of the selected clip).
    readonly property var popupKeys: [Qt.Key_Space, Qt.Key_Return, Qt.Key_Escape,
                                      Qt.Key_Up, Qt.Key_Down, Qt.Key_Left, Qt.Key_Right]
    readonly property var buttonKeys: [Qt.Key_Space, Qt.Key_Return]
    function claimKeys(event, keys) { event.accepted = keys.indexOf(event.key) >= 0 }
    function openDuck(track, point) {
        rememberOpener()
        trackMenu.visible = false
        duckPopover.open(track, point)
    }
    function openColours(track, point) {
        rememberOpener()
        trackMenu.visible = false
        colourPopover.track = track
        const palette = trackPanel.theme.trackPalette
        colourPopover.current = Math.max(0, palette.findIndex(colour =>
            String(track.color).toLowerCase() === colour.toLowerCase()))
        colourPopover.x = Math.max(Theme.spaceXs, Math.min(trackPanel.width - colourPopover.width - Theme.spaceXs, point.x))
        colourPopover.y = Math.max(Theme.spaceXs, Math.min(trackPanel.height - colourPopover.height - Theme.spaceXs, point.y))
        colourPopover.visible = true
        colourPopover.forceActiveFocus()
    }
    // A header drag: the tracks it moves (the selection when the dragged
    // row is in it), the row pressed, and the pointer in the list's frame.
    property var dragIds: []
    property string dragLeadId: ""
    property real dragPointerX: -1
    property real dragPointerY: -1
    readonly property bool dragging: dragIds.length > 0
    // A row's upper half drops above it, the lower half below.
    readonly property real rowMiddle: 0.5
    function trackWithId(id) {
        return bridge.tracks.find(track => track.id === id) || null
    }
    function busColorOf(busId) {
        const tracks = bridge.tracks
        const index = tracks.findIndex(track => track.id === busId)
        return index >= 0 ? theme.trackColor(tracks[index], index) : accentColor
    }
    // "Music bus" for a bus named Music; a name with "bus" in it stays.
    function busLabel(name) {
        return /\bbus\b/i.test(name) ? name : name + " bus"
    }
    // Where a drop at `contentY` (the scrolled list, slot open) lands: in a
    // group bus's bracket (busId; beforeId is the member it goes above, or
    // empty for the bracket's end; the bus header itself takes it as the
    // last member) or on the master (busId empty; beforeId the row it goes
    // above, or empty for the end). The top of a bus header and the bottom
    // of its last member drop outside the bracket.
    function dropAt(contentY) {
        const slot = trackLayout.gapTop
        let at = contentY
        if (slot >= 0 && at >= slot) {
            if (at < slot + Theme.trackDropSlotHeight)
                return trackDrop
            at -= Theme.trackDropSlotHeight
        }
        const tracks = bridge.tracks
        const rows = baseTrackLayout.rows
        const order = []
        tracks.forEach((track, index) => {
            if (track.kind !== "return" && rows[index] && !rows[index].hidden)
                order.push(index)
        })
        order.sort((a, b) => rows[a].top - rows[b].top)
        function topLevelAfter(position) {
            for (let next = position + 1; next < order.length; ++next) {
                if (tracks[order[next]].busId === "")
                    return tracks[order[next]].id
            }
            return ""
        }
        const edge = Theme.trackDropEdgeFraction
        // A bus never goes inside a bus (itself included): to a dragged bus
        // another bus's header is a row on the master like any other.
        const movingBus = dragIds.some(id => {
            const track = trackWithId(id)
            return track !== null && isBus(track)
        })
        let target = { busId: "", beforeId: "" }
        for (let position = 0; position < order.length; ++position) {
            const track = tracks[order[position]]
            const row = rows[order[position]]
            if (at < row.top || at >= row.bottom)
                continue
            const fraction = (at - row.top) / Math.max(1, row.bottom - row.top)
            if (track.kind === "group" && movingBus) {
                target = { busId: "", beforeId: fraction < rowMiddle ? track.id : topLevelAfter(position) }
            } else if (track.kind === "group") {
                target = fraction < edge ? { busId: "", beforeId: track.id } : { busId: track.id, beforeId: "" }
            } else if (track.busId !== "") {
                const following = position + 1 < order.length ? tracks[order[position + 1]] : null
                if (track.lastMember && fraction >= 1 - edge)
                    target = { busId: "", beforeId: topLevelAfter(position) }
                else
                    target = { busId: track.busId, beforeId: fraction < rowMiddle ? track.id
                               : following && following.busId === track.busId ? following.id : "" }
            } else {
                target = { busId: "", beforeId: fraction < rowMiddle ? track.id : topLevelAfter(position) }
            }
            break
        }
        const bus = trackWithId(target.busId)
        return { ids: dragIds, busId: target.busId, beforeId: target.beforeId,
                 refused: target.busId !== "" && movingBus,
                 busName: bus ? bus.name : "" }
    }
    function beginDrag(track) {
        const several = selectedTrackIds.length > 1 && selectedTrackIds.includes(track.id)
        dragLeadId = track.id
        dragIds = (several ? selectedTrackIds : [track.id]).filter(id => {
            const chosen = trackWithId(id)
            return chosen !== null && chosen.kind !== "return"
        })
    }
    // A drop routes and moves the tracks as one undo step; the dragged row
    // stays selected.
    function finishDrag(commit) {
        const drop = trackDrop
        const ids = dragIds
        const lead = dragLeadId
        dragIds = []
        dragLeadId = ""
        dragPointerX = -1
        dragPointerY = -1
        trackDropMoved(null)
        if (!commit || !drop || ids.length === 0)
            return
        bridge.trackCommands.place_tracks(ids, drop.busId, drop.beforeId)
        const index = bridge.tracks.findIndex(track => track.id === lead)
        if (index >= 0)
            trackSelected(index)
    }

    // A parameter key's name on a track, with its group unless a mixer one.
    function targetName(trackId, key) {
        const entry = bridge.arrangementAutomation.tracks.find(track => track.trackId === trackId)
        if (entry) {
            for (const group of entry.targets) {
                const item = group.items.find(candidate => candidate.key === key)
                if (item)
                    return group.group === "Mixer" ? item.name : group.group + " · " + item.name
            }
            const lane = entry.lanes.find(candidate => candidate.key === key)
            if (lane)
                return lane.name
        }
        return key
    }

    function updateDrop() {
        if (!dragging || dragPointerY < 0)
            return
        const drop = dropAt(trackList.contentY + dragPointerY)
        if (!trackDrop || drop.busId !== trackDrop.busId || drop.beforeId !== trackDrop.beforeId
                || drop.refused !== trackDrop.refused)
            trackDropMoved(drop)
    }

    // What a track is and what it plays, in a few words.
    function trackSummary(track) {
        const clips = track.clipCount + (track.clipCount === 1 ? " clip" : " clips")
        if (track.kind === "instrument") {
            const source = track.nativeEngineName && track.nativeEngineName.length > 0
                ? track.nativeEngineName
                : track.soundName && track.soundName.length > 0 ? track.soundName : "Instrument"
            return source + " · " + clips
        }
        if (track.kind === "audio")
            return "Audio · " + clips
        if (track.kind === "group")
            return memberCountText(track) + " → " + track.mainDestinationName
        return track.fedBy.length > 0 ? "Return · fed by " + track.fedBy.join(", ") : "Return · no sends yet"
    }
    function memberCountText(track) {
        return track.memberCount + (track.memberCount === 1 ? " track" : " tracks")
    }
    // A pill per return this track's sends feed, in the return's colour.
    function sendPills(track) {
        if (track.kind === "return")
            return []
        const tracks = bridge.tracks
        const pills = []
        for (const send of track.sends || []) {
            if (send.purpose !== "audible" || !(send.gain > 0))
                continue
            const index = tracks.findIndex(other => other.id === send.destinationId)
            if (index >= 0 && tracks[index].kind === "return")
                pills.push({ label: "↗ " + tracks[index].name, color: theme.trackColor(tracks[index], index) })
        }
        return pills
    }
    // A return's first effect, for its badge.
    function returnEffect(index) {
        const chain = bridge.insertChains[index]
        return chain && chain.effects && chain.effects.length > 0 ? chain.effects[0].name : ""
    }

    // One send knob per return track (the first two, A and B), each in its
    // return's colour. A send exists only once its knob is turned up; until
    // then the knob shows off and its first move creates the send.
    function sendKnobs(track, trackIndex) {
        if (track.kind === "return")
            return []
        const returns = []
        bridge.tracks.forEach((other, index) => {
            if (other.kind === "return" && returns.length < 2)
                returns.push({ track: other, index: index })
        })
        return returns.map((entry, slot) => {
            const name = String(entry.track.name || "")
            const lettered = /^([A-Z])\s*·\s*(.*)$/.exec(name)
            const sends = track.sends || []
            const send = sends.find(item => (item.destinationId || item.returnId) === entry.track.id
                && (item.purpose === undefined || item.purpose === "audible"))
            return {
                send: send || { gain: 0 },
                returnId: entry.track.id,
                trackId: track.id,
                letter: lettered ? lettered[1] : String.fromCharCode(65 + slot),
                returnName: lettered ? lettered[2] : name,
                color: theme.trackColor(entry.track, entry.index),
                trackName: track.name
            }
        })
    }

    signal trackSelected(int index)
    signal trackDropMoved(var drop)
    signal scrollPositionChanged(real contentY)
    signal tempoLaneToggled()
    // Scrolls the rows (and so the arrangement beside them) by pixels.
    function scrollBy(pixels) {
        const limit = Math.max(0, trackList.contentHeight - trackList.height)
        trackList.contentY = Math.max(0, Math.min(limit, trackList.contentY + pixels))
    }
    signal addInstrumentRequested()
    signal importAudioRequested()
    signal importMidiRequested()
    signal addGroupRequested()
    signal addReturnRequested()
    signal resizePreview(string trackId, real height)
    signal resizeCommitted(string trackId, real height)
    signal resizeCanceled(string trackId)
    signal automationToggled(string trackId)
    signal automationLaneAdded(string trackId)
    signal automationLaneChosen(string trackId, int index, string key)
    signal automationLaneHidden(string trackId, int index)
    signal automationDrawToggled()

    // A compact send knob: drag vertically (one undo step on release),
    // double-click for off, arrow keys step. 0 is off; the curve puts unity
    // at about 70% and +6 dB at the top.
    component SendKnob: Item {
        id: knob
        required property var spec
        // Inline components cannot see trackPanel's id; the delegate hands
        // these in.
        required property var bridge
        required property color lineColor
        required property color mainTextColor
        required property color dimTextColor
        required property string monoFamily
        // Sends run to +6 dB (gain 2) on a square-law dial: an arc from
        // arcStart through arcSweep; below litPosition it reads as off.
        readonly property real maxGain: 2
        readonly property real offGain: 0.0001
        readonly property real gainEpsilon: 1e-6
        readonly property real litPosition: 0.001
        readonly property real keyStep: 0.05
        readonly property real dragTravel: 120
        readonly property real arcStart: Math.PI * 0.75
        readonly property real arcSweep: Math.PI * 1.5
        readonly property real gain: Number(spec.send.gain || 0)
        property real dragPosition: -1
        readonly property real position: dragPosition >= 0 ? dragPosition
            : Math.sqrt(Math.max(0, Math.min(maxGain, gain)) / maxGain)
        readonly property real shownGain: maxGain * position * position
        readonly property string dbText: shownGain <= offGain ? "Off"
            : (20 * Math.log(shownGain) / Math.LN10).toFixed(1) + " dB"
        function commit(position) {
            const value = maxGain * Math.max(0, Math.min(1, position)) * Math.max(0, Math.min(1, position))
            if (Math.abs(value - gain) > gainEpsilon)
                bridge.set_send_level(spec.trackId, spec.returnId, value)
        }
        width: Theme.sendKnobSize
        height: Theme.sendKnobSize
        objectName: "trackSendKnob" + spec.letter
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: "Send " + spec.letter + " to " + spec.returnName + " from " + spec.trackName
        Accessible.description: dbText
        // A focused knob keeps its own keys ahead of the shell's shortcuts.
        Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Delete
            || event.key === Qt.Key_Up || event.key === Qt.Key_Down
        Keys.onUpPressed: commit(position + keyStep)
        Keys.onDownPressed: commit(position - keyStep)
        Keys.onDeletePressed: commit(0)
        Canvas {
            id: dial
            anchors.fill: parent
            readonly property real value: knob.position
            readonly property color ring: knob.spec.color
            readonly property bool focused: knob.activeFocus
            onValueChanged: requestPaint()
            onRingChanged: requestPaint()
            onFocusedChanged: requestPaint()
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                const cx = width / 2
                const cy = height / 2
                const start = knob.arcStart
                const sweep = knob.arcSweep
                context.lineCap = "round"
                context.lineWidth = Theme.sendKnobStroke
                context.strokeStyle = Theme.sendKnobTrack
                context.beginPath()
                context.arc(cx, cy, Theme.sendKnobArcRadius, start, start + sweep)
                context.stroke()
                if (value > knob.litPosition) {
                    context.strokeStyle = ring
                    context.beginPath()
                    context.arc(cx, cy, Theme.sendKnobArcRadius, start, start + sweep * value)
                    context.stroke()
                }
                context.fillStyle = focused ? Theme.sendKnobCapFocused : Theme.sendKnobCap
                context.beginPath()
                context.arc(cx, cy, Theme.sendKnobCapRadius, 0, Math.PI * 2)
                context.fill()
            }
        }
        Text {
            anchors.centerIn: parent
            text: knob.spec.letter
            color: knob.position > knob.litPosition ? knob.spec.color : knob.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
        }
        MouseArea {
            id: knobArea
            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            cursorShape: Qt.SizeVerCursor
            property real pressY: 0
            property real pressPosition: 0
            onPressed: mouse => {
                pressY = mouse.y
                pressPosition = knob.position
                knob.dragPosition = knob.position
            }
            onPositionChanged: mouse => {
                if (pressed)
                    knob.dragPosition = Math.max(0, Math.min(1, pressPosition + (pressY - mouse.y) / knob.dragTravel))
            }
            onReleased: {
                const position = knob.dragPosition
                knob.dragPosition = -1
                // A click that did not move writes nothing (a gain above the
                // knob's range stays as it is).
                if (position !== pressPosition)
                    knob.commit(position)
            }
            onCanceled: knob.dragPosition = -1
            onDoubleClicked: {
                knob.dragPosition = -1
                knob.commit(0)
            }
        }
        Rectangle {
            visible: knobArea.containsMouse || knobArea.pressed
            z: 20
            anchors.bottom: parent.top
            anchors.bottomMargin: Theme.spaceXs
            anchors.horizontalCenter: parent.horizontalCenter
            width: tipText.implicitWidth + Theme.sendKnobTipPadding
            height: Theme.sendKnobTipHeight
            radius: Theme.sendKnobTipRadius
            color: Theme.canvas
            border.color: knob.lineColor
            Text {
                id: tipText
                anchors.centerIn: parent
                text: knob.spec.letter + " · " + knob.spec.returnName + "  " + knob.dbText
                color: knob.mainTextColor
                font.pixelSize: Theme.typeSmall
                font.family: knob.monoFamily
            }
        }
    }

    color: panelColor
    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: trackPanel.lineColor
    }
    TrackCreationRow {
        id: trackCreationRow
        width: trackPanel.width
        z: 4
        onAddInstrument: trackPanel.addInstrumentRequested()
        onImportAudio: trackPanel.importAudioRequested()
        onImportMidi: trackPanel.importMidiRequested()
        onAddGroup: trackPanel.addGroupRequested()
        onAddReturn: trackPanel.addReturnRequested()
        onAddAudio: trackPanel.bridge.trackCommands.add_audio_track()
    }

    Rectangle {
        id: tempoLaneHeader
        objectName: "tempoLaneHeader"
        y: trackCreationRow.height
        width: trackPanel.width - Theme.hairline
        height: trackPanel.tempoLaneHeight
        color: Theme.tempoLaneGround
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: trackPanel.tempoLaneExpanded ? "Collapse the project tempo lane" : "Expand the project tempo lane to draw ramps"
        Keys.onReturnPressed: trackPanel.tempoLaneToggled()
        Keys.onSpacePressed: trackPanel.tempoLaneToggled()
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: Theme.hairline
            color: trackPanel.lineColor
        }
        Row {
            x: Theme.spaceLg
            height: Math.min(parent.height, Theme.tempoLaneCollapsedHeight)
            spacing: Theme.spaceSm
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: trackPanel.tempoLaneExpanded ? "▾" : "▸"
                color: trackPanel.dimTextColor
                font.pixelSize: Theme.typeMicro
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.tempoLaneHeaderSwatch
                height: Theme.tempoLaneHeaderSwatch
                radius: Theme.radiusXs
                color: Theme.tempoLaneCurve
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Project tempo"
                color: trackPanel.dimTextColor
                font.pixelSize: Theme.typeMicro
            }
        }
        MouseArea {
            Tip { text: "Tempo lane: draw ramps across the mix" }
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: trackPanel.tempoLaneToggled()
        }
    }

    Flickable {
        id: trackList
        objectName: "trackList"
        x: 0
        y: trackCreationRow.height + trackPanel.tempoLaneHeight
        width: parent.width
        height: Math.max(0, parent.height - y - masterTrackRow.height
            - trackPanel.pianoRollHeight - trackPanel.elapsedRulerHeight)
        contentWidth: width
        contentHeight: trackPanel.rowsContentHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        onContentYChanged: trackPanel.scrollPositionChanged(contentY)
        Timer {
            interval: 25
            // Near the list's top or bottom edge, a dragged row scrolls it.
            readonly property real edgeMargin: 30
            readonly property real scrollStep: 12
            repeat: true
            running: trackPanel.dragging
            onTriggered: {
                const margin = edgeMargin
                let delta = 0
                if (trackPanel.dragPointerY < margin)
                    delta = -scrollStep
                else if (trackPanel.dragPointerY > trackList.height - margin)
                    delta = scrollStep
                const limit = Math.max(0, trackList.contentHeight - trackList.height)
                trackList.contentY = Math.max(0, Math.min(limit, trackList.contentY + delta))
                trackPanel.updateDrop()
            }
        }
        Repeater {
            model: trackPanel.bridge.trackModel
            delegate: Rectangle {
                id: trackRow
                required property var modelData
                required property int index
                readonly property color trackColor: trackPanel.theme.trackColor(modelData, index)
                readonly property bool selected: trackPanel.selectedTrack === trackRow.index
                    || (trackPanel.selectedTrackIds.length > 1
                        && trackPanel.selectedTrackIds.includes(trackRow.modelData.id))
                readonly property var sends: trackPanel.sendKnobs(modelData, index)
                // Bus nesting (TrackRoutingView::Nesting): a group bus heads
                // its members, which sit indented under it on its rail.
                readonly property var layoutEntry: trackPanel.rowLayout[trackRow.index] || null
                readonly property bool isGroup: modelData.kind === "group"
                readonly property bool isReturn: modelData.kind === "return"
                readonly property bool nested: modelData.busId !== undefined && modelData.busId !== ""
                readonly property color busColor: nested ? trackPanel.busColorOf(modelData.busId) : trackColor
                readonly property bool collapsed: isGroup && trackPanel.bridge.collapsedBuses.includes(modelData.id)
                readonly property int contentShift: isGroup ? Theme.trackBusContentShift
                                                            : nested ? Theme.trackMemberIndent : 0
                readonly property bool ghosted: trackPanel.dragIds.includes(modelData.id)
                // The rail lights while a drag would drop into this bus.
                readonly property bool railGlowing: nested && trackPanel.trackDrop !== null
                    && !trackPanel.trackDrop.refused && trackPanel.trackDrop.busId === modelData.busId
                readonly property var pills: trackPanel.sendPills(modelData)
                readonly property string effectName: isReturn ? trackPanel.returnEffect(index) : ""
                visible: !(layoutEntry && layoutEntry.hidden)
                x: 0
                y: layoutEntry ? layoutEntry.top : trackRow.index * Theme.trackRowHeight
                width: trackPanel.width
                height: layoutEntry ? layoutEntry.height : Theme.trackRowHeight
                objectName: "trackRow"
                color: selected ? trackPanel.theme.selection
                    : isReturn ? Theme.trackReturnGround : trackPanel.panelColor
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
                // A track added after the panel opened fades in.
                NumberAnimation on opacity {
                    id: rowArrival
                    running: false
                    from: 0
                    to: 1
                    duration: Motion.normal
                    easing.type: Motion.easing
                }
                Component.onCompleted: {
                    if (trackPanel.rowsSettled && Motion.enabled)
                        rowArrival.start()
                }
                activeFocusOnTab: true
                Accessible.role: Accessible.ListItem
                Accessible.name: trackRow.modelData.name + " " + trackRow.modelData.kind + " track"
                Accessible.description: trackPanel.trackSummary(trackRow.modelData)
                Keys.onReturnPressed: trackPanel.trackSelected(trackRow.index)
                Keys.onSpacePressed: trackPanel.trackSelected(trackRow.index)
                // A bus's header carries its colour lightly; its members fainter.
                Rectangle {
                    objectName: "trackBusTint"
                    visible: trackRow.isGroup || trackRow.nested
                    anchors.fill: parent
                    color: Qt.rgba(trackRow.busColor.r, trackRow.busColor.g, trackRow.busColor.b,
                                   trackRow.isGroup ? Theme.trackBusHeaderTintAlpha : Theme.trackMemberTintAlpha)
                }
                Rectangle {
                    objectName: "trackColorBar"
                    visible: !trackRow.nested
                    x: 0
                    width: trackRow.isGroup ? Theme.trackBusBarWidth : Theme.trackColorBarWidth
                    height: parent.height
                    color: trackRow.trackColor
                    opacity: trackRow.modelData.muted ? Theme.trackMutedOpacity : 1
                }
                // A member's rail, joining it to its bus; it ends just short
                // of the last member's bottom.
                Rectangle {
                    objectName: "trackBusRailGlow"
                    visible: trackRow.railGlowing
                    x: Theme.trackBusRailX + (Theme.trackBusRailWidth - Theme.trackBusRailGlowWidth) / 2
                    width: Theme.trackBusRailGlowWidth
                    height: busRail.height
                    radius: width / 2
                    color: Qt.rgba(trackRow.busColor.r, trackRow.busColor.g, trackRow.busColor.b,
                                   Theme.trackBusRailGlowAlpha)
                }
                Rectangle {
                    id: busRail
                    objectName: "trackBusRail"
                    visible: trackRow.nested
                    x: Theme.trackBusRailX
                    width: Theme.trackBusRailWidth
                    height: parent.height - (trackRow.modelData.lastMember ? Theme.trackBusRailEndGap : 0)
                    radius: trackRow.modelData.lastMember ? Theme.trackBusRailRadius : 0
                    color: trackRow.busColor
                }
                TrackBusChevron {
                    objectName: "trackBusChevron"
                    visible: trackRow.isGroup
                    z: 6
                    x: Theme.trackBusChevronX
                    y: trackName.y + (trackName.height - height) / 2
                    collapsed: trackRow.collapsed
                    busName: trackRow.modelData.name
                    lineColor: trackPanel.mainTextColor
                    accentColor: trackPanel.accentColor
                    onToggled: trackPanel.bridge.set_bus_collapsed(trackRow.modelData.id, !trackRow.collapsed)
                }
                // Frozen (ADR-0059): an icy wash and a badge; the render plays.
                readonly property bool frozen: trackRow.modelData.frozen === true
                readonly property bool rendering: trackPanel.bridge.freezeCommands
                    && trackPanel.bridge.freezeCommands.busy
                    && trackPanel.bridge.freezeCommands.trackId === trackRow.modelData.id
                Rectangle {
                    objectName: "trackFrozenTint"
                    x: Theme.trackColorBarWidth
                    width: parent.width - Theme.trackColorBarWidth
                    height: parent.height
                    visible: trackRow.frozen
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Theme.trackFrozenWashStart }
                        GradientStop { position: 1.0; color: Theme.trackFrozenWashEnd }
                    }
                }
                Rectangle {
                    objectName: "trackFrozenBadge"
                    z: 7
                    visible: trackRow.frozen && !trackRow.rendering
                    // Clear of the level meter at the row's right edge.
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.trackBadgeRightMargin
                    y: Theme.spaceXs
                    width: frozenLabel.implicitWidth + Theme.spaceLg
                    height: Theme.trackBadgeHeight
                    radius: Theme.trackBadgeRadius
                    color: Theme.trackFrozenBadgeFill
                    border.color: Theme.trackFrozenEdge
                    Accessible.role: Accessible.StaticText
                    Accessible.name: "Frozen: plays its render. Unfreeze from the track menu to edit."
                    Text {
                        id: frozenLabel
                        anchors.centerIn: parent
                        text: "❄ FROZEN"
                        color: Theme.trackFrozenText
                        font.pixelSize: Theme.typeMicro
                        font.bold: true
                        font.letterSpacing: Theme.capsLetterSpacing
                    }
                }
                // A render in progress: its bar and Cancel.
                Rectangle {
                    objectName: "trackRenderProgress"
                    z: 7
                    visible: trackRow.rendering
                    // Clear of the meter, and of the name (which ends 139 from the right).
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.trackBadgeRightMargin
                    y: Theme.spaceXs
                    width: Theme.trackRenderBarWidth
                    height: Theme.trackBadgeHeight
                    radius: Theme.trackBadgeRadius
                    color: Theme.trackRenderGround
                    border.color: Theme.trackFrozenEdge
                    Rectangle {
                        x: Theme.hairline; y: Theme.hairline
                        height: parent.height - 2 * Theme.hairline
                        radius: Theme.trackRenderFillRadius
                        width: Math.max(0, (parent.width - 2 * Theme.hairline)
                                        * (trackPanel.bridge.freezeCommands
                                           ? trackPanel.bridge.freezeCommands.progress : 0))
                        color: Theme.trackRenderFill
                        Behavior on width {
                            enabled: Motion.enabled
                            NumberAnimation { duration: Motion.fast }
                        }
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.trackRenderLabelInset
                        anchors.verticalCenter: parent.verticalCenter
                        text: trackPanel.bridge.freezeCommands
                              ? trackPanel.bridge.freezeCommands.action.toUpperCase() : ""
                        color: Theme.trackRenderText
                        font.pixelSize: Theme.typeMicro
                        font.bold: true
                        font.letterSpacing: Theme.capsLetterSpacing
                    }
                    Text {
                        objectName: "trackRenderCancel"
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.spaceSm
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✕"
                        color: Theme.trackRenderText
                        font.pixelSize: Theme.typeCaption
                        MouseArea {
                            Tip {}
                            anchors.fill: parent
                            anchors.margins: -Theme.spaceXs
                            Accessible.role: Accessible.Button
                            Accessible.name: "Cancel the render"
                            onClicked: trackPanel.bridge.freezeCommands.cancel()
                        }
                    }
                }
                readonly property bool hovered: rowArea.containsMouse
                readonly property bool tall: height >= Theme.trackRowTallHeight
                // The colour swatch: click for the palette.
                Rectangle {
                    id: colourSwatch
                    objectName: "trackColourSwatch"
                    z: 6
                    // Buses show an outlined swatch, tracks a filled one.
                    readonly property bool outlined: trackRow.isGroup || trackRow.isReturn
                    x: Theme.trackSwatchX + trackRow.contentShift
                    y: trackRow.tall ? Theme.trackSwatchTallY : Theme.trackSwatchY
                    width: Theme.trackSwatchSize
                    height: Theme.trackSwatchSize
                    radius: Theme.radiusSm
                    color: outlined ? "transparent" : trackRow.trackColor
                    border.width: activeFocus ? Theme.focusRingWidth : outlined ? Theme.trackBusSwatchBorder : Theme.hairline
                    border.color: activeFocus ? Theme.focusRing : swatchArea.containsMouse ? trackPanel.mainTextColor
                        : outlined ? trackRow.trackColor : Theme.canvas
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Colour of " + trackRow.modelData.name
                    Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.buttonKeys)
                    Keys.onReturnPressed: trackPanel.openColours(trackRow.modelData,
                        colourSwatch.mapToItem(trackPanel, 0, colourSwatch.height + Theme.spaceXs))
                    Keys.onSpacePressed: trackPanel.openColours(trackRow.modelData,
                        colourSwatch.mapToItem(trackPanel, 0, colourSwatch.height + Theme.spaceXs))
                    MouseArea {
                        id: swatchArea
                        anchors.fill: parent
                        anchors.margins: -Theme.trackSwatchHitOutset
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: trackPanel.showTip(colourSwatch, "Track colour")
                        onExited: trackPanel.hideTip("Track colour")
                        onClicked: {
                            trackPanel.hideTip("Track colour")
                            trackPanel.openColours(trackRow.modelData,
                                colourSwatch.mapToItem(trackPanel, 0, colourSwatch.height + Theme.spaceXs))
                        }
                    }
                }
                Text {
                    id: trackName
                    objectName: "trackName"
                    x: Theme.trackNameX + trackRow.contentShift
                    y: trackRow.tall ? Theme.trackNameTallY : Theme.trackNameY
                    width: Math.max(0, parent.width - Theme.trackNameRightReserve - trackRow.contentShift
                                    - (duckHint.visible ? duckHint.width + Theme.spaceSm : 0)
                                    - (effectBadge.visible ? effectBadge.width + Theme.spaceXs : 0))
                    text: trackRow.modelData.name
                    elide: Text.ElideRight
                    color: trackPanel.mainTextColor
                    font.pixelSize: Theme.typeEmphasis
                    font.weight: trackRow.isGroup ? Font.Bold : Font.DemiBold
                }
                // A return's effect, as a badge after its name.
                TrackBadge {
                    id: effectBadge
                    objectName: "trackReturnEffect"
                    visible: trackRow.effectName !== ""
                    z: 6
                    x: trackName.x + Math.min(trackName.implicitWidth, trackName.width) + Theme.spaceXs
                    y: trackName.y + (trackName.height - height) / 2
                    text: trackRow.effectName
                    tone: trackRow.trackColor
                }
                // A kick that keys nothing yet offers to duck the rest, on
                // hover or selection only, so it never nags.
                Rectangle {
                    id: duckHint
                    objectName: "trackDuckHint"
                    visible: trackRow.modelData.kickLike === true && trackRow.modelData.keysSidechain !== true
                        && trackPanel.bridge.tracks.length > 1 && (trackRow.hovered || trackRow.selected)
                    z: 6
                    x: trackName.x + Math.min(trackName.implicitWidth, trackName.width) + Theme.spaceSm
                    y: trackName.y
                    width: duckHintText.implicitWidth + Theme.trackDuckHintPadding
                    height: Theme.trackDuckHintHeight
                    radius: height / 2
                    color: duckHintArea.containsMouse ? Theme.menuHover : "transparent"
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: trackPanel.accentColor
                    activeFocusOnTab: visible
                    Accessible.role: Accessible.Button
                    Accessible.name: "Duck everything to " + trackRow.modelData.name
                    Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.buttonKeys)
                    Keys.onReturnPressed: trackPanel.openDuck(trackRow.modelData,
                        duckHint.mapToItem(trackPanel, 0, duckHint.height + Theme.spaceXs))
                    Keys.onSpacePressed: trackPanel.openDuck(trackRow.modelData,
                        duckHint.mapToItem(trackPanel, 0, duckHint.height + Theme.spaceXs))
                    // Its edge is always accent, so focus shows as a ring outside it.
                    FocusRing { targetRadius: duckHint.radius }
                    Text {
                        id: duckHintText
                        anchors.centerIn: parent
                        text: "Duck the rest"
                        color: trackPanel.accentColor
                        font.pixelSize: Theme.typeCaption
                        font.bold: true
                    }
                    MouseArea {
                        id: duckHintArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onEntered: trackPanel.showTip(duckHint, "A kick: Pump every other track, keyed from it")
                        onExited: trackPanel.hideTip("A kick: Pump every other track, keyed from it")
                        onClicked: {
                            trackPanel.hideTip("A kick: Pump every other track, keyed from it")
                            trackPanel.openDuck(trackRow.modelData,
                                duckHint.mapToItem(trackPanel, 0, duckHint.height + Theme.spaceXs))
                        }
                    }
                }
                // Double-click the row (or Rename in its menu) to edit the name.
                InlineNameEditor {
                    id: nameEditor
                    objectName: "trackNameEditor"
                    z: 7
                    x: trackName.x - Theme.trackNameEditorInsetX
                    y: trackName.y - Theme.trackNameEditorInsetY
                    width: Math.max(Theme.trackNameEditorMinWidth, trackName.width + 2 * Theme.trackNameEditorInsetX)
                    height: Theme.trackNameEditorHeight
                    textColor: trackPanel.mainTextColor
                    accentColor: trackPanel.accentColor
                    onCommitted: name => trackPanel.bridge.set_track_name(trackRow.modelData.id, name)
                }
                readonly property bool renameRequested: trackPanel.renamingTrackId === modelData.id
                onRenameRequestedChanged: {
                    if (renameRequested) {
                        trackPanel.renamingTrackId = ""
                        nameEditor.begin(trackRow.modelData.name)
                    }
                }
                readonly property var automationShown: trackPanel.automationView[modelData.id] || null
                readonly property bool automationOpen: automationShown !== null && automationShown.open === true
                // With automation shown, the parameter laid over the clips;
                // click to choose another.
                Rectangle {
                    objectName: "trackAutomationChooser"
                    visible: trackRow.automationOpen
                    z: 3
                    x: Theme.trackChooserX
                    y: Theme.trackChooserY
                    width: Math.max(0, parent.width - Theme.trackDetailRightReserve)
                    height: Theme.trackChooserHeight
                    radius: Theme.trackChooserRadius
                    color: overlayChooserArea.containsMouse ? Theme.buttonFace : Theme.trackChooserGround
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: activeFocus ? Theme.focusRing : trackRow.trackColor
                    border.width: activeFocus ? Theme.focusRingWidth : Theme.hairline
                    activeFocusOnTab: trackRow.automationOpen
                    Accessible.role: Accessible.ComboBox
                    Accessible.name: "Automation shown on the clips: "
                        + (trackRow.automationOpen ? trackPanel.targetName(trackRow.modelData.id, trackRow.automationShown.overlay) : "")
                    Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.buttonKeys)
                    Keys.onReturnPressed: chooser.openFor(trackRow.modelData.id, -1, trackRow.y + Theme.trackChooserMenuOffset)
                    Keys.onSpacePressed: chooser.openFor(trackRow.modelData.id, -1, trackRow.y + Theme.trackChooserMenuOffset)
                    Text {
                        x: Theme.spaceSm
                        width: parent.width - Theme.trackChooserTextRightGap
                        anchors.verticalCenter: parent.verticalCenter
                        text: trackRow.automationOpen
                            ? trackPanel.targetName(trackRow.modelData.id, trackRow.automationShown.overlay) : ""
                        elide: Text.ElideRight
                        color: trackPanel.mainTextColor
                        font.pixelSize: Theme.typeBody
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.trackChooserArrowMargin
                        anchors.verticalCenter: parent.verticalCenter
                        text: "▾"
                        color: trackPanel.dimTextColor
                        font.pixelSize: Theme.typeSmall
                    }
                    MouseArea {
                        Tip {}
                        id: overlayChooserArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: chooser.openFor(trackRow.modelData.id, -1, trackRow.y + Theme.trackChooserMenuOffset)
                    }
                }
                // What it is: a bus's member count, a return's feeders, and
                // pills for the returns a track's sends feed.
                Row {
                    id: summaryLine
                    objectName: "trackSummaryLine"
                    visible: !trackRow.automationOpen
                    // Members step in; the line stops short of the row's buttons.
                    x: Theme.trackSummaryX + (trackRow.nested ? Theme.trackMemberIndent : 0)
                    y: trackRow.tall ? Theme.trackSummaryTallY : Theme.trackSummaryY
                    width: Math.max(0, trackRow.width - Theme.trackAutomationButtonRight - Theme.spaceXs - x)
                    spacing: Theme.spaceXs
                    TrackBadge {
                        id: memberBadge
                        objectName: "trackBusBadge"
                        visible: trackRow.isGroup
                        anchors.verticalCenter: parent.verticalCenter
                        text: trackPanel.memberCountText(trackRow.modelData)
                        tone: trackRow.trackColor
                    }
                    Text {
                        objectName: "trackSummary"
                        readonly property real room: summaryLine.width
                            - (memberBadge.visible ? memberBadge.width + summaryLine.spacing : 0)
                            - (pillRow.visible ? pillRow.width + summaryLine.spacing : 0)
                        visible: !pillRow.visible || room >= Theme.trackSummaryMinWidth
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, room)
                        elide: Text.ElideRight
                        text: trackRow.isGroup ? "→ " + trackRow.modelData.mainDestinationName
                                               : trackPanel.trackSummary(trackRow.modelData)
                        color: trackPanel.dimTextColor
                        font.pixelSize: Theme.typeBody
                    }
                    Row {
                        id: pillRow
                        objectName: "trackSendPills"
                        visible: trackRow.pills.length > 0
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.spaceXs
                        Repeater {
                            model: trackRow.pills
                            delegate: TrackBadge {
                                required property var modelData
                                objectName: "trackSendPill"
                                text: modelData.label
                                tone: modelData.color
                                outlined: true
                            }
                        }
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.hairline
                    color: Theme.trackRowDivider
                }
                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    // A click selects, so the row keeps the arrow; the hand
                    // appears only while dragging the track to a new place.
                    cursorShape: dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
                    property real pressY: 0
                    // A press that moves this far drags the row: into a bus,
                    // out to the master, or to another place.
                    readonly property real dragThreshold: 8
                    property bool dragging: false
                    onPressed: function(mouse) {
                        if (mouse.button === Qt.RightButton) {
                            const point = mapToItem(trackPanel, mouse.x, mouse.y)
                            const track = trackRow.modelData
                            const index = trackRow.index
                            // Opened once the press is delivered, as the clip menus are.
                            Qt.callLater(() => trackPanel.openTrackMenu(track, index, point))
                            return
                        }
                        pressY = mapToItem(trackList.contentItem, mouse.x, mouse.y).y
                        dragging = false
                    }
                    onDoubleClicked: function(mouse) {
                        if (mouse.button === Qt.LeftButton)
                            nameEditor.begin(trackRow.modelData.name)
                    }
                    onPositionChanged: function(mouse) {
                        if (!pressed || !(pressedButtons & Qt.LeftButton))
                            return
                        const contentY = mapToItem(trackList.contentItem, mouse.x, mouse.y).y
                        trackPanel.dragPointerY = mapToItem(trackList, mouse.x, mouse.y).y
                        trackPanel.dragPointerX = mapToItem(trackPanel, mouse.x, mouse.y).x
                        if (!dragging && Math.abs(contentY - pressY) >= dragThreshold) {
                            dragging = true
                            trackPanel.beginDrag(trackRow.modelData)
                        }
                        if (dragging)
                            trackPanel.updateDrop()
                    }
                    onReleased: function(mouse) {
                        if (mouse.button === Qt.RightButton)
                            return
                        const wasDragging = dragging
                        dragging = false
                        if (wasDragging)
                            trackPanel.finishDrag(true)
                        else
                            trackPanel.selectTrack(trackRow.index, mouse.modifiers)
                    }
                    onCanceled: {
                        dragging = false
                        trackPanel.finishDrag(false)
                    }
                }
                // A/B sends show once a row is opened out; M and S sit at the
                // right, with delete on the hovered or selected row.
                Row {
                    visible: trackRow.tall
                    x: Theme.spaceLg
                    y: Theme.trackSendsY
                    spacing: Theme.spaceXs
                    Repeater {
                        model: trackRow.sends
                        delegate: SendKnob {
                            required property var modelData
                            spec: modelData
                            bridge: trackPanel.bridge
                            lineColor: trackPanel.lineColor
                            mainTextColor: trackPanel.mainTextColor
                            dimTextColor: trackPanel.dimTextColor
                            monoFamily: trackPanel.theme.monoFamily
                        }
                    }
                }
                Rectangle {
                    id: automationButton
                    objectName: "trackAutomationButton"
                    readonly property bool open: trackRow.automationOpen
                    // The track holds saved lanes (its own or its clips'):
                    // the closed button wears the track colour and a dot.
                    readonly property int laneCount: trackRow.modelData.automationLaneCount || 0
                    readonly property bool holdsAutomation: laneCount > 0
                    x: parent.width - Theme.trackAutomationButtonRight
                    y: trackRow.tall ? Theme.trackButtonTallY : (parent.height - height) / 2
                    width: Theme.trackButtonSize
                    height: Theme.trackButtonSize
                    radius: Theme.trackButtonRadius
                    color: open ? trackRow.trackColor : automationArea.containsMouse ? Theme.buttonHover : Theme.buttonFace
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: activeFocus ? trackPanel.accentColor
                        : holdsAutomation && !open ? trackRow.trackColor : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: (open ? "Hide " : "Show ") + trackRow.modelData.name + " automation"
                    Accessible.description: automationButton.laneText
                    readonly property string laneText: !holdsAutomation ? "No automation"
                        : laneCount === 1 ? "1 automation lane" : laneCount + " automation lanes"
                    Keys.onReturnPressed: trackPanel.automationToggled(trackRow.modelData.id)
                    Keys.onSpacePressed: trackPanel.automationToggled(trackRow.modelData.id)
                    Text {
                        anchors.centerIn: parent
                        text: "A"
                        color: automationButton.open ? Theme.canvas
                            : automationButton.holdsAutomation ? trackRow.trackColor : trackPanel.dimTextColor
                        font.pixelSize: Theme.typeSmall
                        font.bold: true
                    }
                    Rectangle {
                        objectName: "trackAutomationMark"
                        visible: automationButton.holdsAutomation && !automationButton.open
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.trackAutomationMarkInset
                        width: Theme.trackAutomationMarkSize
                        height: width
                        radius: width / 2
                        color: trackRow.trackColor
                    }
                    MouseArea {
                        id: automationArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        readonly property string tip: (automationButton.open ? "Hide" : "Show") + " automation · "
                            + automationButton.laneText
                        onEntered: trackPanel.showTip(automationButton, tip, "A")
                        onExited: trackPanel.hideTip(tip)
                        onClicked: trackPanel.automationToggled(trackRow.modelData.id)
                    }
                }
                Rectangle {
                    id: muteButton
                    objectName: "trackMuteButton"
                    x: parent.width - Theme.trackMuteButtonRight
                    y: trackRow.tall ? Theme.trackButtonTallY : (parent.height - height) / 2
                    width: Theme.trackButtonSize
                    height: Theme.trackButtonSize
                    radius: Theme.trackButtonRadius
                    color: trackRow.modelData.muted ? Theme.amber : muteArea.containsMouse ? Theme.buttonHover : Theme.buttonFace
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: activeFocus ? trackPanel.accentColor
                        : trackRow.modelData.muted ? Theme.amber : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: (trackRow.modelData.muted ? "Unmute " : "Mute ") + trackRow.modelData.name
                    Keys.onReturnPressed: trackPanel.bridge.trackCommands.set_track_muted(trackRow.modelData.id,
                                                                                          !trackRow.modelData.muted)
                    Keys.onSpacePressed: trackPanel.bridge.trackCommands.set_track_muted(trackRow.modelData.id,
                                                                                         !trackRow.modelData.muted)
                    Text {
                        anchors.centerIn: parent
                        text: "M"
                        color: trackRow.modelData.muted ? Theme.canvas : trackPanel.dimTextColor
                        font.pixelSize: Theme.typeSmall
                        font.bold: true
                    }
                    MouseArea {
                        id: muteArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        readonly property string tip: trackRow.modelData.muted ? "Unmute" : "Mute"
                        onEntered: trackPanel.showTip(muteButton, tip, "M")
                        onExited: trackPanel.hideTip(tip)
                        onClicked: trackPanel.bridge.trackCommands.set_track_muted(trackRow.modelData.id,
                                                                                   !trackRow.modelData.muted)
                    }
                }
                Rectangle {
                    id: soloButton
                    objectName: "trackSoloButton"
                    x: parent.width - Theme.trackSoloButtonRight
                    y: trackRow.tall ? Theme.trackButtonTallY : (parent.height - height) / 2
                    width: Theme.trackButtonSize
                    height: Theme.trackButtonSize
                    radius: Theme.trackButtonRadius
                    color: trackRow.modelData.soloed ? Theme.accent : soloArea.containsMouse ? Theme.buttonHover : Theme.buttonFace
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: activeFocus ? trackPanel.accentColor
                        : trackRow.modelData.soloed ? Theme.accent : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: (trackRow.modelData.soloed ? "Unsolo " : "Solo ") + trackRow.modelData.name
                    Keys.onReturnPressed: trackPanel.bridge.trackCommands.set_track_soloed(trackRow.modelData.id,
                                                                                           !trackRow.modelData.soloed)
                    Keys.onSpacePressed: trackPanel.bridge.trackCommands.set_track_soloed(trackRow.modelData.id,
                                                                                          !trackRow.modelData.soloed)
                    Text {
                        anchors.centerIn: parent
                        text: "S"
                        color: trackRow.modelData.soloed ? Theme.canvas : trackPanel.dimTextColor
                        font.pixelSize: Theme.typeSmall
                        font.bold: true
                    }
                    MouseArea {
                        id: soloArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        readonly property string tip: trackRow.modelData.soloed ? "Unsolo" : "Solo"
                        onEntered: trackPanel.showTip(soloButton, tip, "S")
                        onExited: trackPanel.hideTip(tip)
                        onClicked: trackPanel.bridge.trackCommands.set_track_soloed(trackRow.modelData.id,
                                                                                    !trackRow.modelData.soloed)
                    }
                }
                // Live post-fader level on a -60..0 dB scale: louder channel's
                // RMS as the fill, its peak as a tick. Empty with no reading.
                Item {
                    id: trackMeter
                    objectName: "trackMeter"
                    readonly property var reading: trackPanel.bridge.meters.channel(trackRow.modelData.id)
                    readonly property bool valid: !!reading && reading.valid
                    readonly property real rms: valid ? position(Math.max(reading.leftRms, reading.rightRms)) : 0
                    readonly property real peak: valid ? position(Math.max(reading.leftPeak, reading.rightPeak)) : 0
                    // Levels from -floorDb dB (empty) to 0 dB (full).
                    readonly property real floorDb: 60
                    function position(level) {
                        return level <= 0 ? 0 : Math.max(0, Math.min(1, (20 * Math.log10(level) + floorDb) / floorDb))
                    }
                    x: parent.width - Theme.trackMeterRight
                    y: trackRow.tall ? Theme.trackMeterTallY : (parent.height - height) / 2
                    width: Theme.trackMeterWidth
                    height: Theme.trackMeterHeight
                    Accessible.role: Accessible.Indicator
                    Accessible.name: trackRow.modelData.name + " level"
                    Accessible.description: !valid ? "No reading"
                        : (reading.clipped ? "Clipping. " : "") + "Peak "
                          + (peak <= 0 ? "silent" : (floorDb * peak - floorDb).toFixed(1) + " dB")
                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.radiusXs
                        color: Theme.trackMeterWell
                    }
                    // The gradient spans the full meter so a colour marks a level.
                    Item {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: parent.height * trackMeter.rms
                        clip: true
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: trackMeter.height
                            radius: Theme.radiusXs
                            gradient: Gradient {
                                GradientStop { position: Theme.trackMeterHotStop; color: Theme.amber }
                                GradientStop { position: 1; color: Theme.accent }
                            }
                        }
                    }
                    Rectangle {
                        visible: trackMeter.peak > 0
                        y: (parent.height - height) * (1 - trackMeter.peak)
                        width: parent.width
                        height: Theme.hairline
                        color: trackMeter.valid && trackMeter.reading.clipped === true ? Theme.trackMeterClip : Theme.textMain
                    }
                }
                Rectangle {
                    id: deleteButton
                    objectName: "trackDeleteButton"
                    visible: trackRow.selected || trackRow.hovered || activeFocus
                    x: parent.width - Theme.trackDeleteButtonRight
                    y: trackRow.tall ? Theme.trackButtonTallY : (parent.height - height) / 2
                    width: Theme.trackButtonSize
                    height: Theme.trackButtonSize
                    radius: Theme.trackButtonRadius
                    color: deleteArea.containsMouse ? Theme.trackDeleteHover : "transparent"
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    border.color: activeFocus ? trackPanel.accentColor : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Delete " + trackRow.modelData.name + " track"
                    Keys.onReturnPressed: trackPanel.bridge.trackCommands.remove_track(trackRow.modelData.id)
                    Keys.onSpacePressed: trackPanel.bridge.trackCommands.remove_track(trackRow.modelData.id)
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: deleteArea.containsMouse ? Theme.trackDeleteGlyphHover : trackPanel.dimTextColor
                        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                        font.pixelSize: Theme.trackDeleteGlyphSize
                    }
                    MouseArea {
                        id: deleteArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        readonly property string tip: "Delete track (undo restores it)"
                        onEntered: trackPanel.showTip(deleteButton, tip)
                        onExited: trackPanel.hideTip(tip)
                        onClicked: {
                            trackPanel.hideTip(tip)
                            trackPanel.bridge.trackCommands.remove_track(trackRow.modelData.id)
                        }
                    }
                }
                Rectangle {
                    objectName: "trackHeightHandle"
                    z: 4
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.trackResizeHandleHeight
                    color: resizeArea.containsMouse ? Theme.trackResizeHandleHover : "transparent"
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    MouseArea {
                        Tip {}
                        id: resizeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        cursorShape: Qt.SizeVerCursor
                        property real startY: 0
                        property real startHeight: 0
                        Accessible.role: Accessible.Button
                        Accessible.name: "Resize " + trackRow.modelData.name + " track vertically"
                        onPressed: mouse => {
                            startY = mapToItem(trackPanel, mouse.x, mouse.y).y
                            startHeight = trackRow.height
                        }
                        onPositionChanged: mouse => {
                            if (pressed)
                                trackPanel.resizePreview(trackRow.modelData.id,
                                    startHeight + mapToItem(trackPanel, mouse.x, mouse.y).y - startY)
                        }
                        onReleased: trackPanel.resizeCommitted(trackRow.modelData.id, trackRow.height)
                        onCanceled: trackPanel.resizeCanceled(trackRow.modelData.id)
                        onDoubleClicked: trackPanel.resizeCommitted(trackRow.modelData.id, Theme.trackRowHeight)
                    }
                }
                // Dragged: the row fades where it was until it lands.
                Rectangle {
                    objectName: "trackDragGhost"
                    visible: trackRow.ghosted
                    z: 8
                    anchors.fill: parent
                    color: Qt.rgba(trackPanel.panelColor.r, trackPanel.panelColor.g, trackPanel.panelColor.b,
                                   1 - Theme.trackDragGhostOpacity)
                }
                // A browser card dropped here swaps the instrument or adds
                // the effect to this track's chain.
                TrackDeviceDrop {
                    objectName: "trackRowDeviceDrop"
                    z: 5
                    anchors.fill: parent
                    bridge: trackPanel.bridge
                    track: trackRow.modelData
                    trackIndex: trackRow.index
                    accentColor: trackPanel.accentColor
                }
            }
        }
        // The header of each open automation lane: what it automates (click
        // to choose), Draw, another lane, and hide.
        Repeater {
            model: trackPanel.bridge.trackModel
            delegate: Repeater {
                id: laneHeaders
                required property var modelData
                required property int index
                readonly property color trackColor: trackPanel.theme.trackColor(modelData, index)
                readonly property var automation: trackPanel.bridge.arrangementAutomation.tracks
                    .find(entry => entry.trackId === modelData.id) || null
                model: trackPanel.rowLayout[index] ? trackPanel.rowLayout[index].lanes : []
                delegate: Rectangle {
                    id: laneHeader
                    objectName: "automationLaneHeader"
                    required property var modelData
                    required property int index
                    readonly property string trackId: laneHeaders.modelData.id
                    readonly property var lane: laneHeaders.automation
                        ? laneHeaders.automation.lanes.find(entry => entry.key === modelData.key) || null
                        : null
                    y: modelData.top
                    width: trackPanel.width
                    height: modelData.height
                    color: Theme.laneHeaderGround
                    Rectangle {
                        width: Theme.trackColorBarWidth
                        height: parent.height
                        color: laneHeaders.trackColor
                        opacity: Theme.laneHeaderColorBarOpacity
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: Theme.hairline
                        color: Theme.trackRowDivider
                    }
                    Rectangle {
                        objectName: "automationLaneChooser"
                        x: Theme.laneHeaderChooserX
                        y: Theme.spaceSm
                        width: parent.width - Theme.laneHeaderChooserRightReserve
                        height: Theme.trackButtonSize
                        radius: Theme.trackButtonRadius
                        color: chooserArea.containsMouse ? Theme.buttonFace : "transparent"
                        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                        border.color: activeFocus ? Theme.focusRing : Theme.laneHeaderChooserEdge
                        border.width: activeFocus ? Theme.focusRingWidth : Theme.hairline
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ComboBox
                        Accessible.name: "Automated parameter: " + trackPanel.targetName(laneHeader.trackId, laneHeader.modelData.key)
                        Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.buttonKeys)
                        Keys.onReturnPressed: chooser.openFor(laneHeader.trackId, laneHeader.index, laneHeader.y + Theme.laneHeaderMenuOffset)
                        Keys.onSpacePressed: chooser.openFor(laneHeader.trackId, laneHeader.index, laneHeader.y + Theme.laneHeaderMenuOffset)
                        Text {
                            x: Theme.laneHeaderChooserTextX
                            width: parent.width - Theme.laneHeaderChooserTextRightGap
                            anchors.verticalCenter: parent.verticalCenter
                            text: trackPanel.targetName(laneHeader.trackId, laneHeader.modelData.key)
                            elide: Text.ElideRight
                            color: trackPanel.mainTextColor
                            font.pixelSize: Theme.typeBody
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.spaceSm
                            anchors.verticalCenter: parent.verticalCenter
                            text: "▾"
                            color: trackPanel.dimTextColor
                            font.pixelSize: Theme.typeSmall
                        }
                        MouseArea {
                            Tip {}
                            id: chooserArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: chooser.openFor(laneHeader.trackId, laneHeader.index, laneHeader.y + Theme.laneHeaderMenuOffset)
                        }
                    }
                    Text {
                        x: Theme.laneHeaderChooserX
                        y: Theme.laneHeaderCountY
                        text: laneHeader.lane ? laneHeader.lane.points.length + (laneHeader.lane.points.length === 1 ? " point" : " points")
                                              : "No points: click the lane"
                        color: trackPanel.dimTextColor
                        font.pixelSize: Theme.typeSmall
                    }
                    Repeater {
                        model: [["✎", "Draw automation (B)", "draw"], ["+", "Add another lane", "add"], ["×", "Hide this lane", "hide"]]
                        delegate: Rectangle {
                            id: laneButton
                            required property var modelData
                            required property int index
                            readonly property bool lit: modelData[2] === "draw" && trackPanel.automationDrawMode
                            x: laneHeader.width - Theme.laneHeaderButtonsRight + index * Theme.laneHeaderButtonStride
                            y: Theme.spaceSm
                            width: Theme.trackButtonSize
                            height: Theme.trackButtonSize
                            radius: Theme.trackButtonRadius
                            color: lit ? trackPanel.accentColor : Theme.buttonFace
                            border.color: activeFocus ? trackPanel.accentColor : "transparent"
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData[1]
                            function activate() {
                                if (modelData[2] === "draw")
                                    trackPanel.automationDrawToggled()
                                else if (modelData[2] === "add")
                                    trackPanel.automationLaneAdded(laneHeader.trackId)
                                else
                                    trackPanel.automationLaneHidden(laneHeader.trackId, laneHeader.index)
                            }
                            Keys.onReturnPressed: activate()
                            Keys.onSpacePressed: activate()
                            Text {
                                anchors.centerIn: parent
                                text: laneButton.modelData[0]
                                color: laneButton.lit ? Theme.canvas : trackPanel.dimTextColor
                                font.pixelSize: laneButton.modelData[0] === "×" ? Theme.laneHeaderCloseGlyphSize : Theme.typeLabel
                            }
                            MouseArea {
                                Tip {}
                                anchors.fill: parent
                                onClicked: laneButton.activate()
                            }
                        }
                    }
                }
            }
        }
        // The parameter list a lane header opens, grouped as the track's
        // mixer, sends, engine and plug-ins.
        Rectangle {
            id: chooser
            transformOrigin: Item.Top
            Reveal { target: chooser; fromScale: Motion.popScale; duration: Motion.fast }
            objectName: "automationTargetChooser"
            property string trackId: ""
            property int laneIndex: -1
            readonly property var targets: {
                const entry = trackPanel.bridge.arrangementAutomation.tracks.find(track => track.trackId === trackId)
                if (!entry)
                    return []
                const rows = laneIndex < 0
                    ? [{ heading: false, action: true, name: "+ Show another lane below", key: "" }] : []
                for (const group of entry.targets) {
                    rows.push({ heading: true, action: false, name: group.group, key: "" })
                    for (const item of group.items)
                        rows.push({ heading: false, action: false, name: item.name, key: item.key })
                }
                return rows
            }
            // The keyboard's row: Up and Down step over the group headings,
            // Return or Space chooses, Escape closes; focus goes back to the
            // chooser that opened the list.
            property int current: -1
            function chooseable(index) { return index >= 0 && index < targets.length && !targets[index].heading }
            function step(direction) {
                for (let index = current + direction; index >= 0 && index < targets.length; index += direction) {
                    if (chooseable(index)) {
                        current = index
                        targetList.positionViewAtIndex(index, ListView.Contain)
                        return
                    }
                }
            }
            function choose(index) {
                if (!chooseable(index))
                    return
                const row = targets[index]
                visible = false
                if (row.action)
                    trackPanel.automationLaneAdded(trackId)
                else
                    trackPanel.automationLaneChosen(trackId, laneIndex, row.key)
            }
            function openFor(trackId, laneIndex, top) {
                trackPanel.rememberOpener()
                chooser.trackId = trackId
                chooser.laneIndex = laneIndex
                current = -1
                step(1)
                y = top
                visible = true
                forceActiveFocus()
            }
            Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.popupKeys)
            Keys.onUpPressed: step(-1)
            Keys.onDownPressed: step(1)
            Keys.onReturnPressed: {
                const index = current
                trackPanel.restoreOpener()
                choose(index)
            }
            Keys.onSpacePressed: {
                const index = current
                trackPanel.restoreOpener()
                choose(index)
            }
            visible: false
            x: Theme.spaceLg
            z: 8
            width: trackPanel.width - Theme.space2xl
            height: Math.min(Theme.trackChooserMaxHeight, targetList.contentHeight + Theme.spaceMd)
            radius: Theme.radiusLg
            color: Theme.panelRaised
            border.color: Theme.controlEdge
            Keys.onEscapePressed: {
                visible = false
                trackPanel.restoreOpener()
            }
            onActiveFocusChanged: if (!activeFocus && !targetList.activeFocus) visible = false
            ListView {
                id: targetList
                anchors.fill: parent
                anchors.margins: Theme.spaceXs
                clip: true
                model: chooser.targets
                delegate: Rectangle {
                    id: targetRow
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: modelData.heading ? Theme.trackChooserHeadingHeight : Theme.trackChooserRowHeight
                    radius: Theme.radiusMd
                    color: !modelData.heading && targetArea.containsMouse ? Theme.trackChooserRowHover : "transparent"
                    border.color: chooser.current === index && chooser.activeFocus ? Theme.focusRing : "transparent"
                    border.width: Theme.focusRingWidth
                    Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                    Text {
                        x: targetRow.modelData.heading ? Theme.spaceXs : Theme.spaceLg
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - x - Theme.spaceXs
                        elide: Text.ElideRight
                        text: targetRow.modelData.name
                        color: targetRow.modelData.heading ? trackPanel.dimTextColor
                            : targetRow.modelData.action ? trackPanel.accentColor : trackPanel.mainTextColor
                        font.pixelSize: targetRow.modelData.heading ? Theme.typeCaption : Theme.typeBody
                        font.capitalization: targetRow.modelData.heading ? Font.AllUppercase : Font.MixedCase
                        font.letterSpacing: targetRow.modelData.heading ? Theme.capsLetterSpacing : 0
                    }
                    MouseArea {
                        id: targetArea
                        anchors.fill: parent
                        enabled: !targetRow.modelData.heading
                        hoverEnabled: true
                        onClicked: chooser.choose(targetRow.index)
                    }
                }
            }
        }
        // The returns' section, under the tracks.
        Item {
            objectName: "trackReturnsLabel"
            visible: trackPanel.trackLayout.returnsTop >= 0
            y: Math.max(0, trackPanel.trackLayout.returnsTop)
            width: trackPanel.width
            height: Theme.trackReturnsLabelHeight
            Rectangle {
                width: parent.width
                height: Theme.hairline
                color: Theme.lineStrong
            }
            Text {
                x: Theme.trackSummaryX
                anchors.verticalCenter: parent.verticalCenter
                text: "RETURNS"
                color: trackPanel.dimTextColor
                font.pixelSize: Theme.typeCaption
                font.bold: true
                font.letterSpacing: Theme.capsLetterSpacing
            }
        }
        // The slot a dragged header would drop into.
        TrackDropSlot {
            objectName: "trackDropSlot"
            readonly property var drop: trackPanel.trackDrop
            visible: trackPanel.trackLayout.gapTop >= 0 && drop !== null
            y: Math.max(0, trackPanel.trackLayout.gapTop)
            width: trackPanel.width
            height: Theme.trackDropSlotHeight
            nested: drop !== null && drop.busId !== ""
            tone: nested ? trackPanel.busColorOf(drop.busId) : trackPanel.accentColor
            text: nested ? "Drop here to route into " + drop.busName : "Drop here to route to the master"
        }
    }
    // The dragged header's chip: what moves and where it would land.
    TrackDragChip {
        objectName: "trackDragChip"
        readonly property var drop: trackPanel.trackDrop
        readonly property var lead: trackPanel.trackWithId(trackPanel.dragLeadId)
        visible: trackPanel.dragging && drop !== null
        z: 50
        x: Math.max(Theme.spaceXs, Math.min(trackPanel.width - width - Theme.spaceXs,
                                            trackPanel.dragPointerX + Theme.trackDragChipOffset))
        y: trackList.y + trackPanel.dragPointerY + Theme.trackDragChipOffset
        maxWidth: trackPanel.width - 2 * Theme.spaceXs
        swatch: lead ? trackPanel.theme.trackColor(lead, trackPanel.bridge.tracks.indexOf(lead)) : trackPanel.accentColor
        subject: trackPanel.dragIds.length > 1 ? trackPanel.dragIds.length + " tracks" : lead ? lead.name : ""
        destination: drop === null ? "" : drop.refused ? "Buses stay on the master"
            : drop.busId !== "" ? trackPanel.busLabel(drop.busName) : "Master"
        tone: drop === null ? trackPanel.mainTextColor : drop.refused ? Theme.amber
            : drop.busId !== "" ? trackPanel.busColorOf(drop.busId) : trackPanel.mainTextColor
        textColor: trackPanel.mainTextColor
    }
    // An instrument dropped under the last track makes a new one.
    DeviceTrackDrop {
        objectName: "trackListEmptyDrop"
        x: 0
        y: trackList.y + Math.min(trackList.height,
            Math.max(0, trackPanel.rowsContentHeight - trackList.contentY))
        width: parent.width
        height: trackList.y + trackList.height - y
        bridge: trackPanel.bridge
        accentColor: trackPanel.accentColor
    }
    Text {
        visible: trackPanel.bridge.tracks.length === 0
        x: Theme.spaceXl
        y: trackCreationRow.height + Theme.space2xl
        width: parent.width - 2 * Theme.spaceXl
        text: "No tracks yet. Double-click an instrument in the browser, or use + Track."
        wrapMode: Text.WordWrap
        color: trackPanel.dimTextColor
        font.pixelSize: Theme.typeLabel
        lineHeight: Theme.emptyTrackListLineHeight
    }
    Rectangle {
        id: masterTrackRow
        objectName: "masterTrackRow"
        z: 3
        anchors.bottom: parent.bottom
        anchors.bottomMargin: trackPanel.pianoRollHeight + trackPanel.elapsedRulerHeight
        width: parent.width
        height: Theme.masterRowHeight
        color: trackPanel.selectedTrack === -2 ? trackPanel.theme.selection : Theme.trackMasterGround
        Rectangle { width: parent.width; height: Theme.hairline; color: trackPanel.lineColor }
        Rectangle {
            width: Theme.trackColorBarWidth
            height: parent.height
            color: trackPanel.accentColor
        }
        Text {
            x: Theme.trackSummaryX
            y: Theme.trackNameY
            text: "Master"
            color: trackPanel.mainTextColor
            font.pixelSize: Theme.typeEmphasis
            font.weight: Font.DemiBold
        }
        Text {
            x: Theme.trackSummaryX
            y: Theme.trackSummaryY
            text: "Output to speakers"
            color: trackPanel.dimTextColor
            font.pixelSize: Theme.typeBody
        }
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: "Master bus track"
        Keys.onReturnPressed: trackPanel.trackSelected(-2)
        Keys.onSpacePressed: trackPanel.trackSelected(-2)
        MouseArea { anchors.fill: parent; onClicked: trackPanel.trackSelected(-2) }
    }
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: trackPanel.pianoRollHeight
        width: parent.width
        visible: trackPanel.elapsedRulerHeight > 0
        height: trackPanel.elapsedRulerHeight
        color: trackPanel.panelColor
        border.color: trackPanel.lineColor
        Text {
            x: Theme.spaceXl
            anchors.verticalCenter: parent.verticalCenter
            text: "TIME"
            color: trackPanel.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.capsLetterSpacing
        }
    }

    // Outside the menu, palette or duck list, a click closes them (and goes no further).
    MouseArea {
        objectName: "trackPopupDismiss"
        anchors.fill: parent
        z: 40
        visible: trackMenu.visible || colourPopover.visible || duckPopover.visible
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: {
            trackMenu.visible = false
            colourPopover.close()
            duckPopover.close()
        }
    }

    // A track's actions, on right-click.
    Rectangle {
        id: trackMenu
        transformOrigin: Item.TopLeft
        Reveal { target: trackMenu; fromScale: Motion.popScale; duration: Motion.fast }
        objectName: "trackContextMenu"
        property var track: null
        property var trackIds: []
        property int current: 0
        // The tracks the menu acts on: the selection, or the one clicked.
        readonly property var chosen: trackPanel.bridge.tracks.filter(t => trackIds.includes(t.id))
        readonly property bool several: chosen.length > 1
        readonly property var routable: chosen.filter(t => !trackPanel.isBus(t))
        // Send to bus: a new group bus, each existing one, or back to the master.
        readonly property var busEntries: routable.length === 0 ? [] : [
            { name: "Send to new bus", hint: "A group bus just above " + (routable.length > 1 ? "them" : "it"),
              action: "newBus" }
        ].concat(trackPanel.bridge.tracks
                 .filter(t => t.kind === "group" && !trackIds.includes(t.id)
                         && routable.some(r => r.mainDestinationId !== t.id))
                 .map(t => ({ name: "Send to " + t.name, hint: "Add " + (routable.length > 1 ? "them" : "it")
                              + " to this bus", action: "bus:" + t.id })))
          .concat(routable.some(t => t.mainDestinationId !== "")
                  ? [{ name: "Back to master", hint: "Route straight out again", action: "master" }] : [])
        readonly property var entries: !track ? [] : several ? busEntries : busEntries.concat([
            { name: "Rename", hint: "Or double-click the track", action: "rename" },
            { name: "Colour…", hint: "Pick from the palette", action: "colour" },
            { name: (trackPanel.automationView[track.id] || {}).open === true ? "Hide automation" : "Show automation",
              hint: "Automate over the clips (A)", action: "automation" },
            track.kickLike === true
                ? { name: "Duck everything to this…", hint: "A kick: Pump the rest, keyed from here", action: "duck" }
                : { name: "Duck tracks to this…", hint: "Pump them, keyed from here", action: "duck" },
            { name: "Duplicate", hint: "Clips, devices and settings", action: "duplicate" }
        ].concat(track.kickLockOffered === true ? [
            track.kickLocked === true
                ? { name: "Unlock kick & bass", hint: "Anvil and Nova play on their own again", action: "unlockKick" }
                : { name: "Lock kick & bass", hint: "Anvil tunes to this bass; it ducks under the kick", action: "lockKick" }
        ] : []).concat(track.kind === "audio" || track.kind === "instrument" ? [
            track.frozen === true
                ? { name: "Unfreeze", hint: "Play the devices again", action: "unfreeze" }
                : { name: "Freeze", hint: "Play a render; saves CPU", action: "freeze" },
            { name: "Bounce to audio track", hint: "A new track; this one muted", action: "bounce" }
        ].concat(trackPanel.bridge.loopTransport && trackPanel.bridge.loopTransport.enabled ? [
            { name: "Bounce the loop", hint: "Bars " + trackPanel.bridge.loopTransport.startBar + "–"
                  + trackPanel.bridge.loopTransport.endBar + " to a new audio track", action: "bounceLoop" }
        ] : []).concat([
            { name: "Flatten", hint: "Replace the devices with audio", action: "flatten" }
        ]) : []).concat([
            { name: "Delete", hint: "Undo restores it", action: "delete" }
        ]))
        function activate(action) {
            const chosen = track
            const ids = routable.map(t => t.id)
            visible = false
            if (!chosen)
                return
            if (action === "newBus")
                trackPanel.bridge.trackCommands.route_to_bus(ids, "")
            else if (action.startsWith("bus:"))
                trackPanel.bridge.trackCommands.route_to_bus(ids, action.slice(4))
            else if (action === "master")
                trackPanel.bridge.trackCommands.route_to_master(ids)
            else if (action === "rename")
                trackPanel.renamingTrackId = chosen.id
            else if (action === "colour")
                trackPanel.openColours(chosen, Qt.point(x, y))
            else if (action === "automation")
                trackPanel.automationToggled(chosen.id)
            else if (action === "duck")
                trackPanel.openDuck(chosen, Qt.point(x, y))
            else if (action === "duplicate")
                trackPanel.bridge.trackCommands.duplicate_track(chosen.id)
            else if (action === "lockKick" || action === "unlockKick")
                trackPanel.bridge.trackCommands.lock_kick_and_bass(chosen.id, action === "lockKick")
            else if (action === "freeze")
                trackPanel.bridge.freezeCommands.freeze(chosen.id)
            else if (action === "unfreeze")
                trackPanel.bridge.freezeCommands.unfreeze(chosen.id)
            else if (action === "bounce")
                trackPanel.bridge.freezeCommands.bounce(chosen.id)
            else if (action === "flatten")
                trackPanel.bridge.freezeCommands.flatten(chosen.id)
            else if (action === "bounceLoop")
                trackPanel.bridge.freezeCommands.bounce_bars(chosen.id,
                    trackPanel.bridge.loopTransport.startBar, trackPanel.bridge.loopTransport.endBar)
            else if (action === "delete")
                trackPanel.bridge.trackCommands.remove_track(chosen.id)
        }
        visible: false
        z: 41
        width: Theme.trackMenuWidth
        height: Theme.trackPopoverTitleHeight + entries.length * Theme.trackMenuEntryStride + Theme.spaceSm
        radius: Theme.radiusXl
        color: trackPanel.raisedColor
        border.color: trackPanel.lineColor
        Accessible.role: Accessible.PopupMenu
        Accessible.name: track ? track.name + " track actions" : "Track actions"
        onVisibleChanged: current = 0
        Keys.onEscapePressed: visible = false
        // An empty menu (every chosen track a bus) has nothing to move to or run.
        Keys.onUpPressed: if (entries.length > 0) current = (current + entries.length - 1) % entries.length
        Keys.onDownPressed: if (entries.length > 0) current = (current + 1) % entries.length
        Keys.onReturnPressed: if (entries.length > 0) activate(entries[current].action)
        Keys.onSpacePressed: if (entries.length > 0) activate(entries[current].action)
        onActiveFocusChanged: if (!activeFocus) visible = false
        Text {
            x: Theme.spaceLg
            y: Theme.trackPopoverTitleY
            width: parent.width - Theme.space2xl
            elide: Text.ElideRight
            text: trackMenu.several ? trackMenu.chosen.length + " TRACKS"
                : trackMenu.track ? "TRACK · " + String(trackMenu.track.name).toUpperCase() : ""
            color: trackPanel.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.capsLetterSpacing
        }
        Repeater {
            model: trackMenu.entries
            delegate: Rectangle {
                id: menuEntry
                required property var modelData
                required property int index
                x: Theme.spaceSm
                y: Theme.trackPopoverTitleHeight + index * Theme.trackMenuEntryStride
                width: trackMenu.width - Theme.spaceLg
                height: Theme.trackMenuEntryHeight
                radius: Theme.trackMenuEntryRadius
                color: entryArea.containsMouse || trackMenu.current === index ? Theme.menuHover : "transparent"
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                Text {
                    x: Theme.trackMenuEntryTextX
                    y: Theme.trackMenuEntryTextY
                    text: menuEntry.modelData.name
                    color: menuEntry.modelData.action === "delete" ? Theme.trackMenuDeleteText : trackPanel.mainTextColor
                    font.pixelSize: Theme.typeBody
                    font.bold: true
                }
                Text {
                    x: Theme.trackMenuEntryTextX
                    y: Theme.trackMenuEntryHintY
                    text: menuEntry.modelData.hint
                    color: trackPanel.dimTextColor
                    font.pixelSize: Theme.typeCaption
                }
                MouseArea {
                    id: entryArea
                    objectName: "trackMenuAction:" + menuEntry.modelData.action
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    Accessible.role: Accessible.MenuItem
                    Accessible.name: menuEntry.modelData.name
                    onEntered: trackMenu.current = menuEntry.index
                    onClicked: trackMenu.activate(menuEntry.modelData.action)
                }
            }
        }
    }

    // Duck tracks to a key: tick the tracks, and each gets a Pump keyed from
    // it, all linked to one shape (one undo step).
    Rectangle {
        id: duckPopover
        transformOrigin: Item.TopLeft
        Reveal { target: duckPopover; fromScale: Motion.popScale; duration: Motion.fast }
        objectName: "trackDuckPopover"
        property var track: null
        property var chosen: ({})
        readonly property var others: track
            ? trackPanel.bridge.tracks.filter(other => other.id !== track.id) : []
        readonly property int count: Object.keys(chosen).filter(id => chosen[id]).length
        function open(key, point) {
            track = key
            const all = {}
            for (const other of trackPanel.bridge.tracks)
                if (other.id !== key.id && other.kind !== "return")
                    all[other.id] = true
            chosen = all
            current = 0
            x = Math.max(Theme.spaceXs, Math.min(trackPanel.width - width - Theme.spaceXs, point.x))
            y = Math.max(Theme.spaceXs, Math.min(trackPanel.height - height - Theme.spaceXs, point.y))
            visible = true
            forceActiveFocus()
        }
        function close() { visible = false }
        // The keyboard's row: Up and Down move, Space ticks or unticks it,
        // Return ducks the ticked tracks, Escape closes; focus goes back to
        // the control that opened the list.
        property int current: 0
        function step(direction) {
            if (others.length === 0)
                return
            current = Math.max(0, Math.min(others.length - 1, current + direction))
            const rowTop = current * Theme.duckRowHeight
            if (rowTop < duckList.contentY)
                duckList.contentY = rowTop
            else if (rowTop + Theme.duckRowHeight > duckList.contentY + duckList.height)
                duckList.contentY = rowTop + Theme.duckRowHeight - duckList.height
        }
        Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.popupKeys)
        Keys.onUpPressed: step(-1)
        Keys.onDownPressed: step(1)
        Keys.onSpacePressed: if (current < others.length) toggle(others[current].id)
        function toggle(id) {
            const next = Object.assign({}, chosen)
            next[id] = !next[id]
            chosen = next
        }
        function duck() {
            const ids = Object.keys(chosen).filter(id => chosen[id])
            const key = track
            close()
            if (key && ids.length > 0)
                trackPanel.bridge.routeCommands.duck_tracks_to(key.id, ids)
        }
        visible: false
        z: 41
        width: Theme.duckPopoverWidth
        height: Theme.trackPopoverTitleHeight + Math.min(others.length, Theme.duckPopoverMaxRows) * Theme.duckRowHeight + Theme.duckFooterHeight
        radius: Theme.radiusXl
        color: trackPanel.raisedColor
        border.color: trackPanel.lineColor
        Accessible.role: Accessible.Dialog
        Accessible.name: track ? "Duck tracks to " + track.name : "Duck tracks"
        Keys.onEscapePressed: {
            close()
            trackPanel.restoreOpener()
        }
        Keys.onReturnPressed: {
            trackPanel.restoreOpener()
            duck()
        }
        onActiveFocusChanged: if (!activeFocus) close()
        Text {
            x: Theme.spaceLg
            y: Theme.trackPopoverTitleY
            width: parent.width - Theme.space2xl
            elide: Text.ElideRight
            text: duckPopover.track ? "DUCK TO · " + String(duckPopover.track.name).toUpperCase() : ""
            color: trackPanel.dimTextColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.capsLetterSpacing
        }
        Flickable {
            id: duckList
            x: Theme.spaceSm
            y: Theme.trackPopoverTitleHeight
            width: parent.width - Theme.spaceLg
            height: Math.min(duckPopover.others.length, Theme.duckPopoverMaxRows) * Theme.duckRowHeight
            contentHeight: duckPopover.others.length * Theme.duckRowHeight
            clip: true
            Column {
                Repeater {
                    model: duckPopover.others
                    delegate: Rectangle {
                        id: duckRow
                        required property var modelData
                        required property int index
                        readonly property bool ticked: duckPopover.chosen[modelData.id] === true
                        width: duckPopover.width - Theme.spaceLg
                        height: Theme.duckRowHeight
                        radius: Theme.trackMenuEntryRadius
                        color: duckRowArea.containsMouse ? Theme.menuHover : "transparent"
                        border.color: duckPopover.current === index && duckPopover.activeFocus ? Theme.focusRing : "transparent"
                        border.width: Theme.focusRingWidth
                        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                        Rectangle {
                            x: Theme.duckCheckX
                            anchors.verticalCenter: parent.verticalCenter
                            width: Theme.duckCheckSize
                            height: Theme.duckCheckSize
                            radius: Theme.radiusSm
                            color: duckRow.ticked ? trackPanel.accentColor : "transparent"
                            border.color: duckRow.ticked ? trackPanel.accentColor : trackPanel.dimTextColor
                        }
                        Text {
                            x: Theme.duckRowTextX
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - Theme.duckRowTextX - Theme.spaceMd
                            elide: Text.ElideRight
                            text: duckRow.modelData.name
                            color: trackPanel.mainTextColor
                            font.pixelSize: Theme.typeBody
                        }
                        MouseArea {
                            id: duckRowArea
                            objectName: "trackDuckChoice:" + duckRow.modelData.id
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            Accessible.role: Accessible.CheckBox
                            Accessible.name: duckRow.modelData.name
                            Accessible.checked: duckRow.ticked
                            onClicked: duckPopover.toggle(duckRow.modelData.id)
                        }
                    }
                }
            }
        }
        Rectangle {
            x: Theme.spaceSm
            y: parent.height - Theme.duckApplyBottom
            width: parent.width - Theme.spaceLg
            height: Theme.duckApplyHeight
            radius: Theme.trackMenuEntryRadius
            color: duckButtonArea.containsMouse && duckPopover.count > 0 ? Theme.menuHover : Theme.duckApplyIdle
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            opacity: duckPopover.count > 0 ? 1 : Theme.duckApplyDisabledOpacity
            Text {
                anchors.centerIn: parent
                text: duckPopover.count === 1 ? "Duck 1 track" : "Duck " + duckPopover.count + " tracks"
                color: trackPanel.mainTextColor
                font.pixelSize: Theme.typeBody
                font.bold: true
            }
            MouseArea {
                Tip {}
                id: duckButtonArea
                objectName: "trackDuckApply"
                anchors.fill: parent
                hoverEnabled: true
                enabled: duckPopover.count > 0
                cursorShape: Qt.PointingHandCursor
                Accessible.role: Accessible.Button
                Accessible.name: "Duck the ticked tracks"
                onClicked: duckPopover.duck()
            }
        }
    }

    // The track palette: click a colour to set it.
    Rectangle {
        id: colourPopover
        transformOrigin: Item.TopLeft
        Reveal { target: colourPopover; fromScale: Motion.popScale; duration: Motion.fast }
        objectName: "trackColourPopover"
        property var track: null
        function close() { visible = false }
        // The keyboard's colour: the arrows move round the palette, Return
        // or Space sets it, Escape closes; focus goes back to the swatch.
        property int current: 0
        readonly property int colourCount: trackPanel.theme.trackPalette.length
        function move(offset) {
            if (colourCount > 0)
                current = Math.max(0, Math.min(colourCount - 1, current + offset))
        }
        function choose(index) {
            const track = colourPopover.track
            close()
            if (track && index >= 0 && index < colourCount)
                trackPanel.bridge.set_track_color(track.id, trackPanel.theme.trackPalette[index])
        }
        Keys.onShortcutOverride: event => trackPanel.claimKeys(event, trackPanel.popupKeys)
        Keys.onLeftPressed: move(-1)
        Keys.onRightPressed: move(1)
        Keys.onUpPressed: move(-Theme.paletteColumns)
        Keys.onDownPressed: move(Theme.paletteColumns)
        Keys.onReturnPressed: {
            const index = current
            trackPanel.restoreOpener()
            choose(index)
        }
        Keys.onSpacePressed: {
            const index = current
            trackPanel.restoreOpener()
            choose(index)
        }
        visible: false
        z: 41
        width: Theme.paletteColumns * (Theme.paletteSwatchSize + Theme.spaceXs) + 2 * Theme.spaceMd
        height: Math.ceil(trackPanel.theme.trackPalette.length / Theme.paletteColumns) * (Theme.paletteSwatchSize + Theme.spaceXs) + 2 * Theme.spaceMd
        radius: Theme.radiusXl
        color: trackPanel.raisedColor
        border.color: trackPanel.lineColor
        Accessible.role: Accessible.Dialog
        Accessible.name: "Track colour"
        Keys.onEscapePressed: {
            close()
            trackPanel.restoreOpener()
        }
        onActiveFocusChanged: if (!activeFocus) close()
        Grid {
            x: Theme.spaceMd
            y: Theme.spaceMd
            columns: Theme.paletteColumns
            spacing: Theme.spaceXs
            Repeater {
                model: trackPanel.theme.trackPalette
                delegate: Rectangle {
                    id: paletteSwatch
                    required property string modelData
                    required property int index
                    readonly property bool chosen: !!colourPopover.track
                        && String(colourPopover.track.color).toLowerCase() === modelData.toLowerCase()
                    readonly property bool keyed: colourPopover.current === index && colourPopover.activeFocus
                    objectName: "trackPaletteSwatch:" + index
                    width: Theme.paletteSwatchSize
                    height: Theme.paletteSwatchSize
                    radius: Theme.trackButtonRadius
                    color: modelData
                    border.width: chosen || paletteArea.containsMouse ? Theme.strokeBold : 0
                    border.color: trackPanel.mainTextColor
                    // The keyboard's colour, ringed just outside the swatch.
                    FocusRing {
                        targetRadius: paletteSwatch.radius
                        visible: paletteSwatch.keyed
                    }
                    Accessible.role: Accessible.Button
                    Accessible.name: "Colour " + modelData
                    MouseArea {
                        Tip {}
                        id: paletteArea
                        objectName: "trackPaletteColour:" + paletteSwatch.modelData
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: colourPopover.choose(paletteSwatch.index)
                    }
                }
            }
        }
    }
}
