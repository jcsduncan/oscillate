pragma ComponentBehavior: Bound
import QtQuick

// The phosphor screen: 480 min/max columns for the track (green) and the
// sidechain (cyan) overlaid, swept left to right with the beam at the
// write column; the sweep behind the beam fades as it ages. Behind them a
// graticule: amplitude divisions, and the grid of the window, bars and
// beats when synced or time divisions when free.
Item {
    id: screen
    required property ScopeTheme theme
    // (min, max) per column, 2 * columns values each; empty for none.
    property var track: []
    property var key: []
    property int writeColumn: 0
    property bool live: false
    property bool overlay: true
    // The grid: lines as {at: 0..1, weight: 0 fine, 1 beat, 2 bar} and
    // labels as {at, text}.
    property var lines: []
    property var labels: []
    readonly property int columns: 480

    objectName: "scopeScreen"

    // The glass.
    Rectangle {
        id: glass
        anchors.fill: parent
        radius: screen.theme.radiusLg
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(screen.theme.screen, screen.theme.glassLift) }
            GradientStop { position: screen.theme.glassMidStop; color: screen.theme.screen }
            GradientStop { position: 1; color: Qt.darker(screen.theme.screen, screen.theme.glassShade) }
        }
        border.color: screen.theme.screenEdge
    }

    readonly property real padX: theme.screenPadX
    readonly property real padTop: theme.screenPadTop
    readonly property real padBottom: theme.screenPadBottom
    readonly property real plotX: padX
    readonly property real plotY: padTop
    readonly property real plotW: width - 2 * padX
    readonly property real plotH: height - padTop - padBottom

    // The graticule: amplitude divisions, and ticks along the centre line.
    readonly property int divisions: 8
    readonly property int centreTicks: 40

    // Amplitude graticule: eight divisions, the centre line stronger.
    Repeater {
        model: screen.divisions + 1
        delegate: Rectangle {
            required property int index
            x: screen.plotX
            y: Math.round(screen.plotY + screen.plotH * index / screen.divisions)
            width: screen.plotW
            height: screen.theme.hairline
            color: index === screen.divisions / 2 ? screen.theme.gridStrong : screen.theme.grid
        }
    }
    // The window's grid.
    Repeater {
        model: screen.lines
        delegate: Rectangle {
            required property var modelData
            x: Math.round(screen.plotX + screen.plotW * modelData.at)
            y: screen.plotY
            width: modelData.weight >= 2 ? screen.theme.strokeBold : screen.theme.hairline
            height: screen.plotH
            color: modelData.weight >= 2 ? Qt.rgba(screen.theme.bar.r, screen.theme.bar.g, screen.theme.bar.b, screen.theme.barLineAlpha)
                 : modelData.weight === 1 ? screen.theme.gridStrong : screen.theme.grid
        }
    }
    // Centre-line ticks, as on a real graticule.
    Repeater {
        model: screen.centreTicks + 1
        delegate: Rectangle {
            required property int index
            x: Math.round(screen.plotX + screen.plotW * index / screen.centreTicks)
            y: Math.round(screen.plotY + screen.plotH / 2) - screen.theme.centreTickRise
            width: screen.theme.hairline
            height: screen.theme.centreTickLength
            color: screen.theme.gridStrong
        }
    }
    Repeater {
        model: screen.labels
        delegate: Text {
            required property var modelData
            x: Math.max(screen.plotX, Math.min(screen.plotX + screen.plotW - width,
                screen.plotX + screen.plotW * modelData.at - width / 2))
            y: screen.plotY + screen.plotH + screen.theme.screenLabelGap
            text: modelData.text
            color: screen.theme.graticuleInk
            font.family: screen.theme.mono
            font.pixelSize: screen.theme.typeSmall
        }
    }

    Canvas {
        id: traces
        objectName: "scopeTraces"
        x: screen.plotX
        y: screen.plotY
        width: screen.plotW
        height: screen.plotH
        renderStrategy: Canvas.Cooperative
        function envelope(ctx, data, from, to) {
            const n = screen.columns
            const half = height / 2
            const dx = width / n
            ctx.beginPath()
            for (let c = from; c <= to; ++c) {
                const top = half - Math.max(-1, Math.min(1, data[2 * c + 1])) * half
                if (c === from)
                    ctx.moveTo(c * dx + dx / 2, top)
                else
                    ctx.lineTo(c * dx + dx / 2, top)
            }
            for (let c = to; c >= from; --c) {
                const bottom = half - Math.max(-1, Math.min(1, data[2 * c])) * half
                ctx.lineTo(c * dx + dx / 2, bottom)
            }
            ctx.closePath()
        }
        function drawTrace(ctx, data, colour, glow, alphaScale) {
            if (data.length < 2 * screen.columns)
                return
            // Twenty-column chunks, brightest just behind the beam.
            const chunk = 20
            for (let from = 0; from < screen.columns; from += chunk) {
                const to = Math.min(screen.columns - 1, from + chunk)
                const centre = (from + to) / 2
                const age = ((screen.writeColumn - centre) % screen.columns + screen.columns) % screen.columns
                const fresh = 1 - age / screen.columns
                const t = screen.theme
                const a = alphaScale * (t.traceFreshFloor + t.traceFreshGain * fresh)
                envelope(ctx, data, from, to)
                ctx.fillStyle = Qt.rgba(glow.r, glow.g, glow.b, t.traceFillAlpha * a)
                ctx.fill()
                ctx.lineWidth = t.traceGlowWidth
                ctx.strokeStyle = Qt.rgba(glow.r, glow.g, glow.b, t.traceGlowAlpha * a)
                ctx.stroke()
                ctx.lineWidth = t.traceLineWidth
                ctx.strokeStyle = Qt.rgba(colour.r, colour.g, colour.b, t.traceLineAlpha * a)
                ctx.stroke()
            }
        }
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!screen.live)
                return
            ctx.lineJoin = "round"
            if (screen.overlay)
                drawTrace(ctx, screen.key, screen.theme.key, screen.theme.keyGlow, screen.theme.keyTraceAlpha)
            drawTrace(ctx, screen.track, screen.theme.trace, screen.theme.traceGlow, 1)
        }
        Connections {
            target: screen
            function onTrackChanged() { traces.requestPaint() }
            function onKeyChanged() { traces.requestPaint() }
            function onOverlayChanged() { traces.requestPaint() }
            function onLiveChanged() { traces.requestPaint() }
        }
    }

    // The beam: the column being written, with its bloom, and the gap
    // just ahead of it where the old sweep is erased.
    Item {
        objectName: "scopeBeam"
        visible: screen.live
        x: screen.plotX + screen.plotW * (screen.writeColumn + 0.5) / screen.columns
        y: screen.plotY
        height: screen.plotH
        Rectangle {
            x: screen.theme.eraseGap
            width: Math.min(screen.theme.eraseWidth, screen.plotX + screen.plotW - parent.x - screen.theme.eraseGap)
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Qt.rgba(screen.theme.screen.r, screen.theme.screen.g, screen.theme.screen.b, screen.theme.eraseAlpha) }
                GradientStop { position: 1; color: Qt.rgba(screen.theme.screen.r, screen.theme.screen.g, screen.theme.screen.b, 0) }
            }
        }
        Rectangle {
            x: -width / 2
            width: screen.theme.bloomWidth
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Qt.rgba(screen.theme.beam.r, screen.theme.beam.g, screen.theme.beam.b, 0) }
                GradientStop { position: screen.theme.bloomMidStop; color: Qt.rgba(screen.theme.beam.r, screen.theme.beam.g, screen.theme.beam.b, screen.theme.bloomAlpha) }
                GradientStop { position: 1; color: Qt.rgba(screen.theme.beam.r, screen.theme.beam.g, screen.theme.beam.b, 0) }
            }
        }
        Rectangle {
            x: -width / 2
            width: screen.theme.hairline
            height: parent.height
            color: Qt.rgba(screen.theme.beam.r, screen.theme.beam.g, screen.theme.beam.b, screen.theme.beamAlpha)
        }
    }

    Text {
        visible: !screen.live
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -screen.padBottom / 2 + screen.padTop / 2
        text: "NO SIGNAL"
        color: screen.theme.graticuleInk
        font.family: screen.theme.sans
        font.pixelSize: screen.theme.typeEmphasis
        font.bold: true
        font.letterSpacing: screen.theme.noSignalSpacing
    }
}
