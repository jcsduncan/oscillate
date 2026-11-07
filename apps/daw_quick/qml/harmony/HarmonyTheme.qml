import QtQuick
import "../devicekit"

// Harmony's look: the DAW shell's graphite with a violet accent for chords
// and amber for Comp. Tension runs teal (at rest) to pink (most pull); the
// mood pad runs a dark violet to a dark amber.
KitTheme {
    prefix: "harmony"

    readonly property color bg: "#0b0d10"
    readonly property color panel: "#11151b"
    readonly property color panel2: "#161b22"
    readonly property color panel3: "#1a212a"
    readonly property color line: "#222a33"
    readonly property color line2: "#2f3943"
    readonly property color dim: "#97a3ae"
    readonly property color faint: "#7e8a97"
    readonly property color violet: "#a98bff"
    readonly property color violetText: "#140d24"
    readonly property color amber: "#f5b14c"
    readonly property color amberText: "#1f1404"
    readonly property color pink: "#ff6fb5"
    readonly property color teal: "#5fe0bf"
    readonly property color moodDark: "#1c1631"
    readonly property color moodBright: "#3a2b12"
    readonly property int radius: 7
    readonly property int controlHeight: 26
    readonly property int labelSize: 9
    readonly property int textSize: 11
    readonly property real labelSpacing: 1.2
    readonly property int gap: 14
    readonly property int pad: 12
    // Tension hue: teal (165°) at rest to pink (330°) at most pull.
    readonly property real tensionHueFrom: 165 / 360
    readonly property real tensionHueTo: 330 / 360
    readonly property real tensionSaturation: 0.8
    readonly property real tensionLightness: 0.64

    // Gaps between items in a row or column, and between groups of controls.
    readonly property int itemGap: 5
    readonly property int groupGap: 10
    // Spaced mono captions ("CHORDS", "RESTLESS").
    readonly property real captionSpacing: 1
    // Notes: wrapped help text.
    readonly property real noteLineHeight: 1.25
    readonly property int chordNameSize: 28

    // The chord strip: blocks inset from the strip and each other, a
    // tension rule along each block's foot, the roman numeral only on
    // blocks wide enough for it.
    readonly property int stripBlockGap: 1
    readonly property int stripBlockMarginY: 3
    readonly property int stripBlockMinWidth: 4
    readonly property real stripLitAlpha: 0.28
    readonly property int tensionBarHeight: 2
    readonly property real tensionBarOpacity: 0.8
    readonly property int stripRomanMinWidth: 90

    // The panel: tab bar, section headers, cards and picks.
    readonly property int tabBarHeight: 28
    readonly property int sectionHeaderHeight: 22
    readonly property real cardLitAlpha: 0.10
    readonly property int tagHeight: 18
    readonly property real tagFillAlpha: 0.08
    readonly property real tagEdgeAlpha: 0.4
    readonly property int pickWidth: 38
    readonly property int pickHeight: 24
    readonly property real pickRadius: 5
    readonly property int amountHeight: 18
    readonly property real amountFillAlpha: 0.55
    readonly property int patternCardHeight: 38
    readonly property int patternTextX: 9
    readonly property int patternTitleY: 5
    readonly property int patternSummaryY: 21
    readonly property int shapeCardHeight: 44
    readonly property int shapeCardInset: 7
    readonly property int shapeSummaryY: 25
    readonly property int shapeGlyphWidth: 34
    readonly property int shapeGlyphHeight: 12
    readonly property real shapeGlyphStroke: 1.4
    readonly property int suggestionPadX: 10
    readonly property int suggestionPadY: 7
    readonly property int hintPadX: 9
    readonly property real hintAlpha: 0.06
    readonly property real hintHoverAlpha: 0.12
    readonly property real hintEdgeAlpha: 0.45
    readonly property real hintAppliedOpacity: 0.6
    readonly property int moodPadMaxWidth: 150
    readonly property real moodPadShare: 0.48
    readonly property int nextGraphMaxWidth: 220

    // The mood pad: a glow from the top, faint grid lines, the dot in its halo.
    readonly property real moodGlowAlpha: 0.16
    readonly property real moodGridAlpha: 0.06
    readonly property int moodHaloSize: 30
    readonly property real moodHaloAlpha: 0.16
    readonly property int moodDotSize: 14

    // The next-chord graph: dashed likelihood rings, spokes weighted by
    // likelihood, the current chord's hub and the candidates' nodes.
    readonly property var graphRingDash: [2, 4]
    readonly property real graphSpokeAlphaMin: 0.15
    readonly property real graphSpokeAlphaRange: 0.5
    readonly property real graphSpokeWidthMin: 1
    readonly property real graphSpokeWidthRange: 3
    readonly property int graphHubSize: 60
    readonly property int graphNodeSize: 46
    readonly property real graphNodeStroke: 1.5
    readonly property real graphNodeFocusStroke: 2.5

    well: "#080a0d"
    ink: "#e6ebf0"
    inkDim: dim
    inkFaint: faint
    inkGround: panel3
    sans: "Inter"
    mono: "JetBrains Mono"
    cardRadius: 10
    accent: violet
    accentInk: violetText
    tipIsName: true

    // The kit's Button: bold spaced mono, lit in the view's accent.
    pushButtonHeight: controlHeight
    pushButtonPad: 22
    pushButtonRadius: radius
    pushButtonFont: mono
    pushButtonTextSize: textSize
    pushButtonWeight: Font.Bold
    pushButtonLetterSpacing: 0.6
    pushButtonFace: panel2
    pushButtonHover: panel3
    pushButtonIdleEdge: line2
    pushButtonHoverEdge: faint
    pushButtonFocusEdge: ink
    pushButtonIdleInk: dim
    pushButtonHoverInk: ink

    // The kit's SegmentedControl: square cells fitted to their text in a
    // well, ruled apart.
    segmentedHeight: controlHeight
    segmentedRadius: radius
    segmentedFrame: well
    segmentedFrameEdge: line2
    segmentedClip: true
    segmentedDividers: true
    segmentedDivider: line
    segmentedFontFamily: mono
    segmentedTextSize: textSize
    segmentedHover: panel3
    segmentedIdleInk: dim
    segmentedHoverInk: ink
    segmentedFocusEdge: ink

    function tensionColor(tension) {
        const t = Math.max(0, Math.min(100, Number(tension) || 0)) / 100
        return Qt.hsla(tensionHueFrom + (tensionHueTo - tensionHueFrom) * t, tensionSaturation,
                       tensionLightness, 1)
    }
    function alpha(colour, amount) {
        return Qt.rgba(colour.r, colour.g, colour.b, amount)
    }
    function list(value) {
        return value !== null && value !== undefined && typeof value !== "string"
               && typeof value.length === "number" ? value : []
    }
    function map(value) {
        return value !== null && value !== undefined && typeof value === "object" ? value : ({})
    }
}
