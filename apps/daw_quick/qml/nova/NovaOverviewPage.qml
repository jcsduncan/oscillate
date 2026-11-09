pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract

// The overview (ADR-0076 revision 2), page 0: the whole synth on one page.
// Wide, a grid of cards in these areas
//     OSC A   OSC B    OSC C      SUB · NOISE
//     FILTER 1 FILTER 2 ROUTING ·········
//     MODULATION ················ MACROS
// with the three oscillator columns equal and the fourth narrower; below
// `panel.overviewNarrowWidth` two columns: OSC A, OSC B / OSC C, SUB · NOISE
// / FILTER 1, FILTER 2 / ROUTING / MODULATION / MACROS. A row is as tall as
// its tallest card; spare height is shared between the rows and goes to
// the displays. When the cards need more height than the page has, the
// page scrolls.
//
// Every card (NovaModuleCard) shows its display and every control its
// module has; a module that is off fades, and OSC C folds to its header. ⤢ or a double-click on a title
// opens that card large over the grid, under a scrim (`panel.focusModule`);
// Esc, ⤢ again or a click on the scrim restores it. The MODULATION card
// holds a tab per source: a click selects it (`panel.modTab`) and its
// editor and knobs fill the card; a drag routes it onto any knob. The
// strip's jump (`panel.revealModule`) scrolls to a card and flashes it.
Item {
    id: page
    required property var panel

    // Layout.
    readonly property real gap: panel.theme.spaceMd
    readonly property real expandInset: panel.theme.spaceLg
    readonly property real expandedZ: 2
    readonly property real scrimZ: 1
    readonly property var wideWeights: [1, 1, 1, 0.62]
    readonly property var narrowWeights: [1, 1]
    // Each card's [column, row, columns spanned].
    readonly property var wideAreas: ({
        "a": [0, 0, 1], "b": [1, 0, 1], "c": [2, 0, 1], "subnoise": [3, 0, 1],
        "filter1": [0, 1, 1], "filter2": [1, 1, 1], "routing": [2, 1, 2],
        "mod": [0, 2, 3], "macros": [3, 2, 1] })
    readonly property var narrowAreas: ({
        "a": [0, 0, 1], "b": [1, 0, 1], "c": [0, 1, 1], "subnoise": [1, 1, 1],
        "filter1": [0, 2, 1], "filter2": [1, 2, 1], "routing": [0, 3, 2],
        "mod": [0, 4, 2], "macros": [0, 5, 2] })
    readonly property int wideRows: 3
    readonly property int narrowRows: 6
    // Narrow is measured on the area right of the browser, margins included.
    readonly property bool narrow: width + 2 * panel.pageMargin < panel.overviewNarrowWidth
    readonly property var weights: narrow ? narrowWeights : wideWeights
    readonly property var areas: narrow ? narrowAreas : wideAreas
    readonly property int rows: narrow ? narrowRows : wideRows
    readonly property int columns: weights.length
    readonly property real weightTotal: weights.reduce((a, b) => a + b, 0)
    readonly property real unit: (scroller.width - (columns - 1) * gap) / weightTotal
    // The MODULATION card: the source's editor's share of the width beside
    // its knobs, and its height before any spare height reaches it.
    readonly property real editorShare: 0.58
    readonly property real editorHeight: 132
    // Below these the page is scaled down to fit (NovaPanel's Page); it
    // scrolls rather than scales when it is short.
    readonly property real minimumWidth: 640
    readonly property real minimumHeight: 320

    readonly property var cards: [oscA, oscB, oscC, subNoise, filterOne, filterTwo, routing, modulation, macros]
    // Each row's height before the spare height is shared out.
    readonly property var naturalRows: {
        const heights = []
        for (let r = 0; r < rows; ++r)
            heights.push(0)
        for (let i = 0; i < cards.length; ++i) {
            const area = areas[cards[i].key]
            heights[area[1]] = Math.max(heights[area[1]], cards[i].cellHeight)
        }
        return heights
    }
    readonly property real naturalHeight: naturalRows.reduce((a, b) => a + b, 0) + (rows - 1) * gap
    readonly property real rowExtra: Math.max(0, scroller.height - naturalHeight) / rows
    readonly property var expandedCard: {
        for (let i = 0; i < cards.length; ++i)
            if (cards[i].expanded)
                return cards[i]
        return null
    }
    // Where the view was when a card opened large: the card opens there.
    property real expandedTop: 0

    objectName: "novaOverview"

    function columnX(column) {
        let weight = 0
        for (let c = 0; c < column; ++c)
            weight += weights[c]
        return weight * unit + column * gap
    }
    function spanWidth(column, span) {
        let weight = 0
        for (let c = column; c < column + span; ++c)
            weight += weights[c]
        return weight * unit + (span - 1) * gap
    }
    function rowY(row) {
        let y = 0
        for (let r = 0; r < row; ++r)
            y += naturalRows[r] + rowExtra + gap
        return y
    }
    function rowHeight(row) {
        return naturalRows[row] + rowExtra
    }
    function cardFor(key) {
        for (let i = 0; i < cards.length; ++i)
            if (cards[i].key === key)
                return cards[i]
        return null
    }
    // Scrolls to the card for `key` and flashes it.
    function reveal(key) {
        const card = cardFor(key)
        if (card === null)
            return
        if (panel.focusModule !== "" && panel.focusModule !== key)
            panel.restore()
        const end = Math.max(0, scroller.contentHeight - scroller.height)
        scroller.contentY = Math.max(0, Math.min(card.y - gap, end))
        card.flash()
    }

    Connections {
        target: page.panel
        function onFocusModuleChanged() {
            page.expandedTop = scroller.contentY
        }
        function onFlashSerialChanged() {
            page.reveal(page.panel.flashKey)
        }
    }

    // A card in its grid area, or large over the grid.
    component Cell: NovaModuleCard {
        id: cell
        readonly property var area: page.areas[cell.key]
        panel: page.panel
        x: cell.expanded ? page.expandInset : page.columnX(cell.area[0])
        y: cell.expanded ? page.expandedTop + page.expandInset : page.rowY(cell.area[1])
        width: cell.expanded ? scroller.width - 2 * page.expandInset : page.spanWidth(cell.area[0], cell.area[2])
        // A folded card keeps its header's height at the top of its area.
        height: cell.expanded ? Math.max(scroller.height - 2 * page.expandInset, cell.implicitHeight)
                              : cell.collapsed ? cell.implicitHeight : page.rowHeight(cell.area[1])
        z: cell.expanded ? page.expandedZ : 0
    }
    // An oscillator: its display by type, and its controls.
    component OscCell: Cell {
        id: oscCell
        readonly property string base: "nova.osc." + oscCell.key
        title: "OSC " + oscCell.key.toUpperCase()
        accent: page.panel.accentFor(base)
        ledParameter: base + ".on"
        typeParameter: base + ".type"
        sub: page.panel.moduleSub(oscCell.key)
        display: Component {
            NovaOscDisplay {
                panel: page.panel
                osc: oscCell.key
                type: page.panel.controller.revision >= 0 ? page.panel.controller.choice(oscCell.base + ".type") : 1
                table: page.panel.oscTable(oscCell.key)
            }
        }
        controls: Component {
            NovaOscControls {
                panel: page.panel
                osc: oscCell.key
            }
        }
    }
    // A filter: its response, and its controls; the cutoff on the right.
    component FilterCell: Cell {
        id: filterCell
        required property int number
        readonly property string base: "nova.filter." + filterCell.number
        title: "FILTER " + filterCell.number
        accent: page.panel.accentFor(base)
        ledParameter: base + ".on"
        typeParameter: base + ".model"
        typeGroups: Contract.filterGroups
        sub: page.panel.controller.revision >= 0 ? Contract.hz(page.panel.filterCutoff(filterCell.number)) : ""
        display: Component {
            NovaFilterResponse {
                panel: page.panel
                filter: filterCell.number
            }
        }
        controls: Component {
            NovaFilterControls {
                panel: page.panel
                filter: filterCell.number
            }
        }
    }

    Flickable {
        id: scroller
        objectName: "novaOverviewScroller"
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(page.naturalHeight, page.expandedCard !== null
            ? page.expandedCard.y + page.expandedCard.height + page.expandInset : 0)
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Item {
            id: grid
            width: scroller.width
            height: Math.max(scroller.height, scroller.contentHeight)

            OscCell { id: oscA; key: "a" }
            OscCell { id: oscB; key: "b" }
            // OSC C folds to its header while it is off.
            OscCell { id: oscC; key: "c"; collapsible: true }

            Cell {
                id: subNoise
                key: "subnoise"
                title: "SUB · NOISE"
                accent: page.panel.accentFor("nova.sub.")
                sub: page.panel.moduleSub(subNoise.key)
                display: Component {
                    NovaSubNoiseDisplay { panel: page.panel }
                }
                controls: Component {
                    NovaSubNoiseControls { panel: page.panel }
                }
            }

            FilterCell { id: filterOne; key: "filter1"; number: 1 }
            FilterCell { id: filterTwo; key: "filter2"; number: 2 }

            // The sources into the filters, and the output's level.
            Cell {
                id: routing
                key: "routing"
                title: "ROUTING"
                expandable: false
                accent: page.panel.accentFor("nova.filter.routing")
                bodyHeight: Math.max(routingGrid.implicitHeight, out.implicitHeight)
                NovaRouting {
                    id: routingGrid
                    panel: page.panel
                    width: Math.min(implicitWidth, routing.body.width - out.width - routing.gap)
                    height: implicitHeight
                }
                Column {
                    id: out
                    objectName: "novaOut"
                    x: routing.body.width - width
                    spacing: routing.gap
                    NovaHeading {
                        anchors.horizontalCenter: parent.horizontalCenter
                        panel: page.panel
                        text: "OUT"
                    }
                    NovaKnob {
                        panel: page.panel
                        parameterId: "nova.master"
                        label: "MASTER"
                    }
                }
            }

            // The modulation sources: a tab each, the selected one's editor
            // on the left and its knobs on the right.
            Cell {
                id: modulation
                readonly property string source: page.panel.modTab
                readonly property bool envelope: source.indexOf("env.") === 0
                readonly property real editorNatural: modulation.expanded ? modulation.expandedDisplayHeight
                                                                      : page.editorHeight
                key: "mod"
                title: "MODULATION"
                sub: "CLICK TO EDIT · DRAG ONTO ANY KNOB"
                subColor: page.panel.theme.faint
                accent: page.panel.sourceAccent(source)
                bodyHeight: tabs.height + modulation.gap + Math.max(modulation.editorNatural, sourceControls.height)
                Flow {
                    id: tabs
                    objectName: "novaModTabs"
                    width: modulation.body.width
                    spacing: modulation.gap / 2
                    Repeater {
                        model: Contract.envSources.concat(Contract.lfoSources)
                        delegate: NovaSourceChip {
                            required property string modelData
                            panel: page.panel
                            source: modelData
                            focusKey: modelData
                            text: page.panel.sourceName(modelData).toUpperCase()
                                + (modelData === Contract.ampEnvelope ? " · AMP" : "")
                        }
                    }
                    Repeater {
                        model: Contract.perfSources.concat(Contract.voiceSources)
                        delegate: NovaSourceChip {
                            required property string modelData
                            panel: page.panel
                            source: modelData
                        }
                    }
                    Repeater {
                        model: Contract.macroSources
                        delegate: NovaSourceChip {
                            required property string modelData
                            panel: page.panel
                            source: modelData
                            focusKey: "macros"
                        }
                    }
                }
                Loader {
                    id: editor
                    objectName: "novaModEditor"
                    y: tabs.height + modulation.gap
                    width: (modulation.body.width - modulation.gap) * page.editorShare
                    height: modulation.body.height - y
                    sourceComponent: modulation.envelope ? envelopeEditor : lfoEditor
                }
                // Each editor in its plot frame: level up, and for an LFO
                // the cycle across (an envelope's time axis is compressed
                // per stage, so it has no time scale).
                Component {
                    id: envelopeEditor
                    Kit.PlotFrame {
                        theme: page.panel.theme
                        live: page.panel.live
                        yUnit: "%"
                        yTicks: envelopeView.levelTicks
                        NovaEnvelopeView {
                            id: envelopeView
                            anchors.fill: parent
                            panel: page.panel
                            envelope: modulation.envelope ? modulation.source : Contract.ampEnvelope
                        }
                    }
                }
                Component {
                    id: lfoEditor
                    Kit.PlotFrame {
                        theme: page.panel.theme
                        live: page.panel.live
                        xUnit: "%"
                        yUnit: "%"
                        xTicks: curveEditor.xTicks
                        yTicks: curveEditor.yTicks
                        NovaCurveEditor {
                            id: curveEditor
                            anchors.fill: parent
                            panel: page.panel
                            lfo: modulation.envelope ? Contract.lfoSources[0] : modulation.source
                        }
                    }
                }
                Loader {
                    id: sourceControls
                    x: editor.width + modulation.gap
                    y: editor.y
                    width: modulation.body.width - x
                    height: item !== null ? item.implicitHeight : 0
                    sourceComponent: NovaSourceControls {
                        panel: page.panel
                        source: modulation.source
                    }
                }
            }

            Cell {
                id: macros
                key: "macros"
                title: "MACROS"
                expandable: false
                accent: page.panel.accentFor("nova.macro.")
                bodyHeight: macroKnobs.implicitHeight
                NovaMacros {
                    id: macroKnobs
                    panel: page.panel
                    compact: true
                    width: macros.body.width
                    height: implicitHeight
                }
            }

            // Under a large card: the rest of the overview, dimmed. A click
            // restores the card.
            Rectangle {
                id: scrim
                objectName: "novaScrim"
                visible: page.expandedCard !== null
                width: grid.width
                height: grid.height
                z: page.scrimZ
                color: page.panel.theme.scrim
                MouseArea {
                    // Esc, or ⤢ on the large card, restores it from the keyboard.
                    readonly property string keyboardAlternative: "Esc, or the large card's ⤢ button"
                    anchors.fill: parent
                    onClicked: page.panel.restore()
                    onWheel: wheel => wheel.accepted = false
                }
            }
        }
    }
}
