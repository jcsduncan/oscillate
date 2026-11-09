pragma ComponentBehavior: Bound
import QtQuick

// The reduction history under the display: each poll adds a row of the
// gain Temper applies across the spectrum (newest at the top), coloured
// from no cut through copper to amber-white at maxCutDb. Boosts are not
// drawn; the display shows them.
Rectangle {
    id: heat
    required property TemperTheme theme
    // The gain now per display band (dB; negative is a cut), or [] when idle.
    property var gain: []
    // The depth that reads hottest.
    property real maxCutDb: 12
    // Rows kept, one per poll.
    property int rows: 48
    // Below this a cell is not drawn (dB of cut).
    readonly property real visibleCutDb: 0.1
    // Cells overlap their neighbours by this much so no seams show.
    readonly property real cellOverlap: 0.5
    property var history: []
    readonly property int rowCount: history.length

    objectName: "temperHeatmap"
    readonly property string plotExemptReason: "shares the frequency axis of the display above it; its rows are polls, newest at the top, not a timed scale"
    color: theme.heatCold
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Reduction history"

    function push(row) {
        const next = history.slice(0, rows - 1)
        next.unshift(row)
        history = next
        canvas.requestPaint()
    }
    function clear() {
        history = []
        canvas.requestPaint()
    }
    function mix(a, b, u) {
        return Qt.rgba(a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u, 1)
    }
    function colourOf(cutDb) {
        const u = Math.max(0, Math.min(1, cutDb / maxCutDb))
        return u < 0.5 ? mix(theme.heatCold, theme.heatWarm, u * 2)
                       : mix(theme.heatWarm, theme.heatHot, (u - 0.5) * 2)
    }

    onGainChanged: {
        if (gain.length > 0)
            push(gain)
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: heat.theme.hairline
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const rowH = height / heat.rows
            for (let r = 0; r < heat.history.length; ++r) {
                const row = heat.history[r]
                const cellW = width / row.length
                for (let i = 0; i < row.length; ++i) {
                    const cut = -Number(row[i])
                    if (cut < heat.visibleCutDb)
                        continue
                    ctx.fillStyle = heat.colourOf(cut)
                    ctx.fillRect(i * cellW, r * rowH, cellW + heat.cellOverlap, rowH + heat.cellOverlap)
                }
            }
        }
    }
    Text {
        x: heat.theme.spaceMd
        anchors.bottom: parent.bottom
        anchors.bottomMargin: heat.theme.spaceXs
        text: "REDUCTION HISTORY · NEWEST AT TOP"
        color: heat.theme.inkFaint
        font.pixelSize: heat.theme.typeMicro
        font.bold: true
        font.letterSpacing: heat.theme.capsSpacing
    }
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
