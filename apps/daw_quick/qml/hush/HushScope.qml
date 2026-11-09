import QtQuick

// Hush's gate view: the last few seconds scrolling right to left, each
// slice the input's peak mirrored about the centre on a decibel scale, in
// a grey ghost, and over it what the gate let through in the accent. The
// open threshold is a solid line, the close threshold (open less the
// hysteresis) a dashed one. It fills its Kit.PlotFrame's plot area; the
// frame draws the scales from dbTicks at levelAt.
Item {
    id: view
    required property HushTheme theme
    // Per slice, oldest first: the input's peak (linear) and the least gain
    // the gate applied in it (0 to 1).
    property var peaks: []
    property var gains: []
    property bool live: false
    property real openDb: -40
    property real closeDb: -46

    readonly property real floorDb: -60
    readonly property var dbTicks: [0, -12, -24, -48]
    // Where a level sits from the centre (0) to the top edge (1).
    function levelAt(db) {
        return Math.max(0, Math.min(1, (db - floorDb) / -floorDb))
    }
    function dbOf(gain) {
        return gain > 0 ? 20 * Math.log(gain) / Math.LN10 : floorDb
    }

    onPeaksChanged: canvas.requestPaint()
    onGainsChanged: canvas.requestPaint()
    onOpenDbChanged: canvas.requestPaint()
    onCloseDbChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: view.theme.screen
    }
    Canvas {
        id: canvas
        objectName: "hushScopeCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            const mid = h / 2
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of view.dbTicks) {
                const dy = view.levelAt(db) * mid
                ctx.beginPath()
                ctx.moveTo(0, mid - dy)
                ctx.lineTo(w, mid - dy)
                ctx.moveTo(0, mid + dy)
                ctx.lineTo(w, mid + dy)
                ctx.stroke()
            }
            ctx.strokeStyle = t.centre
            ctx.beginPath()
            ctx.moveTo(0, mid)
            ctx.lineTo(w, mid)
            ctx.stroke()

            const count = view.peaks.length
            if (count > 0) {
                const slot = w / count
                const barWidth = Math.max(t.hairline, slot - t.sliceGap)
                for (let i = 0; i < count; ++i) {
                    const peak = view.peaks[i]
                    if (!(peak > 0))
                        continue
                    const x = i * slot
                    const inner = view.levelAt(view.dbOf(peak)) * mid
                    ctx.fillStyle = t.held
                    ctx.fillRect(x, mid - inner, barWidth, 2 * inner)
                    const gain = i < view.gains.length ? view.gains[i] : 0
                    const passed = view.levelAt(view.dbOf(peak * gain)) * mid
                    if (passed > 0) {
                        ctx.fillStyle = t.accent
                        ctx.fillRect(x, mid - passed, barWidth, 2 * passed)
                    }
                }
            }

            ctx.lineWidth = t.thresholdStroke
            ctx.strokeStyle = t.accent
            const open = view.levelAt(view.openDb) * mid
            ctx.beginPath()
            ctx.moveTo(0, mid - open)
            ctx.lineTo(w, mid - open)
            ctx.moveTo(0, mid + open)
            ctx.lineTo(w, mid + open)
            ctx.stroke()
            const close = view.levelAt(view.closeDb) * mid
            ctx.setLineDash([t.dashOn, t.dashOff])
            ctx.strokeStyle = t.inkDim
            ctx.beginPath()
            ctx.moveTo(0, mid - close)
            ctx.lineTo(w, mid - close)
            ctx.moveTo(0, mid + close)
            ctx.lineTo(w, mid + close)
            ctx.stroke()
        }
    }
}
