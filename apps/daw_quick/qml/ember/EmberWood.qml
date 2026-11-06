import QtQuick

// A walnut end cheek with drawn grain.
Canvas {
    id: wood
    required property EmberTheme theme
    property bool leftSide: true
    // The grain's sway: two sine ripples along the cheek.
    readonly property real swayRate: 0.021
    readonly property real swayPhaseStep: 1.7
    readonly property real swayDepth: 1.6
    readonly property real rippleRate: 0.07
    readonly property real rippleDepth: 0.6
    renderStrategy: Canvas.Cooperative
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const fill = ctx.createLinearGradient(0, 0, width, 0)
        fill.addColorStop(0, wood.leftSide ? wood.theme.woodDark : wood.theme.woodLight)
        fill.addColorStop(0.5, wood.theme.woodMid)
        fill.addColorStop(1, wood.leftSide ? wood.theme.woodLight : wood.theme.woodDark)
        ctx.fillStyle = fill
        ctx.beginPath()
        ctx.roundedRect(0, 0, wood.width, wood.height, wood.theme.radiusLg, wood.theme.radiusLg)
        ctx.fill()
        // Wavy grain lines with a deterministic pattern.
        ctx.strokeStyle = wood.theme.woodGrain
        ctx.lineWidth = wood.theme.hairline
        const t = wood.theme
        for (let i = 0; i < t.grainLines; ++i) {
            const base = t.grainInset + i * (wood.width - 2 * t.grainInset) / (t.grainLines - 1)
            ctx.globalAlpha = t.grainAlpha + t.grainAlphaStep * (i % t.grainAlphaCycle)
            ctx.beginPath()
            ctx.moveTo(base, t.grainMargin)
            for (let y = t.grainMargin; y < wood.height - t.grainMargin; y += t.grainStep) {
                const sway = Math.sin(y * wood.swayRate + i * wood.swayPhaseStep) * wood.swayDepth
                    + Math.sin(y * wood.rippleRate + i) * wood.rippleDepth
                ctx.lineTo(base + sway, y)
            }
            ctx.stroke()
        }
        ctx.globalAlpha = 1
        ctx.strokeStyle = wood.theme.woodEdge
        ctx.beginPath()
        ctx.roundedRect(wood.theme.hairline / 2, wood.theme.hairline / 2,
                        wood.width - wood.theme.hairline, wood.height - wood.theme.hairline,
                        wood.theme.radiusLg, wood.theme.radiusLg)
        ctx.stroke()
    }
    onHeightChanged: wood.requestPaint()
}
