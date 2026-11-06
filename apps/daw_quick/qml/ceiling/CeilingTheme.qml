import QtQuick
import "../devicekit"

// Ceiling design tokens: a mastering brickwall. A cool charcoal case, slate
// plates and a deep screen; the ceiling and the controls in coral, the gain
// reduction hanging from the top in a red that deepens as it bites, and the
// ceiling line drawn across the output meters.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "ceiling"

    // The case: charcoal, a touch lighter at the top edge.
    caseTop: "#211e21"
    caseColor: "#181618"
    caseBottom: "#100f10"
    // Where the gradient reaches caseColor.
    property real caseMidStop: 0.35
    plate: "#1c1a1c"
    plateEdge: "#2f2b2f"
    plateHi: "#433d43"
    well: "#0c0b0c"
    wellEdge: "#2a262a"
    seam: "#262226"

    // The history screen.
    property color screen: "#0a090a"
    grid: "#1c191c"
    gridStrong: "#2e292e"

    // Gain reduction and the ceiling line.
    property color reduction: "#ff6b6b"
    property color reductionDeep: "#ff3d4f"
    property color ceilingLine: "#ffb0b0"
    // The reduction fill: faint at 0 dB, deeper where it bites.
    property real reductionFillTopAlpha: 0.10
    property real reductionFillBottomAlpha: 0.55
    property real reductionStroke: 1.6

    accent: "#ff8a8a"
    accentSoft: "#a34f4f"
    steel: "#7a3a3a"
    glacier: "#fbf1f1"
    ink: "#f1eaea"
    inkDim: "#a89c9e"
    inkFaint: "#6b6163"

    knobTop: "#4a4247"
    knobBottom: "#131113"
    track: "#2a2529"
    button: "#171517"
    buttonHi: "#201d20"
    ledOff: "#302b2f"

    warning: "#f2b25c"
    live: "#7fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The panel's layout.
    property int panelWidth: 1240
    property int panelHeight: 640
    property real panelRadius: 10
    property real cardGap: 10
    property real cardInset: 14
    // Where a card's content starts below its title.
    property real cardContentY: 34
    property real noteBottom: 26
    property real captionSpacing: 1.4
    property real focusStroke: 1.5
    // A control with nothing to act on.
    property real idleOpacity: 0.45

    // The header: wordmark, preset browser and status.
    property real headerY: 10
    property real headerHeight: 54
    property int wordmarkSize: 27
    property real wordmarkSpacing: 9
    // How much shorter than the wordmark its accent rule is.
    property real wordmarkRuleTrim: 10
    property real latencyGap: 7
    property real latencySpacing: 2.4
    property real browserX: 250
    property real browserY: 5
    property real presetDisplayWidth: 540
    property real presetDisplayHeight: 42
    property real presetTextX: 14
    property real presetCaptionY: 5
    property real presetNameY: 16
    property real presetNameWidth: 250
    property real presetNameSpacing: 0.4
    property real presetDividerX: 276
    property real readoutX: 288
    property real readoutValueY: 17
    property real readoutRight: 26
    property real readoutSpacing: 1.2
    property real chevronMargin: 10
    property real statusY: 14

    // Row A: the reduction history and the level meters.
    property real rowAY: 76
    property real rowAHeight: 330
    property real meterPlateWidth: 250
    property real historyY: 30
    property real historyBottomTrim: 40
    // The history's dB scale on the left, margin on the right, time axis below.
    property real historyScaleWidth: 40
    property real historyRightPad: 10
    property real historyAxisHeight: 22
    // Lifts a dB label to centre it on its grid line.
    property real historyLabelLift: 6
    property real historyTimeGap: 5
    property real reductionColumnWidth: 150
    property real reductionColumnInner: 140
    property real reductionColumnY: 44
    property int reductionNowSize: 44
    property int reductionLargestSize: 22
    property real reductionSpacer: 26
    property real clearSpacer: 18
    property real clearWidth: 110
    property real clearHeight: 26
    property real clearSpacing: 1.6
    property real meterHeight: 230
    property real meterPairWidth: 80
    // The peak readouts and caption above a pair of meters.
    property real meterPairChrome: 54
    property real peakRowY: 14
    property real peakTextWidth: 36
    property real meterBarsY: 32
    property real meterBarWidth: 20
    // The PeakMeter's inset from its edge to the scale.
    property real meterInset: 2

    // Row B: gain, ceiling, lookahead and release.
    property real gainPlateWidth: 270
    property real ceilingPlateWidth: 360
    property real lookaheadPlateWidth: 290
    property real heroKnobX: 18
    property int heroKnobDiameter: 104
    property int smallKnobDiameter: 62
    property real gainValueX: 140
    property real gainValueY: 66
    property int gainValueSize: 34
    property real gainUnitGap: 5
    property real gainNoteY: 148
    property real ceilingValueX: 172
    property real ceilingValueY: 58
    property int ceilingValueSize: 48
    property real ceilingNoteY: 128
    property real sideNoteX: 120
    property real sideNoteY: 40

    // The control last touched, read back in the display.
    lastTouched: ""
}
