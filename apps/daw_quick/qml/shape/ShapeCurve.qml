pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The hero display: Shape's response from 20 Hz to 20 kHz and +24 to -24 dB,
// evaluated on the EQ's own biquad cascade (NativeSynthDeviceController::
// shape_response), so it is the curve the EQ runs. Each band that is on
// draws its own curve in its colour under the summed one and carries a
// numbered handle: drag it to move the band's frequency and, for shelves
// and bells, its gain; scroll over it for Q (Shift for fine); click it to
// select the band; double-click it to return its gain to 0 dB.
// Double-click the empty screen to switch on a free band there.
Rectangle {
    id: view
    required property var controller
    required property ShapeTheme theme
    // The band shown in the panel's band strip (from 0).
    property int selected: 0
    // The running EQ's telemetry: spectra before and after it, when playing.
    property var live: ({})
    readonly property var spectrumIn: live.live === true && live.input_spectrum ? live.input_spectrum : []
    readonly property var spectrumOut: live.live === true && live.output_spectrum ? live.output_spectrum : []
    // The spectra's own scale, dBFS from the floor to the top of the plot.
    readonly property real spectrumFloor: -90
    function spectrumY(db) {
        return plotY + plotH * (1 - (Math.max(spectrumFloor, Math.min(0, db)) - spectrumFloor) / -spectrumFloor)
    }
    signal picked(int band)

    readonly property int bandCount: 8
    readonly property int points: 240
    readonly property int revision: controller.revision
    readonly property var response: revision >= 0 ? controller.shape_response(points, -1) : []
    readonly property var bandCurves: {
        const curves = []
        for (let b = 0; b < bandCount; ++b)
            curves.push(revision >= 0 ? controller.shape_response(bandPoints, b) : [])
        return curves
    }
    readonly property int onCount: {
        let n = 0
        for (let b = 0; b < bandCount; ++b)
            n += isOn(b) ? 1 : 0
        return n
    }
    readonly property real topDb: 24
    readonly property real bottomDb: -24
    readonly property real gridStepDb: 6
    // Every second dB line is drawn stronger.
    readonly property real gridStrongDb: 12
    // Strokes land on pixel centres.
    readonly property real pixelCentre: 0.5
    // The detail of each band's own curve (the summed one takes `points`).
    readonly property int bandPoints: 160

    objectName: "shapeCurve"
    radius: theme.viewRadius
    color: theme.screen
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "EQ response, " + onCount + (onCount === 1 ? " band" : " bands") + " on"

    // Plot area inside the screen bezel.
    readonly property real plotX: theme.plotLeft
    readonly property real plotY: theme.plotTop
    readonly property real plotW: width - plotX - theme.plotRight
    readonly property real plotH: height - plotY - theme.plotBottom
    function xOf(hz) { return plotX + controller.fraction_of(hz) * plotW }
    function yOf(db) { return plotY + (topDb - Math.max(bottomDb, Math.min(topDb, db))) / (topDb - bottomDb) * plotH }
    function hzAt(x) { return controller.frequency_at(Math.max(0, Math.min(1, (x - plotX) / plotW))) }
    function dbAt(y) {
        const db = topDb - (y - plotY) / plotH * (topDb - bottomDb)
        return Math.max(bottomDb, Math.min(topDb, db))
    }

    function bandId(band, name) { return "shape." + name + "_" + (band + 1) }
    function isOn(band) { return revision >= 0 && controller.choice(bandId(band, "on")) === 1 }
    function kindOf(band) { return revision >= 0 ? controller.choice(bandId(band, "kind")) : 4 }
    // Shelves and bells have gain; the pass filters have none.
    function hasGain(band) { return kindOf(band) >= 2 }
    // The band a double-click switches on: the first free bell, else the
    // first free band; -1 when all eight are on.
    function freeBand() {
        let first = -1
        for (let b = 0; b < bandCount; ++b) {
            if (isOn(b))
                continue
            if (kindOf(b) === 4)
                return b
            if (first < 0)
                first = b
        }
        return first
    }
    function addBandAt(x, y) {
        const band = freeBand()
        if (band < 0 || !controller.editable)
            return
        const on = bandId(band, "on")
        controller.begin_gesture(on)
        controller.set_choice(on, 1)
        controller.set_value(bandId(band, "freq"), hzAt(x))
        if (hasGain(band))
            controller.set_value(bandId(band, "gain"), dbAt(y))
        controller.end_gesture(on)
        theme.lastTouched = bandId(band, "freq")
        picked(band)
    }

    // A soft glow in the middle of the screen.
    Rectangle {
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        radius: parent.radius
        gradient: Gradient {
            GradientStop { position: 0; color: view.theme.screenGlow }
            GradientStop { position: view.theme.screenGlowEnd; color: view.theme.screen }
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
            const t = view.theme
            const x0 = view.plotX
            const y0 = view.plotY
            const w = view.plotW
            const h = view.plotH
            // Graticule: 1-2-5 lines with the decades stronger; dB every 6
            // with 0 dB brightest.
            ctx.lineWidth = t.hairline
            for (const hz of [30, 40, 50, 60, 70, 80, 90, 200, 300, 400, 500, 600, 700, 800, 900,
                              2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000]) {
                const x = Math.round(view.xOf(hz)) + view.pixelCentre
                ctx.strokeStyle = t.grid
                ctx.beginPath()
                ctx.moveTo(x, y0)
                ctx.lineTo(x, y0 + h)
                ctx.stroke()
            }
            for (const hz of [100, 1000, 10000]) {
                const x = Math.round(view.xOf(hz)) + view.pixelCentre
                ctx.strokeStyle = t.gridStrong
                ctx.beginPath()
                ctx.moveTo(x, y0)
                ctx.lineTo(x, y0 + h)
                ctx.stroke()
            }
            for (let db = view.topDb; db >= view.bottomDb; db -= view.gridStepDb) {
                const y = Math.round(view.yOf(db)) + view.pixelCentre
                ctx.strokeStyle = db === 0 ? t.zero : db % view.gridStrongDb === 0 ? t.gridStrong : t.grid
                ctx.beginPath()
                ctx.moveTo(x0, y)
                ctx.lineTo(x0 + w, y)
                ctx.stroke()
            }
            ctx.save()
            ctx.beginPath()
            ctx.rect(x0, y0, w, h)
            ctx.clip()
            ctx.lineJoin = "round"
            // The live spectra, behind everything: band i sits at the centre
            // of its log band, 20 Hz to 20 kHz.
            const spectrum = (bands, fill) => {
                if (bands.length < 2)
                    return
                const n = bands.length
                ctx.beginPath()
                for (let i = 0; i < n; ++i) {
                    const x = x0 + (i + 0.5) / n * w
                    const y = view.spectrumY(Number(bands[i]))
                    if (i === 0) {
                        if (fill)
                            ctx.moveTo(x0, y0 + h)
                        else
                            ctx.moveTo(x, y)
                    }
                    ctx.lineTo(x, y)
                }
                if (fill) {
                    ctx.lineTo(x0 + w, y0 + h)
                    ctx.closePath()
                    ctx.fillStyle = t.spectrumIn
                    ctx.globalAlpha = t.spectrumInAlpha
                    ctx.fill()
                } else {
                    ctx.strokeStyle = t.spectrumOut
                    ctx.globalAlpha = t.spectrumOutAlpha
                    ctx.lineWidth = t.spectrumOutStroke
                    ctx.stroke()
                }
                ctx.globalAlpha = 1
            }
            spectrum(view.spectrumIn, true)
            spectrum(view.spectrumOut, false)
            const zeroY = view.yOf(0)
            const path = r => {
                ctx.beginPath()
                for (let i = 0; i < r.length; ++i) {
                    const x = x0 + i / (r.length - 1) * w
                    if (i === 0)
                        ctx.moveTo(x, view.yOf(r[i]))
                    else
                        ctx.lineTo(x, view.yOf(r[i]))
                }
            }
            // Each band's own curve, washed down to 0 dB in its colour.
            for (let b = 0; b < view.bandCount; ++b) {
                const r = view.bandCurves[b]
                if (r.length < 2)
                    continue
                const chosen = b === view.selected
                ctx.beginPath()
                ctx.moveTo(x0, zeroY)
                for (let i = 0; i < r.length; ++i)
                    ctx.lineTo(x0 + i / (r.length - 1) * w, view.yOf(r[i]))
                ctx.lineTo(x0 + w, zeroY)
                ctx.closePath()
                ctx.fillStyle = t.bands[b]
                ctx.globalAlpha = chosen ? t.bandFillChosenAlpha : t.bandFillAlpha
                ctx.fill()
                path(r)
                ctx.strokeStyle = t.bands[b]
                ctx.globalAlpha = chosen ? t.bandLineChosenAlpha : t.bandLineAlpha
                ctx.lineWidth = chosen ? t.bandLineChosenStroke : t.hairline
                ctx.stroke()
                ctx.globalAlpha = 1
            }
            // The summed curve, glowing.
            const sum = view.response
            if (sum.length > 1) {
                const a = t.accent
                path(sum)
                ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, t.curveGlowWideAlpha)
                ctx.lineWidth = t.curveGlowWideStroke
                ctx.stroke()
                ctx.strokeStyle = Qt.rgba(a.r, a.g, a.b, t.curveGlowTightAlpha)
                ctx.lineWidth = t.curveGlowTightStroke
                ctx.stroke()
                ctx.strokeStyle = t.curve
                ctx.lineWidth = t.curveStroke
                ctx.stroke()
            }
            ctx.restore()
        }
    }

    // Frequency and level scales.
    Repeater {
        model: [{ hz: 20, text: "20" }, { hz: 50, text: "50" }, { hz: 100, text: "100" },
                { hz: 200, text: "200" }, { hz: 500, text: "500" }, { hz: 1000, text: "1k" },
                { hz: 2000, text: "2k" }, { hz: 5000, text: "5k" }, { hz: 10000, text: "10k" },
                { hz: 20000, text: "20k" }]
        delegate: Text {
            required property var modelData
            x: Math.min(view.xOf(modelData.hz) - implicitWidth / 2, view.plotX + view.plotW - implicitWidth)
            y: view.plotY + view.plotH + view.theme.scaleLabelGap
            text: modelData.text
            color: modelData.hz === 100 || modelData.hz === 1000 || modelData.hz === 10000
                ? view.theme.inkDim : view.theme.inkFaint
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeCaption
        }
    }
    Repeater {
        model: [24, 12, 6, 0, -6, -12, -24]
        delegate: Text {
            required property var modelData
            x: view.plotX - implicitWidth - view.theme.spaceSm
            y: view.yOf(modelData) - implicitHeight / 2
            text: Format.signed_number(modelData, 0)
            color: modelData === 0 ? view.theme.accent : view.theme.inkFaint
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeCaption
        }
    }
    Text {
        objectName: "shapeCurveCaption"
        x: view.theme.captionX
        y: view.theme.captionY
        text: view.onCount === 0 ? "FLAT · EVERY BAND OFF · BIT-EXACT PASS-THROUGH"
            : view.onCount + (view.onCount === 1 ? " BAND ON" : " BANDS ON") + " · 48 kHz BIQUADS"
        color: view.onCount === 0 ? view.theme.inkDim : view.theme.accent
        font.pixelSize: view.theme.typeCaption
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.hintRight
        y: view.theme.captionY
        text: "DRAG A HANDLE · SCROLL FOR Q · DOUBLE-CLICK TO ADD A BAND"
        color: view.theme.inkFaint
        font.pixelSize: view.theme.typeMicro
        font.bold: true
        font.letterSpacing: view.theme.capsTightSpacing
    }

    // Double-click the empty screen to switch on a free band there.
    MouseArea {
        objectName: "shapeCurveArea"
        x: view.plotX
        y: view.plotY
        width: view.plotW
        height: view.plotH
        enabled: view.controller.editable
        onDoubleClicked: mouse => view.addBandAt(mouse.x + x, mouse.y + y)
    }

    // One handle per band that is on.
    Repeater {
        model: view.bandCount
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property string freqId: view.bandId(index, "freq")
            readonly property string gainId: view.bandId(index, "gain")
            readonly property string qId: view.bandId(index, "q")
            readonly property bool gainful: view.hasGain(index)
            readonly property real freq: view.revision >= 0 ? view.controller.value(freqId) : 1000
            readonly property real gain: view.revision >= 0 && gainful ? view.controller.value(gainId) : 0
            readonly property real hx: view.xOf(freq)
            readonly property real hy: view.yOf(gain)
            readonly property color tone: view.theme.bands[index]
            readonly property bool chosen: index === view.selected
            property real grabX: 0
            property real grabY: 0
            objectName: "shapeHandle:" + (index + 1)
            visible: view.isOn(index)
            x: hx - width / 2
            y: hy - height / 2
            width: chosen ? view.theme.handleChosenSize : view.theme.handleSize
            height: width
            radius: width / 2
            color: grab.pressed ? tone : Qt.rgba(tone.r, tone.g, tone.b,
                chosen ? view.theme.handleChosenAlpha : view.theme.handleAlpha)
            border.color: chosen ? view.theme.glacier : tone
            border.width: chosen ? view.theme.strokeBold : view.theme.handleStroke
            Accessible.role: Accessible.Slider
            Accessible.name: "Band " + (index + 1)
            Accessible.description: view.revision < 0 ? "" : view.controller.display(freqId)
                + (gainful ? ", " + view.controller.display(gainId) : "")
            Text {
                anchors.centerIn: parent
                text: handle.index + 1
                color: grab.pressed ? view.theme.screen : view.theme.ink
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeCaption
                font.bold: true
            }
            MouseArea {
                id: grab
                anchors.fill: parent
                anchors.margins: -view.theme.handleGrabOutset
                enabled: view.controller.editable
                hoverEnabled: true
                preventStealing: true
                cursorShape: handle.gainful ? Qt.SizeAllCursor : Qt.SizeHorCursor
                onPressed: mouse => {
                    const p = mapToItem(view, mouse.x, mouse.y)
                    handle.grabX = p.x - handle.hx
                    handle.grabY = p.y - handle.hy
                    view.picked(handle.index)
                    view.theme.lastTouched = handle.freqId
                    view.controller.begin_gesture(handle.freqId)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(view, mouse.x, mouse.y)
                    view.controller.set_value(handle.freqId, view.hzAt(p.x - handle.grabX))
                    if (handle.gainful)
                        view.controller.set_value(handle.gainId, view.dbAt(p.y - handle.grabY))
                }
                onReleased: view.controller.end_gesture(handle.freqId)
                onCanceled: view.controller.cancel_gesture(handle.freqId)
                onDoubleClicked: {
                    if (handle.gainful) {
                        view.controller.reset_to_default(handle.gainId)
                        view.theme.lastTouched = handle.gainId
                    }
                }
                onWheel: wheel => {
                    if (wheel.angleDelta.y !== 0) {
                        view.picked(handle.index)
                        view.controller.step(handle.qId, wheel.angleDelta.y > 0 ? 1 : -1,
                            (wheel.modifiers & Qt.ShiftModifier) !== 0)
                        view.theme.lastTouched = handle.qId
                    }
                    wheel.accepted = true
                }
            }
        }
    }

    onResponseChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onBandCurvesChanged: canvas.requestPaint()
    onSelectedChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
