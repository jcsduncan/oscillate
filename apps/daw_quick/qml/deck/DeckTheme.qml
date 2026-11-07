import QtQuick
import "../devicekit"

// Deck design tokens: a club mixer's channel. A black anodised face with a
// faint warm sheen, matte plates and deep wells; the three bands in the
// colours a DJ reads at a glance (low in crimson, mid in amber, high in
// ice blue), each band's knob, kill and level bar in its own; the device's
// own mark in magenta.
KitTheme {
    prefix: "deck"
    knobDiameter: 72

    caseTop: "#1d1a1f"
    caseColor: "#141215"
    caseBottom: "#0c0b0d"
    plate: "#19171b"
    plateEdge: "#2c2830"
    plateHi: "#3e3843"
    well: "#0a090b"
    wellEdge: "#26222a"
    seam: "#221f25"

    // The response screen.
    property color screen: "#070608"
    grid: "#16131a"
    gridStrong: "#272230"
    property color zero: "#3a3342"
    property color curve: "#ffe3f4"

    // Low, mid, high.
    property var bands: ["#ff4f6d", "#ffb547", "#5fd4ff"]
    property var bandNames: ["LOW", "MID", "HIGH"]

    accent: "#ff8fd0"
    accentSoft: "#8a3f6b"
    steel: "#6b3a58"
    glacier: "#fff2fa"
    ink: "#efe9f1"
    inkDim: "#a097a6"
    inkFaint: "#887e8f"

    knobTop: "#4a444f"
    knobBottom: "#0f0d10"
    track: "#262229"
    button: "#151316"
    buttonHi: "#1e1b20"
    ledOff: "#2d2930"

    property color meterLow: "#4fd18b"
    property color meterMid: "#f2c35c"
    property color meterHigh: "#ff5d55"

    warning: "#f2b25c"
    live: "#4fd18b"

    sans: "DejaVu Sans"
    mono: "DejaVu Sans Mono"

    lastTouched: ""

    // The panel's frame: its size, rounded case and where the case
    // gradient reaches its body colour.
    property int panelWidth: 980
    property int panelHeight: 600
    property real caseRadius: 10
    property real caseMidStop: 0.35
    // Between the plates.
    property real plateGap: 10
    // A focused well's edge.
    property real focusStroke: 1.5

    // Small bold captions (L/R, the preset category, the readout name).
    property real captionSpacing: 1.4

    // The header: the wordmark, its three-colour rule and the subtitle.
    property real headerY: 10
    property real headerHeight: 54
    property int wordmarkSize: 27
    property real wordmarkSpacing: 9
    property real wordmarkRuleGap: 1
    property real wordmarkRuleHeight: 2
    // How much shorter than the wordmark the rule runs.
    property real wordmarkRuleInset: 10
    property real subtitleGap: 7
    property real subtitleSpacing: 2.4
    property real statusY: 14

    // The preset display between the arrows, and the readout inside it.
    property real presetX: 210
    property real presetY: 5
    property real presetWidth: 470
    property real presetHeight: 42
    property real presetTextX: 14
    property real presetCaptionY: 5
    property real presetNameY: 16
    property real presetNameWidth: 210
    property real presetNameSpacing: 0.4
    property real presetDividerX: 236
    property real presetDividerInset: 8
    property real readoutX: 248
    property real readoutValueY: 17
    property real readoutRight: 26
    property real readoutSpacing: 1.2
    property real chevronMargin: 10

    // The body: the response plate on the left, the channel on the right.
    property real bodyY: 76
    property real channelWidth: 330
    property real responseHeight: 300
    // Where a card's contents start below its title.
    property real cardBodyY: 30
    property real cardContentY: 34
    property real screenInset: 10

    // The crossover and output knobs, and the output peak meters.
    property real knobRowY: 38
    property real knobRowSpacing: 26
    property int smallKnobDiameter: 54
    property real outputMeterMargin: 18
    property real outputMeterWidth: 10
    // The plate height the meters leave free above and below.
    property real outputMeterInset: 80

    // A band's row in the channel: its name, state and live level bar.
    property real bandTextX: 118
    property real bandNameY: 12
    property real bandNameSpacing: 3
    property real bandStateY: 34
    property real bandLevelY: 58
    property real bandLevelWidth: 120
    property real bandLevelHeight: 6
    property real bandKillY: 78

    // The response screen: band fills and outlines, the crossover dash, and
    // the scale captions inside the bottom edge.
    property real bandFillAlpha: 0.18
    property real bandLineAlpha: 0.85
    property var crossoverDash: [3, 4]
    property real responseCaptionBottom: 14
}
