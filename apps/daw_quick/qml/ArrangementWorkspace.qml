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
    readonly property int automationLaneHeight: Theme.automationLaneHeight
    // Per track: the clip row ({top, height}), its open automation lanes
    // ([{key, top, height}]) and where the next track starts (bottom).
    readonly property var trackRowLayout: {
        const tracks = bridge.tracks
        const saved = bridge.trackRowHeights
        const preview = trackResizePreview
        const view = automationView
        let top = 0
        return tracks.map(track => {
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
            const row = { top: top, height: height, lanes: lanes, bottom: laneTop }
            top = laneTop
            return row
        })
    }
    readonly property real trackContentHeight: trackRowLayout.length > 0
        ? trackRowLayout[trackRowLayout.length - 1].bottom
        : 0

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
    function hideAutomationLane(trackId, index) {
        const shown = automationView[trackId]
        if (!shown)
            return
        const keys = shown.keys.slice()
        keys.splice(index, 1)
        setAutomationView(trackId, Object.assign({}, shown, { keys: keys }))
    }

    signal trackSelected(int index)
    signal trackMoveRequested(string trackId, int toIndex)
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
        automationView: workspace.automationView
        automationDrawMode: workspace.automationDrawMode
        panelColor: workspace.panelColor
        raisedColor: workspace.raisedColor
        lineColor: workspace.lineColor
        mainTextColor: workspace.mainTextColor
        dimTextColor: workspace.dimTextColor
        accentColor: workspace.accentColor
        theme: workspace.theme
        onTrackSelected: function(index) { workspace.trackSelected(index) }
        onTrackMoveRequested: function(trackId, toIndex) { workspace.trackMoveRequested(trackId, toIndex) }
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
        snapToBeat: workspace.snapToBeat
        automationView: workspace.automationView
        automationDrawMode: workspace.automationDrawMode
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
