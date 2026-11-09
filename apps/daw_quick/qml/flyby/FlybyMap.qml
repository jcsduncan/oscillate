pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format
import "FlybyPaths.js" as Paths

// Flyby's chart (ADR-0067): you at the centre, facing up the page, ears to
// the left and right; rings every 10 m; the path drawn from the parameters
// with its four points to drag (the ends filled, the handles hollow, each
// drag one undo step); the source where the engine hears it now, the line
// to you, the closest approach, and wavefronts that bunch up ahead of a
// moving source. The view fits the path, and holds still while you drag.
Item {
    id: map
    required property var controller
    required property FlybyTheme theme
    // controller.telemetry()'s map.
    property var live: ({ live: false })

    readonly property int revision: controller.revision
    // Heights and the chart scale read in whole metres.
    readonly property int wholeMetres: 0
    readonly property bool playing: live.live === true
    readonly property int kind: revision >= 0 ? controller.choice("flyby.path") : Paths.pass
    readonly property real radius: revision >= 0 ? controller.value("flyby.radius") : 0
    readonly property int direction: revision >= 0 ? controller.choice("flyby.direction") : Paths.clockwise
    readonly property real heightMetres: revision >= 0 ? controller.value("flyby.height") : 0
    readonly property bool speakers: revision >= 0 && controller.choice("flyby.listen") === 1
    readonly property real limit: controller.parameter("flyby.x1").maximum !== undefined
        ? controller.parameter("flyby.x1").maximum : Paths.maxView
    readonly property int pointCount: 4
    readonly property var points: {
        const out = []
        for (let i = 1; i <= pointCount; ++i)
            out.push(revision >= 0 ? [controller.value("flyby.x" + i), controller.value("flyby.y" + i)] : [0, 0])
        return out
    }
    // Metres from the centre to the chart's nearer edge; frozen mid-drag.
    property real view: Paths.reach(kind, points, radius)
    property bool dragging: false
    readonly property real fitted: Paths.reach(kind, points, radius)
    onFittedChanged: if (!dragging) view = fitted
    readonly property real pxPerMetre: Math.min(width, height) / 2 / view
    // Where the source is now: from the telemetry, else the path's start.
    readonly property var source: playing ? [Number(live.x), Number(live.y)]
        : (kind === Paths.orbit ? Paths.orbitAt(radius, direction, 0) : points[0])
    property var waves: []
    property real lastWave: 0
    readonly property int waveInterval: 220
    readonly property int waveCount: 24
    // Rings reach this far past the view, to the chart's corners.
    readonly property real ringReach: 1.5
    // Points drawn along the path, and where its arrows sit among them
    // (each pointing at the point this many samples on).
    readonly property int pathSamples: 200
    readonly property var arrowSamples: [50, 100, 150]
    readonly property int arrowLookahead: 2
    // An arrow's tip ahead of its point and its tail's half-width behind.
    readonly property real arrowTip: 6
    readonly property real arrowTail: 4
    // Wavefronts redraw at about 30 frames a second.
    readonly property int frameInterval: 33

    // The chart's scale for its frame: metres in front (+) and behind (-)
    // at half the view each way, the centre being you.
    readonly property var metreTicks: height > 0
        ? [view / 2, 0, -view / 2].map(m => ({ at: 0.5 + m * pxPerMetre / height,
                                               label: Format.signed_number(m, wholeMetres) })) : []

    objectName: "flybyMap"
    clip: true

    function toPx(p) {
        return [width / 2 + p[0] * pxPerMetre, height / 2 - p[1] * pxPerMetre]
    }
    function toMetres(x, y) {
        return [(x - width / 2) / pxPerMetre, (height / 2 - y) / pxPerMetre]
    }
    function clampMetres(v) {
        return Math.max(-limit, Math.min(limit, v))
    }

    onRevisionChanged: chart.requestPaint()
    onWidthChanged: chart.requestPaint()
    onHeightChanged: chart.requestPaint()
    onViewChanged: chart.requestPaint()
    onLiveChanged: {
        if (playing) {
            const now = Date.now()
            if (now - lastWave > waveInterval) {
                const next = waves.slice(-(waveCount - 1))
                next.push({ x: source[0], y: source[1], t: now })
                waves = next
                lastWave = now
            }
        }
        chart.requestPaint()
    }

    Rectangle {
        anchors.fill: parent
        color: map.theme.chart
    }

    Canvas {
        id: chart
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const g = getContext("2d")
            g.reset()
            const t = map.theme
            const W = width, H = height, cx = W / 2, cy = H / 2
            // Rings and the axes.
            g.strokeStyle = t.grid
            g.lineWidth = t.hairline
            for (let r = Paths.ringMetres; r <= map.view * map.ringReach; r += Paths.ringMetres) {
                g.beginPath()
                g.arc(cx, cy, r * map.pxPerMetre, 0, Math.PI * 2)
                g.stroke()
            }
            g.beginPath()
            g.moveTo(cx, 0); g.lineTo(cx, H); g.moveTo(0, cy); g.lineTo(W, cy)
            g.stroke()
            // Wavefronts: each left the source when it was there.
            const now = Date.now()
            g.strokeStyle = t.wave
            for (const w of map.waves) {
                const r = (now - w.t) / 1000 * Paths.speedOfSound / Paths.waveSlowdown
                const c = map.toPx([w.x, w.y])
                g.beginPath()
                g.arc(c[0], c[1], r * map.pxPerMetre, 0, Math.PI * 2)
                g.stroke()
            }
            // The path, with arrows along it.
            const line = Paths.sample(map.kind, map.points, map.radius, map.direction, map.pathSamples)
            g.strokeStyle = t.path
            g.lineWidth = t.pathWidth
            g.beginPath()
            for (let i = 0; i < line.length; ++i) {
                const q = map.toPx(line[i])
                if (i === 0) g.moveTo(q[0], q[1]); else g.lineTo(q[0], q[1])
            }
            g.stroke()
            g.fillStyle = t.path
            for (const f of map.arrowSamples) {
                const a = map.toPx(line[f]), b = map.toPx(line[f + map.arrowLookahead])
                g.save()
                g.translate(a[0], a[1])
                g.rotate(Math.atan2(b[1] - a[1], b[0] - a[0]))
                g.beginPath(); g.moveTo(map.arrowTip, 0); g.lineTo(-map.arrowTail, -map.arrowTail); g.lineTo(-map.arrowTail, map.arrowTail); g.closePath(); g.fill()
                g.restore()
            }
            // The Bézier handles' arms.
            if (map.kind === Paths.pass) {
                const P = map.points.map(p => map.toPx(p))
                g.strokeStyle = t.handleArm
                g.lineWidth = t.hairline
                g.beginPath()
                g.moveTo(P[0][0], P[0][1]); g.lineTo(P[1][0], P[1][1])
                g.moveTo(P[3][0], P[3][1]); g.lineTo(P[2][0], P[2][1])
                g.stroke()
            }
            // The closest approach, as a ring through it.
            if (map.playing && map.live.closest !== undefined) {
                g.setLineDash(t.closestDash)
                g.strokeStyle = t.closestRing
                g.beginPath()
                g.arc(cx, cy, Math.max(0, Number(map.live.closest)) * map.pxPerMetre, 0, Math.PI * 2)
                g.stroke()
                g.setLineDash([])
            }
            // The line from you to the source.
            const s = map.toPx(map.source)
            g.strokeStyle = t.sourceLine
            g.setLineDash(t.sourceDash)
            g.beginPath(); g.moveTo(cx, cy); g.lineTo(s[0], s[1]); g.stroke()
            g.setLineDash([])
        }
    }
    // Wavefronts keep growing between telemetry frames.
    Timer {
        interval: map.frameInterval
        repeat: true
        running: map.visible && map.waves.length > 0
        onTriggered: chart.requestPaint()
    }

    // You: a head facing up, the ears lit by what each hears.
    Item {
        id: listener
        objectName: "flybyListener"
        x: map.width / 2
        y: map.height / 2
        // The level an ear reads when nothing plays, and the span of dB
        // over which an ear goes from dim to fully lit.
        readonly property real quietDb: -60
        readonly property real litRangeDb: 30
        // The head's drawing: the nose above it, the ears at its sides and
        // their L and R labels beyond.
        readonly property real noseX: -5
        readonly property real noseY: -26
        readonly property real earGap: 13
        readonly property real earWidth: 4
        readonly property real earHeight: 8
        readonly property real earRadius: 1
        readonly property real leftLabelX: -32
        readonly property real rightLabelX: 24
        readonly property real labelY: -7
        readonly property real leftDb: map.playing ? Number(map.live.level_left_db) : quietDb
        readonly property real rightDb: map.playing ? Number(map.live.level_right_db) : quietDb
        function lit(db) { return Math.max(map.theme.earQuietOpacity, Math.min(1, 1 + db / litRangeDb)) }
        Rectangle {
            x: -map.theme.headSize / 2; y: -map.theme.headSize / 2
            width: map.theme.headSize; height: map.theme.headSize; radius: map.theme.headSize / 2
            color: map.theme.listenerFill
            border.color: map.theme.listener
            border.width: map.theme.headStroke
        }
        Text {
            x: listener.noseX; y: listener.noseY
            text: "▲"
            color: map.theme.listener
            font.pixelSize: map.theme.typeSmall
        }
        Rectangle {
            objectName: "flybyEarLeft"
            x: -listener.earGap - listener.earWidth; y: -listener.earHeight / 2
            width: listener.earWidth; height: listener.earHeight; radius: listener.earRadius
            color: map.theme.listener
            opacity: listener.lit(listener.leftDb)
        }
        Rectangle {
            objectName: "flybyEarRight"
            x: listener.earGap; y: -listener.earHeight / 2
            width: listener.earWidth; height: listener.earHeight; radius: listener.earRadius
            color: map.theme.listener
            opacity: listener.lit(listener.rightDb)
        }
        Text { x: listener.leftLabelX; y: listener.labelY; text: "L"; color: map.theme.inkDim; font.family: map.theme.mono; font.pixelSize: map.theme.typeBody }
        Text { x: listener.rightLabelX; y: listener.labelY; text: "R"; color: map.theme.inkDim; font.family: map.theme.mono; font.pixelSize: map.theme.typeBody }
    }

    // The source where the engine hears it.
    Rectangle {
        id: dot
        objectName: "flybySource"
        readonly property var at: map.toPx(map.source)
        x: at[0] - width / 2
        y: at[1] - height / 2
        // The height label's lift above the dot.
        readonly property real heightLabelY: -12
        width: map.theme.sourceSize
        height: map.theme.sourceSize
        radius: map.theme.sourceSize / 2
        color: map.theme.path
        Rectangle {
            anchors.centerIn: parent
            width: map.theme.sourceCore; height: map.theme.sourceCore; radius: map.theme.sourceCore / 2
            color: map.theme.chart
        }
        Text {
            objectName: "flybyHeight"
            visible: map.heightMetres > 0
            x: parent.width + map.theme.spaceSm
            y: dot.heightLabelY
            text: "↑ " + Format.quantity(map.heightMetres, "m", map.wholeMetres) + " up"
            color: map.theme.inkDim
            font.family: map.theme.mono
            font.pixelSize: map.theme.typeBody
        }
    }

    // The four points to drag (a Pass only).
    Repeater {
        model: map.kind === Paths.pass ? map.pointCount : 0
        delegate: Rectangle {
            id: handle
            required property int index
            readonly property bool end: index === 0 || index === map.pointCount - 1
            readonly property string xId: "flyby.x" + (index + 1)
            readonly property string yId: "flyby.y" + (index + 1)
            readonly property var at: map.toPx(map.points[index])
            objectName: "flybyPoint:" + (index + 1)
            x: at[0] - width / 2
            y: at[1] - height / 2
            width: end ? map.theme.pointEndSize : map.theme.pointHandleSize
            height: width
            radius: width / 2
            color: end ? map.theme.ink : map.theme.handleFill
            border.color: map.theme.ink
            border.width: map.theme.pointStroke
            // Keys: Left/Right move the point across, Up/Down along the
            // chart (Shift: fine); Delete puts it back.
            activeFocusOnTab: map.controller.editable
            Accessible.role: Accessible.Slider
            Accessible.name: map.controller.parameter(xId).name + " and Y"
            Accessible.description: "Arrow keys move the point"
            Keys.onPressed: event => {
                if (!map.controller.editable)
                    return
                const fine = (event.modifiers & Qt.ShiftModifier) !== 0
                if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
                    map.controller.step(handle.xId, event.key === Qt.Key_Right ? 1 : -1, fine)
                    map.theme.lastTouched = handle.xId
                } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                    map.controller.step(handle.yId, event.key === Qt.Key_Up ? 1 : -1, fine)
                    map.theme.lastTouched = handle.yId
                } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                    map.controller.reset_to_default(handle.xId)
                    map.controller.reset_to_default(handle.yId)
                    map.theme.lastTouched = handle.xId
                } else {
                    return
                }
                event.accepted = true
            }
            // The focus ring round the point.
            Rectangle {
                visible: handle.activeFocus
                anchors.centerIn: parent
                width: parent.width + 2 * map.theme.pointFocusGap
                height: width
                radius: width / 2
                color: "transparent"
                border.color: map.theme.accent
                border.width: map.theme.pointStroke
            }
            MouseArea {
                anchors.fill: parent
                anchors.margins: -map.theme.pointGrabMargin
                enabled: map.controller.editable
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                preventStealing: true
                onPressed: {
                    handle.forceActiveFocus()
                    map.dragging = true
                    map.controller.begin_gesture(handle.xId)
                    map.theme.lastTouched = handle.xId
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(map, mouse.x, mouse.y)
                    const m = map.toMetres(p.x, p.y)
                    map.controller.set_value(handle.xId, map.clampMetres(m[0]))
                    map.controller.set_value(handle.yId, map.clampMetres(m[1]))
                }
                onReleased: {
                    map.controller.end_gesture(handle.xId)
                    map.dragging = false
                    map.view = map.fitted
                }
                onCanceled: {
                    map.controller.end_gesture(handle.xId)
                    map.dragging = false
                }
            }
        }
    }

    Text {
        x: map.theme.spaceLg
        y: map.theme.chartLabelTop
        text: "FRONT ↑ · YOU FACE UP THE CHART\nRINGS EVERY " + Format.quantity(Paths.ringMetres, "m", map.wholeMetres)
        color: map.theme.inkFaint
        font.family: map.theme.mono
        font.pixelSize: map.theme.typeSmall
        lineHeight: map.theme.legendLineHeight
    }
}
