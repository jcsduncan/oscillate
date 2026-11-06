pragma ComponentBehavior: Bound
import QtQuick

// The filter's analog-prototype response on a dim oscilloscope-style grid,
// with the cutoff marked. Drag horizontally to move the cutoff; a drag is
// one undo step. `oversample` samples the response that many times denser
// and peak-holds it down, so narrow comb teeth keep their tops.
Rectangle {
    id: view
    required property var controller
    required property KitTheme theme
    property color accent: theme.accent
    property string cutoffId: theme.prefix + ".filter.cutoff"
    property int points: 160
    property int oversample: 1
    readonly property var response: {
        if (controller.revision < 0)
            return []
        const dense = controller.filter_response(points * oversample)
        if (oversample === 1)
            return dense
        const out = []
        for (let i = 0; i + oversample - 1 < dense.length; i += oversample) {
            let peak = dense[i]
            for (let k = 1; k < oversample; ++k)
                peak = Math.max(peak, dense[i + k])
            out.push(peak)
        }
        return out
    }
    // The vertical grid's frequencies (Hz).
    readonly property var gridHz: [100, 1000, 10000]
    readonly property real cutoffFraction: controller.revision >= 0
        ? controller.fraction_of(controller.value(cutoffId)) : 0

    objectName: theme.prefix + "FilterView"
    radius: theme.viewRadius
    color: theme.well
    border.color: theme.viewEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Filter response"

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        // Strokes sit on pixel centres.
        readonly property real pixelCentre: 0.5
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            const top = view.theme.scopeTopPad
            const ceiling = view.theme.scopeCeilingDb
            const floor = view.theme.scopeFloorDb
            const db = value => top + (h - top - view.theme.scopeBottomPad)
                * (ceiling - Math.max(floor, Math.min(ceiling, value))) / (ceiling - floor)
            ctx.strokeStyle = view.theme.viewGrid
            ctx.lineWidth = view.theme.hairline
            ctx.globalAlpha = view.theme.scopeGridOpacity
            for (const hz of view.gridHz) {
                const x = Math.round(view.controller.fraction_of(hz) * w) + pixelCentre
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke()
            }
            const zero = Math.round(db(0)) + pixelCentre
            ctx.beginPath(); ctx.moveTo(0, zero); ctx.lineTo(w, zero); ctx.stroke()
            ctx.globalAlpha = 1
            const response = view.response
            if (response.length < 2)
                return
            ctx.beginPath()
            ctx.moveTo(0, db(response[0]))
            for (let i = 1; i < response.length; ++i)
                ctx.lineTo(i / (response.length - 1) * w, db(response[i]))
            const line = ctx.createLinearGradient(0, 0, w, 0)
            ctx.lineTo(w, h); ctx.lineTo(0, h); ctx.closePath()
            line.addColorStop(0, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, view.theme.scopeFillAlpha))
            line.addColorStop(1, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, view.theme.scopeFillEndAlpha))
            ctx.fillStyle = line
            ctx.fill()
            ctx.beginPath()
            ctx.moveTo(0, db(response[0]))
            for (let i = 1; i < response.length; ++i)
                ctx.lineTo(i / (response.length - 1) * w, db(response[i]))
            ctx.strokeStyle = view.accent
            ctx.lineWidth = view.theme.scopeLineWidth
            ctx.stroke()
            const cx = view.cutoffFraction * w
            ctx.fillStyle = view.theme.viewMarker
            ctx.beginPath()
            ctx.arc(cx, db(response[Math.round(view.cutoffFraction * (response.length - 1))]), view.theme.scopeMarkerRadius, 0, Math.PI * 2)
            ctx.fill()
        }
        Connections {
            target: view
            function onResponseChanged() { plot.requestPaint() }
        }
    }
    Text {
        x: view.theme.scopeLabelX
        y: view.theme.scopeLabelY
        text: view.controller.revision >= 0 ? view.controller.display(view.cutoffId) : ""
        color: view.theme.viewLabelInk
        font.pixelSize: view.theme.typeCaption
        font.bold: true
    }
    MouseArea {
        anchors.fill: parent
        enabled: view.controller.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            view.controller.begin_gesture(view.cutoffId)
            view.controller.set_value(view.cutoffId,
                view.controller.frequency_at(Math.max(0, Math.min(1, mouse.x / width))))
        }
        onPositionChanged: mouse => {
            if (pressed)
                view.controller.set_value(view.cutoffId,
                    view.controller.frequency_at(Math.max(0, Math.min(1, mouse.x / width))))
        }
        onReleased: view.controller.end_gesture(view.cutoffId)
        onCanceled: view.controller.cancel_gesture(view.cutoffId)
    }
}
