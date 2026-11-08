pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The echo train on a dark glass screen: the dry hit, then every repeat at
// its time, tall by its level (dB), wide by its diffusion smear (shown
// three times wide), and
// darkening as the loop takes its highs. Stereo shows a left and a right
// lane, ping-pong bounces between them, mono shares one lane. The grid
// counts beats when synced, milliseconds when free.
Rectangle {
    id: view
    required property var controller
    required property var relay
    required property RelayTheme theme
    readonly property int revision: controller.revision + relay.revision
    readonly property int routing: revision >= 0 ? controller.choice("relay.routing") : 0
    readonly property bool synced: revision >= 0 && controller.choice("relay.sync") === 1
    readonly property real leftMs: revision >= 0 ? relay.time_ms(0) : 0
    readonly property real rightMs: revision >= 0 ? relay.time_ms(1) : 0
    // Show about six repeats of the longer line, at least half a second.
    readonly property real horizon: Math.max(0.5, Math.min(12, 6.4 * Math.max(leftMs, rightMs) / 1000))
    readonly property var echoes: revision >= 0 ? relay.echoes(horizon, 96) : []
    readonly property int echoCount: echoes.length
    readonly property real tailSeconds: revision >= 0 ? relay.tail_seconds(60) : 0
    property bool live: false

    // The diffusion smear is drawn this many times wider than it is, so a
    // few tens of milliseconds read at a multi-second scale.
    readonly property real smearWiden: 3
    // The smear's bell: sampled this many steps per unit either side of
    // the bar, out to bellHalfSteps.
    readonly property int bellHalfSteps: 22
    readonly property real bellStepsPerUnit: 10

    objectName: "relayEchoView"
    radius: theme.radiusLg
    color: theme.screen
    border.color: theme.screenEdge
    border.width: theme.strokeBold
    clip: true
    Accessible.role: Accessible.Graphic
    Accessible.name: "Echo train: " + echoCount + " repeats, tail " + Format.seconds(tailSeconds)

    function laneName(lane) {
        return routing === 2 ? "L+R" : lane === 0 ? "L" : "R"
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: view.border.width
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height
            const left = t.echoPlotLeft
            const right = w - t.echoPlotRight
            const top = t.echoPlotTop
            const bottom = h - t.echoPlotBottom
            const span = right - left
            const lanes = view.routing === 2 ? 1 : 2
            const laneHeight = (bottom - top) / lanes
            const xAt = s => left + span * s / view.horizon
            // Glass vignette.
            const glass = ctx.createLinearGradient(0, 0, 0, h)
            glass.addColorStop(0, t.glassTop)
            glass.addColorStop(1, t.glassBottom)
            ctx.fillStyle = glass
            ctx.fillRect(0, 0, w, h)

            // Grid: beats when synced, otherwise a round number of ms.
            ctx.font = t.typeCaption + "px " + t.mono
            ctx.textAlign = "center"
            let step = 0
            let labelOf = null
            if (view.synced) {
                step = 60 / view.relay.tempo
                while (view.horizon / step > 16)
                    step *= 2
                labelOf = i => {
                    const beats = Math.round(i * step * view.relay.tempo / 60)
                    const bar = Math.floor(beats / view.relay.beats_per_bar) + 1
                    const beat = beats % view.relay.beats_per_bar + 1
                    return bar + "." + beat
                }
            } else {
                const choices = [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2]
                step = choices[choices.length - 1]
                for (let i = 0; i < choices.length; ++i) {
                    if (view.horizon / choices[i] <= 12) {
                        step = choices[i]
                        break
                    }
                }
                labelOf = i => Format.seconds(i * step)
            }
            for (let i = 0; i * step <= view.horizon + 1e-9; ++i) {
                const x = xAt(i * step)
                ctx.strokeStyle = view.theme.screenGrid
                ctx.lineWidth = i === 0 ? t.echoOriginStroke : t.hairline
                ctx.beginPath()
                ctx.moveTo(x, top - t.gridOverhang)
                ctx.lineTo(x, bottom)
                ctx.stroke()
                if (i > 0) {
                    ctx.fillStyle = t.gridLabelInk
                    ctx.fillText(labelOf(i), x, h - t.gridLabelBottom)
                }
            }
            // dB guides per lane: 0, -20, -40 dB.
            for (let lane = 0; lane < lanes; ++lane) {
                const base = top + laneHeight * (lane + 1) - t.laneFloorInset
                const tall = laneHeight - t.laneHeadroom
                for (const db of [0, -20, -40]) {
                    const y = base - tall * (db + 60) / 60
                    ctx.strokeStyle = db === 0 ? t.dbGuideZero : t.dbGuide
                    ctx.setLineDash([t.dbGuideDash, t.dbGuideGap])
                    ctx.beginPath()
                    ctx.moveTo(left, y)
                    ctx.lineTo(right, y)
                    ctx.stroke()
                    ctx.setLineDash([])
                }
                ctx.strokeStyle = t.laneBaseline
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                ctx.moveTo(left, base)
                ctx.lineTo(right, base)
                ctx.stroke()
                ctx.fillStyle = lane === 0 ? view.theme.laneLeft : view.theme.laneRight
                ctx.font = "bold " + t.typeBody + "px " + t.mono
                ctx.textAlign = "center"
                ctx.fillText(view.laneName(lane), t.laneLabelX, base - tall / 2 + t.laneLabelDrop)
            }

            const laneOf = e => lanes === 1 ? 0 : e.channel === 1 ? 1 : 0
            const baseOf = lane => top + laneHeight * (lane + 1) - t.laneFloorInset
            const tallOf = () => laneHeight - t.laneHeadroom
            const heightOf = e => tallOf() * Math.max(0, Math.min(1, (e.db + 60) / 60))
            const colourOf = (e, alpha) => {
                const c = laneOf(e) === 0 ? view.theme.laneLeft : view.theme.laneRight
                // Lose highs: blend toward a dark umber as brightness falls.
                const b = Math.max(0, Math.min(1, e.brightness))
                const k = t.highLossFloor + (1 - t.highLossFloor) * b
                const u = t.highLossUmber
                return Qt.rgba(c.r * k + u[0] * (1 - k), c.g * k + u[1] * (1 - k),
                               c.b * k + u[2] * (1 - k), alpha)
            }

            // The dry hit on every lane.
            for (let lane = 0; lane < lanes; ++lane) {
                const base = baseOf(lane)
                ctx.fillStyle = view.theme.screenGlow
                ctx.fillRect(left - t.dryWidth / 2, base - tallOf(), t.dryWidth, tallOf())
            }
            ctx.fillStyle = view.theme.screenGlow
            ctx.font = "bold " + t.typeCaption + "px " + t.mono
            ctx.textAlign = "center"
            ctx.fillText("DRY", left, h - t.gridLabelBottom)

            // Ping-pong: the bounce between lanes.
            if (view.routing === 1 && view.echoes.length > 0) {
                ctx.strokeStyle = Qt.rgba(view.theme.screenGlow.r, view.theme.screenGlow.g,
                                          view.theme.screenGlow.b, t.bounceAlpha)
                ctx.lineWidth = t.bounceStroke
                ctx.beginPath()
                let px = xAt(0)
                let py = baseOf(0) - tallOf()
                ctx.moveTo(px, py)
                for (let i = 0; i < view.echoes.length; ++i) {
                    const e = view.echoes[i]
                    const x = xAt(e.seconds)
                    const y = baseOf(laneOf(e)) - heightOf(e)
                    ctx.quadraticCurveTo((px + x) / 2, Math.min(py, y) - t.bounceLift, x, y)
                    px = x
                    py = y
                }
                ctx.stroke()
            }

            // Decay envelopes through each lane's peaks.
            for (let lane = 0; lane < lanes; ++lane) {
                ctx.strokeStyle = t.decayLine
                ctx.setLineDash([t.decayDash, t.decayDash])
                ctx.lineWidth = t.hairline
                ctx.beginPath()
                let first = true
                for (let i = 0; i < view.echoes.length; ++i) {
                    const e = view.echoes[i]
                    if (laneOf(e) !== lane)
                        continue
                    const x = xAt(e.seconds)
                    const y = baseOf(lane) - heightOf(e)
                    if (first)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                    first = false
                }
                ctx.stroke()
                ctx.setLineDash([])
            }

            // Repeats: a glow, the smear, then the bar.
            for (let i = 0; i < view.echoes.length; ++i) {
                const e = view.echoes[i]
                const lane = laneOf(e)
                const x = xAt(e.seconds)
                if (x > right + t.echoClipSlack)
                    continue
                const base = baseOf(lane)
                const height = heightOf(e)
                // The diffusion smear, drawn wider than it is (smearWiden).
                const spread = e.spread_ms > 0
                    ? view.smearWiden * span * e.spread_ms / 1000 / view.horizon + t.smearPad : 0
                const glow = ctx.createLinearGradient(0, base - height - t.glowRise, 0, base)
                glow.addColorStop(0, colourOf(e, 0))
                glow.addColorStop(1, colourOf(e, t.glowAlpha))
                ctx.fillStyle = glow
                ctx.fillRect(x - t.glowWidth / 2, base - height - t.glowRise, t.glowWidth, height + t.glowRise)
                if (spread > 0) {
                    const reach = view.bellHalfSteps / view.bellStepsPerUnit
                    const bell = ctx.createLinearGradient(0, base - height, 0, base)
                    bell.addColorStop(0, colourOf(e, t.smearTopAlpha))
                    bell.addColorStop(1, colourOf(e, t.smearBaseAlpha))
                    ctx.fillStyle = bell
                    ctx.beginPath()
                    ctx.moveTo(x - spread * reach, base)
                    for (let k = -view.bellHalfSteps; k <= view.bellHalfSteps; ++k) {
                        const f = k / view.bellStepsPerUnit
                        ctx.lineTo(x + spread * f, base - height * Math.exp(-f * f))
                    }
                    ctx.lineTo(x + spread * reach, base)
                    ctx.closePath()
                    ctx.fill()
                }
                const bar = ctx.createLinearGradient(0, base - height, 0, base)
                bar.addColorStop(0, colourOf(e, 1))
                bar.addColorStop(1, colourOf(e, t.barBaseAlpha))
                ctx.fillStyle = bar
                ctx.fillRect(x - t.barWidth / 2, base - height, t.barWidth, height)
                ctx.fillStyle = colourOf(e, 1)
                ctx.beginPath()
                ctx.arc(x, base - height, t.barCapRadius, 0, Math.PI * 2)
                ctx.fill()
                ctx.fillStyle = t.barGlint
                ctx.beginPath()
                ctx.arc(x - t.glintOffset, base - height - t.glintOffset, t.glintRadius, 0, Math.PI * 2)
                ctx.fill()
            }
        }
        Connections {
            target: view
            function onEchoesChanged() { canvas.requestPaint() }
            function onRoutingChanged() { canvas.requestPaint() }
            function onSyncedChanged() { canvas.requestPaint() }
            function onWidthChanged() { canvas.requestPaint() }
            function onHeightChanged() { canvas.requestPaint() }
        }
    }

    // Readouts on the glass.
    Row {
        x: view.theme.echoReadoutX
        y: view.theme.echoReadoutY
        spacing: view.theme.echoReadoutSpacing
        Text {
            objectName: "relayTimeReadout"
            text: view.routing !== 2 && Math.abs(view.rightMs - view.leftMs) > 0.05
                  ? "L " + Format.milliseconds(view.leftMs) + "   R " + Format.milliseconds(view.rightMs)
                  : Format.milliseconds(view.leftMs)
            color: view.theme.screenGlow
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeLabel
            font.bold: true
        }
        Text {
            visible: view.synced
            text: view.controller.revision >= 0 && view.synced
                  ? view.controller.display("relay.division") + " @ " + Format.number(view.relay.tempo, 0) + " BPM" : ""
            color: view.theme.accent
            font.family: view.theme.mono
            font.pixelSize: view.theme.typeLabel
        }
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.echoReadoutRight
        y: view.theme.echoReadoutY
        text: (view.echoCount === 0 ? "NO REPEATS" : view.echoCount + (view.echoCount >= 96 ? "+" : "")
               + " REPEATS · TAIL " + (view.tailSeconds >= 59.5 ? "∞" : Format.seconds(view.tailSeconds)))
        color: view.theme.echoCountInk
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeSmall
    }
    Rectangle {
        objectName: "relayLiveLamp"
        anchors.right: parent.right
        anchors.rightMargin: view.theme.echoReadoutRight
        y: view.theme.liveLampY
        width: view.theme.liveLampSize
        height: view.theme.liveLampSize
        radius: width / 2
        visible: view.live
        color: view.theme.live
    }
}
