pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The docked clip editor (ADR-0053): the selected audio clip zoomed in, with
// its tempo, beat grid, downbeat, transients and warp markers, and a grid
// check that finds where the kicks drift off the grid. Warp markers are a
// last resort: "Fix drift" places them from the transients.
Item {
    id: editor
    objectName: "clipEditor"
    required property var bridge
    required property string clipId
    required property var clip            // the arrangement's projection of the clip
    required property color trackColor
    required property color lineColor
    required property color textColor
    required property color dimColor
    required property color accentColor
    required property color amberColor
    required property string monoFamily

    readonly property var controller: bridge.clipTempo
    readonly property var warp: bridge.clipWarp.state
    readonly property var tempo: controller.clips[clipId]
    // One wheel notch's angle delta.
    readonly property real wheelNotch: 120
    readonly property color gold: Theme.clipEditorMarker
    readonly property color late: Theme.clipEditorLate
    readonly property int beatsPerBar: 4
    readonly property bool editable: bridge.audioEditEnabled
    // The detail view's range, as fractions of the clip; eight bars to start.
    property real viewFrom: 0
    property real viewTo: 1
    property bool viewSet: false
    property var check: ({})
    property real hoverX: -1
    property real selectFrom: -1
    property real selectTo: -1

    function refreshCheck() {
        check = clipId.length > 0 ? controller.inspect(clipId) : ({})
        overview.requestPaint()
        detail.requestPaint()
    }
    // Takes the clip record to reset from, so a clipId change never reads the
    // previous clip's tempo before the binding updates.
    function resetView(record) {
        const tempo = record === undefined ? editor.tempo : record
        if (!tempo || !(tempo.beat > 0)) {
            viewFrom = 0
            viewTo = 1
            return
        }
        const start = Math.max(0, tempo.downbeat >= 0 ? tempo.downbeat : 0)
        viewFrom = start
        viewTo = Math.min(1, start + 32 * tempo.beat)
        viewSet = true
    }
    function zoom(factor, around) {
        const span = Math.max(0.0005, Math.min(1, (viewTo - viewFrom) * factor))
        const anchor = around === undefined ? (viewFrom + viewTo) / 2 : around
        const share = (anchor - viewFrom) / Math.max(1e-9, viewTo - viewFrom)
        let from = anchor - share * span
        from = Math.max(0, Math.min(1 - span, from))
        viewFrom = from
        viewTo = from + span
    }
    function scroll(delta) {
        const span = viewTo - viewFrom
        const from = Math.max(0, Math.min(1 - span, viewFrom + delta))
        viewFrom = from
        viewTo = from + span
    }
    function fractionAt(x) { return viewFrom + x / Math.max(1, detail.width) * (viewTo - viewFrom) }
    function xAt(fraction) { return (fraction - viewFrom) / Math.max(1e-9, viewTo - viewFrom) * detail.width }
    // The transient nearest `x` within 12 px, else the nearest beat line.
    function snapped(x, free) {
        const at = fractionAt(x)
        if (free)
            return at
        let best = -1
        let distance = Theme.clipEditorSnapDistance
        for (const onset of (check.transients || [])) {
            const d = Math.abs(xAt(onset.fraction) - x)
            if (d < distance) {
                distance = d
                best = onset.fraction
            }
        }
        if (best >= 0)
            return best
        if (tempo && tempo.beat > 0 && tempo.downbeat >= 0) {
            const beats = Math.round((at - tempo.downbeat) / tempo.beat)
            return tempo.downbeat + beats * tempo.beat
        }
        return at
    }
    function minutes(seconds) {
        const whole = Math.floor(seconds)
        return Math.floor(whole / 60) + ":" + String(whole % 60).padStart(2, "0")
    }

    onClipIdChanged: {
        viewSet = false
        resetView(controller.clips[clipId])
        refreshCheck()
    }
    onTempoChanged: {
        if (!viewSet)
            resetView()
        refreshCheck()
    }
    onViewFromChanged: { overview.requestPaint(); detail.requestPaint() }
    onViewToChanged: { overview.requestPaint(); detail.requestPaint() }
    Connections {
        target: editor.bridge.clipWarp
        function onChanged() { editor.refreshCheck() }
    }

    component ToolButton: Rectangle {
        id: tool
        property string label
        property string accessibleName: label
        property bool lit: false
        property color tone: Theme.chipText
        property bool active: true
        signal activated()
        height: Theme.clipEditorControlHeight
        width: toolText.implicitWidth + Theme.clipEditorButtonPadding
        radius: Theme.radiusLg
        opacity: active ? 1 : Theme.clipEditorInactiveOpacity
        color: lit ? Qt.rgba(tone.r, tone.g, tone.b, Theme.clipEditorLitWashAlpha) : toolArea.containsMouse ? Theme.clipEditorButtonHover : Theme.panelRaised
        border.color: lit ? tone : activeFocus ? Theme.accent : Theme.controlEdge
        activeFocusOnTab: active
        Accessible.role: Accessible.Button
        Accessible.name: accessibleName
        Tip { text: tool.accessibleName }
        Keys.onReturnPressed: if (active) activated()
        Keys.onSpacePressed: if (active) activated()
        Text {
            id: toolText
            anchors.centerIn: parent
            text: tool.label
            color: tool.lit ? tool.tone : Theme.chipText
            font.pixelSize: Theme.typeBody
            font.weight: tool.label === "TAP" ? Font.DemiBold : Font.Normal
        }
        MouseArea {
            id: toolArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: tool.active
            onClicked: tool.activated()
        }
    }

    // --- Header: tempo, Follow, mode, pitch, grid status ---
    Row {
        id: header
        x: Theme.spaceXl
        y: Theme.clipEditorHeaderY
        height: Theme.clipEditorHeaderHeight
        spacing: Theme.clipEditorHeaderSpacing
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.clipEditorSwatchSize
            height: Theme.clipEditorSwatchSize
            radius: Theme.radiusSm
            color: editor.trackColor
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: editor.clip ? (editor.clip.name || "Audio") : ""
            color: editor.textColor
            font.pixelSize: Theme.typeEmphasis
            font.weight: Font.DemiBold
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: editor.tempo ? editor.minutes(editor.tempo.seconds) : ""
            color: editor.dimColor
            font.pixelSize: Theme.typeLabel
        }
        Rectangle { width: Theme.hairline; height: Theme.clipEditorDividerHeight; anchors.verticalCenter: parent.verticalCenter; color: Theme.controlEdge }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Track tempo"
            color: editor.dimColor
            font.pixelSize: Theme.typeBody
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.clipEditorTempoWidth
            height: Theme.clipEditorControlHeight
            radius: Theme.radiusLg
            color: Theme.canvas
            border.color: tempoField.activeFocus ? editor.accentColor
                : editor.tempo && editor.tempo.bpm > 0 && !editor.tempo.corrected && editor.tempo.confidence < 0.5
                  ? editor.amberColor : Theme.controlEdge
            TextInput {
                id: tempoField
                objectName: "clipEditorTempoField"
                anchors.fill: parent
                anchors.leftMargin: Theme.clipEditorTempoTextInset
                verticalAlignment: TextInput.AlignVCenter
                text: editor.tempo && editor.tempo.bpm > 0 ? editor.tempo.bpm.toFixed(2)
                    : editor.tempo && editor.tempo.analysing ? "…" : ""
                color: editor.textColor
                font.pixelSize: Theme.typeEmphasis
                font.family: editor.monoFamily
                selectByMouse: true
                validator: DoubleValidator { bottom: 20; top: 999; decimals: 3; locale: "C" }
                Accessible.role: Accessible.EditableText
                Accessible.name: "Track tempo in BPM"
                onAccepted: {
                    const bpm = Number(text)
                    if (bpm >= 20 && bpm <= 999)
                        editor.controller.set_bpm(editor.clipId, bpm)
                }
            }
        }
        ToolButton {
            anchors.verticalCenter: parent.verticalCenter
            label: "÷2"
            accessibleName: "Halve the tempo"
            active: editor.editable
            onActivated: editor.controller.scale_bpm(editor.clipId, 0.5)
        }
        ToolButton {
            anchors.verticalCenter: parent.verticalCenter
            label: "×2"
            accessibleName: "Double the tempo"
            active: editor.editable
            onActivated: editor.controller.scale_bpm(editor.clipId, 2.0)
        }
        ToolButton {
            anchors.verticalCenter: parent.verticalCenter
            label: "TAP"
            accessibleName: "Tap the tempo"
            active: editor.editable
            onActivated: editor.controller.tap(editor.clipId)
        }
        Rectangle { width: Theme.hairline; height: Theme.clipEditorDividerHeight; anchors.verticalCenter: parent.verticalCenter; color: Theme.controlEdge }
        Rectangle {
            objectName: "clipEditorFollow"
            readonly property bool on: !!editor.tempo && editor.tempo.follow
            anchors.verticalCenter: parent.verticalCenter
            height: Theme.clipEditorControlHeight
            width: followRow.implicitWidth + Theme.clipEditorButtonPadding
            radius: Theme.radiusLg
            color: on ? Qt.rgba(editor.accentColor.r, editor.accentColor.g, editor.accentColor.b, Theme.clipEditorLitWashAlpha) : Theme.panelRaised
            border.color: on ? editor.accentColor : Theme.controlEdge
            activeFocusOnTab: editor.editable
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Follow project tempo"
            Tip { text: "Follow project tempo" }
            Accessible.checked: on
            Keys.onSpacePressed: editor.controller.set_follow(editor.clipId, !on)
            Row {
                id: followRow
                anchors.centerIn: parent
                spacing: Theme.spaceMd
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.clipEditorSwitchWidth
                    height: Theme.clipEditorSwitchHeight
                    radius: height / 2
                    color: parent.parent.on ? editor.accentColor : Theme.controlEdge
                    Rectangle {
                        x: parent.parent.parent.on ? parent.width - width - Theme.spaceXxs : Theme.spaceXxs
                        y: Theme.spaceXxs
                        width: Theme.clipEditorSwitchKnobSize
                        height: Theme.clipEditorSwitchKnobSize
                        radius: width / 2
                        color: Theme.canvas
                    }
                }
                Text {
                    text: "Follow project tempo"
                    color: parent.parent.on ? editor.accentColor : editor.textColor
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }
            }
            MouseArea {
                anchors.fill: parent
                enabled: editor.editable
                onClicked: editor.controller.set_follow(editor.clipId, !parent.on)
            }
        }
        ToolButton {
            id: modeButton
            anchors.verticalCenter: parent.verticalCenter
            label: "Mode: " + (editor.tempo ? editor.tempo.mode : "") + " ▾"
            accessibleName: "Stretch mode " + (editor.tempo ? editor.tempo.mode : "")
            active: editor.editable
            onActivated: modePopover.show(editor.clipId, "mode", modeButton.mapToItem(editor, 0, modeButton.height + Theme.spaceXs))
        }
        // Pitch in semitones: click the left half to lower, the right to raise.
        ToolButton {
            id: pitchButton
            anchors.verticalCenter: parent.verticalCenter
            readonly property int semitones: Number(editor.warp.semitones || 0)
            label: "Pitch " + (semitones > 0 ? "+" : "") + semitones + " st"
            accessibleName: "Pitch " + semitones + " semitones; left half lowers, right half raises"
            active: editor.editable && editor.warp.transposeActive !== false
            Keys.onLeftPressed: editor.bridge.clipWarp.set_transpose(semitones - 1, Number(editor.warp.cents || 0))
            Keys.onRightPressed: editor.bridge.clipWarp.set_transpose(semitones + 1, Number(editor.warp.cents || 0))
            MouseArea {
                anchors.fill: parent
                enabled: pitchButton.active
                acceptedButtons: Qt.LeftButton
                onClicked: mouse => editor.bridge.clipWarp.set_transpose(
                    pitchButton.semitones + (mouse.x < width / 2 ? -1 : 1), Number(editor.warp.cents || 0))
                // Whole notches only: fine-resolution deltas accumulate, and a
                // zero (horizontal or scroll-phase) delta does nothing.
                property real wheelRemainder: 0
                onWheel: wheel => {
                    wheelRemainder += wheel.angleDelta.y
                    const steps = Math.trunc(wheelRemainder / editor.wheelNotch)
                    if (steps === 0)
                        return
                    wheelRemainder -= steps * editor.wheelNotch
                    editor.bridge.clipWarp.set_transpose(pitchButton.semitones + steps,
                                                         Number(editor.warp.cents || 0))
                }
            }
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXl
        y: Theme.clipEditorHeaderY
        height: Theme.clipEditorHeaderHeight
        spacing: Theme.spaceSm
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.clipEditorStatusDotSize
            height: Theme.clipEditorStatusDotSize
            radius: width / 2
            color: editor.check.pending || editor.check.pending === undefined ? editor.dimColor
                : editor.check.driftBar >= 0 ? editor.late
                : editor.check.steady ? editor.accentColor : editor.amberColor
        }
        Text {
            objectName: "clipEditorGridStatus"
            anchors.verticalCenter: parent.verticalCenter
            color: editor.dimColor
            font.pixelSize: Theme.typeBody
            text: {
                const c = editor.check
                if (!editor.tempo) return ""
                if (editor.tempo.analysing || c.pending) return "Finding the beat grid…"
                if (!(editor.tempo.bpm > 0)) return "No steady beat found"
                const fit = "Grid fits " + Math.round((c.fit || 0) * 100) + "%"
                if (c.driftBar >= 0)
                    return fit + " · kicks drift " + Math.abs(c.driftMs).toFixed(0) + " ms from bar " + c.driftBar
                return fit + (c.steady ? " · steady tempo" : "")
            }
        }
    }

    // --- Overview of the whole clip, with the zoomed range ---
    Canvas {
        id: overview
        objectName: "clipEditorOverview"
        x: Theme.spaceXl
        y: Theme.clipEditorOverviewY
        width: parent.width - 2 * Theme.spaceXl
        height: Theme.clipEditorOverviewHeight
        onWidthChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.fillStyle = Theme.canvas
            context.fillRect(0, 0, width, height)
            if (editor.clipId.length === 0 || width < 2) return
            const columns = editor.controller.detail(editor.clipId, 0, 1, Math.min(4096, Math.ceil(width)))
            context.fillStyle = Qt.rgba(editor.trackColor.r, editor.trackColor.g, editor.trackColor.b, Theme.clipEditorOverviewWaveAlpha)
            for (let index = 0; index < columns.length; ++index) {
                const x = index * width / columns.length
                const top = height * (1 - columns[index].max) / 2
                const bottom = height * (1 - columns[index].min) / 2
                context.fillRect(x, top, Math.max(1, width / columns.length), Math.max(1, bottom - top))
            }
        }
        Rectangle {
            objectName: "clipEditorViewport"
            x: editor.viewFrom * parent.width
            width: Math.max(Theme.clipEditorViewBoxMinWidth, (editor.viewTo - editor.viewFrom) * parent.width)
            height: parent.height
            color: Qt.rgba(editor.accentColor.r, editor.accentColor.g, editor.accentColor.b, Theme.clipEditorViewWashAlpha)
            border.color: editor.accentColor
        }
        MouseArea {
            anchors.fill: parent
            Accessible.role: Accessible.ScrollBar
            Accessible.name: "Clip overview: drag to move the zoomed view"
            function centreOn(x) {
                editor.scroll(x / Math.max(1, width) - (editor.viewFrom + editor.viewTo) / 2)
            }
            onPressed: mouse => centreOn(mouse.x)
            onPositionChanged: mouse => { if (pressed) centreOn(mouse.x) }
        }
    }

    // --- Clip bar ruler: bars from the downbeat ---
    Item {
        id: ruler
        x: Theme.spaceXl
        y: Theme.clipEditorRulerY
        width: parent.width - 2 * Theme.spaceXl
        height: Theme.clipEditorRulerHeight
        clip: true
        Repeater {
            model: {
                const t = editor.tempo
                if (!t || !(t.beat > 0) || t.downbeat < 0) return []
                const bar = editor.beatsPerBar * t.beat
                const pxPerBar = bar / Math.max(1e-9, editor.viewTo - editor.viewFrom) * ruler.width
                const step = pxPerBar >= Theme.clipEditorRulerBeatLabelMinBar ? 1 / editor.beatsPerBar : Math.max(1, Math.pow(2, Math.ceil(Math.log2(Theme.clipEditorRulerLabelMinSpacing / Math.max(1, pxPerBar)))))
                const ticks = []
                const first = Math.floor((editor.viewFrom - t.downbeat) / (bar * step)) * step
                for (let at = first; ticks.length < 200; at += step) {
                    const fraction = t.downbeat + at * bar
                    if (fraction > editor.viewTo) break
                    const whole = Math.floor(at + 1e-9)
                    const beat = Math.round((at - whole) * editor.beatsPerBar) + 1
                    ticks.push({ fraction: fraction, label: (whole + 1) + "." + beat })
                }
                return ticks
            }
            delegate: Item {
                required property var modelData
                x: editor.xAt(modelData.fraction)
                height: ruler.height
                Rectangle { width: Theme.hairline; height: parent.height; color: Theme.clipEditorRulerTick }
                Text {
                    x: Theme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    text: parent.modelData.label
                    color: editor.dimColor
                    font.pixelSize: Theme.typeSmall
                    font.family: editor.monoFamily
                }
            }
        }
    }

    // --- The detail waveform ---
    Rectangle {
        id: detailFrame
        x: Theme.spaceXl
        y: Theme.clipEditorDetailY
        width: parent.width - 2 * Theme.spaceXl
        height: Math.max(Theme.clipEditorDetailMinHeight, parent.height - y - Theme.clipEditorFooterHeight)
        color: Theme.clipEditorDetailGround
        border.color: Theme.clipEditorDetailEdge
        clip: true

        Canvas {
            id: detail
            objectName: "clipEditorDetail"
            anchors.fill: parent
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                if (editor.clipId.length === 0 || width < 2) return
                const t = editor.tempo
                // Beat grid, bars brighter.
                if (t && t.beat > 0 && t.downbeat >= 0) {
                    const px = t.beat / Math.max(1e-9, editor.viewTo - editor.viewFrom) * width
                    if (px >= Theme.clipEditorBeatMinSpacing) {
                        let index = Math.ceil((editor.viewFrom - t.downbeat) / t.beat)
                        for (; ; ++index) {
                            const fraction = t.downbeat + index * t.beat
                            if (fraction > editor.viewTo) break
                            context.fillStyle = index % editor.beatsPerBar === 0 ? Theme.clipEditorBarLine : Theme.clipEditorBeatLine
                            context.fillRect(Math.round(editor.xAt(fraction)), 0, Theme.hairline, height)
                        }
                    }
                }
                const columns = editor.controller.detail(editor.clipId, editor.viewFrom, editor.viewTo,
                                                         Math.min(4096, Math.ceil(width)))
                context.fillStyle = Qt.rgba(editor.trackColor.r, editor.trackColor.g, editor.trackColor.b, Theme.clipEditorDetailWaveAlpha)
                for (let index = 0; index < columns.length; ++index) {
                    const x = index * width / columns.length
                    const top = height * (1 - columns[index].max) / 2
                    const bottom = height * (1 - columns[index].min) / 2
                    context.fillRect(x, top, Math.max(Theme.hairline, width / columns.length + Theme.hairline / 2), Math.max(Theme.hairline, bottom - top))
                }
                // Transients along the bottom: red where they sit off the grid.
                for (const onset of (editor.check.transients || [])) {
                    if (onset.fraction < editor.viewFrom || onset.fraction > editor.viewTo) continue
                    context.fillStyle = onset.late ? String(editor.late) : Theme.clipEditorOnset
                    context.fillRect(Math.round(editor.xAt(onset.fraction)), height - Theme.clipEditorOnsetHeight, Theme.hairline, Theme.clipEditorOnsetHeight)
                }
            }
        }

        // The selection (Shift-drag), for "Straighten selection".
        Rectangle {
            visible: editor.selectFrom >= 0 && editor.selectTo >= 0
            x: editor.xAt(Math.min(editor.selectFrom, editor.selectTo))
            width: Math.abs(editor.xAt(editor.selectTo) - editor.xAt(editor.selectFrom))
            height: parent.height
            color: Qt.rgba(editor.accentColor.r, editor.accentColor.g, editor.accentColor.b, Theme.clipEditorSelectionWashAlpha)
            border.color: Qt.rgba(editor.accentColor.r, editor.accentColor.g, editor.accentColor.b, Theme.clipEditorSelectionEdgeAlpha)
        }

        // Hover ghost: where a click pins a marker (snapped; Alt: anywhere).
        Item {
            visible: editor.hoverX >= 0 && markerOverlay.dragIndex < 0 && editor.editable
            x: editor.hoverX
            height: parent.height
            Rectangle { width: Theme.hairline; height: parent.height; color: Theme.clipEditorHoverGhost }
            Text {
                x: Theme.spaceSm
                y: Theme.clipEditorHoverLabelY
                text: "click to pin a marker here"
                color: editor.gold
                font.pixelSize: Theme.typeSmall
            }
        }

        MouseArea {
            // One wheel notch (editor.wheelNotch) zooms by wheelZoomStep or
            // scrolls by wheelScrollShare of the view.
            readonly property real wheelNotch: editor.wheelNotch
            readonly property real wheelZoomStep: 0.8
            readonly property real wheelScrollShare: 0.1
            id: detailArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
            enabled: editor.clipId.length > 0
            property bool selecting: false
            onExited: editor.hoverX = -1
            onPositionChanged: mouse => {
                if (selecting) {
                    editor.selectTo = editor.fractionAt(mouse.x)
                    return
                }
                editor.hoverX = editor.xAt(editor.snapped(mouse.x, (mouse.modifiers & Qt.AltModifier) !== 0))
            }
            onPressed: mouse => {
                if ((mouse.modifiers & Qt.ShiftModifier) !== 0) {
                    selecting = true
                    editor.selectFrom = editor.fractionAt(mouse.x)
                    editor.selectTo = editor.selectFrom
                }
            }
            onReleased: mouse => {
                if (selecting) {
                    selecting = false
                    return
                }
                if (!editor.editable) return
                // A release outside the waveform is not a click.
                if (mouse.x < 0 || mouse.x > width || mouse.y < 0 || mouse.y > height) return
                editor.selectFrom = -1
                editor.selectTo = -1
                editor.bridge.clipWarp.add_marker(editor.snapped(mouse.x, (mouse.modifiers & Qt.AltModifier) !== 0))
            }
            onWheel: wheel => {
                if ((wheel.modifiers & Qt.ControlModifier) !== 0)
                    editor.zoom(Math.pow(wheelZoomStep, wheel.angleDelta.y / wheelNotch), editor.fractionAt(wheel.x))
                else
                    editor.scroll(-(wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y) / wheelNotch
                                  * wheelScrollShare * (editor.viewTo - editor.viewFrom))
            }
        }

        // The downbeat flag: drag onto the kick that starts a bar.
        Item {
            id: downbeatFlag
            objectName: "clipEditorDownbeat"
            property bool dragging: false
            property real preview: 0
            readonly property real at: dragging ? preview : editor.tempo ? editor.tempo.downbeat : -1
            visible: at >= editor.viewFrom && at <= editor.viewTo
            x: editor.xAt(at)
            height: parent.height
            z: 3
            Rectangle { width: Theme.strokeBold; height: parent.height; color: editor.accentColor }
            Rectangle {
                width: flagLabel.implicitWidth + Theme.spaceLg
                height: Theme.clipEditorFlagHeight
                radius: Theme.radiusSm
                color: editor.accentColor
                Text {
                    id: flagLabel
                    anchors.centerIn: parent
                    text: "1.1 · downbeat"
                    color: Theme.canvas
                    font.pixelSize: Theme.typeSmall
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: editor.editable
                    cursorShape: Qt.SizeHorCursor
                    preventStealing: true
                    Accessible.role: Accessible.Slider
                    Accessible.name: "Downbeat: drag onto the first beat of a bar"
                    onPressed: {
                        downbeatFlag.preview = downbeatFlag.at
                        downbeatFlag.dragging = true
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const x = mapToItem(detailFrame, mouse.x, mouse.y).x
                        downbeatFlag.preview = editor.snapped(x, (mouse.modifiers & Qt.AltModifier) !== 0)
                    }
                    onReleased: mouse => {
                        downbeatFlag.dragging = false
                        editor.controller.set_downbeat(editor.clipId, downbeatFlag.preview, true)
                    }
                    onCanceled: downbeatFlag.dragging = false
                }
            }
        }

        // Warp markers, with the arrangement's gestures and commands.
        WarpMarkerOverlay {
            id: markerOverlay
            objectName: "clipEditorMarkers"
            anchors.fill: parent
            z: 4
            bridge: editor.bridge
            clipId: editor.clipId
            viewFrom: editor.viewFrom
            viewTo: editor.viewTo
            editable: editor.editable
            monoFamily: editor.monoFamily
            pinBand: false
        }

        // Where the kicks start to drift off the grid.
        Row {
            readonly property real at: {
                const t = editor.tempo
                const c = editor.check
                if (!t || !(c.driftBar >= 0) || !(t.beat > 0)) return -1
                return t.downbeat + editor.beatsPerBar * t.beat * (c.driftBar - 1)
            }
            visible: at >= editor.viewFrom && at <= editor.viewTo
            x: editor.xAt(at) + Theme.spaceXs
            y: parent.height * Theme.clipEditorDriftY
            spacing: Theme.clipEditorDriftSpacing
            z: 5
            Rectangle {
                width: Theme.clipEditorDriftBadgeSize
                height: Theme.clipEditorDriftBadgeSize
                radius: width / 2
                color: editor.late
                Text { anchors.centerIn: parent; text: "!"; color: Theme.canvas; font.pixelSize: Theme.typeBody; font.bold: true }
            }
            Rectangle {
                width: driftText.implicitWidth + Theme.spaceLg
                height: Theme.clipEditorDriftBadgeSize
                radius: Theme.radiusMd
                color: Theme.clipEditorDriftGround
                border.color: Theme.clipEditorDriftEdge
                Text {
                    id: driftText
                    anchors.centerIn: parent
                    text: "Kick " + Math.abs(editor.check.driftMs || 0).toFixed(0) + " ms "
                        + ((editor.check.driftMs || 0) > 0 ? "late" : "early") + " of the grid from bar " + editor.check.driftBar
                    color: Theme.clipEditorDriftText
                    font.pixelSize: Theme.typeBody
                }
            }
        }
    }

    // --- Tools ---
    Row {
        x: Theme.spaceXl
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceLg
        spacing: Theme.spaceMd
        ToolButton {
            label: "Re-analyse"
            active: editor.editable
            onActivated: editor.controller.reanalyse(editor.clipId)
        }
        ToolButton {
            objectName: "clipEditorFixDrift"
            visible: (editor.check.driftBar || -1) >= 0
            label: "Fix drift from bar " + editor.check.driftBar
            lit: true
            tone: editor.gold
            active: editor.editable && !!editor.tempo && editor.tempo.follow
            onActivated: editor.controller.fix_drift(editor.clipId, editor.check.driftBar)
        }
        ToolButton {
            label: "Straighten selection"
            accessibleName: "Remove the warp markers in the selection (Shift-drag to select)"
            active: editor.editable && editor.selectFrom >= 0 && (editor.warp.markers || []).some(m =>
                m.fraction >= Math.min(editor.selectFrom, editor.selectTo) && m.fraction <= Math.max(editor.selectFrom, editor.selectTo))
            onActivated: {
                const low = Math.min(editor.selectFrom, editor.selectTo)
                const high = Math.max(editor.selectFrom, editor.selectTo)
                const markers = (editor.warp.markers || []).filter(m => m.fraction >= low && m.fraction <= high)
                for (let index = markers.length - 1; index >= 0; --index)
                    editor.bridge.clipWarp.remove_marker(markers[index].index)
                editor.selectFrom = -1
                editor.selectTo = -1
            }
        }
        ToolButton {
            label: "Clear markers"
            active: editor.editable && (editor.warp.markers || []).length > 0
            onActivated: editor.controller.clear_markers(editor.clipId)
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.spaceXl
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spaceLg
        spacing: Theme.spaceMd
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Click to pin a marker · drag it to stretch · Shift-drag it to slide the audio · right-click for more · Shift-drag to select · Ctrl+wheel zooms"
            color: editor.dimColor
            font.pixelSize: Theme.typeBody
        }
        Rectangle { width: Theme.hairline; height: Theme.clipEditorFooterDividerHeight; anchors.verticalCenter: parent.verticalCenter; color: Theme.controlEdge }
        ToolButton { label: "−"; accessibleName: "Zoom out"; onActivated: editor.zoom(2) }
        ToolButton { label: "+"; accessibleName: "Zoom in"; onActivated: editor.zoom(0.5) }
    }

    ClipTempoPopover {
        id: modePopover
        anchors.fill: parent
        controller: editor.controller
        amberColor: editor.amberColor
        accentColor: editor.accentColor
        lineColor: editor.lineColor
        textColor: editor.textColor
        dimColor: editor.dimColor
        monoFamily: editor.monoFamily
    }
}
