pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// An oscillator's display, by type (NOVA-UI-03/04). Wavetable: a 3D stack of
// frames with the frame at the *modulated* position lit, following
// telemetry. Classic: the band-limited shape. Granular: the grains in
// flight. Sample, Multisample, Spectral: the source and its scan. While a
// synth sounds, the front trace is the engine's own cycle (contract §6).
// Every type but Classic scans its position: drag across the display (one
// undo step, Shift fine), double-click to reset it.
Rectangle {
    id: view
    required property var panel
    required property string osc
    required property int type
    required property string table
    readonly property int index: Contract.oscKeys.indexOf(osc)
    readonly property string base: "nova.osc." + osc
    readonly property var controller: panel.controller
    readonly property real position: panel.live ? panel.at(Contract.T.position + index)
        : (controller.revision >= 0 ? controller.value(base + ".position") : 0)
    readonly property int shape: controller.revision >= 0 ? controller.choice(base + ".shape") : 2
    readonly property real pw: controller.revision >= 0 ? controller.value(base + ".pw") : 0.5
    readonly property var typeNames: ["CLASSIC · BAND-LIMITED", "WAVETABLE", "SAMPLE", "MULTISAMPLE", "GRANULAR", "SPECTRAL"]
    readonly property bool scans: type !== 0
    // Wavetable: the frames stacked, each stepped right and up from the one
    // before; the stack's left inset, the room it leaves on the right, its
    // baseline and amplitude as shares of the height; samples per frame.
    readonly property int stackFrames: 18
    readonly property real stackStepX: 3.4
    readonly property real stackStepY: 2.2
    readonly property real stackInset: 12
    readonly property real stackRoom: 92
    readonly property real stackBaseline: 0.74
    readonly property real stackAmplitude: 0.16
    readonly property int frameSamples: 100
    readonly property int liveSamples: 160
    // Classic: two cycles across the width inside this inset, at this amplitude.
    readonly property int classicSamples: 400
    readonly property real classicInset: 10
    readonly property real classicAmplitude: 0.32
    // Granular: grains scattered by these pseudo-random strides, drifting
    // with time, in a band of the height; each a bar of a random width.
    readonly property int idleGrains: 24
    readonly property real grainSeed: 0.137
    readonly property real grainDrift: 0.18
    readonly property real grainScatter: 0.61
    readonly property real grainSpread: 0.3
    readonly property real grainTravel: 0.2
    readonly property real grainBandTop: 0.2
    readonly property real grainBand: 0.6
    readonly property real grainWidthSeed: 0.3
    readonly property real grainMinWidth: 10
    readonly property real grainWidthRange: 20
    readonly property real grainHeight: 3
    // Sample: a sketched waveform (a floor plus two beating partials, decaying
    // to the right) with the engine's cycle over it.
    readonly property int sketchStep: 2
    readonly property real sketchFloor: 0.3
    readonly property real sketchRateA: 0.05
    readonly property real sketchRateB: 0.013
    readonly property real sketchDecay: 1.2
    readonly property real sketchAmplitude: 0.36
    readonly property real cycleAmplitude: 0.3
    // The scan position's line: its top and the room it leaves below.
    readonly property real scanTop: 16
    readonly property real scanRoom: 22
    // Shift drags the position this much finer.
    readonly property real fineScale: 0.25

    objectName: "novaOscDisplay:" + osc
    radius: panel.theme.displayRadius
    color: panel.theme.wellFill
    border.color: panel.theme.line
    clip: true

    function liveCycle() {
        if (!panel.live)
            return null
        const from = Contract.T.frames + index * Contract.T.framePoints
        let energy = 0
        const points = []
        for (let i = 0; i < Contract.T.framePoints; ++i) {
            const v = Number(panel.frame[from + i])
            energy += Math.abs(v)
            points.push(v)
        }
        return energy > 1e-4 ? points : null
    }
    function classicSample(x) {
        switch (shape) {
        case 0: return Math.sin(x * Math.PI * 2)
        case 1: return 1 - 4 * Math.abs(x - 0.5)
        case 2: return 1 - 2 * x
        case 3: return x < 0.5 ? 1 : -1
        default: return x < pw ? 1 : -1
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.panel.theme
            const W = width, H = height
            const cycle = view.liveCycle()
            if (view.type === 1) {
                // Frames back to front, the current one in cyan.
                const N = view.stackFrames
                const spanX = W - view.stackRoom
                for (let f = N - 1; f >= 0; --f) {
                    const at = f / (N - 1)
                    const dx = f * view.stackStepX, dy = -f * view.stackStepY
                    const current = Math.abs(at - view.position) < 0.5 / N
                    ctx.beginPath()
                    for (let i = 0; i <= view.frameSamples; ++i) {
                        const xx = i / view.frameSamples
                        const y = Contract.tableSample(view.table, at, xx)
                        const px = view.stackInset + dx + xx * spanX
                        const py = H * view.stackBaseline + dy - y * H * view.stackAmplitude
                        if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                    }
                    ctx.strokeStyle = current ? t.cy : t.soft(t.dim, t.frameBaseAlpha + t.frameDepthAlpha * (1 - f / N))
                    ctx.lineWidth = current ? t.strokeBold : t.hairline
                    ctx.stroke()
                }
                // The modulated frame, lifted out of the stack with a glow.
                const lift = view.position * (N - 1)
                ctx.beginPath()
                const n = cycle ? cycle.length : view.liveSamples
                for (let i = 0; i <= n; ++i) {
                    const xx = i / n
                    const y = cycle ? cycle[i % cycle.length] : Contract.tableSample(view.table, view.position, xx)
                    const px = view.stackInset + lift * view.stackStepX + xx * spanX
                    const py = H * view.stackBaseline - lift * view.stackStepY - y * H * view.stackAmplitude
                    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                }
                // The glow is a wide faint stroke under the line: a software shadow
                // blur costs a hundred times more to paint.
                ctx.strokeStyle = t.soft(t.cy, t.glowAlpha)
                ctx.lineWidth = t.frameGlowWidth
                ctx.stroke()
                ctx.strokeStyle = t.cy
                ctx.lineWidth = t.frameLineWidth
                ctx.stroke()
            } else if (view.type === 0) {
                ctx.strokeStyle = t.line
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                ctx.moveTo(0, H / 2)
                ctx.lineTo(W, H / 2)
                ctx.stroke()
                ctx.beginPath()
                for (let i = 0; i <= view.classicSamples; ++i) {
                    const xx = (i / view.classicSamples * 2) % 1
                    const y = cycle ? cycle[Math.floor(xx * cycle.length) % cycle.length] : view.classicSample(xx)
                    const px = view.classicInset + i / view.classicSamples * (W - 2 * view.classicInset)
                    const py = H / 2 - y * H * view.classicAmplitude
                    if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                }
                // The glow is a wide faint stroke under the line: a software shadow
                // blur costs a hundred times more to paint.
                ctx.strokeStyle = t.soft(t.cy, t.glowAlpha)
                ctx.lineWidth = t.traceGlowWidth
                ctx.stroke()
                ctx.strokeStyle = t.cy
                ctx.lineWidth = t.strokeBold
                ctx.stroke()
            } else if (view.type === 4) {
                // Grains: bars drifting through the source, brighter when young.
                const count = view.panel.live ? Math.max(1, Math.round(view.panel.at(Contract.T.unison + view.index))) : view.idleGrains
                const clock = view.panel.live ? Date.now() / 1000 : 0
                for (let g = 0; g < count; ++g) {
                    const age = (g * view.grainSeed + clock * view.grainDrift) % 1
                    const x = (view.position + (g * view.grainScatter % 1 - 0.5) * view.grainSpread + age * view.grainTravel) % 1
                    const y = H * (view.grainBandTop + view.grainBand * ((g * view.grainScatter) % 1))
                    const w = view.grainMinWidth + view.grainWidthRange * ((g * view.grainWidthSeed) % 1)
                    ctx.fillStyle = t.soft(t.cy, t.grainMinAlpha + t.grainAgeAlpha * (1 - age))
                    ctx.fillRect((x < 0 ? x + 1 : x) * W, y, w, view.grainHeight)
                }
                ctx.fillStyle = t.am
                ctx.fillRect(view.position * W - t.strokeBold / 2, view.scanTop, t.strokeBold, H - view.scanRoom)
            } else {
                // A sample sketch with the scan position in amber.
                ctx.fillStyle = t.soft(t.cy, t.sketchAlpha)
                for (let i = 0; i < W; i += view.sketchStep) {
                    const a = (view.sketchFloor + (1 - view.sketchFloor)
                               * Math.abs(Math.sin(i * view.sketchRateA) * Math.sin(i * view.sketchRateB)))
                        * Math.exp(-i / W * view.sketchDecay) * H * view.sketchAmplitude
                    ctx.fillRect(i, H / 2 - a, t.sketchBarWidth, a * 2)
                }
                if (cycle) {
                    ctx.beginPath()
                    for (let i = 0; i < cycle.length; ++i) {
                        const px = i / (cycle.length - 1) * W, py = H / 2 - cycle[i] * H * view.cycleAmplitude
                        if (i === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py)
                    }
                    ctx.strokeStyle = t.cy
                    ctx.lineWidth = t.sketchCycleWidth
                    ctx.stroke()
                }
                ctx.fillStyle = t.am
                ctx.fillRect(view.position * W - t.strokeBold / 2, view.scanTop, t.strokeBold, H - view.scanRoom)
            }
        }
    }
    Connections {
        target: view.panel
        enabled: view.visible && view.panel.live
        function onFrameChanged() { canvas.requestPaint() }
    }
    onPositionChanged: canvas.requestPaint()
    onTypeChanged: canvas.requestPaint()
    onTableChanged: canvas.requestPaint()
    onShapeChanged: canvas.requestPaint()
    onPwChanged: canvas.requestPaint()

    Text {
        x: view.panel.theme.displayCaptionX
        y: view.panel.theme.displayCaptionY
        text: view.type === 1 ? "FRAME " + (1 + Math.round(view.position * 255)) + " / 256" : view.typeNames[view.type]
        color: view.panel.theme.faint
        font.family: view.panel.theme.sans
        font.pixelSize: view.panel.theme.typeSmall
        font.weight: Font.Bold
        font.letterSpacing: view.panel.theme.displayCaptionSpacing
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.margins: -view.panel.theme.displayCaptionBackdrop
            color: view.panel.theme.wellFill
            visible: view.type === 1
        }
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.panel.theme.displayCaptionX
        y: view.panel.theme.displayReadoutY
        text: view.type === 1 || view.type === 4 ? Contract.mediaName(Contract.tables, view.table, "")
            : view.type === 0 ? ["Sine", "Triangle", "Saw", "Square", "Pulse"][view.shape]
            : view.type === 5 ? "FFT " + (view.controller.revision >= 0 ? view.controller.display(view.base + ".spectral.fft") : "")
            : Contract.noteName(view.controller.revision >= 0 ? Math.round(view.controller.value(view.base + ".sample.root")) : 60) + " root"
        color: view.panel.theme.cy
        font.family: view.panel.theme.mono
        font.pixelSize: view.panel.theme.typeSmall
    }
    Text {
        visible: view.type === 2 || view.type === 3 || view.type === 5
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height - view.panel.theme.displayFootLift
        text: "No sample loaded"
        color: view.panel.theme.faint
        font.family: view.panel.theme.sans
        font.pixelSize: view.panel.theme.typeSmall
    }

    // Drag across the display to scan the position: its full width is the
    // whole range.
    MouseArea {
        id: scan
        objectName: "novaOscScan:" + view.osc
        property real startX: 0
        property real startValue: 0
        property bool active: false
        anchors.fill: parent
        enabled: view.scans && view.controller.editable
        preventStealing: true
        cursorShape: Qt.SizeHorCursor
        onPressed: mouse => {
            const id = view.base + ".position"
            startX = mouse.x
            startValue = view.controller.normalised(id)
            active = true
            view.controller.begin_gesture(id)
            view.panel.touchId(id, "POS")
        }
        onPositionChanged: mouse => {
            if (!pressed || !active)
                return
            const travel = (mouse.x - startX) / width * ((mouse.modifiers & Qt.ShiftModifier) ? view.fineScale : 1)
            view.controller.set_normalised(view.base + ".position", Math.max(0, Math.min(1, startValue + travel)))
        }
        onReleased: {
            if (active)
                view.controller.end_gesture(view.base + ".position")
            active = false
        }
        onCanceled: {
            if (active)
                view.controller.cancel_gesture(view.base + ".position")
            active = false
        }
        onDoubleClicked: {
            view.controller.reset_to_default(view.base + ".position")
            view.panel.touchId(view.base + ".position", "POS")
        }
    }
}
