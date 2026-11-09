pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import "NovaContract.js" as Contract
import Oscillate.Format

// The Nova device panel (ADR-0057, ADR-0076 revision 2, spec 08 §2,
// contract 08a): the device header (Kit.DeviceHeader: the NOVA wordmark,
// the preset browser ◀ name ▾ ▶, the readout of the control last touched
// and the status badge) with its second row of views and tools (BROWSE,
// ✦ CONSTELLATION, the page buttons MAIN, FX, MATRIX, ARP · CLIP, VOICES,
// GLOBAL; voices and CPU and the keys pill); the slim signal strip (a
// light, a name and a small live picture per module in signal order: OSC A,
// B, C, SUB · NOISE, FILTER 1, 2, FX, VOICES, OUT); the preset browser
// docked at the left; six pages (MAIN, the overview with every sound module
// on it, then FX, MATRIX, ARP · CLIP, GLOBAL, VOICES); the playable key
// strip along the bottom; and the wavetable editor overlay. The panel
// reflows to whatever size its host gives it at or above its minimum; a
// page that needs more than the area it gets is scaled down to fit (the
// overview scrolls instead when it is short), the header, strip and keys
// never are. Every module draws in its own accent (accentFor), on neutral
// graphite.
// Values go through `controller` (NativeSynthDeviceController); routes,
// curves, chains, media, labels and sequences through `controller.document`
// (NativeSynthDocumentModel). Live state is the engine's telemetry frame
// (contract §6), polled on its own clock while the panel shows.
Rectangle {
    id: panel
    required property var controller
    property NovaTheme theme: NovaTheme {}
    readonly property var doc: controller.document
    readonly property alias menu: menu
    readonly property alias presetBrowser: presetBrowser

    // Size (ADR-0076). The host lays the panel out at its area divided by
    // `zoom`, never below the minimum (it scales the panel down instead);
    // the designed size is what a host with no size of its own shows, and
    // holds the whole overview unscaled and unscrolled. The minimum width
    // holds the compact header (headerCompactWidth).
    readonly property real minimumWidth: 980
    readonly property real minimumHeight: 720
    readonly property real designedWidth: 1440
    readonly property real designedHeight: 1240
    // Below this width (the page area's, margins included) the overview
    // goes to two columns.
    readonly property real overviewNarrowWidth: 1050
    // The panel zoom the host chose (its zoom chips, ADR-0103).
    property real zoom: 1
    // The header: its rows and the room above and below them. Narrower than
    // headerCompactWidth it is compact: ✦ alone for CONSTELLATION and the
    // keys pill as its light.
    readonly property real headerHeight: header.y + header.height + theme.headerFoot
    readonly property real headerMargin: theme.spaceXl
    readonly property real headerCompactWidth: 1250
    readonly property bool compactHeader: width < headerCompactWidth
    readonly property real stripHeight: 28
    readonly property real keyStripHeight: 56
    readonly property real pageMargin: 12
    readonly property real browserWidth: 300
    // Where the pages sit: under the strip, right of the browser drawer,
    // over the keys.
    readonly property real bodyTop: headerHeight + stripHeight
    readonly property real bodyBottom: height - keyStripHeight
    readonly property real pageLeft: (presetBrowser.opened ? browserWidth : 0) + pageMargin
    readonly property real pageTop: bodyTop + pageMargin

    // The telemetry frame and whether a synth is sounding it.
    property var frame: []
    readonly property bool live: frame.length >= Contract.T.size
    // 0 MAIN (the overview), 1 FX, 2 MATRIX, 3 ARP · CLIP, 4 GLOBAL,
    // 5 VOICES.
    property int page: 0
    readonly property int mainPage: 0
    readonly property int fxPage: 1
    readonly property int matrixPage: 2
    readonly property int arpPage: 3
    readonly property int globalPage: 4
    readonly property int voicesPage: 5
    // The overview card open large over the others ("a", "b", "c",
    // "subnoise", "filter1", "filter2" or "mod"), or "" for none.
    property string focusModule: ""
    readonly property var expandableModules: ["a", "b", "c", "subnoise", "filter1", "filter2", "mod"]
    // The modulation source the MODULATION card edits: an envelope
    // ("env.1".."env.4") or an LFO ("lfo.1".."lfo.8").
    property string modTab: Contract.ampEnvelope
    // The strip's jump: the overview scrolls to `flashKey`'s card and
    // flashes it each time `flashSerial` moves on.
    property string flashKey: ""
    property int flashSerial: 0
    // The signal strip, in signal order, then VOICES and OUT. A sound
    // module jumps to its card on MAIN; FX, VOICES and OUT open their pages.
    readonly property var spineModules: [
        { key: "a", title: "OSC A", page: mainPage, accent: theme.cy },
        { key: "b", title: "OSC B", page: mainPage, accent: theme.cy },
        { key: "c", title: "OSC C", page: mainPage, accent: theme.cy },
        { key: "subnoise", title: "SUB · NOISE", page: mainPage, accent: theme.gr },
        { key: "filter1", title: "FILTER 1", page: mainPage, accent: theme.am },
        { key: "filter2", title: "FILTER 2", page: mainPage, accent: theme.am },
        { key: "fx", title: "FX", page: fxPage, accent: theme.pk },
        { key: "voices", title: "VOICES", page: voicesPage, accent: theme.vi },
        { key: "out", title: "OUT", page: globalPage, accent: theme.neutral }
    ]
    // Each module's accent, by parameter id: oscillators cyan, Sub · Noise
    // green, filters, envelopes and the sequencer amber, FX pink, voices,
    // LFOs, macros and routes violet; the rest (the output, global
    // settings) neutral white.
    readonly property var moduleAccents: [
        { prefix: "nova.osc.", accent: theme.cy }, { prefix: "nova.sub.", accent: theme.gr },
        { prefix: "nova.noise.", accent: theme.gr }, { prefix: "nova.filter.", accent: theme.am },
        { prefix: "nova.env.", accent: theme.am }, { prefix: "nova.arp.", accent: theme.am },
        { prefix: "nova.seq.", accent: theme.am }, { prefix: "nova.fx.", accent: theme.pk },
        { prefix: "nova.analogue.", accent: theme.vi }, { prefix: "nova.lfo.", accent: theme.vi },
        { prefix: "nova.macro.", accent: theme.vi }, { prefix: "nova.mod.", accent: theme.vi },
        { prefix: "nova.perf.", accent: theme.vi }
    ]
    // The header's page buttons, in order.
    readonly property var pageButtons: [{ label: "MAIN", page: mainPage }, { label: "FX", page: fxPage },
                                        { label: "MATRIX", page: matrixPage }, { label: "ARP · CLIP", page: arpPage },
                                        { label: "VOICES", page: voicesPage }, { label: "GLOBAL", page: globalPage }]
    // ✦ CONSTELLATION: the preset map shown over the page area (ADR-0077).
    property bool constellationOpen: false
    // The note the key strip's A key plays (the host's typing-keyboard
    // octave).
    property int keyBase: Contract.defaultKeyBase
    // The groove pool ([{name, stepsPerQuarter, timings, velocities}], the
    // host's NoteToolsController.grooves): the arp's GROOVE list.
    property var grooves: []
    // The arp page's pattern search has the keys (letters type, not play).
    property bool arpSearching: false
    // The effect chain the FX page shows.
    property string fxChain: "fx.main"
    // The oscillator whose wavetable editor is open ("a", "b", "c"), or "".
    property string editorOsc: ""

    // The header's readout: the last control touched, or a route.
    property string touchedId: ""
    property string touchedLabel: ""
    property var touchedFormatter: null
    property var touchedRoute: null

    // Drag to modulate.
    property string dragSource: ""
    property real dragX: 0
    property real dragY: 0
    property var dropKnob: null
    property var hoverKnob: null
    // Every knob on a loaded page, for drop hit-testing.
    property var knobs: []
    // A drop lands on a knob this far below its dial too.
    readonly property real dropSlack: 4
    // A new route's depth.
    readonly property real newRouteDepth: 0.3
    // The telemetry poll's interval while a synth sounds (about 60 Hz) and while idle, ms.
    readonly property int livePollInterval: 16
    readonly property int idlePollInterval: 200
    // CPU load above this reads in red.
    readonly property real cpuWarning: 0.8

    // A clip-sequencer clip dragged to the arrangement (NOVA-ARP-03); the
    // host makes a MIDI clip of it through its session commands.
    signal sequenceDragged(string sequence)
    // The key strip: the host plays these notes on the synth's track.
    signal keyPressed(int note, int velocity)
    signal keyReleased(int note)

    objectName: "novaPanel"
    implicitWidth: designedWidth
    implicitHeight: designedHeight
    radius: theme.panelRadius
    color: theme.plateMid
    border.color: theme.line2
    clip: true
    Accessible.role: Accessible.Pane
    Accessible.name: "Nova flagship synthesizer"

    Component.onCompleted: {
        for (let i = 1; i <= 8; ++i)
            doc.set_curve_default("lfo." + i, Contract.defaultLfo)
        for (let o = 0; o < 3; ++o)
            for (let w = 1; w <= 2; ++w)
                doc.set_curve_default("warp." + Contract.oscKeys[o] + "." + w,
                                      [{ x: 0, y: 0, tension: 0, step: false }, { x: 1, y: 1, tension: 0, step: false }])
    }

    // ---- Shared behaviour ----------------------------------------------------------------

    function at(index) {
        return live && index < frame.length ? Number(frame[index]) : 0
    }
    function sourceName(id) {
        if (id === undefined || id === "")
            return "—"
        if (id.indexOf("env.") === 0)
            return "ENV " + id.substring(4)
        if (id.indexOf("lfo.") === 0)
            return "LFO " + id.substring(4)
        if (id.indexOf("macro.") === 0)
            return doc.labels_revision >= 0 ? doc.label(id, "Macro " + id.substring(6)) : ""
        if (Contract.voiceNames[id] !== undefined)
            return Contract.voiceNames[id]
        if (Contract.arpNames[id] !== undefined)
            return Contract.arpNames[id]
        return Contract.perfNames[id] !== undefined ? Contract.perfNames[id] : id
    }
    // A destination's short name and group, for the matrix and readouts.
    function destinationName(id) {
        const info = controller.parameter(id)
        return info.name !== undefined ? info.name.toUpperCase() : id
    }
    function destinationGroup(id) {
        const info = controller.parameter(id)
        return info.group !== undefined ? info.group : ""
    }
    function registerKnob(knob) {
        knobs.push(knob)
    }
    function unregisterKnob(knob) {
        const index = knobs.indexOf(knob)
        if (index >= 0)
            knobs.splice(index, 1)
        if (dropKnob === knob)
            dropKnob = null
        if (hoverKnob === knob)
            hoverKnob = null
    }
    // While a card is open large, the overview's other knobs lie under the
    // scrim and take no drop.
    function underScrim(knob) {
        if (focusModule === "")
            return false
        let overview = false
        for (let item = knob.parent; item !== null; item = item.parent) {
            if (item.objectName === "novaModuleCard:" + focusModule)
                return false
            if (item.objectName === "novaOverview")
                overview = true
        }
        return overview
    }
    function knobAt(x, y) {
        for (let i = 0; i < knobs.length; ++i) {
            const k = knobs[i]
            if (!k.visible || !k.modulatable || underScrim(k))
                continue
            const p = k.mapFromItem(panel, x, y)
            if (p.x >= 0 && p.y >= 0 && p.x < k.width && p.y < k.dial + dropSlack)
                return k
        }
        return null
    }
    function touch(knob) {
        touchedRoute = null
        touchedId = knob.parameterId
        touchedLabel = knob.label
        touchedFormatter = knob.formatter
        theme.lastTouched = touchedId
    }
    function touchId(id, label) {
        touchedRoute = null
        touchedId = id
        touchedLabel = label
        touchedFormatter = null
        theme.lastTouched = id
    }
    function touchRoute(route, knob) {
        touchedId = route.destination
        touchedLabel = knob ? knob.label : destinationName(route.destination)
        touchedFormatter = null
        touchedRoute = route
        theme.lastTouched = touchedId
    }
    // The readout's name and value for `id`: a route as source → knob and
    // its depth, a Nova knob by its caption and formatter; a control that
    // did not go through touch() (a kit control) by its parameter's name.
    function touchedName(id) {
        if (id !== touchedId) {
            const info = controller.parameter(id)
            return info.name === undefined ? "" : info.name.toUpperCase()
        }
        return touchedRoute !== null ? sourceName(touchedRoute.source) + " → " + touchedLabel : touchedLabel
    }
    function touchedValue(id) {
        if (controller.revision < 0)
            return ""
        if (id !== touchedId)
            return controller.display(id)
        if (touchedRoute !== null)
            return Format.fraction(controller.value(touchedRoute.amount_id))
        return touchedFormatter ? touchedFormatter(controller.normalised(id)) : controller.display(id)
    }
    // The preset arrows: with a browser filter set, they walk what it shows.
    function stepPreset(direction) {
        if (presetBrowser.filterActive)
            presetBrowser.step(direction)
        else
            controller.step_preset(direction)
    }

    function beginSourceDrag(source, x, y) {
        if (!controller.editable)
            return
        dragSource = source
        dragX = x
        dragY = y
        hoverKnob = null
    }
    function moveSourceDrag(x, y) {
        if (dragSource === "")
            return
        dragX = x
        dragY = y
        dropKnob = knobAt(x, y)
    }
    function endSourceDrag(x, y) {
        if (dragSource === "")
            return
        const source = dragSource
        const target = x >= 0 ? knobAt(x, y) : null
        dragSource = ""
        dropKnob = null
        if (target === null)
            return
        const slot = doc.add_route(source, target.parameterId, newRouteDepth)
        if (slot >= 0)
            touchRoute(doc.route(slot), target)
    }

    function openKnobMenu(knob) {
        const items = [{ label: knob.label, value: "", header: true }]
        const routes = knob.routes
        for (let i = 0; i < routes.length; ++i) {
            const amount = controller.value(routes[i].amount_id)
            items.push({ label: "Remove " + sourceName(routes[i].source) + " ("
                                + Format.fraction(amount) + ")", value: "remove:" + routes[i].slot })
        }
        if (routes.length > 0)
            items.push({ label: "Edit in matrix", value: "matrix" })
        items.push({ label: "Reset to default", value: "reset" })
        menu.open(knob, items, null, theme.vi, value => {
            if (value.indexOf("remove:") === 0) {
                panel.doc.remove_route(parseInt(value.substring(7)))
                panel.touch(knob)
            } else if (value === "matrix") {
                panel.page = panel.matrixPage
            } else if (value === "reset") {
                panel.controller.reset_to_default(knob.parameterId)
                panel.touch(knob)
            }
        })
    }

    // A new effect slot's values: the module's defaults (contract §5) over
    // every p value, or `over` (a preset or copied settings), on.
    function moduleDefaults(uid, moduleId, over) {
        const values = Contract.slotValues(uid, Contract.module(moduleId), over)
        values["nova.fx." + uid + ".on"] = 1
        return values
    }
    // Adds `moduleId` at `index` of `chain` (the end when omitted) in the
    // lowest free slot uid, with `over` or its defaults; the uid or -1.
    function addEffect(chain, moduleId, index, over) {
        const uid = doc.free_chain_uid(Contract.maxFxUid)
        const count = doc.chain(chain).length
        if (uid < 0 || count >= Contract.fxChainSlots)
            return -1
        controller.set_values(moduleDefaults(uid, moduleId, over))
        const at = index === undefined ? count : index
        return doc.insert_chain_module(chain, at, moduleId, uid) ? uid : -1
    }

    function openEditor(osc) {
        editorOsc = osc
    }

    // ---- Accents ---------------------------------------------------------------------------

    // A parameter's module accent (moduleAccents), neutral white otherwise.
    function accentFor(id) {
        for (let i = 0; i < moduleAccents.length; ++i)
            if (id.indexOf(moduleAccents[i].prefix) === 0)
                return moduleAccents[i].accent
        return theme.neutral
    }
    // A modulation source's accent: envelopes amber, the rest violet.
    function sourceAccent(id) {
        return id.indexOf("env.") === 0 ? theme.am : theme.vi
    }
    function isSource(key) {
        return key.indexOf("env.") === 0 || key.indexOf("lfo.") === 0
    }

    // ---- The overview, the strip, the browser and the constellation ----------------------------

    // The strip module `key`, or a placeholder.
    function spineModule(key) {
        for (let i = 0; i < spineModules.length; ++i)
            if (spineModules[i].key === key)
                return spineModules[i]
        return { key: key, title: key.toUpperCase(), page: mainPage, accent: theme.neutral }
    }
    // Opens the card for `key` large over the overview, or restores it.
    function expand(key) {
        if (expandableModules.indexOf(key) < 0)
            return
        focusModule = focusModule === key ? "" : key
    }
    function restore() {
        focusModule = ""
    }
    // Shows MAIN, scrolled to the card for `key`, and flashes it.
    function revealModule(key) {
        if (focusModule !== "" && focusModule !== key)
            restore()
        showPage(mainPage)
        flashKey = key
        flashSerial += 1
    }
    // A source tab's click: an envelope or LFO is edited on the MODULATION
    // card; "macros" shows the MACROS card.
    function selectSource(key) {
        if (isSource(key))
            modTab = key
        else if (key === "macros")
            revealModule(key)
    }
    // A strip item's click: a sound module's card on MAIN, or FX, VOICES
    // and OUT's page.
    function openModule(key) {
        const module = spineModule(key)
        if (module.page === mainPage)
            revealModule(key)
        else
            showPage(module.page)
    }
    function showPage(index) {
        constellationOpen = false
        page = index
    }
    function toggleBrowser() {
        if (!presetBrowser.opened)
            constellationOpen = false
        presetBrowser.toggle()
    }
    function openBrowser() {
        constellationOpen = false
        presetBrowser.open()
    }
    function toggleConstellation() {
        constellationOpen = !constellationOpen
        if (constellationOpen && presetBrowser.opened)
            presetBrowser.close()
    }

    // The table an oscillator plays (its media slot, or the default).
    function oscTable(key) {
        const media = doc.media_revision >= 0 ? doc.media("osc." + key) : ({})
        return media.kind === "builtin" && media.name !== "" ? media.name : "basic_shapes"
    }
    function filterNumber(key) {
        return key === "filter1" ? 1 : 2
    }
    function filterCutoff(n) {
        return live ? at(Contract.T.cutoff + n - 1) : controller.value("nova.filter." + n + ".cutoff")
    }
    function filterResonance(n) {
        return live ? at(Contract.T.resonance + n - 1) : controller.value("nova.filter." + n + ".resonance")
    }
    function fxModules() {
        return doc.chains_revision >= 0 ? doc.chain("fx.main") : []
    }
    // The output's louder side, dBFS, while a synth sounds.
    function outputDb() {
        const out = Contract.T.meters + 2 * Contract.fxChains.length
        return Math.max(at(out), at(out + 1))
    }
    // Whether a module is on (its strip light; its card fades while off).
    function moduleOn(key) {
        if (controller.revision < 0)
            return false
        switch (key) {
        case "a":
        case "b":
        case "c":
            return controller.choice("nova.osc." + key + ".on") === 1
        case "subnoise":
            return controller.choice("nova.sub.on") === 1 || controller.choice("nova.noise.on") === 1
        case "filter1":
        case "filter2":
            return controller.choice("nova.filter." + filterNumber(key) + ".on") === 1
        case "fx": {
            const modules = fxModules()
            for (let i = 0; i < modules.length; ++i)
                if (controller.value("nova.fx." + modules[i].uid + ".on") >= 0.5)
                    return true
            return false
        }
        case "voices":
            return controller.choice("nova.analogue.on") === 1
        default:
            return true
        }
    }
    // A module's one-line summary: what an oscillator plays, the parts of
    // SUB · NOISE, a filter's model and cutoff, the FX chain, the voices,
    // the output's level.
    function moduleSub(key) {
        if (controller.revision < 0)
            return ""
        switch (key) {
        case "a":
        case "b":
        case "c": {
            const base = "nova.osc." + key
            const type = controller.choice(base + ".type")
            // Wavetable (1) and granular (4) play a table; classic (0) a shape.
            if (type === 1 || type === 4)
                return Contract.mediaName(Contract.tables, oscTable(key), "Basic Shapes")
            if (type === 0)
                return controller.display(base + ".shape")
            return controller.display(base + ".type")
        }
        case "subnoise": {
            const parts = []
            if (controller.choice("nova.sub.on") === 1)
                parts.push("Sub " + controller.display("nova.sub.octave"))
            if (controller.choice("nova.noise.on") === 1) {
                const am = controller.choice("nova.noise.am.source")
                parts.push("Noise" + (am > 0 ? " AM ← " + Contract.noiseAmLabels[am] : ""))
            }
            return parts.join(" · ")
        }
        case "filter1":
        case "filter2": {
            const n = filterNumber(key)
            return controller.display("nova.filter." + n + ".model") + " · " + Contract.hz(filterCutoff(n))
        }
        case "fx": {
            const modules = fxModules()
            const names = []
            for (let i = 0; i < modules.length; ++i)
                names.push(Contract.module(modules[i].module).name)
            return names.length > 0 ? names.join(" → ") : "Empty"
        }
        case "voices":
            return controller.choice("nova.analogue.on") === 1
                ? "Analogue · age " + Format.number(controller.normalised("nova.analogue.age"))
                : Math.round(controller.value("nova.global.polyphony")) + " voices"
        default:
            return live ? Contract.db(outputDb()) : "Master " + controller.display("nova.master")
        }
    }
    // A strip item's thumbnail: the oscillator's cycle (live when a synth
    // sounds), the filter's response at its modulated cutoff, the output's
    // level, or a sketch of the module. Its proportions, as fractions of the
    // thumbnail's height: a wave's reach either side of the middle; a
    // filter's 0 dB line from the top, its plotted dB range and the height
    // the range spans; the FX sketch's drive, cycles, level and reach; the
    // voices sketch's voices, detune per voice and reach; the output's two
    // bars' tops and height. The SUB · NOISE sketch is a few beating sines
    // under a swell. Below thumbnailSilence the cycle is drawn from settings.
    readonly property int thumbnailTraceStep: 2
    readonly property real thumbnailSilence: 1e-4
    readonly property real thumbnailWaveReach: 0.36
    readonly property real thumbnailFilterZero: 0.4
    readonly property real thumbnailFilterFloorDb: -40
    readonly property real thumbnailFilterCeilingDb: 24
    readonly property real thumbnailFilterRangeDb: 36
    readonly property real thumbnailFilterReach: 0.6
    readonly property real thumbnailFxDrive: 3
    readonly property real thumbnailFxCycles: 4
    readonly property real thumbnailFxLevel: 0.8
    readonly property real thumbnailFxReach: 0.4
    readonly property int thumbnailVoices: 4
    readonly property real thumbnailVoiceDetune: 0.03
    readonly property real thumbnailVoiceReach: 0.32
    readonly property real thumbnailMeterFloorDb: -60
    readonly property real thumbnailMeterRangeDb: 66
    readonly property real thumbnailMeterTop: 0.2
    readonly property real thumbnailMeterStride: 0.38
    readonly property real thumbnailMeterHeight: 0.22
    readonly property var thumbnailNoise: ({ fast: 90.1, mid: 47.3, midPhase: 1, slow: 13,
                                             swell: 1.5, floor: 0.4, depth: 0.6 })
    function paintThumbnail(ctx, key, W, H, colour) {
        ctx.reset()
        if (controller.revision < 0 || W <= 0 || H <= 0)
            return
        ctx.strokeStyle = colour
        ctx.fillStyle = colour
        ctx.lineWidth = theme.stripThumbnailStroke
        const trace = f => {
            ctx.beginPath()
            for (let i = 0; i <= W; i += thumbnailTraceStep) {
                const y = f(i / W)
                if (i === 0)
                    ctx.moveTo(i, y)
                else
                    ctx.lineTo(i, y)
            }
            ctx.stroke()
        }
        const wave = sample => trace(x => H / 2 - sample((x * 2) % 1) * H * thumbnailWaveReach)
        switch (key) {
        case "a":
        case "b":
        case "c": {
            const index = Contract.oscKeys.indexOf(key)
            const base = "nova.osc." + key
            const type = controller.choice(base + ".type")
            const from = Contract.T.frames + index * Contract.T.framePoints
            let energy = 0
            if (live)
                for (let i = 0; i < Contract.T.framePoints; ++i)
                    energy += Math.abs(at(from + i))
            if (energy > thumbnailSilence) {
                wave(x => at(from + Math.floor(x * Contract.T.framePoints) % Contract.T.framePoints))
            } else if (type === 1) {
                const position = live ? at(Contract.T.position + index) : controller.value(base + ".position")
                const table = oscTable(key)
                wave(x => Contract.tableSample(table, position, x))
            } else if (type === 0) {
                const shape = controller.choice(base + ".shape")
                const pw = controller.value(base + ".pw")
                wave(x => shape === 0 ? Math.sin(x * Math.PI * 2) : shape === 1 ? 1 - 4 * Math.abs(x - 0.5)
                         : shape === 2 ? 1 - 2 * x : shape === 3 ? (x < 0.5 ? 1 : -1) : (x < pw ? 1 : -1))
            } else {
                wave(x => Math.sin(x * Math.PI * 2))
            }
            return
        }
        case "subnoise":
            const n = thumbnailNoise
            wave(x => (Math.sin(x * n.fast) * Math.sin(x * n.mid + n.midPhase) + Math.sin(x * n.slow)) * 0.5
                      * (n.floor + n.depth * Math.abs(1 - 2 * ((x * n.swell + 0.5) % 1))))
            return
        case "filter1":
        case "filter2": {
            const n = filterNumber(key)
            const model = controller.choice("nova.filter." + n + ".model")
            const fc = filterCutoff(n)
            const res = filterResonance(n)
            trace(x => H * thumbnailFilterZero
                       - Math.max(thumbnailFilterFloorDb, Math.min(thumbnailFilterCeilingDb,
                                                                  Contract.filterDb(model, Contract.displayHz(x), fc, res)))
                       / thumbnailFilterRangeDb * H * thumbnailFilterReach)
            return
        }
        case "fx":
            trace(x => H / 2 - Math.tanh(thumbnailFxDrive * Math.sin(x * Math.PI * thumbnailFxCycles))
                       * thumbnailFxLevel * H * thumbnailFxReach)
            return
        case "voices": {
            const spread = controller.choice("nova.analogue.on") === 1 ? controller.normalised("nova.analogue.age") : 0
            for (let v = 0; v < thumbnailVoices; ++v) {
                ctx.globalAlpha = theme.stripThumbnailVoiceAlpha + theme.stripThumbnailVoiceAlphaStep * v
                trace(x => H / 2 - (1 - 2 * ((x * 2 + v * thumbnailVoiceDetune * spread) % 1)) * H * thumbnailVoiceReach)
            }
            ctx.globalAlpha = 1
            return
        }
        default: {
            // The output: one bar per side, -60..+6 dB.
            const out = Contract.T.meters + 2 * Contract.fxChains.length
            const inset = theme.stripThumbnailInset
            for (let side = 0; side < 2; ++side) {
                const fill = live ? Math.max(0, Math.min(1, (at(out + side) - thumbnailMeterFloorDb)
                                                             / thumbnailMeterRangeDb)) : 0
                const top = H * (thumbnailMeterTop + thumbnailMeterStride * side)
                ctx.globalAlpha = theme.stripThumbnailMeterTrackAlpha
                ctx.fillRect(inset, top, W - 2 * inset, H * thumbnailMeterHeight)
                ctx.globalAlpha = theme.stripThumbnailMeterAlpha
                ctx.fillRect(inset, top, (W - 2 * inset) * fill, H * thumbnailMeterHeight)
            }
            ctx.globalAlpha = 1
        }
        }
    }

    // ---- Plate -------------------------------------------------------------------------

    // A radial light from the top edge, a fifth of the way across, reaching
    // a little past the plate's longer side.
    Canvas {
        readonly property string plotExemptReason: "the plate's lighting behind the panel, not a plot"
        readonly property real lightX: 0.2
        readonly property real lightReach: 1.15
        readonly property real midStop: 0.55
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const g = ctx.createRadialGradient(width * lightX, 0, 0, width * lightX, 0, Math.max(width, height) * lightReach)
            g.addColorStop(0, panel.theme.plateTop)
            g.addColorStop(midStop, panel.theme.plateMid)
            g.addColorStop(1, panel.theme.plateBottom)
            ctx.fillStyle = g
            ctx.fillRect(0, 0, width, height)
        }
    }

    // A header button: BROWSE and ✦ CONSTELLATION. Lit while its view is
    // open: neutral white, or for `star` the brand's cyan-violet-pink sweep.
    component HeaderButton: Rectangle {
        id: button
        property string text: ""
        property bool on: false
        property bool star: false
        signal clicked()
        width: buttonLabel.implicitWidth + panel.theme.headerButtonPadding
        height: panel.theme.headerControlHeight
        radius: panel.theme.headerControlRadius
        color: on && !star ? panel.theme.neutral : buttonArea.containsMouse ? panel.theme.hover : "transparent"
        border.color: activeFocus ? panel.theme.accent : on ? (star ? "transparent" : panel.theme.neutral) : panel.theme.line2
        border.width: activeFocus ? panel.theme.strokeBold : panel.theme.hairline
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.checked: on
        Keys.onSpacePressed: button.clicked()
        Keys.onReturnPressed: button.clicked()
        // The star's lit fill: a cyan-violet-pink sweep.
        Rectangle {
            visible: button.on && button.star
            anchors.fill: parent
            radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: panel.theme.cy }
                GradientStop { position: 0.5; color: panel.theme.vi }
                GradientStop { position: 1; color: panel.theme.pk }
            }
        }
        Text {
            id: buttonLabel
            anchors.centerIn: parent
            text: button.text
            color: button.on ? panel.theme.bg : buttonArea.containsMouse ? panel.theme.text : panel.theme.dim
            font.family: panel.theme.mono
            font.pixelSize: panel.theme.typeSmall
            font.weight: Font.DemiBold
            font.letterSpacing: panel.theme.headerLabelSpacing
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
    }

    // ---- Header (NOVA-UI-01, Kit.DeviceHeader) ----------------------------------------------

    // Over the plate's lit top edge: the NOVA wordmark, the preset browser
    // (its list is the browser drawer), the readout and the status badge;
    // under them the views (BROWSE, ✦ CONSTELLATION, the pages) on the left
    // and the tools (voices and CPU, the keys pill) on the right.
    Kit.DeviceHeader {
        id: header
        x: panel.headerMargin
        y: panel.theme.headerY
        width: panel.width - 2 * panel.headerMargin
        controller: panel.controller
        theme: panel.theme
        brand: Component {
            // The wordmark: wide-tracked light capitals in a cyan-to-violet light.
            Canvas {
                objectName: "novaWordmark"
                readonly property real tracking: panel.theme.wordmarkTracking
                readonly property real fontSize: panel.theme.wordmarkSize
                width: panel.theme.wordmarkWidth
                height: panel.theme.wordmarkHeight
                Accessible.role: Accessible.StaticText
                Accessible.name: "Nova"
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const g = ctx.createLinearGradient(0, 0, width, 0)
                    g.addColorStop(0, panel.theme.cyLight)
                    g.addColorStop(panel.theme.wordmarkMidStop, panel.theme.cy)
                    g.addColorStop(1, panel.theme.vi)
                    ctx.fillStyle = g
                    ctx.font = "300 " + fontSize + "px \"" + panel.theme.sans + "\""
                    ctx.textBaseline = "alphabetic"
                    let x = 0
                    const letters = "NOVA"
                    for (let i = 0; i < letters.length; ++i) {
                        ctx.fillText(letters[i], x, height - panel.theme.wordmarkDescent)
                        x += ctx.measureText(letters[i]).width + tracking
                    }
                }
            }
        }
        // The name opens the browser drawer; with a filter set the arrows
        // walk what it shows.
        opener: function(display) { panel.openBrowser() }
        stepper: function(direction) { panel.stepPreset(direction) }
        readoutNameOf: function(id) { return panel.touchedName(id) }
        readoutValueOf: function(id) { return panel.touchedValue(id) }

        // The views and tools, one row under the first.
        Item {
            width: header.width
            height: panel.theme.headerControlHeight

            Row {
                id: views
                height: parent.height
                spacing: panel.theme.headerSpacing
                HeaderButton {
                    id: browseButton
                    objectName: "novaPresetBrowse"
                    anchors.verticalCenter: parent.verticalCenter
                    text: "BROWSE"
                    on: presetBrowser.opened
                    enabled: panel.controller.editable
                    onClicked: panel.toggleBrowser()
                }
                HeaderButton {
                    id: constellationButton
                    objectName: "novaConstellationButton"
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.compactHeader ? "✦" : "✦ CONSTELLATION"
                    star: true
                    Accessible.name: "Constellation"
                    on: panel.constellationOpen
                    onClicked: panel.toggleConstellation()
                }

                // The pages: MAIN (the overview), FX, MATRIX, ARP · CLIP, VOICES
                // and GLOBAL, each named for panel.page's number.
                Rectangle {
                    id: pageTabs
                    objectName: "novaPages"
                    readonly property real inset: panel.theme.spaceXxs
                    readonly property real tabPadding: panel.theme.pageTabPadding
                    readonly property real tabRadius: panel.theme.pageTabRadius
                    readonly property real labelSize: panel.theme.pageTabLabelSize
                    readonly property real labelSpacing: panel.theme.headerLabelSpacing
                    anchors.verticalCenter: parent.verticalCenter
                    width: pageRow.implicitWidth + 2 * inset
                    height: panel.theme.headerControlHeight
                    radius: panel.theme.headerControlRadius
                    color: panel.theme.wellFill
                    border.color: panel.theme.line
                    Row {
                        id: pageRow
                        x: pageTabs.inset
                        y: pageTabs.inset
                        height: parent.height - 2 * pageTabs.inset
                        spacing: pageTabs.inset
                        Repeater {
                            model: panel.pageButtons
                            delegate: Rectangle {
                                id: tab
                                required property var modelData
                                // The page the button shows.
                                readonly property int target: modelData.page
                                readonly property bool on: panel.page === target
                                objectName: "novaTab:" + target
                                width: tabLabel.implicitWidth + 2 * pageTabs.tabPadding
                                height: parent.height
                                radius: pageTabs.tabRadius
                                color: on ? panel.theme.card2 : tabArea.containsMouse ? panel.theme.hover : "transparent"
                                border.color: activeFocus ? panel.theme.accent : "transparent"
                                border.width: activeFocus ? panel.theme.strokeBold : 0
                                activeFocusOnTab: true
                                Accessible.role: Accessible.PageTab
                                Accessible.name: modelData.label
                                Accessible.checked: on
                                Keys.onSpacePressed: panel.showPage(target)
                                Keys.onReturnPressed: panel.showPage(target)
                                Text {
                                    id: tabLabel
                                    anchors.centerIn: parent
                                    text: tab.modelData.label
                                    color: tab.on ? panel.theme.text : panel.theme.dim
                                    font.family: panel.theme.mono
                                    font.pixelSize: pageTabs.labelSize
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: pageTabs.labelSpacing
                                }
                                MouseArea {
                                    id: tabArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: panel.showPage(tab.target)
                                }
                            }
                        }
                    }
                }
            }

            Row {
                id: tools
                anchors.right: parent.right
                height: parent.height
                spacing: panel.theme.headerSpacing

                // Voices in use against the polyphony, and the engine's CPU.
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: panel.theme.spaceSm
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "VOICES"
                        color: panel.theme.faint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeCaption
                        font.letterSpacing: panel.theme.headerLabelSpacing
                    }
                    Text {
                        objectName: "novaVoices"
                        anchors.verticalCenter: parent.verticalCenter
                        text: (panel.live ? Math.round(panel.at(Contract.T.voices)) : 0) + " / "
                            + (panel.controller.revision >= 0 ? Math.round(panel.controller.value("nova.global.polyphony")) : 0)
                        color: panel.theme.text
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                    Item {
                        width: panel.theme.headerMeterGap
                        height: parent.height
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "CPU"
                        color: panel.theme.faint
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeCaption
                        font.letterSpacing: panel.theme.headerLabelSpacing
                    }
                    Text {
                        objectName: "novaCpu"
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.live ? Format.fraction(panel.at(Contract.T.cpu)) : "— %"
                        color: panel.live && panel.at(Contract.T.cpu) > panel.cpuWarning ? panel.theme.red : panel.theme.text
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }

                // Whether the typing keyboard plays notes: green while it does,
                // amber while a browser field has the keys. Compact, the light alone.
                Rectangle {
                    id: keysPill
                    objectName: "novaKeysPill"
                    readonly property bool typing: presetBrowser.typing || panel.arpSearching
                    readonly property color tone: typing ? panel.theme.am : panel.theme.gr
                    anchors.verticalCenter: parent.verticalCenter
                    width: keysRow.implicitWidth + panel.theme.keysPillPadding
                    height: panel.theme.headerChipHeight
                    radius: height / 2
                    color: panel.theme.soft(tone, panel.theme.keysPillFillAlpha)
                    border.color: panel.theme.soft(tone, panel.theme.keysPillEdgeAlpha)
                    Accessible.role: Accessible.StaticText
                    Accessible.name: keysText.text
                    Row {
                        id: keysRow
                        anchors.centerIn: parent
                        spacing: panel.theme.spaceSm
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: panel.theme.keysLedSize
                            height: panel.theme.keysLedSize
                            radius: width / 2
                            color: keysPill.tone
                        }
                        Text {
                            id: keysText
                            objectName: "novaKeysText"
                            visible: !panel.compactHeader
                            anchors.verticalCenter: parent.verticalCenter
                            text: !keysPill.typing ? "KEYS PLAY NOTES"
                                : presetBrowser.searching || panel.arpSearching ? "TYPING IN SEARCH · ESC TO PLAY"
                                : "TYPING A NAME · ESC TO PLAY"
                            color: keysPill.tone
                            font.family: panel.theme.mono
                            font.pixelSize: panel.theme.typeCaption
                            font.weight: Font.DemiBold
                            font.letterSpacing: panel.theme.keysLabelSpacing
                        }
                    }
                }
            }
        }
    }
    // The preset tag in Nova's words: the category, a user preset or an
    // unnamed patch, and EDITED once the preset has changed.
    Binding {
        target: header.browser
        when: header.browser !== null
        property: "tag"
        value: (panel.controller.preset_category === "User" ? "USER"
                : panel.controller.preset_index < 0 ? "PATCH"
                : panel.controller.preset_category).toUpperCase()
            + (panel.controller.preset_modified ? " · EDITED" : "")
    }
    // The header's foot.
    Rectangle {
        y: panel.headerHeight - height
        width: panel.width
        height: panel.theme.hairline
        color: panel.theme.line
    }

    // ---- The signal strip (ADR-0076 revision 2) ----------------------------------------------

    // One thin row: per module a light, its name and a small live picture,
    // with › between them. A sound module jumps to its card on MAIN; FX,
    // VOICES and OUT open their pages. An off module's light is grey and
    // its picture faint. Wider than the panel, the row scrolls sideways.
    Rectangle {
        id: spine
        objectName: "novaSpine"
        readonly property real margin: panel.theme.spaceLg
        readonly property real itemPadding: panel.theme.spaceSm
        readonly property real itemSpacing: panel.theme.spaceSm
        readonly property real itemRadius: panel.theme.radiusMd
        readonly property real itemHeight: panel.theme.stripItemHeight
        readonly property real separatorWidth: panel.theme.stripSeparatorWidth
        readonly property real separatorSize: panel.theme.typeBody
        readonly property real ledSize: panel.theme.stripLedSize
        readonly property real titleSize: panel.theme.typeMicro
        readonly property real titleSpacing: panel.theme.stripTitleSpacing
        readonly property real thumbnailWidth: panel.theme.stripThumbnailWidth
        readonly property real thumbnailHeight: panel.theme.stripThumbnailHeight
        readonly property real offAlpha: panel.theme.stripOffOpacity
        y: panel.headerHeight
        width: parent.width
        height: panel.stripHeight
        color: panel.theme.rail

        Rectangle {
            y: parent.height - height
            width: parent.width
            height: panel.theme.hairline
            color: panel.theme.line
        }

        Flickable {
            id: stripScroller
            x: spine.margin
            width: spine.width - 2 * spine.margin
            height: spine.height
            contentWidth: stripRow.implicitWidth
            contentHeight: height
            interactive: contentWidth > width
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            clip: true
            Row {
                id: stripRow
                height: stripScroller.height
                Repeater {
                    model: panel.spineModules
                    delegate: Row {
                        id: slot
                        required property var modelData
                        required property int index
                        height: parent.height
                        Text {
                            visible: slot.index > 0
                            width: spine.separatorWidth
                            height: parent.height
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: "›"
                            color: panel.theme.faint
                            font.family: panel.theme.sans
                            font.pixelSize: spine.separatorSize
                        }
                        Rectangle {
                            id: stripItem
                            readonly property string key: slot.modelData.key
                            readonly property color accent: slot.modelData.accent
                            readonly property bool on: panel.moduleOn(key)
                            // FX, VOICES and OUT light while their page shows.
                            readonly property bool selected: slot.modelData.page !== panel.mainPage
                                && panel.page === slot.modelData.page
                            // Repaint the thumbnail when what it draws changes;
                            // live items follow the telemetry frame.
                            readonly property bool follows: ["a", "b", "c", "filter1", "filter2", "out"].indexOf(key) >= 0
                            readonly property var watched: [panel.controller.revision, panel.doc.media_revision,
                                                            panel.doc.chains_revision, follows && panel.live ? panel.frame : null]
                            objectName: "novaSpine:" + key
                            anchors.verticalCenter: parent.verticalCenter
                            width: itemRow.implicitWidth + 2 * spine.itemPadding
                            height: spine.itemHeight
                            radius: spine.itemRadius
                            color: selected || itemArea.containsMouse ? panel.theme.card : "transparent"
                            border.color: activeFocus ? accent : "transparent"
                            border.width: activeFocus ? panel.theme.strokeBold : 0
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: slot.modelData.title + (on ? "" : ", off")
                            Accessible.checked: selected
                            Keys.onSpacePressed: panel.openModule(key)
                            Keys.onReturnPressed: panel.openModule(key)
                            onWatchedChanged: thumbnail.requestPaint()
                            Row {
                                id: itemRow
                                x: spine.itemPadding
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: spine.itemSpacing
                                // On or off: lit in the module's accent, grey while off.
                                Rectangle {
                                    objectName: "novaSpineLed:" + stripItem.key
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: spine.ledSize
                                    height: spine.ledSize
                                    radius: spine.ledSize / 2
                                    color: stripItem.on ? stripItem.accent : panel.theme.faint
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: slot.modelData.title
                                    color: stripItem.selected || itemArea.containsMouse ? panel.theme.text
                                        : stripItem.on ? panel.theme.dim : panel.theme.faint
                                    font.family: panel.theme.mono
                                    font.pixelSize: spine.titleSize
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: spine.titleSpacing
                                }
                                Canvas {
                                    id: thumbnail
                                    objectName: "novaSpineThumbnail:" + stripItem.key
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: spine.thumbnailWidth
                                    height: spine.thumbnailHeight
                                    opacity: stripItem.on ? 1 : spine.offAlpha
                                    renderStrategy: Canvas.Cooperative
                                    onPaint: panel.paintThumbnail(getContext("2d"), stripItem.key, width, height,
                                                                  stripItem.accent)
                                }
                            }
                            MouseArea {
                                id: itemArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: panel.openModule(stripItem.key)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- Pages -----------------------------------------------------------------------------

    // A page fills the area beside the browser and under the strip, laid out
    // at that size; one that needs more (its minimumWidth and minimumHeight)
    // is laid out at its minimum and scaled down to fit.
    component Page: Item {
        id: pageHost
        required property int index
        property Component pageComponent: null
        property bool loaded: false
        readonly property real fit: loader.item !== null
            ? Math.min(1, width / loader.item.minimumWidth, height / loader.item.minimumHeight) : 1
        x: panel.pageLeft
        y: panel.pageTop
        width: panel.width - x - panel.pageMargin
        height: panel.bodyBottom - y - panel.pageMargin
        visible: panel.page === index
        Loader {
            id: loader
            active: pageHost.visible || pageHost.loaded
            sourceComponent: pageHost.pageComponent
            width: pageHost.width / pageHost.fit
            height: pageHost.height / pageHost.fit
            scale: pageHost.fit
            transformOrigin: Item.TopLeft
            onLoaded: pageHost.loaded = true
        }
    }

    Page {
        index: 0
        objectName: "novaPage:0"
        pageComponent: NovaOverviewPage { panel: panel }
    }
    Page {
        index: 1
        objectName: "novaPage:1"
        pageComponent: NovaFxPage { panel: panel }
    }
    Page {
        index: 2
        objectName: "novaPage:2"
        pageComponent: NovaMatrixPage { panel: panel }
    }
    Page {
        index: 3
        objectName: "novaPage:3"
        pageComponent: NovaArpPage { panel: panel }
    }
    Page {
        index: 4
        objectName: "novaPage:4"
        pageComponent: NovaGlobalPage { panel: panel }
    }
    Page {
        index: 5
        objectName: "novaPage:5"
        pageComponent: NovaVoicesPage { panel: panel }
    }

    // The browser, docked at the left of the page area.
    NovaPresetBrowser {
        id: presetBrowser
        x: 0
        y: panel.bodyTop
        width: panel.browserWidth
        height: panel.bodyBottom - y
        z: 10
        panel: panel
    }

    // ✦ CONSTELLATION (ADR-0077): over the browser and the pages while open.
    Loader {
        id: constellationHost
        objectName: "novaConstellationHost"
        x: 0
        y: panel.bodyTop
        width: panel.width
        height: panel.bodyBottom - y
        active: panel.constellationOpen && panel.controller.constellation !== null
        visible: active
        z: 20
        sourceComponent: NovaConstellationView { panel: panel }
    }

    // ---- The keys ------------------------------------------------------------------------------

    NovaKeyStrip {
        id: keyStrip
        panel: panel
        base: panel.keyBase
        y: panel.height - height
        width: panel.width
        height: panel.keyStripHeight
        onKeyPressed: (note, velocity) => panel.keyPressed(note, velocity)
        onKeyReleased: note => panel.keyReleased(note)
    }

    // ---- Overlays --------------------------------------------------------------------------

    Loader {
        objectName: "novaEditorLoader"
        active: panel.editorOsc !== ""
        anchors.fill: parent
        z: 30
        sourceComponent: NovaTableEditor {
            panel: panel
            osc: panel.editorOsc
        }
    }

    // The route popover over a hovered, modulated knob (NOVA-UI-05).
    Rectangle {
        id: popover
        objectName: "novaRoutePopover"
        readonly property var knob: panel.hoverKnob
        readonly property var routes: knob !== null ? knob.routes : []
        readonly property point anchorPoint: knob !== null
            ? knob.mapToItem(panel, knob.width + panel.theme.spaceSm, 0) : Qt.point(0, 0)
        visible: knob !== null && routes.length > 0 && panel.dragSource === "" && !menu.opened
        z: 40
        x: Math.min(anchorPoint.x, panel.width - width - panel.theme.spaceMd)
        y: Math.min(anchorPoint.y, panel.height - height - panel.theme.spaceMd)
        width: panel.theme.routePopoverWidth
        height: popColumn.implicitHeight + 2 * panel.theme.spaceMd
        radius: panel.theme.routePopoverRadius
        color: panel.theme.popFill
        border.color: panel.theme.line2
        Column {
            id: popColumn
            x: panel.theme.routePopoverPadding
            y: panel.theme.spaceMd
            width: parent.width - 2 * panel.theme.routePopoverPadding
            spacing: panel.theme.routePopoverSpacing
            Text {
                text: (popover.knob !== null ? popover.knob.label : "") + " · MODULATED BY"
                color: panel.theme.faint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeCaption
                font.weight: Font.ExtraBold
                font.letterSpacing: panel.theme.routePopoverHeadingSpacing
            }
            Repeater {
                model: popover.routes
                delegate: Row {
                    id: popRow
                    required property var modelData
                    required property int index
                    readonly property real amount: popover.knob !== null && index < popover.knob.amounts.length
                        ? popover.knob.amounts[index] : 0
                    spacing: panel.theme.spaceMd
                    height: panel.theme.routePopoverRowHeight
                    Text {
                        width: panel.theme.routePopoverSourceWidth
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.sourceName(popRow.modelData.source)
                        color: panel.theme.vi
                        font.family: panel.theme.sans
                        font.pixelSize: panel.theme.typeBody
                        font.weight: Font.Bold
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: panel.theme.routePopoverBarWidth
                        height: panel.theme.routePopoverBarHeight
                        radius: panel.theme.radiusXs
                        color: panel.theme.line
                        Rectangle {
                            width: parent.width * Math.min(1, Math.abs(popRow.amount))
                            height: parent.height
                            radius: panel.theme.radiusXs
                            color: panel.theme.vi
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Format.fraction(popRow.amount)
                        color: panel.theme.text
                        font.family: panel.theme.mono
                        font.pixelSize: panel.theme.typeSmall
                    }
                }
            }
            Text {
                text: "Drag the ring for depth · right-click to remove"
                color: panel.theme.faint
                font.family: panel.theme.sans
                font.pixelSize: panel.theme.typeSmall
            }
        }
    }

    // The drag ghost while a source is carried to a knob.
    Rectangle {
        objectName: "novaDragGhost"
        visible: panel.dragSource !== ""
        z: 60
        x: panel.dragX + panel.theme.dragGhostOffsetX
        y: panel.dragY + panel.theme.dragGhostOffsetY
        width: ghostText.implicitWidth + panel.theme.dragGhostPadding
        height: panel.theme.dragGhostHeight
        radius: panel.theme.radiusLg
        color: panel.theme.vi
        Text {
            id: ghostText
            anchors.centerIn: parent
            text: "✥ " + panel.sourceName(panel.dragSource)
                + (panel.dropKnob !== null ? "  →  " + panel.dropKnob.label : "")
            color: panel.theme.viInk
            font.family: panel.theme.sans
            font.pixelSize: panel.theme.typeSmall
            font.weight: Font.ExtraBold
        }
    }

    Kit.Menu {
        id: menu
        anchors.fill: parent
        theme: panel.theme
    }

    // Telemetry: about 60 Hz while a synth sounds, slower while idle.
    Timer {
        objectName: "novaTelemetryPoll"
        interval: panel.live ? panel.livePollInterval : panel.idlePollInterval
        repeat: true
        running: panel.visible
        onTriggered: {
            const state = panel.controller.telemetry()
            panel.frame = state.frame !== undefined ? state.frame : []
        }
    }
}
