import QtQuick
import "../devicekit"

// Stick design tokens, from the approved mockup: a blue-black case with
// raised cards, warm amber as the accent, brass for the cymbals and teal for
// the meters. Every size the panel draws with is named here, so the panel
// and its tests share one value.
KitTheme {
    prefix: "stick"

    // The case, its cards, tiles and the plot's well.
    caseColor: "#10131a"
    property color card: "#171c26"
    property color cardEdge: "#262c38"
    property color tile: "#1b202b"
    property color tileEdge: "#2c3343"
    property color tileOn: "#2a2620"
    property color focusRing: "#f3d9b8"
    cardRadius: 12

    // The kit's colours: amber for the chosen and the lit, brass for the
    // cymbals, teal for the meters; a drum's head and a cymbal's face.
    property color amber: "#e0a96d"
    property color amberSoft: Qt.rgba(amber.r, amber.g, amber.b, softAlpha)
    property real softAlpha: 0.22
    property color meterInk: "#5fc7c0"
    property color drumFace: "#1f2531"
    property color drumEdge: "#3a4256"
    property color cymbalFace: "#3a3222"
    property color cymbalEdge: "#8a7442"
    property color plotLine: "#232937"

    accent: amber
    accentSoft: amberSoft
    accentInk: caseColor
    segmentAccent: amber
    knobAccent: amber
    plate: card
    plateEdge: cardEdge
    plateHi: tileEdge
    well: caseColor
    wellEdge: cardEdge
    seam: cardEdge
    grid: plotLine
    gridStrong: tileEdge
    track: "#2a3141"
    button: tile
    buttonHi: "#222837"
    ledOff: tileEdge
    meterWell: track

    glacier: "#f6f7fa"
    ink: "#e7e9ee"
    inkDim: "#9aa3b4"
    inkFaint: "#8f97a8"
    pointer: ink
    knobTop: "#1f2531"
    knobBottom: "#151a23"
    knobRing: tileEdge

    warning: "#f2b880"
    live: meterInk

    menuMinWidth: 220

    sans: "Inter"
    mono: "JetBrains Mono"

    // Type sizes: body text and tile names, captions, small print.
    property int bodySize: 13
    property int captionSize: 12
    property int smallSize: 11
    property int tileNameSize: 13
    property real headingSpacing: 1.6
    property real labelSpacing: 1.2

    // Every control is at least this tall and wide to the pointer.
    property int touch: 44
    // Between a caption's name and its value.
    property int captionGap: 8
    // A control's edge: idle, and focused or held.
    property int idleBorder: 1
    property int focusBorder: 2
    // A control the patch cannot change now.
    property real disabledOpacity: 0.5

    // The kit's Slider: its name and value over the rail (together one
    // touch target), and an optional hint under them.
    sliderWidth: 200
    sliderCaptionHeight: 18
    sliderRailHeight: touch - sliderCaptionHeight
    sliderHintLines: 1
    sliderHandle: 14
    sliderTrack: 4
    sliderCaptionGap: captionGap
    sliderTouchCaption: true
    sliderHandleEdge: tileEdge
    sliderHandleEdgeWidth: idleBorder
    sliderHandleActiveEdgeWidth: focusBorder
    sliderLabelSize: bodySize
    sliderValueSize: captionSize
    sliderHintSize: smallSize
    sliderDisabledOpacity: disabledOpacity
    // Shift-dragging a slider moves it this many times more finely.
    sliderFineDivisor: 10

    // Tiles: the kit and mix pickers' buttons; their corner radius.
    property int tileRadius: 10
    property int tilePad: 14
    property int tileGap: 8
    // Between a tile's name and its hint.
    property int hintGap: 3

    // The kit's ChoiceTiles: touch-sized tiles in whole pixels, ringed in
    // the focus colour, or a segment of joined buttons.
    choiceWidth: sliderWidth
    choiceTileHeight: touch
    choiceSpacing: tileGap
    choiceSegmentWidth: touch
    choiceRadius: tileRadius
    choiceEdgeWidth: idleBorder
    choiceTile: tile
    choiceTileHover: buttonHi
    choiceTileOn: tileOn
    choiceTileEdge: tileEdge
    choiceFocusMargin: focusBorder + idleBorder
    choiceFocusRadius: tileRadius + focusBorder
    choiceFocusWidth: focusBorder
    choiceFocusInk: focusRing
    choiceTextPad: tilePad
    choiceHintGap: hintGap
    choiceNameSize: tileNameSize
    choiceHintSize: captionSize
    choiceHintSizeSmall: smallSize
    choiceWholePixels: true
    choiceSpanLast: false
    choiceDisabledOpacity: disabledOpacity

    // The panel: its designed size, margin and gaps; the pickers' height
    // and the mix picker's width; the kit card's width; the piece card's
    // and the kit row's heights.
    property real panelWidth: 1240
    property real panelHeight: 760
    property real panelMargin: 24
    property real panelGap: 14
    property real pickerHeight: 48
    property real mixWidth: 320
    property real kitWidth: 440
    property real pieceHeight: 150
    property real kitRowHeight: 72
    // How often the panel reads the engine while it plays and while idle.
    property real pollLiveMs: 33
    property real pollIdleMs: 250

    // The kit drawing: its design box (drummer at the bottom), a piece's
    // ring and the selected piece's, and how far a hit's glow reaches.
    property real drawingWidth: 400
    property real drawingHeight: 290
    property real pieceRing: 1.5
    property real selectedRing: 3
    property real glowReach: 12
    property real glowBase: 4
    property real glowAlpha: 0.42
    property real absentOpacity: 0.25
    property int pieceNameSize: 11
    property int pieceKeySize: 10

    // The mixer: a strip's least width, its meter's width, the switches'
    // size, and the meter's scale.
    property real stripMinWidth: 48
    property real meterWidth: 6
    property real stripSwitchSize: 24
    property real meterFloorDb: -60
    property real meterCeilDb: 0
    property real meterHot: 0.75
    property int stripLabelLines: 2
    // The strip's chain chip: a lamp per stage and the parallel send.
    property real chainChipHeight: 18
    property real chainLampSize: 5
    property real chainLampGap: 3

    // The chain editor (ADR-0121): knobs per row in a stage, a knob's cell
    // and its dial.
    property int chainColumns: 3
    property real chainCell: 64
    property real chainCellHeight: 78
    property int chainKnobDiameter: 34

    // Cards and their titles.
    property int cardPad: 16
    property int titleHeight: 18
    property int titleGap: 10
    knobDiameter: 48

    // Push buttons (Kit.Button): a full touch target tall, a tile that
    // lightens under the pointer and rings when focused; a click focuses.
    pushButtonHeight: touch
    pushButtonMinWidth: touch
    pushButtonPad: 2 * tilePad
    pushButtonRadius: tileRadius
    pushButtonTextSize: bodySize
    pushButtonWeight: Font.Normal
    pushButtonEdgeWidth: idleBorder
    pushButtonFocusEdgeWidth: focusBorder
    pushButtonPointer: true
    pushButtonTipFallback: false
    pushButtonFocusOnClick: true
    function pushButtonFill(button) {
        return button.hovered ? buttonHi : tile
    }
    function pushButtonEdge(button) {
        return button.activeFocus ? focusRing : tileEdge
    }
    function pushButtonInk(button) {
        return ink
    }

    lastTouched: ""
}
