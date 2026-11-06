pragma ComponentBehavior: Bound
import QtQuick
import "shell"

Rectangle {
    id: menu
    transformOrigin: Item.TopLeft
    Reveal { target: menu; fromScale: Motion.popScale }
    objectName: "midiClipContextMenu"
    required property var bridge
    required property string clipId
    required property string timebase
    required property bool looping
    required property color raisedColor
    required property color lineColor
    required property color mainTextColor
    required property color dimTextColor
    property string clipColor: ""
    property string clipName: ""
    signal closeRequested()
    signal clipDeleted()
    // The selected clips became one, now the selection.
    signal clipsMerged()

    width: Theme.clipMenuWidth
    height: Theme.clipMenuTitleHeight + entries.length * Theme.clipMenuEntryStride + Theme.spaceXs + extrasRow.implicitHeight + Theme.spaceSm
    readonly property var loopBrace: bridge.loopTransport
    readonly property bool loopSet: loopBrace !== null && loopBrace.enabled
    // Right-clicked inside a selected group: duplicate, merge and delete
    // take the whole group.
    readonly property int groupSize: bridge.selectedClipIds.indexOf(clipId) >= 0
        ? bridge.selectedClipIds.length : 1
    readonly property var entries: (groupSize > 1 ? [
            {name: "Merge " + groupSize + " clips", hint: "Into one clip on this track; gaps stay silent",
             action: "merge", keys: "Ctrl+J"},
            {name: "Duplicate " + groupSize + " clips", hint: "After the selection, same tracks",
             action: "duplicate", keys: "Ctrl+D"}
        ] : [
            {name: "Duplicate after clip", hint: "Keep notes, loop and clip settings", action: "duplicate",
             keys: "Ctrl+D"}
        ]).concat([
            {name: "Split at playhead", hint: "Cut in two at the current position", action: "split",
             keys: "Ctrl+E"},
            {name: menu.looping ? "Turn loop off" : "Loop clip",
             hint: menu.looping ? "Right edge sets the clip's length" : "Right edge repeats the whole clip",
             action: "loop", keys: ""},
            {name: menu.timebase === "Musical" ? "Lock to seconds" : "Lock to bars",
             hint: menu.timebase === "Musical" ? "Stays at its time when the tempo changes"
                                               : "Moves with the bars when the tempo changes",
             action: "timebase", keys: ""},
            {name: "Apply groove", hint: menu.bridge.noteTools.currentGroove + " · choose in the piano roll's Tools",
             action: "groove", keys: ""},
            {name: "Humanise", hint: "Nudge timing and velocity; again for another take",
             action: "humanise", keys: ""},
            {name: "Bounce to audio", hint: "Its sound on a new audio track; this one muted",
             action: "bounce", keys: ""}
        ]).concat(menu.loopSet ? [
            {name: "Consolidate the loop", hint: "Bars " + menu.loopBrace.startBar + "–"
                 + menu.loopBrace.endBar + ": this track's clips become one", action: "consolidate",
             keys: ""},
            {name: "Arrange from the loop", hint: "A whole track from bars " + menu.loopBrace.startBar
                 + "–" + menu.loopBrace.endBar + ": keep it or bin it", action: "arrange", keys: ""}
        ] : []).concat([
            {name: groupSize > 1 ? "Delete " + groupSize + " clips" : "Delete clip",
             hint: "Undo restores notes and settings", action: "delete", keys: "Delete"}
        ])
    radius: Theme.radiusXl
    color: raisedColor
    border.color: lineColor
    activeFocusOnTab: true
    Accessible.role: Accessible.PopupMenu
    Accessible.name: "MIDI clip actions"
    Keys.onEscapePressed: closeRequested()

    Text {
        x: Theme.spaceLg
        y: Theme.clipMenuTitleY
        text: "MIDI CLIP · " + (menu.timebase === "Musical" ? "ON BARS" : "ON SECONDS")
        color: menu.dimTextColor
        font.pixelSize: Theme.typeCaption
        font.bold: true
        font.letterSpacing: Theme.capsLetterSpacing
    }
    Repeater {
        model: menu.entries
        delegate: Rectangle {
            id: entry
            required property var modelData
            required property int index
            x: Theme.spaceSm
            y: Theme.clipMenuTitleHeight + index * Theme.clipMenuEntryStride
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
            MouseArea {
                id: action
                objectName: entry.modelData.action === "duplicate" ? "duplicateMidiClipAction"
                    : entry.modelData.action === "split" ? "splitMidiClipAction"
                    : entry.modelData.action === "timebase" ? "toggleMidiClipTimebaseAction"
                    : entry.modelData.action === "loop" ? "toggleMidiClipLoopAction"
                    : entry.modelData.action === "groove" ? "applyMidiClipGrooveAction"
                    : entry.modelData.action === "humanise" ? "humaniseMidiClipAction"
                    : entry.modelData.action === "consolidate" ? "consolidateMidiClipsAction"
                    : entry.modelData.action === "bounce" ? "bounceMidiClipAction"
                    : entry.modelData.action === "merge" ? "mergeMidiClipsAction"
                    : entry.modelData.action === "arrange" ? "arrangeFromLoopAction"
                    : "deleteMidiClipAction"
                anchors.fill: parent
                hoverEnabled: true
                activeFocusOnTab: true
                Accessible.role: Accessible.MenuItem
                Accessible.name: entry.modelData.name
                function activate() {
                    if (entry.modelData.action === "duplicate" && menu.groupSize > 1)
                        menu.bridge.duplicate_selection()
                    else if (entry.modelData.action === "duplicate")
                        menu.bridge.duplicate_midi_clip_after(menu.clipId)
                    else if (entry.modelData.action === "merge") {
                        menu.bridge.merge_selected_clips()
                        menu.clipsMerged()
                    }
                    else if (entry.modelData.action === "split")
                        menu.bridge.split_midi_clip_at_playhead(menu.clipId)
                    else if (entry.modelData.action === "timebase")
                        menu.bridge.toggle_midi_clip_timebase(menu.clipId)
                    else if (entry.modelData.action === "loop")
                        menu.bridge.set_midi_clip_loop(menu.clipId, !menu.looping)
                    else if (entry.modelData.action === "groove")
                        menu.bridge.noteTools.apply_current_groove(menu.clipId, [])
                    else if (entry.modelData.action === "humanise")
                        menu.bridge.noteTools.humanize(menu.clipId, [], 0.35, 0.4)
                    else if (entry.modelData.action === "bounce")
                        menu.bridge.freezeCommands.bounce_clip(menu.clipId)
                    else if (entry.modelData.action === "arrange")
                        menu.bridge.arrange.openRequested()
                    else if (entry.modelData.action === "consolidate")
                        menu.bridge.freezeCommands.consolidate_bars(
                            menu.bridge.clip_track_id(menu.clipId), menu.loopBrace.startBar,
                            menu.loopBrace.endBar)
                    else if (menu.groupSize > 1) {
                        menu.bridge.delete_selected_clips()
                        menu.clipDeleted()
                    } else {
                        menu.bridge.delete_midi_clip(menu.clipId)
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
        nameable: false
        mainTextColor: menu.mainTextColor
        dimTextColor: menu.dimTextColor
        lineColor: menu.lineColor
        onDone: menu.closeRequested()
    }
}
