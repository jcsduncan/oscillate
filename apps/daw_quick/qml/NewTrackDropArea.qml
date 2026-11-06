pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// Files and VST3 instruments dropped anywhere in the window make a track.
// Browser cards (BrowserCatalogue.dragPayload under
// application/x-oscillate-device) are refused here so the targets below take
// them: a track row or lane (TrackDeviceDrop) and the empty space under the
// last track (DeviceTrackDrop). The two never overlap.
DropArea {
    id: area
    objectName: "newTrackDropArea"
    signal audioDropped(url source)
    signal midiDropped(url source)
    // A MIDI file on an instrument track goes into it instead.
    signal midiDroppedOnTrack(url source, string trackId, int bar)
    // (item, x, y) -> {trackId, bar} or null.
    property var instrumentTargetAt: null
    signal instrumentDropped(int entryIndex, string classId)
    readonly property string instrumentFormat: "application/x-oscillate-vst3-instrument"

    onEntered: function(drag) {
        drag.accepted = drag.hasUrls && drag.urls.length > 0
            || drag.formats.indexOf(instrumentFormat) !== -1
    }
    onDropped: function(drop) {
        if (drop.formats.indexOf(instrumentFormat) !== -1) {
            try {
                const choice = JSON.parse(drop.getDataAsString(instrumentFormat))
                if (Number.isInteger(choice.index) && typeof choice.classId === "string") {
                    instrumentDropped(choice.index, choice.classId)
                    drop.acceptProposedAction()
                } else {
                    drop.accepted = false
                }
            } catch (error) {
                drop.accepted = false
            }
        } else if (drop.hasUrls && drop.urls.length > 0) {
            const source = drop.urls[0]
            const midi = /\.(mid|midi|smf)$/i.test(String(source))
            const target = midi && instrumentTargetAt ? instrumentTargetAt(area, drop.x, drop.y) : null
            if (target)
                midiDroppedOnTrack(source, target.trackId, target.bar)
            else if (midi)
                midiDropped(source)
            else
                audioDropped(source)
            drop.acceptProposedAction()
        }
    }
    Rectangle {
        anchors.fill: parent
        visible: area.containsDrag
        color: Theme.dropZoneFill
        border.color: Theme.dropZoneEdge
        border.width: Theme.strokeBold
    }

}
