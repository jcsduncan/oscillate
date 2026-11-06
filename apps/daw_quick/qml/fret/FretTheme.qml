import QtQuick
import "../devicekit"

// Fret design tokens: a night-blue case with
// raised cards, the pluck blue as the accent, staccato in amber and ghost
// notes in slate. Each articulation keeps its colour in the groove lane,
// the Articulation card and the last-note readout alike.
KitTheme {
    prefix: "fret"

    // The case, its cards and the lane's well.
    caseColor: "#0b0e13"
    property color card: "#131820"
    property color cardEdge: "#222a35"
    property color lane: "#0e1218"
    property color laneEdge: "#1f2731"
    // The lane's sixteenth columns: on a beat, and between.
    property color groundBeat: "#161d27"
    property color ground: "#11161d"
    // Tiles and buttons: idle, and chosen in their accent's dark tint.
    property color tile: "#161b22"
    property color tileEdge: "#2a3340"
    property color tileOn: "#12253d"
    property color tileOnAmber: "#33240f"
    cardRadius: 16

    // The articulations, as the lane colours each note.
    property color pluck: "#4ea1ff"
    property color staccato: "#ffb257"
    property color ghost: "#5e6a78"
    property color bridge: "#b98cff"
    // The ring around a pushed note.
    property color pushedRing: "#f2f5f8"
    property color toneDot: "#c9d4df"
    property color noteInk: "#c9d4df"

    accent: pluck
    accentSoft: "#2a5a8f"
    segmentAccent: pluck
    knobAccent: pluck
    plate: card
    plateEdge: cardEdge
    plateHi: tileEdge
    well: lane
    wellEdge: laneEdge
    seam: cardEdge
    grid: ground
    gridStrong: groundBeat
    track: "#1c232d"
    button: tile
    buttonHi: "#1b222b"
    ledOff: tileEdge
    meterWell: track

    glacier: "#f2f5f8"
    ink: "#e6edf3"
    inkDim: "#9aa7b4"
    inkFaint: "#6e7a87"
    pointer: ink
    knobTop: "#232b36"
    knobBottom: "#10141a"
    knobRing: tileEdge

    warning: "#ffb257"
    live: "#7ee2a8"

    arrowHeight: presetHeight
    arrowRadius: tileRadius
    menuMinWidth: 220

    sans: "Inter"
    mono: "JetBrains Mono"

    // The case's corners.
    property int caseRadius: 14
    // Type: the wordmark, and the letter spacing of card titles and of
    // upper-case captions.
    property int wordmarkSize: 30
    property real wordmarkSpacing: 6
    property real headingSpacing: 2
    property real labelSpacing: 1.5
    // Wrapped notes under a card's controls.
    property real bodyLineHeight: 1.2

    // Cards' titles and their dots.
    property int titleHeight: 20
    property int titleDot: 10
    property int titleGap: spaceLg
    // Between a caption and its value (the preset's, the meter's).
    property int captionGap: 10
    // A colour swatch: the latest note's articulation, the lane's legend.
    property int swatch: 10

    // Tiles, the preset display and push buttons.
    property int tileRadius: 10
    property int styleTileHeight: 64
    property int styleTileGap: 10
    property int compactTileHeight: 34
    property int presetWidth: 220
    property int presetHeight: 44
    // The preset's caption from the left edge, the chevron from the right,
    // and the room the caption and chevron leave for the name.
    property int presetInset: 18
    property int presetChevronInset: 14
    property int presetNameRoom: 120
    property int buttonHeight: 36
    property int buttonPad: 14

    // The groove lane's well, and the lane inside it: inset on three sides,
    // less at the foot.
    property int laneRadius: 12
    property int laneInset: 14
    property int laneFootInset: 10

    // The install command's box: its text's horizontal and vertical inset.
    property int commandPadX: 10
    property int commandPadY: 7
    // The missing library's message takes this share of the plate's width.
    property real libraryMessageShare: 0.42

    // The kit's Slider: its name and value over the rail, and an optional
    // hint.
    sliderWidth: 200
    sliderCaptionHeight: 18
    sliderRailHeight: 22
    sliderHintLines: 2
    sliderHandle: 14
    sliderHandleEdge: tileEdge
    // The kit's ChoiceTiles: a short row's last tile spans the rest.
    choiceWidth: 320
    choiceTileHeight: 44
    choiceSpacing: spaceMd
    choiceRadius: tileRadius
    choiceTile: tile
    choiceTileHover: buttonHi
    choiceTileOn: tileOn
    choiceTileEdge: tileEdge
    choiceSpanLast: true
    // The header's output meter, dBFS, and its size.
    property real meterFloorDb: -60
    property real meterCeilDb: 6
    property int meterWidth: 120
    property int meterHeight: 8

    // The colour of an articulation, by its name from the controller.
    function articulationColor(name) {
        return name === "staccato" ? staccato : name === "ghost" ? ghost
            : name === "bridge" ? bridge : pluck
    }

    // Push buttons (Kit.Button): a tile, filled while on, that lightens
    // under the pointer; its edge turns the pluck colour when focused, and
    // a click focuses.
    pushButtonHeight: buttonHeight
    pushButtonPad: 2 * buttonPad
    pushButtonRadius: radiusXl
    pushButtonTextSize: typeEmphasis
    pushButtonWeight: Font.Normal
    pushButtonTipFallback: false
    pushButtonFocusOnClick: true
    function pushButtonFill(button) {
        return button.on ? tileOn : button.hovered ? buttonHi : tile
    }
    function pushButtonEdge(button) {
        return button.activeFocus ? pluck : tileEdge
    }
    function pushButtonInk(button) {
        return ink
    }

    lastTouched: ""
}
