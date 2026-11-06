pragma ComponentBehavior: Bound
import QtQuick
import "TemperCurves.js" as Curves

// The hero display, 20 Hz to 20 kHz. Behind: the live spectra (in as a
// fill, out as a line, the sidechain dashed while Unmask hears it) on their
// own dBFS scale. Over them, on a +-18 dB scale: the cut Temper applies now,
// hanging copper from 0 dB; each dynamic band's full range (dashed) and its
// gain now (filled), with a numbered node; and the ice-blue sensitivity
// curve with a diamond per node. Drag a diamond to move where Temper
// listens; drag a band's node for its frequency and range, scroll over it
// for Q, double-click it to switch it on or off. Double-click the empty
// screen to switch on the next free band there. Under the plot, the
// reduction history.
Rectangle {
    id: view
    required property var controller
    required property TemperTheme theme
    required property int bandCount
    required property int nodeCount
    // The band in the inspector (from 0).
    property int selected: 0
    // The running effect's telemetry, when playing.
    property var live: ({})
    signal picked(int band)

    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true
    readonly property var spectrumIn: playing && live.input_spectrum ? live.input_spectrum : []
    readonly property var spectrumOut: playing && live.output_spectrum ? live.output_spectrum : []
    readonly property var spectrumSide: playing && live.keyed === true && unmaskOn
        && live.sidechain_spectrum ? live.sidechain_spectrum : []
    readonly property var gain: playing && live.gain ? live.gain : []
    readonly property var bandGain: playing && live.band_gain ? live.band_gain : []
    readonly property bool unmaskOn: revision >= 0 && controller.choice("temper.unmask") === 1
    readonly property bool deltaOn: revision >= 0 && controller.choice("temper.delta") === 1

    readonly property real topDb: 18
    readonly property real bottomDb: -18
    readonly property real spectrumFloor: -90
    readonly property int points: 200
    // The dB grid's step on the curve scale.
    readonly property real gridStepDb: 6
    // Offsets that centre a 1 px line on a pixel and overlap filled cells.
    readonly property real pixelCentre: 0.5
    // A square turned to a diamond for the sensitivity nodes.
    readonly property real diamondRotation: 45
    readonly property real heatHeight: theme.heatHeight

    // Plot area inside the bezel; the heatmap sits under the scale.
    readonly property real plotX: theme.plotLeft
    readonly property real plotY: theme.plotTop
    readonly property real plotW: width - plotX - theme.plotRight
    readonly property real plotH: height - plotY - heatHeight - theme.plotBottom

    objectName: "temperDisplay"
    radius: theme.viewRadius
    color: theme.screen
    border.color: theme.wellEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Spectrum, sensitivity curve, " + onCount
        + (onCount === 1 ? " dynamic band" : " dynamic bands") + " and the cut"

    function xOf(hz) { return plotX + controller.fraction_of(hz) * plotW }
    function yOf(db) { return plotY + (topDb - Math.max(bottomDb, Math.min(topDb, db))) / (topDb - bottomDb) * plotH }
    function hzAt(x) { return controller.frequency_at(Math.max(0, Math.min(1, (x - plotX) / plotW))) }
    function dbAt(y) {
        const db = topDb - (y - plotY) / plotH * (topDb - bottomDb)
        return Math.max(bottomDb, Math.min(topDb, db))
    }
    function spectrumY(db) {
        return plotY + plotH * (1 - (Math.max(spectrumFloor, Math.min(0, db)) - spectrumFloor) / -spectrumFloor)
    }
    function hzOfPoint(i) { return controller.frequency_at(i / (points - 1)) }

    function bandId(band, name) { return "temper.band_" + name + "_" + (band + 1) }
    function nodeId(node, name) { return "temper.sens_" + name + "_" + (node + 1) }
    function isOn(band) { return revision >= 0 && controller.choice(bandId(band, "on")) === 1 }
    readonly property int onCount: {
        let n = 0
        for (let b = 0; b < bandCount; ++b)
            n += isOn(b) ? 1 : 0
        return n
    }
    // The nodes as the sensitivity curve reads them.
    readonly property var nodes: {
        const out = []
        for (let n = 0; n < nodeCount; ++n) {
            out.push({ hz: revision >= 0 ? controller.value(nodeId(n, "freq")) : 1000,
                       db: revision >= 0 ? controller.value(nodeId(n, "gain")) : 0 })
        }
        return out
    }
    // Each band's response at full range on the display's points ([] when off).
    readonly property var bandCurves: {
        const curves = []
        for (let b = 0; b < bandCount; ++b) {
            const curve = []
            if (isOn(b)) {
                const kind = controller.choice(bandId(b, "kind"))
                const freq = controller.value(bandId(b, "freq"))
                const q = controller.value(bandId(b, "q"))
                const range = controller.value(bandId(b, "range"))
                for (let i = 0; i < points; ++i)
                    curve.push(Curves.bandDb(kind, freq, q, range, hzOfPoint(i)))
            }
            curves.push(curve)
        }
        return curves
    }

    // The next band a double-click switches on; -1 when all are on.
    function freeBand() {
        for (let b = 0; b < bandCount; ++b) {
            if (!isOn(b))
                return b
        }
        return -1
    }
    function addBandAt(x, y) {
        const band = freeBand()
        if (band < 0 || !controller.editable)
            return
        const on = bandId(band, "on")
        controller.begin_gesture(on)
        controller.set_choice(on, 1)
        controller.set_value(bandId(band, "freq"), hzAt(x))
        controller.set_value(bandId(band, "range"), dbAt(y))
        controller.end_gesture(on)
        theme.lastTouched = bandId(band, "freq")
        picked(band)
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: view.theme.hairline
        radius: parent.radius
        gradient: Gradient {
            GradientStop { position: 0; color: view.theme.screenGlow }
            GradientStop { position: view.theme.screenGlowEnd; color: view.theme.screen }
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
            ctx.lineWidth = t.hairline
            for (const hz of [30, 40, 50, 60, 70, 80, 90, 200, 300, 400, 500, 600, 700, 800, 900,
                              2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000]) {
                const x = Math.round(view.xOf(hz)) + view.pixelCentre
                ctx.strokeStyle = t.grid
                ctx.beginPath(); ctx.moveTo(x, y0); ctx.lineTo(x, y0 + h); ctx.stroke()
            }
            for (const hz of [100, 1000, 10000]) {
                const x = Math.round(view.xOf(hz)) + view.pixelCentre
                ctx.strokeStyle = t.gridStrong
                ctx.beginPath(); ctx.moveTo(x, y0); ctx.lineTo(x, y0 + h); ctx.stroke()
            }
            for (let db = view.topDb; db >= view.bottomDb; db -= view.gridStepDb) {
                const y = Math.round(view.yOf(db)) + view.pixelCentre
                ctx.strokeStyle = db === 0 ? t.zero : t.grid
                ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x0 + w, y); ctx.stroke()
            }
            ctx.save()
            ctx.beginPath()
            ctx.rect(x0, y0, w, h)
            ctx.clip()
            ctx.lineJoin = "round"
            // Display band i sits at the centre of its log band.
            const bandX = (i, n) => x0 + (i + 0.5) / n * w
            const spectrum = (bands, style) => {
                const n = bands.length
                if (n < 2)
                    return
                ctx.beginPath()
                if (style === "fill")
                    ctx.moveTo(x0, y0 + h)
                for (let i = 0; i < n; ++i) {
                    const x = bandX(i, n)
                    const y = view.spectrumY(Number(bands[i]))
                    if (i === 0 && style !== "fill")
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                if (style === "fill") {
                    ctx.lineTo(x0 + w, y0 + h)
                    ctx.closePath()
                    ctx.fillStyle = t.spectrumIn
                    ctx.globalAlpha = t.spectrumInAlpha
                    ctx.fill()
                } else if (style === "side") {
                    ctx.setLineDash(t.sideDash)
                    ctx.strokeStyle = t.side
                    ctx.lineWidth = t.sideStroke
                    ctx.stroke()
                    ctx.setLineDash([])
                } else {
                    ctx.strokeStyle = t.spectrumOut
                    ctx.globalAlpha = t.spectrumOutAlpha
                    ctx.lineWidth = t.spectrumOutStroke
                    ctx.stroke()
                }
                ctx.globalAlpha = 1
            }
            spectrum(view.spectrumIn, "fill")
            spectrum(view.spectrumSide, "side")
            spectrum(view.spectrumOut, "line")

            // The cut now, hanging from 0 dB.
            const zeroY = view.yOf(0)
            const g = view.gain
            if (g.length > 1) {
                const n = g.length
                ctx.beginPath()
                ctx.moveTo(x0, zeroY)
                for (let i = 0; i < n; ++i)
                    ctx.lineTo(bandX(i, n), view.yOf(Math.min(0, Number(g[i]))))
                ctx.lineTo(x0 + w, zeroY)
                ctx.closePath()
                const fill = ctx.createLinearGradient(0, zeroY, 0, view.yOf(view.bottomDb))
                fill.addColorStop(0, Qt.rgba(t.cut.r, t.cut.g, t.cut.b, t.cutFillTopAlpha))
                fill.addColorStop(1, Qt.rgba(t.cutHot.r, t.cutHot.g, t.cutHot.b, t.cutFillDeepAlpha))
                ctx.fillStyle = fill
                ctx.fill()
                ctx.beginPath()
                for (let i = 0; i < n; ++i) {
                    const y = view.yOf(Math.min(0, Number(g[i])))
                    if (i === 0) ctx.moveTo(bandX(i, n), y); else ctx.lineTo(bandX(i, n), y)
                }
                ctx.strokeStyle = t.cut
                ctx.lineWidth = t.cutStroke
                ctx.stroke()
            }

            // Each band: its full range dashed, its gain now filled.
            const pointX = i => x0 + i / (view.points - 1) * w
            for (let b = 0; b < view.bandCount; ++b) {
                const r = view.bandCurves[b]
                if (r.length < 2)
                    continue
                const tone = t.bands[b]
                const chosen = b === view.selected
                ctx.beginPath()
                for (let i = 0; i < r.length; ++i) {
                    if (i === 0) ctx.moveTo(pointX(i), view.yOf(r[i])); else ctx.lineTo(pointX(i), view.yOf(r[i]))
                }
                ctx.setLineDash(t.bandRangeDash)
                ctx.strokeStyle = tone
                ctx.globalAlpha = chosen ? t.bandRangeChosenAlpha : t.bandRangeAlpha
                ctx.lineWidth = t.hairline
                ctx.stroke()
                ctx.setLineDash([])
                const range = view.controller.value(view.bandId(b, "range"))
                const now = b < view.bandGain.length ? Number(view.bandGain[b]) : 0
                if (range !== 0 && now !== 0) {
                    const scale = now / range
                    ctx.beginPath()
                    ctx.moveTo(x0, zeroY)
                    for (let i = 0; i < r.length; ++i)
                        ctx.lineTo(pointX(i), view.yOf(r[i] * scale))
                    ctx.lineTo(x0 + w, zeroY)
                    ctx.closePath()
                    ctx.fillStyle = tone
                    ctx.globalAlpha = chosen ? t.bandGainChosenAlpha : t.bandGainAlpha
                    ctx.fill()
                }
                ctx.globalAlpha = 1
            }

            // Where Temper listens, glowing.
            const l = t.listen
            ctx.beginPath()
            for (let i = 0; i < view.points; ++i) {
                const y = view.yOf(Curves.sensitivityDb(view.nodes, view.hzOfPoint(i)))
                if (i === 0) ctx.moveTo(pointX(i), y); else ctx.lineTo(pointX(i), y)
            }
            ctx.strokeStyle = Qt.rgba(l.r, l.g, l.b, t.listenGlowAlpha)
            ctx.lineWidth = t.listenGlowStroke
            ctx.stroke()
            ctx.strokeStyle = l
            ctx.lineWidth = t.strokeBold
            ctx.stroke()
            ctx.restore()
        }
    }

    // Frequency and gain scales.
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
            color: view.theme.inkFaint
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeCaption
        }
    }
    Repeater {
        model: [18, 12, 6, 0, -6, -12, -18]
        delegate: Text {
            required property var modelData
            x: view.plotX - implicitWidth - view.theme.spaceSm
            y: view.yOf(modelData) - implicitHeight / 2
            text: modelData > 0 ? "+" + modelData : modelData < 0 ? "−" + (-modelData) : "0"
            color: modelData === 0 ? view.theme.inkDim : view.theme.inkFaint
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeCaption
        }
    }
    Text {
        objectName: "temperDisplayCaption"
        x: view.theme.spaceLg
        y: view.theme.spaceMd
        text: view.deltaOn ? "DELTA · ONLY WHAT TEMPER REMOVES"
            : (view.playing ? "CUT NOW " + (view.live.max_cut > 0.05 ? "−" + Number(view.live.max_cut).toFixed(1) + " DB" : "0.0 DB")
                            : "NO SIGNAL")
            + " · " + view.onCount + (view.onCount === 1 ? " BAND ON" : " BANDS ON")
        color: view.deltaOn ? view.theme.cutHot : view.playing ? view.theme.cut : view.theme.inkDim
        font.pixelSize: view.theme.typeCaption
        font.bold: true
        font.letterSpacing: view.theme.displayCaptionSpacing
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.spaceXl
        y: view.theme.spaceMd
        text: "DRAG ◆ TO LISTEN · DRAG A BAND · SCROLL FOR Q · DOUBLE-CLICK TO ADD"
        color: view.theme.inkFaint
        font.pixelSize: view.theme.typeMicro
        font.bold: true
        font.letterSpacing: view.theme.capsTightSpacing
    }

    MouseArea {
        objectName: "temperDisplayArea"
        x: view.plotX
        y: view.plotY
        width: view.plotW
        height: view.plotH
        enabled: view.controller.editable
        onDoubleClicked: mouse => view.addBandAt(mouse.x + x, mouse.y + y)
    }

    // One diamond per sensitivity node.
    Repeater {
        model: view.nodeCount
        delegate: Item {
            id: node
            required property int index
            readonly property string freqId: view.nodeId(index, "freq")
            readonly property string gainId: view.nodeId(index, "gain")
            readonly property real hx: view.xOf(view.nodes[index].hz)
            readonly property real hy: view.yOf(view.nodes[index].db)
            property real grabX: 0
            property real grabY: 0
            objectName: "temperNode:" + (index + 1)
            x: hx - width / 2
            y: hy - height / 2
            width: view.theme.nodeHitSize
            height: view.theme.nodeHitSize
            Accessible.role: Accessible.Slider
            Accessible.name: "Listen " + (index + 1)
            Accessible.description: view.revision < 0 ? ""
                : view.controller.display(freqId) + ", " + view.controller.display(gainId)
            Rectangle {
                anchors.centerIn: parent
                width: nodeGrab.pressed ? view.theme.nodeDiamondPressed : view.theme.nodeDiamond
                height: width
                rotation: view.diamondRotation
                color: nodeGrab.pressed ? view.theme.listen : view.theme.screen
                border.color: view.theme.listen
                border.width: view.theme.nodeStroke
            }
            MouseArea {
                id: nodeGrab
                anchors.fill: parent
                enabled: view.controller.editable
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeAllCursor
                onPressed: mouse => {
                    const p = mapToItem(view, mouse.x, mouse.y)
                    node.grabX = p.x - node.hx
                    node.grabY = p.y - node.hy
                    view.theme.lastTouched = node.gainId
                    view.controller.begin_gesture(node.gainId)
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(view, mouse.x, mouse.y)
                    view.controller.set_value(node.freqId, view.hzAt(p.x - node.grabX))
                    view.controller.set_value(node.gainId, view.dbAt(p.y - node.grabY))
                }
                onReleased: view.controller.end_gesture(node.gainId)
                onCanceled: view.controller.cancel_gesture(node.gainId)
                onDoubleClicked: {
                    view.controller.reset_to_default(node.gainId)
                    view.theme.lastTouched = node.gainId
                }
            }
        }
    }

    // One node per band that is on, and the selected band's even when off.
    Repeater {
        model: view.bandCount
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property string onId: view.bandId(index, "on")
            readonly property string freqId: view.bandId(index, "freq")
            readonly property string rangeId: view.bandId(index, "range")
            readonly property string qId: view.bandId(index, "q")
            readonly property bool on: view.isOn(index)
            readonly property real hx: view.xOf(view.revision >= 0 ? view.controller.value(freqId) : 1000)
            readonly property real hy: view.yOf(view.revision >= 0 ? view.controller.value(rangeId) : 0)
            readonly property color tone: view.theme.bands[index]
            readonly property bool chosen: index === view.selected
            property real grabX: 0
            property real grabY: 0
            objectName: "temperHandle:" + (index + 1)
            visible: on || chosen
            opacity: on ? 1 : view.theme.offBandOpacity
            x: hx - width / 2
            y: hy - height / 2
            width: chosen ? view.theme.handleChosenSize : view.theme.handleSize
            height: width
            radius: width / 2
            color: on ? tone : view.theme.ledOff
            border.color: chosen ? view.theme.glacier : tone
            border.width: chosen ? view.theme.strokeBold : view.theme.handleStroke
            Accessible.role: Accessible.Slider
            Accessible.name: "Band " + (index + 1) + (on ? "" : ", off")
            Accessible.description: view.revision < 0 ? ""
                : view.controller.display(freqId) + ", " + view.controller.display(rangeId)
            Text {
                anchors.centerIn: parent
                text: handle.index + 1
                color: view.theme.screen
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeCaption
                font.bold: true
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -view.theme.spaceXs
                enabled: view.controller.editable
                hoverEnabled: true
                preventStealing: true
                cursorShape: Qt.SizeAllCursor
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
                    view.controller.set_value(handle.rangeId, view.dbAt(p.y - handle.grabY))
                }
                onReleased: view.controller.end_gesture(handle.freqId)
                onCanceled: view.controller.cancel_gesture(handle.freqId)
                onDoubleClicked: {
                    view.controller.set_choice(handle.onId, handle.on ? 0 : 1)
                    view.theme.lastTouched = handle.onId
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

    TemperHeatmap {
        id: heatmap
        x: view.plotX
        y: view.height - view.heatHeight - view.theme.spaceMd
        width: view.plotW
        height: view.heatHeight
        theme: view.theme
        gain: view.gain
        maxCutDb: view.revision >= 0 ? view.controller.value("temper.max_cut") : 12
    }

    onRevisionChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onSelectedChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
}
