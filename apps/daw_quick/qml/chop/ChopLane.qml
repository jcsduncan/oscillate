pragma ComponentBehavior: Bound
import QtQuick
import "ChopGate.js" as Gate

// One lane of per-step values (chop.step.<n>.<field>): a cell per step,
// drawn by dragging across it. A press starts one gesture and the release
// ends it, so a whole stroke is one undo step; a stroke that skips cells
// fills the ones between. Stepped fields (ratchet, repeat) draw blocks.
Item {
    id: lane
    required property var controller
    required property ChopTheme theme
    required property string field
    required property string title
    required property string caption
    required property color tint
    required property int stepCount
    // The pattern step playing now (-1: none), and whether each step plays
    // this time round (chance), from the telemetry.
    property int playingStep: -1
    property var played: []

    readonly property var info: controller.parameter(Gate.stepId(0, field))
    readonly property bool stepped: info.discrete === true
    readonly property real minimum: info.minimum !== undefined ? info.minimum : 0
    readonly property real maximum: info.maximum !== undefined ? info.maximum : 1
    readonly property int revision: controller.revision
    readonly property real nameWidth: theme.laneNameWidth
    readonly property real cellGap: theme.laneCellGap

    // The stroke in progress: the gesture's id, and the last cell painted.
    property string gestureId: ""
    property int lastStep: -1
    property real lastValue: 0

    objectName: "chopLane:" + field
    Accessible.role: Accessible.Grouping
    Accessible.name: title + " lane"

    function valueAt(step) {
        return revision >= 0 ? controller.value(Gate.stepId(step, field)) : 0
    }
    function fraction(value) {
        return maximum > minimum ? (value - minimum) / (maximum - minimum) : 0
    }
    function valueFromY(y) {
        const f = Math.max(0, Math.min(1, 1 - y / cells.height))
        const v = minimum + f * (maximum - minimum)
        return stepped ? Math.round(v) : v
    }
    function stepFromX(x) {
        return Math.max(0, Math.min(stepCount - 1, Math.floor(x / cells.width * stepCount)))
    }
    // Paints the cells from the last one to `step`, the values in between
    // following a straight line, as one value set.
    function paintTo(step, value) {
        const values = {}
        if (lastStep < 0 || lastStep === step) {
            values[Gate.stepId(step, field)] = value
        } else {
            const direction = step > lastStep ? 1 : -1
            for (let s = lastStep + direction; s !== step + direction; s += direction) {
                const t = (s - lastStep) / (step - lastStep)
                const v = lastValue + t * (value - lastValue)
                values[Gate.stepId(s, field)] = stepped ? Math.round(v) : v
            }
        }
        lastStep = step
        lastValue = value
        theme.lastTouched = Gate.stepId(step, field)
        controller.set_values(values)
    }

    Column {
        width: lane.nameWidth - lane.theme.laneNamePad
        anchors.verticalCenter: parent.verticalCenter
        spacing: lane.theme.spaceXxs
        Text {
            text: lane.title.toUpperCase()
            color: lane.theme.inkDim
            font.family: lane.theme.sans
            font.pixelSize: lane.theme.typeBody
            font.bold: true
            font.letterSpacing: lane.theme.laneTitleSpacing
        }
        Text {
            text: lane.caption
            color: lane.theme.inkFaint
            font.family: lane.theme.sans
            font.pixelSize: lane.theme.typeSmall
        }
    }

    Rectangle {
        id: cells
        x: lane.nameWidth
        width: lane.width - lane.nameWidth
        height: lane.height
        radius: lane.theme.laneRadius
        color: lane.theme.well
        border.color: lane.theme.wellEdge
        clip: true

        Repeater {
            model: lane.stepCount
            delegate: Item {
                id: cell
                required property int index
                readonly property string parameterId: Gate.stepId(index, lane.field)
                readonly property real value: lane.valueAt(index)
                readonly property real level: lane.fraction(value)
                readonly property bool silent: lane.played.length > index && lane.played[index] === false
                objectName: "chopStep:" + parameterId
                x: index * cells.width / lane.stepCount
                width: cells.width / lane.stepCount
                height: cells.height
                Accessible.role: Accessible.Slider
                Accessible.name: "Step " + (index + 1) + " " + lane.title
                Accessible.description: lane.revision >= 0 ? lane.controller.display(parameterId) : ""

                Rectangle {
                    x: lane.theme.cellInset
                    y: lane.theme.cellInset
                    width: parent.width - lane.cellGap - lane.theme.cellInset
                    height: parent.height - 2 * lane.theme.cellInset
                    color: cell.index % Gate.beatSteps === 0 ? lane.theme.cellBeat : lane.theme.cellEven
                    border.color: cell.index === lane.playingStep ? lane.theme.ink : "transparent"
                    opacity: cell.silent ? lane.theme.silentOpacity : 1
                }
                // Continuous: a wash up to the value with a bright top edge.
                Rectangle {
                    visible: !lane.stepped
                    x: lane.theme.washInset
                    width: parent.width - 2 * lane.theme.washInset - lane.cellGap
                    height: cell.level * (parent.height - 2 * lane.theme.washInset)
                    y: parent.height - height - lane.theme.cellInset
                    color: Qt.rgba(lane.tint.r, lane.tint.g, lane.tint.b, lane.theme.washAlpha)
                    Rectangle {
                        width: parent.width
                        height: lane.theme.washEdge
                        color: lane.tint
                    }
                }
                // Stepped: a block per count; repeat "off" is a dash.
                Repeater {
                    model: lane.stepped ? Math.round(cell.value) : 0
                    delegate: Rectangle {
                        required property int index
                        readonly property real block: (cell.height - lane.theme.blockColumnPad) / lane.maximum
                        x: lane.theme.blockInset
                        width: cell.width - 2 * lane.theme.blockInset - lane.cellGap
                        height: block - lane.theme.blockGap
                        y: cell.height - lane.theme.blockFloor - (index + 1) * block + lane.theme.blockGap
                        radius: lane.theme.radiusXs
                        color: lane.tint
                    }
                }
                Rectangle {
                    visible: lane.stepped && Math.round(cell.value) === 0
                    x: lane.theme.blockInset
                    y: cell.height - lane.theme.blockFloor
                    width: cell.width - 2 * lane.theme.blockInset - lane.cellGap
                    height: lane.theme.repeatOffHeight
                    color: lane.theme.gridStrong
                }
            }
        }

        MouseArea {
            id: area
            anchors.fill: parent
            enabled: lane.controller.editable
            preventStealing: true
            cursorShape: Qt.CrossCursor
            onPressed: mouse => {
                const step = lane.stepFromX(mouse.x)
                lane.gestureId = Gate.stepId(step, lane.field)
                lane.lastStep = -1
                lane.controller.begin_gesture(lane.gestureId)
                lane.paintTo(step, lane.valueFromY(mouse.y))
            }
            onPositionChanged: mouse => {
                if (pressed)
                    lane.paintTo(lane.stepFromX(mouse.x), lane.valueFromY(mouse.y))
            }
            onReleased: {
                lane.controller.end_gesture(lane.gestureId)
                lane.gestureId = ""
                lane.lastStep = -1
            }
            onCanceled: {
                lane.controller.cancel_gesture(lane.gestureId)
                lane.gestureId = ""
                lane.lastStep = -1
            }
        }
    }
}
