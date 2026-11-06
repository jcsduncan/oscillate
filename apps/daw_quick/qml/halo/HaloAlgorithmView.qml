import QtQuick

// The current algorithm's routing, drawn from the engine's algorithm data:
// numbered operator boxes, carriers (amber) on the bottom row feeding the
// output bus, modulators (teal) above their targets, and feedback in red:
// a small loop on an operator that feeds back on itself, or, where the chart
// closes a loop across operators (algorithms 4 and 6), an arrow from the
// loop's last operator back into its first.
Rectangle {
    id: view
    required property var controller
    required property HaloTheme theme
    required property int algorithm
    readonly property var layout: controller.fm_algorithm(algorithm)
    readonly property int feedbackFrom: layout.feedback_from !== undefined ? layout.feedback_from : -1
    readonly property bool loop: layout.feedback !== undefined && feedbackFrom !== layout.feedback
    readonly property int operators: 6

    // The drawing's geometry: each operator cell is at most maxCellWidth by
    // maxCellHeight and holds a box of boxSize (less boxGap in short cells).
    // The output bus runs busClearance below the bottom row; busReserve keeps
    // room for it, busDrop is its share of the centred drawing and busLift
    // raises the whole drawing. The bus ends in an arrow; feedback loops step
    // out and over by their gaps and end in a small arrowhead.
    readonly property real maxCellWidth: 62
    readonly property real maxCellHeight: 46
    readonly property real boxSize: 28
    readonly property real boxGap: 8
    readonly property real busReserve: 28
    readonly property real busDrop: 22
    readonly property real busLift: 6
    readonly property real busClearance: 14
    readonly property real busRun: 16
    readonly property real busArrowLength: 5
    readonly property real busArrowHalf: 3
    readonly property real loopSideGap: 10
    readonly property real loopTopGap: 7
    readonly property real loopArrowLength: 5
    readonly property real loopArrowHalf: 3.5
    readonly property real loopEntryGap: 4
    readonly property real selfLoopRise: 5
    readonly property real selfLoopReach: 7
    // Edges closer than this run straight down.
    readonly property real straightTolerance: 0.5

    objectName: "haloAlgorithmView"
    radius: theme.radiusMd
    color: theme.lcd
    border.color: theme.lcdEdge
    Accessible.role: Accessible.Graphic
    Accessible.name: "Algorithm " + (algorithm + 1) + " routing"
    Accessible.description: {
        const l = layout
        if (l.carriers === undefined)
            return ""
        const heard = []
        for (let i = 0; i < view.operators; ++i)
            if (l.carriers[i])
                heard.push(i + 1)
        return "Carriers " + heard.join(", ") + (view.loop
            ? "; feedback loop from operator " + (view.feedbackFrom + 1) + " into operator " + (l.feedback + 1)
            : "; feedback on operator " + (l.feedback + 1))
    }

    Text {
        x: view.theme.spaceMd
        y: view.theme.spaceSm
        text: "ALG " + (view.algorithm + 1 < 10 ? "0" : "") + (view.algorithm + 1)
        color: view.theme.lcdText
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeEmphasis
        font.bold: true
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.spaceMd
        y: view.theme.spaceMd
        text: "■ CARRIER  □ MODULATOR"
        color: view.theme.lcdDim
        font.family: view.theme.mono
        font.pixelSize: view.theme.typeMicro
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.topMargin: view.theme.routingTop
        anchors.bottomMargin: view.theme.spaceSm
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const l = view.layout
            if (l.carriers === undefined)
                return
            const cellW = Math.min(width / Math.max(l.columns, 1), view.maxCellWidth)
            const cellH = Math.min((height - view.busReserve) / Math.max(l.rows, 1), view.maxCellHeight)
            // Tall stacks shrink the boxes so edges stay visible between them.
            const box = Math.min(view.boxSize, cellH - view.boxGap)
            const x0 = (width - cellW * l.columns) / 2
            // Centre the drawing (rows plus the output bus) vertically.
            const used = cellH * l.rows + view.busDrop
            const bus = (height + used) / 2 - view.busLift
            const cx = op => x0 + (l.column[op] + 0.5) * cellW
            const cy = op => bus - view.busClearance - (l.row[op] + 0.5) * cellH
            const teal = view.theme.teal
            const amber = view.theme.amber
            ctx.lineWidth = view.theme.routingStroke
            ctx.lineCap = "round"

            // Modulation edges: down from the modulator, across, into the target.
            ctx.strokeStyle = Qt.rgba(teal.r, teal.g, teal.b, view.theme.routingEdgeAlpha)
            for (const edge of l.edges) {
                const fx = cx(edge.from)
                const fy = cy(edge.from) + box / 2
                const tx = cx(edge.to)
                const ty = cy(edge.to) - box / 2
                const mid = (fy + ty) / 2
                ctx.beginPath()
                ctx.moveTo(fx, fy)
                if (Math.abs(fx - tx) > view.straightTolerance) {
                    ctx.lineTo(fx, mid)
                    ctx.lineTo(tx, mid)
                }
                ctx.lineTo(tx, ty)
                ctx.stroke()
            }

            // Output bus.
            let left = width
            let right = 0
            ctx.strokeStyle = amber
            for (let op = 0; op < view.operators; ++op) {
                if (!l.carriers[op])
                    continue
                left = Math.min(left, cx(op))
                right = Math.max(right, cx(op))
                ctx.beginPath()
                ctx.moveTo(cx(op), cy(op) + box / 2)
                ctx.lineTo(cx(op), bus)
                ctx.stroke()
            }
            ctx.beginPath()
            ctx.moveTo(left, bus)
            ctx.lineTo(right + view.busRun, bus)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(right + view.busRun, bus - view.busArrowHalf)
            ctx.lineTo(right + view.busRun + view.busArrowLength, bus)
            ctx.lineTo(right + view.busRun, bus + view.busArrowHalf)
            ctx.closePath()
            ctx.fillStyle = amber
            ctx.fill()

            const fb = l.feedback
            const fx = cx(fb)
            const fy = cy(fb)
            ctx.strokeStyle = view.theme.red
            ctx.fillStyle = view.theme.red
            if (view.loop) {
                // A loop across operators: out of the source's right side,
                // up past the destination and down into its top.
                const sx = cx(view.feedbackFrom)
                const sy = cy(view.feedbackFrom)
                const side = Math.max(sx, fx) + box / 2 + view.loopSideGap
                const top = fy - box / 2 - view.loopTopGap
                ctx.beginPath()
                ctx.moveTo(sx + box / 2, sy)
                ctx.lineTo(side, sy)
                ctx.lineTo(side, top)
                ctx.lineTo(fx, top)
                ctx.lineTo(fx, fy - box / 2 - view.loopEntryGap)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(fx - view.loopArrowHalf, fy - box / 2 - view.loopArrowLength)
                ctx.lineTo(fx + view.loopArrowHalf, fy - box / 2 - view.loopArrowLength)
                ctx.lineTo(fx, fy - box / 2)
                ctx.closePath()
                ctx.fill()
            } else {
                // Self-feedback: out of the top, round the right side and
                // back in.
                ctx.beginPath()
                ctx.moveTo(fx, fy - box / 2)
                ctx.lineTo(fx, fy - box / 2 - view.selfLoopRise)
                ctx.lineTo(fx + box / 2 + view.selfLoopReach, fy - box / 2 - view.selfLoopRise)
                ctx.lineTo(fx + box / 2 + view.selfLoopReach, fy)
                ctx.lineTo(fx + box / 2, fy)
                ctx.stroke()
            }

            // Operator boxes.
            ctx.font = "bold " + view.theme.typeEmphasis + "px sans-serif"
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            for (let op = 0; op < view.operators; ++op) {
                const heard = l.carriers[op]
                const colour = heard ? amber : teal
                const x = cx(op) - box / 2
                const y = cy(op) - box / 2
                ctx.fillStyle = heard ? Qt.rgba(amber.r, amber.g, amber.b, view.theme.carrierFillAlpha) : view.theme.lcd
                ctx.fillRect(x, y, box, box)
                ctx.strokeStyle = colour
                ctx.lineWidth = view.theme.routingStroke
                // Inset by half a hairline so the edge lands on whole pixels.
                const edge = view.theme.hairline
                ctx.strokeRect(x + edge / 2, y + edge / 2, box - edge, box - edge)
                ctx.fillStyle = heard ? view.theme.lcd : teal
                ctx.fillText(String(op + 1), cx(op), cy(op) + view.theme.hairline / 2)
            }
        }
    }
    onLayoutChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
}
