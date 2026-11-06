pragma ComponentBehavior: Bound
import QtQuick

// Joined segment buttons. Bound to a choice parameter they pick its value
// (labels default to its choices); unbound they show `labels` with
// `current` and report `picked`.
Rectangle {
    id: segs
    required property var panel
    property string parameterId: ""
    property var labels: []
    property int current: 0
    property real fontSize: panel.theme.typeSmall
    property real segmentPadding: panel.theme.segsPadding
    property color accent: parameterId !== "" ? panel.accentFor(parameterId) : panel.theme.cy
    // Share the given width equally instead of sizing to the labels.
    property bool fill: false
    readonly property var info: parameterId !== "" ? panel.controller.parameter(parameterId) : ({})
    readonly property var shown: labels.length > 0 ? labels : (info.choices !== undefined ? info.choices : [])
    readonly property int selected: parameterId !== ""
        ? (panel.controller.revision >= 0 ? panel.controller.choice(parameterId) : 0) : current
    signal picked(int index)

    objectName: "novaSegs:" + (parameterId !== "" ? parameterId : shown.join("|"))
    implicitWidth: fill ? panel.theme.segsFillWidth : row.implicitWidth + 2 * panel.theme.hairline
    implicitHeight: panel.theme.segsHeight
    radius: panel.theme.radiusLg
    color: panel.theme.wellFill
    border.color: panel.theme.line2
    clip: true
    Accessible.role: Accessible.PageTabList

    function pick(index) {
        if (parameterId !== "") {
            if (!panel.controller.editable)
                return
            panel.controller.set_choice(parameterId, index)
            panel.touchId(parameterId, (info.name || "").toUpperCase())
        }
        segs.picked(index)
    }

    Row {
        id: row
        x: segs.panel.theme.hairline
        y: segs.panel.theme.hairline
        height: segs.height - 2 * segs.panel.theme.hairline
        Repeater {
            model: segs.shown.length
            delegate: Rectangle {
                id: tile
                required property int index
                readonly property bool on: index === segs.selected
                objectName: segs.objectName + ":" + index
                width: segs.fill ? (segs.width - 2 * segs.panel.theme.hairline) / Math.max(1, segs.shown.length)
                                 : label.implicitWidth + 2 * segs.segmentPadding
                height: row.height
                color: on ? segs.panel.theme.tab : "transparent"
                Accessible.role: Accessible.PageTab
                Accessible.name: segs.shown[index]
                Accessible.checked: on
                Rectangle {
                    visible: tile.index > 0
                    width: segs.panel.theme.hairline
                    height: parent.height
                    color: segs.panel.theme.line
                }
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: segs.shown[tile.index]
                    color: tile.on ? segs.accent : segs.panel.theme.dim
                    font.family: segs.panel.theme.sans
                    font.pixelSize: segs.fontSize
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: segs.pick(tile.index)
                }
            }
        }
    }
}
