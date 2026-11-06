import QtQuick
import "../mixcheck"

// Arrange from a loop's palette: the shell and Mix Check tokens, plus a
// colour per role (in the controller's role order: Kick, Hats, Perc, Bass,
// Stab, Lead, Pad, Fx, Vocal), a tint per section kind (Intro, Build,
// Groove, Break, Drop, Groove 2, Outro) and one per automation lane.
MixCheckTheme {
    readonly property var roleColors: ["#ff8f6b", "#f5b14c", "#e8d9b8", "#8fa6ff", "#c49bff",
                                       "#5fe0bf", "#7fb8ff", "#ff7aa8", "#9be36b"]
    readonly property var sectionColors: ["#2a3340", "#34405a", "#2b4a44", "#3d3350", "#4a3a2c",
                                          "#2b4a44", "#2a3340"]
    readonly property var laneColors: ({ "low-pass": "#6aa7ff", "high-pass": "#c49bff",
                                         "throws": "#5fe0bf", "fills": "#f5b14c",
                                         "transitions": "#ff7aa8" })
    readonly property int gutter: 150
    readonly property int rowHeight: 22
    readonly property int laneHeight: 16
    readonly property int laneGap: 4

    // The view: its title, the intro paragraph, the rows of options and the
    // closing note.
    readonly property int arrangeTitleSize: 24
    readonly property real arrangeTitleSpacing: -0.4
    readonly property real introLineHeight: 1.25
    readonly property int toolbarSpacing: 10
    readonly property int optionsSpacing: 18
    readonly property int lengthSliderWidth: 210
    readonly property int breakdownSliderWidth: 170
    readonly property int variationSliderWidth: 150
    readonly property int statusMaxWidth: 520
    readonly property int proposalHeaderHeight: 20
    readonly property int decisionSpacing: 10
    readonly property int decisionBarWidth: 56
    readonly property int noteInsetY: 11
    readonly property int noteTrailing: 14
    readonly property int noteBarWidth: 3
    readonly property real noteLineHeight: 1.3
    // A rule it keeps: a pill with a dot, green when it holds.
    readonly property int ruleChipHeight: 26
    readonly property int ruleChipPad: 18
    readonly property int ruleChipSpacing: 7
    readonly property int ruleDotSize: 7
    readonly property real ruleDotRadius: 4
    readonly property real ruleTextSize: 11.5

    // A role card: a tinted stripe, the name with its role and LOCK tags,
    // the evidence, and the roles to choose from.
    readonly property int roleCardMinWidth: 230
    readonly property int roleCardInset: 7
    readonly property int roleCardBodyX: 20
    readonly property int roleStripeWidth: 4
    readonly property int roleCardHeaderHeight: 20
    readonly property real roleCardIdleOpacity: 0.62
    readonly property real roleNameSize: 12.5
    readonly property int roleTagHeight: 18
    readonly property int roleChipPad: 12
    readonly property int roleTagPad: 10
    readonly property real roleChipWashAlpha: 0.12
    readonly property real roleChipHoverAlpha: 0.22
    readonly property real roleChipEdgeAlpha: 0.5
    readonly property real roleChoiceWashAlpha: 0.25
    readonly property real roleChoiceEdgeAlpha: 0.6
    readonly property real roleChoiceSize: 10.5
    readonly property real roleEvidenceLineHeight: 1.2
    readonly property real arrangeTagSize: 8.5
    // Letter spacing of the spaced capital captions and of the small tags.
    readonly property real arrangeCaptionSpacing: 0.8
    readonly property real arrangeTagSpacing: 0.6

    // The timeline: blocks keep a hairline apart, rows and lanes are inset.
    readonly property int timelineBlockGap: 1
    readonly property real minorBarLineAlpha: 0.45
    readonly property int rulerLabelInset: 3
    readonly property int gutterLabelGap: 10
    readonly property real timelineLabelSize: 9.5
    readonly property int edgeHandleWidth: 10
    readonly property real edgeRestAlpha: 0.5
    readonly property real unarrangedOpacity: 0.5
    readonly property int runInset: 3
    readonly property real runFillAlpha: 0.55
    readonly property int fillMinWidth: 3
    readonly property real laneWashAlpha: 0.05
    readonly property int laneItemMinWidth: 3
    readonly property real laneItemAlpha: 0.7
    // A low-pass sweep fades in from this; a high-pass one rises to the other.
    readonly property real laneRampLowAlpha: 0.15
    readonly property real laneRampHighAlpha: 0.95
    readonly property int checkDotSize: 8
    readonly property int checkDotHitOutset: 4

    // The kit's Slider for Arrange's options: a spaced capital caption with
    // the readout after it, over a groove the handle stays inside.
    sliderWidth: 180
    sliderCaptionHeight: 0
    sliderCaptionGap: spaceSm
    sliderValueInline: true
    sliderRailGap: spaceSm
    sliderRailHeight: 18
    sliderGroove: line
    sliderGrooveInset: false
    sliderHandleInside: true
    sliderHandle: 12
    sliderHandleFill: textMain
    sliderHandleEdge: lineStrong
    sliderHandleEdgeWidth: strokeBold
    sliderLabelFont: monoFamily
    sliderLabelSize: 9.5
    sliderLabelWeight: Font.Bold
    sliderLabelSpacing: 0.8
    sliderLabelUpper: true
    sliderLabelInk: textFaint
    sliderLabelFocusInk: textFaint
    sliderValueFont: monoFamily
    sliderValueSize: 9.5
    sliderValueWeight: Font.Bold
    sliderValueInk: textMain
    sliderValueDragInk: textMain
    sliderResizeCursor: false
    sliderHover: false

    function roleColor(index) {
        return roleColors[Math.max(0, Number(index) || 0) % roleColors.length]
    }
    function sectionColor(kind) {
        return sectionColors[Math.max(0, Number(kind) || 0) % sectionColors.length]
    }
    function laneColor(kind) {
        return laneColors[kind] !== undefined ? laneColors[kind] : accent
    }
    function checkColor(status) {
        return status === "added" ? amber : status === "failed" ? red : accent
    }
}
