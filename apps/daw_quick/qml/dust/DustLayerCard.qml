pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

// One layer on Dust's shelf: its glyph and on/off switch, a level fader,
// Tone and Width, the layer's own knobs and (where it has one) its choice
// across the foot. An off layer dims but stays editable, so it can be set
// up before it is heard.
Item {
    id: card
    required property var controller
    required property DustTheme theme
    // { key, title, knobs: [suffix], choice: suffix or "" } (see DustPanel).
    required property var spec
    readonly property string key: spec.key
    readonly property string base: "dust." + key + "."
    readonly property color tint: theme.layerTint[key]
    readonly property bool on: controller.revision >= 0 && controller.choice(base + "on") === 1
    readonly property bool hasChoice: spec.choice !== ""
    readonly property var knobIds: ["tone", "width"].concat(spec.knobs).map(s => base + s)
    readonly property real pad: theme.spaceMd
    readonly property real headerHeight: theme.layerHeaderHeight
    readonly property real faderWidth: theme.layerFaderWidth
    readonly property real footHeight: hasChoice ? theme.segmentHeight + pad : 0

    objectName: "dustLayer:" + key
    Accessible.role: Accessible.Grouping
    Accessible.name: spec.title

    Kit.Card {
        anchors.fill: parent
        theme: card.theme
        title: ""
        border.color: card.on ? Qt.rgba(card.tint.r, card.tint.g, card.tint.b, card.theme.layerEdgeAlpha) : card.theme.plateEdge
    }
    DustGlyph {
        x: card.pad + card.theme.spaceXxs
        y: card.theme.layerGlyphTop
        width: card.theme.layerGlyphSize
        height: card.theme.layerGlyphSize
        theme: card.theme
        kind: card.key
        color: card.on ? card.tint : card.theme.inkFaint
    }
    Kit.Switch {
        x: card.pad + card.theme.layerSwitchX
        y: card.theme.spaceMd
        controller: card.controller
        theme: card.theme
        parameterId: card.base + "on"
        label: card.spec.title.toUpperCase()
        accent: card.tint
    }

    Item {
        id: body
        y: card.headerHeight
        width: card.width
        height: card.height - card.headerHeight
        opacity: card.on ? 1 : card.theme.offOpacity

        Kit.Fader {
            x: card.pad
            width: card.faderWidth
            height: body.height - card.footHeight - card.pad
            controller: card.controller
            theme: card.theme
            parameterId: card.base + "level"
            accent: card.tint
        }
        Grid {
            x: card.pad + card.faderWidth + card.theme.spaceXs
            y: -card.theme.spaceXxs
            columns: 2
            // Room between neighbouring labels, so two long ones never meet.
            columnSpacing: card.theme.spaceSm
            Repeater {
                model: card.knobIds
                delegate: Kit.Knob {
                    required property string modelData
                    controller: card.controller
                    theme: card.theme
                    parameterId: modelData
                    accent: card.tint
                    // A shorter name from the layer's spec, else the table's.
                    readonly property string shortName: card.spec.labels
                        ? card.spec.labels[modelData.slice(card.base.length)] || "" : ""
                    label: shortName.length > 0 ? shortName : (info.name !== undefined ? info.name : "")
                }
            }
        }
        Loader {
            active: card.hasChoice
            x: card.pad
            y: body.height - card.footHeight
            width: card.width - 2 * card.pad
            height: card.theme.segmentHeight
            sourceComponent: Kit.Segment {
                controller: card.controller
                theme: card.theme
                parameterId: card.base + card.spec.choice
                accent: card.tint
                labels: card.spec.choiceLabels || []
            }
        }
    }
}
