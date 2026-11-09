pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// What sets the selected pad's sound, from the Pulse kernel's laws: the swept
// body's pitch drop for kicks and toms (controller.pad_pitch), otherwise the
// noise or metal band its filters carve (controller.pad_band).
Rectangle {
    id: view
    required property var controller
    required property PulseTheme theme
    required property int pad
    property color accent: theme.yellow

    readonly property real sweepSeconds: 0.3
    readonly property int pitchSamples: 120
    readonly property int bandSamples: 160
    readonly property var pitch: controller.revision >= 0
        ? controller.pad_pitch(pad, sweepSeconds, pitchSamples) : []
    readonly property bool pitched: pitch.length > 0
    readonly property var band: controller.revision >= 0 && !pitched
        ? controller.pad_band(pad, bandSamples) : []
    // Pitch axis 20 Hz..2 kHz; band axis -36..+12 dB.
    readonly property real lowHz: 20
    readonly property real highHz: 2000
    readonly property real bandFloorDb: -36
    readonly property real bandCeilingDb: 12
    readonly property var pitchGridHz: [50, 100, 200, 500, 1000]
    readonly property var pitchLabelHz: [50, 200, 1000]
    readonly property var bandGridHz: [100, 1000, 10000]
    readonly property int bandGridRows: 4
    // The frame's scale: the sweep's time in ms, or the band's frequency.
    readonly property real msPerSecond: 1000
    readonly property var sweepTickMs: [0, 100, 200, 300]
    readonly property var bandTickText: ["100", "1k", "10k"]
    readonly property string timeUnit: pitched ? "ms" : "Hz"
    // A share across the plot, as a share of the view's width.
    function frameAt(f) {
        const inset = theme.hairline + plotLeft
        return width <= 0 ? f : (inset + f * (width - 2 * inset)) / width
    }
    readonly property var timeTicks: pitched
        ? sweepTickMs.map(ms => ({ at: frameAt(ms / (sweepSeconds * msPerSecond)), label: "" + ms }))
        : bandGridHz.map((hz, i) => ({ at: frameAt(controller.fraction_of(hz)), label: bandTickText[i] }))
    // The plot inside the canvas, which sits one hairline in from the frame.
    readonly property real plotLeft: theme.envelopePlotSide
    readonly property real plotTop: theme.envelopePlotTop
    readonly property real plotBottom: theme.envelopePlotBottom

    objectName: "pulseShape"
    radius: theme.radiusMd
    color: theme.lcd
    border.color: theme.stripLine
    Accessible.role: Accessible.Graphic
    Accessible.name: pitched ? "Pitch drop" : "Noise band"
    Accessible.description: pitched ? Format.hertz(pitch[0]) + " falling to " + Format.hertz(pitch[pitch.length - 1])
                                    : "Filter shape of the pad's noise"

    function pitchY(value, top, h) {
        const f = Math.log(Math.max(value, lowHz) / lowHz) / Math.log(highHz / lowHz)
        return top + h * (1 - Math.min(1, f))
    }

    onPitchChanged: canvas.requestPaint()
    onBandChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const left = view.plotLeft
            const top = view.plotTop
            const w = width - 2 * left
            const h = height - top - view.plotBottom
            const snap = view.theme.hairline / 2
            ctx.strokeStyle = view.theme.lcdGrid
            ctx.lineWidth = view.theme.hairline
            if (view.pitched) {
                for (const f of view.pitchGridHz) {
                    const y = Math.round(view.pitchY(f, top, h)) + snap
                    ctx.beginPath()
                    ctx.moveTo(left, y)
                    ctx.lineTo(left + w, y)
                    ctx.stroke()
                }
            } else {
                for (const f of view.bandGridHz) {
                    const x = Math.round(left + w * view.controller.fraction_of(f)) + snap
                    ctx.beginPath()
                    ctx.moveTo(x, top)
                    ctx.lineTo(x, top + h)
                    ctx.stroke()
                }
                for (let i = 0; i <= view.bandGridRows; ++i) {
                    const y = Math.round(top + h * i / view.bandGridRows) + snap
                    ctx.beginPath()
                    ctx.moveTo(left, y)
                    ctx.lineTo(left + w, y)
                    ctx.stroke()
                }
            }
            const p = view.pitched ? view.pitch : view.band
            if (p.length < 2)
                return
            const yAt = i => view.pitched ? view.pitchY(p[i], top, h)
                : top + h * (view.bandCeilingDb - Math.max(view.bandFloorDb, Math.min(view.bandCeilingDb, p[i])))
                  / (view.bandCeilingDb - view.bandFloorDb)
            const grad = ctx.createLinearGradient(0, top, 0, top + h)
            grad.addColorStop(0, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, view.theme.shapeFillAlpha))
            grad.addColorStop(1, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, view.theme.shapeFillEndAlpha))
            ctx.beginPath()
            ctx.moveTo(left, top + h)
            for (let i = 0; i < p.length; ++i)
                ctx.lineTo(left + w * i / (p.length - 1), yAt(i))
            ctx.lineTo(left + w, top + h)
            ctx.closePath()
            ctx.fillStyle = grad
            ctx.fill()
            ctx.beginPath()
            for (let i = 0; i < p.length; ++i) {
                const x = left + w * i / (p.length - 1)
                if (i === 0)
                    ctx.moveTo(x, yAt(i))
                else
                    ctx.lineTo(x, yAt(i))
            }
            ctx.strokeStyle = view.accent
            ctx.lineWidth = view.theme.envelopeLineWidth
            ctx.lineJoin = view.theme.envelopeLineJoin
            ctx.stroke()
        }
    }

    Text {
        id: caption
        x: view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: view.pitched ? "PITCH DROP" : "NOISE BAND"
        color: view.theme.envelopeLabelInk
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
        font.letterSpacing: view.theme.envelopeLabelSpacing
    }
    // Beside the caption, clear of the frame's PREVIEW chip.
    Text {
        x: caption.x + caption.width + view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        visible: view.pitched
        text: view.pitched ? Format.hertz(view.pitch[0]) + " → " + Format.hertz(view.pitch[view.pitch.length - 1]) : ""
        color: view.accent
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
    }
    Repeater {
        model: view.pitched ? view.pitchLabelHz : []
        delegate: Text {
            required property int modelData
            x: view.width - implicitWidth - view.theme.shapeAxisInset
            y: view.theme.hairline + view.pitchY(modelData, view.plotTop,
                view.height - 2 * view.theme.hairline - view.plotTop - view.plotBottom) - implicitHeight
            text: modelData >= 1000 ? (modelData / 1000) + "k" : modelData
            color: view.theme.envelopeLabelInk
            font.pixelSize: view.theme.typeMicro
        }
    }
}
