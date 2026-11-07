import QtQuick
import "../devicekit"

// Knock design tokens: a soot-black workshop in
// chalk ink with a coral strike. The striker and the finish are coral; the
// resonator cyan, the coupled resonator violet and the body ochre, in the
// cards and in the ringing modes alike.
KitTheme {
    prefix: "knock"
    knobLabelBold: true
    knobLabelSpacing: 1.2
    knobLabelSize: 9
    knobDiameter: 38

    // The workshop.
    caseColor: "#12110f"
    property color panel: "#1b1916"
    property color panel2: "#23201c"
    property color line: "#2e2a25"
    property color line2: "#423c35"
    plate: "#23201c"
    plateEdge: "#2e2a25"
    plateHi: "#423c35"
    well: "#0c0b0a"
    wellEdge: "#2e2a25"
    seam: "#2e2a25"
    grid: "#211e1a"
    gridStrong: "#2e2a25"
    cardRadius: 10

    // The strike and the three ringing parts.
    property color strike: "#ff7a59"
    property color strikeSoft: Qt.rgba(strike.r, strike.g, strike.b, 0.16)
    property color resonator: "#5fd0d6"
    property color coupled: "#b79cff"
    property color body: "#d9a441"
    // The pad of a hit flashes in the strike colour at this opacity.
    property real flashOpacity: 0.55
    property int flashMs: 260

    accent: strike
    accentSoft: "#a34a33"
    steel: "#3b2a22"
    segmentAccent: strike
    knobAccent: strike

    glacier: "#fff3ea"
    ink: "#eee8df"
    inkDim: "#a69e92"
    inkFaint: "#90887d"

    knobTop: "#2c2823"
    knobBottom: "#151310"
    knobRing: "#2e2a25"
    pointer: "#eee8df"
    track: "#2e2a25"
    button: "#1b1916"
    buttonHi: "#23201c"
    ledOff: "#2e2a25"

    warning: "#ffb37f"
    live: "#7ee2a8"

    arrowNoun: "kit"
    arrowHeight: 40
    menuMinWidth: 220

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // The kit's Slider as a row: its name, the slider and the value.
    sliderStacked: false
    sliderWidth: 220
    sliderHeight: 26
    sliderLabelWidth: 62
    sliderValueWidth: 58
    sliderGap: 5
    sliderGrooveInset: false
    sliderHandle: 12
    sliderHandleEdge: line2
    sliderLabelSize: typeBody
    sliderLabelInk: inkDim
    sliderLabelFocusInk: ink
    sliderValueSize: typeBody
    sliderValueInk: ink

    // The kit's ChoiceTiles: small wrapping tiles, the chosen one's name
    // bold in its card's colour.
    choiceWidth: 220
    choiceColumns: 3
    choiceTileHeight: 24
    choiceSpacing: 3
    choiceRadius: radiusLg
    choiceTile: panel
    choiceTileHover: panel2
    choiceTileOn: line
    choiceTileEdge: line
    choiceFocusRadius: 7
    choiceTextInset: 3
    choiceNameSize: typeBody
    choiceSpanLast: false
    choiceLongName: "group"
    function choiceNameInk(tile) {
        return inkDim
    }
    function choiceNameWeight(tile) {
        return Font.Bold
    }

    // The panel: its size and corners, the header (wordmark, kit box and
    // status) and the two columns.
    property int panelWidth: 1400
    property int panelHeight: 940
    property real panelRadius: 14
    property int headerHeight: 64
    property real wordmarkTop: 10
    property int wordmarkSize: 30
    property real wordmarkSpacing: 3
    property real taglineGap: 1
    property real taglineSpacing: 2.4
    property real kitBoxX: 214
    property real kitBoxTop: 12
    property int kitBoxWidth: 360
    property int kitBoxHeight: 40
    property real kitBoxRadius: 9
    property real kitBoxInset: 14
    property real kitBoxCaptionTop: 5
    property real kitBoxCaptionSpacing: 1.4
    property real kitBoxNameTop: 17
    // The kit name stops this far short of the box's right edge (the ▾).
    property real kitBoxNameReserve: 40
    property real statusTop: 14
    property int statusWidth: 420
    property real statusGap: 3
    property int leftColumnWidth: 430

    // Section labels and the notes under the cards.
    property real labelSpacing: 2
    property real noteLineHeight: 1.25
    // Small caps captions (tabs, EVOLVE, BAR).
    property real capsSpacing: 1.5
    // A control that cannot be used yet.
    property real disabledOpacity: 0.45

    // A card in the chain: inset, the title's dot and gap, and where its
    // rows start.
    property real stageInset: 11
    property real stageTitleGap: 7
    property int stageDot: 9
    property real stageTitleSpacing: 1.6
    property real stageBodyTop: 36

    // The panel's plain button (Hit pad).
    property int buttonMinWidth: 88
    property int buttonPad: 24
    property int buttonHeight: 30

    // The pads.
    property int padWidth: 100
    property int padHeight: 88
    // A pad's height in the panel's grid.
    property int padGridHeight: 76
    property real padRadius: 10
    property color padHover: Qt.lighter(panel2, 1.12)
    // A sounding pad lights in the strike colour up to this opacity.
    property real padLiveOpacity: 0.35
    property real padInset: 9
    property real padFootInset: 7

    // The global strip: Decay's dial, the five smaller dials beside it and
    // how far they drop to line up with it, and the Orbit row.
    property int globalStripHeight: 172
    property int globalDecayDial: 50
    property int globalSmallDial: 32
    property real globalSmallDialDrop: 6
    property int orbitLabelWidth: 52

    // The right column: the scope (stereo strip over the modes), the pad's
    // header, the chain and the output row.
    property int scopeHeight: 220
    property real scopeRadius: 10
    property int stereoHeight: 28
    property real rightSectionGap: 10
    property int padHeadHeight: 40
    property int padNameSize: 24
    property real padHeadSpacing: 14
    property int followPickWidth: 96
    property int chainHeight: 262
    property real chainSpacing: 14
    property int outRowHeight: 220
    property real outRowSpacing: 10

    // The ringing modes view: its margins, each mode's bar and halo, and the
    // key of parts.
    property real modesInset: 10
    property real modesKeyGap: 10
    property int modesKeyDot: 8
    property real modeBarWidth: 3
    property real modeBarAlpha: 0.9
    property real modeHaloAlpha: 0.25
    property real modeRestingAlpha: 0.28

    // Patterns: tabs, tiles and pills, action buttons, the generator's
    // settings, the loop library and the working pattern's grid.
    property int patternsHeight: 492
    property int tabHeight: 28
    property int tileMinWidth: 26
    property int tilePad: 14
    property int tileHeight: 24
    property real tileOnAlpha: 0.16
    property int actionMinWidth: 64
    property int actionPad: 22
    property int actionHeight: 28
    property int settingHeight: 24
    property real settingLabelWidth: 58
    property real settingValueWidth: 34
    property int settingRailHeight: 4
    property int settingHandle: 10
    // How far above and below the rail a press still lands on it.
    property real settingHitSlop: 8
    property real settingColumnGap: 14
    property real generateRowGap: 10
    property real styleSpacing: 3
    property int loopsPaneHeight: 176
    property int loopCellHeight: 42
    property real loopCellGap: 5
    property real loopMetaTop: 21
    // The thumbnail beside the meta line, under the name.
    property real loopThumbTop: 18
    // The pattern grid: the lane-name column's least width, and the least
    // width a step cell keeps however long a lane's name.
    property real laneNameMinWidth: 62
    property real laneCellMinWidth: 10
    property int loopThumbWidth: 54
    property int loopThumbHeight: 16
    // A plain hit in a thumbnail; accents are drawn solid.
    property real thumbHitAlpha: 0.55
    property real laneSpacing: 3
    property int laneHeight: 18
    property real lockStroke: 1.5
    // A step cell's opacity for a plain hit and a ghost (accents are solid).
    property real cellHitOpacity: 0.62
    property real cellGhostOpacity: 0.32

    // The colour of a ringing part, by its name from the panel model.
    function partColor(part) {
        return part === "coupled" ? coupled : part === "body" ? body : resonator
    }

    // Push buttons (Kit.Button): strike-coloured text on the panel, warming
    // under the pointer; the edge takes the strike colour when focused.
    pushButtonHeight: buttonHeight
    pushButtonMinWidth: buttonMinWidth
    pushButtonPad: buttonPad
    pushButtonRadius: radiusXl
    pushButtonTextSize: typeLabel
    pushButtonWeight: Font.Bold
    pushButtonDisabledOpacity: disabledOpacity
    function pushButtonFill(button) {
        return button.hovered ? strikeSoft : panel2
    }
    function pushButtonEdge(button) {
        return button.activeFocus ? strike : line
    }
    function pushButtonInk(button) {
        return strike
    }

    lastTouched: ""
}
