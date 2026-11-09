pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "ChopGate.js" as Gate

// One pattern of the bass with the gate over it: the step grid (beats stronger), the input in slate with what is heard in
// coral over it while it plays, the gate's curve along the top, a violet
// mark on each repeating step and the playhead. Steps chance skips this
// time round are shaded. The curve is drawn from the values; the bass and
// the playhead come from the telemetry (layout in chop.hpp).
Rectangle {
    id: hero
    required property var controller
    required property ChopTheme theme
    property var live: ({ live: false })

    readonly property int revision: controller.revision
    readonly property bool playing: live.live === true && live.phase !== undefined && live.phase >= 0
    // The pattern's steps (16 or 32), from chop.steps' choice.
    required property int stepCount
    readonly property int mode: revision >= 0 ? controller.choice("chop.mode") : 0
    // The curve's top band and the waveform's half height, in pixels.
    readonly property real curveTop: theme.heroCurveTop
    readonly property real curveHeight: theme.heroCurveHeight
    readonly property real waveMargin: theme.heroWaveMargin
    // The preview bar's height, of the waveform's half height.
    readonly property real previewBarScale: 0.6

    objectName: "chopHero"
    radius: theme.heroRadius
    color: theme.well
    border.color: theme.wellEdge
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: "The bass with the gate drawn over it"

    function steps() {
        const out = []
        for (let s = 0; s < stepCount; ++s) {
            out.push({
                level: controller.value(Gate.stepId(s, "level")),
                length: controller.value(Gate.stepId(s, "length")),
                ratchet: Math.max(1, Math.round(controller.value(Gate.stepId(s, "ratchet")))),
                repeat: Math.round(controller.value(Gate.stepId(s, "repeat")))
            })
        }
        return out
    }
    function playedSteps() {
        const out = []
        for (let s = 0; s < stepCount; ++s)
            out.push(playing && live.played !== undefined ? live.played[s] : true)
        return out
    }

    onRevisionChanged: canvas.requestPaint()
    onLiveChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        objectName: "chopHeroCanvas"
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width, h = height, mid = h / 2
            const theme = hero.theme
            const count = hero.stepCount
            const layout = Gate.slots(count, hero.controller.value("chop.swing"),
                                      Math.round(hero.controller.value("chop.rotate")) % count)
            const steps = hero.steps()
            const played = hero.playedSteps()
            const depth = hero.controller.value("chop.depth")
            // Edges as fractions of a step at the preview tempo.
            const division = hero.controller.parameter("chop.division").choices[
                hero.controller.choice("chop.division")]
            const stepSeconds = Gate.divisionBeats(division) * Gate.secondsPerMinute / Gate.previewBpm
            const edge = hero.controller.value("chop.smooth") / Gate.millisecondsPerSecond / stepSeconds
            const release = edge * (1 + hero.controller.value("chop.shape") * Gate.releaseStretch)
            const gated = hero.mode !== Gate.mode.repeat // Repeat mode does not gate.
            const gain = x => gated ? 1 - depth * (1 - Gate.gateAt(x, steps, played, layout, edge, release)) : 1

            // Shading for the steps chance skips, then the grid.
            for (let k = 0; k < count; ++k) {
                const slot = layout[k]
                if (played[slot.step] === false) {
                    ctx.fillStyle = Qt.rgba(theme.teal.r, theme.teal.g, theme.teal.b, theme.skipShadeAlpha)
                    ctx.fillRect(slot.start * w, 0, (slot.end - slot.start) * w, h)
                }
                ctx.fillStyle = k % Gate.beatSteps === 0 ? theme.gridStrong : theme.grid
                ctx.fillRect(Math.round(slot.start * w), 0, theme.hairline, h)
            }

            // The bass: the input in slate, what is heard in coral.
            const input = hero.playing && hero.live.input !== undefined ? hero.live.input : []
            const output = hero.playing && hero.live.output !== undefined ? hero.live.output : []
            let top = 0
            for (let i = 0; i < input.length; ++i)
                top = Math.max(top, input[i])
            if (top > 1e-4) {
                const scale = (h / 2 - hero.waveMargin) / top
                const n = input.length
                for (let px = 0; px < w; ++px) {
                    const i = Math.min(n - 1, Math.floor(px / w * n))
                    const a = input[i] * scale
                    const b = (output[i] !== undefined ? output[i] : 0) * scale
                    ctx.fillStyle = theme.bass
                    ctx.fillRect(px, mid - a, 1, a * 2)
                    ctx.fillStyle = theme.blade
                    ctx.globalAlpha = theme.heardAlpha
                    ctx.fillRect(px, mid - b, 1, b * 2)
                    ctx.globalAlpha = 1
                }
            } else {
                // Before anything plays: the gate applied to a flat bar.
                for (let px = 0; px < w; ++px) {
                    const a = (h / 2 - hero.waveMargin) * hero.previewBarScale
                    const g = gain(px / w)
                    ctx.fillStyle = theme.bass
                    ctx.fillRect(px, mid - a, 1, a * 2)
                    ctx.fillStyle = theme.blade
                    ctx.globalAlpha = theme.previewGateAlpha
                    ctx.fillRect(px, mid - a * g, 1, a * g * 2)
                    ctx.globalAlpha = 1
                }
            }

            // The gate's curve.
            ctx.beginPath()
            for (let px = 0; px <= w; ++px) {
                const y = hero.curveTop + (1 - gain(Math.min(px / w, 0.999999))) * hero.curveHeight
                if (px === 0)
                    ctx.moveTo(px, y)
                else
                    ctx.lineTo(px, y)
            }
            ctx.strokeStyle = theme.blade2
            ctx.lineWidth = theme.heroCurveWidth
            ctx.stroke()

            // Repeats, where they sound.
            if (hero.mode !== Gate.mode.gate) {
                ctx.font = "600 " + theme.typeSmall + "px " + theme.sans
                ctx.fillStyle = theme.violet
                for (let k = 0; k < count; ++k) {
                    const r = steps[layout[k].step].repeat
                    if (r > 0)
                        ctx.fillText("↻ " + Format.quantity(Math.pow(2, r), "×", 0), layout[k].start * w + theme.repeatMarkInset, h - theme.repeatMarkBaseline)
                }
            }

            if (hero.playing) {
                ctx.fillStyle = theme.ink
                ctx.fillRect(hero.live.phase * w, 0, theme.playheadWidth, h)
            }
        }
    }

    Text {
        objectName: "chopHeroHint"
        visible: !hero.playing
        x: hero.theme.hintInset
        y: hero.theme.hintY
        text: "PLAY TO SEE THE BASS UNDER THE GATE"
        color: hero.theme.inkFaint
        font.family: hero.theme.sans
        font.pixelSize: hero.theme.typeCaption
        font.bold: true
        font.letterSpacing: hero.theme.hintSpacing
    }
}
