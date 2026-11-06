import QtQuick
import "../devicekit"

// Anvil design tokens: forged ember on graphite. The body and its curves
// are ember orange; each layer has its own colour (click gold, sub ice
// blue, rumble violet); time (the playhead) is white-hot.
KitTheme {
    prefix: "anvil"
    segmentHeight: 26
    segmentSpacing: 3
    segmentRadius: 6
    segmentLetterSpacing: 0.6
    segmentUpper: true
    knobLabelBold: true
    knobLabelSpacing: 1.2
    knobLabelSize: 9
    knobDiameter: 40

    // The plate: graphite, warmed from the top.
    caseTop: "#161a1f"
    caseColor: "#0e1115"
    caseBottom: "#0b0d10"
    // Where the case reaches caseColor, down from the top.
    property real caseMidStop: 0.55
    property real panelRadius: 14
    plate: "#141a21"
    plateEdge: "#222c37"
    plateHi: "#34424f"
    well: "#0c1015"
    wellEdge: "#26313c"
    seam: "#1c252f"
    grid: "#18212a"
    gridStrong: "#26313c"
    cardRadius: 10
    cardTitleInk: "#e6edf3"
    cardTitleSize: 10
    cardTitleSpacing: 2

    // The layers, the curve and time.
    property color body: "#ff7a2f"
    property color click: "#ffd166"
    property color sub: "#5ac8fa"
    property color rumble: "#b085f5"
    property color curve: "#ffd0ad"
    property color ghost: "#5c6b78"
    property color wave: "#ff7a2f"
    property color beat: "#9fb0c0"
    property color time: "#fff4e8"
    property color inKey: "#7ee2a8"
    property color offKey: "#ffb37f"
    property color wordmark: "#ff7a2f"
    property color pointFill: "#0c1015"
    property color selected: "#1b1714"
    // A lit button's caption and the chosen curve tab's name.
    property color hotInk: "#ffb37f"

    // The curve editor: the drawn curve over the kick, with the other main
    // curve as a dashed ghost behind it.
    curveRadius: 9
    curveLine: curve
    curveLineWidth: 2.2
    curveGlowAlpha: 0.16
    curvePointRadius: 6.5
    curvePointWidth: strokeBold
    curvePointFill: body
    curvePointEdge: pointFill
    curveHitRadius: 11
    curveRingFill: pointFill
    property int ghostSamples: 200
    property real ghostWidth: 1.2
    property var ghostDash: [4, 4]
    // The kick behind the curve: its fill, its outline and the outline's
    // width; and the playhead over it.
    property real waveFillAlpha: 0.16
    property real waveEdgeAlpha: 0.35
    property real waveEdgeWidth: 0.8
    property real playheadOpacity: 0.85

    // The beat strip: the whole beat and the rumble under it, inset from
    // the well's sides, and its playhead.
    property real beatInset: 10
    property real beatFillAlpha: 0.25
    property real rumbleFillAlpha: 0.55
    property real beatPlayheadWidth: strokeBold
    property real beatPlayheadOpacity: 0.7

    // Small captions over controls.
    property int labelSize: typeCaption
    property real labelSpacing: 1.6
    // The header's wordmark and the line under it.
    property int wordmarkSize: 30
    property real wordmarkSpacing: 8
    property int subtitleSize: typeMicro
    property real subtitleSpacing: 2.4
    // The preset display and the tail-note card: dark wells.
    property real wellRadius: 9
    property real presetCategorySpacing: 1.4
    property int presetNameSize: typeHeading
    property int tailNoteSize: 19
    // The panel's own push buttons.
    property int buttonHeight: 30
    property real buttonRadius: radiusLg
    property real buttonDisabledOpacity: 0.45
    property int buttonTextSize: typeSmall
    property real buttonLetterSpacing: 1
    // The layer cards and the hint card under them; a layer's level bar.
    property real layerCardRadius: 10
    property int layerNameSize: typeEmphasis
    property int levelBarHeight: 4
    property real unpoweredOpacity: 0.5
    property real hintLineHeight: 1.2
    // The curve tabs.
    property real tabSpacing: 1.4
    property real tabRuleHeight: strokeBold
    // The transient's path field.
    property real fieldRadius: 5

    accent: "#ff7a2f"
    accentSoft: "#a3501f"
    steel: "#3a2216"
    segmentAccent: "#ff7a2f"
    knobAccent: "#ff7a2f"

    glacier: "#fff1e6"
    ink: "#e6edf3"
    inkDim: "#93a1ae"
    inkFaint: "#6b7884"

    knobTop: "#222b35"
    knobBottom: "#10151b"
    track: "#26313c"
    button: "#1a222b"
    buttonHi: "#212b36"
    ledOff: "#26313c"

    warning: "#ffb37f"
    live: "#7ee2a8"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    // Push buttons (Kit.Button): a plate that lights steel with hot text
    // when on, its edge in the accent while on or focused; colours fade.
    pushButtonHeight: buttonHeight
    pushButtonRadius: buttonRadius
    pushButtonTextSize: buttonTextSize
    pushButtonWeight: Font.Bold
    pushButtonLetterSpacing: buttonLetterSpacing
    pushButtonDisabledOpacity: buttonDisabledOpacity
    pushButtonFade: true
    function pushButtonFill(control) {
        return control.on ? steel : control.hovered ? buttonHi : button
    }
    function pushButtonEdge(button) {
        return button.activeFocus || button.on ? accent : plateEdge
    }
    function pushButtonInk(button) {
        return button.on ? hotInk : ink
    }

    lastTouched: ""
}
