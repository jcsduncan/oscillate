pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Sixteen-bar channel-level overview. Point positions come from the shared
// tempo/meter projection; new points snap to the displayed bar grid.
Item {
    id: graph
    required property var bridge
    required property string trackId
    required property var lane
    required property color accentColor
    required property color lineColor
    required property color textColor
    property int firstBar: 0
    readonly property int visibleBars: 16
    readonly property var points: lane.points || []
    // The level scale's top: gain 4 (+12 dB) at the plot's top edge.
    readonly property real maxLevel: 4

    function barAt(x) {
        return firstBar + Math.max(0, Math.min(visibleBars - 1,
                                             Math.floor(x / Math.max(1, plot.width) * visibleBars)))
    }
    function levelAt(y) {
        return Math.max(0, Math.min(maxLevel, maxLevel * (1 - y / Math.max(1, plot.height))))
    }
    function xAt(bar) {
        return (bar - firstBar) / visibleBars * plot.width
    }
    function yAt(level) {
        return (1 - level / maxLevel) * plot.height
    }

    Text {
        id: caption
        objectName: "channelGraphCaption"
        text: "BARS " + (graph.firstBar + 1) + "–" + (graph.firstBar + graph.visibleBars)
              + "  ·  CLICK TO ADD  ·  DRAG BAR + LEVEL"
        color: graph.textColor
        font.pixelSize: Theme.typeCaption
    }
    Text {
        id: back
        objectName: "channelGraphPrevious"
        x: graph.width - 2 * Theme.automationGraphPagerStride; width: Theme.automationGraphPagerWidth
        text: "‹"; color: graph.accentColor; font.pixelSize: Theme.typeTitle
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Previous 16 automation bars"
        function activate() { graph.firstBar = Math.max(0, graph.firstBar - graph.visibleBars) }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        MouseArea {
            anchors.fill: parent
            onClicked: back.activate()
            Tip {}
        }
    }
    Text {
        id: forward
        objectName: "channelGraphNext"
        x: graph.width - Theme.automationGraphPagerStride; width: Theme.automationGraphPagerWidth
        text: "›"; color: graph.accentColor; font.pixelSize: Theme.typeTitle
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: "Next 16 automation bars"
        function activate() { graph.firstBar = Math.min(65520, graph.firstBar + graph.visibleBars) }
        Keys.onReturnPressed: activate()
        Keys.onSpacePressed: activate()
        MouseArea {
            anchors.fill: parent
            onClicked: forward.activate()
            Tip {}
        }
    }

    Rectangle {
        id: plot
        objectName: "channelGraphPlot"
        x: 0; y: Theme.automationGraphPlotTop; width: graph.width; height: Math.max(1, graph.height - Theme.automationGraphPlotTop)
        color: Theme.automationGraphGround
        border.color: graph.lineColor
        clip: true

        Canvas {
            anchors.fill: parent
            property var drawnPoints: graph.points
            property int page: graph.firstBar
            onDrawnPointsChanged: requestPaint()
            onPageChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const painter = getContext("2d")
                painter.clearRect(0, 0, width, height)
                painter.strokeStyle = graph.lineColor
                painter.lineWidth = Theme.hairline
                for (let bar = 1; bar < graph.visibleBars; ++bar) {
                    const x = bar / graph.visibleBars * width
                    painter.beginPath(); painter.moveTo(x, 0); painter.lineTo(x, height); painter.stroke()
                }
                painter.strokeStyle = graph.accentColor
                painter.lineWidth = Theme.strokeBold
                const ordered = drawnPoints.filter(point => Number.isFinite(point.graphBar) &&
                                                           point.graphBar >= 0)
                                           .sort((left, right) => left.graphBar - right.graphBar)
                // Show the exact saved breakpoints. Segment interpolation is
                // edited in the point list, and is not approximated here when
                // tempo ramps or meter changes alter the bar-axis spacing.
                for (const point of ordered) {
                    const x = graph.xAt(point.graphBar)
                    if (x < 0 || x >= width) continue
                    painter.beginPath()
                    painter.moveTo(x, graph.yAt(point.value))
                    painter.lineTo(x, height)
                    painter.stroke()
                }
            }
        }
        MouseArea {
            id: addSurface
            objectName: "channelGraphAddSurface"
            anchors.fill: parent
            enabled: graph.bridge.audioEditEnabled && graph.trackId.length > 0
            cursorShape: Qt.CrossCursor
            activeFocusOnTab: enabled
            Accessible.role: Accessible.Button
            Accessible.name: "Add channel automation point on 16 bar grid"
            function addAt(x, y) {
                graph.bridge.trackAutomation.set_point(graph.trackId, graph.barAt(x),
                                                       graph.levelAt(y))
            }
            Keys.onReturnPressed: addAt(width / 2, height / 2)
            Keys.onSpacePressed: addAt(width / 2, height / 2)
            onClicked: mouse => addAt(mouse.x, mouse.y)
        }
        Repeater {
            model: graph.points
            delegate: Rectangle {
                id: handle
                required property var modelData
                objectName: "channelGraphPointHandle"
                property real previewLevel: modelData.value
                property real previewBar: modelData.graphBar
                visible: Number.isFinite(modelData.graphBar) &&
                         modelData.graphBar >= graph.firstBar &&
                         modelData.graphBar < graph.firstBar + graph.visibleBars
                x: graph.xAt(previewBar) - width / 2
                y: graph.yAt(previewLevel) - height / 2
                width: Theme.automationGraphHandleSize; height: Theme.automationGraphHandleSize; radius: Theme.automationGraphHandleRadius
                color: graph.accentColor
                border.color: graph.lineColor
                z: 2
                activeFocusOnTab: graph.bridge.audioEditEnabled
                Accessible.role: Accessible.Slider
                Accessible.name: "Channel automation point at " + modelData.position
                Accessible.description: "Drag horizontally to move by bar and vertically to change level; use arrow keys"
                Keys.onUpPressed: graph.bridge.trackAutomation.set_value(
                    graph.trackId, modelData.id, Math.min(graph.maxLevel, modelData.value + 0.05))
                Keys.onDownPressed: graph.bridge.trackAutomation.set_value(
                    graph.trackId, modelData.id, Math.max(0, modelData.value - 0.05))
                Keys.onLeftPressed: graph.bridge.trackAutomation.move_point(
                    graph.trackId, modelData.id, Math.max(0, Math.round(modelData.graphBar) - 1),
                    modelData.value)
                Keys.onRightPressed: graph.bridge.trackAutomation.move_point(
                    graph.trackId, modelData.id, Math.min(65535, Math.round(modelData.graphBar) + 1),
                    modelData.value)
                Keys.onDeletePressed: graph.bridge.trackAutomation.remove_point(
                    graph.trackId, modelData.id)
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -Theme.automationHandleHitOutset
                    enabled: graph.bridge.audioEditEnabled
                    cursorShape: Qt.SizeAllCursor
                    property real pressX: 0
                    onPressed: mouse => {
                        handle.forceActiveFocus()
                        pressX = mapToItem(plot, mouse.x, mouse.y).x
                    }
                    onPositionChanged: mouse => {
                        if (!pressed) return
                        const position = mapToItem(plot, mouse.x, mouse.y)
                        handle.previewLevel = graph.levelAt(position.y)
                        const barWidth = plot.width / graph.visibleBars
                        const delta = position.x - pressX
                        handle.previewBar = Math.abs(delta) < barWidth * 0.6
                                            ? handle.modelData.graphBar
                                            : Math.max(0, Math.min(65535,
                                                Math.round(handle.modelData.graphBar + delta / barWidth)))
                    }
                    onReleased: {
                        if (Math.abs(handle.previewBar - handle.modelData.graphBar) > 0.000001)
                            graph.bridge.trackAutomation.move_point(
                                graph.trackId, handle.modelData.id, handle.previewBar,
                                handle.previewLevel)
                        else if (Math.abs(handle.previewLevel - handle.modelData.value) > 0.000001)
                            graph.bridge.trackAutomation.set_value(
                                graph.trackId, handle.modelData.id, handle.previewLevel)
                        handle.previewBar = Qt.binding(() => handle.modelData.graphBar)
                        handle.previewLevel = Qt.binding(() => handle.modelData.value)
                    }
                    onCanceled: {
                        handle.previewBar = Qt.binding(() => handle.modelData.graphBar)
                        handle.previewLevel = Qt.binding(() => handle.modelData.value)
                    }
                }
            }
        }
    }
}
