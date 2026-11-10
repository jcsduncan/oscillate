import QtQuick

// Gild's view: the spectrum from 20 Hz to 20 kHz on a log axis, levels in
// dBFS. Behind it each band is shaded in its colour, the crossovers
// dashed between them; the programme (the dry input as heard) is a grey
// ghost, and what Gild adds a gold wash under its outline, so the
// harmonics show where they land against the music. It fills its
// Kit.PlotFrame's plot area.
Item {
    id: view
    required property GildTheme theme
    // Per display bin, low to high (dBFS, gild::spectrum_bins of them,
    // spaced evenly in log frequency over lowestHz..highestHz).
    property var input: []
    property var added: []
    property int bands: 1
    property var crossovers: []
    property bool live: false

    readonly property real lowestHz: 20
    readonly property real highestHz: 20000
    readonly property real floorDb: -96
    readonly property real topDb: 0
    readonly property var dbTicks: [0, -24, -48, -72, -96]
    readonly property var hzTicks: [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]

    // Where a frequency sits from the left (0) to the right (1).
    function hzAt(hz) {
        return Math.max(0, Math.min(1, Math.log(hz / lowestHz) / Math.log(highestHz / lowestHz)))
    }
    // Where a level sits from the bottom (0) to the top (1).
    function dbAt(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / (topDb - floorDb)))
    }

    onInputChanged: canvas.requestPaint()
    onAddedChanged: canvas.requestPaint()
    onBandsChanged: canvas.requestPaint()
    onCrossoversChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Accessible.role: Accessible.Graphic
    Accessible.name: "Gild spectrum: the programme and what Gild adds"

    Rectangle {
        anchors.fill: parent
        color: view.theme.screen
    }
    Canvas {
        id: canvas
        objectName: "gildViewCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            // The bands behind everything, and their crossovers.
            const edges = [0]
            for (let k = 0; k < view.bands - 1 && k < view.crossovers.length; ++k)
                edges.push(view.hzAt(view.crossovers[k]))
            edges.push(1)
            ctx.globalAlpha = t.bandWashAlpha
            for (let b = 0; b < edges.length - 1; ++b) {
                ctx.fillStyle = t.bandColours[b]
                ctx.fillRect(edges[b] * w, 0, (edges[b + 1] - edges[b]) * w, h)
            }
            ctx.globalAlpha = 1
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of view.dbTicks) {
                const y = h * (1 - view.dbAt(db))
                ctx.beginPath()
                ctx.moveTo(0, y)
                ctx.lineTo(w, y)
                ctx.stroke()
            }
            ctx.lineWidth = t.crossoverStroke
            ctx.strokeStyle = t.gridStrong
            ctx.setLineDash([t.dashOn, t.dashOff])
            for (let k = 1; k < edges.length - 1; ++k) {
                ctx.beginPath()
                ctx.moveTo(edges[k] * w, 0)
                ctx.lineTo(edges[k] * w, h)
                ctx.stroke()
            }
            ctx.setLineDash([])

            // A spectrum as a closed area down to the floor.
            function area(levels) {
                const count = levels.length
                ctx.beginPath()
                ctx.moveTo(0, h)
                for (let i = 0; i < count; ++i) {
                    const x = (i + 0.5) / count * w
                    ctx.lineTo(x, h * (1 - view.dbAt(levels[i])))
                }
                ctx.lineTo(w, h)
                ctx.closePath()
            }
            function outline(levels) {
                const count = levels.length
                ctx.beginPath()
                for (let i = 0; i < count; ++i) {
                    const x = (i + 0.5) / count * w
                    const y = h * (1 - view.dbAt(levels[i]))
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
            if (view.input.length > 0) {
                ctx.fillStyle = t.held
                area(view.input)
                ctx.fill()
                ctx.lineWidth = t.hairline
                ctx.strokeStyle = t.heldEdge
                outline(view.input)
            }
            if (view.added.length > 0) {
                ctx.globalAlpha = t.addedWashAlpha
                ctx.fillStyle = t.accent
                area(view.added)
                ctx.fill()
                ctx.globalAlpha = 1
                ctx.lineWidth = t.traceStroke
                ctx.lineJoin = "round"
                ctx.strokeStyle = t.accent
                outline(view.added)
            }
        }
    }
}
