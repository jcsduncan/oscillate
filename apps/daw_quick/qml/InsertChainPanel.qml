pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: chainPanel
    objectName: "insertChainPanel"
    required property var chain
    required property string pathName
    required property string pathId
    required property string trackKind
    property string outputName: ""
    required property bool effectPath
    required property bool editable
    required property bool playbackActive
    required property bool playbackPaused
    required property string playbackStatus
    required property string playbackApplication
    required property bool routedGraphCurrent
    required property bool playbackRevisionInactive
    required property var vst3Editor
    required property bool vst3Available
    required property bool vst3Busy
    required property string vst3Status
    required property var vst3Entries
    required property color panelColor
    required property color raisedColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    required property color warningColor
    property bool horizontalLayout: false
    color: panelColor
    border.color: lineColor
    readonly property var effects: chain.effects || []
    // The bridge reports application for the whole project revision. A card must
    // not imply that its own processor has a separate acknowledgement.
    readonly property string projectApplicationState: !playbackActive ? "READY FOR PLAYBACK"
        : playbackPaused ? "PAUSED" : routedGraphCurrent ? "PROJECT REVISION ACTIVE"
        : playbackApplication === "pending" ? "PROJECT REVISION PENDING"
        : "PROJECT EDIT NOT ACTIVE"
    readonly property bool projectApplicationAlert: playbackRevisionInactive
        || (playbackActive && !playbackPaused && !routedGraphCurrent)
    property string selectedRecoverySlotId: ""
    property bool addMenuOpen: false
    // The instrument swap list, from the ⇄ on the instrument strip.
    property bool swapMenuOpen: false
    readonly property var pathTrack: (projectBridge.tracks || []).find(track => track.id === pathId) || null
    readonly property var selectedRecovery: selectedRecoverySlotId === "__source__"
        ? (chain.sourcePlugin || null)
        : effects.find(effect => effect.slotId === selectedRecoverySlotId
            && Boolean(effect.pluginId)) || null
    // In the dock, only a real instrument heads the chain; inputs and the
    // output are named in the dock header instead.
    readonly property bool sourceVisible: horizontalLayout
        ? trackKind === "instrument" : Boolean(chain.sourceName)
    readonly property bool nativeSynthSource: horizontalLayout && trackKind === "instrument"
        && (projectBridge.nativeSynthEngines || []).some(engine => engine.name === chain.sourceName)
    readonly property int sourceRowHeight: 48
    readonly property int rowHeight: 112
    // Strips run left to right: source, effects, drop zone, sends, output.
    readonly property int horizontalSourceWidth: 290
    readonly property int horizontalEffectWidth: 290
    readonly property int horizontalGap: 28
    readonly property int horizontalCardHeight: Math.max(minCardHeight, chainList.height - Theme.spaceXs)
    readonly property int horizontalEffectStart: sourceVisible
        ? horizontalSourceWidth + horizontalGap : 0
    readonly property int dropZoneWidth: effectPath ? dropZoneFullWidth : 0
    readonly property int horizontalDropX: horizontalEffectStart
        + effects.length * (horizontalEffectWidth + horizontalGap)
    // Sends appear once the track projection carries them.
    property var sends: undefined
    readonly property int sendStripWidth: sends !== undefined && sends !== null && sends.length > 0
        ? Math.max(minSendStripWidth, Theme.spaceXl + sends.length * sendColumnWidth) : 0
    readonly property int horizontalSendsX: horizontalDropX
        + (dropZoneWidth > 0 ? dropZoneWidth + horizontalGap : 0)
    readonly property int horizontalOutputX: horizontalSendsX
        + (sendStripWidth > 0 ? sendStripWidth + horizontalGap : 0)
    readonly property int outputWidth: horizontalLayout ? 0 : listOutputWidth
    readonly property color sourceAccent: chain.sourceAccent ? chain.sourceAccent
        : trackKind === "audio" ? Theme.chainNeutralAccent : accentColor
    readonly property real floatingEditorHeight: Window.window
        ? Math.min(floatingEditorMaxHeight, Window.window.height - floatingEditorWindowRoom) : floatingEditorFallbackHeight
    property bool dragActive: false
    property string dragSlotId: ""
    property string dragPathId: ""
    property int dragStartIndex: -1
    property int dragTargetIndex: -1
    property real dragPointerY: 0
    property real dragPointerX: 0
    // Strip and card geometry shared by the chain's parts.
    readonly property int minCardHeight: 150
    readonly property int dropZoneFullWidth: 168
    readonly property int minSendStripWidth: 150
    readonly property int sendColumnWidth: 62
    readonly property int listOutputWidth: 132
    readonly property int floatingEditorMaxHeight: 560
    readonly property int floatingEditorWindowRoom: 120
    readonly property int floatingEditorFallbackHeight: 520
    readonly property int actionButtonSize: 24
    readonly property int headerX: 17
    readonly property int headerY: 12
    readonly property int headerEffectRoom: 107
    readonly property int headerRoom: 34
    readonly property int countMinRoom: 200
    readonly property int countY: 14
    readonly property int addRight: 85
    readonly property int addWidth: 68
    readonly property real stripRadius: 11
    readonly property int stripHeaderHeight: 36
    readonly property int powerGlyphSize: 12
    readonly property int listInsetX: 13
    readonly property int listInsetY: 40
    readonly property int listBottomRoom: 76
    readonly property int dockInsetY: 14
    // Scrolling: pixels per wheel notch, and the edge band that scrolls a drag.
    readonly property int wheelStepPixels: 48
    readonly property int autoScrollMs: 30
    readonly property int autoScrollEdge: 22
    readonly property int autoScrollStep: 8
    readonly property int stripPad: 10
    readonly property int sourceDotSize: 16
    readonly property int stripTitleX: 34
    readonly property int stripTitleRoom: 120
    readonly property int sourceKindRight: 12
    readonly property int sourceKindOpenRoom: 40
    readonly property int buttonSlot: 32
    readonly property int stripButtonY: 6
    readonly property int headerDragRoom: 40
    readonly property int stripBodyY: 44
    readonly property int sourceBodyRoom: 52
    readonly property int stripNoteY: 48
    readonly property int stripNoteLines: 3
    readonly property int footerRise: 34
    readonly property int fxButtonRight: 114
    readonly property int fxButtonWidth: 44
    readonly property int linkButtonRight: 64
    readonly property int linkButtonWidth: 54
    readonly property int arrowOffsetX: 5
    readonly property int arrowRise: 10
    readonly property real arriveScale: 0.94
    readonly property int arriveDrop: 10
    readonly property int handleX: 36
    readonly property int reorderThreshold: 8
    readonly property real presetMaxShare: 0.4
    readonly property real presetRoomShare: 0.3
    readonly property int stripTextX: 40
    readonly property int stripKindY: 22
    readonly property int effectBodyRoom: 82
    readonly property int vst3NoteY: 52
    readonly property int footerTextRise: 26
    readonly property int footerActionsRoom: 132
    readonly property int footerActionsRise: 30
    readonly property int dropMarkerWidth: 3
    readonly property real dropMarkerRadius: 1
    readonly property real dropTextLineHeight: 1.2
    readonly property int sendsRowY: 48
    readonly property int outputTextY: 34
    readonly property int outputLines: 2
    readonly property int sendKnobWidth: 58
    readonly property int sendKnobHeight: 96
    readonly property int sendNameY: 44
    readonly property int sendValueY: 58
    readonly property int sendDialSize: 40
    readonly property int vst3EditorWidth: 536
    readonly property int sidechainRoom: 38
    readonly property int sidechainRise: 32
    readonly property int sidechainHeight: 30
    readonly property int recoveryY: 64
    readonly property int addMenuRight: 207
    readonly property int addMenuListY: 38
    readonly property int addMenuWidth: 190
    readonly property int addRowPitch: 33
    readonly property int addRowHeight: 29
    readonly property int swapMenuOffset: 40
    readonly property int swapMenuWidth: 210
    readonly property int swapRowPitch: 29
    readonly property int swapRowHeight: 26
    readonly property int swapHeadingRoom: 34
    readonly property int swapRowsY: 28
    readonly property int statusRise: 49
    readonly property int liveStatusRise: 32

    onPathIdChanged: {
        selectedRecoverySlotId = ""
        addMenuOpen = false
        swapMenuOpen = false
    }

    // Strips that arrive or move (ADR-0059 motion): each slot's index in the
    // last chain seen on this path. A new slot pops in; a moved one slides
    // from where it was. Another path, or the first look at one, animates
    // nothing.
    property var slotIndexes: ({})
    property string indexedPath: ""
    // slot id -> previous index, or -1 for a new slot; read by strips made
    // after the change as well as by those already there.
    property var stripMoves: ({})
    signal stripsMoved()
    onEffectsChanged: {
        const next = {}
        const moves = {}
        let moved = false
        const samePath = indexedPath === pathId
        effects.forEach((effect, index) => {
            next[effect.slotId] = index
            const before = slotIndexes[effect.slotId]
            if (!samePath)
                return
            if (before === undefined) {
                moves[effect.slotId] = -1
                moved = true
            } else if (before !== index) {
                moves[effect.slotId] = before
                moved = true
            }
        })
        slotIndexes = next
        indexedPath = pathId
        stripMoves = moves
        if (moved)
            stripsMoved()
    }

    function updateDropTarget(pointerY, pointerX) {
        dragPointerY = pointerY
        dragPointerX = pointerX === undefined ? dragPointerX : pointerX
        if (horizontalLayout) {
            const position = chainList.contentX + dragPointerX - horizontalEffectStart
            const column = Math.floor(position / (horizontalEffectWidth + horizontalGap))
            dragTargetIndex = Math.max(0, Math.min(effects.length - 1, column))
            return
        }
        const row = Math.floor((chainList.contentY + pointerY
            - (sourceVisible ? sourceRowHeight : 0)) / rowHeight)
        dragTargetIndex = Math.max(0, Math.min(effects.length - 1, row))
    }

    function finishDrag(commit) {
        const slot = dragSlotId
        const path = dragPathId
        const target = dragTargetIndex
        const started = dragStartIndex
        const active = dragActive
        dragActive = false
        dragSlotId = ""
        dragPathId = ""
        dragStartIndex = -1
        dragTargetIndex = -1
        if (!commit || !active || !editable || path !== pathId || target === started ||
                target < 0 || target >= effects.length ||
                effects[started]?.slotId !== slot)
            return
        projectBridge.move_effect_insert(path, slot, target)
    }

    component ChainActionButton: Rectangle {
        required property string label
        required property string actionName
        signal triggered()
        width: chainPanel.actionButtonSize
        height: chainPanel.actionButtonSize
        radius: Theme.radiusMd
        color: enabled ? chainPanel.panelColor : chainPanel.raisedColor
        border.color: activeFocus ? chainPanel.accentColor : chainPanel.lineColor
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: actionName
        Keys.onReturnPressed: triggered()
        Keys.onSpacePressed: triggered()
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: parent.enabled ? chainPanel.textColor : chainPanel.dimColor
            font.pixelSize: Theme.typeLabel
        }
        MouseArea {
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: parent.triggered()
        }
    }

    // The selected path's name; the dock header carries the rest.
    Text {
        objectName: "insertChainPathName"
        visible: !chainPanel.horizontalLayout
        x: chainPanel.headerX
        y: chainPanel.headerY
        width: parent.width - (chainPanel.effectPath ? chainPanel.headerEffectRoom : chainPanel.headerRoom)
        elide: Text.ElideRight
        text: chainPanel.pathName
        color: chainPanel.textColor
        font.pixelSize: Theme.typeEmphasis
        font.bold: true
    }
    Text {
        visible: !chainPanel.horizontalLayout
        x: chainPanel.headerX + Math.min(chainPanel.width - chainPanel.countMinRoom, pathNameMetrics.advanceWidth + Theme.spaceLg)
        y: chainPanel.countY
        text: chainPanel.effects.length === 0 ? "No effects yet"
            : chainPanel.effects.length + (chainPanel.effects.length === 1 ? " effect" : " effects")
        color: chainPanel.dimColor
        font.pixelSize: Theme.typeBody
    }
    TextMetrics {
        id: pathNameMetrics
        font.pixelSize: Theme.typeEmphasis
        font.bold: true
        text: chainPanel.pathName
    }
    ChainActionButton {
        objectName: "insertAddNative"
        x: parent.width - chainPanel.addRight
        y: Theme.spaceMd
        width: chainPanel.addWidth
        label: "+ FX"
        actionName: "Choose a native effect for selected track"
        // In the dock the drop zone at the chain's end opens the same menu.
        visible: chainPanel.effectPath && !chainPanel.horizontalLayout
        enabled: chainPanel.editable
        onTriggered: chainPanel.addMenuOpen = !chainPanel.addMenuOpen
    }

    // One strip of the chain: a coloured header (power, name, kind, open)
    // above a body, with a footer for structural actions.
    component StripFrame: Rectangle {
        id: frame
        property color accent: chainPanel.accentColor
        property bool lit: true
        radius: chainPanel.stripRadius
        color: Theme.stripFill
        border.color: lit ? Theme.stripEdge : chainPanel.warningColor
        clip: true
        Rectangle {
            x: Theme.hairline; y: Theme.hairline
            width: parent.width - 2 * Theme.hairline
            height: chainPanel.stripHeaderHeight
            radius: chainPanel.stripRadius - Theme.hairline
            color: Qt.rgba(frame.accent.r, frame.accent.g, frame.accent.b, Theme.stripHeaderAlpha)
        }
    }

    // A power button: lit in the strip's accent while the device processes.
    component PowerButton: Rectangle {
        id: power
        property color accent: chainPanel.accentColor
        property bool lit: true
        property string actionName: ""
        signal triggered()
        width: chainPanel.actionButtonSize
        height: chainPanel.actionButtonSize
        radius: width / 2
        color: "transparent"
        border.color: activeFocus ? chainPanel.textColor : lit ? accent : Theme.powerOffEdge
        opacity: enabled ? 1 : Theme.disabledControlOpacity
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Button
        Accessible.name: actionName
        Keys.onReturnPressed: triggered()
        Keys.onSpacePressed: triggered()
        Canvas {
            anchors.centerIn: parent
            width: chainPanel.powerGlyphSize
            height: chainPanel.powerGlyphSize
            // A power symbol: a stem through the gap of an open ring.
            readonly property real stemTop: 1
            readonly property real ringCentreY: 6.5
            readonly property real ringRadius: 4.3
            readonly property real ringGap: 0.28
            property color ink: power.lit ? power.accent : Theme.textFaint
            onInkChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = ink
                ctx.lineWidth = Theme.powerGlyphStroke
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.moveTo(width / 2, stemTop)
                ctx.lineTo(width / 2, height / 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.arc(width / 2, ringCentreY, ringRadius, -Math.PI * ringGap, Math.PI * (1 + ringGap), true)
                ctx.stroke()
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: power.triggered()
        }
    }

    Flickable {
        id: chainList
        objectName: "insertChainList"
        x: chainPanel.horizontalLayout ? Theme.spaceXl : chainPanel.listInsetX
        y: chainPanel.horizontalLayout ? chainPanel.dockInsetY : chainPanel.listInsetY
        width: parent.width - 2 * x
        height: parent.height - (chainPanel.horizontalLayout ? 2 * chainPanel.dockInsetY : chainPanel.listBottomRoom)
        contentWidth: Math.max(width, chainPanel.horizontalOutputX + chainPanel.outputWidth + Theme.spaceXs)
        contentHeight: height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
        WheelHandler {
            target: null
            onWheel: event => {
                const limit = Math.max(0, chainList.contentWidth - chainList.width)
                chainList.contentX = Math.max(0, Math.min(limit,
                    chainList.contentX - event.angleDelta.y / 120 * chainPanel.wheelStepPixels))
                event.accepted = true
            }
        }
        Timer {
            interval: chainPanel.autoScrollMs
            running: chainPanel.dragActive
            repeat: true
            onTriggered: {
                const edge = chainPanel.autoScrollEdge
                const pointer = chainPanel.dragPointerX
                const direction = pointer < edge ? -1
                    : pointer > chainList.width - edge ? 1 : 0
                if (direction === 0)
                    return
                const limit = Math.max(0, chainList.contentWidth - chainList.width)
                chainList.contentX = Math.max(0, Math.min(limit,
                    chainList.contentX + direction * chainPanel.autoScrollStep))
                chainPanel.updateDropTarget(chainPanel.dragPointerY, chainPanel.dragPointerX)
            }
        }

        // The instrument (or the path's input) heads the chain.
        StripFrame {
            objectName: "instrumentSourceCard"
            visible: chainPanel.sourceVisible
            x: 0
            y: 0
            width: chainPanel.horizontalSourceWidth
            height: chainPanel.horizontalCardHeight
            accent: chainPanel.sourceAccent
            lit: !chainPanel.chain.sourceStatus
            Rectangle {
                x: chainPanel.stripPad; y: chainPanel.stripPad
                width: chainPanel.sourceDotSize; height: chainPanel.sourceDotSize; radius: Theme.radiusMd
                color: chainPanel.sourceAccent
            }
            Text {
                x: chainPanel.stripTitleX
                y: chainPanel.stripPad
                width: parent.width - chainPanel.stripTitleRoom
                elide: Text.ElideRight
                text: (chainPanel.chain.sourceName || (chainPanel.trackKind === "audio"
                    ? "Audio clips" : chainPanel.trackKind === "instrument"
                    ? "Instrument" : chainPanel.pathId === "master"
                    ? "Master sum" : "Routed inputs")).toUpperCase()
                color: chainPanel.chain.sourceStatus ? chainPanel.warningColor : chainPanel.textColor
                font.pixelSize: Theme.typeLabel
                font.bold: true
                font.letterSpacing: Theme.trackingMd
            }
            Text {
                anchors.right: parent.right
                // Clear of the expand button and the ⇄ swap button.
                readonly property bool swapShown: chainPanel.horizontalLayout
                                                  && chainPanel.trackKind === "instrument"
                anchors.rightMargin: (chainPanel.nativeSynthSource ? chainPanel.sourceKindOpenRoom : chainPanel.sourceKindRight) + (swapShown ? chainPanel.buttonSlot : 0)
                y: chainPanel.headerY
                text: (chainPanel.chain.sourceKind || (chainPanel.trackKind === "instrument"
                    ? "Instrument" : "Input")).toUpperCase()
                color: chainPanel.sourceAccent
                font.pixelSize: Theme.typeMicro
                font.bold: true
                font.letterSpacing: Theme.trackingMd
            }
            // Replace the instrument with another built-in one (or drop a
            // browser card on this strip).
            ChainActionButton {
                objectName: "instrumentSourceSwap"
                x: parent.width - (chainPanel.nativeSynthSource ? 2 * chainPanel.buttonSlot : chainPanel.buttonSlot)
                y: chainPanel.stripButtonY
                label: "⇄"
                actionName: "Replace instrument"
                visible: chainPanel.horizontalLayout && chainPanel.trackKind === "instrument"
                enabled: chainPanel.pathId !== ""
                onTriggered: chainPanel.swapMenuOpen = !chainPanel.swapMenuOpen
            }
            Loader {
                anchors.fill: parent
                z: 6
                active: chainPanel.trackKind === "instrument" && chainPanel.pathTrack !== null
                sourceComponent: TrackDeviceDrop {
                    bridge: projectBridge
                    track: chainPanel.pathTrack
                    trackIndex: (projectBridge.tracks || []).indexOf(chainPanel.pathTrack)
                    accentColor: chainPanel.sourceAccent
                }
            }
            ChainActionButton {
                objectName: "instrumentSourceOpen"
                x: parent.width - chainPanel.buttonSlot
                y: chainPanel.stripButtonY
                label: "⤢"
                actionName: "Open " + (chainPanel.chain.sourceName || "instrument")
                    + " editor"
                visible: chainPanel.nativeSynthSource
                enabled: chainPanel.pathId !== ""
                onTriggered: projectBridge.open_native_synth_editor(chainPanel.pathId)
            }
            MouseArea {
                x: 0; y: 0
                width: parent.width - chainPanel.headerDragRoom; height: chainPanel.stripHeaderHeight
                visible: chainPanel.nativeSynthSource
                    || Boolean(chainPanel.chain.sourcePlugin?.pluginId)
                acceptedButtons: Qt.LeftButton
                onDoubleClicked: {
                    if (chainPanel.nativeSynthSource)
                        projectBridge.open_native_synth_editor(chainPanel.pathId)
                    else
                        projectBridge.vst3Editor.open_vst3_instrument_editor(chainPanel.pathId)
                }
            }
            DeviceStripBody {
                visible: chainPanel.nativeSynthSource && chainPanel.chain.sourceEngine !== undefined
                    && chainPanel.chain.sourceEngine !== ""
                x: chainPanel.stripPad
                y: chainPanel.stripBodyY
                width: parent.width - 2 * chainPanel.stripPad
                height: parent.height - chainPanel.sourceBodyRoom
                bridge: projectBridge
                pathId: visible ? chainPanel.pathId : ""
                slotId: "__source__"
                engineId: visible ? chainPanel.chain.sourceEngine : ""
                accent: chainPanel.sourceAccent
                playing: chainPanel.playbackActive && !chainPanel.playbackPaused
            }
            Text {
                visible: !chainPanel.nativeSynthSource
                x: Theme.spaceLg
                y: chainPanel.stripNoteY
                width: parent.width - 2 * Theme.spaceLg
                wrapMode: Text.WordWrap
                maximumLineCount: chainPanel.stripNoteLines
                elide: Text.ElideRight
                text: chainPanel.chain.sourceStatus
                    || (chainPanel.chain.sourcePlugin?.pluginId ? "VST3 instrument · double-click to open"
                        : chainPanel.trackKind === "audio" ? "Clips on this track feed the chain"
                        : chainPanel.pathId === "master" ? "Every track's output, summed"
                        : "Audio sent or routed here")
                color: chainPanel.chain.sourceStatus ? chainPanel.warningColor : chainPanel.dimColor
                font.pixelSize: Theme.typeBody
            }
            ChainActionButton {
                objectName: "instrumentSourceParameters"
                x: parent.width - chainPanel.fxButtonRight
                y: parent.height - chainPanel.footerRise
                width: chainPanel.fxButtonWidth
                label: "FX"
                actionName: "Show saved parameters and automation for instrument source "
                            + (chainPanel.chain.sourceName || "")
                visible: Boolean(chainPanel.chain.sourcePlugin?.pluginId)
                enabled: chainPanel.pathId !== ""
                onTriggered: projectBridge.vst3Editor.open_vst3_instrument_editor(chainPanel.pathId)
            }
            ChainActionButton {
                objectName: "instrumentSourceRelink"
                x: parent.width - chainPanel.linkButtonRight
                y: parent.height - chainPanel.footerRise
                width: chainPanel.linkButtonWidth
                label: "LINK"
                actionName: "Verify or relink saved VST3 instrument source"
                visible: Boolean(chainPanel.chain.sourcePlugin?.pluginId)
                enabled: chainPanel.editable && chainPanel.vst3Available
                onTriggered: chainPanel.selectedRecoverySlotId = "__source__"
            }
            Accessible.name: "Instrument source " + (chainPanel.chain.sourceName || "")
            Accessible.description: chainPanel.chain.sourceStatus || "Source precedes audio effects"
        }

        // Signal-flow arrows in the gaps between strips.
        Repeater {
            model: chainPanel.sourceVisible ? chainPanel.effects.length + 1 : chainPanel.effects.length
            delegate: Text {
                required property int index
                readonly property int stripIndex: chainPanel.sourceVisible ? index : index + 1
                x: chainPanel.horizontalEffectStart - chainPanel.horizontalGap
                    + stripIndex * (chainPanel.horizontalEffectWidth + chainPanel.horizontalGap) + chainPanel.arrowOffsetX
                y: chainPanel.horizontalCardHeight / 2 - chainPanel.arrowRise
                text: "→"
                color: Theme.flowArrow
                font.pixelSize: Theme.typeDisplay
            }
        }

        // Count-keyed so value edits never rebuild a strip (a knob mid-drag
        // keeps its gesture); each strip reads its slot's newest view.
        Repeater {
            model: chainPanel.effects.length
            delegate: StripFrame {
                id: effectCard
                objectName: "insertEffectCard"
                required property int index
                readonly property var modelData: chainPanel.effects[index] || ({})
                readonly property bool builtIn: modelData.nativeKind === "effect"
                readonly property color stripAccent: builtIn && modelData.accent
                    ? modelData.accent : modelData.pluginId ? Theme.chainNeutralAccent : chainPanel.accentColor
                x: chainPanel.horizontalEffectStart
                    + index * (chainPanel.horizontalEffectWidth + chainPanel.horizontalGap)
                y: 0
                width: chainPanel.horizontalEffectWidth
                height: chainPanel.horizontalCardHeight
                accent: stripAccent
                // Arrives with a short pop; a moved strip slides from its old place.
                transform: Translate { id: stripShift }
                function animateMove() {
                    const effect = chainPanel.effects[index]
                    const from = effect ? chainPanel.stripMoves[effect.slotId] : undefined
                    if (from === undefined || !Motion.enabled)
                        return
                    if (from < 0) {
                        stripArrive.restart()
                    } else {
                        stripShift.x = (from - index)
                            * (chainPanel.horizontalEffectWidth + chainPanel.horizontalGap)
                        stripSlide.restart()
                    }
                }
                Component.onCompleted: animateMove()
                Connections {
                    target: chainPanel
                    function onStripsMoved() { effectCard.animateMove() }
                }
                NumberAnimation {
                    id: stripSlide
                    target: stripShift; property: "x"; to: 0
                    duration: Motion.normal; easing.type: Easing.OutCubic
                }
                ParallelAnimation {
                    id: stripArrive
                    NumberAnimation { target: effectCard; property: "scale"; from: chainPanel.arriveScale; to: 1
                                      duration: Motion.normal; easing.type: Easing.OutBack }
                    NumberAnimation { target: stripShift; property: "y"; from: chainPanel.arriveDrop; to: 0
                                      duration: Motion.normal; easing.type: Easing.OutCubic }
                }
                lit: !modelData.missing
                opacity: chainPanel.dragActive && chainPanel.dragSlotId === modelData.slotId
                         ? Theme.draggedStripOpacity : modelData.enabled ? 1 : Theme.bypassedStripOpacity
                readonly property string applicationState: modelData.missing ? "MISSING PLUGIN"
                    : !modelData.enabled ? "BYPASSED"
                    : chainPanel.projectApplicationState
                function openEditor() {
                    if (effectCard.modelData.pluginId)
                        projectBridge.vst3Editor.open_vst3_insert_editor(chainPanel.pathId,
                                                              effectCard.modelData.slotId)
                    else if (effectCard.modelData.nativeKind === "effect")
                        projectBridge.open_native_effect_editor(chainPanel.pathId,
                                                                effectCard.modelData.slotId)
                }

                PowerButton {
                    objectName: "insertBypass"
                    x: Theme.spaceMd
                    y: chainPanel.stripButtonY
                    accent: effectCard.stripAccent
                    lit: Boolean(effectCard.modelData.enabled)
                    actionName: (effectCard.modelData.enabled ? "Bypass " : "Enable ")
                        + effectCard.modelData.name
                    visible: chainPanel.effectPath
                    enabled: chainPanel.editable
                    onTriggered: projectBridge.set_effect_insert_enabled(
                        chainPanel.pathId, effectCard.modelData.slotId,
                        !effectCard.modelData.enabled)
                }
                // The header is the drag grip; double-click opens the panel.
                MouseArea {
                    id: reorderHandle
                    objectName: "insertDragHandle"
                    x: chainPanel.handleX
                    y: 0
                    width: Math.max(0, parent.width - 2 * chainPanel.handleX)
                    height: chainPanel.stripHeaderHeight
                    enabled: true
                    cursorShape: chainPanel.effectPath && chainPanel.editable ? Qt.SizeHorCursor
                        : Qt.ArrowCursor
                    preventStealing: true
                    acceptedButtons: Qt.LeftButton
                    Accessible.role: Accessible.Button
                    Accessible.name: "Drag " + effectCard.modelData.name
                        + " to reorder; use Move earlier and Move later buttons with keyboard"
                    onDoubleClicked: effectCard.openEditor()
                    onPressed: mouse => {
                        if (!chainPanel.effectPath || !chainPanel.editable)
                            return
                        chainPanel.dragSlotId = effectCard.modelData.slotId
                        chainPanel.dragPathId = chainPanel.pathId
                        chainPanel.dragStartIndex = effectCard.index
                        chainPanel.dragTargetIndex = effectCard.index
                        const pointer = reorderHandle.mapToItem(chainList, mouse.x, mouse.y)
                        chainPanel.dragPointerY = pointer.y
                        chainPanel.dragPointerX = pointer.x
                    }
                    onPositionChanged: mouse => {
                        if (chainPanel.dragSlotId !== effectCard.modelData.slotId)
                            return
                        const pointer = reorderHandle.mapToItem(chainList, mouse.x, mouse.y)
                        if (!chainPanel.dragActive
                                && Math.abs(pointer.x - chainPanel.dragPointerX) >= chainPanel.reorderThreshold)
                            chainPanel.dragActive = true
                        if (chainPanel.dragActive)
                            chainPanel.updateDropTarget(pointer.y, pointer.x)
                    }
                    onReleased: mouse => {
                        if (chainPanel.dragActive) {
                            const pointer = reorderHandle.mapToItem(chainList, mouse.x, mouse.y)
                            chainPanel.updateDropTarget(pointer.y, pointer.x)
                        }
                        chainPanel.finishDrag(true)
                    }
                    onCanceled: chainPanel.finishDrag(false)
                }
                HoverHandler { id: cardHover }
                Text {
                    objectName: "insertEffectPreset"
                    anchors.right: parent.right
                    anchors.rightMargin: chainPanel.headerDragRoom
                    y: chainPanel.headerY
                    // The id is null while a removed strip tears down and its
                    // body clears its preset name, which re-runs these bindings.
                    width: effectCard ? Math.min(implicitWidth, effectCard.width * chainPanel.presetMaxShare) : 0
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    text: stripBody.presetName
                    color: chainPanel.dimColor
                    font.pixelSize: Theme.typeBody
                }
                Text {
                    x: chainPanel.stripTextX
                    y: chainPanel.stripButtonY
                    width: effectCard ? effectCard.width - chainPanel.stripTitleRoom
                                        - (stripBody.presetName.length > 0 ? effectCard.width * chainPanel.presetRoomShare : 0) : 0
                    elide: Text.ElideRight
                    text: String(effectCard.modelData.name || "").toUpperCase()
                    color: effectCard.modelData.missing ? chainPanel.warningColor : chainPanel.textColor
                    font.pixelSize: Theme.typeLabel
                    font.bold: true
                    font.letterSpacing: Theme.trackingMd
                }
                Text {
                    x: chainPanel.stripTextX
                    y: chainPanel.stripKindY
                    width: parent.width - chainPanel.stripTitleRoom
                    elide: Text.ElideRight
                    text: (effectCard.builtIn ? (effectCard.modelData.kindLabel || "Effect")
                           : effectCard.modelData.pluginId ? "VST3" : effectCard.modelData.type)
                          + (effectCard.modelData.missing ? "  ·  MISSING" : "")
                          + (!effectCard.modelData.enabled ? "  ·  OFF" : "")
                    color: effectCard.modelData.missing || !effectCard.modelData.enabled
                           ? chainPanel.warningColor : effectCard.stripAccent
                    font.pixelSize: Theme.typeMicro
                    font.bold: true
                    font.letterSpacing: Theme.trackingMd
                }
                ChainActionButton {
                    objectName: "insertOpenEditor"
                    x: parent.width - chainPanel.buttonSlot
                    y: chainPanel.stripButtonY
                    label: effectCard.modelData.pluginId ? "FX" : "⤢"
                    actionName: "Open " + effectCard.modelData.name + " editor"
                    visible: chainPanel.effectPath
                             && (effectCard.modelData.nativeKind !== undefined
                                 || Boolean(effectCard.modelData.pluginId))
                    // Opening a device is safe while the graph is playing or a
                    // saved revision is pending. The editor gates each parameter
                    // independently and reports whether its value reached audio.
                    enabled: chainPanel.pathId !== ""
                    onTriggered: effectCard.openEditor()
                }

                // Body: a built-in engine's live view and key knobs; VST3 a summary.
                DeviceStripBody {
                    id: stripBody
                    visible: effectCard.builtIn
                    x: chainPanel.stripPad
                    y: chainPanel.stripBodyY
                    width: parent.width - 2 * chainPanel.stripPad
                    height: parent.height - chainPanel.effectBodyRoom
                    bridge: projectBridge
                    pathId: effectCard.builtIn ? chainPanel.pathId : ""
                    slotId: effectCard.builtIn ? String(effectCard.modelData.slotId || "") : ""
                    engineId: effectCard.builtIn && effectCard.modelData.engineAvailable
                        ? String(effectCard.modelData.engine || "") : ""
                    accent: effectCard.stripAccent
                    active: Boolean(effectCard.modelData.enabled)
                    playing: chainPanel.playbackActive && !chainPanel.playbackPaused
                }
                Text {
                    x: Theme.spaceLg
                    y: chainPanel.vst3NoteY
                    width: parent.width - 2 * Theme.spaceLg
                    visible: !Boolean(effectCard.modelData.nativeKind)
                    wrapMode: Text.WordWrap
                    maximumLineCount: chainPanel.stripNoteLines
                    elide: Text.ElideRight
                    text: effectCard.modelData.pluginId
                        ? (effectCard.modelData.parameterCount || 0)
                          + " saved parameters · double-click the header for its editor"
                        : "Double-click the header for device details"
                    color: chainPanel.dimColor
                    font.pixelSize: Theme.typeBody
                }

                // Footer: what this device's state means, and its structural
                // actions (also reachable by drag and from the keyboard).
                Text {
                    objectName: "insertEffectApplicationStatus"
                    // Said only when it needs attention: missing, bypassed,
                    // or not yet in what is playing.
                    visible: effectCard.modelData.missing || !effectCard.modelData.enabled
                        || chainPanel.projectApplicationAlert
                    x: Theme.spaceLg
                    y: parent.height - chainPanel.footerTextRise
                    width: parent.width - chainPanel.footerActionsRoom
                    text: effectCard.applicationState
                    color: effectCard.modelData.missing || !effectCard.modelData.enabled
                        || chainPanel.projectApplicationAlert
                        ? chainPanel.warningColor : chainPanel.dimColor
                    font.pixelSize: Theme.typeMicro
                    font.bold: true
                    font.letterSpacing: Theme.trackingXxs
                    elide: Text.ElideRight
                }
                // Reorder and remove: shown on hover or keyboard focus, so the
                // strip reads clean while staying reachable from the keyboard.
                Row {
                    id: cardActions
                    x: parent.width - width - Theme.spaceMd
                    y: parent.height - chainPanel.footerActionsRise
                    spacing: Theme.spaceXs
                    opacity: cardHover.hovered || effectCard.activeFocus || relinkAction.activeFocus
                        || moveEarlierAction.activeFocus || moveLaterAction.activeFocus
                        || removeAction.activeFocus ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Motion.fast } }
                    ChainActionButton {
                        id: relinkAction
                        objectName: "insertRelinkVst3"
                        label: "↻"
                        actionName: "Verify or relink " + effectCard.modelData.name
                        visible: Boolean(effectCard.modelData.pluginId) && chainPanel.effectPath
                        enabled: chainPanel.editable && chainPanel.vst3Available
                        onTriggered: chainPanel.selectedRecoverySlotId = effectCard.modelData.slotId
                    }
                    ChainActionButton {
                        id: moveEarlierAction
                        objectName: "insertMoveUp"
                        label: "‹"
                        actionName: "Move " + effectCard.modelData.name + " earlier"
                        visible: chainPanel.effectPath
                        enabled: chainPanel.editable && effectCard.index > 0
                        onTriggered: projectBridge.move_effect_insert(
                            chainPanel.pathId, effectCard.modelData.slotId, effectCard.index - 1)
                    }
                    ChainActionButton {
                        id: moveLaterAction
                        objectName: "insertMoveDown"
                        label: "›"
                        actionName: "Move " + effectCard.modelData.name + " later"
                        visible: chainPanel.effectPath
                        enabled: chainPanel.editable
                            && effectCard.index + 1 < chainPanel.effects.length
                        onTriggered: projectBridge.move_effect_insert(
                            chainPanel.pathId, effectCard.modelData.slotId, effectCard.index + 1)
                    }
                    ChainActionButton {
                        id: removeAction
                        objectName: "insertRemove"
                        label: "×"
                        actionName: "Remove " + effectCard.modelData.name
                        visible: chainPanel.effectPath
                        enabled: chainPanel.editable
                        onTriggered: projectBridge.remove_effect_insert(
                            chainPanel.pathId, effectCard.modelData.slotId)
                    }
                }
                Accessible.name: "Insert " + (index + 1) + " " + (modelData.name || "")
                Accessible.description: (modelData.type || "")
                                        + (modelData.missing ? ", missing plugin" : "")
                                        + (!modelData.enabled ? ", bypassed" : "")
                                        + ". " + effectCard.applicationState
                                        + (chainPanel.playbackRevisionInactive
                                            ? ". " + chainPanel.playbackStatus : "")
            }
        }
        Rectangle {
            objectName: "insertDropMarker"
            visible: chainPanel.dragActive && chainPanel.dragTargetIndex >= 0
                     && chainPanel.dragTargetIndex !== chainPanel.dragStartIndex
            x: chainPanel.horizontalEffectStart
                + (chainPanel.dragTargetIndex
                    + (chainPanel.dragTargetIndex > chainPanel.dragStartIndex ? 1 : 0))
                    * (chainPanel.horizontalEffectWidth + chainPanel.horizontalGap)
                - chainPanel.horizontalGap / 2 - (chainPanel.dropMarkerWidth - 1) / 2
            y: Theme.spaceXs
            width: chainPanel.dropMarkerWidth
            height: chainPanel.horizontalCardHeight - 2 * Theme.spaceXs
            radius: chainPanel.dropMarkerRadius
            color: chainPanel.accentColor
            z: 3
        }
        Text {
            objectName: "emptyInsertChain"
            visible: false
            text: "No audio effects on this path"
        }

        // Where the next effect goes: click for the menu, or drop one from the
        // browser (BrowserCatalogue.dragPayload under application/x-oscillate-device).
        Rectangle {
            id: dropZone
            objectName: "deviceChainDropZone"
            visible: chainPanel.effectPath
            enabled: chainPanel.editable
            x: chainPanel.horizontalDropX
            y: 0
            width: chainPanel.dropZoneWidth
            height: chainPanel.horizontalCardHeight
            radius: chainPanel.stripRadius
            color: zoneDrop.containsDrag ? Qt.rgba(chainPanel.accentColor.r, chainPanel.accentColor.g,
                                                   chainPanel.accentColor.b, Theme.dropWashAlpha) : "transparent"
            border.color: zoneDrop.containsDrag ? chainPanel.accentColor : Theme.chainDropZoneEdge
            border.width: Theme.chainDropZoneStroke
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "Add an effect at the end of the chain"
            Keys.onReturnPressed: chainPanel.addMenuOpen = !chainPanel.addMenuOpen
            Keys.onSpacePressed: chainPanel.addMenuOpen = !chainPanel.addMenuOpen
            Text {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.spaceLg
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: "+\nDrop an effect, or double-click one in the browser"
                color: Theme.textFaint
                font.pixelSize: Theme.typeBody
                lineHeight: chainPanel.dropTextLineHeight
            }
            MouseArea {
                anchors.fill: parent
                onClicked: chainPanel.addMenuOpen = !chainPanel.addMenuOpen
            }
            DropArea {
                id: zoneDrop
                anchors.fill: parent
                keys: ["application/x-oscillate-device"]
                onDropped: drop => {
                    const payload = JSON.parse(drop.getDataAsString("application/x-oscillate-device") || "{}")
                    if (payload.kind !== "effect" || !chainPanel.editable)
                        return
                    const index = chainPanel.effects.length
                    if (payload.source === "vst3")
                        projectBridge.vst3Browser.insert_vst3_effect(payload.vst3Index, payload.classId, chainPanel.pathId)
                    else
                        projectBridge.add_effect_preset(chainPanel.pathId, index, payload.engine, payload.preset)
                    drop.accept(Qt.CopyAction)
                }
            }
        }

        // Sends: one knob per return (A · Reverb, B · Delay ...), 0 is off.
        StripFrame {
            id: sendStrip
            objectName: "deviceChainSends"
            visible: chainPanel.sendStripWidth > 0
            x: chainPanel.horizontalSendsX
            y: 0
            width: chainPanel.sendStripWidth
            height: chainPanel.horizontalCardHeight
            accent: chainPanel.accentColor
            Text {
                x: Theme.spaceLg
                y: Theme.spaceLg
                text: "SENDS"
                color: chainPanel.textColor
                font.pixelSize: Theme.typeLabel
                font.bold: true
                font.letterSpacing: Theme.trackingMd
            }
            Row {
                x: Theme.spaceMd
                y: chainPanel.sendsRowY
                spacing: Theme.spaceXs
                Repeater {
                    model: chainPanel.sendStripWidth > 0 ? (chainPanel.sends || []).length : 0
                    delegate: SendKnob {
                        required property int index
                        send: (chainPanel.sends || [])[index] || ({})
                        pathId: chainPanel.pathId
                    }
                }
            }
            Text {
                x: Theme.spaceLg
                y: parent.height - chainPanel.footerTextRise
                text: "→ " + (chainPanel.outputName || "Master")
                color: chainPanel.dimColor
                font.pixelSize: Theme.typeBody
            }
        }

        // Where the path's audio goes.
        Rectangle {
            objectName: "insertChainOutputCard"
            visible: !chainPanel.horizontalLayout
            x: chainPanel.horizontalOutputX
            y: 0
            width: chainPanel.outputWidth
            height: chainPanel.horizontalCardHeight
            radius: chainPanel.stripRadius
            color: Theme.outputCardFill
            border.color: Theme.stripEdge
            Text {
                x: Theme.spaceLg
                y: Theme.spaceLg
                text: "OUTPUT"
                color: chainPanel.dimColor
                font.pixelSize: Theme.typeCaption
                font.bold: true
                font.letterSpacing: Theme.trackingMd
            }
            Text {
                x: Theme.spaceLg
                y: chainPanel.outputTextY
                width: parent.width - 2 * Theme.spaceLg
                wrapMode: Text.WordWrap
                maximumLineCount: chainPanel.outputLines
                elide: Text.ElideRight
                text: "→ " + (chainPanel.outputName || (chainPanel.pathId === "master"
                    ? "Audio device" : "Master"))
                color: chainPanel.textColor
                font.pixelSize: Theme.typeEmphasis
                font.bold: true
            }
        }
    }

    // One return send: drag vertically, wheel, or arrow keys; double-click
    // resets to off. The edit commits on release as one undo step.
    component SendKnob: Item {
        id: sendKnob
        required property var send
        required property string pathId
        property real dragGain: -1
        readonly property real gain: dragGain >= 0 ? dragGain : Number(send.gain || 0)
        readonly property color tint: send.color ? send.color : chainPanel.accentColor
        readonly property real position: Math.sqrt(Math.max(0, Math.min(1, gain / maxGain)))
        // Gain 0..2 on a square-root dial; keys, wheel and drag move the dial.
        readonly property real maxGain: 2
        readonly property real keyStep: 0.04
        readonly property real wheelStep: 0.03
        readonly property real dragPixels: 160
        width: chainPanel.sendKnobWidth
        height: chainPanel.sendKnobHeight
        objectName: "deviceChainSend_" + (send.name || "")
        activeFocusOnTab: true
        Accessible.role: Accessible.Slider
        Accessible.name: "Send to " + (send.name || "return")
        Accessible.description: sendKnob.readout
        readonly property string readout: gain <= 0.00001 ? "Off"
            : (20 * Math.log(gain) / Math.LN10).toFixed(1) + " dB"
        function gainAt(position) {
            const p = Math.max(0, Math.min(1, position))
            return p * p * maxGain
        }
        function commit(value) {
            dragGain = -1
            if (send.returnId === undefined)
                return
            projectBridge.set_send_level(pathId, send.returnId, value)
        }
        Keys.onUpPressed: commit(gainAt(position + keyStep))
        Keys.onDownPressed: commit(gainAt(position - keyStep))
        Keys.onDeletePressed: commit(0)
        Canvas {
            id: dial
            x: (parent.width - width) / 2
            y: 0
            width: chainPanel.sendDialSize
            height: chainPanel.sendDialSize
            // A 270° arc round a centre cap, as on the macro knobs.
            readonly property real arcRadius: 16
            readonly property real capRadius: 10
            readonly property real startAngle: Math.PI * 0.75
            readonly property real sweepAngle: Math.PI * 1.5
            readonly property real silentAt: 0.001
            property real at: sendKnob.position
            onAtChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const start = startAngle
                const sweep = sweepAngle
                ctx.lineWidth = Theme.sendDialWidth
                ctx.lineCap = "round"
                ctx.strokeStyle = Theme.sendDialTrack
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, arcRadius, start, start + sweep)
                ctx.stroke()
                if (at > silentAt) {
                    ctx.strokeStyle = sendKnob.tint
                    ctx.beginPath()
                    ctx.arc(width / 2, height / 2, arcRadius, start, start + sweep * at)
                    ctx.stroke()
                }
                ctx.fillStyle = Theme.sendDialCap
                ctx.beginPath()
                ctx.arc(width / 2, height / 2, capRadius, 0, Math.PI * 2)
                ctx.fill()
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: chainPanel.sendNameY
            text: (sendKnob.send.name || "").toUpperCase()
            color: sendKnob.tint
            font.pixelSize: Theme.typeMicro
            font.bold: true
            font.letterSpacing: Theme.trackingSm
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            y: chainPanel.sendValueY
            text: sendKnob.readout
            color: chainPanel.textColor
            font.pixelSize: Theme.typeSmall
        }
        MouseArea {
            anchors.fill: dial
            property real startY: 0
            property real startPosition: 0
            cursorShape: Qt.SizeVerCursor
            onPressed: mouse => {
                startY = mouse.y
                startPosition = sendKnob.position
                sendKnob.dragGain = sendKnob.gain
            }
            onPositionChanged: mouse => {
                if (pressed)
                    sendKnob.dragGain = sendKnob.gainAt(startPosition + (startY - mouse.y) / sendKnob.dragPixels)
            }
            onReleased: sendKnob.commit(sendKnob.gain)
            onCanceled: sendKnob.dragGain = -1
            onDoubleClicked: sendKnob.commit(0)
            onWheel: wheel => sendKnob.commit(sendKnob.gainAt(sendKnob.position
                + (wheel.angleDelta.y > 0 ? sendKnob.wheelStep : -sendKnob.wheelStep)))
        }
    }
    FloatingDeviceFrame {
        parent: chainPanel.horizontalLayout && chainPanel.Window.window
            ? chainPanel.Window.window.contentItem : chainPanel
        anchors.fill: parent
        title: chainPanel.vst3Editor.name || "VST3 effect"
        surfaceWidth: chainPanel.vst3EditorWidth
        // Editors float over the workspace, movable, like built-in panels.
        surfaceHeight: chainPanel.floatingEditorHeight
        preferredX: -1
        preferredY: Theme.floatingPanelTop
        showClose: false
        visible: Boolean(chainPanel.vst3Editor.open)
        z: 20
        Vst3InsertEditorPanel {
            objectName: "vst3InsertEditorPanel"
            width: parent.width
            height: parent.height - (vst3Sidechain.visible ? chainPanel.sidechainRoom : 0)
            editor: chainPanel.vst3Editor
            panelColor: chainPanel.panelColor
            lineColor: chainPanel.lineColor
            textColor: chainPanel.textColor
            dimColor: chainPanel.dimColor
            accentColor: chainPanel.accentColor
        }
        SidechainStrip {
            id: vst3Sidechain
            objectName: "vst3SidechainStrip"
            x: Theme.spaceMd
            y: parent.height - chainPanel.sidechainRise
            width: parent.width - 2 * Theme.spaceMd
            height: chainPanel.sidechainHeight
            bridge: projectBridge
            trackId: chainPanel.vst3Editor.role === "effect" ? (chainPanel.vst3Editor.trackId || "") : ""
            slotId: chainPanel.vst3Editor.slotId || ""
            listenable: false
            accentColor: chainPanel.accentColor
        }
    }
    Vst3RecoveryPanel {
        objectName: "vst3RecoveryPanel"
        x: chainPanel.listInsetX
        y: chainPanel.recoveryY
        width: chainList.width
        height: chainList.height
        visible: chainPanel.selectedRecovery !== null
        z: 7
        target: chainPanel.selectedRecovery || ({})
        trackId: chainPanel.pathId
        entries: chainPanel.vst3Entries
        status: chainPanel.vst3Status
        busy: chainPanel.vst3Busy
        editable: chainPanel.editable
        panelColor: chainPanel.panelColor
        raisedColor: chainPanel.raisedColor
        lineColor: chainPanel.lineColor
        textColor: chainPanel.textColor
        dimColor: chainPanel.dimColor
        accentColor: chainPanel.accentColor
        warningColor: chainPanel.warningColor
        onCloseRequested: chainPanel.selectedRecoverySlotId = ""
    }
    Rectangle {
        objectName: "insertAddNativeMenu"
        x: chainPanel.horizontalLayout
            ? Math.min(parent.width - width - Theme.spaceMd, chainList.x + dropZone.x - chainList.contentX)
            : parent.width - chainPanel.addMenuRight
        y: chainPanel.horizontalLayout ? chainList.y + Theme.spaceMd : chainPanel.addMenuListY
        width: chainPanel.addMenuWidth
        height: addRepeater.count * chainPanel.addRowPitch + 2 * Theme.spaceSm
        z: 8
        visible: chainPanel.addMenuOpen && chainPanel.effectPath && chainPanel.editable
        radius: Theme.radiusLg
        color: chainPanel.raisedColor
        border.color: chainPanel.accentColor
        Repeater {
            id: addRepeater
            // The everyday mixing tools; the browser holds every built-in effect.
            model: [
                {kind: "eq", engine: "shape", name: "Shape · EQ"},
                {kind: "compressor", engine: "press", name: "Press · Compressor"},
                {kind: "reverb", engine: "expanse", name: "Expanse · Reverb"},
                {kind: "limiter", engine: "ceiling", name: "Ceiling · Limiter"}
            ]
            delegate: Rectangle {
                required property var modelData
                required property int index
                objectName: "insertAddNative_" + modelData.kind
                x: Theme.spaceSm
                y: Theme.spaceSm + index * chainPanel.addRowPitch
                width: parent.width - 2 * Theme.spaceSm
                height: chainPanel.addRowHeight
                radius: Theme.radiusMd
                color: addMouse.containsMouse ? chainPanel.panelColor : chainPanel.raisedColor
                Accessible.role: Accessible.Button
                Accessible.name: "Add " + modelData.name
                activeFocusOnTab: visible && chainPanel.editable
                Keys.onReturnPressed: addMouse.commit()
                Keys.onSpacePressed: addMouse.commit()
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.spaceMd
                    text: parent.modelData.name
                    color: chainPanel.textColor
                    font.pixelSize: Theme.typeBody
                }
                MouseArea {
                    id: addMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    function commit() {
                        if (!chainPanel.editable)
                            return
                        chainPanel.addMenuOpen = false
                        projectBridge.add_effect_engine(chainPanel.pathId, chainPanel.effects.length,
                                                        modelData.engine)
                    }
                    onClicked: commit()
                }
            }
        }
    }
    // Clicking anywhere else closes the swap list.
    MouseArea {
        anchors.fill: parent
        z: 7
        visible: chainPanel.swapMenuOpen
        onClicked: chainPanel.swapMenuOpen = false
    }
    Rectangle {
        objectName: "instrumentSwapMenu"
        x: Theme.spaceMd
        y: chainList.y + chainPanel.swapMenuOffset
        width: chainPanel.swapMenuWidth
        height: swapRepeater.count * chainPanel.swapRowPitch + chainPanel.swapHeadingRoom
        z: 8
        visible: chainPanel.swapMenuOpen
        radius: Theme.radiusLg
        color: chainPanel.raisedColor
        border.color: chainPanel.accentColor
        focus: visible
        Keys.onEscapePressed: chainPanel.swapMenuOpen = false
        Text {
            x: chainPanel.stripPad; y: Theme.spaceMd
            text: "REPLACE INSTRUMENT"
            color: chainPanel.dimColor
            font.pixelSize: Theme.typeCaption
            font.bold: true
            font.letterSpacing: Theme.trackingMd
        }
        Repeater {
            id: swapRepeater
            model: projectBridge.nativeSynthEngines || []
            delegate: Rectangle {
                id: swapItem
                required property var modelData
                required property int index
                readonly property bool current: chainPanel.chain.sourceEngine === modelData.id
                objectName: "instrumentSwap_" + modelData.id
                x: Theme.spaceSm
                y: chainPanel.swapRowsY + index * chainPanel.swapRowPitch
                width: parent.width - 2 * Theme.spaceSm
                height: chainPanel.swapRowHeight
                radius: Theme.radiusMd
                color: swapMouse.containsMouse ? chainPanel.panelColor : "transparent"
                Accessible.role: Accessible.Button
                Accessible.name: "Replace the instrument with " + modelData.name
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.spaceMd
                    text: swapItem.modelData.name + (swapItem.current ? "  ✓" : "")
                    color: swapItem.current ? chainPanel.accentColor : chainPanel.textColor
                    font.pixelSize: Theme.typeBody
                }
                MouseArea {
                    id: swapMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        chainPanel.swapMenuOpen = false
                        if (!swapItem.current)
                            projectBridge.replace_native_instrument(chainPanel.pathId, swapItem.modelData.id, -1)
                    }
                }
            }
        }
    }
    Text {
        objectName: "insertChainStatus"
        x: chainPanel.headerX
        visible: !chainPanel.horizontalLayout
        y: parent.height - chainPanel.statusRise
        width: parent.width - chainPanel.headerRoom
        elide: Text.ElideRight
        text: "SAVED  ·  " + (chainPanel.effects.length > 2
            ? "More than two stages; current Play may reject" : chainPanel.chain.sourceStatus
              || chainPanel.chain.status || "No saved chain")
        color: chainPanel.chain.sourceStatus ? chainPanel.warningColor : chainPanel.dimColor
        font.pixelSize: Theme.typeCaption
    }
    Text {
        objectName: "insertChainLiveStatus"
        x: chainPanel.headerX
        visible: !chainPanel.horizontalLayout
        y: parent.height - chainPanel.liveStatusRise
        width: parent.width - chainPanel.headerRoom
        elide: Text.ElideRight
        text: "LIVE  ·  " + chainPanel.playbackStatus
        color: chainPanel.playbackActive ? chainPanel.accentColor : chainPanel.dimColor
        font.pixelSize: Theme.typeCaption
        Accessible.description: "Saved insert order and live graph status are reported separately. "
            + chainPanel.playbackStatus
    }
}
