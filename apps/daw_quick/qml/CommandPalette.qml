pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The command palette (Ctrl+K): type a few letters of anything and press
// Enter. It searches the app's actions (with their shortcuts), the tracks
// (go to one), every built-in instrument and effect (on the selected track,
// as the browser's double-click does) and the open device's presets.
// Ranking is fuzzy ("adpu" finds "Add Pump") and recent picks come first
// (CommandPalette in C++). Keyboard first: arrows, Enter, Escape.
Item {
    id: palette
    objectName: "commandPalette"
    required property var host
    required property var bridge
    readonly property var ranker: bridge.commandPalette
    property bool open: false
    visible: open
    z: 40
    // At most this many results show at once.
    readonly property int maxResults: 12
    readonly property int cardTopMin: 48
    readonly property real cardTopShare: 0.14
    readonly property int cardMaxWidth: 620
    readonly property int cardWindowInset: 48
    readonly property int cardBottomPadding: 22
    readonly property real cardRadius: 12
    readonly property int inputX: 14
    readonly property int inputY: 10
    readonly property int inputHeight: 40
    readonly property int queryX: 28
    readonly property int rowHeight: 34
    readonly property int emptyListHeight: 36
    readonly property real rowRadius: 7
    readonly property int iconX: 10
    readonly property int iconSize: 20
    readonly property int titleX: 40
    readonly property int titleGap: 20
    readonly property int detailMaxWidth: 240

    function show() {
        query.text = ""
        current = 0
        open = true
        refresh()
        query.forceActiveFocus()
    }
    function hide() {
        open = false
    }

    // The browser's catalogue and action rules, for the selected track.
    BrowserCatalogue {
        id: catalogue
        bridge: palette.bridge
        selectedTrack: palette.host.selectedTrackData
    }

    // Every action: key, title, detail (its shortcut), keywords, and when
    // it can run. `run` stays here; only the rest goes to the ranker.
    function actions() {
        const shell = palette.host
        const bridge = palette.bridge
        const track = shell.selectedTrackData
        const open = bridge.projectOpen
        const clip = shell.selectedClip
        const list = [
            { id: "play", title: "Play / pause", shortcut: "Space", keywords: "start transport",
              enabled: bridge.playbackEnabled, run: () => bridge.toggle_pause() },
            { id: "stop", title: "Stop", keywords: "transport", enabled: open,
              run: () => bridge.stop_playback() },
            { id: "loop", title: "Loop on / off", shortcut: "Ctrl+L", keywords: "cycle repeat",
              enabled: open, run: () => bridge.loopTransport.set_enabled(!bridge.loopTransport.enabled) },
            { id: "marker", title: "Add marker at the playhead", shortcut: "Ctrl+M", keywords: "locator",
              enabled: open, run: () => bridge.add_marker_at_playhead() },
            { id: "marker-next", title: "Next marker", shortcut: "]", enabled: open,
              run: () => bridge.jump_to_marker(1) },
            { id: "marker-previous", title: "Previous marker", shortcut: "[", enabled: open,
              run: () => bridge.jump_to_marker(-1) },
            { id: "undo", title: "Undo", shortcut: "Ctrl+Z", enabled: open, run: () => bridge.undo() },
            { id: "redo", title: "Redo", shortcut: "Ctrl+Shift+Z", enabled: open, run: () => bridge.redo() },
            { id: "history", title: "Show history", shortcut: "Ctrl+H", keywords: "undo list steps",
              enabled: open, run: () => shell.historyPanelOpen = true },
            { id: "save", title: "Save", shortcut: "Ctrl+S", enabled: open, run: () => bridge.save() },
            { id: "versions", title: "Saved versions", keywords: "named revisions snapshot restore",
              enabled: open, run: () => { shell.revisionPanelOpen = true; bridge.refresh_named_revisions() } },
            { id: "export", title: "Export…", shortcut: "Ctrl+Shift+E", keywords: "render bounce master wav flac mp3",
              enabled: open, run: () => shell.openExportDialog() },
            { id: "mix-check", title: shell.currentView === "mixcheck" ? "Close Mix Check" : "Mix Check",
              shortcut: "Ctrl+Shift+M",
              keywords: "analyse analyze problems spectrum loudness lufs mono resonance rumble mixdown master",
              enabled: open, run: () => shell.toggleMixCheck() },
            { id: "arrange-from-loop",
              title: shell.currentView === "arrangeloop" ? "Close Arrange from a loop" : "Arrange from the loop",
              keywords: "arrangement structure intro outro breakdown drop dj tool song generate sections",
              enabled: open, run: () => shell.toggleArrangeFromLoop() },
            { id: "chords", title: "Chords in this clip",
              keywords: "harmony chord progression mood shape pedal tone drone voicing key lab comp rhythm write",
              enabled: open && clip !== null && clip.kind === "midi", run: () => shell.editClipChords() },
            { id: "clay-notes", title: "Edit notes (Clay)",
              keywords: "melodyne pitch correct tune vocal audio notes formant harmonise to midi",
              enabled: open && clip !== null && clip.kind === "audio", run: () => shell.editClipNotes() },
            { id: "groove-from-record", title: "Groove from a record…",
              keywords: "swing feel timing shuffle extract learn take hats kick reference pool",
              enabled: open, run: () => bridge.grooveCapture.open = true },
            { id: "import-audio", title: "Import audio", shortcut: "Ctrl+I", keywords: "wav file sample",
              enabled: bridge.imports.importEnabled, run: () => bridge.imports.choose_import_audio() },
            { id: "import-midi", title: "Import MIDI", shortcut: "Ctrl+Shift+I", keywords: "smf file",
              enabled: bridge.imports.importEnabled, run: () => bridge.imports.choose_import_midi() },
            { id: "new-synth", title: "New instrument track", shortcut: "Ctrl+T", keywords: "add midi synth",
              enabled: open, run: () => bridge.trackCommands.add_synth_track() },
            { id: "new-audio", title: "New audio track", keywords: "add", enabled: open,
              run: () => bridge.trackCommands.add_audio_track() },
            { id: "new-group", title: "New group bus", keywords: "add bus folder", enabled: open,
              run: () => bridge.trackCommands.add_bus_track("group") },
            { id: "new-return", title: "New return track", keywords: "add send bus reverb", enabled: open,
              run: () => bridge.trackCommands.add_bus_track("return") },
            { id: "duplicate", title: "Duplicate", shortcut: "Ctrl+D", keywords: "copy clip notes track",
              enabled: open, run: () => shell.duplicateSelection() },
            { id: "delete", title: "Delete the selection", shortcut: "Delete", keywords: "remove clip notes",
              enabled: open && (clip !== null || bridge.selectedNoteIds.length > 0),
              run: () => shell.deleteSelection() },
            { id: "split", title: "Split the clip at the playhead", shortcut: "Ctrl+E", keywords: "cut",
              enabled: open && clip !== null, run: () => shell.splitSelection() },
            { id: "quantize", title: "Quantize the clip", shortcut: "Ctrl+U", keywords: "grid timing",
              enabled: open && clip !== null && clip.kind === "midi", run: () => shell.quantizeSelection() },
            { id: "copy", title: "Copy", shortcut: "Ctrl+C", enabled: open, run: () => bridge.copy_selection() },
            { id: "cut", title: "Cut", shortcut: "Ctrl+X", enabled: open, run: () => bridge.cut_selection() },
            { id: "paste", title: "Paste at the playhead", shortcut: "Ctrl+V", enabled: open,
              run: () => bridge.paste_at_playhead() },
            { id: "mute", title: track && track.muted ? "Unmute the track" : "Mute the track", shortcut: "M",
              enabled: track !== null,
              run: () => bridge.trackCommands.set_track_muted(track.id, !track.muted) },
            { id: "solo", title: track && track.soloed ? "Unsolo the track" : "Solo the track", shortcut: "S",
              enabled: track !== null,
              run: () => bridge.trackCommands.set_track_soloed(track.id, !track.soloed) },
            { id: "duplicate-track", title: "Duplicate the track", keywords: "copy", enabled: track !== null,
              run: () => bridge.duplicate_track(track.id) },
            { id: "trace", title: bridge.performanceTrace.recording ? "Stop and save the performance trace"
                                                                    : "Start a performance trace",
              keywords: "profile perf timing cpu jank glitch xrun", enabled: true,
              run: () => bridge.performanceTrace.recording ? bridge.performanceTrace.stop_and_save()
                                                           : bridge.performanceTrace.start() },
            { id: "zoom-in", title: "Zoom in", shortcut: "+", enabled: open, run: () => shell.zoomIn() },
            { id: "zoom-out", title: "Zoom out", shortcut: "−", enabled: open, run: () => shell.zoomOut() },
            { id: "view-arrange", title: "Show the arrangement", keywords: "timeline view", enabled: open,
              run: () => shell.currentView = "arrange" },
            { id: "view-mixer", title: "Show the mixer", keywords: "faders view loudness", enabled: open,
              run: () => shell.currentView = "mixer" },
            { id: "master-chain", title: "Edit the master effects", keywords: "master chain limiter",
              enabled: open,
              run: () => { shell.selectedTrack = -2; shell.currentView = "arrange" } },
            { id: "plugins", title: "Open the VST3 plug-ins", shortcut: "Ctrl+Shift+P", keywords: "browser vst",
              enabled: open, run: () => shell.openPluginBrowser(false) },
            { id: "browser", title: "Open the browser", keywords: "instruments effects presets", enabled: open,
              run: () => shell.openPluginBrowser(true) },
            { id: "keyboard", title: "Computer keyboard on / off", shortcut: "Ctrl+Shift+K",
              keywords: "play notes typing", enabled: open, run: () => bridge.computerKeyboard.toggle() },
            { id: "keyboard-in-key", title: bridge.computerKeyboard.inKey ? "Computer keyboard: play every note"
                                                                          : "Computer keyboard: play in key",
              keywords: "scale lock snap typing", enabled: open, run: () => bridge.computerKeyboard.toggle_in_key() },
            { id: "mono", title: bridge.masterSection && bridge.masterSection.monoListen
                ? "Listen in stereo" : "Listen in mono", keywords: "master phase check",
              enabled: open && Boolean(bridge.masterSection),
              run: () => bridge.masterSection.monoListen = !bridge.masterSection.monoListen },
            { id: "loudness-reset", title: "Reset integrated loudness", keywords: "lufs master meter",
              enabled: open && Boolean(bridge.masterSection), run: () => bridge.masterSection.reset_loudness() },
            { id: "media", title: "Managed media", keywords: "missing files repair", enabled: open,
              run: () => shell.mediaPanelOpen = true },
            { id: "audio", title: "Audio output settings", keywords: "device buffer latency jack pipewire",
              enabled: true, run: () => shell.audioMenuOpen = true },
            { id: "shortcuts", title: "Keyboard shortcuts", shortcut: "?", keywords: "help keys",
              enabled: true, run: () => shell.shortcutsOpen = true },
            { id: "reduced-motion", title: bridge.commandPalette && bridge.commandPalette.reducedMotion
                ? "Turn animations on" : "Reduce motion (turn animations off)",
              keywords: "animation accessibility settings transitions",
              enabled: Boolean(bridge.commandPalette),
              run: () => bridge.commandPalette.reducedMotion = !bridge.commandPalette.reducedMotion }
        ]
        return list.map(action => Object.assign({}, action, {
            key: "action:" + action.id, kind: "action", detail: action.shortcut || "" }))
    }
    function tracks() {
        return (palette.bridge.tracks || []).map((track, index) => ({
            key: "track:" + track.id, kind: "track",
            title: track.name && track.name.length > 0 ? track.name : "Track " + (index + 1),
            detail: "Go to track", keywords: "track " + (track.kind || "") + " " + (track.nativeEngineName || ""),
            enabled: true,
            run: () => {
                palette.host.currentView = "arrange"
                palette.host.selectedClipId = ""
                palette.host.selectedTrack = index
            }
        }))
    }
    function devices() {
        return catalogue.allCards.filter(card => card.source !== "vst3" || card.entry.state === "ready")
            .map(card => ({
                key: "device:" + card.key, kind: card.instrument ? "instrument" : "effect",
                title: catalogue.isDevice(card) ? card.kind + " · " + card.engineName
                    : card.presetName || card.name,
                detail: catalogue.primaryLabel(card),
                keywords: [card.engineName, card.kind, card.category, card.tag, card.section || "",
                           card.keywords || ""].join(" "),
                enabled: catalogue.primaryEnabled(card),
                run: () => catalogue.activate(card)
            }))
    }
    function presets() {
        const controller = palette.bridge.nativeSynthController || palette.bridge.nativeEffectController
        if (!controller)
            return []
        return (controller.presets || []).map(preset => ({
            key: "preset:" + controller.engine_id + ":" + preset.name, kind: "preset",
            title: preset.name, detail: "Load into " + controller.engine_name,
            keywords: "preset " + (preset.category || "") + " " + controller.engine_name,
            enabled: true, run: () => controller.apply_preset(preset.index)
        }))
    }

    property var entries: []
    property var results: []
    property int current: 0
    // Runners by key, so only plain data crosses into C++.
    property var runners: ({})
    function refresh() {
        if (!open)
            return
        const all = actions().concat(presets(), tracks(), devices())
        const runs = {}
        entries = all.map(entry => {
            runs[entry.key] = entry.run
            return { key: entry.key, kind: entry.kind, title: entry.title, detail: entry.detail,
                     keywords: entry.keywords || "", enabled: entry.enabled !== false }
        })
        runners = runs
        search()
    }
    function search() {
        results = ranker ? ranker.search(entries, query.text, maxResults) : entries.slice(0, maxResults)
        current = Math.max(0, Math.min(current, results.length - 1))
    }
    function runAt(index) {
        const entry = results[index]
        if (!entry || !entry.enabled)
            return
        const run = runners[entry.key]
        hide()
        if (ranker)
            ranker.note_used(entry.key)
        if (run)
            run()
    }

    Rectangle {
        id: scrim
        anchors.fill: parent
        color: Theme.paletteScrim
        Reveal { target: scrim; duration: Motion.fast }
        MouseArea { anchors.fill: parent; onClicked: palette.hide() }
    }
    Rectangle {
        id: card
        objectName: "commandPaletteCard"
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(palette.cardTopMin, parent.height * palette.cardTopShare)
        width: Math.min(palette.cardMaxWidth, parent.width - palette.cardWindowInset)
        height: input.height + list.height + palette.cardBottomPadding
        radius: palette.cardRadius
        color: palette.host.panelRaised
        border.color: palette.host.line
        transformOrigin: Item.Top
        Reveal { target: card; fromScale: Motion.popScale; duration: Motion.fast }
        MouseArea { anchors.fill: parent }

        Item {
            id: input
            x: palette.inputX
            y: palette.inputY
            width: parent.width - 2 * palette.inputX
            height: palette.inputHeight
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "⌕"
                color: palette.host.textDim
                font.pixelSize: Theme.typeDisplay
            }
            TextInput {
                id: query
                objectName: "commandPaletteQuery"
                x: palette.queryX
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - palette.queryX
                color: palette.host.textMain
                font.pixelSize: Theme.typeHeading
                selectByMouse: true
                Accessible.role: Accessible.EditableText
                Accessible.name: "Search commands, tracks, instruments, effects and presets"
                onTextChanged: { palette.current = 0; palette.search() }
                Keys.onEscapePressed: palette.hide()
                Keys.onUpPressed: palette.current = Math.max(0, palette.current - 1)
                Keys.onDownPressed: palette.current = Math.min(palette.results.length - 1, palette.current + 1)
                Keys.onReturnPressed: palette.runAt(palette.current)
                Keys.onEnterPressed: palette.runAt(palette.current)
                Keys.onTabPressed: palette.current = Math.min(palette.results.length - 1, palette.current + 1)
                Text {
                    visible: query.text.length === 0
                    text: "Run a command, go to a track, add an instrument or effect…"
                    color: palette.host.textDim
                    font.pixelSize: Theme.typeHeading
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: Theme.hairline
                color: palette.host.line
            }
        }

        ListView {
            id: list
            objectName: "commandPaletteResults"
            x: Theme.spaceMd
            y: input.y + input.height + Theme.spaceSm
            width: parent.width - 2 * Theme.spaceMd
            height: Math.max(palette.emptyListHeight, Math.min(count, palette.maxResults) * palette.rowHeight)
            interactive: false
            model: palette.results
            currentIndex: palette.current
            delegate: Rectangle {
                id: row
                required property int index
                required property var modelData
                readonly property bool selected: index === palette.current
                width: ListView.view.width
                height: palette.rowHeight
                radius: palette.rowRadius
                color: selected ? Theme.paletteSelection : "transparent"
                opacity: modelData.enabled ? 1 : Theme.disabledControlOpacity
                Accessible.role: Accessible.Button
                Accessible.name: modelData.title + (modelData.detail ? ", " + modelData.detail : "")
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                Rectangle {
                    x: palette.iconX
                    anchors.verticalCenter: parent.verticalCenter
                    width: palette.iconSize
                    height: palette.iconSize
                    radius: Theme.controlRadius
                    color: ({ action: Theme.line, track: Theme.paletteTrackKind, instrument: Theme.paletteInstrumentKind,
                              effect: Theme.paletteEffectKind, preset: Theme.palettePresetKind })[row.modelData.kind] || Theme.line
                    Text {
                        anchors.centerIn: parent
                        text: ({ action: "›", track: "≡", instrument: "♪", effect: "◆", preset: "★" })[row.modelData.kind] || "›"
                        color: palette.host.textMain
                        font.pixelSize: Theme.typeBody
                    }
                }
                Text {
                    x: palette.titleX
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - palette.titleX - detail.width - palette.titleGap
                    elide: Text.ElideRight
                    text: row.modelData.title
                    color: palette.host.textMain
                    font.pixelSize: Theme.typeEmphasis
                }
                Text {
                    id: detail
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spaceLg
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, palette.detailMaxWidth)
                    elide: Text.ElideRight
                    text: row.modelData.detail || ""
                    color: row.modelData.kind === "action" ? palette.host.textMain : palette.host.textDim
                    font.pixelSize: row.modelData.kind === "action" ? Theme.typeSmall : Theme.typeBody
                    font.family: row.modelData.kind === "action" ? palette.host.kit.monoFamily : palette.host.kit.sansFamily
                }
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: row.modelData.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onEntered: palette.current = row.index
                    onClicked: palette.runAt(row.index)
                }
            }
            Text {
                visible: list.count === 0
                // The ListView's own box: its content item is empty here.
                parent: list
                anchors.centerIn: list
                text: "Nothing matches “" + query.text + "”"
                color: palette.host.textDim
                font.pixelSize: Theme.typeLabel
            }
        }
    }
}
