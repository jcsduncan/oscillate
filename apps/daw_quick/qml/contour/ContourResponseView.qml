pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The hero display: the filter's response measured from the real effect,
// drawn on a phosphor screen from 20 Hz to 20 kHz and +24 to -48 dB. While
// the effect runs, the live curve (at the modulated cutoff) burns bright
// over the set one, and a ruler along the top shows how far the LFO (blue)
// and envelope (amber) have moved the cutoff. Drag the cutoff handle
// sideways; scroll over the screen for resonance. Focused, the arrow keys
// do the same. The frame around it (Kit.PlotFrame) draws the scales.
Rectangle {
    id: view
    required property var controller
    required property ContourTheme theme
    // The panel's decoded telemetry and the response at the live cutoff.
    property var live: ({ live: false })
    property var liveResponse: []
    readonly property bool liveOn: live.live === true && live.cutoff_hz !== undefined
    // The model and cutoff shown before the controller has values.
    readonly property int restingModel: 1
    readonly property real restingCutoff: 1200
    readonly property int model: controller.revision >= 0 ? controller.choice("contour.model") : restingModel
    // Points measured across the band for the response curve.
    readonly property int responseSamples: 240
    readonly property var response: controller.revision >= 0 ? controller.contour_response(responseSamples, 0) : []
    readonly property real cutoff: controller.revision >= 0 ? controller.value("contour.cutoff") : restingCutoff
    readonly property real topDb: 24
    readonly property real bottomDb: -48
    readonly property real lowestHz: 20
    readonly property real highestHz: 20000
    // Graticule: a dB line every dbStep, brighter every dbMajorStep.
    readonly property real dbStep: 6
    readonly property real dbMajorStep: 12
    // Where the live cutoff readout sits before telemetry arrives.
    readonly property real restingLiveHz: 1000
    readonly property var captions: [
        "STATE VARIABLE · LOW-PASS · 12 dB/OCT", "STATE VARIABLE · LOW-PASS · 24 dB/OCT",
        "STATE VARIABLE · BAND-PASS · 12 dB/OCT", "STATE VARIABLE · HIGH-PASS · 12 dB/OCT",
        "STATE VARIABLE · HIGH-PASS · 24 dB/OCT", "STATE VARIABLE · NOTCH",
        "TRANSISTOR LADDER · 24 dB/OCT", "OTA CASCADE · 24 dB/OCT",
        "SEM · LOW-PASS ↔ NOTCH ↔ HIGH-PASS", "COMB · POSITIVE FEEDBACK", "COMB · NEGATIVE FEEDBACK"]

    objectName: "contourResponseView"
    radius: theme.radiusLg
    color: theme.well
    border.color: activeFocus ? theme.accent : theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Filter response, " + captions[model].toLowerCase()
    Accessible.description: "Cutoff " + (controller.revision >= 0 ? controller.display("contour.cutoff") : "")
        + ". Left and Right set the cutoff, Up and Down the resonance"

    // Plot area inside the screen bezel.
    readonly property real plotX: theme.screenPlotLeft
    readonly property real plotY: theme.screenPlotTop
    readonly property real plotW: width - plotX - theme.screenPlotRight
    readonly property real plotH: height - plotY - theme.screenPlotBottom
    function xOf(hz) { return plotX + controller.fraction_of(hz) * plotW }
    function yOf(db) { return plotY + (topDb - Math.max(bottomDb, Math.min(topDb, db))) / (topDb - bottomDb) * plotH }
    function hzAt(x) { return controller.frequency_at(Math.max(0, Math.min(1, (x - plotX) / plotW))) }

    // The scales the frame around the screen labels, from this view's own
    // mapping: frequency marks [Hz, label] and levels in dB.
    readonly property var frequencyMarks: [[20, "20"], [50, "50"], [100, "100"], [200, "200"], [500, "500"],
                                           [1000, "1k"], [2000, "2k"], [5000, "5k"], [10000, "10k"], [20000, "20k"]]
    readonly property var levelMarks: [24, 12, 0, -12, -24, -36, -48]
    readonly property var frequencyTicks: width > 0 && controller.revision >= 0
        ? frequencyMarks.map(mark => ({ at: xOf(mark[0]) / width, label: mark[1] })) : []
    readonly property var levelTicks: height > 0
        ? levelMarks.map(db => ({ at: 1 - yOf(db) / height, label: Format.signed_number(db, 0) })) : []

    // Keys: Left/Right step the cutoff, Up/Down the resonance (Shift:
    // fine), Delete resets the cutoff.
    activeFocusOnTab: controller.editable
    Keys.onPressed: event => {
        if (!controller.editable)
            return
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            controller.step("contour.cutoff", event.key === Qt.Key_Right ? 1 : -1, fine)
            theme.lastTouched = "contour.cutoff"
        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            controller.step("contour.resonance", event.key === Qt.Key_Up ? 1 : -1, fine)
            theme.lastTouched = "contour.resonance"
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            controller.reset_to_default("contour.cutoff")
            theme.lastTouched = "contour.cutoff"
        } else {
            return
        }
        event.accepted = true
    }

    // A soft phosphor glow in the middle of the screen.
    Rectangle {
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        radius: parent.radius
        gradient: Gradient {
            GradientStop { position: 0; color: view.theme.screenGlow }
            GradientStop { position: view.theme.screenGlowStop; color: view.theme.well }
            GradientStop { position: 1; color: view.theme.screenDeep }
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const x0 = view.plotX
            const y0 = view.plotY
            const w = view.plotW
            const h = view.plotH
            const t = view.theme
            // Lines on pixel centres.
            const pixel = t.hairline / 2
            // Graticule: minor lines every 1-2-5, majors at decades; dB
            // every 6 with 0 dB brighter.
            ctx.lineWidth = t.hairline
            for (const hz of [30, 40, 50, 60, 70, 80, 90, 200, 300, 400, 500, 600, 700, 800, 900,
                              2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000]) {
                const x = Math.round(view.xOf(hz)) + pixel
                ctx.strokeStyle = view.theme.grid
                ctx.beginPath()
                ctx.moveTo(x, y0)
                ctx.lineTo(x, y0 + h)
                ctx.stroke()
            }
            for (const hz of [100, 1000, 10000]) {
                const x = Math.round(view.xOf(hz)) + pixel
                ctx.strokeStyle = view.theme.gridMajor
                ctx.beginPath()
                ctx.moveTo(x, y0)
                ctx.lineTo(x, y0 + h)
                ctx.stroke()
            }
            for (let db = view.topDb; db >= view.bottomDb; db -= view.dbStep) {
                const y = Math.round(view.yOf(db)) + pixel
                ctx.strokeStyle = db === 0 ? view.theme.accentDim : db % view.dbMajorStep === 0 ? view.theme.gridMajor
                                                                                   : view.theme.grid
                ctx.beginPath()
                ctx.moveTo(x0, y)
                ctx.lineTo(x0 + w, y)
                ctx.stroke()
            }
            const a = view.theme.accent
            const trace = (r, lineWidth, style) => {
                ctx.strokeStyle = style
                ctx.lineWidth = lineWidth
                ctx.beginPath()
                for (let i = 0; i < r.length; ++i) {
                    const x = x0 + i / (r.length - 1) * w
                    if (i === 0)
                        ctx.moveTo(x, view.yOf(r[i]))
                    else
                        ctx.lineTo(x, view.yOf(r[i]))
                }
                ctx.stroke()
            }
            const area = (r, alpha) => {
                const fill = ctx.createLinearGradient(0, y0, 0, y0 + h)
                fill.addColorStop(0, Qt.rgba(a.r, a.g, a.b, alpha))
                fill.addColorStop(1, Qt.rgba(a.r, a.g, a.b, 0))
                ctx.beginPath()
                ctx.moveTo(x0, y0 + h)
                for (let i = 0; i < r.length; ++i)
                    ctx.lineTo(x0 + i / (r.length - 1) * w, view.yOf(r[i]))
                ctx.lineTo(x0 + w, y0 + h)
                ctx.closePath()
                ctx.fillStyle = fill
                ctx.fill()
            }
            ctx.lineJoin = "round"
            const set = view.response
            const liveCurve = view.liveOn && view.liveResponse.length > 1 ? view.liveResponse : null
            if (set.length > 1) {
                if (liveCurve) {
                    // The set curve recedes to a thin trace behind the live one.
                    ctx.setLineDash(t.screenSetDash)
                    trace(set, t.hairline, Qt.rgba(a.r, a.g, a.b, t.screenSetTraceAlpha))
                    ctx.setLineDash([])
                } else {
                    area(set, t.screenSetAreaAlpha)
                    trace(set, t.screenSetHaloWidth, Qt.rgba(a.r, a.g, a.b, t.screenSetHaloAlpha))
                    trace(set, t.screenSetGlowWidth, Qt.rgba(a.r, a.g, a.b, t.screenSetGlowAlpha))
                    trace(set, t.screenSetLineWidth, a)
                }
            }
            if (liveCurve) {
                area(liveCurve, t.screenLiveAreaAlpha)
                trace(liveCurve, t.screenLiveHaloWidth, Qt.rgba(a.r, a.g, a.b, t.screenLiveHaloAlpha))
                trace(liveCurve, t.screenLiveGlowWidth, Qt.rgba(a.r, a.g, a.b, t.screenLiveGlowAlpha))
                trace(liveCurve, t.screenLiveLineWidth, t.liveTrace)
            }
            // Cutoff marker.
            const cx = Math.round(view.xOf(view.cutoff)) + pixel
            ctx.setLineDash(t.screenCutoffDash)
            ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, t.screenCutoffAlpha)
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(cx, y0)
            ctx.lineTo(cx, y0 + h)
            ctx.stroke()
            ctx.setLineDash([])
            // Modulation ruler: set cutoff -> LFO (blue) -> envelope (amber).
            if (view.liveOn) {
                const lfo = view.live.lfo !== undefined ? view.live.lfo : 0
                const lfoHz = view.cutoff * Math.pow(2, lfo / 12)
                const liveHz = view.live.cutoff_hz
                const ry = y0 - t.screenRulerOffset
                const span = (fromHz, toHz, colour) => {
                    const xa = view.xOf(Math.max(view.lowestHz, Math.min(view.highestHz, fromHz)))
                    const xb = view.xOf(Math.max(view.lowestHz, Math.min(view.highestHz, toHz)))
                    if (Math.abs(xb - xa) < 1)
                        return
                    ctx.strokeStyle = colour
                    ctx.lineWidth = t.screenRulerStroke
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.moveTo(xa, ry)
                    ctx.lineTo(xb, ry)
                    ctx.stroke()
                    ctx.lineCap = "butt"
                }
                span(view.cutoff, lfoHz, view.theme.lfo)
                span(lfoHz, liveHz, view.theme.envelope)
                const lx = Math.round(view.xOf(liveHz)) + pixel
                ctx.strokeStyle = t.liveTrace
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                ctx.moveTo(lx, ry - t.screenRulerTickRise)
                ctx.lineTo(lx, y0 + h)
                ctx.stroke()
            }
        }
    }

    // Header lines on the screen.
    Text {
        objectName: "contourModelCaption"
        x: view.theme.screenCaptionX
        y: view.theme.screenCaptionY
        text: view.captions[view.model]
        color: view.theme.accent
        font.pixelSize: view.theme.typeSmall
        font.bold: true
        font.letterSpacing: view.theme.screenCaptionSpacing
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.screenReadoutRight
        y: view.theme.screenReadoutY
        spacing: view.theme.screenReadoutSpacing
        Repeater {
            model: [{ id: "contour.cutoff", tag: "FC" }, { id: "contour.resonance", tag: "RES" },
                    { id: "contour.drive", tag: "DRV" }]
            delegate: Row {
                id: readout
                required property var modelData
                spacing: view.theme.screenReadoutGap
                Text {
                    anchors.baseline: number.baseline
                    text: readout.modelData.tag
                    color: view.theme.inkFaint
                    font.pixelSize: view.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: view.theme.tagSpacing
                }
                Text {
                    id: number
                    objectName: "contourScreenReadout:" + readout.modelData.id
                    text: view.controller.revision >= 0 ? view.controller.display(readout.modelData.id) : ""
                    color: view.theme.ink
                    font.family: view.theme.mono
                    font.pixelSize: view.theme.typeLabel
                    font.bold: true
                }
            }
        }
    }
    Text {
        objectName: "contourLiveCutoff"
        visible: view.liveOn
        x: Math.max(view.plotX, Math.min(view.xOf(view.liveOn ? view.live.cutoff_hz : view.restingLiveHz) + view.theme.spaceSm,
                                         view.plotX + view.plotW - implicitWidth))
        y: view.plotY + view.theme.spaceXs
        text: "LIVE " + (view.liveOn ? Format.hertz(view.live.cutoff_hz) : "")
        color: view.theme.liveTrace
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeCaption
        font.bold: true
    }

    // The cutoff handle: drag sideways to set the cutoff.
    Rectangle {
        id: handle
        objectName: "contourCutoffHandle"
        readonly property real hx: view.xOf(view.cutoff)
        readonly property real hy: {
            const r = view.response
            if (r.length < 2)
                return view.yOf(0)
            const f = Math.max(0, Math.min(1, (hx - view.plotX) / view.plotW))
            return view.yOf(r[Math.round(f * (r.length - 1))])
        }
        x: hx - width / 2
        y: Math.max(view.plotY - view.theme.spaceXxs,
                    Math.min(view.plotY + view.plotH - height + view.theme.spaceXxs, hy - height / 2))
        width: view.theme.cutoffHandleSize
        height: view.theme.cutoffHandleSize
        // A diamond: the square turned an eighth of a turn.
        rotation: 45
        radius: view.theme.radiusXs
        color: handleArea.pressed ? view.theme.accent : view.theme.well
        border.color: view.theme.accent
        border.width: view.theme.cutoffHandleStroke
        Accessible.role: Accessible.Slider
        Accessible.name: "Cutoff"
    }
    MouseArea {
        id: handleArea
        objectName: "contourCutoffDrag"
        x: view.plotX
        y: view.plotY
        width: view.plotW
        height: view.plotH
        enabled: view.controller.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            view.theme.lastTouched = "contour.cutoff"
            view.controller.begin_gesture("contour.cutoff")
            view.controller.set_value("contour.cutoff", view.hzAt(mouse.x + x))
        }
        onPositionChanged: mouse => {
            if (pressed)
                view.controller.set_value("contour.cutoff", view.hzAt(mouse.x + x))
        }
        onReleased: view.controller.end_gesture("contour.cutoff")
        onCanceled: view.controller.cancel_gesture("contour.cutoff")
        onWheel: wheel => {
            if (wheel.angleDelta.y !== 0) {
                view.controller.step("contour.resonance", wheel.angleDelta.y > 0 ? 1 : -1,
                    (wheel.modifiers & Qt.ShiftModifier) !== 0)
                view.theme.lastTouched = "contour.resonance"
            }
            wheel.accepted = true
        }
    }

    onResponseChanged: canvas.requestPaint()
    onLiveResponseChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onCutoffChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
