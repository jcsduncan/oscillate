pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The match EQ's preview (ADR-0120): the correction Lens proposes as a
// dashed gold line and the Shape bands that draw it as a glowing cyan line,
// each band a numbered dot at its frequency and gain, over 20 Hz to 20 kHz
// and ±rangeDb. Outside the low and high limits the plot dims, as the
// correction fades there.
Item {
    id: plot
    required property LensTheme theme
    property bool live: false
    // The fine bands' centres, the correction and the response there (dB).
    property var hz: []
    property var correction: []
    property var response: []
    // [{kind, hz, shape, gain}]
    property var bands: []
    property real lowLimitHz: 20
    property real highLimitHz: 20000
    property real rangeDb: theme.matchRangeDb

    readonly property real lowHz: 20
    readonly property real highHz: 20000
    readonly property var frequencyMarks: [[20, "20"], [100, "100"], [1000, "1k"], [10000, "10k"], [20000, "20k"]]
    readonly property var levelMarks: [-plot.rangeDb, -plot.rangeDb / 2, 0, plot.rangeDb / 2, plot.rangeDb]

    readonly property real px: theme.plotGutterLeft
    readonly property real py: theme.spaceXs
    readonly property real pw: width - px - theme.spaceXs
    readonly property real ph: height - py - theme.plotGutterBottom

    objectName: "lensMatchPlot"

    function xOf(f) {
        return px + pw * Math.log(Math.max(lowHz, Math.min(highHz, f)) / lowHz) / Math.log(highHz / lowHz)
    }
    function yOf(db) {
        return py + ph * (0.5 - 0.5 * Math.max(-1, Math.min(1, db / rangeDb)))
    }

    onCorrectionChanged: canvas.requestPaint()
    onResponseChanged: canvas.requestPaint()
    onBandsChanged: canvas.requestPaint()
    onLowLimitHzChanged: canvas.requestPaint()
    onHighLimitHzChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Kit.PlotFrame {
        objectName: "lensMatchFrame"
        x: 0
        y: plot.py
        width: plot.px + plot.pw
        height: plot.ph + plot.theme.plotGutterBottom
        theme: plot.theme
        live: plot.live
        xUnit: "Hz"
        yUnit: "dB"
        xTicks: plot.pw > 0 ? plot.frequencyMarks.map(m => ({ at: (plot.xOf(m[0]) - plot.px) / plot.pw, label: m[1] })) : []
        yTicks: plot.levelMarks.map(db => ({ at: 0.5 + 0.5 * db / plot.rangeDb, label: Format.number(db, 0) }))

        Canvas {
            id: canvas
            x: -plot.px
            y: -plot.py
            width: plot.width
            height: plot.height
            renderStrategy: Canvas.Cooperative
            function trace(ctx, values) {
                for (let i = 0; i < values.length && i < plot.hz.length; ++i) {
                    const x = plot.xOf(plot.hz[i])
                    const y = plot.yOf(values[i])
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
            }
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = plot.theme
                const left = plot.px
                const top = plot.py
                const bottom = plot.py + plot.ph
                ctx.fillStyle = t.well
                ctx.fillRect(left, top, plot.pw, plot.ph)
                // Beyond the limits the correction fades out.
                ctx.fillStyle = t.matchOutsideFill
                const low = plot.xOf(plot.lowLimitHz)
                const high = plot.xOf(plot.highLimitHz)
                ctx.fillRect(left, top, low - left, plot.ph)
                ctx.fillRect(high, top, left + plot.pw - high, plot.ph)
                ctx.lineWidth = t.hairline
                for (const db of plot.levelMarks) {
                    const y = Math.round(plot.yOf(db)) + t.hairline / 2
                    ctx.strokeStyle = db === 0 ? t.gridStrong : t.grid
                    ctx.beginPath()
                    ctx.moveTo(left, y)
                    ctx.lineTo(left + plot.pw, y)
                    ctx.stroke()
                }
                for (const mark of plot.frequencyMarks) {
                    const x = Math.round(plot.xOf(mark[0])) + t.hairline / 2
                    ctx.strokeStyle = t.grid
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, bottom)
                    ctx.stroke()
                }
                if (plot.correction.length > 0) {
                    ctx.strokeStyle = t.difference
                    ctx.lineWidth = t.differenceStroke
                    ctx.setLineDash([t.differenceDashLength, t.differenceDashPeriod - t.differenceDashLength])
                    ctx.beginPath()
                    trace(ctx, plot.correction)
                    ctx.stroke()
                    ctx.setLineDash([])
                }
                if (plot.response.length > 0) {
                    ctx.strokeStyle = Qt.rgba(t.primary.r, t.primary.g, t.primary.b, t.primaryGlowAlpha)
                    ctx.lineWidth = t.primaryGlowWidth
                    ctx.beginPath()
                    trace(ctx, plot.response)
                    ctx.stroke()
                    ctx.strokeStyle = t.primary
                    ctx.lineWidth = t.primaryStroke
                    ctx.beginPath()
                    trace(ctx, plot.response)
                    ctx.stroke()
                }
                ctx.font = t.typeMicro + "px '" + t.mono + "'"
                ctx.textAlign = "center"
                for (let b = 0; b < plot.bands.length; ++b) {
                    const band = plot.bands[b]
                    const x = plot.xOf(band.hz)
                    const y = plot.yOf(band.gain)
                    ctx.fillStyle = t.well
                    ctx.strokeStyle = t.primary
                    ctx.lineWidth = t.hairline
                    ctx.beginPath()
                    ctx.arc(x, y, t.matchBandDotRadius, 0, 2 * Math.PI)
                    ctx.fill()
                    ctx.stroke()
                    ctx.fillStyle = t.ink
                    ctx.fillText(String(b + 1), x, y + t.matchBandDotTextDrop)
                }
            }
        }
    }
}
