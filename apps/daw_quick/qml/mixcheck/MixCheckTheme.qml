import QtQuick
import "../devicekit" as Kit

// Mix Check's palette: the shell's tokens plus the check's own colours.
// Problems are red or amber, fixed is the teal accent, the "after" curve is
// white over the grey "before". The helpers keep every part of the view safe
// before a first run, when the controller's lists and maps are still empty
// or undefined.
Kit.ShellTheme {
    id: style

    readonly property color red: "#ff5d6c"
    readonly property color blue: "#6aa7ff"
    readonly property color wellFill: "#080a0d"
    readonly property color beforeCurve: "#4a5562"
    readonly property color afterCurve: "#f1f4f7"
    readonly property color labelBox: Qt.rgba(0.043, 0.055, 0.071, 0.88)

    // Layout shared by the cards.
    readonly property int maxContentWidth: 1240
    readonly property int gap: 16
    readonly property int cardPadding: 12
    readonly property int fixBoxWidth: 250
    cardRadius: 10
    // Between the parts inside a card, and between finding cards.
    readonly property int cardSpacing: 10
    readonly property int findingSpacing: 10
    // A card's small spaced capital heading.
    readonly property real headingLetterSpacing: 1.5
    // Prose: a note under a chart, a finding's explanation, and the lists of
    // checks and steps.
    readonly property real proseLineHeight: 1.2
    readonly property real listLineHeight: 1.15
    // A label and the control it names (Target, Spectrum).
    readonly property int controlLabelGap: 10
    // Raised tiles (gauges, meters, the fix box, the steps, the drop zone):
    // their inner padding across and down.
    readonly property int tilePadding: 10
    readonly property int tilePaddingTop: 9
    readonly property int meterSpacing: 3
    // A gauge's or meter's figure.
    readonly property int figureSize: 19
    // Thin progress bars.
    readonly property int progressBarHeight: 3

    // Before the first check: the badge, the title and the pitch.
    readonly property int emptyMargin: 32
    readonly property int emptyMaxWidth: 620
    readonly property int emptyBadgeSize: 44
    readonly property real emptyBadgeRadius: 11
    readonly property int emptyGlyphSize: 24
    readonly property int emptyTitleSize: 28
    readonly property real emptyTitleSpacing: -0.5
    readonly property real emptyLineHeight: 1.25
    readonly property int emptyProgressWidth: 240

    // The scrolled view, the run bar and the spectrum card.
    readonly property int contentTop: 22
    readonly property int contentTail: 60
    readonly property int runBarHeight: 48
    readonly property int runBarPadding: 10
    readonly property int runProgressWidth: 160
    readonly property int spectrumHeadWidth: 220
    readonly property int spectrumLegendSpacing: 14
    readonly property int spectrumSwatchWidth: 16
    readonly property int spectrumSwatchHeight: 3
    readonly property int spectrumWellHeight: 300

    // Since the last check: the arrow column and the text after it.
    readonly property int deltaRowHeight: 18
    readonly property int deltaGlyphWidth: 22
    readonly property int deltaTextX: 30
    // Checked and fine: the text after the tick.
    readonly property int fineTextX: 20
    // The reference: the drop zone's edge and each band's row.
    readonly property real dropZoneEdgeWidth: 1.5
    readonly property int referenceRowHeight: 16
    readonly property int referenceLabelWidth: 74
    readonly property int referenceBarX: 82
    readonly property int referenceValueWidth: 66
    readonly property int referenceBarHeight: 10
    readonly property int referenceCentreOutset: 3

    // A gauge: the rail, the shaded target zone and the value's marker.
    readonly property int gaugeRailHeight: 6
    readonly property real gaugeZoneAlpha: 0.22
    readonly property int gaugeMarkerWidth: 3
    readonly property int gaugeMarkerHeight: 12
    readonly property int gaugeMarkerTop: -4

    // A finding card: its padding, the severity edge and chip, the body,
    // the track and band chips and the fix box.
    readonly property int findingPadding: 12
    readonly property real findingHighlightLift: 1.12
    readonly property int severityEdgeWidth: 3
    readonly property int severityChipPadding: 12
    readonly property int severityChipHeight: 18
    readonly property real severityLetterSpacing: 1.1
    readonly property int findingBodyX: 14
    readonly property int findingTextMaxWidth: 640
    readonly property int findingChipHeight: 22
    readonly property int trackChipPadding: 16
    readonly property int trackChipInset: 7
    readonly property int trackDotSize: 9
    readonly property int bandChipPadding: 18
    readonly property int fixParamGap: 10

    // Plots: a label's baseline from the top, and the curves' strokes.
    readonly property int plotLabelBaseline: 12
    // Evidence: the well, the plot's margins and the marker rules.
    readonly property int evidenceHeight: 64
    readonly property int evidencePlotTop: 16
    readonly property int evidencePlotBottom: 8
    readonly property int evidenceMarkerBottom: 4
    readonly property int evidenceMarkerLabelWidth: 30
    readonly property int evidenceMarkerLabelGap: 3
    readonly property real evidenceBeforeWidth: 1.2
    readonly property real evidenceCurveWidth: 1.5
    // The spectrum: a finding's band shading (hot when picked) and curves.
    readonly property real spectrumBandAlpha: 0.09
    readonly property real spectrumBandHotAlpha: 0.19
    readonly property real spectrumBeforeWidth: 1.5
    readonly property real spectrumAfterWidth: 1.8
    readonly property real spectrumMonoWidth: 1.6
    // Energy: the well, the plot's margins, the drop tint, the curve and
    // the dashed level of the first drop with its label.
    readonly property int energyHeight: 150
    readonly property int energyPlotTop: 22
    readonly property int energyPlotBottom: 10
    readonly property real energyDropAlpha: 0.06
    readonly property real energyLineWidth: 1.6
    readonly property var energyDropDash: [3, 3]
    readonly property int energyGapLabelWidth: 60

    // Groove from a record (GrooveCapturePanel): the scrim and the card,
    // the setup column, the source list, the fields and the fingerprint.
    readonly property real scrimAlpha: 0.72
    readonly property int grooveCardMaxWidth: 1180
    readonly property int grooveCardPadding: 20
    readonly property real grooveCardRadius: 12
    readonly property int grooveHeaderHeight: 30
    readonly property real grooveTitleSpacing: 1.2
    readonly property int closeHitOutset: 6
    readonly property int grooveSetupWidth: 360
    readonly property int grooveSourceListHeight: 150
    readonly property int grooveSourceRowHeight: 22
    readonly property real grooveChosenAlpha: 0.14
    readonly property int grooveSourceDetailWidth: 96
    // The name column gives way to the detail column and its margins.
    readonly property int grooveSourceDetailReserve: 110
    readonly property int grooveFieldWidth: 90
    readonly property int grooveFieldHeight: 26
    readonly property real grooveFieldRadius: 5
    readonly property int grooveNameFieldHeight: 30
    readonly property int grooveButtonGap: 10
    readonly property int grooveRunnerSweep: 700
    readonly property real grooveRunnerShare: 0.25
    readonly property int grooveFactSpacing: 22
    readonly property int grooveFingerprintHeight: 300
    readonly property int grooveHintInset: 20
    readonly property real grooveStepLineAlpha: 0.45
    readonly property real grooveWaveAlpha: 0.55
    readonly property real grooveOnsetWidth: 1.5
    readonly property real grooveHitAlpha: 0.18
    readonly property real grooveHitStrengthAlpha: 0.3
    readonly property int grooveHitDot: 2
    readonly property real grooveWhiskerAlpha: 0.85
    readonly property real grooveLevelAlpha: 0.4
    readonly property int grooveTipPaddingX: 14
    readonly property int grooveTipPaddingY: 8
    readonly property int grooveTipLift: 2
    readonly property real grooveTipRadius: 5
    // The apply amounts: name column, the bar after it and the readout.
    readonly property int grooveAmountHeight: 20
    readonly property int grooveAmountLabelWidth: 70
    readonly property real grooveAmountLabelSpacing: 0.8
    readonly property int grooveAmountBarX: 76
    readonly property int grooveAmountValueWidth: 48
    readonly property int grooveAmountBarHeight: 4
    readonly property int grooveAmountHitOutset: 8

    // The kit's Button: plain on the raised panel, primary in a teal fill
    // (Run, Apply) or done in a teal outline (an applied fix); colours fade.
    pushButtonHeight: 30
    pushButtonPad: space2xl
    pushButtonFont: sansFamily
    pushButtonTextSize: typeLabel
    pushButtonDisabledOpacity: 0.5
    pushButtonPointer: true
    pushButtonFade: true
    accentInk: canvas
    // How much a hovered button lightens (primary, plain), and the done
    // button's teal wash.
    readonly property real primaryHoverLift: 1.08
    readonly property real buttonHoverLift: 1.25
    readonly property real doneWashAlpha: 0.1
    function pushButtonFill(button) {
        if (button.prime)
            return button.hovered ? Qt.lighter(button.accent, primaryHoverLift) : button.accent
        if (button.done)
            return alpha(button.accent, doneWashAlpha)
        return button.hovered ? Qt.lighter(panelRaised, buttonHoverLift) : panelRaised
    }
    function pushButtonEdge(button) {
        return button.activeFocus ? textMain : button.prime || button.done ? button.accent
             : button.hovered ? lineStrong : line
    }
    function pushButtonInk(button) {
        return button.prime ? button.accentInk : button.done ? button.accent
             : button.hovered ? textMain : textDim
    }

    // The kit's SegmentedControl: rounded cells fitted to their text on the
    // raised panel, short rules between them, the chosen one on the line
    // colour; colours fade.
    segmentedHeight: 28
    segmentedFrame: panelRaised
    segmentedFrameEdge: line
    segmentedCellPad: 22
    segmentedCellRadius: radiusLg
    segmentedDividers: true
    segmentedDivider: line
    segmentedDividerInset: spaceXs
    segmentedFontFamily: sansFamily
    segmentedTextSize: typeLabel
    segmentedFocusEdge: accent
    segmentedTips: true
    segmentedPointer: true
    segmentedFade: true
    // How much a hovered cell lightens.
    readonly property real segmentHoverLift: 1.2
    function segmentedFill(cell, stop) {
        return cell.on ? line : cell.hovered ? Qt.lighter(panelRaised, segmentHoverLift) : "transparent"
    }
    function segmentedInk(cell) {
        return cell.on ? textMain : textDim
    }

    // A finding's colour: teal once applied, else by severity.
    function findingColor(finding) {
        if (finding && finding.applied === true)
            return accent
        return finding && finding.severity === "warn" ? amber : red
    }
    function severityLabel(finding) {
        if (finding && finding.applied === true)
            return "Fixed"
        return finding && finding.severity === "warn" ? "Worth a look" : "Pokes out"
    }
    function stateColor(state) {
        return state === "bad" ? red : state === "warn" ? amber : state === "ok" ? accent : textMain
    }
    function alpha(colour, amount) {
        return Qt.rgba(colour.r, colour.g, colour.b, amount)
    }
    // A list from the controller, or an empty one.
    function list(value) {
        return value !== null && value !== undefined && typeof value !== "string"
               && typeof value.length === "number" ? value : []
    }
    // A map from the controller, or an empty one.
    function map(value) {
        return value !== null && value !== undefined && typeof value === "object" ? value : ({})
    }
    function text(value) {
        return value === null || value === undefined ? "" : String(value)
    }
    function number(value) {
        const result = Number(value)
        return value !== null && value !== undefined && value !== "" && isFinite(result) ? result : NaN
    }
    function hz(frequency) {
        const f = Number(frequency)
        if (!isFinite(f))
            return ""
        if (f >= 1000) {
            const k = f / 1000
            return String(k >= 10 ? Math.round(k * 10) / 10 : Math.round(k * 100) / 100) + "k"
        }
        return String(Math.round(f))
    }
    function signedDb(value, digits) {
        const v = Number(value)
        if (!isFinite(v))
            return "—"
        return (v > 0 ? "+" : v < 0 ? "−" : "") + Math.abs(v).toFixed(digits)
    }
}
