pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// One module as a full-width strip (NOVA-UI-11). The header: its place in
// the chain, LED, name (click to swap the module), mode, SYNC for the
// delay, the preset (◀ name ▶; click the name for the list), collapse and
// ⋯. The body: the live display and every parameter the module uses, MIX
// last; the EQ shows its four bands as type, frequency, gain and Q.
// Collapsed, the header lists the main values instead.
Rectangle {
    id: strip
    required property var page
    required property var panel
    required property int uid
    required property string module
    required property int index
    // The scroll view's visible span, in the strips' coordinates.
    property real viewTop: 0
    property real viewBottom: 0
    readonly property var spec: Contract.module(module)
    readonly property var controller: panel.controller
    readonly property bool ready: spec !== null && controller.revision >= 0
    readonly property bool collapsed: page.collapsed[uid] === true
    readonly property bool selected: page.selected === uid
    readonly property bool on: controller.revision >= 0 && controller.choice("nova.fx." + uid + ".on") === 1
    readonly property bool inView: y + height > viewTop && y < viewBottom
    readonly property bool eq: spec !== null && spec.bands !== undefined
    readonly property var modeChoices: spec === null || spec.mode === null ? []
        : spec.mode.choices === "filterModels" ? controller.parameter("nova.filter.1.model").choices
        : spec.mode.choices
    // Read through the revision: value() alone tells a binding nothing.
    readonly property int mode: ready && controller.revision >= 0 && spec.mode !== null
        ? Contract.modeIndex(controller.value(Contract.fxParam(uid, spec.mode.p)), modeChoices.length) : 0
    readonly property bool synced: ready && controller.revision >= 0 && spec.sync !== undefined
        && controller.value(Contract.fxParam(uid, spec.sync)) >= 0.5
    readonly property string presetName: page.presetName(uid, module)

    readonly property real headerHeight: 40
    readonly property real bodyHeight: eq ? 236 : 176
    // The display's share of the body's width; fixed widths of the mode
    // menu, the preset name, the knobs, the impulse's name and the EQ's band
    // type menus.
    readonly property real displayShare: 0.42
    readonly property real displayWidth: Math.round((width - 2 * panel.theme.fxStripPad) * displayShare)
    readonly property real indexWidth: 14
    readonly property real modeWidth: 130
    readonly property real presetWidth: 150
    readonly property real knobWidth: 70
    readonly property real impulseWidth: 150
    readonly property real impulseHeight: 60
    readonly property real bandTypeWidth: 132
    readonly property real bandTypeHeight: 22
    // The knobs a collapsed strip lists.
    readonly property int summaryKnobs: 3

    function p(k) {
        return ready ? controller.value(Contract.fxParam(uid, k)) : 0
    }
    function formatterFor(knob) {
        if (knob.p === 0 && module === "delay")
            return position => Contract.delayTime(position, strip.synced)
        return knob.fmt !== undefined && knob.fmt !== null ? knob.fmt : null
    }

    objectName: "novaFxStrip:" + uid
    height: headerHeight + (collapsed ? 0 : bodyHeight)
    radius: panel.theme.fxStripRadius
    border.color: selected ? panel.theme.soft(panel.theme.pk, panel.theme.fxStripSelectedAlpha) : panel.theme.line
    gradient: Gradient {
        GradientStop { position: 0; color: strip.panel.theme.card }
        GradientStop { position: 1; color: strip.panel.theme.cardBottom }
    }
    clip: true
    Accessible.role: Accessible.Grouping
    Accessible.name: spec !== null ? spec.name : module

    // A click anywhere on the strip selects it.
    MouseArea {
        anchors.fill: parent
        onPressed: mouse => {
            strip.page.selected = strip.uid
            mouse.accepted = false
        }
    }

    // ---- Header ----
    Item {
        id: header
        width: parent.width
        height: strip.headerHeight
        Rectangle {
            visible: !strip.collapsed
            y: parent.height - strip.panel.theme.hairline
            width: parent.width
            height: strip.panel.theme.hairline
            color: strip.panel.theme.line
        }
        Row {
            x: strip.panel.theme.fxStripPad
            height: parent.height
            spacing: strip.panel.theme.fxStripHeaderGap
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: strip.indexWidth
                text: strip.index + 1
                color: strip.panel.theme.faint
                font.family: strip.panel.theme.mono
                font.pixelSize: strip.panel.theme.typeSmall
            }
            NovaLed {
                anchors.verticalCenter: parent.verticalCenter
                panel: strip.panel
                parameterId: "nova.fx." + strip.uid + ".on"
            }
            Text {
                id: name
                objectName: "novaFxModule:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                text: (strip.spec !== null ? strip.spec.name.toUpperCase() : strip.module) + "  ▾"
                color: strip.on ? strip.panel.theme.pk : strip.panel.theme.dim
                font.family: strip.panel.theme.sans
                font.pixelSize: strip.panel.theme.typeBody
                font.weight: Font.ExtraBold
                font.letterSpacing: strip.panel.theme.fxStripNameSpacing
                Accessible.role: Accessible.Button
                Accessible.name: "Swap " + (strip.spec !== null ? strip.spec.name : "") + " for another effect"
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -strip.panel.theme.fxStripNameHit
                    enabled: strip.controller.editable
                    cursorShape: Qt.PointingHandCursor
                    onClicked: strip.page.openBrowser(name, strip.index)
                }
            }
            NovaDropdown {
                visible: !strip.collapsed && strip.modeChoices.length > 0
                objectName: "novaFxMode:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                panel: strip.panel
                text: strip.modeChoices[strip.mode] !== undefined ? strip.modeChoices[strip.mode] : ""
                entries: strip.modeChoices.map((c, i) => ({ label: c, value: i }))
                currentValue: strip.mode
                width: strip.modeWidth
                onPicked: value => strip.controller.set_value(Contract.fxParam(strip.uid, strip.spec.mode.p),
                                                              Contract.modeValue(value, strip.modeChoices.length))
            }
            NovaPill {
                visible: !strip.collapsed && strip.spec !== null && strip.spec.sync !== undefined
                objectName: "novaFxSync:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                panel: strip.panel
                text: "SYNC"
                localOn: strip.synced
                onClicked: strip.controller.set_value(Contract.fxParam(strip.uid, strip.spec.sync), strip.synced ? 0 : 1)
            }
            // The preset: step with the arrows, click the name for the list.
            Rectangle {
                visible: !strip.collapsed
                anchors.verticalCenter: parent.verticalCenter
                width: presetRow.implicitWidth + 2 * strip.panel.theme.hairline
                height: strip.panel.theme.controlRowHeight
                radius: strip.panel.theme.radiusLg
                color: strip.panel.theme.wellFill
                border.color: strip.panel.theme.line2
                Row {
                    id: presetRow
                    x: strip.panel.theme.hairline
                    height: parent.height
                    PresetArrow { owner: strip; glyph: "◀"; direction: -1; objectName: "novaFxPresetPrevious:" + strip.uid }
                    Rectangle {
                        id: presetName
                        objectName: "novaFxPreset:" + strip.uid
                        width: strip.presetWidth
                        height: parent.height
                        color: "transparent"
                        border.color: strip.panel.theme.line
                        Accessible.role: Accessible.ComboBox
                        Accessible.name: "Preset"
                        Accessible.description: strip.presetName
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - strip.panel.theme.fxPresetTextRoom
                            horizontalAlignment: Text.AlignHCenter
                            text: strip.presetName
                            color: strip.presetName === "Custom" ? strip.panel.theme.dim : strip.panel.theme.text
                            font.family: strip.panel.theme.sans
                            font.pixelSize: strip.panel.theme.typeBody
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            anchors.fill: parent
                            enabled: strip.controller.editable
                            onClicked: strip.page.openPresetMenu(presetName, strip.uid, strip.module)
                        }
                    }
                    PresetArrow { owner: strip; glyph: "▶"; direction: 1; objectName: "novaFxPresetNext:" + strip.uid }
                }
            }
            // Collapsed: the main values instead.
            Row {
                visible: strip.collapsed
                objectName: "novaFxSummary:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                spacing: strip.panel.theme.fxSummaryGap
                Repeater {
                    model: strip.spec === null ? [] : strip.eq ? [] : strip.spec.knobs.slice(0, strip.summaryKnobs)
                    delegate: Text {
                        required property var modelData
                        readonly property var fmt: strip.formatterFor(modelData)
                        text: modelData.label + " " + (strip.controller.revision >= 0
                            ? (fmt !== null ? fmt(strip.p(modelData.p)) : strip.controller.display(Contract.fxParam(strip.uid, modelData.p))) : "")
                        color: strip.panel.theme.dim
                        font.family: strip.panel.theme.mono
                        font.pixelSize: strip.panel.theme.typeSmall
                    }
                }
                Text {
                    text: "MIX " + Contract.pct(strip.p("mix"))
                    color: strip.panel.theme.dim
                    font.family: strip.panel.theme.mono
                    font.pixelSize: strip.panel.theme.typeSmall
                }
                Text {
                    text: strip.presetName
                    color: strip.panel.theme.faint
                    font.family: strip.panel.theme.sans
                    font.pixelSize: strip.panel.theme.typeSmall
                }
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: strip.panel.theme.fxStripPad
            height: parent.height
            spacing: strip.panel.theme.spaceLg
            Text {
                objectName: "novaFxCollapse:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                text: strip.collapsed ? "▸" : "▾"
                color: collapseArea.containsMouse ? strip.panel.theme.text : strip.panel.theme.faint
                font.pixelSize: strip.panel.theme.typeEmphasis
                Accessible.role: Accessible.Button
                Accessible.name: strip.collapsed ? "Expand" : "Collapse"
                MouseArea {
                    id: collapseArea
                    anchors.fill: parent
                    anchors.margins: -strip.panel.theme.fxMenuHit
                    hoverEnabled: true
                    onClicked: strip.page.setCollapsed(strip.uid, !strip.collapsed)
                }
            }
            Text {
                id: kebab
                objectName: "novaFxMenu:" + strip.uid
                anchors.verticalCenter: parent.verticalCenter
                text: "⋯"
                color: kebabArea.containsMouse ? strip.panel.theme.text : strip.panel.theme.faint
                font.pixelSize: strip.panel.theme.typeHeading
                Accessible.role: Accessible.Button
                Accessible.name: "More actions"
                MouseArea {
                    id: kebabArea
                    anchors.fill: parent
                    anchors.margins: -strip.panel.theme.fxMenuHit
                    hoverEnabled: true
                    enabled: strip.controller.editable
                    onClicked: strip.page.openSlotMenu(kebab, strip.index)
                }
            }
        }
    }

    component PresetArrow: Rectangle {
        id: arrow
        required property var owner
        required property string glyph
        required property int direction
        width: arrow.owner.panel.theme.fxPresetArrowWidth
        height: parent.height
        color: "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: direction < 0 ? "Previous preset" : "Next preset"
        Text {
            anchors.centerIn: parent
            text: arrow.glyph
            color: arrowArea.containsMouse ? arrow.owner.panel.theme.text : arrow.owner.panel.theme.faint
            font.pixelSize: arrow.owner.panel.theme.typeSmall
        }
        MouseArea {
            id: arrowArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: arrow.owner.controller.editable
            onClicked: arrow.owner.page.stepPreset(arrow.owner.uid, arrow.owner.module, arrow.direction)
        }
    }

    // ---- Body ----
    Item {
        visible: !strip.collapsed
        y: strip.headerHeight
        width: parent.width
        height: strip.bodyHeight

        NovaFxDisplay {
            x: strip.panel.theme.fxStripPad
            y: strip.panel.theme.fxStripPad
            width: strip.displayWidth
            height: parent.height - 2 * strip.panel.theme.fxStripPad
            panel: strip.panel
            owner: strip
        }

        // Every parameter, MIX last. Only one of the two layouts exists, so
        // each knob is made once.
        Loader {
            id: knobs
            active: !strip.eq
            x: strip.displayWidth + strip.panel.theme.fxBodyGap
            width: parent.width - x - strip.panel.theme.fxStripPad
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Flow {
                width: knobs.width
                spacing: strip.panel.theme.spaceSm
                Repeater {
                    model: strip.spec !== null && !strip.eq ? strip.spec.knobs : []
                    delegate: NovaKnob {
                        required property var modelData
                        width: strip.knobWidth
                        panel: strip.panel
                        parameterId: Contract.fxParam(strip.uid, modelData.p)
                        label: modelData.label
                        formatter: strip.formatterFor(modelData)
                    }
                }
                NovaKnob {
                    width: strip.knobWidth
                    panel: strip.panel
                    parameterId: Contract.fxParam(strip.uid, "mix")
                    label: "MIX"
                    formatter: Contract.pct
                }
                // The convolver's impulse, by name.
                Text {
                    visible: strip.module === "convolve"
                    width: strip.impulseWidth
                    height: strip.impulseHeight
                    verticalAlignment: Text.AlignVCenter
                    text: {
                        const media = strip.panel.doc.media_revision >= 0 ? strip.panel.doc.media("fx." + strip.uid + ".ir") : null
                        return media !== null && media.kind !== "" && media.name !== "" ? "IR · " + media.name : "No impulse loaded"
                    }
                    color: strip.panel.theme.dim
                    font.family: strip.panel.theme.sans
                    font.pixelSize: strip.panel.theme.typeBody
                    wrapMode: Text.WordWrap
                }
            }
        }

        // The EQ: four band columns, two by two, then MIX.
        Loader {
            active: strip.eq
            x: strip.displayWidth + strip.panel.theme.fxBodyGap
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: Grid {
                columns: 3
                columnSpacing: strip.panel.theme.eqColumnGap
                rowSpacing: strip.panel.theme.eqRowGap
                Repeater {
                    model: strip.eq ? Contract.eqBands : 0
                    delegate: Column {
                        id: band
                        required property int index
                        readonly property int type: Contract.eqTypeIndex(strip.p(4 * index))
                        spacing: strip.panel.theme.spaceXs
                        Row {
                            spacing: strip.panel.theme.spaceSm
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: band.index + 1
                                color: strip.panel.theme.vi
                                font.family: strip.panel.theme.mono
                                font.pixelSize: strip.panel.theme.typeBody
                                font.weight: Font.Bold
                            }
                            NovaDropdown {
                                objectName: "novaFxBandType:" + strip.uid + ":" + band.index
                                panel: strip.panel
                                width: strip.bandTypeWidth
                                height: strip.bandTypeHeight
                                fontSize: strip.panel.theme.typeSmall
                                text: Contract.eqTypes[band.type]
                                entries: Contract.eqTypes.map((c, i) => ({ label: c, value: i }))
                                currentValue: band.type
                                onPicked: value => strip.controller.set_value(Contract.fxParam(strip.uid, 4 * band.index),
                                                                              Contract.choiceP(value, Contract.eqTypes.length))
                            }
                        }
                        Row {
                            spacing: strip.panel.theme.spaceXxs
                            opacity: band.type === Contract.eqType.off ? strip.panel.theme.eqBandOffOpacity : 1
                            NovaKnob {
                                panel: strip.panel
                                parameterId: Contract.fxParam(strip.uid, 4 * band.index + 1)
                                size: "sm"
                                label: "FREQ"
                                formatter: p => Contract.hz(Contract.eqHz(p))
                            }
                            NovaKnob {
                                panel: strip.panel
                                parameterId: Contract.fxParam(strip.uid, 4 * band.index + 2)
                                size: "sm"
                                label: "GAIN"
                                inactive: !Contract.eqHasGain(band.type)
                                formatter: p => Contract.db(Contract.eqDb(p))
                            }
                            NovaKnob {
                                panel: strip.panel
                                parameterId: Contract.fxParam(strip.uid, 4 * band.index + 3)
                                size: "sm"
                                label: "Q"
                                formatter: p => Contract.number(Contract.eqQ(p))
                            }
                        }
                    }
                }
                NovaKnob {
                    panel: strip.panel
                    parameterId: Contract.fxParam(strip.uid, "mix")
                    label: "MIX"
                    formatter: Contract.pct
                }
            }
        }
    }
}
