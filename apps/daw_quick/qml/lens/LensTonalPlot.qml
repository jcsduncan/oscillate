pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The tonal balance view (ADR-0120): the chosen target's range per
// third-octave band as a violet corridor about its median (drawn flat, at
// 0 dB), and the mix's loudness-normalised curve against it, each band a
// dot: green inside the range, amber outside.
Item {
    id: plot
    required property LensTheme theme
    property bool live: false
    // The bands' centres; the mix's levels, the target's low, median and
    // high (dB), and each band's placement (-1, 0, 1).
    property var hz: []
    property var levels: []
    property var low: []
    property var median: []
    property var high: []
    property var placements: []
    property real rangeDb: theme.tonalRangeDb

    readonly property real lowHz: 20
    readonly property real highHz: 20000
    readonly property var frequencyMarks: [[20, "20"], [100, "100"], [1000, "1k"], [10000, "10k"], [20000, "20k"]]
    readonly property var levelMarks: [-plot.rangeDb, 0, plot.rangeDb]

    readonly property real px: theme.plotGutterLeft
    readonly property real py: theme.spaceXs
    readonly property real pw: width - px - theme.spaceXs
    readonly property real ph: height - py - theme.plotGutterBottom

    objectName: "lensTonalPlot"

    function xOf(f) {
        return px + pw * Math.log(Math.max(lowHz, Math.min(highHz, f)) / lowHz) / Math.log(highHz / lowHz)
    }
    function yOf(db) {
        return py + ph * (0.5 - 0.5 * Math.max(-1, Math.min(1, db / rangeDb)))
    }

    onLevelsChanged: canvas.requestPaint()
    onMedianChanged: canvas.requestPaint()
    onPlacementsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Kit.PlotFrame {
        objectName: "lensTonalFrame"
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
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const t = plot.theme
                const left = plot.px
                const top = plot.py
                const bottom = plot.py + plot.ph
                ctx.fillStyle = t.well
                ctx.fillRect(left, top, plot.pw, plot.ph)
                ctx.lineWidth = t.hairline
                for (const mark of plot.frequencyMarks) {
                    const x = Math.round(plot.xOf(mark[0])) + t.hairline / 2
                    ctx.strokeStyle = t.grid
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, bottom)
                    ctx.stroke()
                }
                const n = Math.min(plot.hz.length, plot.median.length, plot.low.length, plot.high.length)
                if (n > 0) {
                    // The corridor: the middle half of the references.
                    ctx.beginPath()
                    for (let b = 0; b < n; ++b) {
                        const x = plot.xOf(plot.hz[b])
                        const y = plot.yOf(plot.high[b] - plot.median[b])
                        if (b === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    for (let b = n - 1; b >= 0; --b)
                        ctx.lineTo(plot.xOf(plot.hz[b]), plot.yOf(plot.low[b] - plot.median[b]))
                    ctx.closePath()
                    ctx.fillStyle = Qt.rgba(t.overlay.r, t.overlay.g, t.overlay.b, t.tonalRangeAlpha)
                    ctx.fill()
                    ctx.strokeStyle = Qt.rgba(t.overlay.r, t.overlay.g, t.overlay.b, t.overlayAlpha)
                    ctx.stroke()
                }
                const zero = Math.round(plot.yOf(0)) + t.hairline / 2
                ctx.strokeStyle = t.gridStrong
                ctx.beginPath()
                ctx.moveTo(left, zero)
                ctx.lineTo(left + plot.pw, zero)
                ctx.stroke()
                const m = Math.min(n, plot.levels.length)
                if (m > 0) {
                    ctx.strokeStyle = t.primary
                    ctx.lineWidth = t.primaryStroke
                    ctx.beginPath()
                    for (let b = 0; b < m; ++b) {
                        const x = plot.xOf(plot.hz[b])
                        const y = plot.yOf(plot.levels[b] - plot.median[b])
                        if (b === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    ctx.stroke()
                    for (let b = 0; b < m; ++b) {
                        ctx.fillStyle = plot.placements[b] === 0 ? t.live : t.warning
                        ctx.beginPath()
                        ctx.arc(plot.xOf(plot.hz[b]), plot.yOf(plot.levels[b] - plot.median[b]),
                                t.tonalDotRadius, 0, 2 * Math.PI)
                        ctx.fill()
                    }
                }
            }
        }
    }
}
