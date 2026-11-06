pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// An envelope's shape (NOVA-MOD-01): attack, hold, decay, sustain and
// release with their segment curves. Drag a node to set its stage: across
// for the attack, hold, decay or release time, up and down for the sustain
// (the decay's node does both). Drag up or down on a segment between nodes
// to bend it. Each drag is one undo step. A dot rides the newest voice's
// stage.
Kit.EnvelopeView {
    id: view
    required property var panel
    required property string envelope
    readonly property int number: parseInt(envelope.substring(4))
    readonly property string base: "nova." + envelope
    readonly property var shape: {
        if (controller.revision < 0)
            return null
        const v = id => controller.value(base + "." + id)
        return { attack: v("attack"), hold: v("hold"), decay: v("decay"), sustain: v("sustain"),
                 release: v("release"), attackCurve: v("attack_curve"), decayCurve: v("decay_curve"),
                 releaseCurve: v("release_curve") }
    }
    readonly property int stage: panel.live ? Math.round(panel.at(Contract.T.envStage + number - 1)) : 0
    readonly property real progress: panel.at(Contract.T.envProgress + number - 1)
    readonly property real level: panel.at(Contract.T.envLevel + number - 1)
    // A drag across the whole width adds this much to a stage time's square
    // root: the time axis is compressed the same way (widths()).
    readonly property real timeSpan: 4.5
    // The time axis: a stage's width grows with its time to this power, plus
    // a floor so a zero-time stage still shows; the sustain takes a fixed
    // share.
    readonly property real timeCompression: 0.5
    readonly property real shortestStage: 0.04
    readonly property real sustainShare: 0.45
    // Samples drawn along each stage.
    readonly property int stageSamples: 40
    // Pixels of vertical drag that bend a segment through one unit of curve.
    readonly property real bendPixels: 80
    // The plot's level range, top to bottom, in the view.
    readonly property real levelTop: theme.envelopePlotTop + theme.envelopeHeadroom
    readonly property real levelSpan: height - levelTop - theme.envelopePlotBottom

    objectName: "novaEnvelopeView:" + envelope
    controller: panel.controller
    theme: panel.theme
    // Envelopes draw in their amber; the voice's playhead in white over it.
    accent: panel.sourceAccent(envelope)
    clip: true
    points: shape === null ? [] : outline()
    // The start, then every stage's end.
    knots: shape === null ? [] : [[0, 0]].concat(nodes().map(n => [n.x, n.y]))
    playhead: shape !== null && panel.live && stage > 0 ? playheadAt() : -1
    playheadLevel: level

    // Segment widths on a compressed time axis so short stages stay visible.
    function widths() {
        const w = s => Math.pow(Math.max(0, s), timeCompression) + shortestStage
        const parts = [w(shape.attack), shape.hold > 0 ? w(shape.hold) : 0, w(shape.decay), sustainShare, w(shape.release)]
        const total = parts.reduce((a, b) => a + b, 0)
        return parts.map(p => p / total)
    }
    function bentLevel(from, to, t, k) {
        return from + (to - from) * Contract.bend(t, k)
    }
    // The stages (from, to, curve) sampled into the outline.
    function outline() {
        const parts = widths()
        const s = shape
        const stages = [[0, 1, s.attackCurve], [1, 1, 0], [1, s.sustain, s.decayCurve],
                        [s.sustain, s.sustain, 0], [s.sustain, 0, s.releaseCurve]]
        const points = [[0, 0]]
        let at = 0
        for (let k = 0; k < stages.length; ++k) {
            const [from, to, curve] = stages[k]
            for (let i = 1; i <= stageSamples; ++i)
                points.push([at + parts[k] * i / stageSamples, bentLevel(from, to, i / stageSamples, curve)])
            at += parts[k]
        }
        return points
    }
    // The newest voice's place: stage 1 attack .. 5 release.
    function playheadAt() {
        const parts = widths()
        let start = 0
        for (let k = 0; k < stage - 1; ++k)
            start += parts[k]
        return start + parts[stage - 1] * Math.max(0, Math.min(1, progress))
    }
    // The nodes a press can grab, nearest last-drawn first: each with the
    // time it sets across and whether it sets the sustain.
    function nodes() {
        const parts = widths()
        const ends = []
        let at = 0
        for (let k = 0; k < parts.length; ++k) {
            at += parts[k]
            ends.push(at)
        }
        return [{ x: ends[0], y: 1, time: "attack", sustain: false },
                { x: ends[1], y: 1, time: shape.hold > 0 ? "hold" : "", sustain: false },
                { x: ends[2], y: shape.sustain, time: "decay", sustain: true },
                { x: ends[3], y: shape.sustain, time: "", sustain: true },
                { x: ends[4], y: 0, time: "release", sustain: false }]
    }
    function nodeAt(px, py) {
        const list = nodes()
        let best = null
        let nearest = theme.envelopeKnotGrab
        for (let i = 0; i < list.length; ++i) {
            if (list[i].time === "" && !list[i].sustain)
                continue
            const d = Math.hypot(plotX(list[i].x) - px, plotY(list[i].y) - py)
            if (d <= nearest) {
                nearest = d
                best = list[i]
            }
        }
        return best
    }

    Text {
        x: view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: "ENVELOPE · ENV " + view.number + (view.number === 1 ? " · AMP" : "") + " · DRAG A NODE · DRAG A SEGMENT TO BEND IT"
        color: view.theme.envelopeLabelInk
        font.family: view.theme.envelopeLabelFont
        font.pixelSize: view.theme.envelopeLabelSize
        font.weight: Font.Bold
        font.letterSpacing: view.theme.envelopeLabelSpacing
    }

    MouseArea {
        objectName: "novaEnvelopeDrag:" + view.envelope
        property string curveId: ""
        property real startY: 0
        property real startValue: 0
        // A node drag: the time it sets (or "") and whether it sets the sustain.
        property string timeId: ""
        property string sustainId: ""
        property real startX: 0
        property real startRoot: 0
        anchors.fill: parent
        preventStealing: true
        hoverEnabled: true
        cursorShape: view.shape !== null && containsMouse && view.nodeAt(mouseX, mouseY) !== null
            ? Qt.SizeAllCursor : Qt.SizeVerCursor
        enabled: view.panel.controller.editable && view.shape !== null
        onPressed: mouse => {
            const node = view.nodeAt(mouse.x, mouse.y)
            if (node !== null) {
                startX = mouse.x
                timeId = node.time !== "" ? view.base + "." + node.time : ""
                sustainId = node.sustain ? view.base + ".sustain" : ""
                if (timeId !== "") {
                    startRoot = Math.sqrt(Math.max(0, view.controller.value(timeId)))
                    view.controller.begin_gesture(timeId)
                    view.panel.touchId(timeId, "ENV " + view.number + " " + node.time.toUpperCase())
                }
                if (sustainId !== "") {
                    view.controller.begin_gesture(sustainId)
                    if (timeId === "")
                        view.panel.touchId(sustainId, "ENV " + view.number + " SUSTAIN")
                }
                return
            }
            const parts = view.widths()
            const side = view.theme.envelopePlotSide
            const f = (mouse.x - side) / (view.width - 2 * side)
            let at = 0
            let stage = 0
            for (; stage < parts.length - 1 && f > at + parts[stage]; ++stage)
                at += parts[stage]
            const names = ["attack_curve", "", "decay_curve", "", "release_curve"]
            curveId = names[stage] !== "" ? view.base + "." + names[stage] : ""
            if (curveId === "")
                return
            startY = mouse.y
            startValue = view.controller.value(curveId)
            view.controller.begin_gesture(curveId)
            view.panel.touchId(curveId, "ENV " + view.number + " " + names[stage].replace("_", " ").toUpperCase())
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return
            if (timeId !== "") {
                const root = Math.max(0, startRoot + (mouse.x - startX) / width * view.timeSpan)
                view.controller.set_value(timeId, root * root)
            }
            if (sustainId !== "") {
                const level = 1 - (mouse.y - view.levelTop) / view.levelSpan
                view.controller.set_value(sustainId, Math.max(0, Math.min(1, level)))
            }
            if (curveId !== "")
                view.controller.set_value(curveId, Math.max(-1, Math.min(1, startValue + (mouse.y - startY) / view.bendPixels)))
        }
        function finish(commit) {
            for (const id of [curveId, timeId, sustainId]) {
                if (id === "")
                    continue
                if (commit)
                    view.controller.end_gesture(id)
                else
                    view.controller.cancel_gesture(id)
            }
            curveId = ""
            timeId = ""
            sustainId = ""
        }
        onReleased: finish(true)
        onCanceled: finish(false)
    }
}
