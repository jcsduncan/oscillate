import QtQuick
import "../devicekit"

// Wren design tokens: a deep teal-black case with
// raised cards, breath teal as the accent, staccato in apricot and slurs in
// lilac. Each way a note starts keeps its colour in the phrase lane and on
// the Articulation card alike. Every size the panel draws with is named
// here, so the panel and its tests share one value.
KitTheme {
    prefix: "wren"

    // The case, its cards and the lane's well.
    caseColor: "#0d1112"
    property color card: "#131a1c"
    property color cardEdge: "#222b2e"
    property color libraryCard: "#111718"
    property color lane: "#0f1415"
    property color laneEdge: "#1e272a"
    property color laneGround: "#0b0f10"
    // The lane's beat lines: on a bar line, and on a beat.
    property color barLine: "#2e3a3e"
    property color beatLine: "#1a2225"
    // Tiles and buttons: idle, chosen in teal, chosen in lilac, and a
    // segment (Mono / Poly 4, Ornaments) chosen.
    property color tile: "#141a1c"
    property color tileEdge: "#263034"
    property color tileOn: "#1c292b"
    property color tileOnLilac: "#211f2e"
    property color focusRing: "#c8f1ee"
    cardRadius: 16

    // How notes start, as the lane draws them.
    property color tongued: "#8fd3d0"
    property color staccato: "#f2b880"
    property color slur: "#c9b8ff"
    property color grace: "#ffffff"
    property color wave: "#eaf0f1"
    property color toneDot: staccato

    accent: tongued
    accentSoft: "#3d6f6d"
    segmentAccent: tongued
    knobAccent: tongued
    plate: card
    plateEdge: cardEdge
    plateHi: tileEdge
    well: lane
    wellEdge: laneEdge
    seam: cardEdge
    grid: beatLine
    gridStrong: barLine
    track: "#1a2224"
    button: tile
    buttonHi: "#1a2225"
    ledOff: tileEdge
    meterWell: track

    glacier: "#f4f8f8"
    ink: "#eaf0f1"
    inkDim: "#9ba8ac"
    inkFaint: "#768589"
    pointer: ink
    knobTop: "#222c2f"
    knobBottom: "#0f1415"
    knobRing: tileEdge

    warning: "#f2b880"
    live: "#7fd69b"

    menuMinWidth: 220

    sans: "Inter"
    mono: "JetBrains Mono"

    // Type sizes: the preset name, body text and tile names, captions and
    // hints, small print, and a style tile's name.
    property int nameSize: 14
    property int bodySize: 13
    property int captionSize: 12
    property int smallSize: 11
    property int fineSize: 10
    property int tileNameSize: 15
    property real headingSpacing: 2
    property real labelSpacing: 1.5

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

    // Tiles: a style tile (a name and a hint), an articulation tile, and a
    // segment button; their corner radius.
    property int styleTileHeight: 64
    property int articulationTileHeight: 48
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

    // The header (Kit.DeviceHeader): the bold wordmark over the flute it
    // plays, the preset tile (a touch target, as are its arrows) and, once
    // a control is touched, its readout.
    wordmarkSize: 30
    wordmarkWeight: Font.Bold
    wordmarkSpacing: 6
    subtitleSize: fineSize
    subtitleSpacing: 0.4
    subtitleInk: inkDim
    arrowWidth: touch
    arrowHeight: touch
    arrowRadius: tileRadius
    presetDisplayWidth: 220
    presetDisplayHeight: touch
    presetDisplayRadius: tileRadius
    presetFill: tile
    presetEdge: tileEdge
    presetFocusStroke: focusBorder
    presetTextX: tilePad
    presetTagY: spaceSm
    presetTagInk: inkDim
    presetNameY: 20
    presetNameSize: nameSize
    presetNameBold: false
    presetChevronMargin: tilePad
    presetChevronInk: inkDim
    presetChevronSize: captionSize
    readoutWidth: 160
    readoutFill: tile
    readoutEdge: tileEdge
    readoutRadius: tileRadius
    readoutNameY: spaceSm
    readoutValueY: 20

    // Cards and their title dots.
    property int cardPad: 20
    property int titleHeight: 20
    property int titleDot: 10
    property int titleGap: 12

    // The header's output meter, dBFS.
    property real meterFloorDb: -60
    property real meterCeilDb: 6
    property int meterWidth: 100
    property int meterHeight: 8

    // The colour of a note in the lane, by whether it is short.
    function noteColor(staccatoNote) {
        return staccatoNote ? staccato : tongued
    }

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
