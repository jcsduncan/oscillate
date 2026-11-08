pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The browser's shared model: categories, search, tag filter, favourites,
// recents and the action rules for every card. The full browser
// (Vst3BrowserPanel) and the docked column (DockedBrowser) both read it, so
// a double-click or a drag means the same thing everywhere.
//
// A card is a map: key, engineId, engineName, presetIndex (-1: defaults),
// presetName, tag, category, kind, color, about, instrument (bool), source
// ("native" | "starter" | "vst3" | "file"), for VST3 cards the
// scanner entry as `entry`, and for the user's samples (source "file",
// bridge.browserPreview.samples) their `path` and `url`. Cards keep `id` (= engineId) and `group`
// (= category) for callers of the earlier browser API.
QtObject {
    id: catalogue
    objectName: "browserCatalogue"
    required property var bridge
    required property var selectedTrack

    // Category state. VST3 plugins are shown when showBuiltIns is false;
    // showInstruments picks instruments or effects within VST3, and
    // instruments or effects among built-ins when no `narrowTo` narrows them.
    // It follows the selected track's kind until a category is chosen; with
    // no track selected (an empty project) it opens on instruments, the
    // first step.
    property bool showInstruments: !selectedTrack || selectedTrack.kind === "instrument"
    property bool showBuiltIns: true
    // "", "Drums", "Samples", "Effects", "Utilities", "Favourites", "Recent".
    property string narrowTo: ""
    property string query: ""
    property string tag: "All"
    property string selectedKey: ""
    property bool changingCategory: false
    onShowBuiltInsChanged: if (!changingCategory) { narrowTo = ""; tag = "All" }
    onShowInstrumentsChanged: if (!changingCategory) { narrowTo = ""; tag = "All" }

    readonly property var categoryNames: ["Instruments", "Drums", "Effects", "MIDI Effects", "Utilities",
        "Samples", "Racks", "VST3 plugins", "Favourites", "Recent"]
    readonly property string category: !showBuiltIns ? "VST3 plugins"
        : narrowTo.length > 0 ? narrowTo
        : showInstruments ? "Instruments" : "Effects"

    function selectCategory(name) {
        changingCategory = true
        tag = "All"
        if (name === "VST3 plugins") {
            showBuiltIns = false
            narrowTo = ""
        } else {
            showBuiltIns = true
            if (name === "Instruments") { showInstruments = true; narrowTo = "" }
            else if (name === "Drums" || name === "Samples") { showInstruments = true; narrowTo = name }
            else if (name === "Effects" || name === "Utilities") { showInstruments = false; narrowTo = name }
            else narrowTo = name
        }
        changingCategory = false
    }

    // ---- Cards -----------------------------------------------------------
    readonly property var nativeCards: (bridge.browserCatalogue || []).map(card =>
        Object.assign({}, card, { id: card.engineId, group: card.category,
                                  name: card.presetName, detail: card.kind + " · " + card.engineName }))
    readonly property var vst3Cards: (bridge.vst3Browser.vst3BrowserEntries || []).map(entry => ({
        key: "vst3:" + (entry.classId || entry.bundle), id: "vst3", engineId: "vst3",
        engineName: entry.vendor || "VST3", presetIndex: -1, presetName: entry.name,
        name: entry.name, tag: entry.kind === "instrument" ? "Instrument" : "Effect",
        category: "VST3 plugins", group: "VST3 plugins",
        kind: entry.kind === "instrument" ? "VST3 instrument" : "VST3 effect",
        detail: (entry.vendor || entry.bundle) + (entry.state === "ready" ? "" : " · " + entry.state),
        color: entry.kind === "instrument" ? Theme.browserVst3InstrumentColor : Theme.browserVst3EffectColor,
        about: entry.diagnostic || entry.bundle, instrument: entry.kind === "instrument",
        source: "vst3", entry: entry }))
    // The user's sample folder (ADR-0059), under Samples beside the starters.
    readonly property var preview: bridge.browserPreview || null
    readonly property var fileCards: (preview ? preview.samples || [] : []).map(card =>
        Object.assign({}, card, { id: card.engineId, group: card.category,
                                  name: card.presetName, detail: card.kind }))
    // The user's racks (ADR-0059): a track's devices and macros, loaded onto
    // the selected track.
    readonly property var rackCards: (bridge.racks ? bridge.racks.racks || [] : []).map(rack => ({
        key: "rack:" + rack.name, id: "rack", engineId: "rack", engineName: "Rack",
        presetIndex: -1, presetName: rack.name, name: rack.name, tag: "Rack",
        category: "Racks", group: "Racks", kind: "Rack", detail: "Rack · devices and macros",
        color: Theme.browserRackColor, about: "Replaces the selected track's built-in devices and macros",
        instrument: false, source: "rack" }))
    // MIDI effects (ADR-0059): they play into the selected track's built-in synth.
    readonly property var noteEffectCards: (bridge.noteEffects ? bridge.noteEffects.engines : [])
        .map(engine => ({
            key: "notefx:" + engine.id, id: engine.id, engineId: engine.id,
            engineName: engine.name, presetIndex: -1, presetName: engine.name, name: engine.name,
            tag: "MIDI", category: "MIDI Effects", group: "MIDI Effects", kind: "MIDI effect",
            detail: "MIDI effect · before the synth", color: Theme.browserNoteEffectColor,
            about: "Plays into the selected track's built-in synth", instrument: false,
            source: "notefx", presets: engine.presets }))
    readonly property var allCards: nativeCards.concat(vst3Cards, fileCards, rackCards,
                                                       noteEffectCards)

    // Every word of a search must appear in what the card is, its name, its
    // description, its presets or its keywords ("comp", "glue", "verb").
    function haystack(card) {
        return (card.presetName + " " + card.engineName + " " + card.kind + " " + card.tag + " "
                + (card.blurb || "") + " " + (card.presets || []).map(preset => preset.name).join(" ") + " "
                + card.category + " " + (card.section || "") + " " + (card.keywords || "")).toLowerCase()
    }
    function queryWords() {
        return query.trim().toLowerCase().split(/\s+/).filter(word => word.length > 0)
    }
    function matches(card) {
        const text = haystack(card)
        return queryWords().every(word => text.includes(word))
    }
    // How well a matching card fits: what it is or its name first, then a
    // keyword or word that starts with the search, then anything else.
    function score(card) {
        const kind = (card.kind || "").toLowerCase()
        const name = (card.presetName || "").toLowerCase()
        const words = haystack(card).split(/[^a-z0-9&]+/)
        let total = 0
        for (const word of queryWords()) {
            if (kind === word || name === word) total += 100
            else if (kind.startsWith(word) || name.startsWith(word)) total += 60
            else if ((card.keywords || "").split(" ").some(keyword => keyword.startsWith(word))) total += 40
            else if (words.some(candidate => candidate.startsWith(word))) total += 20
            else total += 1
        }
        return total
    }
    // Listing order: by section (Dynamics before Space), then the catalogue's
    // own order; cards without one keep theirs.
    function listed(cards) {
        return cards.map((card, index) => ({ card, index })).sort((a, b) =>
            (a.card.sectionRank ?? -1) - (b.card.sectionRank ?? -1)
            || (a.card.rank ?? -1) - (b.card.rank ?? -1)
            || a.index - b.index).map(entry => entry.card)
    }
    // The Instruments tab lists drums too, under their own heading; the Drums
    // chip narrows to them.
    function inCategory(card, name) {
        if (name === "Favourites") return (bridge.browserFavourites || []).includes(card.key)
        if (name === "Recent") return (bridge.browserRecents || []).includes(card.key)
        if (name === "Instruments") return card.category === "Instruments" || card.category === "Drums"
        return card.category === name
    }
    function count(name) {
        return allCards.filter(card => inCategory(card, name)).length
    }
    // A search spans every category; otherwise the category narrows.
    readonly property var categoryCards: {
        const text = query.trim()
        if (text.length > 0) {
            const found = listed(allCards.filter(card => matches(card)))
            const scores = new Map(found.map(card => [card.key, score(card)]))
            return found.map((card, index) => ({ card, index })).sort((a, b) =>
                scores.get(b.card.key) - scores.get(a.card.key) || a.index - b.index).map(entry => entry.card)
        }
        const base = allCards.filter(card => inCategory(card, category))
        if (category === "Recent") {
            const order = bridge.browserRecents || []
            return base.slice().sort((a, b) => order.indexOf(a.key) - order.indexOf(b.key))
        }
        return listed(base)
    }
    // Filter chips: a device's section ("Dynamics"), else the card's tag.
    function filterTag(card) { return card.section || card.tag }
    readonly property var tags: ["All"].concat(categoryCards.map(card => filterTag(card))
        .filter((value, index, list) => value && list.indexOf(value) === index))
    readonly property var cards: categoryCards.filter(card => tag === "All" || filterTag(card) === tag)
    readonly property var nativeResults: cards.filter(card => card.source !== "vst3")
    readonly property var selectedCard: allCards.find(card => card.key === selectedKey)
        || cards[0] || null
    // The selected engine's presets, each a card that starts from that preset.
    readonly property var presetCards: selectedCard && selectedCard.presets
        ? selectedCard.presets.map(preset => Object.assign({}, selectedCard, {
            presetIndex: preset.index, presetName: preset.name, tag: preset.category,
            key: selectedCard.key + ":" + preset.name })) : []
    // A device is listed by what it is ("Compressor"), its own name beside
    // it; a preset by its name.
    function isDevice(card) { return Boolean(card) && card.source === "native" && card.presetIndex < 0 && Boolean(card.kind) }
    function title(card) { return isDevice(card) ? card.kind : card.presetName }
    function subtitle(card) {
        if (isDevice(card)) return card.engineName + (card.blurb ? " · " + card.blurb : "")
        return card.blurb || (card.engineName + " · " + card.kind)
    }
    // The heading a card is listed under while browsing (not searching).
    function heading(card) { return card ? card.section || card.category || "" : "" }
    function isFavourite(key) { return (bridge.browserFavourites || []).includes(key) }

    // ---- Targets and VST3 eligibility (the live-path rules) ----------------
    readonly property int selectedTrackIndex: selectedTrack
        ? bridge.tracks.findIndex(track => track.id === selectedTrack.id) : -1
    readonly property var selectedEffects: bridge.insertChains[selectedTrackIndex >= 0
        ? selectedTrackIndex : bridge.tracks.length]
        ? bridge.insertChains[selectedTrackIndex >= 0
            ? selectedTrackIndex : bridge.tracks.length].effects || [] : []
    readonly property string selectedPathId: selectedTrack && selectedTrack.id
        ? selectedTrack.id : "master"
    // Any instrument track, whatever it plays now (a VST3 one too).
    readonly property bool nativeInstrumentEligible: selectedTrack
        && selectedTrack.kind === "instrument"
    // Stopped, or playing: a chain edit switches live (ADR-0050). Re-read
    // whenever playback or the chains change.
    readonly property bool nativeEffectEligible: {
        // Re-checked whenever what chain_edit_enabled() reads changes,
        // including the VST3 scan the browser runs when it opens.
        bridge.insertChainEditEnabled
        bridge.playbackActive
        bridge.insertChains
        bridge.vst3Browser.vst3BrowserBusy
        return bridge.chain_edit_enabled(selectedPathId)
            && (selectedPathId === "master" || Boolean(selectedTrack)
                && ["audio", "instrument", "group", "return"].includes(selectedTrack.kind))
    }
    // Any number of instruments and chains play together (ADR-0050).
    readonly property bool newInstrumentEligible: true
    // Whether the selected track can take a VST3 instrument (instruments)
    // or a VST3 effect (!instruments).
    function vst3TrackEligible(instruments) {
        return Boolean(selectedTrack && (instruments
            ? selectedTrack.kind === "instrument"
                && !selectedTrack.nativeSourceReady && !selectedTrack.vst3SourceReady
            : ["audio", "instrument", "group", "return"].includes(selectedTrack.kind)))
    }

    // ---- Actions -----------------------------------------------------------
    // The card's primary action, the one a double-click runs: a new track for
    // an instrument or sound, the end of the selected chain for an effect.
    function primaryLabel(card) {
        if (!card) return ""
        if (card.source === "file") return "New audio track with " + card.presetName
        if (card.source === "rack")
            return "Load onto " + (selectedTrack && selectedTrack.name ? selectedTrack.name : "the selected track")
        if (card.source === "notefx")
            return "Add " + card.presetName + " before " + (selectedTrack && selectedTrack.name
                ? selectedTrack.name + "'s synth" : "the selected synth")
        if (card.source === "vst3") {
            if (card.entry.state !== "ready") return "Retry " + card.name
            return card.instrument
                ? (vst3TrackEligible(true) ? "Load into the selected track" : "New track with " + card.name)
                : "Add " + card.name + " to the selected track"
        }
        return card.instrument ? "New track with " + card.presetName
            : "Add " + card.engineName + " to " + (selectedPathId === "master" ? "the master"
                : selectedTrack && selectedTrack.name ? selectedTrack.name : "the selected track")
    }
    function primaryEnabled(card) {
        if (!card) return false
        if (card.source === "file") return Boolean(bridge.projectOpen) && Boolean(bridge.imports.importEnabled)
        if (card.source === "rack") return Boolean(bridge.projectOpen) && Boolean(selectedTrack && selectedTrack.id)
        if (card.source === "notefx")
            return Boolean(bridge.projectOpen) && Boolean(selectedTrack && selectedTrack.id)
                && bridge.noteEffects.accepts(selectedTrack.id)
        if (card.source === "vst3") {
            if (bridge.vst3Browser.vst3BrowserBusy) return false
            if (card.entry.state !== "ready") return true
            return Boolean(card.entry.insertable) && (card.instrument
                ? vst3TrackEligible(true) || newInstrumentEligible : vst3TrackEligible(false))
        }
        return card.instrument ? Boolean(bridge.projectOpen) : nativeEffectEligible
    }
    function activate(card) {
        if (!card || !primaryEnabled(card)) return
        selectedKey = card.key
        if (card.source === "file") {
            bridge.imports.import_audio(card.url)
            bridge.note_browser_use(card.key)
        } else if (card.source === "rack") {
            bridge.racks.load_rack(selectedTrack.id, card.presetName)
            bridge.note_browser_use(card.key)
        } else if (card.source === "notefx") {
            bridge.noteEffects.add(selectedTrack.id, card.engineId, -1, card.presetIndex)
            bridge.note_browser_use(card.key)
        } else if (card.source === "vst3") {
            const entry = card.entry
            if (entry.state !== "ready")
                bridge.vst3Browser.retry_vst3_bundle(entry.bundle)
            else if (card.instrument && vst3TrackEligible(true))
                bridge.vst3Browser.insert_vst3_instrument(entry.index, entry.classId, selectedTrack.id)
            else if (card.instrument)
                bridge.vst3Browser.create_vst3_instrument_track(entry.index, entry.classId)
            else
                bridge.vst3Browser.insert_vst3_effect(entry.index, entry.classId, selectedTrack.id)
            bridge.note_browser_use(card.key)
        } else if (card.source === "starter") {
            bridge.create_starter_sound_track(card.presetIndex)
        } else if (card.instrument) {
            bridge.create_native_instrument_track(card.engineId, card.presetIndex)
        } else {
            bridge.add_effect_preset(selectedPathId, selectedEffects.length, card.engineId,
                                     card.presetIndex)
        }
    }
    // The secondary action: swap the selected track's instrument, or put the
    // effect on the master.
    function secondaryLabel(card) {
        if (!card || card.source === "vst3" || card.source === "file" || card.source === "rack"
                || card.source === "notefx") return ""
        if (card.source === "starter") return "Use on the selected track"
        return card.instrument ? "Replace the selected track's instrument" : "Add to the master"
    }
    function secondaryEnabled(card) {
        if (!card || card.source === "vst3" || card.source === "file" || card.source === "rack"
                || card.source === "notefx") return false
        if (card.instrument) return nativeInstrumentEligible
        return bridge.chain_edit_enabled(selectedPathId)
    }
    function activateSecondary(card) {
        if (!secondaryEnabled(card)) return
        selectedKey = card.key
        if (card.source === "starter")
            bridge.bind_starter_sound(selectedTrack.id, card.presetIndex)
        else if (card.instrument)
            bridge.replace_native_instrument(selectedTrack.id, card.engineId, card.presetIndex)
        else
            bridge.add_effect_preset("master", (bridge.insertChains[bridge.tracks.length] || { effects: [] }).effects.length,
                                     card.engineId, card.presetIndex)
    }
    // Previews (ADR-0059): an instrument preset plays a short phrase that
    // suits it; a sample plays, stretched to the song's tempo and, while the
    // song plays, from its next beat. With auto-preview on, selecting a card
    // (click or arrow keys) previews it; hovering never does.
    function canPreview(card) {
        return Boolean(card) && (card.source === "native" && card.instrument || card.source === "file")
    }
    property bool previewing: false
    readonly property bool autoPreview: preview ? preview.autoPreview : false
    readonly property string playingKey: preview ? preview.playingKey : ""
    function previewCard(card) {
        if (!canPreview(card)) return
        if (card.source === "file") {
            bridge.stop_browser_preview()
            preview.preview_sample(card.key)
        } else {
            bridge.preview_browser_preset(card.engineId, card.presetIndex)
        }
        previewing = true
    }
    function togglePreview(card) {
        if (previewing) {
            bridge.stop_browser_preview()
            previewing = false
        } else {
            previewCard(card)
        }
    }
    onSelectedKeyChanged: if (autoPreview && selectedKey.length > 0)
        previewDelay.restart()
    // A short wait, so arrowing through cards plays only where you stop.
    readonly property int previewDelayMs: 260
    property Timer previewDelay: Timer {
        interval: catalogue.previewDelayMs
        onTriggered: {
            const card = catalogue.allCards.find(candidate => candidate.key === catalogue.selectedKey)
            if (card && catalogue.canPreview(card)) catalogue.previewCard(card)
        }
    }

    // The drag payload every card carries (MIME application/x-oscillate-device,
    // UTF-8 JSON). Drop targets decide by `kind`:
    //   {"source":"native"|"starter"|"vst3", "kind":"instrument"|"effect",
    //    "engine":"<engine id>", "preset":<preset index or -1>, "key":"<card key>",
    //    "vst3Index":<scanner index>, "classId":"<VST3 class id>"}
    // An instrument dropped on a track header or lane replaces that track's
    // instrument (bridge.replace_native_instrument / bind_starter_sound /
    // insert_vst3_instrument); dropped on empty arrangement space it creates a
    // track (activate()). An effect dropped on a track, or between devices at
    // an index, inserts there (bridge.add_effect_preset /
    // insert_vst3_effect).
    // A sample card drags as a file (text/uri-list), so every drop target
    // that takes audio files takes it.
    function dragMime(card) {
        return card.source === "file" ? { "text/uri-list": card.url }
            : { "application/x-oscillate-device": dragPayload(card) }
    }
    function dragPayload(card) {
        return JSON.stringify({
            source: card.source,
            kind: card.source === "notefx" ? "midi-effect" : card.instrument ? "instrument" : "effect",
            engine: card.engineId, preset: card.presetIndex, key: card.key,
            vst3Index: card.entry ? card.entry.index : -1,
            classId: card.entry ? card.entry.classId : ""
        })
    }
}
