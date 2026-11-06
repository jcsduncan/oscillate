import QtQuick

// Small drawn glyph for a waveform or LFO shape. kind is "saw", "pulse",
// "triangle", "sine", "saw_down", "square" or "sample_hold".
Canvas {
    id: icon
    required property PrismTheme theme
    required property color stroke
    property string kind: "saw"
    // Where the triangle peaks and troughs, and the sample-and-hold steps,
    // as shares of the icon's width and height.
    readonly property real trianglePeak: 0.25
    readonly property real triangleTrough: 0.75
    readonly property var holdSteps: [0.3, 0.85, 0.15, 0.6, 0.4]
    implicitWidth: theme.iconWidth
    implicitHeight: theme.iconHeight
    onKindChanged: requestPaint()
    onStrokeChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const w = width
        const h = height
        const side = theme.iconInsetX
        const top = theme.iconInsetY
        const bottom = h - theme.iconInsetY
        const mid = h / 2
        ctx.strokeStyle = stroke
        ctx.lineWidth = theme.iconStroke
        ctx.lineJoin = "round"
        ctx.lineCap = "round"
        ctx.beginPath()
        if (kind === "saw") {
            ctx.moveTo(side, bottom); ctx.lineTo(w / 2, top); ctx.lineTo(w / 2, bottom)
            ctx.lineTo(w - side, top)
        } else if (kind === "saw_down") {
            ctx.moveTo(side, top); ctx.lineTo(w / 2, bottom); ctx.lineTo(w / 2, top)
            ctx.lineTo(w - side, bottom)
        } else if (kind === "pulse" || kind === "square") {
            ctx.moveTo(side, bottom); ctx.lineTo(side, top); ctx.lineTo(w / 2, top)
            ctx.lineTo(w / 2, bottom); ctx.lineTo(w - side, bottom); ctx.lineTo(w - side, top)
        } else if (kind === "triangle") {
            ctx.moveTo(side, mid); ctx.lineTo(w * trianglePeak, top); ctx.lineTo(w * triangleTrough, bottom)
            ctx.lineTo(w - side, mid)
        } else if (kind === "sample_hold") {
            const steps = holdSteps
            const step = (w - 2 * side) / steps.length
            ctx.moveTo(side, top + steps[0] * (bottom - top))
            for (let i = 0; i < steps.length; ++i) {
                const y = top + steps[i] * (bottom - top)
                ctx.lineTo(side + i * step, y)
                ctx.lineTo(side + (i + 1) * step, y)
            }
        } else {
            for (let x = 0; x <= w - 2 * side; ++x) {
                const y = mid - Math.sin(x / (w - 2 * side) * Math.PI * 2) * (mid - top)
                if (x === 0)
                    ctx.moveTo(side + x, y)
                else
                    ctx.lineTo(side + x, y)
            }
        }
        ctx.stroke()
    }
}
