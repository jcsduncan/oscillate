import QtQuick
import "../devicekit"

// Loudness design tokens: a broadcast-grade instrument. A graphite case,
// slate plates and deep recessed wells; one calm teal accent, large precise
// numerals in white, and traffic-light status against the target (on target
// green, close amber, off red).
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "loudness"

    // The case: graphite, a touch lighter at the top edge.
    caseTop: "#202326"
    caseColor: "#17191b"
    caseBottom: "#101213"
    // Slate plates and the recessed wells inside them.
    plate: "#1b1e20"
    plateEdge: "#2d3134"
    plateHi: "#3c4145"
    well: "#0d0f10"
    wellEdge: "#2a2e31"
    seam: "#24282b"
    grid: "#1f2326"
    gridStrong: "#323739"

    // The two readings: short-term in teal, momentary in a paler teal.
    property color shortTerm: "#3fd1c0"
    property color momentary: "#9fe7df"
    property color peak: "#d9e3e6"

    // Status against the target.
    property color onTarget: "#5fd38a"
    property color near: "#f2b25c"
    property color off: "#ff5a52"

    // Controls: teal arcs, a white index.
    accent: "#3fd1c0"
    accentSoft: "#2a8f84"
    steel: "#256f67"
    glacier: "#f2f6f7"
    property color glint: "#ffffff"

    ink: "#eef2f3"
    inkDim: "#9aa5a9"
    inkFaint: "#7e888d"

    knobTop: "#3b4043"
    knobBottom: "#121415"
    track: "#25292c"
    button: "#15181a"
    buttonHi: "#1e2225"

    warning: "#f2b25c"
    live: "#5fd38a"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // ---- Panel layout ----------------------------------------------------------
    property int panelWidth: 1240
    property int panelHeight: 760
    property real caseRadius: 10
    // Where the case gradient reaches caseColor.
    property real caseMidStop: 0.35
    // Between plates.
    property real panelGap: 10
    // A focused or alarmed edge.
    property real focusStroke: 1.5

    // ---- Header ------------------------------------------------------------
    property real headerTop: 10
    property real headerHeight: 54
    property int wordmarkSize: 27
    property real wordmarkSpacing: 9
    // The accent rule under the wordmark: its gap below, thickness, and how
    // much shorter than the word it runs.
    property real underlineGap: 1
    property real underlineWidth: 2
    property real underlineTrim: 10
    property real taglineGap: 7
    property real taglineSpacing: 2.4
    // The preset browser.
    property real browserX: 250
    property real browserY: 5
    property real presetWidth: 540
    property real presetHeight: 42
    property real presetInset: 14
    property real presetCaptionY: 5
    property real presetCaptionSpacing: 1.4
    property real presetNameY: 16
    property real presetNameWidth: 250
    property real presetNameSpacing: 0.4
    property real presetDividerX: 276
    property real presetDividerInset: 8
    property real readoutX: 288
    property real readoutValueY: 17
    // Room right of the readout for the chevron.
    property real readoutRight: 26
    property real readoutSpacing: 1.2
    property real chevronMargin: 10
    property real statusTop: 14

    // ---- Plates ------------------------------------------------------------
    // The readings row, the history row, and the first control under a
    // plate's title.
    property real rowATop: 76
    property real rowAHeight: 330
    property real rowBHeight: 180
    property real cardBodyY: 30
    // Integrated: the big number, its label, the distance to target, the
    // rule and the facts row under them.
    property real integratedWidth: 420
    property real integratedX: 18
    property real integratedY: 34
    property int integratedSize: 112
    property real integratedLabelX: 24
    property real integratedLabelY: 166
    property real integratedLabelSpacing: 3
    property real deltaX: 22
    property real deltaY: 192
    property int deltaSize: 26
    property real integratedRuleY: 240
    property real factsX: 22
    property real factsY: 254
    property real factsSpacing: 34
    property real factGap: 3
    property real factCaptionSpacing: 1.4
    property int factSize: 17
    // The status lamp and its halo (a tint of the lamp's colour).
    property real lampTop: 12
    property real lampSize: 12
    property real lampHaloSize: 28
    property real lampHaloAlpha: 0.16
    // Now: the reading tiles.
    property real nowWidth: 480
    property real tileRadius: 5
    property real tileCaptionY: 9
    property real tileCaptionSpacing: 1.6
    property real tileValueY: 24
    property int tileValueSize: 36
    property real tileUnitBaseline: 62
    property real tileUnitSpacing: 1.2
    // The meter and history plates' content tops.
    property real meterPlateTop: 28
    property real historyPlateTop: 26
    // Controls.
    property real targetWidth: 210
    property real measureWidth: 300
    property real scaleWidth: 230
    property real knobSpacing: 14
    property int knobLargeDiameter: 44
    property real plateInset: 14
    property real plateControlY: 32
    property real toggleHeight: 42
    property real measureNoteY: 84
    property real scaleNoteY: 70
    property real keyDotSize: 9

    // ---- Bar meter -------------------------------------------------------------
    property real meterBarTop: 18
    property real meterBarBottom: 18
    property real meterScaleWidth: 30
    property real meterBarWidth: 30
    // From the loudness bars to the true-peak bars.
    property real meterPeakGap: 40
    property real meterPeakWidth: 16
    // How far scale ticks and the ceiling line reach past the bars.
    property real meterTickOverhang: 4
    property real meterPeakTickOverhang: 3
    property real meterCeilingOverhang: 5
    property real meterCeilingWidth: 2
    // The fill's inset inside its well, and its maximum tick.
    property real meterFillInset: 2
    property real meterMaxTick: 2
    property real meterPeakFillRadius: 1
    property real meterPeakFillOpacity: 0.85
    // A loudness bar's gradient: lightened at the top, faded at the bottom.
    property real meterFillLift: 1.15
    property real meterFillBottomAlpha: 0.45
    property color zeroTick: Qt.rgba(accent.r, accent.g, accent.b, 0.55)
    property real meterNameSpacing: 1.4
    // The units' captions above the bars.
    property real meterUnitLift: 12
    property real meterUnitSpacing: 1
    // A scale label's lift to centre it on its tick.
    property real scaleLabelLift: 6

    // ---- History ------------------------------------------------------------
    // The plot's margins: the LUFS labels on the left, the time labels below.
    property real historyScaleWidth: 44
    property real historyTop: 8
    property real historyRight: 10
    property real historyBottom: 22
    property real timeLabelGap: 5
    property color toleranceFill: Qt.rgba(onTarget.r, onTarget.g, onTarget.b, 0.10)
    property color targetLine: Qt.rgba(accent.r, accent.g, accent.b, 0.8)
    property var targetDash: [5, 4]
    property color momentaryLine: Qt.rgba(momentary.r, momentary.g, momentary.b, 0.55)
    // The short-term fill's alpha at the top and the bottom of the plot.
    property real traceFillTopAlpha: 0.42
    property real traceFillBottomAlpha: 0.04
    property real traceEdgeWidth: 2
}
