pragma ComponentBehavior: Bound
import QtQuick

// The optional filter's magnitude response on an LCD, 20 Hz to 20 kHz and
// -36 to +18 dB. Off draws a flat bypass line.
Rectangle {
    id: view
    required property var controller
    required property HaloTheme theme
    property color accent: theme.amber
    // The plotted range in decibels, and the points sampled across it.
    readonly property real topDb: 18
    readonly property real floorDb: -36
    readonly property int samples: 96
    // Grid lines at each decade, labelled along the foot.
    readonly property var decades: [{ hz: 100, text: "100" }, { hz: 1000, text: "1k" }, { hz: 10000, text: "10k" }]
    readonly property bool active: controller.revision >= 0 && controller.choice("halo.filter.type") > 0
    readonly property var response: controller.revision >= 0 && active ? controller.filter_response(samples) : []

    objectName: "haloFilterView"
    radius: theme.radiusSm
    color: theme.lcd
    border.color: theme.lcdEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: active ? "Filter response" : "Filter bypassed"

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.theme.spaceXs
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const top = view.topDb
            const bottom = view.floorDb
            const yOf = db => (top - Math.max(bottom, Math.min(top, db))) / (top - bottom) * h
            ctx.strokeStyle = view.theme.lcdDim
            ctx.lineWidth = view.theme.hairline
            for (const decade of view.decades) {
                const x = view.controller.fraction_of(decade.hz) * w
                ctx.beginPath()
                ctx.moveTo(x, 0)
                ctx.lineTo(x, h)
                ctx.stroke()
            }
            ctx.setLineDash(view.theme.zeroDash)
            ctx.beginPath()
            ctx.moveTo(0, yOf(0))
            ctx.lineTo(w, yOf(0))
            ctx.stroke()
            ctx.setLineDash([])
            const r = view.response
            if (r.length < 2)
                return
            ctx.beginPath()
            ctx.moveTo(0, h)
            for (let i = 0; i < r.length; ++i)
                ctx.lineTo(i / (r.length - 1) * w, yOf(r[i]))
            ctx.lineTo(w, h)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(view.accent.r, view.accent.g, view.accent.b, view.theme.responseFillAlpha)
            ctx.fill()
            ctx.beginPath()
            for (let i = 0; i < r.length; ++i) {
                const x = i / (r.length - 1) * w
                if (i === 0)
                    ctx.moveTo(x, yOf(r[i]))
                else
                    ctx.lineTo(x, yOf(r[i]))
            }
            ctx.strokeStyle = view.accent
            ctx.lineWidth = view.theme.responseStroke
            ctx.stroke()
        }
    }
    Text {
        visible: !view.active
        anchors.centerIn: parent
        anchors.verticalCenterOffset: view.theme.bypassOffset
        text: "BYPASS"
        color: view.theme.lcdDim
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeBody
        font.letterSpacing: view.theme.bypassSpacing
    }
    Repeater {
        model: view.decades
        delegate: Text {
            required property var modelData
            x: view.theme.spaceXs + view.controller.fraction_of(modelData.hz) * (view.width - 2 * view.theme.spaceXs)
                + view.theme.frequencyLabelGap
            y: view.height - view.theme.frequencyLabelBottom
            text: modelData.text
            color: view.theme.lcdDim
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeMicro
        }
    }
    onResponseChanged: canvas.requestPaint()
    onActiveChanged: canvas.requestPaint()
}
