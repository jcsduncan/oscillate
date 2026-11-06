pragma ComponentBehavior: Bound
import QtQuick

// Filter routing (NOVA-UI-09), on the overview's ROUTING card:
// serial, parallel or split filters, and which filter (or none) each
// source feeds. Lit in the filters' accent.
Item {
    id: routing
    required property var panel
    readonly property color accent: panel.accentFor("nova.filter.routing")
    readonly property var sources: [{ name: "A", id: "nova.osc.a.route" }, { name: "B", id: "nova.osc.b.route" },
                                    { name: "C", id: "nova.osc.c.route" }, { name: "SUB", id: "nova.sub.route" },
                                    { name: "NOISE", id: "nova.noise.route" }]
    readonly property var columns: ["F1", "F2", "DIRECT"]
    readonly property var columnNames: ["Filter 1", "Filter 2", "Direct"]
    readonly property real segsHeight: 22
    readonly property real cellWidth: 52
    readonly property real cellHeight: 18
    readonly property real nameWidth: 48
    readonly property real gap: panel.theme.spaceSm
    readonly property real headerHeight: 12

    objectName: "novaRouting"
    implicitWidth: nameWidth + columns.length * (cellWidth + gap)
    implicitHeight: segsHeight + gap + grid.implicitHeight

    NovaSegs {
        panel: routing.panel
        parameterId: "nova.filter.routing"
        labels: ["SERIAL", "PARALLEL", "SPLIT"]
        fill: true
        width: Math.min(routing.width, routing.implicitWidth)
        height: routing.segsHeight
        fontSize: routing.panel.theme.routingSegsTextSize
        segmentPadding: routing.panel.theme.routingSegsPadding
    }

    Grid {
        id: grid
        y: routing.segsHeight + routing.gap
        columns: routing.columns.length + 1
        columnSpacing: routing.gap
        rowSpacing: routing.panel.theme.routingRowGap
        verticalItemAlignment: Grid.AlignVCenter
        Item { width: routing.nameWidth; height: routing.headerHeight }
        Repeater {
            model: routing.columns
            delegate: Text {
                required property string modelData
                width: routing.cellWidth
                horizontalAlignment: Text.AlignHCenter
                text: modelData
                color: routing.panel.theme.faint
                font.family: routing.panel.theme.sans
                font.pixelSize: routing.panel.theme.typeSmall
                font.weight: Font.ExtraBold
                font.letterSpacing: routing.panel.theme.routingHeaderSpacing
            }
        }
        Repeater {
            model: routing.sources.length * (routing.columns.length + 1)
            delegate: Item {
                id: cell
                required property int index
                readonly property int stride: routing.columns.length + 1
                readonly property var source: routing.sources[Math.floor(index / stride)]
                readonly property int column: index % stride - 1
                readonly property bool on: column >= 0 && routing.panel.controller.revision >= 0
                    && routing.panel.controller.choice(source.id) === column
                width: cell.column < 0 ? routing.nameWidth : routing.cellWidth
                height: routing.cellHeight
                Text {
                    visible: cell.column < 0
                    anchors.verticalCenter: parent.verticalCenter
                    text: cell.source.name
                    color: routing.panel.theme.text
                    font.family: routing.panel.theme.sans
                    font.pixelSize: routing.panel.theme.typeSmall
                    font.weight: Font.Bold
                }
                Rectangle {
                    visible: cell.column >= 0
                    objectName: "novaRoute:" + cell.source.id + ":" + cell.column
                    anchors.fill: parent
                    radius: routing.panel.theme.routingCellRadius
                    color: cell.on ? routing.panel.theme.soft(routing.accent) : routing.panel.theme.wellFill
                    border.color: cell.on ? routing.accent : routing.panel.theme.line2
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: cell.source.name + " to " + routing.columnNames[Math.max(0, cell.column)]
                    Accessible.checked: cell.on
                    Rectangle {
                        visible: cell.on
                        anchors.centerIn: parent
                        width: routing.panel.theme.routingDotSize
                        height: width
                        radius: width / 2
                        color: routing.accent
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: routing.panel.controller.editable
                        onClicked: {
                            routing.panel.controller.set_choice(cell.source.id, cell.column)
                            routing.panel.touchId(cell.source.id, cell.source.name + " ROUTE")
                        }
                    }
                }
            }
        }
    }
}
