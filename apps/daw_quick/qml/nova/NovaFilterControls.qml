pragma ComponentBehavior: Bound
import QtQuick

// A filter's controls (NOVA-UI-08) on its overview card, under its response
// (NovaFilterResponse); the card's header holds the on LED, the model menu
// and the cutoff. The PRIMARY knobs at full size: CUTOFF, RESONANCE, DRIVE
// and FAT; under a rule the SECONDARY knobs small: KEYTRACK, PAN and MIX.
// The routing of the sources into the filters has its own card.
Item {
    id: controls
    required property var panel
    required property int filter
    readonly property string base: "nova.filter." + filter
    readonly property real sectionGap: panel.theme.spaceMd
    readonly property real knobGap: panel.theme.spaceMd
    readonly property real ruleHeight: panel.theme.hairline

    objectName: "novaFilter:" + filter
    implicitHeight: column.implicitHeight

    Column {
        id: column
        width: controls.width
        spacing: controls.sectionGap
        Flow {
            width: column.width
            spacing: controls.knobGap
            NovaKnob { panel: controls.panel; parameterId: controls.base + ".cutoff"; label: "CUTOFF" }
            NovaKnob { panel: controls.panel; parameterId: controls.base + ".resonance"; label: "RESONANCE" }
            NovaKnob { panel: controls.panel; parameterId: controls.base + ".drive"; label: "DRIVE" }
            NovaKnob { panel: controls.panel; parameterId: controls.base + ".fat"; label: "FAT" }
        }
        Rectangle {
            width: column.width
            height: controls.ruleHeight
            color: controls.panel.theme.line
        }
        NovaKnobGroup {
            panel: controls.panel
            NovaKnob { panel: controls.panel; size: "sm"; parameterId: controls.base + ".keytrack"; label: "KEYTRACK" }
            NovaKnob { panel: controls.panel; size: "sm"; parameterId: controls.base + ".pan"; label: "PAN" }
            NovaKnob { panel: controls.panel; size: "sm"; parameterId: controls.base + ".mix"; label: "MIX" }
        }
    }
}
