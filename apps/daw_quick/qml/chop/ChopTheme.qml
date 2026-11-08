import QtQuick
import "../devicekit"

// Chop design tokens: the shell's graphite with a hot coral accent, the blade. The gate and the
// chopped bass are coral, ratchets amber, repeats violet, chance teal; the
// bass as it comes in is slate, the playhead ink.
KitTheme {
    prefix: "chop"
    segmentHeight: 26
    segmentSpacing: 0
    segmentRadius: 7
    segmentLetterSpacing: 0.4
    knobLabelBold: true
    knobLabelSpacing: 1.2
    knobLabelSize: 9

    // The shell's graphite (ShellTheme).
    property color canvas: "#0b0e12"
    plate: "#11151b"
    plateEdge: "#222a33"
    plateHi: "#151b22"
    well: "#080a0d"
    wellEdge: "#222a33"
    seam: "#222a33"
    grid: "#222a33"
    gridStrong: "#2f3943"
    cardRadius: 9
    cardTitleInk: "#97a3ae"
    cardTitleSize: 10
    cardTitleSpacing: 1.6

    // The blade and the lanes' colours.
    property color blade: "#ff6b57"
    property color blade2: "#ff9a6b"
    property color teal: "#5fe0bf"
    property color amber: "#f5b14c"
    property color violet: "#a98bff"
    property color bass: "#3a4552"
    property color cellEven: "#0e131a"
    property color cellBeat: "#121820"

    accent: blade
    accent2: blade2
    accentSoft: "#7a3329"
    steel: "#5a2a22"
    segmentAccent: blade
    knobAccent: blade

    glacier: "#e6ebf0"
    ink: "#e6ebf0"
    inkDim: "#97a3ae"
    inkFaint: "#74828f"

    knobTop: "#1c232c"
    knobBottom: "#0b0e12"
    knobRing: "#2f3943"
    track: "#151b22"
    button: "#151b22"
    buttonHi: "#1c232c"
    ledOff: "#2f3943"

    warning: amber
    live: teal

    sans: "Inter"
    mono: "JetBrains Mono"

    // The control last touched, read back in the header.
    lastTouched: ""

    // The panel.
    property int panelWidth: 1240
    property int panelHeight: 760
    property real panelRadius: 12
    property real panelMargin: 14
    // Header: the blade glyph, the wordmark and the choice segments.
    property real headerHeight: 30
    property real glyphSize: 26
    property real wordmarkGap: 10
    property int wordmarkSize: 24
    // From the wordmark to the segments.
    property real segmentsGap: 22
    property real modeSegmentWidth: 300
    property real stepsSegmentWidth: 140
    property real divisionSegmentWidth: 170
    // The last-touched readout at the header's right.
    property real readoutWidth: 220
    property real readoutMargin: 14
    property real readoutSpacing: 1.2
    // Pattern preset chips.
    property real chipHeight: 28
    // The chip's width beyond its label.
    property real chipPad: 24
    // The hero: one pattern of the bass with the gate over it.
    property real heroHeight: 190
    property real heroRadius: 9
    // The gate curve's top band and its height, and the waveform's margin
    // from the hero's top and bottom.
    property real heroCurveTop: 10
    property real heroCurveHeight: 30
    property real heroWaveMargin: 12
    property real heroCurveWidth: 1.5
    property real playheadWidth: 1.5
    // Shade over the steps chance skips this time round (teal tint).
    property real skipShadeAlpha: 0.06
    // The heard bass over the input while playing, and the gated preview bar.
    property real heardAlpha: 0.9
    property real previewGateAlpha: 0.55
    // A repeat mark's inset from its step's left and its baseline from the bottom.
    property real repeatMarkInset: 4
    property real repeatMarkBaseline: 8
    // The "play to see" hint at the hero's top right.
    property real hintInset: 10
    property real hintY: 8
    property real hintSpacing: 1.4
    // The lanes: name column, heights, cells.
    property real laneTallHeight: 70
    property real laneHeight: 46
    property real laneNameWidth: 86
    // Space between the lane's names and its cells.
    property real laneNamePad: 10
    property real laneTitleSpacing: 1.2
    property real laneRadius: 7
    property real laneCellGap: 1
    // A cell's tile inset inside its slot.
    property real cellInset: 1
    property real silentOpacity: 0.45
    // Continuous lanes: a wash to the value topped by a bright edge.
    property real washInset: 2
    property real washAlpha: 0.25
    property real washEdge: 2.5
    // Stepped lanes: a block per count over a floor; repeat "off" a dash.
    property real blockInset: 3
    property real blockGap: 3
    property real blockFloor: 6
    // The cell height not used by blocks.
    property real blockColumnPad: 8
    property real repeatOffHeight: 2
    // The knob card's row below its title, and the knobs' spacing.
    property real knobRowY: 24
    property real knobSpacing: 52
}
