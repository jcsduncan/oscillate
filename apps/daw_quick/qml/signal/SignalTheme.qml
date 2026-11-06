import QtQuick
import "../devicekit"

// Signal design tokens: bench test gear. A warm putty-grey instrument
// case with dark recessed bays, black knurled knobs, and a deep LCD window
// glowing in signal orange; the two channels in sky (left) and coral
// (right) on the output diagram.
KitTheme {
    // The device kit: identity and widget styling.
    prefix: "signal"
    cardSheen: Qt.rgba(1, 1, 1, 0.05)

    // The case: warm grey enamel, lighter at the top edge.
    caseTop: "#3a3834"
    caseColor: "#2d2c29"
    caseBottom: "#22211f"
    // Bays and the recessed wells inside them.
    plate: "#1b1a18"
    plateEdge: "#45423c"
    plateHi: "#57534b"
    well: "#0f0e0d"
    wellEdge: "#3a3732"
    seam: "#33302b"

    // The LCD window.
    property color lcd: "#120c06"
    property color lcdEdge: "#4a3418"
    property color lcdGrid: "#2a1c0c"
    property color lcdGridStrong: "#45301a"
    property color lcdInk: "#ff9a3c"
    property color lcdGlow: "#c25e12"
    property color lcdDim: "#7a4a1f"
    property color lcdGhost: "#2f200f"

    // The output diagram.
    property color left: "#5cb8ff"
    property color right: "#ff8a6b"
    property color invert: "#ff5d55"

    // Controls: orange arcs on black knobs, a pale index.
    accent: "#ff9a3c"
    accentSoft: "#a85a1c"
    steel: "#8a4a14"
    glacier: "#f6efe6"
    property color glint: "#ffffff"
    gridStrong: "#57534b"

    ink: "#f1ebe2"
    inkDim: "#aaa197"
    inkFaint: "#6f685f"

    knobTop: "#3e3b37"
    knobBottom: "#0b0a09"
    track: "#2c2a26"
    button: "#171614"
    buttonHi: "#211f1c"

    // The power switch.
    property color power: "#ff5d3a"

    warning: "#f2b25c"
    live: "#7ee08f"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The control last touched, read back in the display.
    lastTouched: ""

    // The panel: its size, the case's corners and where the case gradient
    // reaches its body colour.
    property int panelWidth: 1240
    property int panelHeight: 740
    property real caseRadius: 10
    property real caseMidStop: 0.4
    // The gap between plates.
    property real plateGap: 10
    // Small bold captions and tags.
    property real captionSpacing: 1.4
    property real tagSpacing: 1.2
    // A focused control's edge, and a control that cannot be edited.
    property real focusStroke: 1.5
    property real disabledOpacity: 0.5

    // Rack screws in the case's corners.
    property real screwInset: 5
    property real screwSize: 10
    property real screwSlotLength: 6
    property real screwSlotWidth: 1.5
    property real screwSlotAngle: 35

    // The header: the wordmark, its underline and subtitle.
    property real headerInset: 4
    property real headerY: 10
    property real headerHeight: 54
    property real wordmarkY: 2
    property int wordmarkSize: 27
    property real wordmarkSpacing: 9
    // The underline and subtitle sit this far in from the wordmark's left.
    property real wordmarkInset: 2
    property real underlineGap: 1
    // How much shorter than the wordmark the underline runs.
    property real underlineTrim: 10
    property real subtitleGap: 7
    property real subtitleSpacing: 2.4

    // The preset browser and the readout of the last-touched control.
    property real browserX: 246
    property real browserY: 5
    property int presetDisplayWidth: 540
    property int presetDisplayHeight: 42
    property real presetTextX: 14
    property real presetCategoryY: 5
    property real presetNameY: 16
    property real presetNameWidth: 250
    property real presetNameSpacing: 0.4
    property real presetDividerX: 276
    // The divider's inset from the display's top and bottom.
    property real presetDividerInset: 8
    property real readoutX: 288
    property real readoutValueY: 17
    // Room kept right of the readout for the chevron.
    property real readoutRight: 26
    property real chevronMargin: 10
    property real statusY: 14

    // Plates: padding, the y of the first control, the row heights.
    property real cardPad: 14
    property real cardBodyY: 34
    property real rowAY: 76
    property real rowAHeight: 340
    property int displayWidth: 820
    property int waveWidth: 520
    property int toneWidth: 400

    // The LCD window inside the display plate.
    property real lcdInset: 12
    property real lcdY: 30
    property real lcdBottom: 12
    property real lcdRadius: 6
    // Glass: a faint warm sheen over the top of the window.
    property real glassShare: 0.4
    property color glassSheenTop: Qt.rgba(1, 0.8, 0.6, 0.05)
    property color glassSheenBottom: Qt.rgba(1, 0.8, 0.6, 0)
    // The wave line: its glyph, name and the output summary.
    property real lcdTextX: 20
    property real lcdGlyphY: 18
    property real lcdGlyphWidth: 44
    property real lcdGlyphHeight: 24
    property real lcdGlyphStroke: 1.6
    property real waveNameX: 74
    property real waveNameY: 20
    property real waveNameSpacing: 3
    property real outputNameY: 22
    property real lcdLabelSpacing: 1.6
    // The big digits, their unit, and the line under them.
    property real bigY: 50
    property int bigSize: 78
    property real bigSpacing: 2
    property real bigGlowAlpha: 0.35
    property real bigUnitGap: 10
    property int bigUnitSize: 26
    property real subX: 24
    property real subY: 150
    property real subOpacity: 0.85
    // The level at the top right of the digits.
    property real levelRight: 22
    property real levelY: 62
    property int levelSize: 38
    // The frequency strip along the bottom of the window.
    property real axisY: 186
    property real axisBottom: 14
    property real axisTop: 4
    // The band under the strip where its labels sit.
    property real axisLabelBand: 18
    property real axisLabelBaseline: 4
    property real toneGlowHalfWidth: 14
    property real toneGlowAlpha: 0.45
    property real sweepFillStartAlpha: 0.08
    property real sweepFillEndAlpha: 0.45
    property real slopeFillAlpha: 0.14
    property real axisCaptionX: 6
    property real axisCaptionY: 12

    // The output plate: the power rocker, mode and routing diagram.
    property real powerY: 32
    property real powerHeight: 54
    property real powerFillAlpha: 0.16
    property real powerEdgeAlpha: 0.8
    property real powerGlyphSize: 30
    property real powerGlyphStroke: 2.4
    property real powerLegendX: 56
    property real powerLegendSpacing: 3
    property real powerNoteSpacing: 1
    property real modeCaptionY: 98
    property real modeY: 112
    property real modeHeight: 28
    property real routingCaptionY: 152
    property real routingY: 166
    property real routingHeight: 92
    // The space between the two speakers, for the alternation mark.
    property real speakerGap: 56
    property real speakerEdgeAlpha: 0.75
    property real speakerTextX: 10
    property real speakerNameY: 8
    property real speakerTagY: 9
    property real speakerTagSpacing: 1
    property real speakerGlyphX: 12
    property real speakerGlyphY: 34
    property real speakerGlyphHeight: 40
    // A silent speaker's flat line, across the glyph's middle.
    property real speakerSilentY: 54
    property int routingGlyphSize: 20
    property real outputSegmentY: 268
    property real outputSegmentHeight: 26
    property real outputNoteY: 302

    // The wave plate: five pictured tiles.
    property real wavesY: 32
    property real wavesHeight: 120
    property real tileFillAlpha: 0.12
    property real tileEdgeAlpha: 0.7
    property real tileScreenInset: 8
    property real tileScreenY: 10
    property real tileScreenHeight: 62
    property real tileGlyphInset: 8
    property real tileGlyphStroke: 1.8
    property real tileLabelY: 82
    property real tileLabelInset: 8
    property real tileLabelSpacing: 1.6
    // The rule under the chosen tile.
    property real tileRuleBottom: 12
    property real tileRuleWidth: 24
    property real tileRuleHeight: 3
    property real waveNoteY: 166

    // The tone and timing plates.
    property real toneKnobX: 26
    property real levelKnobX: 220
    property int bigDialDiameter: 84
    property real dialNoteY: 196
    property real dialNoteWidth: 110
    property real levelNoteSpacing: 2.4
    property real plateNoteY: 222
    property real timingPad: 18
    property real progressY: 200
    property real progressHeight: 8
    property real timingIdleNoteY: 60
}
