pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A browser instrument on empty arrangement space: a new track playing it.
DropArea {
    id: space
    required property var bridge
    required property color accentColor
    readonly property string deviceFormat: "application/x-oscillate-device"
    keys: [deviceFormat]

    function payloadOf(event) {
        try {
            return JSON.parse(event.getDataAsString(deviceFormat))
        } catch (error) {
            return null
        }
    }
    function fits(payload) {
        return Boolean(payload) && payload.kind === "instrument"
            && ["native", "starter", "vst3"].includes(payload.source)
    }
    onEntered: drag => { drag.accepted = fits(payloadOf(drag)) }
    onDropped: drop => {
        const payload = payloadOf(drop)
        if (!fits(payload)) {
            drop.accepted = false
            return
        }
        if (payload.source === "vst3")
            bridge.vst3Browser.create_vst3_instrument_track(payload.vst3Index, payload.classId)
        else if (payload.source === "starter")
            bridge.create_starter_sound_track(payload.preset)
        else
            bridge.create_native_instrument_track(payload.engine, payload.preset)
        drop.accept(Qt.CopyAction)
    }
    Rectangle {
        anchors.fill: parent
        visible: space.containsDrag
        color: Qt.rgba(space.accentColor.r, space.accentColor.g, space.accentColor.b, Theme.dropWashAlpha)
        border.color: space.accentColor
        border.width: Theme.hairline
        Text {
            anchors.centerIn: parent
            text: "Drop to create a track"
            color: space.accentColor
            font.pixelSize: Theme.typeBody
            font.letterSpacing: Theme.dropHintLetterSpacing
        }
    }
}
