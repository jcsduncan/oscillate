pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: menu
    transformOrigin: Item.TopLeft
    Reveal { target: menu; fromScale: Motion.popScale }
    objectName: "audioClipContextMenu"
    required property var bridge
    required property string clipId
    required property string timebase
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    property string clipColor: ""
    property string clipName: ""
    signal closeRequested()
    signal clipDeleted()

    width: Theme.clipMenuWidth
    // Six actions, then a divider before the maintenance one.
    height: Theme.clipMenuTitleHeight + actionRows.count * Theme.clipMenuEntryStride + Theme.clipMenuDividerGap + Theme.spaceXs + extrasRow.implicitHeight + Theme.spaceSm
    radius: Theme.radiusXl
    color: raisedColor
    border.color: lineColor
    activeFocusOnTab: true
    Accessible.role: Accessible.PopupMenu
    Accessible.name: "Audio clip actions"
    Keys.onEscapePressed: closeRequested()

    Text {
        x: Theme.spaceLg
        y: Theme.clipMenuTitleY
        text: "AUDIO CLIP · " + (menu.timebase === "Musical" ? "ON BARS" : "ON SECONDS")
        color: menu.dimTextColor
        font.pixelSize: Theme.typeCaption
        font.bold: true
        font.letterSpacing: Theme.capsLetterSpacing
    }
    Repeater {
        id: actionRows
        model: [
            {name: "Duplicate after clip", hint: "Place a copy immediately after", action: "duplicate",
             keys: "Ctrl+D"},
            {name: "Split at playhead", hint: "Cut at the current timeline position", action: "split",
             keys: "Ctrl+E"},
            {name: menu.timebase === "Musical" ? "Lock to seconds" : "Lock to bars",
             hint: menu.timebase === "Musical" ? "Stays at its time when the tempo changes"
                                               : "Moves with the bars when the tempo changes",
             action: "timebase", keys: ""},
            {name: "Take its groove…", hint: "Measure its kick, hats or percussion for the groove pool",
             action: "groove", keys: ""},
            {name: "Convert to sampler", hint: "Crate on a new MIDI track, chopped at its hits",
             action: "sampler", keys: ""},
            {name: "Delete clip", hint: "Keep the source audio in the project", action: "delete",
             keys: "Delete"},
            {name: "Rebuild waveform", hint: "Reanalyse verified source audio", action: "rebuild",
             keys: ""}
        ]
        delegate: Rectangle {
            id: entry
            required property var modelData
            required property int index
            objectName: modelData.action === "rebuild" ? "rebuildWaveformActionRow"
                : modelData.action === "duplicate" ? "duplicateAudioClipActionRow"
                : modelData.action === "split" ? "splitAudioClipActionRow"
                : modelData.action === "timebase" ? "toggleAudioClipTimebaseActionRow"
                : modelData.action === "groove" ? "extractGrooveActionRow"
                : modelData.action === "sampler" ? "convertAudioClipToSamplerActionRow"
                : "deleteAudioClipActionRow"
            x: Theme.spaceSm
            y: Theme.clipMenuTitleHeight + index * Theme.clipMenuEntryStride + (modelData.action === "rebuild" ? Theme.clipMenuDividerGap : 0)
            width: menu.width - Theme.spaceLg
            height: Theme.clipMenuEntryHeight
            radius: Theme.clipMenuEntryRadius
            color: action.containsMouse ? Theme.menuHover : "transparent"
            Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
            Text {
                x: Theme.clipMenuEntryTextX
                y: Theme.spaceXs
                text: entry.modelData.name
                color: menu.mainTextColor
                font.pixelSize: Theme.typeBody
                font.bold: true
            }
            Text {
                x: Theme.clipMenuEntryTextX
                y: Theme.clipMenuEntryHintY
                text: entry.modelData.hint
                color: menu.dimTextColor
                font.pixelSize: Theme.typeCaption
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: Theme.clipMenuEntryTextX
                y: Theme.clipMenuKeysY
                text: entry.modelData.keys
                color: menu.dimTextColor
                font.pixelSize: Theme.typeCaption
            }
            Rectangle {
                visible: entry.modelData.action === "rebuild"
                y: -(Theme.clipMenuDividerGap + Theme.hairline) / 2
                width: parent.width
                height: Theme.hairline
                color: menu.lineColor
            }
            MouseArea {
                id: action
                objectName: entry.modelData.action === "rebuild" ? "rebuildWaveformAction"
                    : entry.modelData.action === "duplicate" ? "duplicateAudioClipAction"
                    : entry.modelData.action === "split" ? "splitAudioClipAction"
                    : entry.modelData.action === "timebase" ? "toggleAudioClipTimebaseAction"
                    : entry.modelData.action === "groove" ? "extractGrooveAction"
                    : entry.modelData.action === "sampler" ? "convertAudioClipToSamplerAction"
                    : "deleteAudioClipAction"
                anchors.fill: parent
                hoverEnabled: true
                activeFocusOnTab: true
                Accessible.role: Accessible.MenuItem
                Accessible.name: entry.modelData.name
                function activate() {
                    const clip = menu.clipId
                    if (entry.modelData.action === "rebuild")
                        menu.bridge.rebuild_clip_waveform(clip)
                    else if (entry.modelData.action === "duplicate")
                        menu.bridge.duplicate_audio_clip_after(clip)
                    else if (entry.modelData.action === "split")
                        menu.bridge.split_audio_clip_at_playhead(clip)
                    else if (entry.modelData.action === "timebase")
                        menu.bridge.toggle_audio_clip_timebase(clip)
                    else if (entry.modelData.action === "groove")
                        menu.bridge.grooveCapture.open_for_clip(clip)
                    else if (entry.modelData.action === "sampler")
                        menu.bridge.convert_audio_clip_to_sampler(clip)
                    else {
                        menu.bridge.delete_audio_clip(clip)
                        menu.clipDeleted()
                    }
                    menu.closeRequested()
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                onClicked: activate()
            }
        }
    }
    Rectangle {
        x: Theme.spaceSm
        y: menu.height - extrasRow.implicitHeight - Theme.spaceMd
        width: menu.width - Theme.spaceLg
        height: Theme.hairline
        color: menu.lineColor
    }
    ClipMenuExtras {
        id: extrasRow
        y: menu.height - implicitHeight - Theme.spaceXs
        width: menu.width
        bridge: menu.bridge
        clipId: menu.clipId
        clipColor: menu.clipColor
        clipName: menu.clipName
        nameable: true
        mainTextColor: menu.mainTextColor
        dimTextColor: menu.dimTextColor
        lineColor: menu.lineColor
        onDone: menu.closeRequested()
    }
}
