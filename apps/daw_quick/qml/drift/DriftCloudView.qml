pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "../devicekit" as Kit

// The grain cloud: buffer position across (the write head is "now" at the
// right, four seconds ago at the left) and grain pitch up. While the synth
// runs it draws the grains actually sounding (ADR-0037), each brightest at
// the middle of its window; otherwise a deterministic preview of what the
// scheduler spawns for these settings. Particle length is grain size,
// scatter is spray and jitter, rows are intervals, magenta grains play
// reversed and wide spread dims them. Freeze frosts the buffer; Sync marks
// the preview's spawn grid.
Rectangle {
    id: view
    required property var controller
    required property DriftTheme theme
    readonly property real previewSeconds: 2.0
    readonly property int previewGrainLimit: 260
    readonly property var preview: controller.revision >= 0 ? controller.grain_cloud(previewSeconds, previewGrainLimit) : []
    // The buffer shown across the view, and the pitch axis in octaves with
    // this much room past the outermost grain (semitones).
    readonly property real bufferSeconds: 4
    readonly property int octave: 12
    readonly property real pitchRoom: 3
    // How far the Blur macro and the wheel widen the spray (seconds).
    readonly property real blurSpray: 0.5
    // The star field: how many, scattered by two primes over two moduli,
    // every few drawn larger.
    readonly property int stars: 70
    readonly property var starPrimes: [7919, 104729]
    readonly property var starModuli: [997, 991]
    readonly property int largeStarEvery: 5
    // Grains: the shortest drawn (px), brightness lost at hard pan and
    // floor of the live window weight, and the arrow on reversed grains.
    readonly property real grainMinLength: 3
    readonly property real grainAlpha: 0.9
    readonly property real panDim: 0.45
    readonly property real weightFloor: 0.25
    readonly property real arrowMinLength: 8
    readonly property real arrowLength: 4
    readonly property real arrowSpread: 3
    // Sync ticks: every bar's first tick is long.
    readonly property int ticksPerBar: 4
    // Where the aurora wash turns teal.
    readonly property real washMiddle: 0.55
    // Live telemetry from the panel: whether the synth publishes, and its
    // grains [{key, behind, semitones, reverse, phase, frozen}].
    property bool live: false
    property var liveGrains: []
    readonly property real grainSeconds: controller.revision >= 0 ? controller.value("drift.grain.size") : 0.1
    // What is drawn: live grains in the preview's shape, or the preview.
    readonly property var grains: {
        if (!live)
            return preview
        const out = []
        for (let i = 0; i < liveGrains.length; ++i) {
            const g = liveGrains[i]
            out.push({ position: g.behind, semitones: g.semitones, length: grainSeconds,
                       reverse: g.reverse, pan: 0, time: 0,
                       weight: Math.sin(Math.PI * Math.max(0, Math.min(1, g.phase))) })
        }
        return out
    }
    readonly property bool liveFrozen: {
        for (let i = 0; i < liveGrains.length; ++i) {
            if (liveGrains[i].frozen)
                return true
        }
        return false
    }
    readonly property bool frozen: live ? liveFrozen
        : controller.revision >= 0 && controller.choice("drift.freeze") === 1
    readonly property bool synced: controller.revision >= 0 && controller.choice("drift.sync") === 1
    readonly property real lag: controller.revision >= 0 ? controller.value("drift.grain.position") : 0
    readonly property real spray: controller.revision >= 0
        ? Math.min(bufferSeconds, controller.value("drift.grain.spray") + blurSpray * Math.min(1, controller.value("drift.blur")
                                                                          + controller.value("drift.perf.wheel")))
        : 0
    // Pitch range: at least an octave either side, widened to fit every grain.
    readonly property var range: {
        let lo = -octave
        let hi = octave
        for (let i = 0; i < grains.length; ++i) {
            lo = Math.min(lo, Math.floor((grains[i].semitones - pitchRoom) / octave) * octave)
            hi = Math.max(hi, Math.ceil((grains[i].semitones + pitchRoom) / octave) * octave)
        }
        return [lo, hi]
    }

    // Whether any grain drawn plays reversed (its arrow points back).
    readonly property bool anyReversed: {
        for (let i = 0; i < grains.length; ++i) {
            if (grains[i].reverse)
                return true
        }
        return false
    }
    // The frame's scales: seconds behind the write head (NOW at the right)
    // and pitch in semitones, an octave a tick.
    readonly property int secondTicks: Math.round(bufferSeconds)
    readonly property var timeTicks: {
        const ticks = []
        for (let k = secondTicks; k >= 0; --k)
            ticks.push({ at: 1 - k / bufferSeconds, label: k === 0 ? "NOW" : Format.number(-k, 0) })
        return ticks
    }
    readonly property var pitchTicks: {
        const ticks = []
        for (let s = range[0]; s <= range[1]; s += octave)
            ticks.push({ at: (s - range[0]) / (range[1] - range[0]), label: Format.signed_number(s, 0) })
        return ticks
    }

    objectName: "driftCloudView"
    radius: theme.radiusXl
    color: theme.well
    border.color: theme.wellEdge
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: (live ? "Live grain cloud, " : "Grain cloud preview, ") + grains.length + " grains"
        + (frozen ? ", frozen" : "") + (synced ? ", synced" : "")

    // Soft aurora light behind the cloud.
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        opacity: view.theme.cloudWashOpacity
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: view.theme.cloudWashLeft }
            GradientStop { position: view.washMiddle; color: view.theme.cloudWashMiddle }
            GradientStop { position: 1.0; color: view.theme.cloudWashRight }
        }
    }

    // The cloud inside its scales; LIVE while the synth's own grains are
    // drawn, else a PREVIEW of what these settings spawn.
    Kit.PlotFrame {
        id: frame
        objectName: "driftCloudFrame"
        anchors.fill: parent
        anchors.leftMargin: view.theme.cloudFrameInset
        anchors.rightMargin: view.theme.cloudFrameInset
        anchors.topMargin: view.theme.cloudPlotVertical
        anchors.bottomMargin: view.theme.cloudFrameInset
        theme: view.theme
        live: view.live
        xUnit: "s"
        yUnit: "st"
        xTicks: view.timeTicks
        yTicks: view.pitchTicks

        Canvas {
            id: canvas
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                const w = width
                const h = height
                const lo = view.range[0]
                const hi = view.range[1]
                const buffer = view.bufferSeconds
                const snap = view.theme.hairline / 2
                const xOf = seconds => w - Math.max(0, Math.min(buffer, seconds)) / buffer * w
                const yOf = semitones => h - (semitones - lo) / (hi - lo) * h

                // A few fixed stars.
                ctx.fillStyle = view.theme.star
                for (let i = 0; i < view.stars; ++i) {
                    const sx = ((i * view.starPrimes[0]) % view.starModuli[0]) / view.starModuli[0] * w
                    const sy = ((i * view.starPrimes[1]) % view.starModuli[1]) / view.starModuli[1] * h
                    const size = i % view.largeStarEvery === 0 ? view.theme.starLarge : view.theme.starSmall
                    ctx.fillRect(sx, sy, size, size)
                }
                // Octave lines and second marks.
                ctx.lineWidth = view.theme.hairline
                for (let s = lo; s <= hi; s += view.octave) {
                    ctx.strokeStyle = s === 0 ? view.theme.cloudZeroLine : view.theme.grid
                    const y = Math.round(yOf(s)) + snap
                    ctx.beginPath()
                    ctx.moveTo(0, y)
                    ctx.lineTo(w, y)
                    ctx.stroke()
                }
                ctx.strokeStyle = view.theme.grid
                for (let t = 1; t < buffer; ++t) {
                    const x = Math.round(xOf(t)) + snap
                    ctx.beginPath()
                    ctx.moveTo(x, 0)
                    ctx.lineTo(x, h)
                    ctx.stroke()
                }

                // Spray: the band grains scatter over around the scan centre.
                const centre = xOf(view.lag)
                const band = view.spray / buffer * w
                const sprayFill = ctx.createLinearGradient(centre - band, 0, centre + band, 0)
                sprayFill.addColorStop(0, "transparent")
                sprayFill.addColorStop(0.5, Qt.rgba(view.theme.accent.r, view.theme.accent.g, view.theme.accent.b,
                                                    view.theme.sprayAlpha))
                sprayFill.addColorStop(1, "transparent")
                ctx.fillStyle = sprayFill
                ctx.fillRect(centre - band, 0, 2 * band, h)

                // Grains: a soft glow, then the bright core.
                const teal = view.theme.accent
                const violet = view.theme.accent2
                const magenta = view.theme.magenta
                for (const pass of [0, 1]) {
                    for (let i = 0; i < view.grains.length; ++i) {
                        const g = view.grains[i]
                        const x = xOf(g.position)
                        const y = yOf(g.semitones)
                        const len = Math.max(view.grainMinLength, g.length / buffer * w)
                        const mix = Math.max(0, Math.min(1, (g.semitones - lo) / (hi - lo)))
                        const c = g.reverse ? magenta
                            : Qt.rgba(teal.r + (violet.r - teal.r) * mix, teal.g + (violet.g - teal.g) * mix,
                                      teal.b + (violet.b - teal.b) * mix, 1)
                        const alpha = (view.grainAlpha - view.panDim * Math.abs(g.pan))
                            * (g.weight !== undefined ? view.weightFloor + (1 - view.weightFloor) * g.weight : 1)
                        ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, pass === 0 ? alpha * view.theme.grainGlowAlpha : alpha)
                        ctx.lineWidth = pass === 0 ? view.theme.grainGlowWidth : view.theme.strokeBold
                        ctx.lineCap = "round"
                        ctx.beginPath()
                        ctx.moveTo(x - len / 2, y)
                        ctx.lineTo(x + len / 2, y)
                        ctx.stroke()
                        if (pass === 1 && g.reverse && len > view.arrowMinLength) {
                            ctx.beginPath()
                            ctx.moveTo(x - len / 2 + view.arrowLength, y - view.arrowSpread)
                            ctx.lineTo(x - len / 2, y)
                            ctx.lineTo(x - len / 2 + view.arrowLength, y + view.arrowSpread)
                            ctx.stroke()
                        }
                    }
                }

                // The scan centre.
                ctx.setLineDash(view.theme.scanDash)
                ctx.strokeStyle = view.theme.scanLine
                ctx.lineWidth = view.theme.hairline
                ctx.beginPath()
                ctx.moveTo(Math.round(centre) + snap, 0)
                ctx.lineTo(Math.round(centre) + snap, h)
                ctx.stroke()
                ctx.setLineDash([])

                // The write head, "now".
                ctx.strokeStyle = view.frozen ? view.theme.frost : teal
                ctx.lineWidth = view.theme.strokeBold
                ctx.beginPath()
                ctx.moveTo(w - 1, 0)
                ctx.lineTo(w - 1, h)
                ctx.stroke()

                // Sync: the spawn grid across the preview's two seconds, a lane
                // of ticks along the bottom.
                if (view.synced && !view.live) {
                    const lane = view.theme.syncLaneHeight
                    const inset = view.theme.syncTickInset
                    ctx.fillStyle = Qt.rgba(violet.r, violet.g, violet.b, view.theme.syncLaneAlpha)
                    ctx.fillRect(0, h - lane, w, lane)
                    ctx.strokeStyle = Qt.rgba(violet.r, violet.g, violet.b, view.theme.syncTickAlpha)
                    ctx.lineWidth = view.theme.syncTickWidth
                    for (let i = 0; i < view.grains.length; ++i) {
                        const x = Math.round(view.grains[i].time / view.previewSeconds * (w - 2 * inset)) + inset + snap
                        ctx.beginPath()
                        ctx.moveTo(x, h - (i % view.ticksPerBar === 0 ? view.theme.syncTickMajor : view.theme.syncTickMinor))
                        ctx.lineTo(x, h - inset)
                        ctx.stroke()
                    }
                }
            }
        }

        // Freeze: frost over the held buffer.
        Rectangle {
            objectName: "driftFrost"
            anchors.fill: canvas
            visible: view.frozen
            radius: view.theme.radiusMd
            gradient: Gradient {
                GradientStop { position: 0; color: view.theme.frostTop }
                GradientStop { position: 1; color: view.theme.frostBottom }
            }
            border.color: view.theme.frostEdge
            Canvas {
                id: crystals
                // Ice crystals: six spokes, each with a branch from partway out;
                // one in each of two corners and one along the top edge.
                readonly property int spokes: 6
                readonly property real branchFrom: 0.55
                readonly property real branchTo: 0.75
                readonly property real branchTurn: 0.5
                readonly property real cornerCrystal: 18
                readonly property real cornerCrystalRadius: 12
                readonly property real farCrystalX: 22
                readonly property real farCrystalY: 20
                readonly property real farCrystalRadius: 14
                readonly property real topCrystalAt: 0.42
                readonly property real topCrystalY: 14
                readonly property real topCrystalRadius: 8
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    // Ice crystals in the corners.
                    ctx.strokeStyle = view.theme.frostCrystal
                    ctx.lineWidth = view.theme.hairline
                    const crystal = (cx, cy, r) => {
                        for (let k = 0; k < crystals.spokes; ++k) {
                            const a = k * 2 * Math.PI / crystals.spokes
                            ctx.beginPath()
                            ctx.moveTo(cx, cy)
                            ctx.lineTo(cx + Math.cos(a) * r, cy + Math.sin(a) * r)
                            ctx.stroke()
                            ctx.beginPath()
                            ctx.moveTo(cx + Math.cos(a) * r * crystals.branchFrom, cy + Math.sin(a) * r * crystals.branchFrom)
                            ctx.lineTo(cx + Math.cos(a + crystals.branchTurn) * r * crystals.branchTo, cy + Math.sin(a + crystals.branchTurn) * r * crystals.branchTo)
                            ctx.stroke()
                        }
                    }
                    crystal(crystals.cornerCrystal, crystals.cornerCrystal, crystals.cornerCrystalRadius)
                    crystal(width - crystals.farCrystalX, height - crystals.farCrystalY, crystals.farCrystalRadius)
                    crystal(width * crystals.topCrystalAt, crystals.topCrystalY, crystals.topCrystalRadius)
                }
            }
            Text {
                x: view.theme.frostLabelInset
                y: view.theme.frostLabelY
                text: "❄ FROZEN"
                color: view.theme.frost
                font.pixelSize: view.theme.typeSmall
                font.weight: Font.Black
                font.letterSpacing: view.theme.frostLabelSpacing
            }
        }
        Text {
            objectName: "driftSyncCaption"
            visible: view.synced && !view.live
            x: view.theme.syncCaptionInset
            y: canvas.height - view.theme.syncCaptionLift
            text: "SPAWN GRID · " + (view.controller.revision >= 0 ? view.controller.display("drift.sync.division") : "")
                + " · 2 s AT 120 BPM"
            color: view.theme.accent2
            font.pixelSize: view.theme.captionSize
            font.bold: true
            font.letterSpacing: view.theme.captionSpacing
        }
    }

    Text {
        id: cloudTitle
        x: view.theme.cloudTitleX
        y: view.theme.cloudTitleY
        text: "GRAIN CLOUD"
        color: view.theme.accent
        font.pixelSize: view.theme.typeCaption
        font.bold: true
        font.letterSpacing: view.theme.cloudTitleSpacing
    }
    // What the arrow on a magenta grain means.
    Text {
        objectName: "driftReversedKey"
        visible: view.anyReversed
        x: cloudTitle.x + cloudTitle.implicitWidth + view.theme.cloudKeyGap
        y: view.theme.cloudTitleY
        text: "◂ MAGENTA GRAINS PLAY REVERSED"
        color: view.theme.magenta
        font.pixelSize: view.theme.captionSize
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
    Text {
        objectName: "driftCloudCaption"
        anchors.right: parent.right
        anchors.rightMargin: view.theme.cloudTitleX
        y: view.theme.cloudTitleY
        visible: view.live
        text: view.grains.length + (view.grains.length === 1 ? " GRAIN" : " GRAINS")
        color: view.theme.accent
        font.pixelSize: view.theme.captionSize
        font.bold: true
        font.letterSpacing: view.theme.captionSpacing
    }
    onGrainsChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onFrozenChanged: canvas.requestPaint()
    onSyncedChanged: canvas.requestPaint()
    onLagChanged: canvas.requestPaint()
    onSprayChanged: canvas.requestPaint()
}
