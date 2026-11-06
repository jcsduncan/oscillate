pragma ComponentBehavior: Bound
import QtQuick

// A filter at its knob's cutoff, 20 Hz to 20 kHz and -42 to +24 dB, on a
// graticule, with the cutoff marked and the filter's name (`caption`)
// above. Modulation moves the real cutoff from here.
Rectangle {
    id: view
    required property var controller
    required property KitTheme theme
    required property string caption
    property string cutoffId: theme.prefix + ".filter.cutoff"
    property color accent: theme.accent
    // Points sampled across the response.
    readonly property int responsePoints: 200
    // The graticule: frequency lines (Hz) and level lines (dB).
    readonly property var gridHz: [50, 100, 200, 500, 1000, 2000, 5000, 10000]
    readonly property var gridDb: [12, 0, -12, -24, -36]
    readonly property var response: controller.revision >= 0 ? controller.filter_response(responsePoints) : []
    readonly property real cutoff: controller.revision >= 0 ? controller.value(cutoffId) : 1000

    objectName: theme.prefix + "FilterView"
    radius: theme.viewRadius
    color: theme.well
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Filter response, " + caption.toLowerCase()

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.filterViewMargin
        anchors.topMargin: view.theme.filterViewTop
        anchors.bottomMargin: view.theme.filterViewBottom
        // Strokes sit on pixel centres.
        readonly property real pixelCentre: 0.5
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const top = view.theme.filterViewCeilingDb
            const bottom = view.theme.filterViewFloorDb
            const yOf = db => (top - Math.max(bottom, Math.min(top, db))) / (top - bottom) * h
            ctx.strokeStyle = view.theme.grid
            ctx.lineWidth = view.theme.hairline
            for (const hz of view.gridHz) {
                const x = Math.round(view.controller.fraction_of(hz) * w) + pixelCentre
                ctx.beginPath()
                ctx.moveTo(x, 0)
                ctx.lineTo(x, h)
                ctx.stroke()
            }
            for (const db of view.gridDb) {
                const y = Math.round(yOf(db)) + pixelCentre
                ctx.strokeStyle = db === 0 ? view.theme.inkFaint : view.theme.grid
                ctx.beginPath()
                ctx.moveTo(0, y)
                ctx.lineTo(w, y)
                ctx.stroke()
            }
            const r = view.response
            if (r.length < 2)
                return
            const a = view.accent
            const fill = ctx.createLinearGradient(0, 0, 0, h)
            fill.addColorStop(0, Qt.rgba(a.r, a.g, a.b, view.theme.filterViewFillAlpha))
            fill.addColorStop(1, Qt.rgba(a.r, a.g, a.b, view.theme.filterViewFillEndAlpha))
            ctx.beginPath()
            ctx.moveTo(0, h)
            for (let i = 0; i < r.length; ++i)
                ctx.lineTo(i / (r.length - 1) * w, yOf(r[i]))
            ctx.lineTo(w, h)
            ctx.closePath()
            ctx.fillStyle = fill
            ctx.fill()
            const trace = () => {
                ctx.beginPath()
                for (let i = 0; i < r.length; ++i) {
                    const x = i / (r.length - 1) * w
                    if (i === 0)
                        ctx.moveTo(x, yOf(r[i]))
                    else
                        ctx.lineTo(x, yOf(r[i]))
                }
                ctx.stroke()
            }
            ctx.lineJoin = "round"
            ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, view.theme.filterViewGlowAlpha)
            ctx.lineWidth = view.theme.filterViewGlowWidth
            trace()
            ctx.strokeStyle = a
            ctx.lineWidth = view.theme.strokeBold
            trace()
            // The cutoff marker.
            const cx = Math.round(view.controller.fraction_of(view.cutoff) * w) + pixelCentre
            ctx.setLineDash(view.theme.filterViewMarkerDash)
            ctx.strokeStyle = view.theme.ink
            ctx.globalAlpha = view.theme.filterViewMarkerOpacity
            ctx.lineWidth = view.theme.hairline
            ctx.beginPath()
            ctx.moveTo(cx, 0)
            ctx.lineTo(cx, h)
            ctx.stroke()
            ctx.setLineDash([])
            ctx.globalAlpha = 1
        }
    }
    Text {
        x: view.theme.filterViewTextInset
        y: view.theme.filterViewCaptionY
        text: view.caption
        color: view.accent
        font.pixelSize: view.theme.typeCaption
        font.bold: true
        font.letterSpacing: view.theme.filterViewCaptionSpacing
    }
    Text {
        objectName: view.theme.prefix + "CutoffReadout"
        anchors.right: parent.right
        anchors.rightMargin: view.theme.filterViewTextInset
        y: view.theme.filterViewReadoutY
        text: view.controller.revision >= 0 ? view.controller.display(view.cutoffId) : ""
        color: view.theme.ink
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeSmall
        font.bold: true
    }
    Repeater {
        model: [{ hz: 100, text: "100" }, { hz: 1000, text: "1k" }, { hz: 10000, text: "10k" }]
        delegate: Text {
            required property var modelData
            x: view.theme.filterViewMargin
                + view.controller.fraction_of(modelData.hz) * (view.width - 2 * view.theme.filterViewMargin)
                + view.theme.filterViewTickLabelOffset
            y: view.height - view.theme.filterViewTickLabelLift
            text: modelData.text
            color: view.theme.inkFaint
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeMicro
        }
    }
    onResponseChanged: canvas.requestPaint()
    onAccentChanged: canvas.requestPaint()
    onCutoffChanged: canvas.requestPaint()
}
