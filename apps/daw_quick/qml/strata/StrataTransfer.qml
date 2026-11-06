pragma ComponentBehavior: Bound
import QtQuick

// The selected band's transfer curve, from the engine's own gain computer
// (strata_transfer): input across, output up, both -60..0 dB. Three handles
// shape it: the threshold point slides along the input; the "above" handle
// at full scale sets the ratio over the threshold, the "below" handle at the
// floor the ratio under it (up lifts, down pushes). Double-click resets both
// ratios. A live dot rides the curve at the band's detector level.
Item {
    id: view
    required property var panel
    required property var controller
    required property StrataTheme theme
    required property int band

    readonly property real floorDb: -60
    readonly property int points: 121
    readonly property int revision: controller.revision
    readonly property var curve: revision >= 0 ? controller.strata_transfer(band, floorDb, points) : []
    readonly property real threshold: revision >= 0 ? panel.bandValue(band, "threshold") : -18
    readonly property color colour: theme.band(band)
    readonly property bool playing: panel.live.live === true
    readonly property real level: playing && panel.live.band_level ? panel.live.band_level[band] : -200
    readonly property bool pointShown: playing && level > floorDb
    readonly property real pointOut: outputAt(level)
    // Grid lines sit on pixel centres.
    readonly property real pixelCentre: 0.5

    objectName: "strataTransfer"

    function xOf(db) { return width * (db - floorDb) / -floorDb }
    function yOf(db) { return height * (1 - (Math.max(floorDb, Math.min(0, db)) - floorDb) / -floorDb) }
    function dbOfX(x) { return floorDb + -floorDb * Math.max(0, Math.min(1, x / width)) }
    function dbOfY(y) { return floorDb + -floorDb * (1 - Math.max(0, Math.min(1, y / height))) }
    // The curve's output at an input level (linear between its points).
    function outputAt(db) {
        if (curve.length < 2)
            return db
        const f = (Math.max(floorDb, Math.min(0, db)) - floorDb) / -floorDb * (curve.length - 1)
        const i = Math.min(curve.length - 2, Math.floor(f))
        return curve[i] + (curve[i + 1] - curve[i]) * (f - i)
    }
    // The ratio that puts the curve at `outDb` for `inDb`, from the threshold.
    function ratioFor(inDb, outDb) {
        const across = inDb - threshold
        const up = outDb - threshold
        // Past the threshold the other way: as steep as the control allows.
        const steepest = 1000
        if (Math.abs(up) < 1e-3)
            return steepest
        const ratio = across / up
        return ratio > 0 ? ratio : steepest
    }

    onCurveChanged: canvas.requestPaint()
    onLevelChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: view.theme.radiusLg
        color: view.theme.screen
        border.color: view.theme.wellEdge
    }
    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = view.theme.grid
            ctx.lineWidth = view.theme.hairline
            for (let db = -50; db < 0; db += 10) {
                const x = Math.round(view.xOf(db)) + view.pixelCentre
                const y = Math.round(view.yOf(db)) + view.pixelCentre
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke()
            }
            ctx.strokeStyle = view.theme.unity
            ctx.setLineDash([view.theme.unityDash, view.theme.unityDash])
            ctx.beginPath(); ctx.moveTo(0, height); ctx.lineTo(width, 0); ctx.stroke()
            ctx.setLineDash([])
            if (view.curve.length < 2)
                return
            ctx.strokeStyle = view.colour
            ctx.lineWidth = view.theme.transferCurveWidth
            ctx.beginPath()
            for (let i = 0; i < view.curve.length; ++i) {
                const input = view.floorDb + -view.floorDb * i / (view.curve.length - 1)
                const x = view.xOf(input)
                const y = view.yOf(view.curve[i])
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.stroke()
            if (view.pointShown) {
                ctx.fillStyle = view.theme.glacier
                ctx.beginPath()
                ctx.arc(view.xOf(view.level), view.yOf(view.pointOut), view.theme.levelDotRadius, 0, 2 * Math.PI)
                ctx.fill()
            }
        }
    }

    component Handle: Rectangle {
        id: knobHandle
        required property string objectId
        required property real hx
        required property real hy
        property color tone: view.colour
        signal moved(real x, real y)
        signal began()
        signal ended()
        objectName: objectId
        x: hx - width / 2
        y: hy - height / 2
        width: view.theme.handleSize
        height: view.theme.handleSize
        radius: width / 2
        color: handleArea.pressed ? tone : view.theme.screen
        border.color: tone
        border.width: view.theme.strokeBold
        MouseArea {
            id: handleArea
            anchors.fill: parent
            anchors.margins: -view.theme.handleHitSlop
            enabled: view.controller.editable
            onPressed: knobHandle.began()
            onPositionChanged: (mouse) => {
                if (pressed) {
                    const p = mapToItem(view, mouse.x, mouse.y)
                    knobHandle.moved(p.x, p.y)
                }
            }
            onReleased: knobHandle.ended()
            onDoubleClicked: view.panel.resetRatios(view.band)
        }
    }

    function begin(field) {
        panel.takeOver()
        const id = panel.bandId(band, field)
        theme.lastTouched = id
        controller.begin_gesture(id)
    }
    function end(field) {
        controller.end_gesture(panel.bandId(band, field))
    }

    Handle {
        objectId: "strataThresholdHandle"
        hx: view.xOf(view.threshold)
        hy: view.yOf(view.outputAt(view.threshold))
        tone: view.theme.glacier
        onBegan: view.begin("threshold")
        onMoved: (x, y) => view.controller.set_value(view.panel.bandId(view.band, "threshold"), view.dbOfX(x))
        onEnded: view.end("threshold")
    }
    Handle {
        objectId: "strataAboveHandle"
        hx: view.xOf(0)
        hy: view.yOf(view.outputAt(0))
        onBegan: view.begin("above")
        onMoved: (x, y) => view.controller.set_value(view.panel.bandId(view.band, "above"),
                                                     view.ratioFor(0, view.dbOfY(y)))
        onEnded: view.end("above")
    }
    Handle {
        objectId: "strataBelowHandle"
        tone: view.theme.live
        hx: view.xOf(view.floorDb)
        hy: view.yOf(view.outputAt(view.floorDb))
        onBegan: view.begin("below")
        onMoved: (x, y) => view.controller.set_value(view.panel.bandId(view.band, "below"),
                                                     view.ratioFor(view.floorDb, view.dbOfY(y)))
        onEnded: view.end("below")
    }
}
