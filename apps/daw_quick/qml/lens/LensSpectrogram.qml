import QtQuick

// A scrolling spectrogram under the spectrum, on the same frequency axis:
// one row per new analysis, newest at the top, levels coloured from night
// blue through cyan to white. The history is kept as raw rows and redrawn
// as runs of equal quantised colour on columns about 3 px wide, which every
// Qt Quick backend draws.
Item {
    id: gram
    required property LensTheme theme
    // The spectrum's plot left edge and width, so the axes line up.
    property real plotX: theme.spectrumScaleWidth
    property real plotWidth: width - plotX - theme.spectrumRightPad
    property real topDb: 0
    property real rangeDb: 90
    // Rows pushed since the last clear, and how many the strip shows.
    property int rows: 0
    readonly property int rowHeight: 1
    readonly property int visibleRows: Math.max(1, Math.floor(height / rowHeight))
    readonly property int columns: Math.max(1, Math.round(plotWidth / theme.spectrogramColumnWidth))
    // Newest first, as raw dB points.
    property var history: []

    readonly property real lowHz: 20
    readonly property real highHz: 20000
    readonly property int points: 400
    readonly property int levels: 48

    objectName: "lensSpectrogram"
    implicitHeight: theme.spectrogramHeight

    // A night-to-white map, as `levels` CSS colours.
    readonly property var colours: {
        const stops = theme.spectrogramStops
        const hex = v => ("0" + Math.round(v).toString(16)).slice(-2)
        const out = []
        for (let i = 0; i < levels; ++i) {
            // A gentle gamma so the busy middle of a mix keeps its contrast.
            const v = Math.pow(i / (levels - 1), theme.spectrogramGamma)
            let k = 0
            while (k < stops.length - 2 && v > stops[k + 1][0])
                ++k
            const a = stops[k]
            const b = stops[k + 1]
            const f = (v - a[0]) / (b[0] - a[0])
            out.push("#" + hex(a[1][0] + f * (b[1][0] - a[1][0])) + hex(a[1][1] + f * (b[1][1] - a[1][1]))
                     + hex(a[1][2] + f * (b[1][2] - a[1][2])))
        }
        return out
    }

    function push(row) {
        if (row.length < points)
            return
        history = [row].concat(history.slice(0, visibleRows - 1))
        rows = rows + 1
        canvas.requestPaint()
    }
    function clear() {
        rows = 0
        history = []
        canvas.requestPaint()
    }
    onTopDbChanged: canvas.requestPaint()
    onRangeDbChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.fillStyle = gram.theme.well
            ctx.fillRect(0, 0, width, height)
            const x0 = gram.plotX
            const w = gram.plotWidth
            const n = gram.columns
            const bottom = gram.topDb - gram.rangeDb
            // Which point each column shows.
            const point = []
            for (let c = 0; c < n; ++c)
                point.push(Math.round((gram.points - 1) * (c + 0.5) / n))
            for (let r = 0; r < gram.history.length; ++r) {
                const row = gram.history[r]
                const y = r * gram.rowHeight
                let start = 0
                let level = -1
                for (let c = 0; c <= n; ++c) {
                    let next = -1
                    if (c < n) {
                        const v = Math.max(0, Math.min(1, (row[point[c]] - bottom) / gram.rangeDb))
                        next = Math.round(v * (gram.levels - 1))
                    }
                    if (next !== level) {
                        // The darkest level is the background already.
                        if (level > 0) {
                            ctx.fillStyle = gram.colours[level]
                            const left = x0 + w * start / n
                            ctx.fillRect(left, y, x0 + w * c / n - left + gram.theme.spectrogramOverlap, gram.rowHeight)
                        }
                        start = c
                        level = next
                    }
                }
            }
        }
    }

    Rectangle {
        x: Math.round(gram.plotX) - gram.theme.hairline
        width: Math.round(gram.plotWidth) + 2 * gram.theme.hairline
        height: gram.height
        color: "transparent"
        border.color: gram.theme.wellEdge
    }
    Text {
        x: gram.plotX - width - gram.theme.spaceSm
        y: gram.theme.hairline
        text: "NOW"
        color: gram.theme.inkDim
        font.pixelSize: gram.theme.typeMicro
        font.bold: true
        font.letterSpacing: gram.theme.captionTightSpacing
    }
    Text {
        x: gram.plotX - width - gram.theme.spaceSm
        y: gram.height - height - gram.theme.hairline
        // One row per 33 ms poll.
        text: "−" + (gram.visibleRows / 30).toFixed(1) + "s"
        color: gram.theme.gridText
        font.family: gram.theme.mono
        font.pixelSize: gram.theme.typeMicro
    }
}
