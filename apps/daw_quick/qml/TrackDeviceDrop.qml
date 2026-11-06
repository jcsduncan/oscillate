pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// A browser card on a track row or lane. An instrument replaces the
// track's instrument (instrument tracks only); an effect goes at the end
// of the track's chain, as the device chain's own drop zone does; a MIDI
// effect goes at the end of the notes' way into a built-in synth.
DropArea {
    id: target
    required property var bridge
    required property var track
    required property int trackIndex
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
        if (!payload)
            return false
        if (payload.kind === "instrument")
            return track.kind === "instrument"
                && ["native", "starter", "vst3"].includes(payload.source)
        // A MIDI effect plays into the track's built-in synth (ADR-0059).
        if (payload.kind === "midi-effect")
            return Boolean(bridge.noteEffects) && bridge.noteEffects.accepts(track.id)
        return payload.kind === "effect"
            && (payload.source === "vst3" || bridge.chain_edit_enabled(track.id))
    }
    onEntered: drag => { drag.accepted = fits(payloadOf(drag)) }
    onDropped: drop => {
        const payload = payloadOf(drop)
        if (!fits(payload)) {
            drop.accepted = false
            return
        }
        if (payload.kind === "midi-effect") {
            bridge.noteEffects.add(track.id, payload.engine, -1, payload.preset)
        } else if (payload.kind === "instrument") {
            if (payload.source === "vst3")
                bridge.vst3Browser.insert_vst3_instrument(payload.vst3Index, payload.classId, track.id)
            else if (payload.source === "starter")
                bridge.bind_starter_sound(track.id, payload.preset)
            else
                bridge.replace_native_instrument(track.id, payload.engine, payload.preset)
        } else {
            const index = bridge.insertChains[trackIndex].effects.length
            if (payload.source === "vst3")
                bridge.vst3Browser.insert_vst3_effect(payload.vst3Index, payload.classId, track.id)
            else
                bridge.add_effect_preset(track.id, index, payload.engine, payload.preset)
        }
        drop.accept(Qt.CopyAction)
    }
    Rectangle {
        anchors.fill: parent
        visible: target.containsDrag
        color: Qt.rgba(target.accentColor.r, target.accentColor.g, target.accentColor.b, Theme.dropWashAlpha)
        border.color: target.accentColor
        border.width: Theme.hairline
    }
}
