import QtQuick
import "../devicekit" as Kit

// The master assistant's layout on the shell's tokens: the goals on the
// left, what was measured in the middle, the proposal on the right.
Kit.ShellTheme {
    id: style

    readonly property int viewMargin: 24
    headerHeight: 52
    readonly property int titleSize: 22
    readonly property real titleSpacing: -0.3
    readonly property int sectionGap: 16
    readonly property int goalsWidth: 330
    readonly property int proposalWidth: 600
    readonly property int buttonHeight: 28
    readonly property int buttonPadding: 28
    readonly property int chipHeight: 26
    readonly property int chipPadding: 18
    readonly property int chipGap: 6

    readonly property int cardPadding: 14
    cardRadius: 10
    readonly property int cardHeaderHeight: 22
    readonly property int rowGap: 10

    // The readouts.
    readonly property int readoutValueSize: 20
    readonly property int readoutWidth: 104
    readonly property int readoutHeight: 46

    // The tonal view: the mix against the reference, a band each.
    readonly property int tonalHeight: 210
    readonly property real tonalStroke: 2
    readonly property real corridorAlpha: 0.22
    readonly property var tonalGridHz: [100, 1000, 10000]
    readonly property var tonalGridLabels: ["100", "1k", "10k"]
    readonly property int tonalLabelWidth: 30

    // The regions' table.
    readonly property int regionRowHeight: 24
    readonly property int regionNameWidth: 70

    // A proposed device.
    readonly property int checkSize: 16
    readonly property int deviceNameWidth: 70
    readonly property real keptOpacity: 1.0
    readonly property real droppedOpacity: 0.45
    readonly property color proposalEdge: "#2f5a51"

    // The A/B switch.
    readonly property int abWidth: 220
    readonly property int progressHeight: 4
}
