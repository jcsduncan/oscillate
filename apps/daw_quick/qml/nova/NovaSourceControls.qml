pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// A modulation source's controls (NOVA-UI-07), on the MODULATION card
// beside its drawing (NovaEnvelopeView or NovaCurveEditor). An envelope:
// ATTACK, HOLD, DECAY, SUSTAIN and RELEASE at full size, then small the
// three curves and velocity, and LOOP and LEGATO. An LFO: its mode and
// sync, RATE, RISE, DELAY, SMOOTH and PHASE at full size, and the shape
// tools (SHAPES, GRID, BIPOLAR, GLOBAL).
Item {
    id: controls
    required property var panel
    required property string source
    readonly property bool envelope: source.indexOf("env.") === 0
    readonly property string base: "nova." + source
    readonly property color accent: panel.sourceAccent(source)
    readonly property int routesRevision: panel.doc.routes_revision
    readonly property var grids: [1, 2, 3, 4, 6, 8, 12, 16, 24, 32]
    readonly property real sectionGap: panel.theme.spaceMd
    readonly property real knobGap: panel.theme.spaceMd
    readonly property real groupGap: panel.theme.knobGroupGap
    readonly property real pillGap: panel.theme.spaceSm
    readonly property real rowHeight: panel.theme.controlRowHeight
    readonly property real syncWidth: 96

    objectName: "novaSourceControls:" + source
    implicitHeight: column.implicitHeight

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

        // An envelope's stages.
        Loader {
            active: controls.envelope
            visible: active
            sourceComponent: Column {
                width: column.width
                spacing: controls.sectionGap
                Flow {
                    width: column.width
                    spacing: controls.knobGap
                    Knob { parameterId: controls.base + ".attack"; label: "ATTACK" }
                    Knob { parameterId: controls.base + ".hold"; label: "HOLD" }
                    Knob { parameterId: controls.base + ".decay"; label: "DECAY" }
                    Knob { parameterId: controls.base + ".sustain"; label: "SUSTAIN" }
                    Knob { parameterId: controls.base + ".release"; label: "RELEASE" }
                }
                Flow {
                    width: column.width
                    spacing: controls.groupGap
                    NovaKnobGroup {
                        panel: controls.panel
                        label: "CURVE"
                        Small { parameterId: controls.base + ".attack_curve"; label: "A CURVE" }
                        Small { parameterId: controls.base + ".decay_curve"; label: "D CURVE" }
                        Small { parameterId: controls.base + ".release_curve"; label: "R CURVE" }
                    }
                    NovaKnobGroup {
                        panel: controls.panel
                        label: "VEL"
                        Small {
                            parameterId: controls.base + ".velocity"
                            label: controls.source === Contract.ampEnvelope ? "AMP VEL" : "VELOCITY"
                        }
                    }
                }
                Row {
                    spacing: controls.pillGap
                    NovaPill {
                        panel: controls.panel
                        text: "LOOP"
                        height: controls.rowHeight
                        parameterId: controls.base + ".loop"
                    }
                    NovaPill {
                        panel: controls.panel
                        text: "LEGATO"
                        height: controls.rowHeight
                        parameterId: controls.base + ".legato"
                    }
                }
            }
        }

        // An LFO's timing and shape tools.
        Loader {
            active: !controls.envelope
            visible: active
            sourceComponent: Column {
                width: column.width
                spacing: controls.sectionGap
                Row {
                    spacing: controls.pillGap
                    NovaSegs {
                        panel: controls.panel
                        parameterId: controls.base + ".mode"
                        labels: ["TRIG", "ENV", "FREE"]
                        height: controls.rowHeight
                    }
                    NovaDropdown {
                        panel: controls.panel
                        parameterId: controls.base + ".sync"
                        width: controls.syncWidth
                        height: controls.rowHeight
                    }
                }
                Flow {
                    width: column.width
                    spacing: controls.knobGap
                    Knob {
                        parameterId: controls.base + ".rate"
                        label: "RATE"
                        inactive: controls.panel.controller.revision >= 0
                                  && controls.panel.controller.choice(controls.base + ".sync") !== 0
                        inactiveReason: "Rate follows Sync"
                    }
                    Knob { parameterId: controls.base + ".rise"; label: "RISE" }
                    Knob { parameterId: controls.base + ".delay"; label: "DELAY" }
                    Knob { parameterId: controls.base + ".smooth"; label: "SMOOTH" }
                    Knob { parameterId: controls.base + ".phase"; label: "PHASE" }
                }
                Flow {
                    width: column.width
                    spacing: controls.pillGap
                    NovaPill {
                        id: shapesPill
                        objectName: "novaShapes"
                        panel: controls.panel
                        accent: controls.accent
                        text: "SHAPES ▾"
                        height: controls.rowHeight
                        onClicked: {
                            const items = Contract.shapes.map((s, i) => ({ label: s.name, value: i }))
                            controls.panel.menu.open(shapesPill, items, null, controls.accent, value =>
                                controls.panel.doc.set_curve(controls.source, Contract.shapes[value].points))
                        }
                    }
                    NovaPill {
                        id: gridPill
                        objectName: "novaGrid:" + controls.base + ".grid"
                        panel: controls.panel
                        accent: controls.accent
                        readonly property int grid: controls.panel.controller.revision >= 0
                            ? Math.round(controls.panel.controller.value(controls.base + ".grid")) : 8
                        text: "GRID " + grid
                        height: controls.rowHeight
                        onClicked: {
                            const items = controls.grids.map(n => ({ label: "Grid " + n, value: n }))
                            controls.panel.menu.open(gridPill, items, gridPill.grid, controls.accent, value => {
                                controls.panel.controller.set_value(controls.base + ".grid", value)
                                controls.panel.touchId(controls.base + ".grid", "GRID")
                            })
                        }
                    }
                    NovaPill {
                        objectName: "novaBipolar"
                        panel: controls.panel
                        text: "BIPOLAR"
                        accent: controls.accent
                        height: controls.rowHeight
                        localOn: {
                            if (controls.routesRevision < 0)
                                return false
                            const count = controls.panel.doc.routes_from(controls.source)
                            if (count === 0)
                                return false
                            const rows = controls.panel.doc.routes
                            for (let i = 0; i < rows.count; ++i) {
                                const route = rows.get(i)
                                if (route.source === controls.source && !route.bipolar)
                                    return false
                            }
                            return true
                        }
                        onClicked: controls.panel.doc.set_source_polarity(controls.source, !localOn)
                    }
                    NovaPill {
                        panel: controls.panel
                        text: "GLOBAL"
                        height: controls.rowHeight
                        parameterId: controls.base + ".scope"
                    }
                }
            }
        }
    }
}
