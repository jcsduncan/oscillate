import QtQuick
import "../devicekit" as Kit

// Rebalance's palette and layout: the shell's tokens plus one colour per
// role, so a role reads the same on its card, its curve and the track list.
Kit.ShellTheme {
    id: style

    readonly property var roleColors: ({
        vocals: "#ff7aa8",
        drums: "#f5b14c",
        bass: "#8fa6ff",
        music: "#5fe0bf",
        none: "#798592"
    })
    function roleColor(id) { return roleColors[id] !== undefined ? roleColors[id] : roleColors.none }

    // The view: margins, the header and the track list beside the cards.
    readonly property int viewMargin: 24
    headerHeight: 52
    readonly property int titleSize: 22
    readonly property real titleSpacing: -0.3
    readonly property int sectionGap: 16
    readonly property int listWidth: 440
    readonly property int buttonHeight: 28
    readonly property int buttonPadding: 28
    readonly property int switchWidth: 64

    // A role's card.
    readonly property int cardWidth: 248
    readonly property int cardGap: 12
    readonly property int cardPadding: 14
    cardRadius: 10
    readonly property int cardHeaderHeight: 22
    readonly property int roleDotSize: 10
    readonly property int roleNameSize: 15
    readonly property int roleFaderHeight: 270
    readonly property int faderGap: 10
    readonly property int scaleWidth: 26
    readonly property int meterGap: 3
    readonly property int gainReadoutSize: 20
    readonly property int gainReadoutHeight: 28
    // The fader's scale marks, in dB.
    readonly property var scaleMarks: [12, 6, 0, -6, -12]
    readonly property real dimmedOpacity: 0.45

    // A role's EQ curve and its knobs.
    readonly property int curveHeight: 92
    readonly property real curveStroke: 2
    readonly property real curveFillAlpha: 0.18
    readonly property real curveGridAlpha: 0.5
    readonly property real bandDotRadius: 3.5
    // The curve's frequency gridlines and their labels.
    readonly property var curveGridHz: [100, 1000, 10000]
    readonly property var curveGridLabels: ["100", "1k", "10k"]
    readonly property int bandLabelWidth: 40
    readonly property int knobSlotWidth: 58
    readonly property int bandRowSpacing: 4

    // The track list.
    readonly property int rowHeight: 34
    readonly property int rowIndent: 18
    readonly property int swatchWidth: 3
    readonly property int reasonWidth: 84
    readonly property int pillWidth: 96
    readonly property int pillHeight: 24
    readonly property real chosenEdge: 1.5
}
