pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Item {
    id: lane
    required property int barCount
    Canvas {
        anchors.fill: parent
        readonly property int bars: lane.barCount
        readonly property int beatsPerBar: 4
        onBarsChanged: requestPaint()
        onWidthChanged: requestPaint()
        onPaint: {
            const context = getContext("2d")
            context.clearRect(0, 0, width, height)
            context.fillStyle = Theme.creationLaneGround
            context.fillRect(0, 0, width, height)
            for (let beat = 0; beat < bars * beatsPerBar; ++beat) {
                const x = Math.floor(beat * width / (bars * beatsPerBar))
                context.fillStyle = beat % beatsPerBar === 0 ? Theme.creationLaneBarLine : Theme.creationLaneBeatLine
                context.fillRect(x, 0, Theme.hairline, height)
            }
        }
    }
}
