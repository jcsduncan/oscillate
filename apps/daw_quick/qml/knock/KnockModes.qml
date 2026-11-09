pragma ComponentBehavior: Bound
import QtQuick

// The selected pad's ringing modes on a log frequency axis: one bar per
// designed mode (knock::design_pad through the panel model), coloured by
// what rings it (resonator, coupled, body). From the moment the pad is hit
// each bar decays at its own T60; at rest a faint outline shows the modes
// the next hit will ring. The panel frames it (Kit.PlotFrame), labelling
// the frequency axis from `frequencyTicks`.
Canvas {
    id: view
    required property KnockTheme theme
    // [{hz, level, t60, part}] for the selected pad.
    property var modes: []
    // The view's range, Hz (the panel model's).
    property real minHz: 30
    property real maxHz: 16000
    // When the selected pad was last hit (Date.now() ms), or -1.
    property real hitAt: -1
    // True while a hit's modes still ring; the view repaints every frame.
    property bool ringing: false
    // Bars drawn by the last paint: live (ringing) and resting.
    property int liveCount: 0
    property int restingCount: 0

    // A T60 is the time to fall 60 dB: three decades of amplitude.
    readonly property real decadesPerT60: 3
    // Below this level (relative to the strongest mode) a bar is gone.
    readonly property real floorLevel: 0.002
    // Bars are drawn at level^shape so quiet modes stay visible.
    readonly property real heightShape: 0.45
    readonly property real restingScale: 0.5
    readonly property real restingAlpha: theme.modeRestingAlpha
    // Repaint once a frame while the modes ring.
    readonly property int frameMs: 16
    // The plot's top margin, under the title and the key.
    readonly property real plotTop: 30
    // A ringing mode's halo: its radius at silence and its growth at full level.
    readonly property real haloRadius: 4
    readonly property real haloGrowth: 6
    readonly property var gridHz: [50, 100, 200, 500, 1000, 2000, 5000, 10000]
    // The gridlines as PlotFrame ticks, from this view's own mapping.
    readonly property var frequencyTicks: width > 0 ? gridHz.map(hz => ({
        at: xOf(hz) / width, label: hz >= 1000 ? (hz / 1000) + "k" : String(hz) })) : []

    objectName: "knockModes"
    renderStrategy: Canvas.Cooperative
    Accessible.role: Accessible.Graphic
    Accessible.name: "Ringing modes of the selected pad"

    onModesChanged: requestPaint()
    onHitAtChanged: {
        ringing = hitAt >= 0
        requestPaint()
    }
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    Timer {
        interval: view.frameMs
        repeat: true
        running: view.ringing && view.visible
        onTriggered: view.requestPaint()
    }

    function xOf(hz) {
        const span = Math.log(maxHz / minHz)
        return theme.modesInset + (width - 2 * theme.modesInset) * Math.max(0, Math.min(1, Math.log(hz / minHz) / span))
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const top = plotTop
        const base = height
        ctx.fillStyle = theme.well
        ctx.fillRect(0, 0, width, height)
        ctx.strokeStyle = theme.line
        ctx.lineWidth = theme.hairline
        for (const hz of gridHz) {
            const x = Math.round(xOf(hz)) + theme.hairline / 2
            ctx.beginPath()
            ctx.moveTo(x, top)
            ctx.lineTo(x, base)
            ctx.stroke()
        }
        const age = hitAt >= 0 ? (Date.now() - hitAt) / 1000 : -1
        let live = 0
        let resting = 0
        if (age >= 0) {
            for (const mode of modes) {
                if (mode.hz < minHz || mode.hz > maxHz || !(mode.t60 > 0))
                    continue
                const level = mode.level * Math.pow(10, -decadesPerT60 * age / mode.t60)
                if (level < floorLevel)
                    continue
                const h = (base - top) * Math.min(1, Math.pow(level, heightShape))
                const x = xOf(mode.hz)
                ctx.fillStyle = theme.partColor(mode.part)
                ctx.globalAlpha = theme.modeBarAlpha
                ctx.fillRect(x - theme.modeBarWidth / 2, base - h, theme.modeBarWidth, h)
                ctx.globalAlpha = theme.modeHaloAlpha
                ctx.beginPath()
                ctx.arc(x, base - h, haloRadius + haloGrowth * level, 0, Math.PI * 2)
                ctx.fill()
                live += 1
            }
        }
        if (live === 0) {
            // At rest: the outline of what the next hit rings.
            ctx.globalAlpha = restingAlpha
            for (const mode of modes) {
                if (mode.hz < minHz || mode.hz > maxHz)
                    continue
                const h = (base - top) * restingScale * Math.pow(Math.max(0, mode.level), heightShape)
                ctx.fillStyle = theme.partColor(mode.part)
                ctx.fillRect(xOf(mode.hz) - theme.modeBarWidth / 2, base - h, theme.modeBarWidth, h)
                resting += 1
            }
        }
        ctx.globalAlpha = 1
        liveCount = live
        restingCount = resting
        if (live === 0 && ringing)
            ringing = false
    }

    // The title, then the key of parts (the frame's LIVE / PREVIEW chip
    // takes the top-right corner).
    Text {
        id: title
        x: view.theme.modesInset
        y: view.theme.spaceMd
        text: "RINGING MODES · log frequency"
        color: view.theme.inkFaint
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeBody
    }
    Row {
        anchors.left: title.right
        anchors.leftMargin: view.theme.modesKeyLead
        y: view.theme.spaceMd
        spacing: view.theme.modesKeyGap
        Repeater {
            model: [{ part: "resonator", name: "resonator" }, { part: "coupled", name: "coupled" },
                    { part: "body", name: "body" }]
            delegate: Row {
                id: key
                required property var modelData
                spacing: view.theme.spaceXs
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: view.theme.modesKeyDot
                    height: view.theme.modesKeyDot
                    radius: view.theme.modesKeyDot / 2
                    color: view.theme.partColor(key.modelData.part)
                }
                Text {
                    text: key.modelData.name
                    color: view.theme.inkFaint
                    font.family: view.theme.mono
                    font.pixelSize: view.theme.typeBody
                }
            }
        }
    }
}
