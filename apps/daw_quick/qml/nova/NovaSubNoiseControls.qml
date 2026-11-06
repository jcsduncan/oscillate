pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// Sub and Noise's controls (NOVA-OSC-10/11) on the SUB · NOISE overview
// card, under its display. Each part has its own on LED: the sub with its
// shape, the noise with its sample. The PRIMARY knobs at full size: the
// sub's level and octave, the noise's level and its AM amount (the noise
// level following an oscillator, 08a §2.12c); under a rule the SECONDARY
// controls small: the noise's pitch, tone and pan, the sub's pan, the AM
// curve with the oscillator it follows, KEYTRACK and ONE-SHOT. Lit in the
// module's green.
Item {
    id: controls
    required property var panel
    readonly property var noise: panel.doc.media_revision >= 0 ? panel.doc.media("noise") : ({})
    readonly property string noiseName: noise.kind === "builtin" && noise.name !== "" ? noise.name : "white"
    readonly property color accent: panel.accentFor("nova.noise.on")
    // The noise AM knobs dim while it follows no oscillator.
    readonly property bool amOff: panel.controller.revision >= 0 && panel.controller.choice("nova.noise.am.source") === 0
    readonly property real sectionGap: panel.theme.spaceMd
    readonly property real knobGap: panel.theme.spaceMd
    readonly property real groupGap: panel.theme.knobGroupGap
    readonly property real partGap: panel.theme.spaceMd
    readonly property real rowHeight: panel.theme.controlRowHeight
    readonly property real noiseWidth: 200
    readonly property real segmentPadding: panel.theme.compactSegsPadding
    readonly property real ruleHeight: panel.theme.hairline

    objectName: "novaSubNoise"
    implicitHeight: column.implicitHeight

    // A part's on LED and name.
    component Header: Row {
        id: header
        required property string parameterId
        required property string text
        spacing: controls.partGap
        height: controls.rowHeight
        NovaLed {
            anchors.verticalCenter: parent.verticalCenter
            panel: controls.panel
            parameterId: header.parameterId
        }
        NovaHeading {
            anchors.verticalCenter: parent.verticalCenter
            panel: controls.panel
            text: header.text
            color: controls.panel.theme.text
        }
    }
    component Knob: NovaKnob {
        panel: controls.panel
    }
    component Small: NovaKnob {
        panel: controls.panel
        size: "sm"
    }

    Column {
        id: column
        width: controls.width
        spacing: controls.sectionGap

        Flow {
            width: column.width
            spacing: controls.partGap
            Header {
                parameterId: "nova.sub.on"
                text: "SUB"
            }
            NovaSegs {
                panel: controls.panel
                parameterId: "nova.sub.shape"
                labels: ["SIN", "TRI", "SAW", "SQ"]
                height: controls.rowHeight
                segmentPadding: controls.segmentPadding
            }
        }
        Flow {
            width: column.width
            spacing: controls.partGap
            Header {
                id: noiseHeader
                parameterId: "nova.noise.on"
                text: "NOISE"
            }
            NovaDropdown {
                objectName: "novaNoise"
                panel: controls.panel
                accent: controls.accent
                text: Contract.mediaName(Contract.noises, controls.noiseName, "White")
                entries: Contract.noises.map(n => ({ label: n.name, value: n.id }))
                currentValue: controls.noiseName
                width: Math.min(controls.noiseWidth, column.width - noiseHeader.width - controls.partGap)
                height: controls.rowHeight
                onPicked: value => controls.panel.doc.set_builtin_media("noise", value, 0)
            }
        }

        // PRIMARY.
        Flow {
            width: column.width
            spacing: controls.knobGap
            Knob { parameterId: "nova.sub.level"; label: "SUB" }
            Knob { parameterId: "nova.sub.octave"; label: "OCT" }
            Knob { parameterId: "nova.noise.level"; label: "NOISE" }
            Knob { parameterId: "nova.noise.am.amount"; label: "AM"; inactive: controls.amOff }
        }

        Rectangle {
            width: column.width
            height: controls.ruleHeight
            color: controls.panel.theme.line
        }

        // SECONDARY. Noise AM: the level follows an oscillator of the same
        // voice at audio rate; Curve below zero smooths it, above zero gates
        // it buzzier.
        Flow {
            width: column.width
            spacing: controls.groupGap
            NovaKnobGroup {
                panel: controls.panel
                label: "NOISE"
                Small { parameterId: "nova.noise.pitch"; label: "PITCH" }
                Small { parameterId: "nova.noise.tone"; label: "TONE" }
                Small { parameterId: "nova.noise.pan"; label: "PAN" }
            }
            NovaKnobGroup {
                panel: controls.panel
                label: "SUB"
                Small { parameterId: "nova.sub.pan"; label: "PAN" }
            }
            NovaKnobGroup {
                panel: controls.panel
                label: "AM"
                Small { parameterId: "nova.noise.am.curve"; label: "CURVE"; inactive: controls.amOff }
                NovaSegs {
                    anchors.verticalCenter: parent.verticalCenter
                    panel: controls.panel
                    parameterId: "nova.noise.am.source"
                    labels: Contract.noiseAmLabels
                    height: controls.rowHeight
                    segmentPadding: controls.segmentPadding
                }
            }
            NovaPill {
                panel: controls.panel
                text: "KEYTRACK"
                parameterId: "nova.noise.keytrack"
                height: controls.rowHeight
            }
            NovaPill {
                panel: controls.panel
                text: "ONE-SHOT"
                parameterId: "nova.noise.mode"
                height: controls.rowHeight
            }
        }
    }
}
