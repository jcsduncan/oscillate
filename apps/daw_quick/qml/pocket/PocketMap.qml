pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The masking map: Pocket's 24 bands on a log frequency axis (30 Hz to
// 8 kHz) and a dBFS scale. The kick is amber, the bass cyan, each with a
// slow-decaying held peak line; where both sound inside the focus the
// overlap is shaded red, and the cut Pocket makes hangs from the bass curve
// in green, the bass after Pocket dashed beneath it. The focus range is
// lit, outside it dimmed, its edges draggable handles that never cross; the
// mono sub's region is hatched. Hovering reads one band out.
Item {
    id: map
    required property var controller
    required property PocketTheme theme
    property bool live: false
    // Per band, in dBFS (kick, bass) and dB of cut (>= 0).
    property var kick: []
    property var bass: []
    property var cut: []
    property var heldKick: []
    property var heldBass: []

    readonly property int revision: controller.revision
    // Levels at or below this read as silence; bare levels have one decimal.
    readonly property real silentDb: -99
    readonly property int levelDecimals: 1
    readonly property real hertzPerKilohertz: 1000
    readonly property real focusLow: revision >= 0 ? controller.value("pocket.focus_low") : 30
    readonly property real focusHigh: revision >= 0 ? controller.value("pocket.focus_high") : 250
    readonly property bool monoOn: revision >= 0 && controller.choice("pocket.mono_sub") === 1
    readonly property real monoHz: revision >= 0 ? controller.value("pocket.mono_freq") : 120
    // The hovered band, or -1.
    property int hovered: -1

    // Plot area.
    readonly property real px: theme.plotLeft
    readonly property real py: theme.mapPlotTop
    readonly property real pw: width - px - theme.plotRight
    readonly property real ph: height - py - theme.mapPlotBottom
    readonly property real lowHz: 25
    readonly property real highHz: 10000
    readonly property real topDb: 0
    readonly property real bottomDb: -78
    // Focus edges keep a third of an octave apart.
    readonly property real spacing: 1.26
    // A cut bar's width as a share of the band step, hovered or not.
    readonly property real cutBarShare: 0.52
    readonly property real cutBarHoveredShare: 0.7

    objectName: "pocketMap"
    implicitWidth: theme.mapWidth
    implicitHeight: theme.mapHeight

    function bandHz(b) {
        return 30 * Math.pow(8000 / 30, b / 23)
    }
    function xOf(hz) {
        return px + pw * Math.log(Math.max(lowHz, Math.min(highHz, hz)) / lowHz) / Math.log(highHz / lowHz)
    }
    function hzOf(x) {
        return lowHz * Math.pow(highHz / lowHz, Math.max(0, Math.min(1, (x - px) / pw)))
    }
    function yOf(db) {
        return py + ph * (topDb - Math.max(bottomDb, Math.min(topDb, db))) / (topDb - bottomDb)
    }
    function hzText(hz) {
        return Format.hertz(hz)
    }
    function dbText(db) {
        return Format.signed_number(db <= silentDb ? -Infinity : db, levelDecimals)
    }
    // Where a focus edge may go: its own range, a third of an octave from
    // the other edge.
    function limitFocus(id, hz) {
        const info = controller.parameter(id)
        let lo = info.minimum
        let hi = info.maximum
        if (id === "pocket.focus_low")
            hi = Math.min(hi, focusHigh / spacing)
        else
            lo = Math.max(lo, focusLow * spacing)
        return Math.max(lo, Math.min(hi, hz))
    }
    function moveFocus(id, hz) {
        controller.set_value(id, limitFocus(id, hz))
        theme.lastTouched = id
    }
    // The nearest band to an x position.
    function bandAt(x) {
        const hz = hzOf(x)
        const b = Math.round(23 * Math.log(hz / 30) / Math.log(8000 / 30))
        return Math.max(0, Math.min(23, b))
    }

    onKickChanged: plot.requestPaint()
    onBassChanged: plot.requestPaint()
    onCutChanged: plot.requestPaint()
    onHeldKickChanged: plot.requestPaint()
    onHeldBassChanged: plot.requestPaint()
    onFocusLowChanged: plot.requestPaint()
    onFocusHighChanged: plot.requestPaint()
    onMonoOnChanged: plot.requestPaint()
    onMonoHzChanged: plot.requestPaint()
    onHoveredChanged: plot.requestPaint()
    onWidthChanged: plot.requestPaint()
    onHeightChanged: plot.requestPaint()

    Canvas {
        id: plot
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        // A smooth curve through the band points.
        function curve(ctx, values, offset) {
            const n = values.length
            if (n === 0)
                return
            const point = i => [map.xOf(map.bandHz(i)), map.yOf(values[i] - (offset ? offset[i] : 0))]
            let [x0, y0] = point(0)
            ctx.moveTo(x0, y0)
            for (let i = 1; i < n; ++i) {
                const [x1, y1] = point(i)
                ctx.quadraticCurveTo(x0, y0, (x0 + x1) / 2, (y0 + y1) / 2)
                x0 = x1
                y0 = y1
            }
            ctx.lineTo(x0, y0)
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = map.theme
            const left = map.px
            const right = map.px + map.pw
            const top = map.py
            const bottom = map.py + map.ph
            // The well.
            ctx.fillStyle = t.well
            ctx.fillRect(left, top, map.pw, map.ph)
            // Grid: octaves of frequency, 12 dB of level.
            ctx.lineWidth = t.hairline
            ctx.font = t.typeMicro + "px '" + t.mono + "'"
            ctx.textBaseline = "top"
            ctx.textAlign = "center"
            for (const hz of [30, 50, 100, 200, 500, 1000, 2000, 5000]) {
                const x = Math.round(map.xOf(hz)) + 0.5
                const strong = hz === 100 || hz === 1000
                ctx.strokeStyle = strong ? t.gridStrong : t.grid
                ctx.beginPath()
                ctx.moveTo(x, top)
                ctx.lineTo(x, bottom)
                ctx.stroke()
                ctx.fillStyle = strong ? t.inkDim : t.inkFaint
                ctx.fillText(hz >= map.hertzPerKilohertz ? Format.number(hz / map.hertzPerKilohertz, 0) + "k"
                                                         : Format.number(hz, 0), x, bottom + t.axisLabelGap)
            }
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (let db = 0; db >= -72; db -= 12) {
                const y = Math.round(map.yOf(db)) + 0.5
                ctx.strokeStyle = t.grid
                ctx.beginPath()
                ctx.moveTo(left, y)
                ctx.lineTo(right, y)
                ctx.stroke()
                ctx.fillStyle = t.inkFaint
                ctx.fillText(Format.number(db, 0), left - t.axisLabelGap, y)
            }

            const n = Math.min(map.kick.length, map.bass.length)
            const focusL = map.xOf(map.focusLow)
            const focusR = map.xOf(map.focusHigh)
            ctx.save()
            ctx.beginPath()
            ctx.rect(left, top, map.pw, map.ph)
            ctx.clip()
            if (n > 0) {
                // Masking: under both curves, inside the focus.
                const shared = []
                for (let i = 0; i < n; ++i)
                    shared.push(Math.min(map.kick[i], map.bass[i]))
                ctx.save()
                ctx.beginPath()
                ctx.rect(focusL, top, focusR - focusL, map.ph)
                ctx.clip()
                ctx.beginPath()
                curve(ctx, shared)
                ctx.lineTo(map.xOf(map.bandHz(n - 1)), bottom)
                ctx.lineTo(map.xOf(map.bandHz(0)), bottom)
                ctx.closePath()
                const red = ctx.createLinearGradient(0, top, 0, bottom)
                red.addColorStop(0, Qt.rgba(t.masked.r, t.masked.g, t.masked.b, t.maskTopAlpha))
                red.addColorStop(1, Qt.rgba(t.masked.r, t.masked.g, t.masked.b, t.maskBottomAlpha))
                ctx.fillStyle = red
                ctx.fill()
                ctx.restore()

                // The kick: a warm glow under its line.
                ctx.beginPath()
                curve(ctx, map.kick)
                ctx.lineTo(map.xOf(map.bandHz(n - 1)), bottom)
                ctx.lineTo(map.xOf(map.bandHz(0)), bottom)
                ctx.closePath()
                const amber = ctx.createLinearGradient(0, top, 0, bottom)
                amber.addColorStop(0, Qt.rgba(t.kick.r, t.kick.g, t.kick.b, t.kickGlowTopAlpha))
                amber.addColorStop(1, Qt.rgba(t.kick.r, t.kick.g, t.kick.b, t.kickGlowBottomAlpha))
                ctx.fillStyle = amber
                ctx.fill()

                // The cut: green bars hanging from the bass, the bass after
                // Pocket dashed beneath.
                const step = map.xOf(map.bandHz(1)) - map.xOf(map.bandHz(0))
                for (let i = 0; i < n && i < map.cut.length; ++i) {
                    const c = map.cut[i]
                    if (c < 0.05)
                        continue
                    const x = map.xOf(map.bandHz(i))
                    const y0 = map.yOf(map.bass[i])
                    const y1 = map.yOf(map.bass[i] - c)
                    const w = step * (i === map.hovered ? map.cutBarHoveredShare : map.cutBarShare)
                    const green = ctx.createLinearGradient(0, y0, 0, y1)
                    green.addColorStop(0, Qt.rgba(t.cleared.r, t.cleared.g, t.cleared.b, t.cutTopAlpha))
                    green.addColorStop(1, Qt.rgba(t.cleared.r, t.cleared.g, t.cleared.b, t.cutBottomAlpha))
                    ctx.fillStyle = green
                    ctx.fillRect(x - w / 2, y0, w, Math.max(1, y1 - y0))
                }
                if (map.cut.length >= n) {
                    ctx.beginPath()
                    curve(ctx, map.bass, map.cut)
                    ctx.setLineDash(t.afterDash)
                    ctx.lineWidth = t.afterStroke
                    ctx.strokeStyle = Qt.rgba(t.bass.r, t.bass.g, t.bass.b, t.afterAlpha)
                    ctx.stroke()
                    ctx.setLineDash([])
                }

                // The bass and the kick lines.
                ctx.lineJoin = "round"
                ctx.beginPath()
                curve(ctx, map.bass)
                ctx.lineWidth = t.curveStroke
                ctx.strokeStyle = t.bass
                ctx.stroke()
                ctx.beginPath()
                curve(ctx, map.kick)
                ctx.lineWidth = t.curveStroke
                ctx.strokeStyle = t.kick
                ctx.stroke()

                // Held peaks: fine bright lines that fall slowly.
                for (const [held, colour] of [[map.heldBass, t.bass], [map.heldKick, t.kick]]) {
                    if (held.length < n)
                        continue
                    ctx.beginPath()
                    curve(ctx, held)
                    ctx.lineWidth = t.hairline
                    ctx.strokeStyle = Qt.rgba(colour.r, colour.g, colour.b, t.heldAlpha)
                    ctx.stroke()
                }
            }

            // Mono sub: hatched below its frequency.
            if (map.monoOn) {
                const x = map.xOf(map.monoHz)
                ctx.save()
                ctx.beginPath()
                ctx.rect(left, top, x - left, map.ph)
                ctx.clip()
                ctx.strokeStyle = Qt.rgba(t.bass.r, t.bass.g, t.bass.b, t.hatchAlpha)
                ctx.lineWidth = t.hairline
                for (let d = -map.ph; d < x - left; d += t.hatchPitch) {
                    ctx.beginPath()
                    ctx.moveTo(left + d, bottom)
                    ctx.lineTo(left + d + map.ph, top)
                    ctx.stroke()
                }
                ctx.restore()
                ctx.strokeStyle = Qt.rgba(t.bass.r, t.bass.g, t.bass.b, t.monoEdgeAlpha)
                ctx.setLineDash(t.monoDash)
                ctx.beginPath()
                ctx.moveTo(Math.round(x) + 0.5, top)
                ctx.lineTo(Math.round(x) + 0.5, bottom)
                ctx.stroke()
                ctx.setLineDash([])
            }

            // Outside the focus, dimmed.
            ctx.fillStyle = t.focusDim
            ctx.fillRect(left, top, focusL - left, map.ph)
            ctx.fillRect(focusR, top, right - focusR, map.ph)
            ctx.restore()

            ctx.strokeStyle = t.wellEdge
            ctx.lineWidth = t.hairline
            ctx.strokeRect(left + 0.5, top + 0.5, map.pw - 1, map.ph - 1)
        }
    }

    // The mono sub's label.
    Text {
        objectName: "pocketMonoShade"
        visible: map.monoOn
        x: map.px + map.theme.monoLabelX
        y: map.py + map.ph - map.theme.monoLabelRise
        text: "MONO SUB < " + map.hzText(map.monoHz)
        color: Qt.rgba(map.theme.bass.r, map.theme.bass.g, map.theme.bass.b, map.theme.monoLabelAlpha)
        font.pixelSize: map.theme.typeMicro
        font.bold: true
        font.letterSpacing: map.theme.keySpacing
    }

    // Hover: the nearest band read out.
    MouseArea {
        id: hover
        objectName: "pocketHover"
        x: map.px
        y: map.py
        width: map.pw
        height: map.ph
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: mouse => { map.hovered = map.bandAt(mouse.x + map.px) }
        onExited: map.hovered = -1
    }
    Rectangle {
        id: readout
        objectName: "pocketBandReadout"
        readonly property int band: map.hovered
        readonly property bool shown: band >= 0 && band < map.kick.length && band < map.bass.length
        property string text: shown
            ? map.hzText(map.bandHz(band)) + "   KICK " + map.dbText(map.kick[band])
              + "   BASS " + map.dbText(map.bass[band])
              + "   CUT " + (band < map.cut.length && map.cut[band] >= 0.05 ? Format.decibels(-map.cut[band]) : "—")
            : ""
        visible: shown
        z: 5
        x: Math.max(map.px + map.theme.bandReadoutEdge,
                    Math.min(map.px + map.pw - width - map.theme.bandReadoutEdge,
                             map.xOf(map.bandHz(Math.max(0, band))) - width / 2))
        y: map.py + map.theme.bandReadoutY
        width: readoutText.implicitWidth + map.theme.bandReadoutPad
        height: map.theme.bandReadoutHeight
        radius: map.theme.bandReadoutRadius
        color: Qt.rgba(map.theme.well.r, map.theme.well.g, map.theme.well.b, map.theme.bandReadoutAlpha)
        border.color: map.theme.plateHi
        Text {
            id: readoutText
            anchors.centerIn: parent
            text: readout.text
            color: map.theme.ink
            font.family: map.theme.mono
            font.pixelSize: map.theme.typeCaption
        }
    }

    // The focus edges.
    Repeater {
        model: ["pocket.focus_low", "pocket.focus_high"]
        delegate: Item {
            id: handle
            required property string modelData
            required property int index
            readonly property string parameterId: modelData
            readonly property real hz: index === 0 ? map.focusLow : map.focusHigh
            objectName: "pocketFocus:" + parameterId
            x: map.xOf(hz) - width / 2
            y: 0
            width: map.theme.handleWidth
            height: map.py + map.ph
            activeFocusOnTab: map.controller.editable
            Accessible.role: Accessible.Slider
            Accessible.name: index === 0 ? "Focus low" : "Focus high"
            Accessible.description: map.hzText(hz)
            Keys.onPressed: event => {
                const factor = (event.modifiers & Qt.ShiftModifier) !== 0 ? 1.01 : 1.06
                if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
                    map.moveFocus(handle.parameterId, handle.hz * factor)
                else if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
                    map.moveFocus(handle.parameterId, handle.hz / factor)
                else
                    return
                event.accepted = true
            }
            Rectangle {
                x: handle.width / 2 - map.theme.hairline / 2
                y: map.py
                width: map.theme.hairline
                height: map.ph
                color: handleArea.pressed || handle.activeFocus ? map.theme.glint
                                                                 : Qt.rgba(map.theme.kick.r, map.theme.kick.g, map.theme.kick.b, map.theme.handleLineAlpha)
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: map.py - map.theme.gripRise
                width: gripText.implicitWidth + map.theme.gripPad
                height: map.theme.gripHeight
                radius: map.theme.radiusSm
                color: handleArea.pressed || handle.activeFocus ? map.theme.kick : map.theme.plateHi
                border.color: map.theme.kickSoft
                Text {
                    id: gripText
                    anchors.centerIn: parent
                    text: (handle.index === 0 ? "◂ " : "") + map.hzText(handle.hz) + (handle.index === 1 ? " ▸" : "")
                    color: handleArea.pressed || handle.activeFocus ? map.theme.caseColor : map.theme.glacier
                    font.family: map.theme.mono
                    font.pixelSize: map.theme.typeCaption
                }
            }
            MouseArea {
                id: handleArea
                x: handle.width / 2 - map.theme.handleGrabWidth / 2
                y: map.py - map.theme.gripRise
                width: map.theme.handleGrabWidth
                height: map.ph + map.theme.gripRise
                enabled: map.controller.editable
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                onPressed: {
                    handle.forceActiveFocus()
                    map.controller.begin_gesture(handle.parameterId)
                    map.theme.lastTouched = handle.parameterId
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        map.moveFocus(handle.parameterId, map.hzOf(mapToItem(map, mouse.x, mouse.y).x))
                }
                onReleased: map.controller.end_gesture(handle.parameterId)
                onCanceled: map.controller.cancel_gesture(handle.parameterId)
                onDoubleClicked: {
                    map.controller.reset_to_default(handle.parameterId)
                    map.theme.lastTouched = handle.parameterId
                }
            }
        }
    }
}
