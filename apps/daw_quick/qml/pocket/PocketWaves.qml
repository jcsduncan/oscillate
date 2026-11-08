pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The last 0.7 s, as Pocket hears it, drawn as envelopes in two lanes on
// one time axis: the kick in amber on top, and below it the bass coming in
// as a cyan outline, the bass going out solid cyan and the space between
// them green, so each hit lines up with the bass carved out under it.
// Columns are (min, max) pairs, oldest first; each envelope holds its peak
// over +-6 columns (16 ms, longer than a sub's period) so a 40 Hz wave reads
// as its level, not its cycles. Each lane scales to its own loudest peak,
// easing down slowly so the picture does not jump.
Item {
    id: waves
    required property PocketTheme theme
    property bool live: false
    property var kick: []
    property var bassIn: []
    property var bassOut: []
    // Seconds the history spans.
    property real seconds: 0.68
    // The lanes' drawn scales, eased toward their loudest peaks.
    property real kickScale: 0.5
    property real bassScale: 0.5
    readonly property int columns: Math.floor(kick.length / 2)
    readonly property int hold: 6

    // A held peak envelope of (min, max) columns.
    function envelope(list) {
        const n = Math.floor(list.length / 2)
        const peaks = []
        for (let i = 0; i < n; ++i)
            peaks.push(Math.max(Math.abs(list[2 * i]), Math.abs(list[2 * i + 1])))
        const out = []
        for (let i = 0; i < n; ++i) {
            let top = 0
            for (let j = Math.max(0, i - hold); j <= Math.min(n - 1, i + hold); ++j)
                top = Math.max(top, peaks[j])
            out.push(top)
        }
        return out
    }
    readonly property var kickEnvelope: envelope(kick)
    readonly property var inEnvelope: envelope(bassIn)
    readonly property var outEnvelope: envelope(bassOut)

    readonly property real px: theme.plotLeft
    readonly property real py: theme.wavesPlotTop
    readonly property real pw: width - px - theme.plotRight
    readonly property real ph: height - py - theme.wavesPlotBottom
    // The kick lane takes the top third, the bass the rest.
    readonly property real kickShare: 0.34
    readonly property real kickHeight: Math.round(ph * kickShare)
    readonly property real bassTop: py + kickHeight + theme.laneGap
    readonly property real bassHeight: ph - kickHeight - theme.laneGap

    objectName: "pocketWaves"
    implicitWidth: theme.wavesWidth
    implicitHeight: theme.wavesHeight

    function peakOf(values) {
        let top = 0.02
        for (let i = 0; i < values.length; ++i)
            top = Math.max(top, values[i])
        return top
    }
    function ease(current, target) {
        return target > current ? target : current + (target - current) * 0.08
    }
    onKickChanged: {
        kickScale = ease(kickScale, peakOf(kickEnvelope) * 1.08)
        plot.requestPaint()
    }
    onBassInChanged: {
        bassScale = ease(bassScale, Math.max(peakOf(inEnvelope), peakOf(outEnvelope)) * 1.08)
        plot.requestPaint()
    }
    onBassOutChanged: plot.requestPaint()
    onWidthChanged: plot.requestPaint()

    Canvas {
        id: plot
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        // A symmetric band of +-envelope about `mid`, or (with `inner`) the
        // ring between two envelopes.
        function band(ctx, mid, k, outer, inner) {
            const n = outer.length
            if (n === 0)
                return
            const xOf = i => waves.px + waves.pw * (i + 0.5) / n
            for (const sign of [-1, 1]) {
                ctx.beginPath()
                ctx.moveTo(xOf(0), mid + sign * outer[0] * k)
                for (let i = 1; i < n; ++i)
                    ctx.lineTo(xOf(i), mid + sign * outer[i] * k)
                for (let i = n - 1; i >= 0; --i)
                    ctx.lineTo(xOf(i), mid + sign * (inner ? inner[i] : 0) * k)
                ctx.closePath()
                ctx.fill()
            }
        }
        function edge(ctx, mid, k, values) {
            const n = values.length
            if (n === 0)
                return
            const xOf = i => waves.px + waves.pw * (i + 0.5) / n
            for (const sign of [-1, 1]) {
                ctx.beginPath()
                ctx.moveTo(xOf(0), mid + sign * values[0] * k)
                for (let i = 1; i < n; ++i)
                    ctx.lineTo(xOf(i), mid + sign * values[i] * k)
                ctx.stroke()
            }
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = waves.theme
            ctx.fillStyle = t.well
            ctx.fillRect(waves.px, waves.py, waves.pw, waves.ph)
            // Time ticks every 100 ms back from now.
            ctx.lineWidth = t.hairline
            ctx.font = t.typeMicro + "px '" + t.mono + "'"
            ctx.textAlign = "center"
            ctx.textBaseline = "top"
            for (let ms = 0; ms <= waves.seconds * 1000; ms += 100) {
                const x = Math.round(waves.px + waves.pw * (1 - ms / 1000 / waves.seconds)) + 0.5
                ctx.strokeStyle = t.grid
                ctx.beginPath()
                ctx.moveTo(x, waves.py)
                ctx.lineTo(x, waves.py + waves.ph)
                ctx.stroke()
                ctx.fillStyle = t.inkFaint
                ctx.fillText(ms === 0 ? "now" : Format.milliseconds(-ms), x, waves.py + waves.ph + t.timeLabelGap)
            }
            // The lane divider and centre lines.
            const kickMid = waves.py + waves.kickHeight / 2
            const bassMid = waves.bassTop + waves.bassHeight / 2
            ctx.strokeStyle = t.seam
            ctx.beginPath()
            ctx.moveTo(waves.px, Math.round(waves.bassTop - t.laneGap / 2) + 0.5)
            ctx.lineTo(waves.px + waves.pw, Math.round(waves.bassTop - t.laneGap / 2) + 0.5)
            ctx.stroke()
            ctx.strokeStyle = t.grid
            for (const y of [kickMid, bassMid]) {
                ctx.beginPath()
                ctx.moveTo(waves.px, Math.round(y) + 0.5)
                ctx.lineTo(waves.px + waves.pw, Math.round(y) + 0.5)
                ctx.stroke()
            }
            ctx.save()
            ctx.beginPath()
            ctx.rect(waves.px, waves.py, waves.pw, waves.ph)
            ctx.clip()
            // Kick lane.
            const kk = (waves.kickHeight / 2 - t.envelopeHeadroom) / waves.kickScale
            ctx.fillStyle = Qt.rgba(t.kick.r, t.kick.g, t.kick.b, t.kickFillAlpha)
            band(ctx, kickMid, kk, waves.kickEnvelope)
            ctx.strokeStyle = t.kick
            edge(ctx, kickMid, kk, waves.kickEnvelope)
            // Bass lane: what came in, what went out, and the cut between.
            const bk = (waves.bassHeight / 2 - t.envelopeHeadroom) / waves.bassScale
            ctx.fillStyle = Qt.rgba(t.bass.r, t.bass.g, t.bass.b, t.bassOutAlpha)
            band(ctx, bassMid, bk, waves.outEnvelope)
            ctx.fillStyle = Qt.rgba(t.cleared.r, t.cleared.g, t.cleared.b, t.cutFillAlpha)
            band(ctx, bassMid, bk, waves.inEnvelope, waves.outEnvelope)
            ctx.lineWidth = t.bassInStroke
            ctx.strokeStyle = t.bass
            edge(ctx, bassMid, bk, waves.inEnvelope)
            ctx.restore()
            ctx.strokeStyle = t.wellEdge
            ctx.lineWidth = t.hairline
            ctx.strokeRect(waves.px + 0.5, waves.py + 0.5, waves.pw - 1, waves.ph - 1)
        }
    }

    // The keys, beside each lane.
    component Key: Row {
        id: key
        required property string name
        required property color fill
        required property color edge
        spacing: waves.theme.waveKeyGap
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: waves.theme.waveKeySwatch
            height: waves.theme.waveKeySwatch
            radius: waves.theme.waveKeyRadius
            color: key.fill
            border.color: key.edge
        }
        Text {
            text: key.name
            color: waves.theme.inkFaint
            font.pixelSize: waves.theme.waveKeySize
            font.bold: true
            font.letterSpacing: waves.theme.waveKeySpacing
        }
    }
    Key {
        x: waves.theme.waveKeyX
        y: waves.py + waves.kickHeight / 2 - height / 2
        name: "KICK"
        fill: waves.theme.kick
        edge: waves.theme.kick
    }
    Column {
        x: waves.theme.waveKeyX
        y: waves.bassTop + waves.bassHeight / 2 - height / 2
        spacing: waves.theme.waveKeyGap
        Key {
            name: "IN"
            fill: "transparent"
            edge: waves.theme.bass
        }
        Key {
            name: "OUT"
            fill: waves.theme.bass
            edge: waves.theme.bass
        }
        Key {
            name: "CUT"
            fill: waves.theme.cleared
            edge: waves.theme.cleared
        }
    }
}
