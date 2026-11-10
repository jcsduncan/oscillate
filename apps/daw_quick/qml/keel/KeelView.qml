import QtQuick

// Keel's balance view, third-octave bands from 20 Hz to 20 kHz on a log
// axis, each centred on its K-weighted mean so only the shape shows: the
// tracked balance as a grey ghost, the target in the accent (dashed when it
// is drawn ahead of any reading), the correction as bars from the 0 dB line
// (lifts in amber, cuts in sky) and the balance Keel lets out (the tracked
// balance plus the correction) in near-white. With the target on Curve the
// drawn curve's handles show and the view edits them: drag a band up or
// down, or choose one with Left and Right and move it with Up and Down
// (Shift for a tenth of a decibel). It fills its Kit.PlotFrame's plot area.
Item {
    id: view
    required property KeelTheme theme
    required property var controller
    // Per band (dB, -200 where nothing reads): the tracked balance, the
    // target and the correction now.
    property var balance: []
    property var target: []
    property var correction: []
    property bool live: false
    // The drawn curve's handles: the curve's parameters, low to high, and
    // where each sits on the view (dB).
    property var curveIds: []
    property var handles: []
    // The bands' centres (Hz, keel::band_hz) and the 1 kHz band.
    property var bandHz: []
    property int referenceBand: 0
    property bool editable: false
    property int chosen: referenceBand

    readonly property int bands: bandHz.length
    readonly property real lowestHz: 20
    readonly property real highestHz: 20000
    readonly property real rangeDb: 18
    readonly property real emptyDb: -150
    readonly property real keyStepDb: 0.5
    readonly property real fineStepDb: 0.1
    readonly property var dbTicks: [18, 12, 6, 0, -6, -12, -18]
    readonly property var hzTicks: [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]

    // Where a frequency sits from the left (0) to the right (1).
    function hzAt(hz) {
        return Math.max(0, Math.min(1, Math.log(hz / lowestHz) / Math.log(highestHz / lowestHz)))
    }
    // Where a level sits from the bottom (0) to the top (1).
    function dbAt(db) {
        return Math.max(0, Math.min(1, 0.5 + db / (2 * rangeDb)))
    }
    // The band whose centre lies nearest x.
    function bandAtX(x) {
        const at = Math.max(0, Math.min(1, x / width))
        let best = 0
        for (let b = 1; b < bands; ++b) {
            if (Math.abs(hzAt(bandHz[b]) - at) < Math.abs(hzAt(bandHz[best]) - at))
                best = b
        }
        return best
    }

    onBalanceChanged: canvas.requestPaint()
    onTargetChanged: canvas.requestPaint()
    onCorrectionChanged: canvas.requestPaint()
    onHandlesChanged: canvas.requestPaint()
    onEditableChanged: canvas.requestPaint()
    onChosenChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    activeFocusOnTab: editable
    Accessible.role: Accessible.Graphic
    Accessible.name: editable ? "Keel target curve: Left and Right choose a band, Up and Down move it"
                              : "Keel balance"

    // Nudges the chosen band's curve value.
    function nudge(db) {
        const id = curveIds[chosen]
        if (!editable || id === undefined)
            return
        controller.set_value(id, controller.value(id) + db)
    }
    Keys.onLeftPressed: chosen = Math.max(0, chosen - 1)
    Keys.onRightPressed: chosen = Math.min(bands - 1, chosen + 1)
    Keys.onUpPressed: event => nudge(event.modifiers & Qt.ShiftModifier ? fineStepDb : keyStepDb)
    Keys.onDownPressed: event => nudge(-(event.modifiers & Qt.ShiftModifier ? fineStepDb : keyStepDb))

    Rectangle {
        anchors.fill: parent
        color: view.theme.screen
        border.color: view.activeFocus ? view.theme.accent : "transparent"
    }
    Canvas {
        id: canvas
        objectName: "keelViewCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            const xOf = band => w * view.hzAt(view.bandHz[band])
            const yOf = db => h * (1 - view.dbAt(db))
            ctx.lineWidth = t.hairline
            ctx.strokeStyle = t.grid
            for (const db of view.dbTicks) {
                ctx.beginPath()
                ctx.moveTo(0, yOf(db))
                ctx.lineTo(w, yOf(db))
                ctx.stroke()
            }
            for (const hz of view.hzTicks) {
                ctx.beginPath()
                ctx.moveTo(w * view.hzAt(hz), 0)
                ctx.lineTo(w * view.hzAt(hz), h)
                ctx.stroke()
            }
            ctx.strokeStyle = t.centre
            ctx.beginPath()
            ctx.moveTo(0, yOf(0))
            ctx.lineTo(w, yOf(0))
            ctx.stroke()

            const valid = (curve, band) => curve.length > band && curve[band] > view.emptyDb
            const line = (curve, offset) => {
                ctx.beginPath()
                let started = false
                for (let b = 0; b < view.bands; ++b) {
                    if (!valid(curve, b)) {
                        started = false
                        continue
                    }
                    const y = yOf(curve[b] + (offset.length > b ? offset[b] : 0))
                    if (!started)
                        ctx.moveTo(xOf(b), y)
                    else
                        ctx.lineTo(xOf(b), y)
                    started = true
                }
                ctx.stroke()
            }

            // The correction's bars, a band wide.
            const slot = w / (view.bands + 1)
            ctx.globalAlpha = t.correctionAlpha
            for (let b = 0; b < Math.min(view.bands, view.correction.length); ++b) {
                const db = view.correction[b]
                if (db === 0)
                    continue
                ctx.fillStyle = db > 0 ? t.boost : t.cut
                const top = Math.min(yOf(db), yOf(0))
                ctx.fillRect(xOf(b) - slot / 2, top, slot - t.hairline, Math.abs(yOf(db) - yOf(0)))
            }
            ctx.globalAlpha = 1

            // The tracked balance: a ghost under what Keel lets out.
            ctx.lineJoin = "round"
            ctx.globalAlpha = t.heldAlpha
            ctx.lineWidth = t.traceStroke
            ctx.strokeStyle = t.held
            line(view.balance, [])
            ctx.globalAlpha = 1
            ctx.strokeStyle = t.output
            ctx.lineWidth = t.traceStroke
            line(view.balance, view.correction)

            // The target, dashed when it is drawn ahead of a reading.
            ctx.strokeStyle = t.accent
            ctx.lineWidth = t.targetStroke
            if (!view.live)
                ctx.setLineDash([t.dashOn, t.dashOff])
            line(view.target, [])
            ctx.setLineDash([])

            if (view.editable) {
                for (let b = 0; b < Math.min(view.bands, view.handles.length); ++b) {
                    ctx.fillStyle = b === view.chosen ? t.accent : t.plate
                    ctx.strokeStyle = t.accent
                    ctx.lineWidth = t.hairline
                    ctx.beginPath()
                    ctx.arc(xOf(b), yOf(view.handles[b]), t.handleRadius, 0, 2 * Math.PI)
                    ctx.fill()
                    ctx.stroke()
                }
                if (view.handles.length > view.chosen) {
                    ctx.strokeStyle = t.accent
                    ctx.beginPath()
                    ctx.arc(xOf(view.chosen), yOf(view.handles[view.chosen]), t.handleRing, 0, 2 * Math.PI)
                    ctx.stroke()
                }
            }
        }
    }

    // Drag a band of the drawn curve: the nearest band to the press, moved
    // by as many decibels as the pointer moves, as one edit.
    MouseArea {
        id: area
        objectName: "keelCurveArea"
        anchors.fill: parent
        enabled: view.editable
        visible: view.editable
        cursorShape: Qt.SizeVerCursor
        property string dragging: ""
        property real startY: 0
        property real startValue: 0
        onPressed: mouse => {
            view.forceActiveFocus()
            view.chosen = view.bandAtX(mouse.x)
            dragging = view.curveIds[view.chosen]
            startY = mouse.y
            startValue = view.controller.value(dragging)
            view.controller.begin_gesture(dragging)
        }
        onPositionChanged: mouse => {
            if (dragging === "")
                return
            const db = (startY - mouse.y) / view.height * 2 * view.rangeDb
            view.controller.set_value(dragging, startValue + db)
        }
        onReleased: {
            if (dragging !== "")
                view.controller.end_gesture(dragging)
            dragging = ""
        }
        onCanceled: {
            if (dragging !== "")
                view.controller.cancel_gesture(dragging)
            dragging = ""
        }
    }
}
