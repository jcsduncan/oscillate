pragma ComponentBehavior: Bound
import QtQuick

// An oscilloscope sketch of the current model's oscillator: two cycles of
// the mix the kernel runs. The Sub model adds its pitch drop as an inset.
Rectangle {
    id: scope
    required property var controller
    required property TorqueTheme theme
    property color accent: theme.accent
    readonly property int model: controller.revision >= 0 ? controller.choice("torque.model") : 0
    readonly property var cycle: controller.revision >= 0 ? controller.oscillator_cycle(160) : []
    readonly property real dropTime: controller.revision >= 0 ? controller.value("torque.sub.drop_time") : 0.06
    readonly property real dropSeconds: Math.max(0.05, Math.min(1.0, 5 * dropTime))
    readonly property var drop: controller.revision >= 0 && model === 3
        ? controller.pitch_drop(dropSeconds, 64) : []
    readonly property real dropStart: drop.length > 0 ? drop[0] : 0
    // The wave's share of the height when the pitch-drop inset shows.
    readonly property real waveShare: 0.62
    // Headroom between the wave's peaks and the canvas edge.
    readonly property real tracePad: 4
    // Vertical grid lines split the two cycles into quarters.
    readonly property int gridDivisions: 4
    // The inset's gap below the wave and its inner vertical padding.
    readonly property real dropGap: 8
    readonly property real dropPad: 3

    objectName: "torqueScopeView"
    radius: theme.radiusLg
    color: theme.well
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Oscillator scope"

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: scope.theme.spaceMd
        anchors.topMargin: scope.theme.scopeCanvasTop
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = scope.model === 3 && scope.dropStart > 0.01 ? height * scope.waveShare : height
            const mid = h / 2
            ctx.strokeStyle = scope.theme.grid
            ctx.lineWidth = scope.theme.hairline
            for (let i = 1; i < scope.gridDivisions; ++i) {
                const x = Math.round(i * w / scope.gridDivisions) + 0.5
                ctx.beginPath()
                ctx.moveTo(x, 0)
                ctx.lineTo(x, h)
                ctx.stroke()
            }
            ctx.strokeStyle = scope.theme.inkFaint
            ctx.beginPath()
            ctx.moveTo(0, Math.round(mid) + 0.5)
            ctx.lineTo(w, Math.round(mid) + 0.5)
            ctx.stroke()
            const c = scope.cycle
            const a = scope.accent
            if (c.length > 1) {
                const n = c.length
                const trace = () => {
                    ctx.beginPath()
                    for (let i = 0; i <= 2 * n; ++i) {
                        const x = i / (2 * n) * w
                        const y = mid - c[i % n] * (mid - scope.tracePad)
                        if (i === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    ctx.stroke()
                }
                ctx.lineJoin = "round"
                ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, scope.theme.glowAlpha)
                ctx.lineWidth = scope.theme.scopeGlowWidth
                trace()
                ctx.strokeStyle = a
                ctx.lineWidth = scope.theme.scopeTraceWidth
                trace()
            }
            const d = scope.drop
            if (d.length > 1 && scope.dropStart > 0.01) {
                const top = h + scope.dropGap
                const dh = height - top
                ctx.strokeStyle = scope.theme.grid
                ctx.strokeRect(0.5, top + 0.5, w - 1, dh - 1)
                ctx.strokeStyle = a
                ctx.lineWidth = scope.theme.dropTraceWidth
                ctx.beginPath()
                for (let i = 0; i < d.length; ++i) {
                    const x = i / (d.length - 1) * w
                    const y = top + scope.dropPad + (1 - d[i] / scope.dropStart) * (dh - 2 * scope.dropPad)
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
        }
    }
    Text {
        x: scope.theme.scopeTitleX
        y: scope.theme.spaceSm
        text: "OSCILLATOR"
        color: scope.accent
        font.pixelSize: scope.theme.typeCaption
        font.bold: true
        font.letterSpacing: scope.theme.scopeTitleSpacing
    }
    Text {
        objectName: "torqueDropReadout"
        visible: scope.model === 3 && scope.dropStart > 0.01
        anchors.right: parent.right
        anchors.rightMargin: scope.theme.scopeTitleX
        y: scope.theme.scopeReadoutY
        text: "DROP +" + scope.dropStart.toFixed(1) + " st"
        color: scope.theme.ink
        font.family: scope.theme.mono
        font.pixelSize: scope.theme.typeCaption
        font.bold: true
    }
    Text {
        visible: scope.model === 3 && scope.dropStart > 0.01
        x: scope.theme.dropLabelX
        y: scope.theme.scopeCanvasTop
           + (scope.height - scope.theme.scopeCanvasTop - scope.theme.spaceMd) * scope.waveShare
           + scope.theme.dropLabelDrop
        text: "PITCH DROP · " + Math.round(scope.dropSeconds * 1000) + " ms"
        color: scope.theme.inkFaint
        font.pixelSize: scope.theme.typeMicro
        font.bold: true
        font.letterSpacing: scope.theme.dropLabelSpacing
    }
    onCycleChanged: canvas.requestPaint()
    onDropChanged: canvas.requestPaint()
    onAccentChanged: canvas.requestPaint()
}
