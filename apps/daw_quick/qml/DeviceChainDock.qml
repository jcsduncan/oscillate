pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: dock
    // Layout: a resize strip and the header across the top, the tab below.
    readonly property int resizeBarHeight: 4
    readonly property int resizeHitSlop: 3
    readonly property int resizeStep: 24
    readonly property int headerHeight: 40
    readonly property int pathDotSize: 10
    readonly property int tabPadding: 24
    readonly property int tabHeight: 26
    readonly property int contextPadding: 20
    readonly property int clipInset: 18
    readonly property int clipTextRightRoom: 20
    readonly property int clipTitleY: 28
    readonly property int clipSpanY: 60
    readonly property int clipNotesY: 86
    readonly property int clipActionsY: 122
    readonly property int clipActionWidth: 112
    readonly property int clipActionHeight: 32
    readonly property int unfreezeWidth: 110
    readonly property int unfreezeHeight: 26
    // The piano roll has the keyboard (Ctrl+A selects its notes), and the
    // roll itself rather than one of its controls holds it (the shell's
    // bare keys, Space and Delete among them, still apply).
    readonly property bool notesHaveKeyboard: pianoRoll.keyboardInside
    readonly property bool notesRollFocused: pianoRoll.activeFocus
    // The chain takes edits when stopped, and while playing when it holds only
    // built-in effects; re-read whenever playback or the chains change.
    readonly property bool chainEditable: {
        dock.bridge.insertChainEditEnabled
        dock.bridge.playbackActive
        dock.bridge.insertChains
        return dock.bridge.chain_edit_enabled(dock.pathId)
    }

    function returnSends() {
        if (!dock.track || dock.track.kind === "return" || dock.selectedTrack === -2)
            return undefined
        const sends = dock.track.sends || []
        const result = []
        dock.bridge.tracks.forEach(other => {
            if (other.kind !== "return")
                return
            const send = sends.find(item => (item.destinationId || item.returnId) === other.id
                && (item.purpose === undefined || item.purpose === "audible"))
            result.push({ returnId: other.id, name: other.name, gain: send ? send.gain : 0 })
        })
        return result.length > 0 ? result : undefined
    }
    objectName: "deviceChainDock"
    required property var bridge
    required property int selectedTrack
    required property string selectedClipId
    required property var currentRouteTrack
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    required property color warningColor
    // The selected path's colour (the kit's track palette, or the accent
    // for the master).
    property color pathColor: accentColor
    property int activeTab: 0
    // The shown tab's content fades in when the tab changes.
    property real tabReveal: 1
    onActiveTabChanged: if (Motion.enabled) tabFade.restart()
    NumberAnimation {
        id: tabFade
        target: dock; property: "tabReveal"; from: Theme.tabRevealFrom; to: 1
        duration: Motion.normal; easing.type: Easing.OutCubic
    }
    property string handledClipId: ""
    signal resizeRequested(int height)
    signal contextRequested()

    readonly property var chain: {
        const index = selectedTrack === -2 ? bridge.tracks.length : selectedTrack
        return index >= 0 && index < bridge.insertChains.length ? bridge.insertChains[index]
            : ({effects: [], sourceName: "", status: "Saved chain unavailable"})
    }
    readonly property var track: selectedTrack >= 0 && selectedTrack < bridge.tracks.length
        ? bridge.tracks[selectedTrack] : null
    readonly property string pathName: selectedTrack === -2 ? "Master"
        : track ? track.name : "No track selected"
    readonly property string pathId: selectedTrack === -2 ? "master" : (currentRouteTrack.id || "")
    readonly property string trackKind: selectedTrack === -2 ? "master" : (track ? track.kind : "")
    // Only a track that can hold clips (the projection's holdsClips, from its
    // kind) has Clip and Notes tabs; the master and buses have the Chain alone.
    readonly property bool holdsClips: selectedTrack !== -2 && !!track && track.holdsClips === true
    onHoldsClipsChanged: if (!holdsClips) activeTab = 0
    // "3 devices · MIDI → Master": what the path holds and where it goes.
    readonly property string pathSummary: {
        const devices = (chain.effects || []).length + (trackKind === "instrument" ? 1 : 0)
        const kind = trackKind === "instrument" ? "MIDI" : trackKind === "master" ? "output"
            : trackKind === "group" || trackKind === "return" ? "bus" : "audio"
        const destination = selectedTrack === -2 ? bridge.audioOutput.selectedAudioDeviceName
            : (currentRouteTrack.mainDestinationName || "Master")
        return devices + (devices === 1 ? " device" : " devices") + " · " + kind + " → " + destination
    }
    readonly property bool effectPath: selectedTrack === -2 || (track
        && (["audio", "group", "return"].includes(track.kind)
            || (track.kind === "instrument" && track.nativeSourceReady)))
    readonly property var selectedClip: {
        for (const item of bridge.tracks) {
            for (const clip of item.clips || []) {
                if (clip.id === selectedClipId)
                    return clip
            }
        }
        return null
    }
    readonly property string selectedClipEngineId: {
        for (const item of bridge.tracks) {
            if ((item.clips || []).some(clip => clip.id === selectedClipId))
                return item.nativeEngineId || ""
        }
        return ""
    }
    readonly property string selectedClipTrackId: {
        for (const item of bridge.tracks) {
            if ((item.clips || []).some(clip => clip.id === selectedClipId))
                return item.id || ""
        }
        return ""
    }
    function openSelectedClipTab() {
        if (!selectedClipId) {
            handledClipId = ""
            activeTab = 0
        } else if (selectedClip && handledClipId !== selectedClipId) {
            handledClipId = selectedClipId
            activeTab = selectedClip.kind === "midi" ? 2 : 1
        }
    }
    // The Notes tab for the selected clip, however the dock was left: the
    // piano roll for MIDI, Clay for audio (ADR-0081).
    function openNotes() {
        if (selectedClip?.kind === "midi" || selectedClip?.kind === "audio") {
            handledClipId = selectedClipId
            activeTab = 2
            // Opened notes take the keyboard, so Ctrl+A selects them.
            if (pianoRoll.visible)
                pianoRoll.forceActiveFocus()
        }
    }
    // Clips selected in the arrangement let go of the piano roll's notes.
    function clearNoteSelection() { pianoRoll.clearSelection() }
    function openChords() {
        if (selectedClip?.kind === "midi") {
            openNotes()
            pianoRoll.chordsOpen = true
        }
    }
    onSelectedClipIdChanged: openSelectedClipTab()
    onSelectedClipChanged: openSelectedClipTab()
    color: Theme.deviceDockFill
    Rectangle { width: parent.width; height: Theme.hairline; color: Theme.line }

    Rectangle {
        id: resizeBar
        objectName: "insertChainResizeHandle"
        x: 0; y: 0; width: parent.width; height: dock.resizeBarHeight
        z: 2
        color: resizeArea.containsMouse || resizeArea.pressed || activeFocus ? Theme.resizeHandleActive : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: "Resize device chain dock"
        Keys.onUpPressed: dock.resizeRequested(dock.height + dock.resizeStep)
        Keys.onDownPressed: dock.resizeRequested(dock.height - dock.resizeStep)
        MouseArea {
            id: resizeArea
            anchors.fill: parent
            anchors.topMargin: -dock.resizeHitSlop
            anchors.bottomMargin: -dock.resizeHitSlop
            hoverEnabled: true
            cursorShape: Qt.SizeVerCursor
            property real startY: 0
            property int startHeight: 0
            onPressed: function(mouse) {
                startY = mapToItem(dock.parent, mouse.x, mouse.y).y
                startHeight = dock.height
            }
            onPositionChanged: function(mouse) {
                if (!pressed) return
                const nowY = mapToItem(dock.parent, mouse.x, mouse.y).y
                dock.resizeRequested(startHeight + Math.round(startY - nowY))
            }
        }
    }

    // Path colour, name and summary at the left; Chain / Clip / Notes and
    // Inspector at the right.
    Rectangle {
        id: header
        x: 0; y: 0; width: parent.width; height: dock.headerHeight
        color: "transparent"
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.hairline; color: Theme.rowDivider }
        Row {
            x: Theme.spaceXl
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceLg
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: dock.pathDotSize; height: dock.pathDotSize; radius: Theme.radiusSm
                color: dock.pathColor
            }
            Text {
                objectName: "insertChainPathName"
                anchors.verticalCenter: parent.verticalCenter
                text: dock.pathName
                color: dock.textColor; font.pixelSize: Theme.typeEmphasis; font.weight: Font.DemiBold
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: dock.selectedTrack === -2 || dock.track ? dock.pathSummary : ""
                color: dock.dimColor; font.pixelSize: Theme.typeLabel
            }
        }
        Row {
            anchors.right: contextButton.left
            anchors.rightMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXxs
            Repeater {
                model: ["Chain", "Clip", "Notes"]
                delegate: Rectangle {
                    id: tab
                    required property string modelData
                    required property int index
                    objectName: "dockTab" + modelData
                    readonly property bool available: index === 0 || dock.holdsClips
                        && ((index === 1 && dock.selectedClip !== null)
                            || (index === 2 && (dock.selectedClip?.kind === "midi" || dock.selectedClip?.kind === "audio")))
                    // Why a tab is off, for its tooltip.
                    readonly property string reason: dock.holdsClips ? "Select a clip first"
                        : (dock.selectedTrack === -2 ? "The master" : "This track") + " holds no clips or notes"
                    width: tabText.implicitWidth + dock.tabPadding; height: dock.tabHeight
                    radius: Theme.radiusLg
                    color: dock.activeTab === index ? Theme.rowHighlight : "transparent"
                    border.color: activeFocus ? dock.accentColor : "transparent"
                    opacity: available ? 1 : Theme.inactiveOpacity
                    activeFocusOnTab: available
                    Accessible.role: Accessible.PageTab
                    Accessible.name: index === 1 ? "Clip detail tab"
                        : index === 2 ? (dock.selectedClip?.kind === "audio" ? "Clay notes tab" : "Piano roll tab")
                        : "Device chain tab"
                    Keys.onReturnPressed: if (available) dock.activeTab = index
                    Keys.onSpacePressed: if (available) dock.activeTab = index
                    Text {
                        id: tabText
                        anchors.centerIn: parent
                        text: tab.modelData
                        color: dock.activeTab === tab.index ? dock.textColor : dock.dimColor
                        font.pixelSize: Theme.typeLabel
                        font.weight: dock.activeTab === tab.index ? Font.DemiBold : Font.Normal
                    }
                    Tip { text: (tab.modelData) + " — " + (tab.index === 0 ? "The track's macros, MIDI effects, instrument and effects" : !tab.available ? tab.reason : tab.index === 1 ? "The selected clip" : dock.selectedClip?.kind === "audio" ? "Edit the notes inside the audio (Clay)" : "Edit the selected MIDI clip's notes") }
                    MouseArea { id: tabArea; anchors.fill: parent; hoverEnabled: true
                                enabled: tab.available; onClicked: dock.activeTab = tab.index }
                }
            }
        }
        Rectangle {
            id: contextButton
            objectName: "deviceContextButton"
            anchors.right: parent.right; anchors.rightMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            width: contextText.implicitWidth + dock.contextPadding; height: dock.tabHeight; radius: Theme.radiusLg
            color: "transparent"
            border.color: activeFocus ? dock.accentColor : Theme.fieldEdge
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Open the inspector"
            Keys.onReturnPressed: dock.contextRequested()
            Keys.onSpacePressed: dock.contextRequested()
            Text { id: contextText; anchors.centerIn: parent; text: "Inspector"; color: Theme.textSoft; font.pixelSize: Theme.typeLabel }
            Tip { text: "Inspector — Routing, analysis and details for the selection" }
            MouseArea { id: inspectorArea; anchors.fill: parent; hoverEnabled: true
                        onClicked: dock.contextRequested() }
        }
    }

    // The track's macros head its chain (ADR-0059); the master has none.
    MacroStrip {
        id: macroStrip
        visible: dock.activeTab === 0 && dock.track !== null && dock.selectedTrack !== -2
        opacity: dock.tabReveal
        x: 0; y: header.y + header.height
        height: dock.height - y
        bridge: dock.bridge
        trackId: dock.track ? dock.track.id : ""
        lineColor: dock.lineColor
        textColor: dock.textColor
        dimColor: dock.dimColor
        accentColor: dock.accentColor
    }
    // MIDI effects play into a built-in synth, after the macros (ADR-0059).
    NoteEffectStrip {
        id: noteEffectStrip
        x: macroStrip.visible ? macroStrip.width : 0; y: header.y + header.height
        opacity: dock.tabReveal
        Behavior on x { enabled: Motion.enabled; NumberAnimation { duration: Motion.normal; easing.type: Easing.OutCubic } }
        height: dock.height - y
        bridge: dock.bridge
        trackId: dock.activeTab === 0 && dock.track && dock.selectedTrack !== -2 ? dock.track.id : ""
        lineColor: dock.lineColor
        textColor: dock.textColor
        dimColor: dock.dimColor
    }
    InsertChainPanel {
        id: chainPanel
        visible: dock.activeTab === 0
        opacity: dock.tabReveal
        Behavior on x { enabled: Motion.enabled; NumberAnimation { duration: Motion.normal; easing.type: Easing.OutCubic } }
        x: (macroStrip.visible ? macroStrip.width : 0)
           + (noteEffectStrip.visible ? noteEffectStrip.width : 0)
        y: header.y + header.height
        width: dock.width - x; height: dock.height - y
        horizontalLayout: true
        chain: dock.chain
        pathName: dock.pathName
        pathId: dock.pathId
        trackKind: dock.trackKind
        // One knob per return track; a send exists only once its knob is
        // turned up. Returns and the master have no Sends strip.
        sends: dock.returnSends()
        outputName: dock.selectedTrack === -2 ? dock.bridge.audioOutput.selectedAudioDeviceName
            : (dock.currentRouteTrack.mainDestinationName || "Master")
        effectPath: dock.effectPath
        editable: (dock.effectPath || Boolean(dock.chain.sourcePlugin?.pluginId)) && dock.chainEditable
        playbackActive: dock.bridge.playbackActive
        playbackPaused: dock.bridge.playbackPaused
        playbackStatus: dock.bridge.playbackStatus
        playbackApplication: dock.bridge.playbackState.application
        routedGraphCurrent: dock.bridge.routedGraphCurrent
        playbackRevisionInactive: dock.bridge.playbackRevisionInactive
        vst3Editor: dock.bridge.vst3Editor.vst3InsertEditor
        vst3Available: dock.bridge.vst3Browser.vst3BrowserAvailable
        vst3Busy: dock.bridge.vst3Browser.vst3BrowserBusy
        vst3Status: dock.bridge.vst3Browser.vst3BrowserStatus
        vst3Entries: dock.bridge.vst3Browser.vst3BrowserEntries
        panelColor: dock.panelColor
        raisedColor: dock.raisedColor
        lineColor: dock.lineColor
        textColor: dock.textColor
        dimColor: dock.dimColor
        accentColor: dock.accentColor
        warningColor: dock.warningColor
    }

    // An audio clip's Clip tab is the docked clip editor (ADR-0053).
    ClipEditor {
        visible: dock.activeTab === 1 && !!dock.selectedClip && dock.selectedClip.kind === "audio"
        opacity: dock.tabReveal
        x: 0; y: header.y + header.height
        width: dock.width; height: dock.height - y
        bridge: dock.bridge
        clipId: visible ? dock.selectedClip.id : ""
        clip: dock.selectedClip
        trackColor: dock.pathColor
        lineColor: dock.lineColor
        textColor: dock.textColor
        dimColor: dock.dimColor
        accentColor: dock.accentColor
        amberColor: dock.warningColor
        monoFamily: "JetBrains Mono"
    }
    Item {
        id: clipDetail
        visible: dock.activeTab === 1 && !(dock.selectedClip && dock.selectedClip.kind === "audio")
        x: dock.clipInset; y: header.y + header.height + dock.clipInset
        width: parent.width - 2 * dock.clipInset; height: parent.height - y - dock.clipInset
        readonly property real startBar: dock.selectedClip
            ? Number(dock.selectedClip.start) * dock.bridge.timelineBars + 1 : 0
        readonly property real lengthBars: dock.selectedClip
            ? Number(dock.selectedClip.length) * dock.bridge.timelineBars : 0
        Text {
            text: dock.selectedClip ? (dock.selectedClip.kind === "audio" ? "AUDIO CLIP" : "MIDI CLIP") : "NO CLIP SELECTED"
            color: dock.accentColor; font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.tracking2xl
        }
        Text {
            y: dock.clipTitleY; width: parent.width - dock.clipTextRightRoom
            text: dock.selectedClip ? (dock.selectedClip.kind === "audio" ? "Audio region" : "MIDI pattern")
                : "Select a clip in the arrangement to inspect it."
            color: dock.textColor; font.pixelSize: Theme.clipDetailTitleSize; font.bold: true; elide: Text.ElideRight
        }
        Text {
            y: dock.clipSpanY; width: parent.width - dock.clipTextRightRoom
            text: dock.selectedClip ? "BAR " + clipDetail.startBar.toFixed(2) + "  ·  "
                + clipDetail.lengthBars.toFixed(2) + " BARS  ·  " + dock.selectedClip.timebase.toUpperCase()
                : ""
            color: dock.dimColor; font.pixelSize: Theme.typeBody; wrapMode: Text.WordWrap
        }
        Text {
            y: dock.clipNotesY; width: parent.width - dock.clipTextRightRoom
            text: dock.selectedClip && dock.selectedClip.kind === "midi"
                ? (dock.selectedClip.notes || []).length + " notes · Open the Notes tab to edit."
                : dock.selectedClip ? "Source analysis, warp and envelopes are in the Inspector." : ""
            color: dock.dimColor; font.pixelSize: Theme.typeBody
        }
        Row {
            y: dock.clipActionsY
            spacing: Theme.spaceMd
            visible: dock.selectedClip && dock.selectedClip.kind === "audio"
            Repeater {
                model: [
                    {label: "DUPLICATE", action: "duplicate"},
                    {label: "SPLIT", action: "split"},
                    {label: "WAVEFORM", action: "waveform"},
                    {label: "TIMEBASE", action: "timebase"}
                ]
                delegate: Rectangle {
                    id: clipAction
                    required property var modelData
                    width: dock.clipActionWidth; height: dock.clipActionHeight; radius: Theme.radiusLg
                    color: dock.raisedColor
                    border.color: activeFocus ? dock.accentColor : dock.lineColor
                    activeFocusOnTab: visible
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.action === "duplicate" ? "Duplicate selected audio clip"
                        : modelData.action === "split" ? "Split selected audio clip at playhead"
                        : modelData.action === "waveform" ? "Rebuild selected clip waveform"
                        : "Toggle selected clip timebase"
                    function activate() {
                        if (!dock.selectedClipId) return
                        if (modelData.action === "duplicate")
                            dock.bridge.duplicate_audio_clip_after(dock.selectedClipId)
                        else if (modelData.action === "split")
                            dock.bridge.split_audio_clip_at_playhead(dock.selectedClipId)
                        else if (modelData.action === "waveform")
                            dock.bridge.rebuild_clip_waveform(dock.selectedClipId)
                        else
                            dock.bridge.toggle_audio_clip_timebase(dock.selectedClipId)
                    }
                    Keys.onReturnPressed: activate()
                    Keys.onSpacePressed: activate()
                    Text { anchors.centerIn: parent; text: clipAction.modelData.label; color: dock.textColor; font.pixelSize: Theme.typeSmall; font.bold: true }
                    Tip { text: clipAction.Accessible.name }
                    MouseArea { id: clipActionArea; anchors.fill: parent; hoverEnabled: true
                                onClicked: clipAction.activate() }
                }
            }
        }
    }
    PianoRoll {
        id: pianoRoll
        visible: dock.activeTab === 2 && dock.selectedClip?.kind === "midi"
        opacity: dock.tabReveal
        x: 0; y: header.y + header.height
        width: dock.width; height: dock.height - y
        bridge: dock.bridge
        selectedClipId: dock.selectedClipId
        selectedClip: dock.selectedClip
        drumEngineId: dock.selectedClipEngineId
        // Knock names its lanes by each pad's sound on the track, so they follow its edits.
        drumLanes: dock.bridge.revision >= 0
            ? dock.bridge.native_synth_note_lanes(dock.selectedClipEngineId, dock.selectedClipTrackId) : []
        raisedColor: dock.raisedColor
        lineColor: dock.lineColor
        mainTextColor: dock.textColor
        dimTextColor: dock.dimColor
        accentColor: dock.accentColor
        amberColor: dock.warningColor
    }
    // An audio clip's Notes tab is Clay (ADR-0081).
    ClayEditor {
        visible: dock.activeTab === 2 && dock.selectedClip?.kind === "audio"
        opacity: dock.tabReveal
        x: 0; y: header.y + header.height
        width: dock.width; height: dock.height - y
        clay: dock.bridge.clay
        bridge: dock.bridge
        clipId: visible ? dock.selectedClipId : ""
    }
    // A frozen track plays its render (ADR-0059): its devices are greyed and
    // take no edits until it is unfrozen.
    Rectangle {
        id: frozenCover
        objectName: "frozenChainCover"
        visible: dock.activeTab === 0 && dock.track !== null && dock.track !== undefined
                 && dock.track.frozen === true && dock.selectedTrack !== -2
        // Frost settles over the chain as the track freezes.
        opacity: visible ? 1 : 0
        Behavior on opacity { enabled: Motion.enabled; NumberAnimation { duration: Motion.slow } }
        z: 20
        x: 0; y: header.y + header.height
        width: dock.width; height: dock.height - y
        color: Theme.frozenCover
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
        Column {
            anchors.centerIn: parent
            spacing: Theme.spaceMd
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "❄  FROZEN"
                color: Theme.frozenTitle
                font.pixelSize: Theme.typeEmphasis
                font.bold: true
                font.letterSpacing: Theme.tracking3xl
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "The track plays its render. Unfreeze it first to edit its devices."
                color: dock.dimColor
                font.pixelSize: Theme.typeBody
            }
            Rectangle {
                objectName: "frozenChainUnfreeze"
                anchors.horizontalCenter: parent.horizontalCenter
                width: dock.unfreezeWidth; height: dock.unfreezeHeight; radius: Theme.radiusLg
                color: unfreezeArea.containsMouse ? Theme.unfreezeHover : Theme.unfreezeFill
                border.color: Theme.unfreezeEdge
                activeFocusOnTab: parent.parent.visible
                Accessible.role: Accessible.Button
                Accessible.name: "Unfreeze the track"
                Keys.onReturnPressed: dock.bridge.freezeCommands.unfreeze(dock.track.id)
                Keys.onSpacePressed: dock.bridge.freezeCommands.unfreeze(dock.track.id)
                Text {
                    anchors.centerIn: parent
                    text: "UNFREEZE"
                    color: Theme.unfreezeText
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                    font.letterSpacing: Theme.trackingMd
                }
                Tip { text: "Unfreeze — Play the track's devices again · Undo freezes it back" }
                MouseArea {
                    id: unfreezeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: dock.bridge.freezeCommands.unfreeze(dock.track.id)
                }
            }
        }
    }
}
