import QtQuick

// What the LFO moves and how far: the swept range (delay in ms, the
// phaser's centre in Hz on a log axis, or the tremolo's gain) with each
// channel's position on it, and below it two cycles of the LFO for each
// channel, offset by the stereo phase.
Canvas {
    id: sweep
    required property OrbitTheme theme
    // {lo, hi, unit: "ms" | "Hz" | "%", log}
    property var range: ({ lo: 0, hi: 1, unit: "ms", log: false })
    property real leftValue: 0
    property real rightValue: 0
    property int shape: 0
    property real stereoDegrees: 90
    property real phase: 0
    property bool live: false
    property real lfoLeft: 0
    property real lfoRight: 0

    // The drawing's layout. The range track: its centre line, half height,
    // tick marks and their labels below it, the needles' labels above and
    // below it, and the delay axis's headroom over the swept maximum.
    readonly property real trackY: 46
    readonly property real trackHalf: 7
    readonly property real tickTop: 8
    readonly property real tickLength: 4
    readonly property real tickLabelY: 24
    readonly property real needleLabelAbove: 18
    readonly property real needleLabelBelow: 38
    readonly property real delayHeadroom: 1.18
    // The LFO cycles: their top, bottom margin, the waves' clearance from
    // both, the grid's divisions, the points per trace, and where the live
    // markers sit in from the right.
    readonly property real cyclesTop: 128
    readonly property real cyclesBottomMargin: 14
    readonly property real waveClearance: 4
    readonly property int gridDivisions: 4
    readonly property int traceSteps: 160
    readonly property real liveMarkerInset: 8

    objectName: "orbitSweep"
    renderStrategy: Canvas.Cooperative

    onRangeChanged: requestPaint()
    onLeftValueChanged: requestPaint()
    onRightValueChanged: requestPaint()
    onShapeChanged: requestPaint()
    onStereoDegreesChanged: requestPaint()
    onPhaseChanged: requestPaint()
    onLfoLeftChanged: requestPaint()
    onLfoRightChanged: requestPaint()
    onWidthChanged: requestPaint()
    Connections {
        target: sweep.theme
        function onAccentChanged() { sweep.requestPaint() }
    }

    function rgba(c, alpha) {
        return Qt.rgba(c.r, c.g, c.b, alpha)
    }
    function format(value) {
        const unit = range.unit
        if (unit === "Hz")
            return value >= 1000 ? (value / 1000).toFixed(2) + " kHz" : Math.round(value) + " Hz"
        if (unit === "%")
            return Math.round(value * 100) + "%"
        return value.toFixed(value < 10 ? 2 : 1) + " ms"
    }
    function wave(phase) {
        const p = phase - Math.floor(phase)
        if (shape === 1)
            return 4 * Math.abs(((p - 0.25) - Math.floor(p - 0.25)) - 0.5) - 1
        return Math.sin(2 * Math.PI * p)
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const w = width
        const h = height
        const t = theme
        const a = t.accent
        const b = t.companion
        const pad = t.sweepPad

        // ---- The range track.
        const axisLo = range.unit === "Hz" ? 20 : 0
        const axisHi = range.unit === "Hz" ? 20000
                     : range.unit === "%" ? 1 : Math.max(1, Math.ceil(range.hi * delayHeadroom))
        const x = value => {
            const v = Math.max(axisLo, Math.min(axisHi, value))
            const f = range.log ? Math.log(v / axisLo) / Math.log(axisHi / axisLo)
                                : (v - axisLo) / (axisHi - axisLo)
            return pad + f * (w - 2 * pad)
        }
        ctx.fillStyle = t.well
        ctx.fillRect(pad, trackY - trackHalf, w - 2 * pad, 2 * trackHalf)
        ctx.strokeStyle = t.wellEdge
        ctx.lineWidth = t.hairline
        ctx.strokeRect(pad + t.hairline / 2, trackY - trackHalf + t.hairline / 2,
                       w - 2 * pad - t.hairline, 2 * trackHalf - t.hairline)
        // Ticks.
        ctx.fillStyle = t.inkFaint
        ctx.font = t.typeCaption + "px sans-serif"
        const ticks = range.unit === "Hz" ? [20, 100, 1000, 10000, 20000]
                    : range.unit === "%" ? [0, 0.5, 1]
                    : [0, axisHi / 2, axisHi]
        for (const tick of ticks) {
            const tx = x(tick)
            ctx.fillRect(tx, trackY + tickTop, t.hairline, tickLength)
            const text = range.unit === "Hz" ? (tick >= 1000 ? tick / 1000 + "k" : "" + tick)
                       : range.unit === "%" ? Math.round(tick * 100) + "%"
                       : (Math.round(tick * 10) / 10) + " ms"
            const tw = ctx.measureText(text).width
            ctx.fillText(text, Math.max(pad, Math.min(w - pad - tw, tx - tw / 2)), trackY + tickLabelY)
        }
        // The swept band.
        const x0 = x(range.lo)
        const x1 = x(range.hi)
        const band = ctx.createLinearGradient(x0, 0, x1, 0)
        band.addColorStop(0, rgba(a, t.bandEdgeAlpha))
        band.addColorStop(0.5, rgba(a, t.bandPeakAlpha))
        band.addColorStop(1, rgba(b, t.bandEdgeAlpha))
        ctx.fillStyle = band
        ctx.fillRect(x0, trackY - t.bandHalf, Math.max(t.bandMinWidth, x1 - x0), 2 * t.bandHalf)
        // Channel needles.
        const needle = (value, color, above, name) => {
            const nx = x(value)
            ctx.fillStyle = rgba(color, t.needleGlowAlpha)
            ctx.fillRect(nx - t.needleGlowWidth / 2, trackY - t.needleHalf, t.needleGlowWidth, 2 * t.needleHalf)
            ctx.fillStyle = color
            ctx.fillRect(nx - t.needleWidth / 2, trackY - t.needleHalf, t.needleWidth, 2 * t.needleHalf)
            ctx.font = "bold " + t.typeSmall + "px sans-serif"
            const text = name + " " + format(value)
            const tw = ctx.measureText(text).width
            const tx = Math.max(pad, Math.min(w - pad - tw, nx - tw / 2))
            ctx.fillText(text, tx, above ? trackY - needleLabelAbove : trackY + needleLabelBelow)
        }
        needle(leftValue, a, true, "L")
        needle(rightValue, b, false, "R")

        // ---- Two LFO cycles per channel.
        const top = cyclesTop
        const bottom = h - cyclesBottomMargin
        const mid = (top + bottom) / 2
        const amp = (bottom - top) / 2 - waveClearance
        ctx.strokeStyle = t.grid
        ctx.lineWidth = t.hairline
        ctx.beginPath()
        ctx.moveTo(pad, mid)
        ctx.lineTo(w - pad, mid)
        ctx.stroke()
        for (let k = 1; k < gridDivisions; ++k) {
            const gx = pad + (w - 2 * pad) * k / gridDivisions
            ctx.beginPath()
            ctx.moveTo(gx, top)
            ctx.lineTo(gx, bottom)
            ctx.stroke()
        }
        const offset = stereoDegrees / 360
        const trace = (shift, color, alpha, lineWidth) => {
            ctx.strokeStyle = rgba(color, alpha)
            ctx.lineWidth = lineWidth
            ctx.beginPath()
            const steps = traceSteps
            for (let s = 0; s <= steps; ++s) {
                const px = pad + (w - 2 * pad) * s / steps
                const py = mid - amp * wave(2 * s / steps + shift)
                if (s === 0)
                    ctx.moveTo(px, py)
                else
                    ctx.lineTo(px, py)
            }
            ctx.stroke()
        }
        trace(offset, b, t.traceGlowAlpha, t.traceGlowWidth)
        trace(offset, b, t.companionTraceAlpha, t.companionTraceWidth)
        trace(0, a, t.traceGlowAlpha, t.traceGlowWidth)
        trace(0, a, t.traceAlpha, t.traceWidth)
        // Where each channel is now: on the first cycle in preview, as a
        // level live (the LFO's phase is not published).
        const marker = (value, cycle, color) => {
            const px = live ? w - pad - liveMarkerInset : pad + (w - 2 * pad) * (cycle / 2)
            const py = mid - amp * value
            const g = ctx.createRadialGradient(px, py, 0, px, py, t.markerGlowRadius)
            g.addColorStop(0, rgba(color, t.markerGlowAlpha))
            g.addColorStop(1, rgba(color, 0))
            ctx.fillStyle = g
            ctx.beginPath()
            ctx.arc(px, py, t.markerGlowRadius, 0, Math.PI * 2)
            ctx.fill()
            ctx.fillStyle = t.glint
            ctx.beginPath()
            ctx.arc(px, py, t.markerDotRadius, 0, Math.PI * 2)
            ctx.fill()
        }
        const p = phase - Math.floor(phase)
        marker(live ? lfoLeft : wave(p), p, a)
        marker(live ? lfoRight : wave(p + offset), p, b)
    }
}
