import QtQuick
import "../devicekit"

// Clay's look: the DAW shell's dark panels with
// the clay accent for notes and the tool in hand.
KitTheme {
    prefix: "clay"

    readonly property color bg: "#07090c"
    readonly property color panel: "#11151b"
    readonly property color panel2: "#151b22"
    readonly property color panel3: "#1a212a"
    readonly property color line: "#222a33"
    readonly property color line2: "#2f3943"
    readonly property color dim: "#97a3ae"
    readonly property color faint: "#5d6873"
    readonly property color clay: "#e8875c"
    readonly property color clay2: "#f6b48e"
    readonly property color clay3: "#b9603c"
    readonly property color clayText: "#1b0e07"
    readonly property color teal: "#5fe0bf"
    readonly property color blue: "#6aa7ff"
    readonly property color violet: "#a98bff"
    readonly property color pink: "#ff6fb5"
    readonly property color amber: "#f5b14c"
    readonly property color green: "#5fe3a1"
    readonly property color red: "#ff5d6c"
    readonly property int radius: 7
    readonly property int controlHeight: 26
    readonly property int labelSize: 9
    readonly property int textSize: 11
    readonly property real labelSpacing: 1.2
    readonly property real disabledOpacity: 0.35

    // Clay washes: the accent behind a lit tool, the newest history row,
    // the selection box and the "only here" badge; the detection sweep's glow.
    readonly property color toolLitFill: Qt.rgba(0.91, 0.53, 0.36, 0.16)
    readonly property color toolLitEdge: Qt.rgba(0.91, 0.53, 0.36, 0.6)
    readonly property color historyCurrentFill: Qt.rgba(0.91, 0.53, 0.36, 0.13)
    readonly property color selectionFill: Qt.rgba(0.91, 0.53, 0.36, 0.12)
    readonly property color badgeEdge: Qt.rgba(0.91, 0.53, 0.36, 0.5)
    readonly property color sweepGlow: Qt.rgba(0.96, 0.71, 0.56, 0.32)
    // Text on the teal A/B badge.
    readonly property color tealText: "#08130f"

    // The toolbar: gaps between groups and between tool buttons, a tool
    // button's side padding and inner gap, its shortcut box, the key picker
    // and the detection progress bar.
    readonly property int toolbarGroupGap: 14
    readonly property int toolButtonSpacing: 3
    readonly property int toolButtonPad: 14
    readonly property int toolButtonInnerGap: 5
    readonly property int shortcutBoxSize: 13
    readonly property int keyPickerWidth: 132
    readonly property int progressWidth: 140
    readonly property int progressHeight: 5

    // The empty state's card.
    readonly property int emptyCardMaxWidth: 520
    readonly property int emptyCardPad: 20
    readonly property int emptyCardGap: 10
    readonly property real emptyCardRadius: 12
    readonly property real bodyLineHeight: 1.3

    // The footer: its height, side inset, the room its buttons keep from the
    // status line, and the render state's pulsing dot.
    readonly property int footerHeight: 36
    readonly property int footerInset: 10
    readonly property int footerButtonsWidth: 720
    readonly property int renderDotSize: 7
    readonly property real pulseLowOpacity: 0.3
    readonly property int pulseDuration: 450

    // The side panel: its width, the inspector's line height and the
    // history rows.
    readonly property int sidePanelWidth: 318
    readonly property real inspectorLineHeight: 1.35
    readonly property int historyRowHeight: 22
    readonly property real historyRowRadius: 5

    // ClayPicker: its width when not sized by the layout.
    readonly property int pickerWidth: 160

    // ClaySection: its padding, the gap under the heading and between
    // controls, the heading's dot and spacing, and the "only here" badge.
    readonly property int sectionPadding: 13
    readonly property int sectionDefaultWidth: 300
    readonly property int sectionGap: 9
    readonly property int sectionDotSize: 6
    readonly property real sectionTitleSpacing: 1.6
    readonly property int badgePad: 14
    readonly property int badgeHeight: 16
    readonly property real badgeSpacing: 0.8

    // ClayCanvas: piano ruler keys and scale marks.
    readonly property color pianoWhiteKey: "#1d242c"
    readonly property real pianoKeyInset: 10
    readonly property real pianoKeyGap: 0.5
    readonly property real scaleMarkRight: 7
    readonly property real scaleMarkWidth: 4
    readonly property real tonicMarkAlpha: 0.9
    readonly property real scaleMarkAlpha: 0.55
    // Rows shorter than this carry no octave label.
    readonly property real octaveLabelMinRow: 6
    // The time ruler: bar ticks, beat ticks and labels, shown once beats are
    // wide enough.
    readonly property real rulerFontSize: 9.5
    readonly property real barTickTop: 6
    readonly property real beatTickHeight: 6
    readonly property real beatLabelOffset: 3
    readonly property real beatTickMinPixels: 26
    readonly property real beatLabelMinPixels: 54
    // The plot: scale rows, the root's tint, grid lines (sixteenths once a
    // beat is wide enough) and the waveform behind.
    readonly property color scaleRowFill: "#0c1015"
    readonly property real tonicRowAlpha: 0.05
    readonly property color gridLineInk: "#ffffff"
    readonly property real barLineAlpha: 0.09
    readonly property real beatLineAlpha: 0.045
    readonly property real stepLineAlpha: 0.02
    readonly property real sixteenthMinPixels: 48
    readonly property real waveformAlpha: 0.05
    // Notes: the blob's fill by state, its outline, the noise hatching, the
    // pitch curves, the formant mark and the lyric.
    readonly property real mutedNoteAlpha: 0.18
    readonly property real selectedNoteAlpha: 0.62
    readonly property real noteAlpha: 0.38
    readonly property real selectedNoteStroke: 1.6
    readonly property real hatchMaxAlpha: 0.35
    readonly property real hatchAlphaPerNoise: 0.45
    readonly property real originalCurveAlpha: 0.6
    readonly property real curveAlpha: 0.9
    readonly property real mutedCurveAlpha: 0.35
    readonly property real curveStroke: 1.4
    readonly property real formantMarkLift: 8
    readonly property real formantMarkHeight: 2
    readonly property real formantMarkMin: 6
    readonly property real formantMarkMax: 28
    readonly property real formantLabelLift: 3
    readonly property real formantFontSize: 8.5
    readonly property real lyricDrop: 12
    // Overlays: the playhead, the A/B badge and the detection sweep.
    readonly property real playheadWidth: 1.5
    readonly property real abBadgeX: 10
    readonly property int abBadgeHeight: 20
    readonly property real sweepWidth: 120

    well: "#080a0d"
    ink: "#e6ebf0"
    sans: "Inter"
    mono: "JetBrains Mono"
    accent: clay
    accentInk: clayText
    tipIsName: true

    // The kit's Slider as a row: a spaced mono label, a well with a clay
    // fill and an edged handle, and the readout.
    sliderStacked: false
    sliderWidth: 260
    sliderHeight: 22
    sliderLabelWidth: 62
    sliderValueWidth: 46
    sliderGap: spaceSm
    sliderGrooveInset: false
    sliderGroove: well
    sliderGrooveEdge: line
    sliderGrooveFocusEdge: clay2
    sliderHandle: 12
    sliderHandleEdge: undefined
    sliderHandleEdgeWidth: strokeBold
    sliderLabelFont: mono
    sliderLabelSize: labelSize
    sliderLabelWeight: Font.DemiBold
    sliderLabelSpacing: labelSpacing
    sliderLabelInk: faint
    sliderLabelFocusInk: faint
    sliderValueFont: mono
    sliderValueSize: textSize
    sliderValueWeight: Font.DemiBold
    sliderValueInk: ink
    sliderValueDragInk: ink
    sliderValueElide: false
    sliderResizeCursor: false

    // The kit's Button: plain on the well, lit clay when on or prime.
    pushButtonHeight: controlHeight
    pushButtonRadius: radius
    pushButtonTextSize: textSize
    pushButtonFace: well
    pushButtonHover: panel3
    pushButtonIdleEdge: line2
    pushButtonHoverEdge: faint
    pushButtonFocusEdge: clay2
    pushButtonIdleInk: dim
    pushButtonHoverInk: ink

    // The kit's SegmentedControl: cells fitted to their text in a well.
    segmentedHeight: controlHeight
    segmentedRadius: radius
    segmentedCellRadius: radius - hairline
    segmentedFrame: well
    segmentedFrameEdge: line2
    segmentedClip: true
    segmentedDividers: true
    segmentedDivider: line2
    segmentedTextSize: textSize
    segmentedHover: panel3
    segmentedIdleInk: dim
    segmentedHoverInk: ink
    segmentedTips: true
    segmentedRadios: true
    function segmentedEdge(cell) {
        return "transparent"
    }
}
