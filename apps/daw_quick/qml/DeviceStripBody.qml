pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "devicekit" as Kit
import "shell"

// The body of a device-chain strip for a built-in engine: a live mini-view
// and four key knobs on the engine's own controller, so every knob move is a
// gesture with undo exactly as in the full panel. The controller is held per
// path and slot ("__source__" for the track's instrument) and re-acquired only
// when the slot's identity changes, so value edits never rebuild the strip.
Item {
    id: body
    objectName: "deviceStripBody"
    required property var bridge
    required property string pathId
    required property string slotId
    required property string engineId
    required property color accent
    property color well: Theme.canvas
    property color ink: Theme.textMain
    property color inkDim: Theme.textDim
    property bool active: true
    property bool playing: false
    property var controller: null
    property string heldPath: ""
    property string heldSlot: ""
    property var response: []
    property var liveFrame: []
    // The engine's own live view (device_strip_view.hpp), drawn by its kind;
    // empty while nothing plays or for engines without one.
    property var strip: ({})
    readonly property string stripKind: strip.kind !== undefined ? strip.kind : ""
    readonly property string identity: pathId + "|" + slotId + "|" + engineId
    // The strip's sound, for its card header ("Concert Hall", "* " once edited).
    readonly property string presetName: controller && controller.revision >= 0
        ? (controller.preset_modified ? "* " : "") + controller.preset_name : ""
    readonly property var info: engineId !== "" ? bridge.device_strip_info(engineId)
        : ({kind: "", accent: "", parameters: []})
    // The mini-view's size: as tall as the strip allows, leaving room for the knobs.
    readonly property int vizMinHeight: 40
    readonly property int vizMaxHeight: 150
    readonly property int knobRoom: 78
    readonly property real vizRadius: 7
    readonly property int unavailableGap: 14
    // Telemetry: a live frame has more points than this; it is polled at this rate.
    readonly property int liveFrameMinPoints: 8
    readonly property int liveFrameMs: 66
    readonly property int responsePoints: 48

    function release() {
        if (heldSlot !== "")
            bridge.release_device_strip(heldPath, heldSlot)
        heldPath = ""
        heldSlot = ""
        controller = null
    }
    function acquire() {
        release()
        if (pathId === "" || slotId === "" || engineId === "")
            return
        controller = bridge.device_strip_controller(pathId, slotId)
        if (controller) {
            heldPath = pathId
            heldSlot = slotId
        }
        refreshStatic()
    }
    function refreshStatic() {
        response = controller && controller.has_filter_response
            ? controller.filter_response(responsePoints) : []
        viz.requestPaint()
    }
    onIdentityChanged: acquire()
    Component.onCompleted: acquire()
    Component.onDestruction: release()

    Connections {
        target: body.bridge
        function onRevisionChanged() {
            // Values may have changed from elsewhere (undo, automation edits);
            // a knob mid-drag already holds the newest values.
            if (body.controller && body.controller.gesture_parameter === "") {
                body.controller.refresh()
                body.refreshStatic()
            }
        }
    }

    Timer {
        interval: body.liveFrameMs
        repeat: true
        running: body.controller !== null && body.playing && body.visible && body.active
        onTriggered: {
            const telemetry = body.controller.telemetry()
            // Only an engine's output spectrum is drawn live; its raw frame is a
            // packed record (peaks, flags, band data, several spectra).
            body.liveFrame = telemetry.spectrum ? telemetry.spectrum : []
            body.strip = telemetry.strip ? telemetry.strip : ({})
            viz.requestPaint()
        }
        onRunningChanged: if (!running) { body.liveFrame = []; body.strip = ({}); viz.requestPaint() }
    }

    Kit.KitTheme {
        id: stripTheme
        prefix: "strip"
        accent: body.accent
        knobAccent: body.accent
        knobDiameter: Theme.stripKnobDiameter
        knobMinWidth: Theme.stripKnobMinWidth
        knobWidthPad: Theme.stripKnobWidthPad
        knobLabelSize: Theme.typeCaption
        // Captions keep to their knob's slot, elided and clipped there, so
        // neighbours never run into each other; caps tracked a little tighter.
        knobCaptionInset: 0
        knobCaptionClip: true
        knobLabelSpacing: Theme.capsLetterSpacing
        knobValueSize: Theme.typeSmall
        // Label above an always-shown value.
        knobStacked: true
        knobStackedValueSize: Theme.typeBody
        knobValueIdleInk: body.ink
        ink: body.ink
        inkDim: body.inkDim
        well: body.well
    }

    // The mini-view while playing: the engine's own view (strip) or its output
    // spectrum; otherwise the filter response when the engine has one, else a
    // quiet baseline.
    Rectangle {
        id: vizWell
        objectName: "deviceStripViz"
        x: 0; y: 0
        width: parent.width
        height: Math.max(body.vizMinHeight, Math.min(body.vizMaxHeight, parent.height - body.knobRoom))
        radius: body.vizRadius
        color: body.well
        // The view's frame (Kit.PlotFrame): its scale with units, and LIVE
        // while the device plays or PREVIEW for the settings' response.
        Kit.PlotFrame {
            id: vizFrame
            objectName: "deviceStripPlot"
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            anchors.topMargin: body.stripKind !== "" ? viz.textRoom + Theme.spaceXs : Theme.spaceXs
            theme: stripTheme
            readonly property bool liveSpectrum: body.liveFrame.length > body.liveFrameMinPoints
            readonly property bool hasResponse: body.response.length > 1
            live: body.stripKind !== "" || liveSpectrum
            showsChip: body.stripKind !== "" || liveSpectrum || hasResponse
            // Each view's scale: its range's ends, the unit on the first.
            function ends(low, high, inverted) {
                return [{ at: inverted ? 1 : 0, label: Format.number(low, 0) },
                        { at: inverted ? 0 : 1, label: Format.number(high, 0) }]
            }
            yUnit: body.stripKind === "scope" ? "FS" : body.stripKind === "loudness" ? "LU"
                : body.stripKind === "meters" ? "" : "dB"
            xUnit: body.stripKind === "meters" ? "dB" : ""
            yTicks: body.stripKind === "reduction" ? ends(-body.strip.range_db, 0, false)
                : body.stripKind === "curve" ? ends(body.strip.floor_db, body.strip.ceiling_db, false)
                : body.stripKind === "scope" ? ends(-1, 1, false)
                : body.stripKind === "loudness" ? ends(-body.strip.below_lu, body.strip.above_lu, false)
                : body.stripKind === "meters" ? []
                : liveSpectrum ? ends(viz.spectrumFloorDb, viz.spectrumCeilingDb, false)
                : hasResponse ? ends(viz.responseFloorDb, viz.responseCeilingDb, false) : []
            xTicks: body.stripKind === "meters" ? ends(body.strip.floor_db, body.strip.ceiling_db, false) : []
            Canvas {
                id: viz
                anchors.fill: parent
                // A baseline when there is nothing to draw; else the curve in a fixed
                // dB window: dBFS for the live spectrum (as the full panels draw it),
                // gain for the response. A strip view keeps a text row clear above it.
                readonly property real baselineRatio: 0.62
                readonly property int maxPoints: 256
                readonly property real responseFloorDb: -30
                readonly property real responseCeilingDb: 12
                readonly property real spectrumFloorDb: -90
                readonly property real spectrumCeilingDb: 0
                readonly property real edgeInset: 2
                readonly property real textRoom: Theme.typeSmall + Theme.spaceXs
                // The readings sit above the frame, so the plot starts at its top.
                readonly property real plotTop: 0
                // Levels at or under this are silence (no reading).
                readonly property real silentBelow: -199
                readonly property int meterRows: 4

                function tint(alpha) {
                    return Qt.rgba(body.accent.r, body.accent.g, body.accent.b, alpha)
                }
                function clamp01(v) {
                    return Math.max(0, Math.min(1, v))
                }
                // A filled curve, low to high across the width, on a dB window.
                function paintCurve(ctx, values, low, high) {
                    const w = width
                    const h = height
                    const count = Math.min(values.length, maxPoints)
                    const span = high - low
                    const plotH = h - plotTop - 2 * edgeInset
                    const point = i => {
                        const v = Number(values[i])
                        const y = isFinite(v) ? (v - low) / span : 0
                        return [i / (count - 1) * w, h - edgeInset - clamp01(y) * plotH]
                    }
                    ctx.beginPath()
                    ctx.moveTo(0, h)
                    for (let i = 0; i < count; ++i) {
                        const p = point(i)
                        ctx.lineTo(p[0], p[1])
                    }
                    ctx.lineTo(w, h)
                    ctx.closePath()
                    ctx.fillStyle = tint(Theme.stripFillAlpha)
                    ctx.fill()
                    ctx.beginPath()
                    for (let i = 0; i < count; ++i) {
                        const p = point(i)
                        if (i === 0)
                            ctx.moveTo(p[0], p[1])
                        else
                            ctx.lineTo(p[0], p[1])
                    }
                    ctx.strokeStyle = body.accent
                    ctx.lineWidth = Theme.stripCurveWidth
                    ctx.stroke()
                }
                // Gain reduction hanging from 0 dB over the last few seconds, newest
                // on the right; under it, each band's share of the detector.
                function paintReduction(ctx) {
                    const s = body.strip
                    const w = width
                    const bandRoom = (height - plotTop) * Theme.stripBandShare
                    const plotH = height - plotTop - bandRoom - Theme.stripBarGap
                    const dx = w / Math.max(1, s.slots - 1)
                    const n = s.history.length
                    const xOf = i => w - (n - 1 - i) * dx
                    const yOf = db => plotTop + clamp01(db / s.range_db) * plotH
                    ctx.strokeStyle = tint(Theme.stripBaselineAlpha)
                    ctx.lineWidth = Theme.hairline
                    ctx.beginPath()
                    ctx.moveTo(0, plotTop)
                    ctx.lineTo(w, plotTop)
                    ctx.stroke()
                    if (n > 0) {
                        ctx.beginPath()
                        ctx.moveTo(xOf(0), plotTop)
                        for (let i = 0; i < n; ++i)
                            ctx.lineTo(xOf(i), yOf(s.history[i]))
                        ctx.lineTo(xOf(n - 1), plotTop)
                        ctx.closePath()
                        ctx.fillStyle = tint(Theme.stripFillAlpha)
                        ctx.fill()
                        ctx.beginPath()
                        for (let i = 0; i < n; ++i) {
                            if (i === 0)
                                ctx.moveTo(xOf(i), yOf(s.history[i]))
                            else
                                ctx.lineTo(xOf(i), yOf(s.history[i]))
                        }
                        ctx.strokeStyle = body.accent
                        ctx.lineWidth = Theme.stripCurveWidth
                        ctx.stroke()
                    }
                    const count = s.bands.length
                    const barW = (w - (count - 1) * Theme.stripBarGap) / count
                    for (let b = 0; b < count; ++b) {
                        const barH = clamp01(s.bands[b]) * bandRoom
                        ctx.fillStyle = b === s.lead ? body.accent : tint(Theme.stripDimAlpha)
                        ctx.fillRect(b * (barW + Theme.stripBarGap), height - barH, barW, barH)
                    }
                }
                // Input and output, left and right: RMS bars with the held peak.
                function paintMeters(ctx) {
                    const s = body.strip
                    const left = Theme.stripMeterLabelRoom
                    const w = width - left
                    const rowH = (height - plotTop - (meterRows - 1) * Theme.stripBarGap) / meterRows
                    const at = db => clamp01((db - s.floor_db) / (s.ceiling_db - s.floor_db))
                    const zone = db => db > s.over_db ? Theme.masterMeterHot
                        : db > s.hot_db ? Theme.warning : body.accent
                    const rows = [s.input_rms[0], s.input_rms[1], s.output_rms[0], s.output_rms[1]]
                    const peaks = [s.input_peak[0], s.input_peak[1], s.output_peak[0], s.output_peak[1]]
                    ctx.font = Theme.typeMicro + "px '" + Theme.monoFamily + "'"
                    ctx.textBaseline = "middle"
                    ctx.fillStyle = body.inkDim
                    const pairH = 2 * rowH + Theme.stripBarGap
                    ctx.fillText("IN", 0, plotTop + pairH / 2)
                    ctx.fillText("OUT", 0, plotTop + 2 * (rowH + Theme.stripBarGap) + pairH / 2)
                    for (let r = 0; r < meterRows; ++r) {
                        const y = plotTop + r * (rowH + Theme.stripBarGap)
                        ctx.fillStyle = Theme.meterTrack
                        ctx.fillRect(left, y, w, rowH)
                        if (rows[r] > s.floor_db) {
                            ctx.fillStyle = zone(rows[r])
                            ctx.fillRect(left, y, at(rows[r]) * w, rowH)
                        }
                        if (peaks[r] > s.floor_db) {
                            ctx.fillStyle = peaks[r] > s.over_db ? Theme.masterMeterHot : body.ink
                            ctx.fillRect(left + at(peaks[r]) * w - Theme.hairline, y, Theme.strokeBold, rowH)
                        }
                    }
                }
                // The scope's sweep: the track's envelope, the key's outline over
                // it, and the beam at the column being written.
                function paintScope(ctx) {
                    const s = body.strip
                    const w = width
                    const plotH = height - plotTop
                    const mid = plotTop + plotH / 2
                    const half = plotH / 2
                    const dx = w / s.columns
                    const envelope = data => {
                        ctx.beginPath()
                        for (let c = 0; c < s.columns; ++c) {
                            const y = mid - Math.max(-1, Math.min(1, data[2 * c + 1])) * half
                            if (c === 0)
                                ctx.moveTo(c * dx + dx / 2, y)
                            else
                                ctx.lineTo(c * dx + dx / 2, y)
                        }
                        for (let c = s.columns - 1; c >= 0; --c)
                            ctx.lineTo(c * dx + dx / 2, mid - Math.max(-1, Math.min(1, data[2 * c])) * half)
                        ctx.closePath()
                    }
                    ctx.strokeStyle = tint(Theme.stripBaselineAlpha)
                    ctx.lineWidth = Theme.hairline
                    ctx.beginPath()
                    ctx.moveTo(0, mid)
                    ctx.lineTo(w, mid)
                    ctx.stroke()
                    envelope(s.track)
                    ctx.fillStyle = tint(Theme.stripFillAlpha)
                    ctx.fill()
                    ctx.strokeStyle = body.accent
                    ctx.lineWidth = Theme.hairline
                    ctx.stroke()
                    if (s.keyed) {
                        envelope(s.key)
                        ctx.strokeStyle = body.inkDim
                        ctx.stroke()
                    }
                    ctx.fillStyle = tint(Theme.stripDimAlpha)
                    ctx.fillRect(s.write_column * dx, plotTop, Theme.hairline, plotH)
                }
                // Short-term loudness over the history around the target line.
                function paintLoudness(ctx) {
                    const s = body.strip
                    const w = width
                    const plotH = height - plotTop - 2 * edgeInset
                    const span = s.above_lu - s.below_lu
                    const yOf = lufs => plotTop + edgeInset
                        + clamp01((s.above_lu - (lufs - s.target)) / span) * plotH
                    ctx.strokeStyle = body.inkDim
                    ctx.lineWidth = Theme.hairline
                    ctx.beginPath()
                    ctx.moveTo(0, yOf(s.target))
                    ctx.lineTo(w, yOf(s.target))
                    ctx.stroke()
                    const n = s.history.length
                    ctx.strokeStyle = body.accent
                    ctx.lineWidth = Theme.stripCurveWidth
                    ctx.beginPath()
                    let drawing = false
                    for (let i = 0; i < n; ++i) {
                        const v = s.history[i]
                        if (!(v > silentBelow)) {
                            drawing = false
                            continue
                        }
                        const x = i / Math.max(1, n - 1) * w
                        if (drawing)
                            ctx.lineTo(x, yOf(v))
                        else
                            ctx.moveTo(x, yOf(v))
                        drawing = true
                    }
                    ctx.stroke()
                }
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const kind = body.stripKind
                    if (kind === "reduction") {
                        paintReduction(ctx)
                        return
                    }
                    if (kind === "meters") {
                        paintMeters(ctx)
                        return
                    }
                    if (kind === "curve") {
                        paintCurve(ctx, body.strip.values, body.strip.floor_db, body.strip.ceiling_db)
                        return
                    }
                    if (kind === "scope") {
                        paintScope(ctx)
                        return
                    }
                    if (kind === "loudness") {
                        paintLoudness(ctx)
                        return
                    }
                    const live = body.liveFrame.length > body.liveFrameMinPoints
                    const values = live ? body.liveFrame : body.response.length > 1 ? body.response : []
                    if (values.length < 2) {
                        ctx.strokeStyle = tint(Theme.stripBaselineAlpha)
                        ctx.lineWidth = Theme.hairline
                        ctx.beginPath()
                        ctx.moveTo(0, height * baselineRatio)
                        ctx.lineTo(width, height * baselineRatio)
                        ctx.stroke()
                        return
                    }
                    paintCurve(ctx, values, live ? spectrumFloorDb : responseFloorDb,
                               live ? spectrumCeilingDb : responseCeilingDb)
                }
            }
        }
        // The strip view's readings: the main one on the left, the leading
        // band or true peak on the right (warning colour over the ceiling).
        Text {
            id: stripNote
            objectName: "deviceStripNote"
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceXs
            text: body.stripKind === "" ? ""
                : body.strip.lead_text ? body.strip.lead_text
                : body.strip.peak_text ? body.strip.peak_text : ""
            color: body.strip.over === true ? Theme.warning : body.accent
            font.family: Theme.monoFamily
            font.pixelSize: Theme.typeSmall
        }
        Text {
            objectName: "deviceStripReading"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: Theme.spaceXs
            width: parent.width - 2 * Theme.spaceXs - stripNote.implicitWidth - Theme.spaceSm
            text: body.stripKind !== "" && body.strip.text ? body.strip.text : ""
            color: body.ink
            font.family: Theme.monoFamily
            font.pixelSize: Theme.typeSmall
            elide: Text.ElideRight
        }
    }

    Row {
        id: knobRow
        objectName: "deviceStripKnobs"
        anchors.horizontalCenter: parent.horizontalCenter
        y: vizWell.height + Theme.spaceSm
        spacing: Theme.spaceXxs
        // Each knob takes an equal slot of the strip, and its captions keep to it.
        readonly property int count: body.controller ? body.info.parameters.length : 0
        readonly property real slotWidth: count > 0
            ? Math.min(Theme.stripKnobMaxWidth, (body.width - (count - 1) * spacing) / count) : 0
        Repeater {
            model: body.controller ? body.info.parameters : []
            delegate: Kit.Knob {
                required property string modelData
                width: knobRow.slotWidth
                controller: body.controller
                theme: stripTheme
                parameterId: modelData
                diameter: Theme.stripKnobDiameter
                opacity: body.active ? 1 : Theme.disabledOpacity
            }
        }
    }

    Text {
        visible: body.engineId !== "" && body.controller === null
        y: vizWell.height + body.unavailableGap
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: "Controls unavailable"
        color: body.inkDim
        font.pixelSize: Theme.typeSmall
    }
}
