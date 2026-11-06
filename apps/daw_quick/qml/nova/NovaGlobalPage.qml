pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// The GLOBAL page (NOVA-UI-14): voicing, playing and quality, then the
// chaos sources, the live performance sources and the bus meters, then
// Kick Lock (ADR-0072). The cards widen with the page and the rows share
// any height above the minimum (ADR-0076); the meters grow with theirs.
Item {
    id: page
    required property var panel
    readonly property real gap: panel.theme.pageGap
    // Portamento in note values: the division dropdown takes the knob's place.
    readonly property bool portaSynced: panel.controller.revision >= 0
                                        && panel.controller.choice("nova.global.portamento_sync") === 1
    readonly property real divisionWidth: 78
    readonly property real divisionHeight: panel.theme.dropdownHeight
    // Fixed widths: the tuning menu, the chaos and output cards, the chaos
    // names and value bars, the live sources' names and values, the mono pill.
    readonly property real tuningWidth: 150
    readonly property real chaosWidth: 330
    readonly property real metersWidth: 310
    readonly property real chaosNameWidth: 80
    readonly property real chaosBarWidth: 90
    readonly property real sourceNameWidth: 74
    readonly property real sourceValueWidth: 28
    readonly property real monoPillWidth: 92
    // The meters: their inset in the card, the dB at the floor and the span
    // to the top, the level that reads as clipping.
    readonly property real metersInset: 18
    readonly property real meterFloorDb: -60
    readonly property real meterSpanDb: 66
    readonly property real meterClipDb: -1
    readonly property real kickNoteTop: 128
    readonly property real third: (width - 2 * gap) / 3
    // The narrowest a top-row card holds its widest row (the portamento
    // switches), and each row's height at the page's minimum.
    readonly property real thirdMinimum: 424
    readonly property real row1Minimum: 230
    readonly property real row2Minimum: 200
    readonly property real row3Minimum: 190
    readonly property real meterMinimum: 118
    // Below these the page is scaled down to fit (NovaPanel's Page).
    readonly property real minimumWidth: 3 * thirdMinimum + 2 * gap
    readonly property real minimumHeight: row1Minimum + row2Minimum + row3Minimum + 2 * gap
    readonly property real rowExtra: Math.max(0, height - minimumHeight) / 3
    readonly property real row1: row1Minimum + rowExtra
    readonly property real row2: row2Minimum + rowExtra
    readonly property real row3: row3Minimum + rowExtra

    objectName: "novaGlobalPage"

    component Note: Text {
        color: page.panel.theme.faint
        font.family: page.panel.theme.sans
        font.pixelSize: page.panel.theme.typeSmall
        lineHeight: page.panel.theme.noteLineHeight
        wrapMode: Text.WordWrap
    }
    component Label: Text {
        color: page.panel.theme.faint
        font.family: page.panel.theme.sans
        font.pixelSize: page.panel.theme.typeSmall
        font.weight: Font.ExtraBold
        font.letterSpacing: page.panel.theme.sectionLabelSpacing
    }

    NovaCard {
        panel: page.panel
        title: "VOICING"
        width: page.third
        height: page.row1
        Column {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            spacing: page.panel.theme.cardBodyGap
            NovaSegs { panel: page.panel; parameterId: "nova.global.voicing"; labels: ["POLY", "MONO", "LEGATO"] }
            Row {
                spacing: page.panel.theme.cardBodyGap
                NovaKnob { panel: page.panel; parameterId: "nova.global.polyphony"; label: "VOICES" }
                // Seconds, or a note value of the tempo with SYNC on.
                NovaKnob {
                    visible: !page.portaSynced
                    panel: page.panel
                    parameterId: "nova.global.portamento"
                    label: "PORTA"
                }
                Column {
                    visible: page.portaSynced
                    spacing: page.panel.theme.spaceXs
                    NovaDropdown {
                        panel: page.panel
                        parameterId: "nova.global.portamento_division"
                        width: page.divisionWidth
                        height: page.divisionHeight
                    }
                    Text {
                        text: "PORTA"
                        color: page.panel.theme.dim
                        font.family: page.panel.theme.sans
                        font.pixelSize: page.panel.theme.knobCaptionSize
                        font.weight: Font.Bold
                    }
                }
                NovaKnob { panel: page.panel; parameterId: "nova.global.portamento_curve"; label: "PORTA CURVE" }
                NovaKnob { panel: page.panel; parameterId: "nova.global.drift"; label: "DRIFT" }
            }
            Row {
                spacing: page.panel.theme.spaceMd
                NovaSegs { panel: page.panel; parameterId: "nova.global.portamento_mode"; labels: ["PORTA: LEGATO ONLY", "ALWAYS"] }
                NovaSegs { panel: page.panel; parameterId: "nova.global.portamento_rate"; labels: ["CONST TIME", "CONST RATE"] }
            }
            NovaSegs { panel: page.panel; parameterId: "nova.global.portamento_sync"; labels: ["PORTA: SECONDS", "SYNC"] }
        }
    }
    NovaCard {
        panel: page.panel
        title: "PLAYING"
        x: page.third + page.gap
        width: page.third
        height: page.row1
        Column {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            width: parent.width - 2 * page.panel.theme.cardBodyX
            spacing: page.panel.theme.cardBodyGap
            Row {
                spacing: page.panel.theme.cardBodyGap
                NovaKnob { panel: page.panel; parameterId: "nova.global.bend_range"; label: "BEND" }
                NovaKnob { panel: page.panel; parameterId: "nova.global.velocity_curve"; label: "VEL CURVE" }
                NovaKnob { panel: page.panel; parameterId: "nova.global.mpe_range"; label: "MPE BEND" }
            }
            NovaSegs { panel: page.panel; parameterId: "nova.global.mpe"; labels: ["MPE OFF", "MPE ON"] }
            Note {
                width: parent.width
                text: "With MPE on, each note's bend, slide (CC 74) and pressure become the Bend, Slide and Pressure sources."
            }
        }
    }
    NovaCard {
        panel: page.panel
        title: "QUALITY"
        x: 2 * (page.third + page.gap)
        width: page.third
        height: page.row1
        Column {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            width: parent.width - 2 * page.panel.theme.cardBodyX
            spacing: page.panel.theme.cardBodyGap
            NovaSegs { panel: page.panel; parameterId: "nova.global.quality"; labels: ["ECO", "HIGH", "ULTRA"] }
            Note {
                width: parent.width
                text: "HIGH: 2× oscillators, 4× filters and distortion. ULTRA: 4× everywhere, for rendering. ECO halves unison above 8 voices."
            }
            Label { text: "TUNING" }
            Row {
                spacing: page.panel.theme.spaceLg
                NovaDropdown {
                    anchors.verticalCenter: parent.verticalCenter
                    panel: page.panel
                    parameterId: "nova.global.tuning"
                    width: page.tuningWidth
                }
                NovaKnob { panel: page.panel; parameterId: "nova.global.a4"; size: "sm"; label: "A4" }
            }
        }
    }

    // Chaos sources.
    NovaCard {
        id: chaos
        panel: page.panel
        title: "CHAOS"
        titleColor: page.panel.theme.vi
        sub: "LORENZ · LOGISTIC"
        y: page.row1 + page.gap
        width: page.chaosWidth
        height: page.row2
        Column {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            spacing: page.panel.theme.spaceMd
            Repeater {
                model: 2
                delegate: Row {
                    id: chaosRow
                    required property int index
                    readonly property string source: "chaos." + (index + 1)
                    spacing: page.panel.theme.cardBodyGap
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: page.panel.theme.spaceXs
                        width: page.chaosNameWidth
                        NovaHandle { panel: page.panel; source: chaosRow.source }
                        Text {
                            text: page.panel.sourceName(chaosRow.source).toUpperCase()
                            color: page.panel.theme.text
                            font.family: page.panel.theme.sans
                            font.pixelSize: page.panel.theme.typeSmall
                            font.weight: Font.Bold
                        }
                    }
                    NovaKnob { panel: page.panel; parameterId: "nova." + chaosRow.source + ".rate"; size: "sm"; label: "RATE" }
                    NovaKnob { panel: page.panel; parameterId: "nova." + chaosRow.source + ".smooth"; size: "sm"; label: "SMOOTH" }
                    // The source's live value.
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: page.chaosBarWidth
                        height: page.panel.theme.valueBarHeight
                        radius: height / 2
                        color: page.panel.theme.wellFill
                        border.color: page.panel.theme.line
                        Rectangle {
                            width: parent.width * Math.max(0, Contract.sourceValue(page.panel.frame, chaosRow.source))
                            height: parent.height
                            radius: height / 2
                            color: page.panel.theme.vi
                        }
                    }
                }
            }
        }
    }

    // Live sources: every performance source and macro as it moves.
    NovaCard {
        id: sources
        objectName: "novaLiveSources"
        panel: page.panel
        title: "SOURCES"
        titleColor: page.panel.theme.vi
        sub: page.panel.live ? "LIVE · NEWEST VOICE" : "PLAY A NOTE TO SEE THEM MOVE"
        x: chaos.width + page.gap
        y: page.row1 + page.gap
        width: page.width - x - page.metersWidth - page.gap
        height: page.row2
        Grid {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            columns: 2
            columnSpacing: page.panel.theme.sourceColumnGap
            rowSpacing: page.panel.theme.sourceRowGap
            Repeater {
                model: Contract.perfSources.concat(Contract.macroSources.slice(0, 4))
                delegate: Row {
                    id: sourceRow
                    required property string modelData
                    readonly property real value: Math.max(0, Contract.sourceValue(page.panel.frame, modelData))
                    spacing: page.panel.theme.spaceMd
                    Text {
                        width: page.sourceNameWidth
                        text: page.panel.sourceName(sourceRow.modelData)
                        color: page.panel.theme.dim
                        font.family: page.panel.theme.sans
                        font.pixelSize: page.panel.theme.typeSmall
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: (sources.width - 2 * page.panel.theme.cardBodyX - page.panel.theme.sourceColumnGap) / 2
                               - (page.sourceNameWidth + page.panel.theme.spaceMd) - (page.sourceValueWidth + page.panel.theme.spaceMd)
                        height: page.panel.theme.sourceBarHeight
                        radius: height / 2
                        color: page.panel.theme.wellFill
                        Rectangle {
                            width: parent.width * sourceRow.value
                            height: parent.height
                            radius: height / 2
                            color: page.panel.theme.vi
                        }
                    }
                    Text {
                        width: page.sourceValueWidth
                        text: page.panel.live ? Math.round(sourceRow.value * 100) : "—"
                        color: page.panel.theme.faint
                        font.family: page.panel.theme.mono
                        font.pixelSize: page.panel.theme.typeSmall
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }
    }

    // Bus meters.
    NovaCard {
        objectName: "novaMeters"
        panel: page.panel
        title: "OUTPUT"
        x: page.width - page.metersWidth
        y: page.row1 + page.gap
        width: page.metersWidth
        height: page.row2
        Row {
            x: page.metersInset
            y: page.panel.theme.cardBodyTop
            spacing: page.panel.theme.meterGap
            Repeater {
                model: ["MAIN", "BUS 1", "BUS 2", "OUT"]
                delegate: Column {
                    id: meter
                    required property string modelData
                    required property int index
                    spacing: page.panel.theme.spaceSm
                    Row {
                        spacing: page.panel.theme.spaceXs
                        Repeater {
                            model: 2
                            delegate: Rectangle {
                                id: bar
                                required property int index
                                readonly property real db: page.panel.live
                                    ? page.panel.at(Contract.T.meters + meter.index * 2 + index) : -120
                                width: page.panel.theme.meterBarWidth
                                height: page.meterMinimum + page.rowExtra
                                radius: page.panel.theme.radiusSm
                                color: page.panel.theme.wellFill
                                border.color: page.panel.theme.line
                                Rectangle {
                                    readonly property real fill: Math.max(0, Math.min(1, (bar.db - page.meterFloorDb) / page.meterSpanDb))
                                    x: page.panel.theme.hairline
                                    width: parent.width - 2 * page.panel.theme.hairline
                                    y: parent.height - page.panel.theme.hairline - height
                                    height: (parent.height - 2 * page.panel.theme.hairline) * fill
                                    radius: page.panel.theme.radiusXs
                                    color: bar.db > page.meterClipDb ? page.panel.theme.red : page.panel.theme.neutral
                                }
                            }
                        }
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: meter.modelData
                    }
                }
            }
        }
    }

    // Kick & Bass Lock (ADR-0072): below the crossover the bass ducks under
    // each locked Anvil hit's own curve and folds to mono.
    NovaCard {
        objectName: "novaKickLock"
        panel: page.panel
        title: "KICK LOCK"
        sub: "FOLLOWS ANVIL"
        ledParameter: "nova.lock.on"
        y: page.row1 + page.row2 + 2 * page.gap
        width: page.third
        height: page.row3
        Row {
            x: page.panel.theme.cardBodyX
            y: page.panel.theme.cardBodyTop
            spacing: page.panel.theme.cardBodyGap
            NovaKnob { panel: page.panel; parameterId: "nova.lock.duck"; label: "DUCK" }
            NovaKnob { panel: page.panel; parameterId: "nova.lock.crossover"; label: "CROSSOVER" }
            NovaPill {
                anchors.verticalCenter: parent.verticalCenter
                panel: page.panel
                text: "MONO LOWS"
                parameterId: "nova.lock.mono"
                width: page.monoPillWidth
            }
        }
        Note {
            x: page.panel.theme.cardBodyX
            y: page.kickNoteTop
            width: parent.width - 2 * page.panel.theme.cardBodyX
            text: "Follows Anvil tracks with Lock on: below the crossover the bass ducks under each kick's own curve, with no latency."
        }
    }
}
