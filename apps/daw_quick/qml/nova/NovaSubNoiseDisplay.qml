pragma ComponentBehavior: Bound
import QtQuick

// The SUB · NOISE card's display: the sub's shape over two cycles in
// the upper half and the noise as a texture below it, each at its level,
// shaped by the AM curve while the noise follows an oscillator; a module
// that is off is drawn faint.
Rectangle {
    id: view
    required property var panel
    readonly property var controller: panel.controller
    readonly property color accent: panel.accentFor("nova.sub.on")
    readonly property bool subOn: controller.revision >= 0 && controller.choice("nova.sub.on") === 1
    readonly property bool noiseOn: controller.revision >= 0 && controller.choice("nova.noise.on") === 1
    readonly property var watched: [controller.revision]
    // Cycles of the sub drawn, the noise texture's two partial rates, and the
    // alpha of a module that is off.
    readonly property int cycles: 2
    readonly property real noiseRateA: 211.3
    readonly property real noiseRateB: 97.7
    readonly property real offAlpha: panel.theme.displayOffAlpha
    readonly property real laneAmplitude: 0.36
    // The least level a lane is drawn at, and the pixels between its samples.
    readonly property real minimumLevel: 0.15
    readonly property int sampleStep: 2

    objectName: "novaSubNoiseDisplay"
    // A picture of the two sources at their levels, not a measured plot.
    readonly property string plotExemptReason: "a sketch of the sub's shape and the noise texture at their levels, with no measured axes"
    radius: panel.theme.displayRadius
    color: panel.theme.wellFill
    border.color: panel.theme.line
    clip: true
    onWatchedChanged: canvas.requestPaint()

    function subSample(shape, x) {
        switch (shape) {
        case 0: return Math.sin(x * Math.PI * 2)
        case 1: return 1 - 4 * Math.abs(x - 0.5)
        case 2: return 1 - 2 * x
        default: return x < 0.5 ? 1 : -1
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (view.controller.revision < 0)
                return
            const t = view.panel.theme
            const W = width, H = height
            ctx.strokeStyle = t.knobTrack
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(0, H / 2)
            ctx.lineTo(W, H / 2)
            ctx.stroke()
            const lane = (centre, amplitude, sample, alpha) => {
                ctx.beginPath()
                for (let i = 0; i <= W; i += view.sampleStep) {
                    const y = centre - sample(i / W) * amplitude
                    if (i === 0) ctx.moveTo(i, y); else ctx.lineTo(i, y)
                }
                ctx.globalAlpha = alpha
                ctx.strokeStyle = t.soft(view.accent, t.glowAlpha)
                ctx.lineWidth = t.displayGlowWidth
                ctx.stroke()
                ctx.strokeStyle = view.accent
                ctx.lineWidth = t.displayLineWidth
                ctx.stroke()
                ctx.globalAlpha = 1
            }
            const shape = view.controller.choice("nova.sub.shape")
            const subLevel = view.controller.value("nova.sub.level")
            lane(H / 4, H * view.laneAmplitude / 2 * Math.max(view.minimumLevel, subLevel),
                 x => view.subSample(shape, (x * view.cycles) % 1), view.subOn ? 1 : view.offAlpha)
            const noiseLevel = view.controller.value("nova.noise.level")
            const am = view.controller.choice("nova.noise.am.source") > 0 ? view.controller.value("nova.noise.am.amount") : 0
            lane(3 * H / 4, H * view.laneAmplitude / 2 * Math.max(view.minimumLevel, Math.sqrt(noiseLevel)),
                 x => Math.sin(x * view.noiseRateA) * Math.sin(x * view.noiseRateB + 1)
                      * (1 - am + am * Math.abs(view.subSample(2, (x * view.cycles) % 1))),
                 view.noiseOn ? 1 : view.offAlpha)
        }
    }
}
