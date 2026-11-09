pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// One main oscillator's controls (NOVA-UI-03) on its overview card, under
// its display (NovaOscDisplay); the card's header holds the on LED and the
// type. First what it plays (the table with its arrows, SPECTRAL and
// ✎ EDIT, the classic shape, the sample or the spectral source); then the
// PRIMARY knobs at full size: LEVEL, the type's main knobs and PAN, a warp
// slot's mode under its amount; then, under a rule, the SECONDARY knobs
// small, in labelled groups: UNISON (with its curve and stack), the type's
// own extras, PITCH and MORE. Every row flows to the card's width.
Item {
    id: card
    required property var panel
    required property string osc
    readonly property string base: "nova.osc." + osc
    readonly property var controller: panel.controller
    readonly property color accent: panel.accentFor(base)
    readonly property int type: controller.revision >= 0 ? controller.choice(base + ".type") : 1
    readonly property bool wavetable: type === 1
    readonly property bool classic: type === 0
    // Pulse width shapes only the Pulse wave (the classic_shapes choice index).
    readonly property int pulseShape: 4
    readonly property bool pulse: classic && controller.revision >= 0
                                  && controller.choice(base + ".shape") === pulseShape
    readonly property bool sampled: type === 2 || type === 3
    readonly property bool granular: type === 4
    readonly property bool spectral: type === 5
    readonly property string mediaSlot: "osc." + osc
    readonly property string table: panel.oscTable(osc)
    // Warp modes from this index on read another oscillator (FM, PM, AM, RM).
    readonly property int crossWarpFirst: 12

    readonly property real sectionGap: panel.theme.spaceMd
    readonly property real knobGap: panel.theme.spaceMd
    readonly property real groupGap: panel.theme.knobGroupGap
    readonly property real stackGap: panel.theme.knobStackGap
    readonly property real sourceGap: panel.theme.spaceSm
    // The gaps the table row leaves beside its picker: two inside the
    // arrows' row, two before SPECTRAL and ✎ EDIT, and one spare so a
    // rounding pixel never wraps the row.
    readonly property int sourceSlots: 5
    readonly property real rowHeight: panel.theme.controlRowHeight
    readonly property real tableArrowWidth: 24
    readonly property real tablePickerMinimum: 150
    readonly property real granularPickerWidth: 160
    readonly property real windowWidth: 96
    readonly property real loopWidth: 104
    readonly property real fftWidth: 84
    readonly property real modeWidth: 72
    readonly property real modeHeight: 20
    readonly property real modeFontSize: panel.theme.typeSmall
    readonly property real unisonCurveWidth: 84
    readonly property real unisonStackWidth: 96
    readonly property real ruleHeight: panel.theme.hairline

    objectName: "novaOsc:" + osc
    implicitHeight: column.implicitHeight

    component Knob: NovaKnob {
        panel: card.panel
    }
    component Small: NovaKnob {
        panel: card.panel
        size: "sm"
    }
    component Group: NovaKnobGroup {
        panel: card.panel
    }
    component Mode: NovaDropdown {
        panel: card.panel
        width: card.modeWidth
        height: card.modeHeight
        fontSize: card.modeFontSize
    }
    // The primary row: LEVEL, a type's own knobs, PAN.
    component Primary: Flow {
        width: column.width
        spacing: card.knobGap
    }
    // A warp slot: its amount, with its mode (and the oscillator a cross
    // mode reads) under it.
    component Warp: Column {
        id: warp
        required property int slot
        property string size: ""
        readonly property string key: card.base + ".warp." + slot
        readonly property bool cross: card.controller.revision >= 0
            && card.controller.choice(warp.key + ".mode") >= card.crossWarpFirst
        spacing: card.stackGap
        NovaKnob {
            anchors.horizontalCenter: parent.horizontalCenter
            panel: card.panel
            size: warp.size
            parameterId: warp.key + ".amount"
            label: "WARP " + warp.slot
            inactive: card.controller.revision >= 0 && card.controller.choice(warp.key + ".mode") === 0
            inactiveReason: inactive ? "Warp " + warp.slot + " is Off" : ""
        }
        Mode {
            anchors.horizontalCenter: parent.horizontalCenter
            parameterId: warp.key + ".mode"
        }
        Mode {
            visible: warp.cross
            anchors.horizontalCenter: parent.horizontalCenter
            parameterId: warp.key + ".source"
        }
    }

    Column {
        id: column
        width: card.width
        spacing: card.sectionGap

        // What the oscillator plays.
        Item {
            id: sourceRow
            width: column.width
            height: sourceFlow.visible ? sourceFlow.height : card.rowHeight

            Flow {
                id: sourceFlow
                visible: card.wavetable || card.granular
                width: sourceRow.width
                spacing: card.sourceGap
                Row {
                    spacing: card.sourceGap
                    NovaPill {
                        visible: card.wavetable
                        objectName: "novaTablePrev:" + card.osc
                        panel: card.panel
                        accent: card.accent
                        text: "‹"
                        width: card.tableArrowWidth
                        height: card.rowHeight
                        Accessible.name: "Previous wavetable"
                        onClicked: card.panel.doc.set_builtin_media(card.mediaSlot,
                                                                    Contract.stepTable(card.table, -1), Contract.tableFrameLength)
                    }
                    NovaDropdown {
                        objectName: "novaTable:" + card.osc
                        panel: card.panel
                        accent: card.accent
                        text: Contract.mediaName(Contract.tables, card.table, "Basic Shapes")
                        entries: Contract.tables.map(t => ({ label: t.name, value: t.id }))
                        currentValue: card.table
                        width: card.granular ? card.granularPickerWidth
                            : Math.max(card.tablePickerMinimum,
                                       sourceRow.width - spectralPill.width - editPill.width
                                       - 2 * card.tableArrowWidth - card.sourceSlots * card.sourceGap)
                        height: card.rowHeight
                        onPicked: value => card.panel.doc.set_builtin_media(card.mediaSlot, value, Contract.tableFrameLength)
                    }
                    NovaPill {
                        visible: card.wavetable
                        objectName: "novaTableNext:" + card.osc
                        panel: card.panel
                        accent: card.accent
                        text: "›"
                        width: card.tableArrowWidth
                        height: card.rowHeight
                        Accessible.name: "Next wavetable"
                        onClicked: card.panel.doc.set_builtin_media(card.mediaSlot,
                                                                    Contract.stepTable(card.table, 1), Contract.tableFrameLength)
                    }
                }
                NovaPill {
                    id: spectralPill
                    visible: card.wavetable
                    panel: card.panel
                    text: "SPECTRAL"
                    height: card.rowHeight
                    parameterId: card.base + ".interpolation"
                }
                NovaPill {
                    id: editPill
                    visible: card.wavetable
                    objectName: "novaEdit:" + card.osc
                    panel: card.panel
                    accent: card.accent
                    text: "✎ EDIT"
                    height: card.rowHeight
                    onClicked: card.panel.openEditor(card.osc)
                }
                NovaDropdown {
                    visible: card.granular
                    panel: card.panel
                    parameterId: card.base + ".grain.window"
                    width: card.windowWidth
                    height: card.rowHeight
                }
            }
            NovaSegs {
                visible: card.classic
                panel: card.panel
                parameterId: card.base + ".shape"
                fill: true
                width: sourceRow.width
                height: card.rowHeight
            }
            Row {
                visible: card.sampled
                spacing: card.sourceGap
                NovaDropdown {
                    panel: card.panel
                    parameterId: card.base + ".sample.loop"
                    width: card.loopWidth
                    height: card.rowHeight
                }
                NovaPill {
                    panel: card.panel
                    text: "KEYTRACK"
                    height: card.rowHeight
                    parameterId: card.base + ".sample.keytrack"
                }
            }
            Row {
                visible: card.spectral
                spacing: card.sourceGap
                NovaDropdown {
                    panel: card.panel
                    parameterId: card.base + ".spectral.fft"
                    width: card.fftWidth
                    height: card.rowHeight
                }
                NovaPill {
                    panel: card.panel
                    text: "FREEZE"
                    height: card.rowHeight
                    parameterId: card.base + ".spectral.freeze"
                }
            }
        }

        // PRIMARY: LEVEL, the type's main knobs, PAN.
        Loader {
            active: card.wavetable
            visible: active
            sourceComponent: Primary {
                Knob { parameterId: card.base + ".level"; label: "LEVEL" }
                Knob { parameterId: card.base + ".position"; label: "POS" }
                Warp { slot: 1 }
                Warp { slot: 2 }
                Knob { parameterId: card.base + ".pan"; label: "PAN" }
            }
        }
        Loader {
            active: card.classic
            visible: active
            sourceComponent: Primary {
                Knob { parameterId: card.base + ".level"; label: "LEVEL" }
                Knob {
                    parameterId: card.base + ".pw"
                    label: "PW"
                    inactive: !card.pulse
                    inactiveReason: inactive ? "Pulse width shapes the Pulse wave only" : ""
                }
                Knob { parameterId: card.base + ".sync"; label: "SYNC" }
                Knob { parameterId: card.base + ".drift"; label: "DRIFT" }
                Knob { parameterId: card.base + ".pan"; label: "PAN" }
            }
        }
        Loader {
            active: card.sampled
            visible: active
            sourceComponent: Primary {
                Knob { parameterId: card.base + ".level"; label: "LEVEL" }
                Knob { parameterId: card.base + ".sample.start"; label: "START" }
                Knob { parameterId: card.base + ".sample.loop_start"; label: "LOOP START" }
                Knob { parameterId: card.base + ".sample.loop_end"; label: "LOOP END" }
                Knob { parameterId: card.base + ".pan"; label: "PAN" }
            }
        }
        Loader {
            active: card.granular
            visible: active
            sourceComponent: Primary {
                Knob { parameterId: card.base + ".level"; label: "LEVEL" }
                Knob { parameterId: card.base + ".grain.size"; label: "SIZE" }
                Knob { parameterId: card.base + ".grain.density"; label: "DENSITY" }
                Knob { parameterId: card.base + ".position"; label: "POS" }
                Knob { parameterId: card.base + ".grain.spray"; label: "SPRAY" }
                Knob { parameterId: card.base + ".pan"; label: "PAN" }
            }
        }
        Loader {
            active: card.spectral
            visible: active
            sourceComponent: Primary {
                Knob { parameterId: card.base + ".level"; label: "LEVEL" }
                Knob { parameterId: card.base + ".position"; label: "POS" }
                Knob { parameterId: card.base + ".spectral.shift"; label: "SHIFT" }
                Knob { parameterId: card.base + ".spectral.stretch"; label: "STRETCH" }
                Knob { parameterId: card.base + ".pan"; label: "PAN" }
            }
        }

        Rectangle {
            width: column.width
            height: card.ruleHeight
            color: card.panel.theme.line
        }

        // SECONDARY: small knobs in labelled groups, side by side where the
        // card is wide enough.
        Flow {
            id: lower
            objectName: "novaOscLower:" + card.osc
            width: column.width
            spacing: card.groupGap
            Group {
                label: "UNISON"
                Column {
                    spacing: card.stackGap
                    Row {
                        spacing: card.stackGap
                        Small { parameterId: card.base + ".unison.voices"; label: "VOICES" }
                        Small { parameterId: card.base + ".unison.detune"; label: "DETUNE" }
                        Small { parameterId: card.base + ".unison.blend"; label: "BLEND" }
                        Small { parameterId: card.base + ".unison.width"; label: "WIDTH" }
                        Small {
                            visible: card.wavetable || card.granular
                            parameterId: card.base + ".unison.position_spread"
                            label: "POS SPREAD"
                        }
                    }
                    Row {
                        spacing: card.stackGap
                        NovaDropdown {
                            panel: card.panel
                            parameterId: card.base + ".unison.curve"
                            width: card.unisonCurveWidth
                            height: card.modeHeight
                            fontSize: card.modeFontSize
                        }
                        NovaDropdown {
                            panel: card.panel
                            parameterId: card.base + ".unison.stack"
                            width: card.unisonStackWidth
                            height: card.modeHeight
                            fontSize: card.modeFontSize
                        }
                    }
                }
            }
            // The type's own extras.
            Loader {
                active: card.classic
                visible: active
                sourceComponent: Group {
                    label: "WARP"
                    Warp { slot: 1; size: "sm" }
                    Small { parameterId: card.base + ".unison.warp_spread"; label: "W.SPREAD" }
                }
            }
            Loader {
                active: card.wavetable
                visible: active
                sourceComponent: Group {
                    label: "WARP"
                    Small { parameterId: card.base + ".unison.warp_spread"; label: "W.SPREAD" }
                }
            }
            Loader {
                active: card.sampled
                visible: active
                sourceComponent: Group {
                    label: "SAMPLE"
                    Small { parameterId: card.base + ".sample.crossfade"; label: "XFADE" }
                    Small { parameterId: card.base + ".sample.root"; label: "ROOT" }
                    Small { parameterId: card.base + ".position"; label: "POS" }
                }
            }
            Loader {
                active: card.granular
                visible: active
                sourceComponent: Group {
                    label: "GRAIN"
                    Small { parameterId: card.base + ".grain.scan_rate"; label: "RATE" }
                    Small { parameterId: card.base + ".grain.pitch_spray"; label: "P.SPRAY" }
                    Small { parameterId: card.base + ".grain.reverse"; label: "REVERSE" }
                }
            }
            Loader {
                active: card.spectral
                visible: active
                sourceComponent: Group {
                    label: "SPECTRAL"
                    Small { parameterId: card.base + ".spectral.smear"; label: "SMEAR" }
                    Small { parameterId: card.base + ".spectral.tilt"; label: "TILT" }
                }
            }
            Group {
                label: "PITCH"
                Small { parameterId: card.base + ".octave"; label: "OCT" }
                Small { parameterId: card.base + ".semitone"; label: "SEMI" }
                Small { parameterId: card.base + ".fine"; label: "FINE" }
                Small { parameterId: card.base + ".phase"; label: "PHASE" }
                Small { parameterId: card.base + ".random_phase"; label: "RANDOM" }
            }
        }
    }
}
