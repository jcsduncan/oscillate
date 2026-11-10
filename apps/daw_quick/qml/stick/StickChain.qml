pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// Stick's production chain, opened from a mixer strip or the kit row
// (ADR-0121): a mic group's inserts in signal order (gate, EQ, compressor,
// transient shaper, saturation) and its parallel send, or the buses (the
// parallel compressor and its return, the kit bus's glue, EQ and clip).
// Each stage has its switch in its title; its knobs dim while it is off.
Rectangle {
    id: chain
    required property var controller
    required property StickTheme theme
    // The mic group's id ("snare_top"), or "" for the buses.
    property string group: ""
    property string groupName: ""
    signal closed()

    readonly property string base: "stick.mic." + group + "."
    readonly property var sections: group !== "" ? [
        { title: "GATE", toggle: base + "gate", knobs: [
            { id: base + "gate_threshold", label: "Threshold" },
            { id: base + "gate_release", label: "Release" }] },
        { title: "EQ", toggle: base + "eq", knobs: [
            { id: base + "low_cut", label: "Low cut" },
            { id: base + "low", label: "Low" },
            { id: base + "mid_freq", label: "Mid freq" },
            { id: base + "mid", label: "Mid" },
            { id: base + "high", label: "High" },
            { id: base + "high_cut", label: "High cut" }] },
        { title: "COMPRESSOR", toggle: base + "comp", knobs: [
            { id: base + "comp_threshold", label: "Threshold" },
            { id: base + "comp_ratio", label: "Ratio" },
            { id: base + "comp_attack", label: "Attack" },
            { id: base + "comp_release", label: "Release" },
            { id: base + "comp_makeup", label: "Makeup" }] },
        { title: "TRANSIENTS", toggle: base + "shaper", knobs: [
            { id: base + "attack", label: "Attack" },
            { id: base + "sustain", label: "Sustain" }] },
        { title: "SATURATION", toggle: base + "sat", model: base + "sat_model", knobs: [
            { id: base + "sat_drive", label: "Drive" },
            { id: base + "sat_mix", label: "Mix" }] },
        { title: "PARALLEL", toggle: "", knobs: [
            { id: base + "parallel", label: "Send" }] }
    ] : [
        { title: "PARALLEL BUS", toggle: "", knobs: [
            { id: "stick.parallel.threshold", label: "Threshold" },
            { id: "stick.parallel.ratio", label: "Ratio" },
            { id: "stick.parallel.attack", label: "Attack" },
            { id: "stick.parallel.release", label: "Release" },
            { id: "stick.parallel.makeup", label: "Makeup" },
            { id: "stick.parallel.level", label: "Return" }] },
        { title: "GLUE", toggle: "stick.bus.comp", knobs: [
            { id: "stick.bus.comp_threshold", label: "Threshold" },
            { id: "stick.bus.comp_ratio", label: "Ratio" },
            { id: "stick.bus.comp_attack", label: "Attack" },
            { id: "stick.bus.comp_release", label: "Release" },
            { id: "stick.bus.comp_makeup", label: "Makeup" }] },
        { title: "EQ", toggle: "stick.bus.eq", knobs: [
            { id: "stick.bus.low_cut", label: "Low cut" },
            { id: "stick.bus.low", label: "Low" },
            { id: "stick.bus.mid_freq", label: "Mid freq" },
            { id: "stick.bus.mid", label: "Mid" },
            { id: "stick.bus.high", label: "High" },
            { id: "stick.bus.high_cut", label: "High cut" }] },
        { title: "CLIP", toggle: "stick.bus.clip", knobs: [
            { id: "stick.bus.clip_drive", label: "Drive" }] }
    ]

    objectName: "stickChain"
    radius: theme.cardRadius
    color: theme.card
    border.color: theme.tileEdge
    Accessible.role: Accessible.Pane
    Accessible.name: group !== "" ? groupName + " chain" : "Kit buses"
    Keys.onEscapePressed: chain.closed()

    Item {
        id: title
        x: chain.theme.cardPad
        y: chain.theme.cardPad
        width: chain.width - 2 * chain.theme.cardPad
        height: chain.theme.statusHeight
        Text {
            objectName: "stickChainTitle"
            anchors.verticalCenter: parent.verticalCenter
            text: (chain.group !== "" ? chain.groupName.toUpperCase() + " · CHAIN"
                                      : "KIT BUSES · PARALLEL AND KIT BUS")
            color: chain.theme.inkDim
            font.family: chain.theme.sans
            font.pixelSize: chain.theme.smallSize
            font.weight: Font.DemiBold
            font.letterSpacing: chain.theme.headingSpacing
        }
        Kit.Button {
            objectName: "stickChainClose"
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            theme: chain.theme
            small: true
            label: "Done"
            tip: "Close the chain"
            onClicked: chain.closed()
        }
    }

    Flow {
        x: chain.theme.cardPad
        y: title.y + title.height + chain.theme.titleGap
        width: chain.width - 2 * chain.theme.cardPad
        height: chain.height - y - chain.theme.cardPad
        spacing: chain.theme.tileGap
        Repeater {
            model: chain.sections
            Rectangle {
                id: section
                required property var modelData
                readonly property bool on: modelData.toggle === ""
                    || (chain.controller.revision >= 0 && chain.controller.choice(modelData.toggle) === 1)
                readonly property int columns: Math.min(modelData.knobs.length, chain.theme.chainColumns)
                objectName: "stickChainSection:" + modelData.title
                width: Math.max(columns * chain.theme.chainCell, model.width) + 2 * chain.theme.tilePad
                height: sectionTitle.height + knobs.height + 2 * chain.theme.tilePad
                        + (modelData.model !== undefined ? chain.theme.segmentHeight + chain.theme.hintGap : 0)
                radius: chain.theme.tileRadius
                color: chain.theme.caseColor
                border.color: section.on && modelData.toggle !== "" ? chain.theme.amber : chain.theme.tileEdge
                Accessible.role: Accessible.Grouping
                Accessible.name: modelData.title

                Item {
                    id: sectionTitle
                    x: chain.theme.tilePad
                    y: chain.theme.tilePad
                    width: section.width - 2 * chain.theme.tilePad
                    height: chain.theme.statusHeight
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: section.modelData.toggle === ""
                        text: section.modelData.title
                        color: chain.theme.inkDim
                        font.family: chain.theme.sans
                        font.pixelSize: chain.theme.smallSize
                        font.weight: Font.DemiBold
                    }
                    Loader {
                        anchors.verticalCenter: parent.verticalCenter
                        active: section.modelData.toggle !== ""
                        sourceComponent: Kit.Switch {
                            objectName: "stickChainSwitch:" + section.modelData.toggle
                            controller: chain.controller
                            theme: chain.theme
                            parameterId: section.modelData.toggle
                            label: section.modelData.title
                        }
                    }
                }
                Loader {
                    id: model
                    x: chain.theme.tilePad
                    y: sectionTitle.y + sectionTitle.height + chain.theme.hintGap
                    active: section.modelData.model !== undefined
                    sourceComponent: Kit.Segment {
                        objectName: "stickChainModel:" + section.modelData.model
                        controller: chain.controller
                        theme: chain.theme
                        parameterId: section.modelData.model
                        inactive: !section.on
                        inactiveReason: "Saturation is off"
                    }
                }
                Grid {
                    id: knobs
                    x: chain.theme.tilePad
                    y: (section.modelData.model !== undefined ? model.y + model.height
                                                              : sectionTitle.y + sectionTitle.height)
                       + chain.theme.hintGap
                    columns: section.columns
                    Repeater {
                        model: section.modelData.knobs
                        Item {
                            id: cell
                            required property var modelData
                            width: chain.theme.chainCell
                            height: chain.theme.chainCellHeight
                            Kit.Knob {
                                anchors.centerIn: parent
                                diameter: chain.theme.chainKnobDiameter
                                readout: true
                                controller: chain.controller
                                theme: chain.theme
                                parameterId: cell.modelData.id
                                label: cell.modelData.label
                                inactive: !section.on
                                inactiveReason: section.modelData.title.toLowerCase() + " is off"
                            }
                        }
                    }
                }
            }
        }
    }
}
