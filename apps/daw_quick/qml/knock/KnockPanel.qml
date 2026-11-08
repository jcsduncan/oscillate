pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "../shell" as Shell
import Oscillate.Format

// The Knock percussion panel (ADR-0068): eight
// pads on the left, each named by its sound and flashing with every hit;
// the global strip below them (Decay first, then the room, velocity and
// output, Swing and Orbit), then the Patterns area (loops, the generator,
// the working pattern and Write to clip); on the right where each pad
// lands in the stereo field and the selected pad's ringing modes, live
// from its last hit, over its chain as cards (Striker, Resonator, Coupled,
// Body), then Finish, Width, Hand and Output. The kit browser sits in the
// header.
//
// Values go through `controller` (NativeSynthDeviceController); `knock`
// (KnockPanelModel) names the pads, gives each pad's parameter ids,
// designs the modes the view draws and holds the patterns.
Rectangle {
    id: panel
    required property var controller
    property var knock: null
    property KnockTheme theme: KnockTheme {}
    readonly property int revision: controller.revision
    // Where Write to clip puts a new clip (the song's playhead, in bars from
    // 0) and the arrangement's selected clips (a MIDI clip on this track is
    // replaced).
    property real playheadBar: 0
    property var selectedClipIds: []
    // Asks the host to play a pad's key (the bridge's audition).
    signal padTriggered(int key)
    // Asks the host to play the working pattern (the bridge's audition).
    signal auditionRequested()

    readonly property int padCount: knock !== null ? knock.pad_count : 0
    readonly property int firstKey: knock !== null ? knock.first_key : 0
    readonly property int columns: 4
    // How often the panel reads the synth's telemetry.
    readonly property int pollMs: 33
    readonly property var keyNames: knock !== null ? knock.key_names : []
    readonly property var labels: knock !== null && knock.revision >= 0 ? knock.pad_labels : []
    // The pad whose chain shows on the right, from 0.
    property int selectedPad: 0
    readonly property var selectedModes: knock !== null && knock.revision >= 0
        ? knock.modes(selectedPad) : []
    // When each pad was last hit (Date.now() ms), -1 before any.
    property var padHitAt: []

    // Live state (knock.hpp telemetry): each pad's output, the latest
    // hit's pad and the hit count, which changes with every hit.
    property var tele: ({ live: false })
    readonly property var levels: tele.live === true && tele.levels !== undefined ? tele.levels : []
    property int seenHits: 0
    // Where each pad's latest hit landed (-1 left .. 1 right) and when
    // (Date.now() ms, -1 before any), for the stereo strip.
    property var places: []
    property var landedAt: []

    objectName: "knockPanel"
    implicitWidth: theme.panelWidth
    implicitHeight: theme.panelHeight
    radius: theme.panelRadius
    color: theme.caseColor
    border.color: theme.line
    Accessible.role: Accessible.Pane
    Accessible.name: "Knock percussion"

    // Pad `pad`'s parameter `control` ("res.pitch"), or "" without the model.
    function padParameter(pad, control) {
        return knock !== null ? knock.pad_parameter(pad, control) : ""
    }
    function padId(control) {
        return padParameter(selectedPad, control)
    }
    function labelOf(pad) {
        return labels[pad] !== undefined && labels[pad] !== "" ? labels[pad] : "Pad " + (pad + 1)
    }
    function keyNameOf(pad) {
        return keyNames[pad] !== undefined ? keyNames[pad] : ""
    }

    function poll() {
        tele = controller.telemetry()
        if (tele.live !== true || tele.hits === undefined)
            return
        if (tele.hits !== seenHits) {
            seenHits = tele.hits
            if (tele.last_pad >= 0 && tele.last_pad < padCount)
                struck(tele.last_pad)
            landed(tele.last_pad, tele.places !== undefined ? tele.places : [])
        }
    }
    // The latest hit's pad lands, and so does any pad whose place moved
    // (several hits between two polls).
    function landed(lastPad, now) {
        const times = landedAt.slice()
        while (times.length < padCount)
            times.push(-1)
        let moved = false
        for (let pad = 0; pad < padCount && pad < now.length; ++pad) {
            const known = places[pad] !== undefined
            if (pad === lastPad || (known && places[pad] !== now[pad])) {
                times[pad] = Date.now()
                moved = true
            }
        }
        places = now.slice()
        if (moved)
            landedAt = times
    }
    Timer {
        interval: panel.pollMs
        repeat: true
        running: panel.visible
        onTriggered: panel.poll()
    }

    // A pad was hit: it flashes, and its modes ring from now.
    function struck(pad) {
        const times = padHitAt.slice()
        while (times.length < padCount)
            times.push(-1)
        times[pad] = Date.now()
        padHitAt = times
        const item = padRepeater.itemAt(pad)
        if (item)
            item.flash()
    }
    // Selects a pad, flashes it and asks the host to play it.
    function play(pad) {
        if (pad < 0 || pad >= padCount)
            return
        selectedPad = pad
        struck(pad)
        padTriggered(firstKey + pad)
    }
    function walk(from, direction) {
        const next = from + direction
        if (next >= 0 && next < padCount) {
            selectedPad = next
            const item = padRepeater.itemAt(next)
            if (item)
                item.forceActiveFocus()
        }
    }

    // Value words where the engine's own display says less.
    function percent(fraction) {
        return Format.fraction(fraction)
    }
    function toneText(value, info) {
        return value <= info.minimum ? "thing" : value >= info.maximum ? "note" : ""
    }
    function bendText(value, info) {
        if (value === 0)
            return "none"
        return value > 0 ? "↓ " + panel.percent(value / info.maximum)
                         : "↑ " + panel.percent(value / info.minimum)
    }
    function offText(value, info) {
        return value <= info.minimum ? "off" : ""
    }
    function fullText(value, info) {
        return value >= info.maximum ? "full" : ""
    }
    function handText(value, info) {
        return value <= info.minimum ? "machine" : ""
    }

    component Slide: Kit.Slider {
        controller: panel.controller
        theme: panel.theme
        width: parent ? parent.width : implicitWidth
    }
    component Pick: Kit.ChoiceTiles {
        controller: panel.controller
        theme: panel.theme
        width: parent ? parent.width : implicitWidth
        onPicked: panel.play(panel.selectedPad)
    }
    component Dial: Kit.Knob {
        controller: panel.controller
        theme: panel.theme
        readout: true
    }
    component Note: Text {
        width: parent ? parent.width : implicitWidth
        color: panel.theme.inkFaint
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.typeBody
        lineHeight: panel.theme.noteLineHeight
        wrapMode: Text.WordWrap
    }
    component Label: Text {
        color: panel.theme.inkDim
        font.family: panel.theme.sans
        font.pixelSize: panel.theme.typeBody
        font.bold: true
        font.letterSpacing: panel.theme.labelSpacing
    }
    // A card in the chain: a coloured dot, its title and its rows.
    component Stage: Rectangle {
        id: stage
        property string title: ""
        property color dot: panel.theme.strike
        default property alias rows: column.data
        radius: panel.theme.cardRadius
        color: panel.theme.panel2
        border.color: panel.theme.line
        Row {
            x: panel.theme.stageInset
            y: panel.theme.stageInset
            spacing: panel.theme.stageTitleGap
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: panel.theme.stageDot
                height: panel.theme.stageDot
                radius: panel.theme.stageDot / 2
                color: stage.dot
            }
            Text {
                text: stage.title.toUpperCase()
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeLabel
                font.bold: true
                font.letterSpacing: panel.theme.stageTitleSpacing
            }
        }
        Column {
            id: column
            x: panel.theme.stageInset
            y: panel.theme.stageBodyTop
            width: stage.width - 2 * panel.theme.stageInset
            spacing: panel.theme.spaceSm
        }
    }

    // ---- Header ---------------------------------------------------------------------

    Item {
        id: header
        x: panel.theme.spaceXl
        y: 0
        width: panel.width - 2 * panel.theme.spaceXl
        height: panel.theme.headerHeight

        Row {
            id: wordmark
            y: panel.theme.wordmarkTop
            Repeater {
                model: ["KN", "O", "CK"]
                delegate: Text {
                    required property string modelData
                    required property int index
                    text: modelData
                    color: index === 1 ? panel.theme.strike : panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.wordmarkSize
                    font.weight: Font.Black
                    font.letterSpacing: panel.theme.wordmarkSpacing
                }
            }
        }
        Text {
            y: wordmark.y + wordmark.height + panel.theme.taglineGap
            text: "MODELLED PERCUSSION"
            color: panel.theme.inkDim
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeMicro
            font.bold: true
            font.letterSpacing: panel.theme.taglineSpacing
        }

        Row {
            x: panel.theme.kitBoxX
            y: panel.theme.kitBoxTop
            spacing: panel.theme.spaceXs
            Kit.PresetArrow {
                objectName: "knockKitPrevious"
                controller: panel.controller
                theme: panel.theme
                direction: -1
            }
            Rectangle {
                id: kitBox
                objectName: "knockKitBox"
                width: panel.theme.kitBoxWidth
                height: panel.theme.kitBoxHeight
                radius: panel.theme.kitBoxRadius
                color: panel.theme.well
                border.color: activeFocus ? panel.theme.strike : panel.theme.line
                activeFocusOnTab: true
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Kit"
                Accessible.description: panel.controller.preset_name
                Keys.onReturnPressed: panel.openKits()
                Keys.onSpacePressed: panel.openKits()
                Text {
                    x: panel.theme.kitBoxInset
                    y: panel.theme.kitBoxCaptionTop
                    text: "KIT" + (panel.controller.preset_category !== "" ? " · " + panel.controller.preset_category.toUpperCase() : "")
                        + (panel.controller.preset_modified ? " · EDITED" : "")
                    color: panel.theme.inkFaint
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeMicro
                    font.bold: true
                    font.letterSpacing: panel.theme.kitBoxCaptionSpacing
                }
                Text {
                    objectName: "knockKitName"
                    x: panel.theme.kitBoxInset
                    y: panel.theme.kitBoxNameTop
                    width: kitBox.width - panel.theme.kitBoxNameReserve
                    text: panel.controller.preset_name !== "" ? panel.controller.preset_name : "Custom kit"
                    color: panel.theme.ink
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeHeading
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: panel.theme.spaceLg
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: panel.theme.inkDim
                    font.pixelSize: panel.theme.typeBody
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: panel.controller.editable
                    onClicked: {
                        kitBox.forceActiveFocus()
                        panel.openKits()
                    }
                    Shell.Tip { text: "Kits" }
                }
            }
            Kit.PresetArrow {
                objectName: "knockKitNext"
                controller: panel.controller
                theme: panel.theme
                direction: 1
            }
        }

        Column {
            anchors.right: parent.right
            y: panel.theme.statusTop
            width: panel.theme.statusWidth
            spacing: panel.theme.statusGap
            Text {
                objectName: "knockStatus"
                readonly property color tone: panel.controller.deferred ? panel.theme.warning
                    : panel.controller.status_detail !== "" ? panel.theme.warning : panel.theme.inkFaint
                width: parent.width
                horizontalAlignment: Text.AlignRight
                text: panel.controller.status_text
                color: tone
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeBody
                elide: Text.ElideRight
            }
            Text {
                objectName: "knockPeak"
                width: parent.width
                horizontalAlignment: Text.AlignRight
                text: panel.tele.live === true && panel.tele.output_peak_db !== undefined
                    ? "OUT " + Format.decibels(panel.tele.output_peak_db) : ""
                color: panel.theme.inkDim
                font.family: panel.theme.mono
                font.pixelSize: panel.theme.typeBody
            }
        }
    }

    // ---- Pads and the global strip ------------------------------------------------

    Item {
        id: left
        x: panel.theme.spaceXl
        y: header.height + panel.theme.spaceMd
        width: panel.theme.leftColumnWidth
        height: panel.height - y - panel.theme.spaceXl

        Label {
            id: padsLabel
            text: "PADS · " + panel.keyNameOf(0) + " TO " + panel.keyNameOf(panel.padCount - 1)
        }
        Grid {
            id: padGrid
            y: padsLabel.height + panel.theme.spaceMd
            width: parent.width
            columns: panel.columns
            spacing: panel.theme.spaceMd
            Repeater {
                id: padRepeater
                model: panel.padCount
                delegate: KnockPad {
                    theme: panel.theme
                    width: (padGrid.width - padGrid.spacing * (panel.columns - 1)) / panel.columns
                    height: panel.theme.padGridHeight
                    columns: panel.columns
                    label: panel.labelOf(index)
                    keyName: panel.keyNameOf(index)
                    selected: index === panel.selectedPad
                    level: panel.levels[index] !== undefined ? panel.levels[index] : 0
                    onPressed: panel.play(index)
                    onWalked: direction => panel.walk(index, direction)
                }
            }
        }

        Stage {
            id: globalStrip
            objectName: "knockGlobal"
            y: padGrid.y + padGrid.height + panel.theme.spaceLg
            width: parent.width
            height: panel.theme.globalStripHeight
            title: "Global"
            dot: panel.theme.ink
            // Six knobs share the strip: the five beside Decay are smaller
            // and split what Decay leaves evenly.
            readonly property int smallCount: 5
            readonly property real smallDial: panel.theme.globalSmallDial
            readonly property real smallWidth: (globalStrip.width - 2 * panel.theme.stageInset - decayDial.width)
                / smallCount
            Row {
                spacing: 0
                Dial {
                    id: decayDial
                    parameterId: "knock.decay"
                    diameter: panel.theme.globalDecayDial
                    accent: panel.theme.strike
                }
                // The room's two knobs and the output say whose they are:
                // their table names ("Size", "Level") repeat.
                Dial { parameterId: "knock.room.size"; label: "Room"; y: panel.theme.globalSmallDialDrop; diameter: globalStrip.smallDial; width: globalStrip.smallWidth }
                Dial { parameterId: "knock.room.level"; label: "Wet"; y: panel.theme.globalSmallDialDrop; diameter: globalStrip.smallDial; width: globalStrip.smallWidth }
                Dial { parameterId: "knock.velocity"; y: panel.theme.globalSmallDialDrop; diameter: globalStrip.smallDial; width: globalStrip.smallWidth }
                Dial { parameterId: "knock.out.level"; label: "Output"; y: panel.theme.globalSmallDialDrop; diameter: globalStrip.smallDial; width: globalStrip.smallWidth }
                // Swing plays the off-beat sixteenths late, from the song.
                Dial {
                    parameterId: "knock.swing"
                    label: "Swing"
                    y: panel.theme.globalSmallDialDrop
                    diameter: globalStrip.smallDial
                    width: globalStrip.smallWidth
                    accent: panel.theme.coupled
                }
            }
            Row {
                id: orbitRow
                spacing: panel.theme.spaceMd
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "ORBIT"
                    width: panel.theme.orbitLabelWidth
                }
                // How long an Orbit motion takes to go round.
                Pick {
                    parameterId: "knock.orbit"
                    columns: 4
                    width: globalStrip.width - 2 * panel.theme.stageInset - panel.theme.orbitLabelWidth - orbitRow.spacing
                    accent: panel.theme.coupled
                }
            }
        }

        KnockPatterns {
            id: patternArea
            y: globalStrip.y + globalStrip.height + panel.theme.spaceLg
            width: parent.width
            height: parent.height - y
            theme: panel.theme
            patterns: panel.knock !== null ? panel.knock.patterns : null
            labels: panel.labels
            kitName: panel.controller.preset_name
            playheadBar: panel.playheadBar
            selectedClipIds: panel.selectedClipIds
            onAuditionRequested: panel.auditionRequested()
        }
    }

    // ---- The selected pad -----------------------------------------------------------

    Item {
        id: right
        x: left.x + left.width + panel.theme.spaceXl
        y: header.height + panel.theme.spaceMd
        width: panel.width - x - panel.theme.spaceXl
        height: panel.height - y - panel.theme.spaceXl

        Rectangle {
            id: scope
            width: parent.width
            height: panel.theme.scopeHeight
            radius: panel.theme.scopeRadius
            color: panel.theme.well
            border.color: panel.theme.line
            clip: true
            // Above the modes: where each pad lands, L to R.
            KnockStereo {
                id: stereo
                x: panel.theme.hairline
                y: panel.theme.hairline
                width: parent.width - 2 * panel.theme.hairline
                height: panel.theme.stereoHeight
                theme: panel.theme
                places: panel.places
                landedAt: panel.landedAt
                selectedPad: panel.selectedPad
                placeMin: panel.knock !== null ? panel.knock.place_min : -1
                placeMax: panel.knock !== null ? panel.knock.place_max : 1
            }
            KnockModes {
                x: panel.theme.hairline
                y: stereo.y + stereo.height
                width: parent.width - 2 * panel.theme.hairline
                height: parent.height - y - panel.theme.hairline
                theme: panel.theme
                modes: panel.selectedModes
                // The model's range; the view's own until the model is set.
                Binding on minHz {
                    when: panel.knock !== null
                    value: panel.knock?.view_min_hz
                }
                Binding on maxHz {
                    when: panel.knock !== null
                    value: panel.knock?.view_max_hz
                }
                hitAt: panel.padHitAt[panel.selectedPad] !== undefined ? panel.padHitAt[panel.selectedPad] : -1
            }
        }

        Item {
            id: padHead
            y: scope.height + panel.theme.rightSectionGap
            width: parent.width
            height: panel.theme.padHeadHeight
            Text {
                id: padName
                objectName: "knockPadName"
                anchors.verticalCenter: parent.verticalCenter
                text: panel.labelOf(panel.selectedPad)
                color: panel.theme.ink
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.padNameSize
                font.weight: Font.Black
            }
            Text {
                objectName: "knockPadDetail"
                x: padName.x + padName.implicitWidth + panel.theme.padHeadSpacing
                anchors.verticalCenter: parent.verticalCenter
                text: {
                    if (panel.revision < 0)
                        return ""
                    const pitch = panel.padId("res.pitch")
                    return "Pad " + (panel.selectedPad + 1) + " · " + panel.keyNameOf(panel.selectedPad)
                        + (pitch !== "" ? " · rings at " + panel.controller.display(pitch) : "")
                }
                color: panel.theme.inkDim
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeEmphasis
            }
            // The pad's resonator tunes to the project's key.
            Row {
                anchors.right: hitPad.left
                anchors.rightMargin: panel.theme.padHeadSpacing
                anchors.verticalCenter: parent.verticalCenter
                spacing: panel.theme.spaceSm
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Follows key"
                    color: panel.theme.inkDim
                    font.family: panel.theme.sans
                    font.pixelSize: panel.theme.typeBody
                }
                Pick {
                    parameterId: panel.padId("key.follow")
                    columns: 2
                    width: panel.theme.followPickWidth
                    accent: panel.theme.resonator
                }
            }
            Kit.Button {
                id: hitPad
                theme: panel.theme
                objectName: "knockHitPad"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                label: "Hit pad"
                onClicked: panel.play(panel.selectedPad)
            }
        }

        // Striker → Resonator → Coupled → Body.
        Row {
            id: chain
            y: padHead.y + padHead.height + panel.theme.rightSectionGap
            width: parent.width
            height: panel.theme.chainHeight
            spacing: panel.theme.chainSpacing
            readonly property real cardWidth: (width - 3 * spacing) / 4

            Stage {
                width: chain.cardWidth
                height: chain.height
                title: "Striker"
                dot: panel.theme.strike
                Pick { parameterId: panel.padId("striker.type"); columns: 4; accent: panel.theme.strike }
                Slide { parameterId: panel.padId("striker.hardness"); accent: panel.theme.strike }
                Slide { parameterId: panel.padId("striker.position"); accent: panel.theme.strike }
                Slide { parameterId: panel.padId("striker.click"); accent: panel.theme.strike }
                Note { text: "Velocity hits harder, so a loud hit is a brighter one." }
            }
            Stage {
                width: chain.cardWidth
                height: chain.height
                title: "Resonator"
                dot: panel.theme.resonator
                Pick { parameterId: panel.padId("res.material"); accent: panel.theme.resonator }
                Slide { parameterId: panel.padId("res.pitch"); accent: panel.theme.resonator }
                Slide {
                    parameterId: panel.padId("res.tone")
                    accent: panel.theme.resonator
                    format: panel.toneText
                }
                Slide { parameterId: panel.padId("res.decay"); accent: panel.theme.resonator }
                Slide { parameterId: panel.padId("res.damping"); accent: panel.theme.resonator }
                Slide {
                    parameterId: panel.padId("res.bend")
                    accent: panel.theme.resonator
                    format: panel.bendText
                }
            }
            Stage {
                width: chain.cardWidth
                height: chain.height
                title: "Coupled"
                dot: panel.theme.coupled
                Pick { parameterId: panel.padId("couple.material"); accent: panel.theme.coupled }
                Slide { parameterId: panel.padId("couple.tuning"); accent: panel.theme.coupled }
                Slide {
                    parameterId: panel.padId("couple.amount")
                    accent: panel.theme.coupled
                    format: panel.offText
                }
                Note { text: "A second resonator the first sets ringing: a skin on a box, a plate on a tube." }
            }
            Stage {
                width: chain.cardWidth
                height: chain.height
                title: "Body"
                dot: panel.theme.body
                Slide { parameterId: panel.padId("body.size"); accent: panel.theme.body }
                Slide { parameterId: panel.padId("body.hollow"); accent: panel.theme.body }
                Note { text: "The box around the resonator: its low modes and the air inside it." }
            }
        }

        // Finish, Width, Hand and Output.
        Row {
            id: outRow
            y: chain.y + chain.height + panel.theme.spaceLg
            width: parent.width
            height: panel.theme.outRowHeight
            spacing: panel.theme.outRowSpacing
            readonly property real cardWidth: (width - 3 * spacing) / 4

            Stage {
                width: outRow.cardWidth
                height: outRow.height
                title: "Finish"
                dot: panel.theme.strike
                Slide { parameterId: panel.padId("finish.snap"); accent: panel.theme.strike }
                Slide {
                    parameterId: panel.padId("finish.length")
                    accent: panel.theme.strike
                    format: panel.fullText
                }
                Slide {
                    parameterId: panel.padId("finish.lift")
                    accent: panel.theme.strike
                    format: panel.offText
                }
                Slide {
                    parameterId: panel.padId("finish.crush")
                    accent: panel.theme.strike
                    format: panel.offText
                }
            }
            Stage {
                objectName: "knockWidth"
                width: outRow.cardWidth
                height: outRow.height
                title: "Width"
                dot: panel.theme.coupled
                Slide {
                    parameterId: panel.padId("width.spread")
                    label: "Spread"
                    accent: panel.theme.coupled
                }
                Pick {
                    parameterId: panel.padId("width.motion")
                    columns: 2
                    accent: panel.theme.coupled
                }
                Slide {
                    parameterId: panel.padId("width.depth")
                    label: "Depth"
                    accent: panel.theme.coupled
                }
                Slide {
                    parameterId: panel.padId("width.haas")
                    label: "Haas"
                    accent: panel.theme.coupled
                    format: panel.offText
                }
                Row {
                    width: parent.width
                    spacing: panel.theme.spaceSm
                    Text {
                        id: followsLabel
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Follows swing"
                        color: panel.theme.inkDim
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeBody
                    }
                    Pick {
                        parameterId: panel.padId("swing.follow")
                        columns: 2
                        width: parent.width - followsLabel.implicitWidth - panel.theme.spaceSm
                        accent: panel.theme.coupled
                    }
                }
            }
            Stage {
                width: outRow.cardWidth
                height: outRow.height
                title: "Hand"
                dot: panel.theme.ink
                Slide {
                    parameterId: panel.padId("hand")
                    accent: panel.theme.ink
                    format: panel.handText
                }
                Note {
                    text: "Each hit lands a little differently in force, place and tune. The variation "
                        + "is seeded, so an export matches playback."
                }
            }
            Stage {
                width: outRow.cardWidth
                height: outRow.height
                title: "Output"
                dot: panel.theme.inkDim
                Slide { parameterId: panel.padId("level"); accent: panel.theme.ink }
                Slide { parameterId: panel.padId("pan"); accent: panel.theme.ink }
                Slide { parameterId: panel.padId("room"); accent: panel.theme.ink }
            }
        }
    }

    function openKits() {
        if (!controller.editable)
            return
        menu.openPresets(kitBox, controller, theme.strike)
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }
}
