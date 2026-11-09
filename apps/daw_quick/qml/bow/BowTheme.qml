import QtQuick
import "../devicekit"

// Bow design tokens, from the approved mockup: a blue-black case with raised
// cards, rosin amber as the accent and a cool teal for vibrato and the
// keyswitches. Every size the panel draws with is named here, so the panel
// and its tests share one value.
KitTheme {
    prefix: "bow"

    // The case, its cards, tiles and the plot's well.
    caseColor: "#10131a"
    property color card: "#171c26"
    property color cardEdge: "#262c38"
    property color tile: "#1b202b"
    property color tileEdge: "#2c3343"
    property color tileOn: "#2a2620"
    property color focusRing: "#f3d9b8"
    cardRadius: 12

    // The bow's colours: amber for the dynamics and the chosen, teal for
    // vibrato and keyswitches, and the keyboard's keys in and out of range.
    property color rosin: "#e0a96d"
    property color rosinSoft: Qt.rgba(rosin.r, rosin.g, rosin.b, softAlpha)
    property real softAlpha: 0.22
    property color vibratoInk: "#5fc7c0"
    property color keyInRange: "#d9dde5"
    property color keyOutOfRange: "#5a6070"
    property real keyOutOfRangeOpacity: 0.55
    property color plotLine: "#232937"

    accent: rosin
    accentSoft: rosinSoft
    accentInk: caseColor
    segmentAccent: rosin
    knobAccent: rosin
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
    live: vibratoInk

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

    // Tiles: an articulation's row, and the section picker's and Solo or
    // Section's buttons; their corner radius.
    property int articulationTileHeight: 40
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

    // The panel: its designed size, margin and gaps; the section picker's
    // height and the Solo or Section switch's width; the articulation
    // card's width; the knob card's and the keyboard strip's heights, and a
    // key's gap and corner.
    property real panelWidth: 1240
    property real panelHeight: 760
    property real panelMargin: 24
    property real panelGap: 14
    property real pickerHeight: 40
    property real modeWidth: 236
    property real articulationWidth: 236
    property real knobsHeight: 128
    property real stripHeight: 92
    property real keyGap: 1
    property real keyRadius: 3
    // The bow display's span, and how often the panel reads the engine
    // while it plays and while it is idle.
    property real historySeconds: 6
    property real pollLiveMs: 33
    property real pollIdleMs: 250
    // The bow display's strokes and its playhead dot.
    property real dynamicsStroke: 2.5
    property real vibratoStroke: 1.5
    property real playheadRadius: 5
    property real playheadEdge: 2

    // Cards and their titles.
    property int cardPad: 16
    property int titleHeight: 18
    property int titleGap: 10
    knobDiameter: 54

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
