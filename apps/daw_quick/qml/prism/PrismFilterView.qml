pragma ComponentBehavior: Bound
import QtQuick

// The filter's response curve. Drag anywhere: left/right moves the cutoff,
// up/down sets resonance. Scroll adjusts resonance; double-click resets both.
Rectangle {
    id: view
    required property var controller
    required property PrismTheme theme
    property color accent: theme.filter
    readonly property int revision: controller.revision
    readonly property real cutoffFraction: revision >= 0
        ? controller.fraction_of(controller.value("prism.filter.cutoff")) : 0.5
    readonly property real resonance: revision >= 0 ? controller.value("prism.filter.resonance") : 0
    readonly property real topDb: 18
    readonly property real bottomDb: -42
    // The response is sampled every other pixel, never fewer than fewestPoints.
    readonly property int fewestPoints: 32
    readonly property int points: Math.max(fewestPoints, Math.round(width / 2))
    readonly property var response: revision >= 0 ? controller.filter_response(points) : []
    readonly property real cutoffDb: response.length > 0
        ? response[Math.round(cutoffFraction * (points - 1))] : 0
    // The decades the grid marks and the levels the frame labels, as
    // [value, label]; the first carries the axis unit.
    readonly property var frequencyMarks: [[100, "100"], [1000, "1k"], [10000, "10k"]]
    readonly property var levelMarks: [[0, "0"], [12, "+12"], [-24, "-24"]]
    // The same marks as PlotFrame ticks, from this view's own mapping.
    readonly property var frequencyTicks: frequencyMarks.map(mark => ({
        at: controller.fraction_of(mark[0]), label: mark[1] }))
    readonly property var levelTicks: height > 0 ? levelMarks.map(mark => ({
        at: 1 - yOf(mark[0]) / height, label: mark[1] })) : []

    objectName: "prismFilterView"
    radius: theme.viewRadius
    color: theme.well
    border.color: activeFocus ? accent : theme.line
    activeFocusOnTab: controller.editable
    Accessible.role: Accessible.Graphic
    Accessible.name: "Filter response"
    Accessible.description: "Cutoff " + controller.display("prism.filter.cutoff") + ", resonance "
        + controller.display("prism.filter.resonance")
        + ". Drag horizontally for cutoff and vertically for resonance"

    function yOf(db) {
        const clamped = Math.max(bottomDb, Math.min(topDb, db))
        return theme.filterPlotInset + (topDb - clamped) / (topDb - bottomDb) * (height - 2 * theme.filterPlotInset)
    }

    Keys.onPressed: event => {
        const fine = (event.modifiers & Qt.ShiftModifier) !== 0
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            controller.step("prism.filter.cutoff", event.key === Qt.Key_Right ? 1 : -1, fine)
            event.accepted = true
        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            controller.step("prism.filter.resonance", event.key === Qt.Key_Up ? 1 : -1, fine)
            event.accepted = true
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            // Decade grid; the frame labels it.
            const t = view.theme
            ctx.strokeStyle = t.line
            ctx.lineWidth = t.hairline
            for (let i = 0; i < view.frequencyMarks.length; ++i) {
                const x = Math.round(view.controller.fraction_of(view.frequencyMarks[i][0]) * w) + 0.5
                ctx.globalAlpha = t.filterGridAlpha
                ctx.beginPath(); ctx.moveTo(x, t.filterPlotInset); ctx.lineTo(x, h - t.filterPlotInset); ctx.stroke()
                ctx.globalAlpha = 1
            }
            const zero = Math.round(view.yOf(0)) + 0.5
            ctx.globalAlpha = t.filterZeroAlpha
            ctx.setLineDash(t.filterZeroDash)
            ctx.beginPath(); ctx.moveTo(0, zero); ctx.lineTo(w, zero); ctx.stroke()
            ctx.setLineDash([])
            ctx.globalAlpha = 1

            const response = view.response
            const step = w / (response.length - 1)
            const gradient = ctx.createLinearGradient(0, 0, 0, h)
            gradient.addColorStop(0, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, t.filterFillAlpha))
            gradient.addColorStop(1, Qt.rgba(view.accent.r, view.accent.g, view.accent.b, t.filterFillEndAlpha))
            ctx.fillStyle = gradient
            ctx.beginPath()
            ctx.moveTo(0, h)
            for (let i = 0; i < response.length; ++i)
                ctx.lineTo(i * step, view.yOf(response[i]))
            ctx.lineTo(w, h)
            ctx.closePath()
            ctx.fill()
            ctx.strokeStyle = view.accent
            ctx.lineWidth = t.filterLineWidth
            ctx.lineJoin = "round"
            ctx.beginPath()
            for (let i = 0; i < response.length; ++i) {
                const y = view.yOf(response[i])
                if (i === 0)
                    ctx.moveTo(0, y)
                else
                    ctx.lineTo(i * step, y)
            }
            ctx.stroke()
        }
        Connections {
            target: view
            function onRevisionChanged() { canvas.requestPaint() }
        }
    }

    // Cutoff handle rides the curve at the cutoff frequency.
    Rectangle {
        id: handle
        x: view.cutoffFraction * view.width - width / 2
        y: view.yOf(view.cutoffDb) - height / 2
        width: view.theme.filterHandleSize
        height: view.theme.filterHandleSize
        radius: width / 2
        color: view.theme.panel
        border.color: view.accent
        border.width: view.theme.filterHandleEdgeWidth
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: view.controller.editable
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.CrossCursor
        function apply(mouse) {
            const fx = Math.max(0, Math.min(1, mouse.x / view.width))
            const fy = Math.max(0, Math.min(1, 1 - (mouse.y - view.theme.filterDragInset)
                / (view.height - 2 * view.theme.filterDragInset)))
            view.controller.set_value("prism.filter.cutoff", view.controller.frequency_at(fx))
            view.controller.set_value("prism.filter.resonance", fy)
        }
        onPressed: mouse => {
            view.forceActiveFocus()
            view.controller.begin_gesture("prism.filter.cutoff")
            apply(mouse)
        }
        onPositionChanged: mouse => {
            if (pressed)
                apply(mouse)
        }
        onReleased: view.controller.end_gesture("prism.filter.cutoff")
        onCanceled: view.controller.cancel_gesture("prism.filter.cutoff")
        onDoubleClicked: {
            view.controller.reset_to_default("prism.filter.cutoff")
            view.controller.reset_to_default("prism.filter.resonance")
        }
        onWheel: wheel => {
            view.controller.step("prism.filter.resonance", wheel.angleDelta.y > 0 ? 1 : -1,
                (wheel.modifiers & Qt.ShiftModifier) !== 0)
            wheel.accepted = true
        }
    }
}
