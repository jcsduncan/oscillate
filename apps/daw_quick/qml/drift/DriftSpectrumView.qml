pragma ComponentBehavior: Bound
import QtQuick

// The source's partial bank: sixteen lines at n sqrt(1 + B n^2) times the
// note, their height the bank's n^-tilt law with the odd/even balance (dB),
// faded by the partials level; air shows as a haze at the floor.
Rectangle {
    id: view
    required property var controller
    required property DriftTheme theme
    readonly property var lines: controller.revision >= 0 ? controller.partial_spectrum() : []
    readonly property real partials: controller.revision >= 0 ? controller.value("drift.partials.level") : 1
    readonly property real air: controller.revision >= 0 ? controller.value("drift.air.level") : 0
    // The drawing: the haze's height (a fraction from the floor), headroom
    // over the top partial, the level range shown (dB) and the smallest
    // level measured, and how tall the lines stand at zero partials level.
    readonly property real hazeHeight: 0.6
    readonly property real ratioHeadroom: 1.05
    readonly property real floorDb: 48
    readonly property real silentLevel: 1e-6
    readonly property real lineFloor: 0.25

    objectName: "driftSpectrumView"
    radius: theme.radiusLg
    color: theme.well
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Partial spectrum"

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.viewPad
        anchors.topMargin: view.theme.spectrumTop
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            // Air: a haze along the floor.
            const hazeTop = h * (1 - view.hazeHeight)
            const haze = ctx.createLinearGradient(0, h, 0, hazeTop)
            haze.addColorStop(0, Qt.rgba(view.theme.accent2.r, view.theme.accent2.g, view.theme.accent2.b,
                                         view.theme.hazeAlpha * view.air))
            haze.addColorStop(1, "transparent")
            ctx.fillStyle = haze
            ctx.fillRect(0, hazeTop, w, h * view.hazeHeight)
            const lines = view.lines
            if (lines.length === 0)
                return
            const top = lines[lines.length - 1].ratio * view.ratioHeadroom
            for (let i = 0; i < lines.length; ++i) {
                const x = lines[i].ratio / top * w
                const db = 20 * Math.log10(Math.max(lines[i].level, view.silentLevel))
                const height = Math.max(0, (view.floorDb + db) / view.floorDb) * h
                    * (view.lineFloor + (1 - view.lineFloor) * view.partials)
                const fill = ctx.createLinearGradient(0, h, 0, h - height)
                fill.addColorStop(0, view.theme.accent)
                fill.addColorStop(1, view.theme.accent2)
                ctx.strokeStyle = fill
                ctx.lineWidth = view.theme.partialStroke
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.moveTo(x, h)
                ctx.lineTo(x, h - height)
                ctx.stroke()
            }
        }
    }
    Text {
        x: view.theme.viewLabelX
        y: view.theme.spectrumLabelY
        text: "PARTIALS"
        color: view.theme.inkFaint
        font.pixelSize: view.theme.captionSize
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
    onLinesChanged: canvas.requestPaint()
    onPartialsChanged: canvas.requestPaint()
    onAirChanged: canvas.requestPaint()
}
