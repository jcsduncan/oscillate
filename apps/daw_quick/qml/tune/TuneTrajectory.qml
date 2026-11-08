pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The kick's pitch trajectory: its frequency at 5 ms steps from the onset
// over the first 400 ms, on a log axis with a line at every C and G, and
// the settled pitch marked across it with its note name.
Item {
    id: plot
    required property TuneTheme theme
    // Hz at 5 ms steps; 0 past the end of the hit.
    property var points: []
    property real settledHz: 0
    property string settledName: ""
    // The twelve note names in the panel's notation, from C.
    property var names: []
    function nameOf(k) {
        return names.length === 12 ? names[((k % 12) + 12) % 12] + (Math.floor(k / 12) - 1) : ""
    }
    property real reference: 440
    readonly property real lowHz: 25
    readonly property real highHz: 320
    readonly property real milliseconds: 400
    readonly property real padLeft: 46
    readonly property real padBottom: 22
    readonly property real plotX: padLeft
    readonly property real padRight: 10
    readonly property real plotW: width - padLeft - padRight
    readonly property real plotY: 8
    readonly property real plotH: height - plotY - padBottom
    // Time ticks across the plot, the ends included.
    readonly property int ticks: 5
    // The time labels' gap below the plot.
    readonly property real tickLabelGap: 5
    function yOf(hz) {
        const f = Math.log(Math.max(lowHz, Math.min(highHz, hz)) / lowHz) / Math.log(highHz / lowHz)
        return plotY + plotH * (1 - f)
    }
    function hzOf(note) {
        return reference * Math.pow(2, (note - 69) / 12)
    }
    readonly property var gridNotes: {
        const lines = []
        for (let n = 12; n <= 72; ++n) {
            const pc = n % 12
            const hz = hzOf(n)
            if ((pc === 0 || pc === 7) && hz >= lowHz && hz <= highHz)
                lines.push(n)
        }
        return lines
    }
    readonly property bool hasHit: {
        for (let i = 0; i < points.length; ++i) {
            if (points[i] > 0)
                return true
        }
        return false
    }

    objectName: "tuneTrajectory"

    Rectangle {
        anchors.fill: parent
        radius: plot.theme.viewWellRadius
        color: plot.theme.well
        border.color: plot.theme.wellEdge
    }
    Repeater {
        model: plot.gridNotes
        delegate: Item {
            id: line
            required property int modelData
            y: plot.yOf(plot.hzOf(modelData))
            width: plot.width
            Rectangle {
                x: plot.plotX
                width: plot.plotW
                height: plot.theme.hairline
                color: line.modelData % 12 === 0 ? plot.theme.gridStrong : plot.theme.grid
            }
            Text {
                x: plot.theme.spaceSm
                anchors.verticalCenter: parent.top
                width: plot.padLeft - plot.theme.spaceLg
                horizontalAlignment: Text.AlignRight
                text: plot.nameOf(line.modelData)
                color: plot.theme.inkFaint
                font.family: plot.theme.mono
                font.pixelSize: plot.theme.typeCaption
            }
        }
    }
    Repeater {
        model: plot.ticks
        delegate: Item {
            id: tick
            required property int index
            readonly property bool last: index === plot.ticks - 1
            x: plot.plotX + plot.plotW * index / (plot.ticks - 1)
            Rectangle {
                y: plot.plotY
                width: plot.theme.hairline
                height: plot.plotH
                color: plot.theme.grid
            }
            Text {
                x: tick.last ? -width : tick.index === 0 ? 0 : -width / 2
                y: plot.plotY + plot.plotH + plot.tickLabelGap
                text: Format.number(tick.index * plot.milliseconds / (plot.ticks - 1)) + (tick.last ? " ms" : "")
                color: plot.theme.inkFaint
                font.family: plot.theme.mono
                font.pixelSize: plot.theme.typeCaption
            }
        }
    }
    // The settled pitch.
    Item {
        objectName: "tuneSettledLine"
        visible: plot.settledHz > 0
        y: plot.yOf(plot.settledHz)
        x: plot.plotX
        width: plot.plotW
        Rectangle {
            width: parent.width
            height: plot.theme.hairline
            color: Qt.rgba(plot.theme.amber.r, plot.theme.amber.g, plot.theme.amber.b, plot.theme.settledLineAlpha)
        }
        Rectangle {
            anchors.right: parent.right
            anchors.bottom: parent.top
            anchors.bottomMargin: plot.theme.tagLift
            width: settledLabel.implicitWidth + plot.theme.spaceLg
            height: plot.theme.tagHeight
            radius: plot.theme.tagHeight / 2
            color: plot.theme.amberGlow
            border.color: plot.theme.amberDim
            Text {
                id: settledLabel
                anchors.centerIn: parent
                text: "SETTLES " + plot.settledName
                color: plot.theme.amberHot
                font.pixelSize: plot.theme.typeMicro
                font.bold: true
                font.letterSpacing: plot.theme.tagSpacing
            }
        }
    }
    Canvas {
        id: curve
        x: plot.plotX
        y: 0
        width: plot.plotW
        height: plot.height
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const pts = plot.points
            const n = pts.length
            if (n < 2)
                return
            const t = plot.theme
            const dx = width / Math.max(1, n - 1)
            const trace = (style, lineWidth) => {
                ctx.strokeStyle = style
                ctx.lineWidth = lineWidth
                ctx.beginPath()
                let open = false
                for (let i = 0; i < n; ++i) {
                    if (pts[i] <= 0) {
                        open = false
                        continue
                    }
                    const px = i * dx
                    const py = plot.yOf(pts[i])
                    if (!open)
                        ctx.moveTo(px, py)
                    else
                        ctx.lineTo(px, py)
                    open = true
                }
                ctx.stroke()
            }
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            trace(Qt.rgba(t.amber.r, t.amber.g, t.amber.b, t.glowAlpha), t.glowStroke)
            trace(t.amber, t.strokeBold)
            // Each measured cycle, as a dot.
            ctx.fillStyle = t.amberHot
            for (let i = 0; i < n; ++i) {
                if (pts[i] > 0) {
                    ctx.beginPath()
                    ctx.arc(i * dx, plot.yOf(pts[i]), t.traceDotRadius, 0, Math.PI * 2)
                    ctx.fill()
                }
            }
        }
        Connections {
            target: plot
            function onPointsChanged() { curve.requestPaint() }
        }
    }
    Text {
        visible: !plot.hasHit
        anchors.centerIn: parent
        text: "WAITING FOR A KICK"
        color: plot.theme.inkFaint
        font.pixelSize: plot.theme.typeBody
        font.bold: true
        font.letterSpacing: plot.theme.waitingSpacing
    }
}
