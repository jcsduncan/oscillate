pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The strip in an audio clip's header (ADR-0053): its name, the source tempo
// chip, Follow and the stretch mode. The chips ask the arrangement to open
// their editors; what they show comes from the clip tempo controller.
Row {
    id: strip
    objectName: "clipStrip"
    required property var tempo       // the controller's entry for this clip, or undefined
    required property string name
    required property color amberColor
    required property color accentColor
    required property string monoFamily
    required property bool editable
    signal tempoRequested(Item chip)
    signal modeRequested(Item chip)
    signal followToggled(bool follow)

    readonly property bool known: !!tempo && tempo.bpm > 0
    readonly property bool unsure: known && !tempo.corrected && tempo.confidence < 0.5
    readonly property bool follows: !!tempo && tempo.follow

    spacing: Theme.clipStripSpacing
    height: Theme.clipStripHeight

    component Chip: Rectangle {
        id: chip
        property string label
        property bool lit: false
        property color tone: Theme.chipText
        property bool mono: false
        // Inline components do not see the file's ids: the strip passes these.
        property color accent
        property string monoFamily
        property bool editable
        signal activated()
        height: Theme.clipChipHeight
        width: chipText.implicitWidth + Theme.clipChipPadding
        radius: Theme.radiusSm
        // Dark, so they read on any track colour.
        color: lit ? Qt.tint(Theme.panelRaised, Qt.rgba(tone.r, tone.g, tone.b, Theme.clipChipLitAlpha)) : Theme.panelRaised
        border.color: lit ? tone : activeFocus ? accent : Theme.clipChipEdge
        activeFocusOnTab: editable
        Accessible.role: Accessible.Button
        Keys.onReturnPressed: chip.activated()
        Keys.onSpacePressed: chip.activated()
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.lit ? chip.tone : Theme.chipText
            font.pixelSize: Theme.typeCaption
            font.family: chip.mono ? chip.monoFamily : Qt.application.font.family
            font.weight: chip.mono ? Font.Normal : Font.DemiBold
        }
        MouseArea {
            anchors.fill: parent
            enabled: chip.editable
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.activated()
        }
    }

    Text {
        id: title
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, Math.max(0, strip.width - (chips.visible ? chips.width + Theme.spaceSm : 0)))
        text: strip.name
        elide: Text.ElideRight
        color: Theme.canvas
        font.pixelSize: Theme.typeSmall
        font.weight: Font.DemiBold
    }
    Row {
        id: chips
        spacing: Theme.clipStripSpacing
        anchors.verticalCenter: parent.verticalCenter
        // Narrow clips keep their name; the chips need about 170 px.
        visible: !!strip.tempo && strip.width >= Theme.clipStripMinWidth

        Chip {
            id: tempoChip
            accent: strip.accentColor
            monoFamily: strip.monoFamily
            editable: strip.editable
            objectName: "clipTempoChip"
            mono: true
            label: !strip.tempo ? "" : strip.tempo.analysing ? "♩ …"
                : strip.known ? "♩ " + strip.tempo.bpm.toFixed(2) : "♩ set"
            lit: strip.unsure
            tone: strip.amberColor
            Accessible.name: strip.known
                ? "Track tempo " + strip.tempo.bpm.toFixed(2) + " BPM" + (strip.unsure ? ", unsure: check it" : "")
                : "Track tempo not known; set it"
            onActivated: strip.tempoRequested(tempoChip)
        }
        Chip {
            objectName: "clipFollowChip"
            accent: strip.accentColor
            monoFamily: strip.monoFamily
            editable: strip.editable
            label: strip.follows ? "FOLLOW" : "OWN TEMPO"
            lit: strip.follows
            tone: strip.accentColor
            Accessible.name: strip.follows ? "Follows the project tempo; switch to its own tempo"
                                           : "Plays at its own tempo; switch to follow the project tempo"
            onActivated: strip.followToggled(!strip.follows)
        }
        Chip {
            id: modeChip
            accent: strip.accentColor
            monoFamily: strip.monoFamily
            editable: strip.editable
            objectName: "clipModeChip"
            label: strip.tempo ? strip.tempo.mode + " ▾" : ""
            Accessible.name: "Stretch mode " + (strip.tempo ? strip.tempo.mode : "")
            onActivated: strip.modeRequested(modeChip)
        }
    }
}
