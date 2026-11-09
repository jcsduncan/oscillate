pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// The transfer screen: output level against a steady input level, from
// -60 to 0 dBFS in and -60 to +12 dB out at one scale, through the
// compressor's own gain computer (NativeSynthDeviceController::
// press_transfer), makeup included. The unity line is dashed; the knee is
// a shaded band either side of the threshold. While the track plays, the
// operating point sits at the input peak, lowered by the reduction the
// compressor applies. Drag sideways for the threshold; scroll for the
// ratio (Shift for fine). The shared plot frame labels both axes and says
// LIVE while the track plays.
Rectangle {
    id: view
    required property var controller
    required property PressTheme theme
    property bool playing: false
    // The louder channel's input peak (dBFS) and the gain reduction (dB).
    property real inputDb: -200
    property real reductionDb: 0

    readonly property int revision: controller.revision
    readonly property real floorDb: -60
    readonly property real topDb: 12
    readonly property int points: 121
    readonly property var curve: revision >= 0 ? controller.press_transfer(floorDb, points) : []
    readonly property real threshold: revision >= 0 ? controller.value("press.threshold") : -18
    readonly property real knee: revision >= 0 ? controller.value("press.knee") : 6
    readonly property real makeup: revision >= 0 ? controller.value("press.makeup") : 0
    // The operating point, once there is an input to place.
    // Grid lines sit on pixel centres.
    readonly property real pixelCentre: 0.5
    readonly property bool pointShown: playing && inputDb > floorDb
    readonly property real pointOut: inputDb - reductionDb + makeup
    // Labelled levels on both axes, and the grid's step.
    readonly property real scaleStep: 12
    readonly property real gridStep: 6
    readonly property var xTicks: {
        const ticks = []
        for (let db = floorDb; db <= 0; db += scaleStep)
            ticks.push({ at: (db - floorDb) / (0 - floorDb), label: String(db) })
        return ticks
    }
    readonly property var yTicks: {
        const ticks = []
        for (let db = floorDb; db <= topDb; db += scaleStep)
            ticks.push({ at: (db - floorDb) / (topDb - floorDb), label: db > 0 ? "+" + db : String(db) })
        return ticks
    }

    objectName: "pressTransfer"
    radius: theme.transferRadius
    color: theme.screen
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Transfer curve"
    Accessible.description: revision < 0 ? "" : controller.display("press.ratio") + " above "
        + controller.display("press.threshold")

    // One scale on both axes: 60 dB across, 72 dB up.
    readonly property real plotX: theme.transferPlotX
    readonly property real plotY: theme.transferPlotY
    readonly property real perDb: Math.max(1, Math.min((width - plotX - theme.transferPlotRight) / (0 - floorDb),
                                                       (height - plotY - theme.transferPlotBottom) / (topDb - floorDb)))
    readonly property real plotW: perDb * (0 - floorDb)
    readonly property real plotH: perDb * (topDb - floorDb)
    function xOf(db) { return plotX + (Math.max(floorDb, Math.min(0, db)) - floorDb) * perDb }
    function yOf(db) { return plotY + (topDb - Math.max(floorDb, Math.min(topDb, db))) * perDb }
    function dbAt(x) { return Math.max(floorDb, Math.min(0, floorDb + (x - plotX) / perDb)) }
    // The curve's output at an input level, between its samples.
    function outputAt(db) {
        const c = curve
        if (c.length < 2)
            return db
        const f = (Math.max(floorDb, Math.min(0, db)) - floorDb) / (0 - floorDb) * (c.length - 1)
        const i = Math.min(c.length - 2, Math.floor(f))
        return c[i] + (c[i + 1] - c[i]) * (f - i)
    }

    Kit.PlotFrame {
        id: frame
        x: view.plotX - gutterLeft
        y: view.plotY
        width: view.plotW + gutterLeft
        height: view.plotH + gutterBottom
        theme: view.theme
        live: view.playing
        xTicks: view.xTicks
        xUnit: "dBFS"
        yTicks: view.yTicks
        yUnit: "dB out"

        // Draws in the screen's coordinates, shifted onto the frame's plot area.
        Canvas {
            id: canvas
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.translate(-view.plotX, -view.plotY)
                const t = view.theme
                const x0 = view.plotX
                const y0 = view.plotY
                const w = view.plotW
                const h = view.plotH
                // Grid every 6 dB, 0 dB stronger.
                ctx.lineWidth = t.hairline
                for (let db = view.floorDb; db <= 0; db += view.gridStep) {
                    const x = Math.round(view.xOf(db)) + view.pixelCentre
                    ctx.strokeStyle = db === 0 ? t.gridStrong : t.grid
                    ctx.beginPath()
                    ctx.moveTo(x, y0)
                    ctx.lineTo(x, y0 + h)
                    ctx.stroke()
                }
                for (let db = view.topDb; db >= view.floorDb; db -= view.gridStep) {
                    const y = Math.round(view.yOf(db)) + view.pixelCentre
                    ctx.strokeStyle = db === 0 ? t.gridStrong : t.grid
                    ctx.beginPath()
                    ctx.moveTo(x0, y)
                    ctx.lineTo(x0 + w, y)
                    ctx.stroke()
                }
                ctx.save()
                ctx.beginPath()
                ctx.rect(x0, y0, w, h)
                ctx.clip()
                // The knee, either side of the threshold.
                const a = t.knee
                if (view.knee > 0) {
                    const kx0 = view.xOf(view.threshold - view.knee / 2)
                    const kx1 = view.xOf(view.threshold + view.knee / 2)
                    ctx.fillStyle = Qt.rgba(a.r, a.g, a.b, t.kneeFillAlpha)
                    ctx.fillRect(kx0, y0, kx1 - kx0, h)
                }
                // Unity, dashed.
                ctx.setLineDash([t.unityDash, t.unityDash])
                ctx.strokeStyle = t.unity
                ctx.beginPath()
                ctx.moveTo(view.xOf(view.floorDb), view.yOf(view.floorDb))
                ctx.lineTo(view.xOf(0), view.yOf(0))
                ctx.stroke()
                // The threshold.
                const tx = Math.round(view.xOf(view.threshold)) + view.pixelCentre
                ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, t.thresholdLineAlpha)
                ctx.beginPath()
                ctx.moveTo(tx, y0)
                ctx.lineTo(tx, y0 + h)
                ctx.stroke()
                ctx.setLineDash([])
                // The curve, glowing.
                const c = view.curve
                if (c.length > 1) {
                    const acc = t.accent
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    for (let i = 0; i < c.length; ++i) {
                        const x = x0 + i / (c.length - 1) * w
                        if (i === 0)
                            ctx.moveTo(x, view.yOf(c[i]))
                        else
                            ctx.lineTo(x, view.yOf(c[i]))
                    }
                    ctx.strokeStyle = Qt.rgba(acc.r, acc.g, acc.b, t.curveGlowAlpha)
                    ctx.lineWidth = t.curveGlowWidth
                    ctx.stroke()
                    ctx.strokeStyle = Qt.rgba(acc.r, acc.g, acc.b, t.curveHaloAlpha)
                    ctx.lineWidth = t.curveHaloWidth
                    ctx.stroke()
                    ctx.strokeStyle = acc
                    ctx.lineWidth = t.curveWidth
                    ctx.stroke()
                }
                // The operating point and its cross-hair.
                if (view.pointShown) {
                    const px = view.xOf(view.inputDb)
                    const py = view.yOf(view.pointOut)
                    const p = t.operating
                    ctx.strokeStyle = Qt.rgba(p.r, p.g, p.b, t.operatingCrossAlpha)
                    ctx.lineWidth = t.hairline
                    ctx.beginPath()
                    ctx.moveTo(px, y0 + h)
                    ctx.lineTo(px, py)
                    ctx.lineTo(x0, py)
                    ctx.stroke()
                    ctx.fillStyle = Qt.rgba(p.r, p.g, p.b, t.operatingHaloAlpha)
                    ctx.beginPath()
                    ctx.arc(px, py, t.operatingHaloRadius, 0, Math.PI * 2)
                    ctx.fill()
                    ctx.fillStyle = p
                    ctx.beginPath()
                    ctx.arc(px, py, t.operatingDotRadius, 0, Math.PI * 2)
                    ctx.fill()
                }
                ctx.restore()
            }
    }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.screenReadoutRight
        y: view.theme.screenReadoutY
        spacing: view.theme.spaceLg
        Repeater {
            model: [{ id: "press.threshold", tag: "THR" }, { id: "press.ratio", tag: "RATIO" },
                    { id: "press.knee", tag: "KNEE" }]
            delegate: Row {
                id: readout
                required property var modelData
                spacing: view.theme.spaceXs
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
                    objectName: "pressScreenReadout:" + readout.modelData.id
                    text: view.revision >= 0 ? view.controller.display(readout.modelData.id) : ""
                    color: view.theme.ink
                    font.family: view.theme.mono
                    font.pixelSize: view.theme.typeBody
                    font.bold: true
                }
            }
        }
    }

    // The threshold handle on the curve.
    Rectangle {
        objectName: "pressThresholdHandle"
        // A square turned on its corner.
        readonly property real diamondTurn: 45
        readonly property real hx: view.xOf(view.threshold)
        readonly property real hy: view.yOf(view.outputAt(view.threshold))
        visible: view.threshold >= view.floorDb
        x: hx - width / 2
        y: hy - height / 2
        width: view.theme.handleSize
        height: view.theme.handleSize
        rotation: diamondTurn
        radius: view.theme.radiusXs
        color: dragArea.pressed ? view.theme.accent : view.theme.screen
        border.color: view.theme.accent
        border.width: view.theme.handleStroke
        Accessible.role: Accessible.Slider
        Accessible.name: "Threshold"
    }
    MouseArea {
        id: dragArea
        objectName: "pressTransferDrag"
        readonly property string keyboardAlternative: "the Threshold and Ratio knobs set the same parameters from the keyboard"
        x: view.plotX
        y: view.plotY
        width: view.plotW
        height: view.plotH
        enabled: view.controller.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            view.theme.lastTouched = "press.threshold"
            view.controller.begin_gesture("press.threshold")
            view.controller.set_value("press.threshold", view.dbAt(mouse.x + x))
        }
        onPositionChanged: mouse => {
            if (pressed)
                view.controller.set_value("press.threshold", view.dbAt(mouse.x + x))
        }
        onReleased: view.controller.end_gesture("press.threshold")
        onCanceled: view.controller.cancel_gesture("press.threshold")
        onWheel: wheel => {
            if (wheel.angleDelta.y !== 0) {
                view.controller.step("press.ratio", wheel.angleDelta.y > 0 ? 1 : -1,
                    (wheel.modifiers & Qt.ShiftModifier) !== 0)
                view.theme.lastTouched = "press.ratio"
            }
            wheel.accepted = true
        }
    }

    onCurveChanged: canvas.requestPaint()
    onThresholdChanged: canvas.requestPaint()
    onKneeChanged: canvas.requestPaint()
    onPointShownChanged: canvas.requestPaint()
    onInputDbChanged: canvas.requestPaint()
    onPointOutChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
