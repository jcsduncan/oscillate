pragma ComponentBehavior: Bound
import QtQuick
import "devicekit" as Kit
import "harmony"
import "shell"

Window {
    id: shell
    width: Theme.windowWidth
    height: Theme.windowHeight
    minimumWidth: Theme.windowMinWidth
    minimumHeight: Theme.windowMinHeight
    visible: true
    title: projectBridge.projectOpen ? "OSCILLATE — " + projectBridge.projectName : "OSCILLATE — Welcome"
    color: canvas

    // The shell palette comes from the device kit's tokens (devicekit/
    // ShellTheme), shared with any kit widget the shell hosts.
    readonly property Kit.ShellTheme kit: Theme
    readonly property color canvas: kit.canvas
    readonly property color panel: kit.panel
    readonly property color panelRaised: kit.panelRaised
    readonly property color line: kit.line
    readonly property color textMain: kit.textMain
    readonly property color textDim: kit.textDim
    readonly property color accent: kit.accent
    readonly property color amber: kit.amber
    // The browser docks at the left of the arrangement (the shell's
    // layout: top bar, browser beside the arrangement, device chain below).
    readonly property int browserWidth: projectBridge.projectOpen && currentView === "arrange" ? Theme.browserDockWidth : 0
    readonly property int inspectorWidth: inspectorWorkspace.inspectorWidth
    readonly property bool fxPanelActive: inspectorWorkspace.fxPanelActive
    property alias selectedSendId: inspectorWorkspace.selectedSendId
    property alias exportFormatMenuOpen: exportDialog.exportFormatMenuOpen
    property alias exportRateMenuOpen: exportDialog.exportRateMenuOpen
    property alias exportMoreOpen: exportDialog.moreOpen
    property bool exportDialogOpen: false
    property int insertChainHeight: projectBridge.workspaceLayout.deviceDockHeight
    onInsertChainHeightChanged: projectBridge.workspaceLayout.deviceDockHeight = insertChainHeight
    property bool contextPanelOpen: projectBridge.workspaceLayout.contextPanelOpen
    onContextPanelOpenChanged: projectBridge.workspaceLayout.contextPanelOpen = contextPanelOpen
    readonly property int actualDockHeight: Math.max(Theme.deviceDockMinHeight,
        Math.min(Math.max(insertChainHeight,
            deviceDock.activeTab === 2 && deviceDock.selectedClip?.kind === "midi" ? Theme.deviceDockMidiHeight : Theme.deviceDockMinHeight),
            height - topBar.height - Theme.deviceDockArrangeReserve))
    // "arrange", "mixer" (the view switch in the top bar) or "mixcheck"
    // (Mix Check: the toolbar button, Ctrl+Shift+M, the command palette).
    property string currentView: "arrange"
    property bool developerDetailsOpen: false
    property bool shortcutsOpen: false
    property bool audioMenuOpen: false
    property bool revisionPanelOpen: false
    property bool historyPanelOpen: false
    property bool mediaPanelOpen: false
    property bool cachePanelOpen: false
    property bool saveCopyPanelOpen: false
    property bool pluginBrowserOpen: false
    property bool pluginBrowserScanned: false
    function openPluginBrowser(showBuiltIns) {
        pluginBrowser.showBuiltIns = showBuiltIns === true
        pluginBrowserOpen = true
        if (!pluginBrowserScanned && projectBridge.vst3Browser.vst3BrowserAvailable) {
            pluginBrowserScanned = true
            projectBridge.vst3Browser.refresh_vst3_catalogue()
        }
    }
    // The arrangement's view in (fractional) bars: from the whole project
    // down to half a beat, for lining up two tracks' kicks.
    readonly property real minimumVisibleBars: 0.125
    property real visibleBars: 16
    property real viewStartBar: 0
    function clampView() {
        visibleBars = Math.max(minimumVisibleBars, Math.min(visibleBars, projectBridge.timelineBars))
        viewStartBar = Math.max(0, Math.min(viewStartBar, projectBridge.timelineBars - visibleBars))
    }
    // Zooms by `factor` (below 1 zooms in) keeping `anchorBar` where it is
    // on screen: the playhead when it is in view, else the view's centre.
    function zoomAround(factor, anchorBar) {
        const next = Math.max(minimumVisibleBars, Math.min(projectBridge.timelineBars, visibleBars * factor))
        const share = (anchorBar - viewStartBar) / visibleBars
        const nextStart = Math.max(0, Math.min(anchorBar - share * next,
            projectBridge.timelineBars - next))
        visibleBars = next
        viewStartBar = nextStart
    }
    function zoomAnchor() {
        const playhead = projectBridge.playheadBar
        return playhead >= viewStartBar && playhead <= viewStartBar + visibleBars
            ? playhead : viewStartBar + visibleBars / 2
    }
    function zoomIn() { zoomAround(0.5, zoomAnchor()) }
    function zoomOut() { zoomAround(2, zoomAnchor()) }
    function panBars(direction) {
        viewStartBar = viewStartBar + direction * visibleBars
        clampView()
    }
    // Wheel and trackpad scrolling: `bars` may be fractional.
    function scrollBars(bars) {
        viewStartBar = viewStartBar + bars
        clampView()
    }
    // -2 is the reserved logical master path; it is never a saved TrackId.
    property int selectedTrack: projectBridge.tracks.length > 0 ? 0 : -2
    property alias trackScrollY: workspace.trackScrollY
    readonly property var currentRouteTrack: selectedTrack >= 0 && selectedTrack < projectBridge.tracks.length
        ? projectBridge.tracks[selectedTrack]
        : ({ id: "", mainDestinationName: "Master", sends: [], sendCount: 0 })
    property string selectedClipId: ""
    // Set while selectClips() picks a group, so the group stays selected.
    property bool keepClipGroup: false
    onSelectedClipIdChanged: {
        projectBridge.select_audio_clip(selectedClipId);
        if (!keepClipGroup)
            projectBridge.selectedClipIds = selectedClipId.length > 0 ? [selectedClipId] : [];
        for (let index = 0; index < projectBridge.tracks.length; ++index) {
            if (projectBridge.tracks[index].clips.some(clip => clip.id === selectedClipId)) {
                selectedTrack = index;
                break;
            }
        }
    }
    property bool snapToBeat: true

    // What the musician needs to know, in plain words. The exact engine
    // status stays in the developer details popover.
    readonly property bool autosaveFailed: {
        const text = projectBridge.autosaveStatus
        return text.startsWith("Autosave failed") || text.startsWith("Autosave could not")
            || text.startsWith("Autosave worker") || text.startsWith("Autosave durability")
    }
    readonly property bool statusWarning: projectBridge.playbackRevisionInactive
        || autosaveFailed || projectBridge.saveProblem.length > 0
    readonly property string plainStatus: {
        if (projectBridge.imports.importBusy)
            return "Importing…"
        if (projectBridge.exports.exportBusy)
            return "Exporting…"
        if (projectBridge.saveProblem.length > 0)
            return "Not saved · press Ctrl+S to try again"
        if (autosaveFailed)
            return "Autosave failed · save to keep your changes"
        if (projectBridge.playbackRevisionInactive)
            return "Restart playback to hear this change"
        if (projectBridge.playbackActive && projectBridge.playbackPaused)
            return "Paused · changes apply live"
        if (projectBridge.playbackActive)
            return "Playing · changes apply live"
        if (!projectBridge.playbackEnabled)
            return "Saved · preparing playback"
        return "Saved · changes apply live"
    }

    // The selection the editing shortcuts act on.
    readonly property var selectedClip: {
        if (selectedClipId.length === 0)
            return null
        for (const track of projectBridge.tracks) {
            const found = track.clips.find(clip => clip.id === selectedClipId)
            if (found)
                return found
        }
        return null
    }
    readonly property var selectedTrackData: selectedTrack >= 0 && selectedTrack < projectBridge.tracks.length
        ? projectBridge.tracks[selectedTrack] : null
    // Copy, cut, paste and duplicate act on the bridge's selection.
    onSelectedTrackDataChanged: projectBridge.selectedTrackId = selectedTrackData ? selectedTrackData.id : ""
    // Text entry keeps every key; bare-key shortcuts also stand aside while
    // an editor overlay, the browser or the device dock has the keyboard, so
    // a focused knob's Delete or arrow keys never edit the arrangement. The
    // piano roll itself (focused by a click on its grid) is the exception:
    // Space, Delete and the other bare keys still work there.
    readonly property bool textFocus: activeFocusItem !== null
        && typeof activeFocusItem.cursorPosition === "number"
    function focusInside(container) {
        for (let item = activeFocusItem; item; item = item.parent) {
            if (item === container)
                return true
        }
        return false
    }
    // A project change held for the plug-in state choice keeps the project
    // still: no shortcut acts until the choice is made.
    readonly property bool pluginStateChoiceOpen: projectBridge.pluginStateProblem.length > 0
    readonly property bool projectKeys: projectBridge.projectOpen && !pluginStateChoiceOpen
    readonly property bool bareKeysAllowed: projectKeys && !textFocus
        && projectBridge.nativeSynthController === null
        && projectBridge.nativeEffectController === null
        && !pluginBrowserOpen && !shortcutsOpen && !commandPalette.open
        && (!focusInside(deviceDock) || deviceDock.notesRollFocused)
    // Selected notes or clips duplicate in place; with neither, the track.
    function duplicateSelection() {
        if (projectBridge.selectedNoteIds.length > 0 || projectBridge.selectedClipIds.length > 0)
            projectBridge.duplicate_selection()
        else if (selectedTrackData)
            projectBridge.duplicate_track(selectedTrackData.id)
    }
    // Several clips at once (Shift/Ctrl-click, a drawn box): the last one
    // picked is the clip the inspector and dock show.
    function selectClips(ids) {
        keepClipGroup = true
        selectedClipId = ids.length > 0 ? ids[ids.length - 1] : ""
        keepClipGroup = false
        projectBridge.selectedClipIds = ids
    }
    function deleteSelection() {
        // Selected notes go first: Delete in the piano roll never takes the clip.
        if (projectBridge.selectedNoteIds.length > 0) {
            projectBridge.remove_notes(projectBridge.selectedNoteClipId, projectBridge.selectedNoteIds)
            return
        }
        if (projectBridge.selectedClipIds.length > 1) {
            projectBridge.delete_selected_clips()
            selectedClipId = ""
            return
        }
        const clip = selectedClip
        if (!clip)
            return
        if (clip.kind === "audio")
            projectBridge.delete_audio_clip(clip.id)
        else
            projectBridge.delete_midi_clip(clip.id)
        selectedClipId = ""
    }
    function splitSelection() {
        const clip = selectedClip
        if (clip && clip.kind === "audio")
            projectBridge.split_audio_clip_at_playhead(clip.id)
        else if (clip && clip.kind === "midi")
            projectBridge.split_midi_clip_at_playhead(clip.id)
    }
    function quantizeSelection() {
        const clip = selectedClip
        if (clip && clip.kind === "midi")
            projectBridge.quantize_midi_clip(clip.id, 16)
    }
    // Mix Check is a full view; asking again goes back to the arrangement.
    function toggleMixCheck() {
        if (projectBridge.projectOpen)
            currentView = currentView === "mixcheck" ? "arrange" : "mixcheck"
    }
    // The selected audio clip's notes in Clay (ADR-0081), in the dock.
    function editClipNotes() {
        currentView = "arrange"
        deviceDock.openNotes()
    }
    // The piano roll with its chord strip and tools open (ADR-0084).
    function editClipChords() {
        currentView = "arrange"
        deviceDock.openChords()
    }
    // Arrange from a loop (ADR-0080) is a full view too; leaving it bins a
    // proposal that is only being heard.
    function toggleArrangeFromLoop() {
        if (projectBridge.projectOpen)
            currentView = currentView === "arrangeloop" ? "arrange" : "arrangeloop"
    }
    // A chord the piano roll's chord tools ask to hear (ADR-0084) plays on
    // the clip's track through its own sound, unless the song is playing.
    readonly property int harmonyAuditionMs: 700
    property var harmonyAuditionKeys: []
    Connections {
        target: projectBridge.harmony
        function onAuditionRequested(track, keys) {
            if (track === "" || projectBridge.playbackActive || keys.length === 0)
                return
            shell.harmonyAuditionKeys.forEach(key => projectBridge.release_note(key))
            shell.harmonyAuditionKeys = keys
            keys.forEach(key => projectBridge.play_note(track, key, 92))
            harmonyRelease.restart()
        }
    }
    Timer {
        id: harmonyRelease
        interval: shell.harmonyAuditionMs
        onTriggered: {
            shell.harmonyAuditionKeys.forEach(key => projectBridge.release_note(key))
            shell.harmonyAuditionKeys = []
        }
    }
    Connections {
        target: projectBridge.arrange
        function onOpenRequested() {
            if (projectBridge.projectOpen)
                shell.currentView = "arrangeloop"
        }
        function onCloseRequested() {
            if (shell.currentView === "arrangeloop")
                shell.currentView = "arrange"
        }
    }
    // Export opens the dialog; the export itself starts from its button.
    function openExportDialog() {
        if (projectBridge.projectOpen)
            exportDialogOpen = true
    }

    onClosing: function(close) {
        close.accepted = projectBridge.request_close()
    }
    Connections {
        target: projectBridge
        function onReadyToClose() { shell.close() }
        function onTrackCreated(index) {
            shell.selectedClipId = ""
            shell.selectedTrack = index
        }
        function onTimelineChanged() { shell.clampView() }
        function onTracksChanged() {
            if (shell.selectedTrack >= projectBridge.tracks.length)
                shell.selectedTrack = projectBridge.tracks.length > 0
                    ? projectBridge.tracks.length - 1 : -2
        }
        function onNamedRevisionApplied() {
            shell.selectedClipId = ""
            shell.selectedTrack = projectBridge.tracks.length > 0 ? 0 : -2
            shell.revisionPanelOpen = false
            shell.mediaPanelOpen = false
        }
    }

    Shortcut {
        sequence: "Ctrl+Shift+N"
        enabled: shell.projectKeys
        onActivated: projectBridge.trackCommands.add_synth_track()
    }
    Shortcut {
        sequence: "Ctrl+S"
        enabled: shell.projectKeys
        onActivated: projectBridge.save()
    }
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: shell.projectKeys
        onActivated: projectBridge.undo()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Z"
        enabled: shell.projectKeys
        onActivated: projectBridge.redo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: shell.projectKeys
        onActivated: projectBridge.redo()
    }
    Shortcut {
        // The computer keyboard as a musical keyboard (A W S E D F T G Y H U J K…).
        sequence: "Ctrl+Shift+K"
        enabled: shell.projectKeys
        onActivated: projectBridge.computerKeyboard.toggle()
    }
    Shortcut {
        // Record (ADR-0086): notes played go into the track.
        sequence: "Ctrl+R"
        enabled: shell.projectKeys
        onActivated: projectBridge.recording.toggle_record()
    }
    Shortcut {
        // Anything by name: actions, tracks, instruments, effects, presets.
        sequence: "Ctrl+K"
        enabled: !shell.pluginStateChoiceOpen
        onActivated: commandPalette.open ? commandPalette.hide() : commandPalette.show()
    }
    Shortcut {
        sequence: "Ctrl+H"
        enabled: shell.projectKeys
        onActivated: shell.historyPanelOpen = !shell.historyPanelOpen
    }
    Shortcut {
        sequence: "Space"
        enabled: shell.bareKeysAllowed && projectBridge.playbackEnabled
        onActivated: projectBridge.toggle_pause()
    }
    Shortcut {
        sequence: "Ctrl+Shift+E"
        enabled: shell.projectKeys
        onActivated: {
            if (shell.exportDialogOpen)
                shell.exportDialogOpen = false
            else
                shell.openExportDialog()
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+M"
        enabled: shell.projectKeys
        onActivated: shell.toggleMixCheck()
    }
    // Editing. ShortcutsOverlay lists these; keep the two in step.
    Shortcut {
        sequence: "Ctrl+D"
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: shell.duplicateSelection()
    }
    Shortcut {
        sequence: "Ctrl+J"
        enabled: shell.projectKeys && !shell.textFocus
                 && projectBridge.selectedClipIds.length > 1
        onActivated: {
            projectBridge.merge_selected_clips()
            shell.selectClips(projectBridge.selectedClipIds)
        }
    }
    Shortcut {
        // Every clip on every track. The piano roll keeps its own Ctrl+A
        // while it has the keyboard.
        sequences: [StandardKey.SelectAll]
        enabled: shell.bareKeysAllowed && shell.currentView === "arrange"
                 && !deviceDock.notesHaveKeyboard
        onActivated: {
            // The notes let go first, so Delete then removes the clips.
            deviceDock.clearNoteSelection()
            workspace.selectAllClips()
        }
    }
    Shortcut {
        sequences: [StandardKey.Copy]
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: projectBridge.copy_selection()
    }
    Shortcut {
        sequences: [StandardKey.Cut]
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: {
            projectBridge.cut_selection()
            if (projectBridge.selectedClipIds.length === 0)
                shell.selectedClipId = ""
        }
    }
    Shortcut {
        sequences: [StandardKey.Paste]
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: projectBridge.paste_at_playhead()
    }
    Shortcut {
        sequence: "A"
        enabled: shell.bareKeysAllowed && shell.currentView === "arrange"
            && projectBridge.tracks.length > shell.selectedTrack && shell.selectedTrack >= 0
        onActivated: workspace.toggleAutomation(projectBridge.tracks[shell.selectedTrack].id)
    }
    Shortcut {
        sequence: "B"
        enabled: shell.bareKeysAllowed && shell.currentView === "arrange"
        onActivated: workspace.automationDrawMode = !workspace.automationDrawMode
    }
    Shortcut {
        sequences: ["Delete", "Backspace"]
        enabled: shell.bareKeysAllowed
                 && (shell.selectedClip !== null || projectBridge.selectedNoteIds.length > 0)
        onActivated: shell.deleteSelection()
    }
    Shortcut {
        sequence: "Ctrl+E"
        enabled: shell.projectKeys && !shell.textFocus && shell.selectedClip !== null
        onActivated: shell.splitSelection()
    }
    Shortcut {
        sequence: "Ctrl+U"
        enabled: shell.projectKeys && !shell.textFocus && shell.selectedClip !== null
        onActivated: shell.quantizeSelection()
    }
    Shortcut {
        sequence: "Ctrl+L"
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: projectBridge.loopTransport.set_enabled(!projectBridge.loopTransport.enabled)
    }
    Shortcut {
        sequences: ["+", "="]
        enabled: shell.bareKeysAllowed
        onActivated: shell.zoomIn()
    }
    Shortcut {
        sequence: "-"
        enabled: shell.bareKeysAllowed
        onActivated: shell.zoomOut()
    }
    Shortcut {
        sequence: "Ctrl+T"
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: projectBridge.trackCommands.add_synth_track()
    }
    Shortcut {
        sequence: "M"
        enabled: shell.bareKeysAllowed && shell.selectedTrackData !== null
        onActivated: projectBridge.trackCommands.set_track_muted(shell.selectedTrackData.id,
                                                                 !shell.selectedTrackData.muted)
    }
    Shortcut {
        sequence: "S"
        enabled: shell.bareKeysAllowed && shell.selectedTrackData !== null
        onActivated: projectBridge.trackCommands.set_track_soloed(shell.selectedTrackData.id,
                                                                  !shell.selectedTrackData.soloed)
    }
    Shortcut {
        sequence: "Ctrl+M"
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: projectBridge.add_marker_at_playhead()
    }
    Shortcut {
        sequence: "["
        enabled: shell.bareKeysAllowed
        onActivated: projectBridge.jump_to_marker(-1)
    }
    Shortcut {
        sequence: "]"
        enabled: shell.bareKeysAllowed
        onActivated: projectBridge.jump_to_marker(1)
    }
    Shortcut {
        sequences: ["?", "F1"]
        enabled: shell.projectKeys && !shell.textFocus
        onActivated: shell.shortcutsOpen = !shell.shortcutsOpen
    }
    Shortcut {
        sequence: "Escape"
        enabled: commandPalette.open || shell.historyPanelOpen || shell.exportDialogOpen
            || shell.shortcutsOpen || shell.developerDetailsOpen
            || (shell.currentView === "mixcheck" && !shell.textFocus)
            || (shell.currentView === "arrangeloop" && !shell.textFocus)
            || (shell.selectedClipId.length > 0 && projectBridge.selectedNoteIds.length === 0
                && !shell.textFocus)
        onActivated: {
            if (commandPalette.open) {
                commandPalette.hide()
            } else if (shell.historyPanelOpen) {
                shell.historyPanelOpen = false
            } else if (shell.exportDialogOpen) {
                shell.exportDialogOpen = false
            } else if (shell.shortcutsOpen || shell.developerDetailsOpen) {
                shell.shortcutsOpen = false
                shell.developerDetailsOpen = false
            } else if (shell.currentView === "mixcheck" || shell.currentView === "arrangeloop") {
                shell.currentView = "arrange"
            } else {
                // Escape lets go of the selected clip.
                shell.selectedClipId = ""
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+I"
        enabled: projectBridge.imports.importEnabled
        onActivated: projectBridge.imports.choose_import_audio()
    }
    Shortcut {
        sequence: "Ctrl+Shift+I"
        enabled: projectBridge.imports.importEnabled
        onActivated: projectBridge.imports.choose_import_midi()
    }
    Shortcut {
        sequence: "Ctrl+Shift+P"
        enabled: shell.projectKeys
        onActivated: shell.openPluginBrowser()
    }

    ProjectToolbar {
        id: topBar
        visible: projectBridge.projectOpen
        anchors.top: parent.top
        width: parent.width
        bridge: projectBridge
        panelColor: shell.panel
        raisedColor: shell.panelRaised
        lineColor: shell.line
        mainTextColor: shell.textMain
        dimTextColor: shell.textDim
        accentColor: shell.accent
        amberColor: shell.amber
        canvasColor: shell.canvas
        monoFamily: shell.kit.monoFamily
        statusText: shell.plainStatus
        statusWarning: shell.statusWarning
        currentView: shell.currentView
        onViewRequested: function(view) { shell.currentView = view }
        onExportRequested: shell.openExportDialog()
        onMixCheckRequested: shell.toggleMixCheck()
        onDetailsRequested: shell.developerDetailsOpen = !shell.developerDetailsOpen
        onPlaybackRequested: projectBridge.toggle_pause()
        onStopRequested: projectBridge.stop_playback()
        onMediaRequested: {
            shell.mediaPanelOpen = !shell.mediaPanelOpen
            if (shell.mediaPanelOpen) shell.revisionPanelOpen = false
        }
        onRevisionsRequested: {
            shell.revisionPanelOpen = !shell.revisionPanelOpen
            if (shell.revisionPanelOpen) {
                shell.mediaPanelOpen = false
                projectBridge.refresh_named_revisions()
            }
        }
        onAudioMenuRequested: shell.audioMenuOpen = !shell.audioMenuOpen
        onInspectorRequested: inspectorWorkspace.showContext(false)
        // The master chain is edited like any track's, in the device chain.
        onMasterEffectsRequested: {
            shell.selectedTrack = -2
            shell.currentView = "arrange"
            deviceDock.activeTab = 0
        }
        onPluginsRequested: shell.openPluginBrowser()
        onCacheRequested: shell.cachePanelOpen = !shell.cachePanelOpen
        onSaveCopyRequested: shell.saveCopyPanelOpen = !shell.saveCopyPanelOpen
        onShortcutsRequested: shell.shortcutsOpen = !shell.shortcutsOpen
    }

    DockedBrowser {
        id: browser
        visible: shell.browserWidth > 0
        x: 0
        y: topBar.height
        width: shell.browserWidth
        height: inspectorWorkspace.height
        bridge: projectBridge
        selectedTrack: shell.selectedTrackData
        onExpandRequested: shell.openPluginBrowser(true)
    }

    ManagedMediaPanel {
        host: shell
        bridge: projectBridge
        y: topBar.height - 1
    }

    NamedRevisionPanel {
        host: shell
        bridge: projectBridge
        y: topBar.height - 1
    }

    HistoryPanel {
        host: shell
        bridge: projectBridge
        y: topBar.height - 1
    }

    SaveCopyPanel {
        host: shell
        bridge: projectBridge
        y: topBar.height - 1
    }

    PluginStateProblemPanel {
        host: shell
        bridge: projectBridge
    }

    AudioOutputPanel {
        host: shell
        bridge: projectBridge
        y: topBar.height - 1
    }

    CachePanel {
        host: shell
        controls: projectBridge.cacheControls
    }

    ArrangementWorkspace {
        id: workspace
        visible: projectBridge.projectOpen && shell.currentView === "arrange"
        theme: shell.kit
        x: shell.browserWidth
        y: topBar.height
        width: shell.width - x - shell.inspectorWidth
        height: inspectorWorkspace.height
        bridge: projectBridge
        selectedTrack: shell.selectedTrack
        selectedClipId: shell.selectedClipId
        visibleBars: shell.visibleBars
        viewStartBar: shell.viewStartBar
        snapToBeat: shell.snapToBeat
        canvasColor: shell.canvas
        panelColor: shell.panel
        raisedColor: shell.panelRaised
        lineColor: shell.line
        mainTextColor: shell.textMain
        dimTextColor: shell.textDim
        accentColor: shell.accent
        amberColor: shell.amber
        onRowHeightCommitted: function(trackId, height) {
            projectBridge.set_track_row_height(trackId, height)
        }
        onAddInstrumentRequested: projectBridge.trackCommands.add_synth_track()
        onImportAudioRequested: projectBridge.imports.choose_import_audio_above_placeholder()
        onImportMidiRequested: projectBridge.imports.choose_import_midi()
        onAddGroupRequested: projectBridge.trackCommands.add_bus_track("group")
        onAddReturnRequested: projectBridge.trackCommands.add_bus_track("return")
        onZoomInRequested: shell.zoomIn()
        onZoomOutRequested: shell.zoomOut()
        onPanRequested: function(direction) { shell.panBars(direction) }
        onZoomAtRequested: function(factor, bar) { shell.zoomAround(factor, bar) }
        onScrollRequested: function(bars) { shell.scrollBars(bars) }
        onSnapToggled: shell.snapToBeat = !shell.snapToBeat
        onClipSelected: function(clipId) { shell.selectedClipId = clipId }
        onClipsSelected: function(clipIds) { shell.selectClips(clipIds) }
        onClipOpened: function(clipId) {
            shell.selectedClipId = clipId
            deviceDock.openNotes()
        }
        onTrackSelected: function(index) {
            // A clip on another track stops being the target of Delete,
            // Ctrl+D and Ctrl+E once a different track is chosen.
            const track = projectBridge.tracks[index]
            if (track && shell.selectedClip && !track.clips.some(clip => clip.id === shell.selectedClipId))
                shell.selectedClipId = ""
            shell.selectedTrack = index
        }
        // Switching views crossfades: the incoming view fades in.
        Reveal { target: workspace; duration: Motion.slow }
    }
    // A dropped clip offered the song's key (ADR-0059).
    KeyMatchHint {
        bridge: projectBridge
        z: 30
        x: workspace.x + (workspace.width - width) / 2
        y: workspace.y + workspace.height - height - Theme.keyHintBottomGap
        visible: workspace.visible && shown
    }

    InspectorWorkspace {
        id: inspectorWorkspace
        visible: projectBridge.projectOpen
        x: 0
        y: topBar.height
        width: shell.width
        height: shell.height - topBar.height - shell.actualDockHeight
        bridge: projectBridge
        selectedTrack: shell.selectedTrack
        selectedClipId: shell.selectedClipId
        currentRouteTrack: shell.currentRouteTrack
        contextPanelOpen: shell.contextPanelOpen
        canvas: shell.canvas
        panel: shell.panel
        panelRaised: shell.panelRaised
        line: shell.line
        textMain: shell.textMain
        textDim: shell.textDim
        accent: shell.accent
        amber: shell.amber
        onContextPanelRequested: shell.contextPanelOpen = true
        onContextPanelCloseRequested: shell.contextPanelOpen = false
        onExportRequested: shell.openExportDialog()
    }

    DeviceChainDock {
        id: deviceDock
        visible: projectBridge.projectOpen && shell.currentView === "arrange"
        x: 0
        y: shell.height - shell.actualDockHeight
        width: shell.width - x - shell.inspectorWidth
        height: shell.actualDockHeight
        bridge: projectBridge
        selectedTrack: shell.selectedTrack
        selectedClipId: shell.selectedClipId
        currentRouteTrack: shell.currentRouteTrack
        panelColor: shell.panel
        raisedColor: shell.panelRaised
        lineColor: shell.line
        textColor: shell.textMain
        dimColor: shell.textDim
        accentColor: shell.accent
        warningColor: shell.amber
        onResizeRequested: function(height) {
            shell.insertChainHeight = Math.max(Theme.deviceDockMinHeight,
                Math.min(shell.height - topBar.height - Theme.deviceDockArrangeReserve, height))
        }
        onContextRequested: shell.contextPanelOpen = true
        pathColor: shell.selectedTrack === -2 || !shell.selectedTrackData ? shell.accent
            : shell.kit.trackColor(shell.selectedTrackData, shell.selectedTrack)
        Reveal { target: deviceDock; duration: Motion.slow }
    }

    // The mixer takes the arrangement and dock's place.
    Item {
        id: mixerArea
        objectName: "mixerArea"
        visible: projectBridge.projectOpen && shell.currentView === "mixer"
        x: 0
        y: topBar.height
        width: shell.width - x
        height: shell.height - topBar.height
        Reveal { target: mixerArea; duration: Motion.slow }
        Loader {
            id: mixerLoader
            anchors.fill: parent
            active: mixerArea.visible
            onActiveChanged: {
                if (active)
                    setSource("MixerView.qml", { bridge: projectBridge })
            }
            // "+ Insert" opens the browser for that strip's path; a convolution
            // insert opens in the arrangement's device chain.
            function selectPath(pathId) {
                shell.selectedTrack = pathId === "master" ? -2
                    : projectBridge.tracks.findIndex(track => track.id === pathId)
            }
            onLoaded: {
                item.browserRequested.connect(function(pathId) {
                    mixerLoader.selectPath(pathId)
                    shell.openPluginBrowser(true)
                })
                item.nativeInsertEditorRequested.connect(function(pathId, slotId, kind) {
                    mixerLoader.selectPath(pathId)
                    shell.currentView = "arrange"
                    deviceDock.activeTab = 0
                })
            }
        }
    }

    // Mix Check takes the arrangement and dock's place, like the mixer.
    Item {
        id: mixCheckArea
        objectName: "mixCheckArea"
        visible: projectBridge.projectOpen && shell.currentView === "mixcheck"
        x: 0
        y: topBar.height
        width: shell.width
        height: shell.height - topBar.height
        Reveal { target: mixCheckArea; duration: Motion.slow }
        Loader {
            id: mixCheckLoader
            anchors.fill: parent
            active: mixCheckArea.visible
            onActiveChanged: {
                if (active)
                    setSource("MixCheckView.qml", { controller: projectBridge.mixCheck })
            }
        }
    }

    // Groove from a record (ADR-0079): over everything, from the piano roll's
    // tools, an audio clip's menu or the command palette.
    Loader {
        id: grooveCaptureLoader
        objectName: "grooveCaptureLoader"
        z: 40
        anchors.fill: parent
        active: projectBridge.projectOpen && projectBridge.grooveCapture.open
        onActiveChanged: {
            if (active)
                setSource("GrooveCapturePanel.qml", { bridge: projectBridge })
        }
        onLoaded: item.forceActiveFocus()
    }

    // Arrange from a loop takes the same place as Mix Check.
    Item {
        id: arrangeLoopArea
        objectName: "arrangeLoopArea"
        visible: projectBridge.projectOpen && shell.currentView === "arrangeloop"
        x: 0
        y: topBar.height
        width: shell.width
        height: shell.height - topBar.height
        Reveal { target: arrangeLoopArea; duration: Motion.slow }
        Loader {
            id: arrangeLoopLoader
            anchors.fill: parent
            active: arrangeLoopArea.visible
            onActiveChanged: {
                if (active)
                    setSource("ArrangeLoopView.qml", { controller: projectBridge.arrange, bridge: projectBridge })
            }
        }
    }

    // Engine detail for diagnosis: the model revision and the exact playback,
    // autosave and buffer status, kept out of the musician's way.
    Rectangle {
        id: developerDetails
        objectName: "developerDetails"
        visible: shell.developerDetailsOpen && projectBridge.projectOpen
        z: 25
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceLg
        anchors.top: topBar.bottom
        anchors.topMargin: Theme.spaceSm
        width: Theme.developerDetailsWidth
        height: detailsColumn.implicitHeight + 2 * Theme.spaceLg
        radius: Theme.popoverRadius
        color: shell.panelRaised
        border.color: shell.kit.lineStrong
        transformOrigin: Item.TopRight
        Reveal { target: developerDetails; fromScale: Motion.popScale }
        Column {
            id: detailsColumn
            x: Theme.spaceLg
            y: Theme.spaceLg
            width: parent.width - 2 * Theme.spaceLg
            spacing: Theme.spaceSm
            Text {
                text: "ENGINE DETAILS"
                color: shell.textDim
                font.pixelSize: Theme.typeSmall
                font.letterSpacing: Theme.trackingMd
            }
            Repeater {
                model: [
                    "Model revision " + projectBridge.revision,
                    "Playback: " + projectBridge.playbackStatus,
                    "Autosave: " + projectBridge.autosaveStatus,
                    "Buffer: " + projectBridge.audioOutput.bufferSizeStatus,
                    "Status: " + projectBridge.status
                ]
                delegate: Text {
                    required property string modelData
                    width: detailsColumn.width
                    wrapMode: Text.Wrap
                    text: modelData
                    color: shell.textMain
                    font.pixelSize: Theme.typeBody
                    font.family: shell.kit.monoFamily
                }
            }
        }
    }

    Rectangle {
        id: browserScrim
        anchors.fill: parent
        visible: shell.pluginBrowserOpen && projectBridge.projectOpen
        color: Theme.browserScrim
        z: 19
        Reveal { target: browserScrim }
        MouseArea { anchors.fill: parent; onClicked: shell.pluginBrowserOpen = false }
    }
    NewTrackDropArea {
        anchors.fill: parent
        // Mix Check takes its own drops (a reference track).
        visible: projectBridge.projectOpen && shell.currentView !== "mixcheck"
        z: 18
        onAudioDropped: function(source) { projectBridge.imports.import_audio(source) }
        instrumentTargetAt: (item, x, y) => workspace.visible ? workspace.instrumentDropTarget(item, x, y) : null
        onMidiDropped: function(source) { projectBridge.imports.import_midi(source) }
        onMidiDroppedOnTrack: function(source, trackId, bar) {
            projectBridge.imports.import_midi_to_track(source, trackId, bar)
        }
        onInstrumentDropped: function(entryIndex, classId) {
            projectBridge.vst3Browser.create_vst3_instrument_track(entryIndex, classId)
        }
    }
    Vst3BrowserPanel {
        id: pluginBrowser
        objectName: "vst3BrowserPanel"
        x: shell.width - width
        y: topBar.height
        width: Math.min(Theme.pluginBrowserWidth, shell.width * Theme.pluginBrowserWidthShare)
        height: shell.height - topBar.height
        z: 20
        visible: shell.pluginBrowserOpen && projectBridge.projectOpen
        bridge: projectBridge
        selectedTrack: shell.currentRouteTrack
        onCloseRequested: shell.pluginBrowserOpen = false
    }

    Vst3NativeEditorOverlay {
        anchors.fill: parent
        z: 40
        bridge: projectBridge
        panelColor: shell.panel
        panelRaisedColor: shell.panelRaised
        lineColor: shell.line
        textColor: shell.textMain
        dimColor: shell.textDim
        accentColor: shell.accent
    }

    NativeSynthEditorOverlay {
        id: nativeSynthEditorOverlay
        objectName: "nativeSynthEditorOverlay"
        anchors.fill: parent
        // Docked, a device panel belongs to the device chain: switching the
        // bottom area to the clip or MIDI editor puts it away.
        visible: controller !== null
            && (!docked || deviceDock.activeTab === 0 || !deviceDock.visible)
        z: 42
        bridge: projectBridge
        controller: projectBridge.nativeSynthController
        automation: projectBridge.nativeSynthAutomation
        panelModel: projectBridge.nativeSynthPanelModel
        onCloseRequested: projectBridge.close_native_synth_editor()
    }

    NativeEffectEditorOverlay {
        id: nativeEffectEditorOverlay
        objectName: "nativeEffectEditorOverlay"
        anchors.fill: parent
        // Docked, a device panel belongs to the device chain: switching the
        // bottom area to the clip or MIDI editor puts it away.
        visible: controller !== null
            && (!docked || deviceDock.activeTab === 0 || !deviceDock.visible)
        z: 43
        bridge: projectBridge
        controller: projectBridge.nativeEffectController
        automation: projectBridge.nativeEffectAutomation
        context: projectBridge.nativeEffectContext
        panelModel: projectBridge.nativeEffectPanelModel
        onCloseRequested: projectBridge.close_native_effect_editor()
    }

    // Opening a device panel brings the device chain forward in the dock.
    Connections {
        target: projectBridge
        function onNativeSynthControllerChanged() {
            if (projectBridge.nativeSynthController !== null)
                deviceDock.activeTab = 0
        }
        function onNativeEffectControllerChanged() {
            if (projectBridge.nativeEffectController !== null)
                deviceDock.activeTab = 0
        }
    }

    CommandPalette {
        id: commandPalette
        anchors.fill: parent
        host: shell
        bridge: projectBridge
    }

    ShortcutsOverlay {
        objectName: "shortcutsOverlay"
        anchors.fill: parent
        z: 50
        visible: shell.shortcutsOpen && projectBridge.projectOpen
        panelColor: shell.panel
        raisedColor: shell.panelRaised
        lineColor: shell.line
        textColor: shell.textMain
        dimColor: shell.textDim
        accentColor: shell.accent
        monoFamily: shell.kit.monoFamily
        onCloseRequested: shell.shortcutsOpen = false
    }

    ExportDialog {
        id: exportDialog
        objectName: "exportDialog"
        anchors.fill: parent
        z: 52
        visible: shell.exportDialogOpen && projectBridge.projectOpen
        bridge: projectBridge
        canvas: shell.canvas
        panel: shell.panel
        panelRaised: shell.panelRaised
        line: shell.line
        textMain: shell.textMain
        textDim: shell.textDim
        accent: shell.accent
        amber: shell.amber
        onCloseRequested: shell.exportDialogOpen = false
    }

    // Problems the bridge reports (a refused edit, a failed save) appear
    // briefly where the musician is looking, in plain words; routine
    // confirmations stay quiet. Engine details still holds every message.
    Rectangle {
        id: problemToast
        objectName: "problemToast"
        property string message: ""
        // How long a problem stays on screen before it fades.
        readonly property int visibleMs: 4500
        readonly property var problemPattern:
            /rejected|refused|could not|couldn't|failed|failure|not saved|finish|stop playback|wait for|unavailable|invalid|outside|occupied|only |not found|cannot|can't|uncertain/i
        function plain(text) {
            return String(text)
                .replace(/\s*·\s*(model )?revision \d+/gi, "")
                .replace(/\s*·\s*Save to keep/gi, "")
                .replace(/\s*·\s*Undo is available/gi, "")
                .trim()
        }
        function show(text) {
            const cleaned = plain(text)
            if (cleaned.length === 0 || !problemPattern.test(cleaned))
                return
            message = cleaned
            opacity = 1
            hideTimer.restart()
        }
        Connections {
            target: projectBridge
            function onStatusChanged() { problemToast.show(projectBridge.status) }
        }
        Timer { id: hideTimer; interval: problemToast.visibleMs; onTriggered: problemToast.opacity = 0 }
        z: 60
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.toastBottomMargin
        width: Math.min(parent.width - Theme.toastWindowInset, toastText.implicitWidth + Theme.toastPaddingX)
        height: toastText.implicitHeight + Theme.toastPaddingY
        radius: Theme.radiusXl
        color: Theme.toastFill
        border.color: shell.amber
        opacity: 0
        visible: opacity > 0
        Behavior on opacity {
            enabled: Motion.enabled
            NumberAnimation { duration: Motion.normal; easing.type: Motion.easing }
        }
        Accessible.role: Accessible.AlertMessage
        Accessible.name: message
        Text {
            id: toastText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, problemToast.parent.width - Theme.toastTextInset)
            text: problemToast.message
            color: Theme.toastText
            font.pixelSize: Theme.typeLabel
            wrapMode: Text.WordWrap
        }
        MouseArea { anchors.fill: parent; onClicked: problemToast.opacity = 0 }
    }

    WelcomeScreen {
        objectName: "welcomeScreen"
        anchors.fill: parent
        visible: !projectBridge.projectOpen || projectBridge.recoveryChoice.active === true
        z: 10
    }

    // The shell's one tooltip (shell/Tip), above everything.
    TipLayer {
        anchors.fill: parent
        z: 200
        background: Theme.canvas
        border: shell.kit.lineStrong
        textColor: shell.textMain
        dimTextColor: shell.textDim
    }
}
