import QtQuick

// The header's readout: the control last touched (theme.lastTouched), its
// group and name over its value, in a box of its own beside the preset
// browser. Hidden while no control has been touched, so the header never
// shows an idle prompt. A panel may name a control its own way by setting
// `nameOf(id)` and `valueOf(id)`.
Rectangle {
    id: readout
    required property var controller
    required property KitTheme theme
    property var nameOf: null
    property var valueOf: null
    readonly property string parameterId: theme.lastTouched
    readonly property var info: parameterId !== "" ? controller.parameter(parameterId) : ({})
    readonly property string name: parameterId === "" ? ""
        : nameOf ? nameOf(parameterId)
        : info.name === undefined ? ""
        : (info.group !== undefined && info.group !== "" && info.group !== info.name
           ? info.group + " · " + info.name : info.name).toUpperCase()
    readonly property string value: parameterId === "" || controller.revision < 0 ? ""
        : valueOf ? valueOf(parameterId) : controller.display(parameterId)
    // Shown only once a control has been touched.
    readonly property bool idle: name === ""

    objectName: theme.prefix + "ReadoutBox"
    visible: !idle
    width: theme.readoutWidth
    height: theme.presetDisplayHeight
    radius: theme.readoutRadius
    color: theme.readoutFill
    border.color: theme.readoutEdge
    Accessible.role: Accessible.StaticText
    Accessible.name: name + " " + value
    Text {
        objectName: readout.theme.prefix + "Readout"
        x: readout.theme.readoutTextX
        y: readout.theme.readoutNameY
        width: readout.width - 2 * x
        text: readout.name
        color: readout.theme.readoutNameInk
        font.pixelSize: readout.theme.readoutNameSize
        font.bold: true
        font.letterSpacing: readout.theme.readoutSpacing
        elide: Text.ElideRight
    }
    Text {
        objectName: readout.theme.prefix + "ReadoutValue"
        x: readout.theme.readoutTextX
        y: readout.theme.readoutValueY
        width: readout.width - 2 * x
        text: readout.value
        color: readout.theme.readoutValueInk
        font.family: readout.theme.readoutValueFont
        font.pixelSize: readout.theme.readoutValueSize
        font.bold: readout.theme.readoutValueBold
        elide: Text.ElideRight
    }
}
