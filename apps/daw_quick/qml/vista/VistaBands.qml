pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The band strip: a log frequency axis from 20 Hz to 20 kHz with the side
// width of each band as a column (0-200%, 100% dashed) and the crossovers
// as handles to drag. Only the bands the Bands control asks for are shown;
// a crossover can never pass its neighbours (a third of an octave apart, as
// the imager keeps them). Mono Bass shades the side it removes. Live, each
// band shows its correlation.
Item {
    id: strip
    required property var controller
    required property VistaTheme theme
    property bool live: false
    property var bandCorrelation: [1, 1, 1, 1]
    property var bandWidthDb: [-120, -120, -120, -120]

    readonly property int revision: controller.revision
    readonly property int count: revision >= 0 ? controller.choice("vista.bands") + 1 : 3
    readonly property bool monoOn: revision >= 0 && controller.choice("vista.mono_bass") === 1
    // The overall width scales every band.
    readonly property real overall: revision >= 0 ? controller.value("vista.width") : 1
    readonly property real monoHz: revision >= 0 ? controller.value("vista.mono_freq") : 120
    readonly property var crossovers: revision >= 0
        ? [controller.value("vista.xover_1"), controller.value("vista.xover_2"),
           controller.value("vista.xover_3")] : [180, 2500, 7000]
    // Band edges in Hz: 20, the active crossovers, 20 k.
    readonly property var edges: {
        const e = [20]
        for (let k = 0; k < count - 1; ++k)
            e.push(crossovers[k])
        e.push(20000)
        return e
    }
    readonly property var names: count === 1 ? ["FULL BAND"]
        : count === 2 ? ["LOW", "HIGH"]
        : count === 3 ? ["LOW", "MID", "HIGH"] : ["LOW", "LOW MID", "HIGH MID", "HIGH"]

    // Plot area.
    readonly property real px: theme.bandsPlotLeft
    readonly property real py: theme.bandsPlotTop
    readonly property real pw: width - px - theme.bandsPlotRight
    readonly property real ph: height - py - theme.bandsPlotBottom

    // The spacing the imager keeps between crossovers.
    readonly property real spacing: 1.26
    // A band's width column as a fraction of the band.
    readonly property real columnFraction: 0.42
    // Below this correlation a band reads as wide rather than healthy.
    readonly property real wideBelow: 0.3
    // Arrow keys move a crossover by these factors (fine with Shift).
    readonly property real fineStep: 1.01
    readonly property real coarseStep: 1.06

    objectName: "vistaBands"
    implicitWidth: theme.bandsDefaultWidth
    implicitHeight: theme.bandsDefaultHeight

    function xOf(hz) {
        return px + pw * Math.log(Math.max(20, Math.min(20000, hz)) / 20) / Math.log(1000)
    }
    function hzOf(x) {
        return 20 * Math.pow(1000, Math.max(0, Math.min(1, (x - px) / pw)))
    }
    function yOf(width) {
        return py + ph * (1 - Math.max(0, Math.min(2, width)) / 2)
    }
    function widthOf(y) {
        return Math.max(0, Math.min(2, 2 * (1 - (y - py) / ph)))
    }
    // Where crossover k (1-based) may go: its range, kept a third of an
    // octave from the crossovers either side.
    function limitCrossover(k, hz) {
        const info = controller.parameter("vista.xover_" + k)
        let lo = info.minimum
        let hi = info.maximum
        if (k > 1)
            lo = Math.max(lo, crossovers[k - 2] * spacing)
        if (k < 3)
            hi = Math.min(hi, crossovers[k] / spacing)
        return Math.max(lo, Math.min(hi, hz))
    }
    function moveCrossover(k, hz) {
        controller.set_value("vista.xover_" + k, limitCrossover(k, hz))
        theme.lastTouched = "vista.xover_" + k
    }
    function hzText(hz) {
        return Format.hertz(hz)
    }
    readonly property real hertzPerKilohertz: 1000

    // The grid: decades and widths.
    Canvas {
        id: grid
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = strip.theme
            ctx.fillStyle = t.well
            ctx.fillRect(strip.px, strip.py, strip.pw, strip.ph)
            ctx.strokeStyle = t.wellEdge
            ctx.lineWidth = t.hairline
            ctx.strokeRect(strip.px + t.hairline / 2, strip.py + t.hairline / 2, strip.pw - t.hairline, strip.ph - t.hairline)
            ctx.font = t.typeMicro + "px '" + t.mono + "'"
            ctx.textBaseline = "top"
            ctx.textAlign = "center"
            for (const hz of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
                const x = Math.round(strip.xOf(hz)) + t.hairline / 2
                const decade = hz === 100 || hz === 1000 || hz === 10000
                ctx.strokeStyle = decade ? t.gridStrong : t.grid
                ctx.beginPath()
                ctx.moveTo(x, strip.py)
                ctx.lineTo(x, strip.py + strip.ph)
                ctx.stroke()
                ctx.fillStyle = decade ? t.inkDim : t.inkFaint
                ctx.fillText(hz >= strip.hertzPerKilohertz ? Format.number(hz / strip.hertzPerKilohertz, 0) + "k"
                                                           : Format.number(hz, 0), x, strip.py + strip.ph + t.spaceSm)
            }
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (const w of [0, 0.5, 1, 1.5, 2]) {
                const y = Math.round(strip.yOf(w)) + t.hairline / 2
                ctx.strokeStyle = w === 1 ? t.gridStrong : t.grid
                ctx.setLineDash(w === 1 ? t.bandsUnityDash : [])
                ctx.beginPath()
                ctx.moveTo(strip.px, y)
                ctx.lineTo(strip.px + strip.pw, y)
                ctx.stroke()
                ctx.fillStyle = w === 1 ? t.inkDim : t.inkFaint
                ctx.fillText(Format.fraction(w), strip.px - t.spaceSm, y)
            }
            ctx.setLineDash([])
        }
        Connections {
            target: strip
            function onWidthChanged() { grid.requestPaint() }
            function onHeightChanged() { grid.requestPaint() }
        }
    }

    // Mono Bass: the side removed below its frequency, hatched.
    Item {
        objectName: "vistaMonoShade"
        visible: strip.monoOn
        x: strip.px + strip.theme.hairline
        y: strip.py + strip.theme.hairline
        width: Math.max(0, strip.xOf(strip.monoHz) - strip.px - strip.theme.hairline)
        height: strip.ph - 2 * strip.theme.hairline
        clip: true
        Rectangle {
            anchors.fill: parent
            color: strip.theme.monoShade
        }
        Canvas {
            id: hatch
            anchors.fill: parent
            renderStrategy: Canvas.Cooperative
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = strip.theme.monoHatch
                ctx.lineWidth = strip.theme.hairline
                for (let x = -height; x < width; x += strip.theme.monoHatchPitch) {
                    ctx.beginPath()
                    ctx.moveTo(x, height)
                    ctx.lineTo(x + height, 0)
                    ctx.stroke()
                }
            }
            onWidthChanged: requestPaint()
        }
        Rectangle {
            anchors.right: parent.right
            width: strip.theme.hairline
            height: parent.height
            color: strip.theme.accentSoft
        }
        Text {
            x: strip.theme.monoLabelX
            y: parent.height - strip.theme.monoLabelRise
            text: "MONO"
            color: strip.theme.glacier
            font.pixelSize: strip.theme.typeMicro
            font.bold: true
            font.letterSpacing: strip.theme.monoLabelSpacing
            visible: parent.width > strip.theme.monoLabelMinWidth
        }
    }

    // The bands.
    Repeater {
        model: 4
        delegate: Item {
            id: band
            required property int index
            readonly property string parameterId: "vista.width_" + (index + 1)
            readonly property real lowX: strip.xOf(strip.edges[Math.min(index, strip.edges.length - 2)])
            readonly property real highX: strip.xOf(strip.edges[Math.min(index + 1, strip.edges.length - 1)])
            readonly property real value: strip.revision >= 0 ? strip.controller.value(parameterId) : 1
            // What is heard: the band's width times the overall width.
            readonly property real effective: Math.min(2, value * strip.overall)
            readonly property bool scaled: Math.abs(strip.overall - 1) > 0.005
            readonly property real correlation: strip.bandCorrelation[index] !== undefined
                ? strip.bandCorrelation[index] : 1
            readonly property bool editable: strip.controller.editable

            objectName: "vistaBandBar:" + parameterId
            visible: index < strip.count
            x: lowX
            y: strip.py
            width: highX - lowX
            height: strip.ph
            activeFocusOnTab: editable
            Accessible.role: Accessible.Slider
            Accessible.name: strip.names[Math.min(index, strip.names.length - 1)] + " width"
            Accessible.description: strip.revision >= 0 ? strip.controller.display(parameterId) : ""
            Keys.onPressed: event => {
                const fine = (event.modifiers & Qt.ShiftModifier) !== 0
                if (event.key === Qt.Key_Up || event.key === Qt.Key_Right) {
                    strip.controller.step(parameterId, 1, fine)
                } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Left) {
                    strip.controller.step(parameterId, -1, fine)
                } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
                    strip.controller.reset_to_default(parameterId)
                } else {
                    return
                }
                strip.theme.lastTouched = parameterId
                event.accepted = true
            }

            // Alternate bands sit on a faintly lighter ground.
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: strip.theme.hairline
                anchors.bottomMargin: strip.theme.hairline
                color: band.index % 2 === 1 ? strip.theme.bandAlternateFill : "transparent"
                border.color: band.activeFocus ? strip.theme.accent : "transparent"
            }
            // The width column: filled to the width heard, outlined at the
            // band's own setting when the overall width scales it.
            Rectangle {
                id: column
                readonly property real topY: strip.yOf(band.effective) - strip.py
                width: Math.max(strip.theme.bandColumnMin, Math.min(strip.theme.bandColumnMax, band.width * strip.columnFraction))
                x: (band.width - width) / 2
                y: topY
                height: band.height - topY
                radius: strip.theme.radiusXs
                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: band.effective > 1 ? strip.theme.bandWideTop : strip.theme.bandNarrowTop
                    }
                    GradientStop {
                        position: 1
                        color: strip.theme.bandColumnFoot
                    }
                }
                Rectangle {
                    width: parent.width
                    height: strip.theme.bandCapHeight
                    color: band.activeFocus || bandArea.pressed ? strip.theme.glint : strip.theme.glacier
                }
            }
            Rectangle {
                id: setting
                objectName: "vistaBandSetting:" + band.index
                visible: band.scaled
                readonly property real topY: strip.yOf(band.value) - strip.py
                x: column.x
                y: topY
                width: column.width
                height: band.height - topY
                radius: strip.theme.radiusXs
                color: "transparent"
                border.color: strip.theme.bandSettingEdge
            }
            Text {
                objectName: "vistaBandValue:" + band.index
                y: Math.max(strip.theme.spaceXxs, Math.min(column.y, band.scaled ? setting.y : column.y) - strip.theme.bandValueRise)
                width: band.width
                horizontalAlignment: Text.AlignHCenter
                text: strip.revision >= 0
                    ? strip.controller.display(band.parameterId)
                      + (band.scaled ? "  →  " + Format.fraction(band.effective) : "")
                    : ""
                color: strip.theme.glacier
                font.family: strip.theme.mono
                font.pixelSize: strip.theme.typeBody
            }
            Text {
                y: -strip.theme.bandNameRise
                width: band.width
                horizontalAlignment: Text.AlignHCenter
                text: strip.names[Math.min(band.index, strip.names.length - 1)]
                color: strip.theme.inkDim
                font.pixelSize: strip.theme.typeMicro
                font.bold: true
                font.letterSpacing: strip.theme.bandNameSpacing
                elide: Text.ElideRight
            }
            // Live correlation of the band, coloured by zone.
            Rectangle {
                objectName: "vistaBandCorrelation:" + band.index
                visible: strip.live && band.width > strip.theme.chipMinBandWidth
                anchors.horizontalCenter: parent.horizontalCenter
                y: band.height - strip.theme.chipRise
                width: chip.implicitWidth + strip.theme.chipPad
                height: strip.theme.chipHeight
                radius: height / 2
                readonly property real value: band.correlation
                color: strip.theme.well
                border.color: band.correlation < 0 ? strip.theme.negative
                    : band.correlation < strip.wideBelow ? strip.theme.wide : strip.theme.positive
                Text {
                    id: chip
                    anchors.centerIn: parent
                    text: "ρ " + Format.signed_number(band.correlation)
                    color: strip.theme.ink
                    font.family: strip.theme.mono
                    font.pixelSize: strip.theme.typeCaption
                }
            }
            MouseArea {
                id: bandArea
                anchors.fill: parent
                enabled: band.editable
                preventStealing: true
                cursorShape: Qt.SizeVerCursor
                onPressed: mouse => {
                    band.forceActiveFocus()
                    strip.theme.lastTouched = band.parameterId
                    strip.controller.begin_gesture(band.parameterId)
                    strip.controller.set_value(band.parameterId, strip.widthOf(mouse.y + strip.py))
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        strip.controller.set_value(band.parameterId, strip.widthOf(mouse.y + strip.py))
                }
                onReleased: strip.controller.end_gesture(band.parameterId)
                onCanceled: strip.controller.cancel_gesture(band.parameterId)
                onDoubleClicked: strip.controller.reset_to_default(band.parameterId)
            }
        }
    }

    // The crossover handles.
    Repeater {
        model: 3
        delegate: Item {
            id: handle
            required property int index
            readonly property string parameterId: "vista.xover_" + (index + 1)
            readonly property real hz: strip.crossovers[index]
            objectName: "vistaCrossover:" + parameterId
            visible: index < strip.count - 1
            x: strip.xOf(hz) - width / 2
            y: 0
            width: strip.theme.handleWidth
            height: strip.py + strip.ph
            activeFocusOnTab: strip.controller.editable
            Accessible.role: Accessible.Slider
            Accessible.name: "Crossover " + (index + 1)
            Accessible.description: strip.hzText(hz)
            Keys.onPressed: event => {
                const factor = (event.modifiers & Qt.ShiftModifier) !== 0 ? strip.fineStep : strip.coarseStep
                if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
                    strip.moveCrossover(handle.index + 1, handle.hz * factor)
                else if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
                    strip.moveCrossover(handle.index + 1, handle.hz / factor)
                else
                    return
                event.accepted = true
            }

            Rectangle {
                x: handle.width / 2 - strip.theme.hairline / 2
                y: strip.py
                width: strip.theme.hairline
                height: strip.ph
                color: handleArea.pressed || handle.activeFocus ? strip.theme.glint : strip.theme.accentSoft
            }
            Rectangle {
                id: grip
                anchors.horizontalCenter: parent.horizontalCenter
                y: strip.py + strip.theme.spaceXs
                width: label.implicitWidth + strip.theme.gripPad
                height: strip.theme.gripHeight
                radius: strip.theme.radiusSm
                color: handleArea.pressed || handle.activeFocus ? strip.theme.accent : strip.theme.plateHi
                border.color: strip.theme.accentSoft
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: strip.hzText(handle.hz)
                    color: handleArea.pressed || handle.activeFocus ? strip.theme.caseColor : strip.theme.glacier
                    font.family: strip.theme.mono
                    font.pixelSize: strip.theme.typeCaption
                }
            }
            MouseArea {
                id: handleArea
                x: handle.width / 2 - strip.theme.handleGrabWidth / 2
                y: strip.py
                width: strip.theme.handleGrabWidth
                height: strip.ph
                enabled: strip.controller.editable
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                onPressed: {
                    handle.forceActiveFocus()
                    strip.controller.begin_gesture(handle.parameterId)
                    strip.theme.lastTouched = handle.parameterId
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        strip.moveCrossover(handle.index + 1,
                                            strip.hzOf(mapToItem(strip, mouse.x, mouse.y).x))
                }
                onReleased: strip.controller.end_gesture(handle.parameterId)
                onCanceled: strip.controller.cancel_gesture(handle.parameterId)
                onDoubleClicked: strip.controller.reset_to_default(handle.parameterId)
            }
            // The grip itself is draggable too.
            MouseArea {
                anchors.fill: grip
                enabled: strip.controller.editable
                preventStealing: true
                cursorShape: Qt.SizeHorCursor
                onPressed: {
                    handle.forceActiveFocus()
                    strip.controller.begin_gesture(handle.parameterId)
                    strip.theme.lastTouched = handle.parameterId
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        strip.moveCrossover(handle.index + 1,
                                            strip.hzOf(mapToItem(strip, mouse.x, mouse.y).x))
                }
                onReleased: strip.controller.end_gesture(handle.parameterId)
                onCanceled: strip.controller.cancel_gesture(handle.parameterId)
            }
        }
    }
}
