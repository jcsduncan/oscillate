pragma ComponentBehavior: Bound
import QtQuick

// A plot's frame, shared by every panel (issue #169): labelled axes with
// the unit on the first tick ("20 Hz", "0 dB"), an optional grid at the
// ticks, and a chip in the top-right corner saying whether the plot shows
// the running effect (LIVE) or the settings drawn ahead of it (PREVIEW).
// The plot draws its own content inside `plotArea` (the frame's children
// go there), whose width and height are the frame's less the gutters.
//
// A tick is { at, label }: `at` runs 0..1 left to right on x and bottom to
// top on y. An axis with no ticks takes no gutter.
Item {
    id: frame
    required property KitTheme theme
    // Marks the frame for the panel contracts.
    readonly property bool plotFrame: true
    property bool live: false
    property bool showsChip: true
    property var xTicks: []
    property var yTicks: []
    property string xUnit: ""
    property string yUnit: ""
    property bool grid: false
    readonly property alias plotArea: area
    default property alias content: area.data
    readonly property string chipText: live ? "LIVE" : "PREVIEW"
    // Whether each axis is drawn. Settled after the ticks change, not bound
    // to them: a plot's ticks often follow its own size, which follows the
    // gutters, and a binding would loop through them.
    property bool xAxisShown: false
    property bool yAxisShown: false
    Component.onCompleted: settleAxes()
    function settleAxes() {
        xAxisShown = xTicks.length > 0
        yAxisShown = yTicks.length > 0
    }
    onXTicksChanged: Qt.callLater(settleAxes)
    onYTicksChanged: Qt.callLater(settleAxes)
    readonly property real gutterLeft: yAxisShown ? theme.plotGutterLeft : 0
    readonly property real gutterBottom: xAxisShown ? theme.plotGutterBottom : 0

    // A tick's label, with the axis unit on the first tick.
    function tickLabel(ticks, index, unit) {
        // A negative tick reads with a minus sign, never a hyphen.
        const label = String(ticks[index].label).replace(/^-/, "−")
        return index === 0 && unit !== "" ? label + " " + unit : label
    }

    Item {
        id: area
        objectName: frame.theme.prefix + "PlotArea"
        x: frame.gutterLeft
        width: frame.width - frame.gutterLeft
        height: frame.height - frame.gutterBottom
    }
    // The grid under the content.
    Repeater {
        model: frame.grid ? frame.yTicks : []
        Rectangle {
            required property var modelData
            x: area.x
            y: area.y + area.height * (1 - modelData.at)
            z: -1
            width: area.width
            height: frame.theme.hairline
            color: frame.theme.plotGridInk
        }
    }
    Repeater {
        model: frame.grid ? frame.xTicks : []
        Rectangle {
            required property var modelData
            x: area.x + area.width * modelData.at
            y: area.y
            z: -1
            width: frame.theme.hairline
            height: area.height
            color: frame.theme.plotGridInk
        }
    }
    // The axes and their ticks.
    Rectangle {
        visible: frame.yAxisShown
        x: area.x
        y: area.y
        width: frame.theme.hairline
        height: area.height
        color: frame.theme.plotAxisInk
    }
    Rectangle {
        visible: frame.xAxisShown
        x: area.x
        y: area.y + area.height
        width: area.width
        height: frame.theme.hairline
        color: frame.theme.plotAxisInk
    }
    // Tick labels laid out as they are drawn. A label that would meet an
    // earlier one (a plot drawn small) is left out; the first, which carries
    // the unit, always shows.
    FontMetrics {
        id: tickMetrics
        font.family: frame.theme.plotTickFont
        font.pixelSize: frame.theme.plotTickSize
    }
    function yLabelTop(index) {
        return Math.max(0, Math.min(area.height - tickMetrics.height,
                                    area.height * (1 - yTicks[index].at) - tickMetrics.height / 2))
    }
    function xLabelLeft(index) {
        const w = tickMetrics.advanceWidth(tickLabel(xTicks, index, xUnit))
        return Math.max(0, Math.min(width - area.x - w, area.width * xTicks[index].at - w / 2))
    }
    function yLabelFits(index) {
        for (let j = 0; j < index; ++j) {
            if (Math.abs(yLabelTop(index) - yLabelTop(j)) < tickMetrics.height)
                return false
        }
        return true
    }
    function xLabelFits(index) {
        const left = xLabelLeft(index)
        const right = left + tickMetrics.advanceWidth(tickLabel(xTicks, index, xUnit))
        for (let j = 0; j < index; ++j) {
            const otherLeft = xLabelLeft(j)
            const otherRight = otherLeft + tickMetrics.advanceWidth(tickLabel(xTicks, j, xUnit))
            if (left < otherRight + theme.plotTickGap && otherLeft < right + theme.plotTickGap)
                return false
        }
        return true
    }
    Repeater {
        model: frame.yTicks.length
        Text {
            id: yLabel
            required property int index
            objectName: frame.theme.prefix + "PlotTickY:" + index
            visible: frame.yLabelFits(index)
            x: area.x - frame.theme.plotTickGap - implicitWidth
            y: area.y + frame.yLabelTop(index)
            text: frame.tickLabel(frame.yTicks, index, frame.yUnit)
            color: frame.theme.plotTickInk
            font.family: frame.theme.plotTickFont
            font.pixelSize: frame.theme.plotTickSize
        }
    }
    Repeater {
        model: frame.xTicks.length
        Text {
            required property int index
            objectName: frame.theme.prefix + "PlotTickX:" + index
            visible: frame.xLabelFits(index)
            x: area.x + frame.xLabelLeft(index)
            y: area.y + area.height + frame.theme.plotTickGap
            text: frame.tickLabel(frame.xTicks, index, frame.xUnit)
            color: frame.theme.plotTickInk
            font.family: frame.theme.plotTickFont
            font.pixelSize: frame.theme.plotTickSize
        }
    }
    // LIVE or PREVIEW, in words.
    Rectangle {
        id: chip
        objectName: frame.theme.prefix + "PlotChip"
        visible: frame.showsChip
        z: 1
        x: area.x + area.width - width - frame.theme.plotChipInset
        y: frame.theme.plotChipInset
        width: chipLabel.implicitWidth + 2 * frame.theme.plotChipPadX
        height: chipLabel.implicitHeight + 2 * frame.theme.plotChipPadY
        radius: height / 2
        color: frame.theme.plotChipFill
        border.color: frame.live ? frame.theme.plotLiveInk : frame.theme.plotPreviewInk
        Accessible.role: Accessible.StaticText
        Accessible.name: frame.live ? "Live: drawn from the running device" : "Preview: drawn from the settings"
        Text {
            id: chipLabel
            objectName: frame.theme.prefix + "PlotChipText"
            anchors.centerIn: parent
            text: frame.chipText
            color: frame.live ? frame.theme.plotLiveInk : frame.theme.plotPreviewInk
            font.pixelSize: frame.theme.plotChipSize
            font.bold: true
            font.letterSpacing: frame.theme.plotChipSpacing
        }
    }
}
