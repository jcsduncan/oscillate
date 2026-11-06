pragma ComponentBehavior: Bound
import QtQuick

// Strata's spectrum: the input as a faint trace and the output bright over
// it, a tint per band, each band's live gain change drawn in the band's own
// shape around a 0 dB line, the crossovers as handles to drag (kept apart by
// the engine) and each band's threshold as a dashed line to drag on its own
// -60..0 dB scale. Double-click adds a band where you click. Shapes and
// crossovers come from the controller (strata_band_shapes,
// strata_crossovers), levels only from the telemetry.
Item {
    id: view
    required property var panel
    required property var controller
    required property StrataTheme theme

    readonly property real lowHz: 20
    readonly property real span: 1000
    // The spectra's scale (dBFS) and the gain overlay's (dB either way).
    readonly property real topDb: -6
    readonly property real bottomDb: -96
    readonly property real gainSpanDb: 18
    readonly property real thresholdFloorDb: -60
    readonly property int points: 160
    readonly property var gridHz: [50, 100, 200, 500, 1000, 2000, 5000, 10000]

    readonly property int revision: controller.revision
    readonly property var crossovers: revision >= 0 ? controller.strata_crossovers() : []
    readonly property var shapes: revision >= 0 ? controller.strata_band_shapes(points) : []
    readonly property int bands: crossovers.length + 1
    // Grid lines sit on pixel centres.
    readonly property real pixelCentre: 0.5

    objectName: "strataSpectrum"

    function xOf(hz) {
        return width * Math.log(Math.max(hz, lowHz) / lowHz) / Math.log(span)
    }
    function hzOf(x) {
        return lowHz * Math.pow(span, Math.max(0, Math.min(1, x / width)))
    }
    function ySpectrum(db) {
        return height * (topDb - db) / (topDb - bottomDb)
    }
    function yGain(db) {
        return height / 2 - (height / 2) * Math.max(-1, Math.min(1, db / gainSpanDb))
    }
    function yThreshold(db) {
        return height * db / thresholdFloorDb
    }
    function thresholdOf(y) {
        return thresholdFloorDb * Math.max(0, Math.min(1, y / height))
    }
    function edgeLow(b) {
        return b === 0 ? lowHz : crossovers[b - 1]
    }
    function edgeHigh(b) {
        return b >= crossovers.length ? lowHz * span : crossovers[b]
    }
    function centreOf(b) {
        return Math.sqrt(edgeLow(b) * edgeHigh(b))
    }

    onRevisionChanged: canvas.requestPaint()
    Connections {
        target: view.panel
        function onLiveChanged() { canvas.requestPaint() }
        function onSelectedChanged() { canvas.requestPaint() }
    }

    Rectangle {
        anchors.fill: parent
        radius: view.theme.radiusLg
        color: view.theme.screen
        border.color: view.theme.wellEdge
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width
            const h = height
            // Band tints.
            for (let b = 0; b < view.bands; ++b) {
                const x0 = view.xOf(view.edgeLow(b))
                const x1 = view.xOf(view.edgeHigh(b))
                const c = Qt.color(view.theme.band(b))
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, b === view.panel.selected
                    ? view.theme.bandTintSelectedAlpha : view.theme.bandTintAlpha)
                ctx.fillRect(x0, 0, x1 - x0, h)
            }
            // Grid.
            ctx.strokeStyle = view.theme.grid
            ctx.lineWidth = view.theme.hairline
            for (const hz of view.gridHz) {
                const x = Math.round(view.xOf(hz)) + view.pixelCentre
                ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke()
            }
            ctx.strokeStyle = view.theme.gridStrong
            ctx.beginPath(); ctx.moveTo(0, h / 2 + view.pixelCentre); ctx.lineTo(w, h / 2 + view.pixelCentre); ctx.stroke()

            const live = view.panel.live
            const playing = live.live === true
            // Spectra.
            const trace = function(values, colour, lineWidth) {
                if (!values || values.length === 0)
                    return
                ctx.strokeStyle = colour
                ctx.lineWidth = lineWidth
                ctx.beginPath()
                for (let i = 0; i < values.length; ++i) {
                    const hz = view.lowHz * Math.pow(view.span, (i + 0.5) / values.length)
                    const x = view.xOf(hz)
                    const y = view.ySpectrum(Math.max(view.bottomDb, values[i]))
                    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
            if (playing) {
                trace(live.input_spectrum, view.theme.inputTrace, view.theme.inputTraceWidth)
                trace(live.output_spectrum, view.theme.outputTrace, view.theme.outputTraceWidth)
            }
            // Each band's gain change in its own shape.
            for (let b = 0; b < view.bands && b < view.shapes.length; ++b) {
                const gain = playing && live.band_gain ? live.band_gain[b] : 0
                if (Math.abs(gain) < 0.05)
                    continue
                const shape = view.shapes[b]
                const c = Qt.color(view.theme.band(b))
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, view.theme.gainFillAlpha)
                ctx.beginPath()
                ctx.moveTo(0, view.yGain(0))
                for (let i = 0; i < shape.length; ++i) {
                    const x = w * i / (shape.length - 1)
                    ctx.lineTo(x, view.yGain(gain * shape[i]))
                }
                ctx.lineTo(w, view.yGain(0))
                ctx.closePath()
                ctx.fill()
            }
        }
    }

    // Double-click adds a band at that frequency.
    MouseArea {
        objectName: "strataSpectrumArea"
        anchors.fill: parent
        enabled: view.controller.editable
        onDoubleClicked: (mouse) => view.panel.addBandAt(view.hzOf(mouse.x))
    }

    // Thresholds, dashed, one per band across its region.
    Repeater {
        model: view.bands
        delegate: Item {
            id: line
            required property int index
            readonly property string parameterId: view.panel.bandId(index, "threshold")
            readonly property real threshold: view.revision >= 0
                ? view.panel.bandValue(index, "threshold") : -18
            objectName: "strataThreshold:" + index
            x: view.xOf(view.edgeLow(index)) + view.theme.thresholdLineInset
            width: view.xOf(view.edgeHigh(index)) - view.xOf(view.edgeLow(index))
                - 2 * view.theme.thresholdLineInset
            y: view.yThreshold(threshold) - height / 2
            height: view.theme.thresholdHitHeight
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: view.theme.thresholdDashGap
                Repeater {
                    model: Math.max(0, Math.floor(line.width / (view.theme.thresholdDash + view.theme.thresholdDashGap)))
                    delegate: Rectangle {
                        width: view.theme.thresholdDash
                        height: view.theme.hairline
                        color: view.theme.band(line.index)
                        opacity: view.theme.thresholdDashOpacity
                    }
                }
            }
            Text {
                anchors.right: parent.right
                anchors.bottom: parent.top
                text: line.threshold.toFixed(1) + " dB"
                color: view.theme.band(line.index)
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeCaption
                visible: lineArea.containsMouse || lineArea.pressed
            }
            MouseArea {
                id: lineArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeVerCursor
                enabled: view.controller.editable
                onPressed: (mouse) => {
                    view.panel.takeOver()
                    view.panel.selected = line.index
                    view.theme.lastTouched = line.parameterId
                    view.controller.begin_gesture(line.parameterId)
                }
                onPositionChanged: (mouse) => {
                    if (pressed)
                        view.controller.set_value(line.parameterId,
                            view.thresholdOf(line.mapToItem(view, mouse.x, mouse.y).y))
                }
                onReleased: view.controller.end_gesture(line.parameterId)
            }
        }
    }

    // Crossover handles.
    Repeater {
        model: view.crossovers.length
        delegate: Item {
            id: handle
            required property int index
            readonly property string parameterId: "strata.xover_" + (index + 1)
            readonly property real hz: view.crossovers[index] !== undefined ? view.crossovers[index] : 100
            objectName: "strataCrossover:" + parameterId
            x: view.xOf(hz) - width / 2
            y: 0
            width: view.theme.crossoverHitWidth
            height: view.height
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: crossArea.containsMouse || crossArea.pressed ? view.theme.strokeBold : view.theme.hairline
                height: parent.height
                color: view.theme.inkDim
                opacity: view.theme.crossoverLineOpacity
            }
            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                y: view.theme.crossoverKnobY
                width: view.theme.crossoverKnobSize
                height: view.theme.crossoverKnobSize
                radius: width / 2
                color: view.theme.plateHi
                border.color: view.theme.ink
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                y: view.height - view.theme.crossoverLabelBottom
                text: handle.hz >= 1000 ? (handle.hz / 1000).toFixed(handle.hz >= 10000 ? 1 : 2) + "k"
                                        : Math.round(handle.hz)
                color: view.theme.inkDim
                font.family: view.theme.mono
                font.pixelSize: view.theme.typeCaption
            }
            MouseArea {
                id: crossArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeHorCursor
                enabled: view.controller.editable
                onPressed: {
                    view.panel.takeOver()
                    view.theme.lastTouched = handle.parameterId
                    view.controller.begin_gesture(handle.parameterId)
                }
                onPositionChanged: (mouse) => {
                    if (pressed)
                        view.controller.set_value(handle.parameterId,
                            view.hzOf(handle.mapToItem(view, mouse.x, mouse.y).x))
                }
                onReleased: view.controller.end_gesture(handle.parameterId)
            }
        }
    }
}
