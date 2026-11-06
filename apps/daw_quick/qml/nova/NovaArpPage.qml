pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// The ARP · CLIP page (ADR-0078): the arpeggiator as a pattern engine and
// the clip sequencer under it.
//   - ARPEGGIATOR: sixteen modes in their groups (Order, Pedal, Chance,
//     Harmony, Drawn, Rhythm), rate, gate, swing, octaves, strum, groove
//     amount and seed, velocity mode, retrigger, latch, variation every N
//     bars and FILL (held while pressed; automatable).
//   - HARMONY · RHYTHM: how the Note lane moves (held notes or scale
//     degrees), the scale and root, passing notes, the Euclidean rhythm
//     with its ring, and the groove from the pool.
//   - PATTERNS: the library (NovaArpLibrary), which keeps the keys.
//   - STEP LANES: eleven lanes of up to 32 steps, each with its own length
//     (NovaArpLane), the period they repeat after, and MUTATE with its
//     seed and amount.
// The pattern step playing is amber in every lane.
Item {
    id: page
    required property var panel
    readonly property real gap: 10
    readonly property real topHeight: 250
    readonly property real laneHeight: 26
    readonly property real laneSpacing: 3
    readonly property real lanesTop: 38
    readonly property real lanesFooter: 44
    readonly property real sequencerMinimum: 240
    readonly property real arpWidth: 600
    readonly property real harmonyWidth: 330
    readonly property real libraryMinimum: 300
    readonly property var controller: panel.controller
    readonly property var arp: controller.arp
    readonly property var theme: panel.theme
    readonly property var laneSpecs: arp !== null ? arp.lanes : []
    readonly property real lanesHeight: lanesTop + laneSpecs.length * (laneHeight + laneSpacing) + lanesFooter
    // Below these the page is scaled down to fit (NovaPanel's Page).
    readonly property real minimumWidth: arpWidth + harmonyWidth + libraryMinimum + 2 * gap
    readonly property real minimumHeight: topHeight + lanesHeight + sequencerMinimum + 2 * gap
    readonly property int arpStep: panel.live ? Math.round(panel.at(Contract.T.arpStep)) : -1
    readonly property bool variationBar: panel.live && panel.at(Contract.T.arpVariation) >= 0.5
    readonly property int mode: controller.revision >= 0 ? controller.choice("nova.arp.mode") : 0
    readonly property string modeName: arp !== null && mode >= 0 && mode < arp.modes.length ? arp.modes[mode].name : ""
    readonly property bool euclidean: modeName === "Euclidean"
    // Each lane's colour, by lane id.
    readonly property var laneAccents: ({ "note": theme.cy, "octave": theme.vi, "velocity": theme.am,
                                          "gate": theme.gr, "tie": theme.pk, "ratchet": theme.am,
                                          "chance": theme.gr, "condition": theme.vi, "accent": theme.pk,
                                          "mod": theme.cy, "chord": theme.vi })

    objectName: "novaArpPage"

    component Label: Text {
        color: page.theme.faint
        font.family: page.theme.sans
        font.pixelSize: page.theme.typeCaption
        font.weight: Font.ExtraBold
        font.letterSpacing: page.theme.arpPageLabelSpacing
    }

    // ---- ARPEGGIATOR -------------------------------------------------------------------
    NovaCard {
        id: arpCard
        panel: page.panel
        title: "ARPEGGIATOR"
        ledParameter: "nova.arp.on"
        dimWhenOff: false
        width: page.arpWidth
        height: page.topHeight
        Rectangle {
            objectName: "novaArpVariationBadge"
            visible: page.variationBar
            anchors.right: parent.right
            anchors.rightMargin: page.theme.spaceLg
            y: page.theme.arpPageBadgeTop
            width: badgeText.implicitWidth + page.theme.arpPageBadgePadding
            height: page.theme.arpPageBadgeHeight
            radius: height / 2
            color: page.theme.soft(page.theme.am)
            border.color: page.theme.am
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: "VARIATION BAR"
                color: page.theme.am
                font.family: page.theme.sans
                font.pixelSize: page.theme.typeMicro
                font.weight: Font.Bold
                font.letterSpacing: page.theme.arpPageBadgeSpacing
            }
        }
        // The modes, in their groups.
        Flow {
            id: modes
            x: page.theme.arpPageCardPadding
            y: page.theme.arpPageCardBodyTop
            width: arpCard.width - 2 * page.theme.arpPageCardPadding
            spacing: page.theme.spaceLg
            Repeater {
                model: {
                    const groups = []
                    const all = page.arp !== null ? page.arp.modes : []
                    for (let i = 0; i < all.length; ++i) {
                        if (groups.length === 0 || groups[groups.length - 1].name !== all[i].group)
                            groups.push({ name: all[i].group, modes: [] })
                        groups[groups.length - 1].modes.push(all[i])
                    }
                    return groups
                }
                delegate: Column {
                    id: group
                    required property var modelData
                    spacing: page.theme.arpPageModeSpacing
                    Label { text: group.modelData.name.toUpperCase() }
                    Row {
                        spacing: page.theme.arpPageModeSpacing
                        Repeater {
                            model: group.modelData.modes
                            delegate: Rectangle {
                                id: modeButton
                                required property var modelData
                                readonly property bool on: page.mode === modelData.index
                                readonly property color accent: page.panel.accentFor("nova.arp.mode")
                                objectName: "novaArpMode:nova.arp.mode:" + modelData.index
                                width: modeText.implicitWidth + page.theme.arpPageModePadding
                                height: page.theme.arpPageModeHeight
                                radius: page.theme.arpPageModeRadius
                                color: on ? page.theme.soft(accent) : modeArea.containsMouse ? page.theme.hover
                                                                                             : page.theme.wellFill
                                border.color: on ? accent : page.theme.line2
                                Accessible.role: Accessible.RadioButton
                                Accessible.name: modelData.name
                                Accessible.checked: on
                                Text {
                                    id: modeText
                                    anchors.centerIn: parent
                                    text: modeButton.modelData.name
                                    color: modeButton.on ? modeButton.accent : page.theme.dim
                                    font.family: page.theme.sans
                                    font.pixelSize: page.theme.typeSmall
                                    font.weight: Font.DemiBold
                                }
                                MouseArea {
                                    id: modeArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    enabled: page.controller.editable
                                    onClicked: {
                                        page.controller.set_choice("nova.arp.mode", modeButton.modelData.index)
                                        page.panel.touchId("nova.arp.mode", "ARP MODE")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Row {
            id: knobs
            x: page.theme.arpPageCardPadding
            y: modes.y + modes.height + page.theme.spaceLg
            spacing: page.theme.spaceLg
            NovaKnob { panel: page.panel; parameterId: "nova.arp.rate"; label: "RATE" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.gate"; label: "GATE" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.swing"; label: "SWING" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.octaves"; label: "OCTAVES" }
            NovaKnob {
                panel: page.panel
                parameterId: "nova.arp.strum"
                label: "STRUM"
                inactive: page.modeName !== "Strum"
            }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.groove_amount"; label: "GROOVE" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.seed"; label: "SEED" }
        }
        Row {
            x: page.theme.arpPageCardPadding
            anchors.bottom: parent.bottom
            anchors.bottomMargin: page.theme.spaceLg
            spacing: page.theme.spaceSm
            Label { anchors.verticalCenter: parent.verticalCenter; text: "VELOCITY" }
            NovaSegs {
                anchors.verticalCenter: parent.verticalCenter
                panel: page.panel
                parameterId: "nova.arp.velocity"
                labels: ["PLAYED", "PATTERN"]
            }
            NovaPill { anchors.verticalCenter: parent.verticalCenter; panel: page.panel; text: "RETRIG"; parameterId: "nova.arp.retrigger" }
            NovaPill { anchors.verticalCenter: parent.verticalCenter; panel: page.panel; text: "LATCH"; parameterId: "nova.arp.latch" }
            Label { anchors.verticalCenter: parent.verticalCenter; text: "VARY" }
            NovaDropdown {
                anchors.verticalCenter: parent.verticalCenter
                panel: page.panel
                parameterId: "nova.arp.variation"
                width: page.theme.arpPageVaryWidth
            }
            // FILL plays while held: a fill's conditions, as a variation
            // bar does. It is a parameter, so automation can play it too.
            Rectangle {
                id: fill
                readonly property bool on: page.controller.revision >= 0 && page.controller.value("nova.arp.fill") >= 0.5
                objectName: "novaArpFill:nova.arp.fill"
                anchors.verticalCenter: parent.verticalCenter
                width: fillText.implicitWidth + page.theme.arpPageFillPadding
                height: page.theme.arpPageFillHeight
                radius: page.theme.radiusLg
                color: on ? page.theme.soft(page.theme.pk) : fillArea.containsMouse ? page.theme.hover : page.theme.wellFill
                border.color: on ? page.theme.pk : page.theme.line2
                Accessible.role: Accessible.Button
                Accessible.name: "Fill (hold)"
                Accessible.checked: on
                Text {
                    id: fillText
                    anchors.centerIn: parent
                    text: "FILL"
                    color: fill.on ? page.theme.pk : page.theme.dim
                    font.family: page.theme.sans
                    font.pixelSize: page.theme.typeSmall
                    font.weight: Font.Bold
                    font.letterSpacing: page.theme.arpPageFillSpacing
                }
                MouseArea {
                    id: fillArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: page.controller.editable
                    onPressed: {
                        page.controller.begin_gesture("nova.arp.fill")
                        page.controller.set_value("nova.arp.fill", 1)
                        page.panel.touchId("nova.arp.fill", "FILL")
                    }
                    onReleased: {
                        page.controller.set_value("nova.arp.fill", 0)
                        page.controller.end_gesture("nova.arp.fill")
                    }
                    onCanceled: page.controller.cancel_gesture("nova.arp.fill")
                }
                Kit.Tip {
                    theme: page.theme
                    shown: fillArea.containsMouse && !fillArea.pressed
                    text: "Fill"
                    detail: "Hold to play the fill: steps with a Fill condition play, Not fill rests"
                }
            }
        }
    }

    // ---- HARMONY · RHYTHM ----------------------------------------------------------------
    NovaCard {
        id: harmony
        panel: page.panel
        title: "HARMONY · RHYTHM"
        x: arpCard.width + page.gap
        width: page.harmonyWidth
        height: page.topHeight
        readonly property real column: page.theme.arpPageCardPadding
        Row {
            x: harmony.column
            y: page.theme.arpPageCardBodyTop
            spacing: page.theme.spaceSm
            Label { anchors.verticalCenter: parent.verticalCenter; text: "NOTE LANE" }
            NovaSegs {
                anchors.verticalCenter: parent.verticalCenter
                panel: page.panel
                parameterId: "nova.arp.note_mode"
                labels: ["+ HELD", "+ SCALE"]
            }
        }
        Row {
            x: harmony.column
            y: page.theme.arpPageScaleRowTop
            spacing: page.theme.spaceSm
            NovaDropdown { panel: page.panel; parameterId: "nova.arp.scale"; width: page.theme.arpPageScaleWidth }
            NovaDropdown { panel: page.panel; parameterId: "nova.arp.root"; width: page.theme.arpPageRootWidth }
            NovaPill { panel: page.panel; text: "PASSING"; parameterId: "nova.arp.passing" }
        }
        Label {
            x: harmony.column
            y: page.theme.arpPageEuclidLabelTop
            text: "EUCLIDEAN" + (page.euclidean ? "" : " · IN EUCLIDEAN MODE")
        }
        Row {
            x: harmony.column
            y: page.theme.arpPageEuclidKnobsTop
            spacing: page.theme.spaceMd
            opacity: page.euclidean ? 1 : page.theme.arpPageInactiveOpacity
            NovaKnob { panel: page.panel; parameterId: "nova.arp.euclid.hits"; label: "HITS"; size: "sm" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.euclid.steps"; label: "STEPS"; size: "sm" }
            NovaKnob { panel: page.panel; parameterId: "nova.arp.euclid.rotate"; label: "ROTATE"; size: "sm" }
        }
        // The rhythm as a ring: a lit dot per hit.
        Item {
            id: ring
            objectName: "novaArpEuclidRing"
            readonly property var steps: page.controller.revision >= 0 && page.arp !== null ? page.arp.euclid_steps() : []
            readonly property real radius: page.theme.arpPageRingRadius
            x: harmony.width - 2 * radius - page.theme.arpPageRingRight
            y: page.theme.arpPageRingTop
            width: 2 * radius + page.theme.arpPageRingDotRoom
            height: width
            opacity: page.euclidean ? 1 : page.theme.arpPageInactiveOpacity
            Repeater {
                model: ring.steps.length
                delegate: Rectangle {
                    required property int index
                    readonly property real angle: 2 * Math.PI * index / ring.steps.length - Math.PI / 2
                    readonly property bool hit: ring.steps[index] === true
                    readonly property bool here: page.euclidean && page.arpStep >= 0
                                                 && page.arpStep % ring.steps.length === index
                    width: hit ? page.theme.arpPageRingHitSize : page.theme.arpPageRingStepSize
                    height: width
                    radius: width / 2
                    x: ring.width / 2 + ring.radius * Math.cos(angle) - width / 2
                    y: ring.height / 2 + ring.radius * Math.sin(angle) - height / 2
                    color: here ? page.theme.am : hit ? page.panel.accentFor("nova.arp.mode") : page.theme.line2
                }
            }
        }
        Row {
            x: harmony.column
            anchors.bottom: parent.bottom
            anchors.bottomMargin: page.theme.spaceLg
            spacing: page.theme.spaceSm
            Label { anchors.verticalCenter: parent.verticalCenter; text: "GROOVE" }
            NovaDropdown {
                id: grooveList
                readonly property bool grooved: page.controller.revision >= 0
                    && page.controller.value("nova.arp.groove.steps") > 0
                objectName: "novaArpGroove"
                anchors.verticalCenter: parent.verticalCenter
                panel: page.panel
                width: page.theme.arpPageGrooveWidth
                accent: page.theme.am
                text: grooved && page.panel.doc.labels_revision >= 0
                      ? page.panel.doc.label("arp.groove", "Groove") : "None"
                entries: [{ label: "None", value: -1 }].concat(
                             page.panel.grooves.map((g, i) => ({ label: g.name, value: i })))
                currentValue: -1
                onPicked: value => {
                    if (value < 0)
                        page.arp.clear_groove()
                    else
                        page.arp.set_groove(page.panel.grooves[value])
                }
            }
        }
    }

    // ---- PATTERNS -------------------------------------------------------------------------
    NovaArpLibrary {
        panel: page.panel
        x: harmony.x + harmony.width + page.gap
        width: page.width - x
        height: page.topHeight
    }

    // ---- STEP LANES -------------------------------------------------------------------------
    NovaCard {
        id: lanes
        objectName: "novaArpLanes"
        panel: page.panel
        title: "STEP LANES"
        sub: "DRAG TO PAINT · CLICK TO TOGGLE · DOUBLE-CLICK RESETS · EACH LANE LOOPS AT ITS OWN LENGTH"
        y: page.topHeight + page.gap
        width: page.width
        height: page.lanesHeight
        readonly property real period: page.controller.revision >= 0 && page.arp !== null ? page.arp.period() : 1
        readonly property real periodBeats: page.controller.revision >= 0 && page.arp !== null ? page.arp.period_beats() : 4
        Text {
            objectName: "novaArpPeriod"
            anchors.right: parent.right
            anchors.rightMargin: page.theme.arpPageCardPadding
            y: page.theme.arpPageCardPadding
            text: "REPEATS EVERY " + Math.round(lanes.period) + " STEPS · "
                + (lanes.periodBeats / 4 >= 1000 ? "1000+" : +(lanes.periodBeats / 4).toFixed(1)) + " BARS"
            color: page.theme.am
            font.family: page.theme.mono
            font.pixelSize: page.theme.typeSmall
        }
        Column {
            x: page.theme.arpPageCardPadding
            y: page.lanesTop
            width: lanes.width - 2 * page.theme.arpPageCardPadding
            spacing: page.laneSpacing
            Repeater {
                model: page.laneSpecs
                delegate: NovaArpLane {
                    required property var modelData
                    panel: page.panel
                    spec: modelData
                    accent: page.laneAccents[modelData.id] !== undefined ? page.laneAccents[modelData.id] : page.theme.cy
                    maxSteps: page.arp !== null ? page.arp.maxSteps : 32
                    patternStep: page.arpStep
                    width: parent.width
                    height: page.laneHeight
                }
            }
        }
        // MUTATE: seeded variations of the pattern, locked lanes kept.
        Row {
            x: page.theme.arpPageCardPadding
            anchors.bottom: parent.bottom
            anchors.bottomMargin: page.theme.spaceLg
            spacing: page.theme.spaceMd
            Rectangle {
                id: mutate
                objectName: "novaArpMutate"
                anchors.verticalCenter: parent.verticalCenter
                width: mutateText.implicitWidth + page.theme.arpPageMutatePadding
                height: page.theme.arpPageMutateHeight
                radius: page.theme.radiusLg
                color: mutateArea.containsMouse ? page.theme.soft(page.theme.vi, page.theme.arpPageMutateHoverAlpha)
                                                : page.theme.soft(page.theme.vi)
                border.color: page.theme.vi
                Accessible.role: Accessible.Button
                Accessible.name: "Mutate"
                Text {
                    id: mutateText
                    anchors.centerIn: parent
                    text: "✦ MUTATE"
                    color: page.theme.vi
                    font.family: page.theme.sans
                    font.pixelSize: page.theme.typeSmall
                    font.weight: Font.Bold
                    font.letterSpacing: page.theme.arpPageMutateSpacing
                }
                MouseArea {
                    id: mutateArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: page.controller.editable
                    onClicked: page.arp.mutate(page.arp.mutation + 1)
                }
            }
            Text {
                objectName: "novaArpMutateBack"
                anchors.verticalCenter: parent.verticalCenter
                text: "‹"
                color: page.theme.dim
                font.pixelSize: page.theme.typeTitle
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -page.theme.spaceXs
                    enabled: page.controller.editable && page.arp !== null && page.arp.mutation > 0
                    onClicked: page.arp.mutate(page.arp.mutation - 1)
                }
            }
            Text {
                objectName: "novaArpMutation"
                anchors.verticalCenter: parent.verticalCenter
                width: page.theme.arpPageMutationWidth
                horizontalAlignment: Text.AlignHCenter
                text: page.arp === null || page.arp.mutation === 0 ? "PATTERN" : "SEED " + page.arp.mutation
                color: page.theme.text
                font.family: page.theme.mono
                font.pixelSize: page.theme.typeSmall
            }
            Text {
                objectName: "novaArpMutateNext"
                anchors.verticalCenter: parent.verticalCenter
                text: "›"
                color: page.theme.dim
                font.pixelSize: page.theme.typeTitle
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -page.theme.spaceXs
                    enabled: page.controller.editable
                    onClicked: page.arp.mutate(page.arp.mutation + 1)
                }
            }
            Label { anchors.verticalCenter: parent.verticalCenter; text: "AMOUNT" }
            // The amount: drag along the bar.
            Rectangle {
                id: amount
                objectName: "novaArpMutateAmount"
                anchors.verticalCenter: parent.verticalCenter
                width: page.theme.arpPageAmountWidth
                height: page.theme.arpPageAmountHeight
                radius: height / 2
                color: page.theme.wellFill
                border.color: page.theme.line2
                Accessible.role: Accessible.Slider
                Accessible.name: "Mutate amount"
                Rectangle {
                    width: Math.max(height, parent.width * (page.arp !== null ? page.arp.amount : 0))
                    height: parent.height
                    radius: height / 2
                    color: page.theme.vi
                    opacity: page.theme.arpPageAmountOpacity
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -page.theme.arpPageAmountSlop
                    preventStealing: true
                    function set(x) {
                        page.arp.amount = Math.max(0, Math.min(1, (x - page.theme.arpPageAmountSlop) / amount.width))
                    }
                    onPressed: mouse => set(mouse.x)
                    onPositionChanged: mouse => { if (pressed) set(mouse.x) }
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: page.arp !== null ? Math.round(page.arp.amount * 100) + "%" : ""
                color: page.theme.dim
                font.family: page.theme.mono
                font.pixelSize: page.theme.typeSmall
            }
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: page.theme.arpPageCardPadding
            anchors.bottom: parent.bottom
            anchors.bottomMargin: page.theme.arpPageNoteBottom
            text: "ARP MOD and ARP ACCENT are mod sources: route them in the MATRIX"
            color: page.theme.faint
            font.family: page.theme.sans
            font.pixelSize: page.theme.typeCaption
        }
    }

    NovaClipSequencer {
        panel: page.panel
        y: lanes.y + lanes.height + page.gap
        width: page.width
        height: page.height - y
    }
}
