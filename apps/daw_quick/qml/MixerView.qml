pragma ComponentBehavior: Bound
import QtQuick
import "mixer"
import "shell"

// The mixer (redesign, ADR-0043 follow-up): one strip per track in
// arrangement order, groups among them, then the returns, then the master.
// Self-contained: the shell loads it with `bridge` and listens for the
// requests it cannot satisfy itself (adding an insert, dedicated native
// editors that live in the device chain).
Rectangle {
    id: mixer
    objectName: "mixerView"
    required property var bridge

    // '+ Insert' on a strip: the shell opens the browser for that path.
    signal browserRequested(string pathId)
    // The master section's Rebalance button: the shell opens the view.
    signal rebalanceRequested()

    color: Theme.canvas
    Accessible.role: Accessible.Pane
    Accessible.name: "Mixer"

    readonly property var trackPalette: Theme.mixerTrackPalette
    readonly property var returnPalette: Theme.mixerReturnPalette
    readonly property var engineColors: Theme.mixerEngineColors

    function trackColor(track, index) {
        return track.color ? track.color : trackPalette[index % trackPalette.length]
    }
    function sourceName(track) {
        if (track.kind !== "instrument")
            return ""
        if (track.nativeEngineName)
            return track.nativeEngineName
        if (track.vst3SourceReady && track.vst3Plugins && track.vst3Plugins.length > 0)
            return track.vst3Plugins[0].name
        return track.soundName ? track.soundName : ""
    }
    function kindLabel(track) {
        switch (track.kind) {
        case "instrument": {
            const source = sourceName(track)
            return source ? "MIDI · " + source : "MIDI"
        }
        case "group": return "Group"
        case "return": return "Return"
        default: return "Audio"
        }
    }
    function insertsOf(chain) {
        const effects = chain && chain.effects ? chain.effects : []
        return effects.map(effect => ({
            slotId: effect.slotId,
            name: effect.engineName || effect.name || effect.type || "Effect",
            enabled: effect.enabled !== false,
            pluginId: effect.pluginId || "",
            kind: effect.nativeKind || "",
            color: effect.pluginId ? Theme.textDim
                : effect.nativeKind === "effect" ? (engineColors[effect.engine] || Theme.accent)
                : Theme.textDim
        }))
    }
    function truePeakText() {
        const reading = bridge.masterTruePeak
        if (!reading || !reading.valid)
            return "True peak —"
        const db = value => value <= 0 ? "−∞" : (20 * Math.log10(value)).toFixed(1).replace("-", "−")
        return "TP " + db(reading.left) + " / " + db(reading.right) + " dBTP"
    }

    // The returns, lettered in track order. A string, so strips rebind only
    // when the returns themselves change, not on every track edit.
    readonly property string returnsKey: {
        const returns = []
        for (const track of (bridge.tracks || [])) {
            if (track.kind !== "return")
                continue
            returns.push({ id: track.id, letter: String.fromCharCode(65 + returns.length),
                           color: track.color ? track.color : returnPalette[returns.length % returnPalette.length],
                           name: track.name })
        }
        return JSON.stringify(returns)
    }
    readonly property var returns: JSON.parse(returnsKey)
    // One track's strip, from its own row and insert chain only.
    function stripFor(track, index, chain) {
        const returnInfo = returns.find(item => item.id === track.id)
        const audible = (track.sends || []).filter(send => send.purpose !== "sidechain")
        return {
            id: track.id, pathId: track.id, master: false, returnBus: track.kind === "return",
            name: track.name,
            kindLabel: returnInfo ? "Return " + returnInfo.letter : kindLabel(track),
            color: returnInfo ? returnInfo.color : trackColor(track, index),
            level: Number(track.level ?? 1), pan: Number(track.pan ?? 0),
            muted: track.muted === true, soloed: track.soloed === true,
            mainName: track.mainDestinationName || "Master",
            sourceName: sourceName(track),
            nativeSource: track.nativeSourceReady === true,
            vst3Source: track.vst3SourceReady === true,
            inserts: insertsOf(chain),
            sends: track.kind === "return" ? [] : returns.map(item => {
                const send = audible.find(candidate => candidate.destinationId === item.id)
                return { returnId: item.id, letter: item.letter, name: item.name, color: item.color,
                         sendId: send ? send.id : "", gain: send ? Number(send.gain) : 0 }
            }),
            truePeakText: ""
        }
    }
    // The master's chain is the last insert path; its true peak changes at
    // meter rate, so only the master strip reads it.
    readonly property string masterChainKey: {
        const chains = bridge.insertChains || []
        return JSON.stringify(chains.length > 0 ? chains[chains.length - 1] : null)
    }
    readonly property var masterStrip: ({
        id: "master", pathId: "master", master: true, returnBus: false, name: "Master",
        kindLabel: "Output", color: Theme.accent, level: Number(bridge.masterLevel ?? 1), pan: 0,
        muted: false, soloed: false, mainName: "Speakers", sourceName: "", nativeSource: false,
        vst3Source: false, inserts: insertsOf(JSON.parse(masterChainKey)), sends: [],
        truePeakText: truePeakText()
    })
    function routeOptionsFor(stripData) {
        const options = [{ id: "", name: "Master" }]
        for (const target of (bridge.routeTargets || [])) {
            if (target.kind === "group" && target.id !== stripData.id)
                options.push({ id: target.id, name: target.name })
        }
        return options
    }
    function activateInsert(pathId, insert) {
        if (insert.pluginId)
            bridge.vst3Editor.open_vst3_insert_editor(pathId, insert.slotId)
        else if (insert.kind === "effect")
            bridge.open_native_effect_editor(pathId, insert.slotId)
    }
    function activateSource(trackId) {
        const track = (bridge.tracks || []).find(item => item.id === trackId)
        if (!track)
            return
        if (track.nativeSourceReady)
            bridge.open_native_synth_editor(trackId)
        else if (track.vst3SourceReady)
            bridge.vst3Editor.open_vst3_instrument_editor(trackId)
    }

    // A track's strip, loaded by its row of the track model (the Loader
    // carries the row's index and fields).
    Component {
        id: trackStrip
        MixerStrip {
            id: strip
            readonly property int trackIndex: parent ? parent.index : 0
            readonly property var track: parent ? parent.modelData : ({})
            // This strip's insert chain, as a string so another track's
            // chain edit leaves it alone.
            readonly property string chainKey:
                JSON.stringify((mixer.bridge.insertChains || [])[trackIndex] || null)
            height: scroller.height
            bridge: mixer.bridge
            channel: mixer.stripFor(track, trackIndex, JSON.parse(chainKey))
            routeOptions: mixer.routeOptionsFor(strip.channel)
            onInsertActivated: function(pathId, insert) { mixer.activateInsert(pathId, insert) }
            onAddInsertRequested: function(pathId) { mixer.browserRequested(pathId) }
            onSourceActivated: function(trackId) { mixer.activateSource(trackId) }
        }
    }

    Flickable {
        id: scroller
        objectName: "mixerStrips"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: masterSection.left
        anchors.margins: Theme.spaceXl
        contentWidth: row.width
        contentHeight: height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Row {
            id: row
            height: scroller.height
            spacing: Theme.spaceMd
            // A new strip fades in; the strips after it slide aside.
            add: Transition {
                enabled: Motion.enabled
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Motion.normal; easing.type: Motion.easing }
            }
            move: Transition {
                enabled: Motion.enabled
                NumberAnimation { property: "x"; duration: Motion.normal; easing.type: Motion.easing }
            }
            // Strips repeat the track model: an edit rebinds only its own
            // track's strip, and only adding or removing a track builds one.
            // Tracks in order, then the returns, then the master.
            Repeater {
                model: mixer.bridge.trackModel
                delegate: Loader {
                    required property int index
                    required property var modelData
                    active: modelData.kind !== "return"
                    visible: active
                    sourceComponent: trackStrip
                }
            }
            Repeater {
                model: mixer.bridge.trackModel
                delegate: Loader {
                    required property int index
                    required property var modelData
                    active: modelData.kind === "return"
                    visible: active
                    sourceComponent: trackStrip
                }
            }
            MixerStrip {
                height: row.height
                bridge: mixer.bridge
                channel: mixer.masterStrip
                routeOptions: mixer.routeOptionsFor(mixer.masterStrip)
                onInsertActivated: function(pathId, insert) { mixer.activateInsert(pathId, insert) }
                onAddInsertRequested: function(pathId) { mixer.browserRequested(pathId) }
            }
        }
    }

    // Loudness, true peak, spectrum and the mono check stay in view however
    // far the strips scroll.
    MasterSectionPanel {
        id: masterSection
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spaceXl
        width: implicitWidth
        bridge: mixer.bridge
        onRebalanceRequested: mixer.rebalanceRequested()
    }
}
