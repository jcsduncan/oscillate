pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: workspace
    // The instrument track (and bar) under a dropped MIDI file, or null.
    function instrumentDropTarget(item, x, y) {
        return arrangement.instrumentDropTarget(item, x, y) || trackPanel.instrumentDropTarget(item, x, y)
    }
    required property var bridge
    required property int selectedTrack
    required property string selectedClipId
    required property real visibleBars
    required property real viewStartBar
    required property bool snapToBeat
    required property color canvasColor
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    required property color accentColor
    required property color amberColor
    // The shell's kit theme (devicekit/ShellTheme): track colours and type.
    required property var theme

    readonly property int trackWidth: Theme.trackListWidth
    readonly property int creationHeight: 0
    // Elapsed time under the arrangement, as the bar ruler above it.
    readonly property int elapsedRulerHeight: Theme.elapsedRulerHeight
    property real trackScrollY: 0
    property var trackResizePreview: ({})
    // Track id → {open, overlay, keys}: with automation open, the track's
    // clip row shows `overlay`'s envelope over its clips for editing, and
    // `keys` are extra lanes beneath it. A track opened for the first time
    // overlays the parameter it already automates, or Level.
    property var automationView: ({})
    // B, or the pencil on a lane header: drag paints the envelope.
    property bool automationDrawMode: false
    // The tempo lane under the ruler (ADR-0112): a thin line, or expanded
    // for drawing ramps. View state, not saved with the project.
    property bool tempoLaneExpanded: false
    readonly property int tempoLaneHeight: tempoLaneExpanded ? Theme.tempoLaneExpandedHeight
                                                             : Theme.tempoLaneCollapsedHeight
    readonly property int automationLaneHeight: Theme.automationLaneHeight
    // While a track header is dragged (TrackListPanel): the tracks moving
    // (`ids`), where they would land (`busId`, empty for the master, and
    // `beforeId`, empty for the bracket's or list's end) and whether the
    // drop is refused; null otherwise. The lists open a slot there.
    property var trackDrop: null
    // The rows top down in the list's order (TrackRoutingView::Nesting):
    // each group bus with its members under it, the returns in a section of
    // their own at the bottom. Per track, by saved index: the clip row
    // ({top, height}), its open automation lanes ([{key, top, height}]),
    // where the next row starts (bottom) and whether a folded bus hides it.
    // `drop` opens the drop slot (gapTop) where it would land.
    function layoutRows(drop) {
        const tracks = bridge.tracks
        const saved = bridge.trackRowHeights
        const preview = trackResizePreview
        const view = automationView
        const collapsed = bridge.collapsedBuses
        const order = tracks.map((track, index) => index)
        order.sort((a, b) => tracks[a].displayIndex - tracks[b].displayIndex)
        const rows = new Array(tracks.length)
        let top = 0
        let returnsTop = -1
        let gapTop = -1
        function openGap() {
            if (drop && gapTop < 0) {
                gapTop = top
                top += Theme.trackDropSlotHeight
            }
        }
        for (let at = 0; at < order.length; ++at) {
            const index = order[at]
            const track = tracks[index]
            if (track.kind === "return" && returnsTop < 0) {
                // A drop at the end of the tracks lands above the returns.
                if (drop && drop.beforeId === "" && drop.busId === "")
                    openGap()
                returnsTop = top
                top += Theme.trackReturnsLabelHeight
            }
            if (drop && drop.beforeId === track.id)
                openGap()
            if (track.busId !== "" && collapsed.includes(track.busId)) {
                rows[index] = { top: top, height: 0, lanes: [], bottom: top, hidden: true }
            } else {
                const height = preview[track.id] ?? saved[track.id] ?? Theme.trackRowHeight
                const shown = view[track.id]
                const lanes = []
                let laneTop = top + height
                if (shown && shown.open) {
                    for (const key of shown.keys) {
                        lanes.push({ key: key, top: laneTop, height: automationLaneHeight })
                        laneTop += automationLaneHeight
                    }
                }
                rows[index] = { top: top, height: height, lanes: lanes, bottom: laneTop, hidden: false }
                top = laneTop
            }
            // A drop at a bracket's end lands after its last member.
            const next = at + 1 < order.length ? tracks[order[at + 1]] : null
            if (drop && drop.busId !== "" && drop.beforeId === ""
                    && (track.id === drop.busId || track.busId === drop.busId)
                    && (!next || next.busId !== drop.busId))
                openGap()
        }
        if (drop && drop.busId === "" && drop.beforeId === "")
            openGap()
        return { rows: rows, returnsTop: returnsTop, gapTop: gapTop, height: top }
    }
    // Without a slot: where the dragged header's pointer finds its target.
    readonly property var baseTrackLayout: layoutRows(null)
    readonly property var trackLayout: trackDrop && !trackDrop.refused ? layoutRows(trackDrop) : baseTrackLayout
    readonly property var trackRowLayout: trackLayout.rows
    readonly property real trackContentHeight: trackLayout.height

    function automationTrack(trackId) {
        const tracks = bridge.arrangementAutomation.tracks
        return tracks.find(track => track.trackId === trackId) || null
    }
    function setAutomationView(trackId, view) {
        const next = Object.assign({}, automationView)
        next[trackId] = view
        automationView = next
    }
    function toggleAutomation(trackId) {
        const shown = automationView[trackId]
        if (shown) {
            setAutomationView(trackId, Object.assign({}, shown, { open: !shown.open }))
            return
        }
        const automation = automationTrack(trackId)
        const existing = automation ? automation.lanes.map(lane => lane.key) : []
        setAutomationView(trackId, { open: true, overlay: existing.length > 0 ? existing[0] : "level",
                                     keys: existing.slice(1) })
    }
    // Shows another parameter in a lane under the track.
    function addAutomationLane(trackId) {
        const shown = automationView[trackId] || { open: true, overlay: "level", keys: [] }
        const automation = automationTrack(trackId)
        const candidates = automation
            ? automation.targets.flatMap(group => group.items.map(item => item.key)) : []
        const key = candidates.find(candidate => candidate !== shown.overlay
                                    && shown.keys.indexOf(candidate) < 0)
        if (key !== undefined)
            setAutomationView(trackId, { open: true, overlay: shown.overlay, keys: shown.keys.concat([key]) })
    }
    // Index -1 is the overlay on the clip row.
    function setAutomationLaneKey(trackId, index, key) {
        const shown = automationView[trackId]
        if (!shown)
            return
        if (index < 0) {
            setAutomationView(trackId, { open: true, overlay: key,
                                         keys: shown.keys.filter(existing => existing !== key) })
            return
        }
        if (key === shown.overlay || shown.keys.indexOf(key) >= 0)
            return
        const keys = shown.keys.slice()
        keys[index] = key
        setAutomationView(trackId, { open: true, overlay: shown.overlay, keys: keys })
    }
    // Opens a track's automation with `key` over its clips (a device
    // editor's Automate action), keeping its other lanes below.
    function showAutomation(trackId, key) {
        const shown = automationView[trackId] || { open: true, overlay: key, keys: [] }
        setAutomationView(trackId, { open: true, overlay: key,
                                     keys: shown.keys.filter(existing => existing !== key) })
    }
    Connections {
        target: workspace.bridge.arrangementAutomation
        function onRevealRequested(trackId, key) { workspace.showAutomation(trackId, key) }
    }
    function hideAutomationLane(trackId, index) {
        const shown = automationView[trackId]
        if (!shown)
            return
        const keys = shown.keys.slice()
        keys.splice(index, 1)
        setAutomationView(trackId, Object.assign({}, shown, { keys: keys }))
    }

    signal trackSelected(int index)
    signal clipSelected(string clipId)
    signal clipsSelected(var clipIds)
    signal clipOpened(string clipId)
    signal zoomInRequested()
    signal zoomOutRequested()
    signal panRequested(int direction)
    signal zoomAtRequested(real factor, real bar)
    signal scrollRequested(real bars)
    signal snapToggled()
    signal rowHeightCommitted(string trackId, int height)
    signal addInstrumentRequested()
    signal importAudioRequested()
    signal importMidiRequested()
    signal addGroupRequested()
    signal addReturnRequested()

    // Ctrl+A in the arrangement: every clip on every track.
    function selectAllClips() { arrangement.selectAllClips() }
    function previewTrackHeight(trackId, height) {
        const next = Object.assign({}, trackResizePreview)
        next[trackId] = Math.max(Theme.trackRowMinHeight, Math.min(Theme.trackRowMaxHeight, Math.round(height)))
        trackResizePreview = next
    }
    function finishTrackHeight(trackId, height, commit) {
        if (commit)
            rowHeightCommitted(trackId, Math.max(Theme.trackRowMinHeight, Math.min(Theme.trackRowMaxHeight, Math.round(height))))
        const next = Object.assign({}, trackResizePreview)
        delete next[trackId]
        trackResizePreview = next
    }

    TrackListPanel {
        id: trackPanel
        anchors.left: parent.left
        width: workspace.trackWidth
        height: parent.height
        bridge: workspace.bridge
        selectedTrack: workspace.selectedTrack
        pianoRollHeight: arrangement.pianoRollHeight
        creationHeight: workspace.creationHeight
        elapsedRulerHeight: workspace.elapsedRulerHeight
        rowLayout: workspace.trackRowLayout
        rowsContentHeight: workspace.trackContentHeight
        trackLayout: workspace.trackLayout
        baseTrackLayout: workspace.baseTrackLayout
        trackDrop: workspace.trackDrop
        automationView: workspace.automationView
        automationDrawMode: workspace.automationDrawMode
        tempoLaneHeight: workspace.tempoLaneHeight
        tempoLaneExpanded: workspace.tempoLaneExpanded
        panelColor: workspace.panelColor
        raisedColor: workspace.raisedColor
        lineColor: workspace.lineColor
        mainTextColor: workspace.mainTextColor
        dimTextColor: workspace.dimTextColor
        accentColor: workspace.accentColor
        theme: workspace.theme
        onTrackSelected: function(index) { workspace.trackSelected(index) }
        onTrackDropMoved: function(drop) { workspace.trackDrop = drop }
        onScrollPositionChanged: function(contentY) { workspace.trackScrollY = contentY }
        onResizePreview: function(trackId, height) { workspace.previewTrackHeight(trackId, height) }
        onResizeCommitted: function(trackId, height) { workspace.finishTrackHeight(trackId, height, true) }
        onResizeCanceled: function(trackId) { workspace.finishTrackHeight(trackId, Theme.trackRowHeight, false) }
        onAddInstrumentRequested: workspace.addInstrumentRequested()
        onImportAudioRequested: workspace.importAudioRequested()
        onImportMidiRequested: workspace.importMidiRequested()
        onAddGroupRequested: workspace.addGroupRequested()
        onAddReturnRequested: workspace.addReturnRequested()
        onAutomationToggled: function(trackId) { workspace.toggleAutomation(trackId) }
        onAutomationLaneAdded: function(trackId) { workspace.addAutomationLane(trackId) }
        onAutomationLaneChosen: function(trackId, index, key) { workspace.setAutomationLaneKey(trackId, index, key) }
        onAutomationLaneHidden: function(trackId, index) { workspace.hideAutomationLane(trackId, index) }
        onAutomationDrawToggled: workspace.automationDrawMode = !workspace.automationDrawMode
        onTempoLaneToggled: workspace.tempoLaneExpanded = !workspace.tempoLaneExpanded
    }

    ArrangementView {
        id: arrangement
        x: workspace.trackWidth
        width: workspace.width - x
        height: parent.height
        bridge: workspace.bridge
        visibleBars: workspace.visibleBars
        viewStartBar: workspace.viewStartBar
        trackScrollY: workspace.trackScrollY
        creationHeight: workspace.creationHeight
        onVerticalScrollRequested: function(pixels) { trackPanel.scrollBy(pixels) }
        elapsedRulerHeight: workspace.elapsedRulerHeight
        rowLayout: workspace.trackRowLayout
        trackLayout: workspace.trackLayout
        trackDrop: workspace.trackDrop
        snapToBeat: workspace.snapToBeat
        automationView: workspace.automationView
        automationDrawMode: workspace.automationDrawMode
        tempoLaneHeight: workspace.tempoLaneHeight
        tempoLaneExpanded: workspace.tempoLaneExpanded
        onTempoLaneExpandRequested: workspace.tempoLaneExpanded = true
        selectedClipId: workspace.selectedClipId
        selectedTrack: workspace.selectedTrack
        canvasColor: workspace.canvasColor
        raisedColor: workspace.raisedColor
        lineColor: workspace.lineColor
        mainTextColor: workspace.mainTextColor
        dimTextColor: workspace.dimTextColor
        accentColor: workspace.accentColor
        amberColor: workspace.amberColor
        theme: workspace.theme
        onZoomInRequested: workspace.zoomInRequested()
        onZoomOutRequested: workspace.zoomOutRequested()
        onPanRequested: function(direction) { workspace.panRequested(direction) }
        onZoomAtRequested: function(factor, bar) { workspace.zoomAtRequested(factor, bar) }
        onScrollRequested: function(bars) { workspace.scrollRequested(bars) }
        onSnapToggled: workspace.snapToggled()
        onClipSelected: function(clipId) { workspace.clipSelected(clipId) }
        onClipsSelected: function(clipIds) { workspace.clipsSelected(clipIds) }
        onClipOpened: function(clipId) { workspace.clipOpened(clipId) }
        onTrackSelected: function(index) { workspace.trackSelected(index) }
        onAutomationDrawToggled: workspace.automationDrawMode = !workspace.automationDrawMode
    }
}
