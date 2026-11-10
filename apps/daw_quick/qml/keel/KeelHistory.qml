import QtQuick

// Keel's correction over time: one column per slice, oldest on the left,
// the third-octave bands from 20 Hz at the bottom to 20 kHz at the top,
// each cell lit by what Keel applied there (lifts in amber, cuts in sky,
// full at historyFullDb). It fills its Kit.PlotFrame's plot area.
Item {
    id: history
    required property KeelTheme theme
    // Per slice, oldest first: the correction per band (dB).
    property var slices: []

    readonly property int bands: 31

    onSlicesChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        color: history.theme.screen
    }
    Canvas {
        id: canvas
        objectName: "keelHistoryCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = history.theme
            const count = history.slices.length
            if (count === 0)
                return
            const cellWidth = width / count
            const cellHeight = height / history.bands
            for (let s = 0; s < count; ++s) {
                const slice = history.slices[s]
                for (let b = 0; b < Math.min(history.bands, slice.length); ++b) {
                    const db = slice[b]
                    const alpha = Math.min(1, Math.abs(db) / t.historyFullDb)
                    if (alpha < t.historyFloorAlpha)
                        continue
                    ctx.globalAlpha = alpha
                    ctx.fillStyle = db > 0 ? t.boost : t.cut
                    ctx.fillRect(s * cellWidth, height - (b + 1) * cellHeight,
                                 Math.max(t.hairline, cellWidth - t.hairline), cellHeight)
                }
            }
            ctx.globalAlpha = 1
        }
    }
}
