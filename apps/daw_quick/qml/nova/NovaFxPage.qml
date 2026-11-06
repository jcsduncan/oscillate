pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// The FX page (NOVA-UI-11): the rack on the left reads top to bottom in
// the order the sound passes, Main or a bus at a time. Drag a row to
// reorder it (one chain edit), drop it on another bus's tab to move it
// there (Ctrl copies), click it to jump to its module. Every module of the
// chain is a full-width strip on the right, with its live display and all
// of its controls; a strip collapses to a one-line summary. Selection,
// collapse, the last preset applied and the settings clipboard are the
// panel's own state, kept while the panel lives.
Item {
    id: page
    required property var panel
    readonly property var controller: panel.controller
    readonly property var doc: panel.doc
    readonly property var chainIds: Contract.fxChains.map(c => c.id)
    readonly property int bus: Math.max(0, chainIds.indexOf(panel.fxChain))
    readonly property var modules: doc.chains_revision >= 0 ? doc.chain(panel.fxChain) : []
    readonly property bool busFull: modules.length >= Contract.fxChainSlots
    readonly property bool splitOn: controller.revision >= 0 && controller.choice("nova.fx.split") === 1

    // Layout.
    readonly property real rackWidth: 284
    readonly property real gap: panel.theme.spaceLg
    readonly property real rowHeight: 46
    readonly property real wireHeight: 8
    readonly property real insertHeight: 3
    // The narrowest a module strip lays out its controls, and the shortest
    // the rack holds a full chain; below these the page is scaled down to
    // fit (NovaPanel's Page). The strips take any width and scroll.
    readonly property real stripMinimum: 720
    readonly property real minimumWidth: rackWidth + gap + stripMinimum
    readonly property real minimumHeight: 560
    // Pixels a row must travel before a press becomes a drag.
    readonly property real dragThreshold: 4
    readonly property int noteMs: 1800
    // A popover's gap from the control that opened it.
    readonly property real popoverGap: panel.theme.spaceSm
    // Fixed sizes in the rack: a bus tab, the routing box (Main, a bus),
    // + Add effect and the split row while on; the empty note's top.
    readonly property real busTabHeight: 42
    readonly property real routeHeight: 46
    readonly property real busRouteHeight: 92
    readonly property real addHeight: 38
    readonly property real splitHeight: 64
    readonly property real meterFloorDb: -60
    readonly property real emptyTop: 120
    readonly property real promptWidth: 280
    readonly property real promptHeight: 78

    // The strip in focus (a slot uid), or -1.
    property int selected: modules.length > 0 ? modules[0].uid : -1
    // Slot uid -> collapsed; uid -> {module, name, values} of the preset last
    // applied. Replaced, never mutated, so bindings see them.
    property var collapsed: ({})
    property var presetState: ({})
    // Copied settings: {module, values} with values keyed p0..p15 and mix.
    property var clipboard: null
    // A row drag: its index, where it would land in this chain (0..count)
    // or the bus tab under it (-1 for none).
    property int dragFrom: -1
    property int dragInsert: -1
    property int dragBus: -1
    property string note: ""

    objectName: "novaFxPage"

    // ---- Slot values -------------------------------------------------------------------

    // The slot's values keyed p0..p15 and mix.
    function slotValues(uid) {
        const out = {}
        for (let k = 0; k < Contract.fxSlotParams; ++k)
            out["p" + k] = controller.value(Contract.fxParam(uid, k))
        out.mix = controller.value(Contract.fxParam(uid, "mix"))
        return out
    }
    // Does the slot hold exactly `over` on top of `module`'s defaults?
    function holds(uid, module, over) {
        const wanted = Contract.slotValues(uid, Contract.module(module), over)
        for (const id in wanted)
            if (Math.abs(controller.value(id) - wanted[id]) > 1e-6)
                return false
        return true
    }
    function rememberPreset(uid, module, name, over) {
        const next = Object.assign({}, presetState)
        next[uid] = { module: module, name: name, values: over }
        presetState = next
    }
    function forgetPreset(uid) {
        const next = Object.assign({}, presetState)
        delete next[uid]
        presetState = next
    }
    // The name a strip shows: the preset last applied while the values still
    // match it, "Default" at the defaults, else "Custom".
    function presetName(uid, module) {
        if (controller.revision < 0)
            return ""
        const state = presetState[uid]
        if (state !== undefined && state.module === module && holds(uid, module, state.values))
            return state.name
        return holds(uid, module, {}) ? "Default" : "Custom"
    }
    // Every preset a module offers, in stepping order: Default, the factory
    // presets, then the user's own. {name, values, user}.
    function presetList(module) {
        const spec = Contract.module(module)
        const list = [{ name: "Default", values: {}, user: false }]
        if (spec === null)
            return list
        for (let i = 0; i < spec.presets.length; ++i)
            list.push({ name: spec.presets[i].name, values: spec.presets[i].values, user: false })
        const users = controller.fx_presets_revision >= 0 ? controller.fx_presets(module) : []
        for (let i = 0; i < users.length; ++i)
            list.push({ name: users[i], values: null, user: true })
        return list
    }
    function applyPreset(uid, module, preset) {
        const values = preset.user ? controller.fx_preset(module, preset.name) : preset.values
        controller.set_values(Contract.slotValues(uid, Contract.module(module), values))
        rememberPreset(uid, module, preset.name, values)
    }
    function stepPreset(uid, module, direction) {
        const list = presetList(module)
        const current = presetName(uid, module)
        let at = -1
        for (let i = 0; i < list.length; ++i)
            if (list[i].name === current)
                at = i
        const next = at < 0 ? (direction > 0 ? 0 : list.length - 1)
                            : (at + direction + list.length) % list.length
        applyPreset(uid, module, list[next])
    }
    function openPresetMenu(anchor, uid, module) {
        const list = presetList(module)
        const items = [{ label: "FACTORY", value: "", header: true }]
        let users = false
        for (let i = 0; i < list.length; ++i) {
            if (list[i].user && !users) {
                items.push({ label: "YOUR PRESETS", value: "", header: true })
                users = true
            }
            items.push({ label: list[i].name, value: "p:" + i })
        }
        const current = presetName(uid, module)
        const own = list.filter(p => p.user && p.name === current)
        if (own.length > 0)
            items.push({ label: "Delete “" + current + "”", value: "delete" })
        items.push({ label: "Save as preset…", value: "save" })
        panel.menu.open(anchor, items, null, panel.theme.pk, value => {
            if (value === "delete") {
                page.controller.delete_fx_preset(module, current)
                page.forgetPreset(uid)
            } else if (value === "save") {
                page.askPresetName(anchor, uid, module)
            } else {
                page.applyPreset(uid, module, list[parseInt(value.substring(2))])
            }
        })
    }

    // ---- Chain edits -----------------------------------------------------------------------

    function indexOfUid(uid) {
        for (let i = 0; i < modules.length; ++i)
            if (modules[i].uid === uid)
                return i
        return -1
    }
    function select(uid) {
        selected = uid
        if (collapsed[uid] === true)
            setCollapsed(uid, false)
        stack.reveal(uid)
    }
    function setCollapsed(uid, value) {
        const next = Object.assign({}, collapsed)
        next[uid] = value
        collapsed = next
    }
    function showNote(text) {
        note = text
        noteTimer.restart()
    }
    function addModule(moduleId) {
        const uid = panel.addEffect(panel.fxChain, moduleId)
        if (uid < 0) {
            showNote("No free slot: remove an effect first")
            return
        }
        select(uid)
    }
    // Swaps the module at `index` for `moduleId`, keeping its uid.
    function swapModule(index, moduleId) {
        const slot = modules[index]
        if (slot === undefined || slot.module === moduleId)
            return
        controller.set_values(panel.moduleDefaults(slot.uid, moduleId))
        doc.set_chain_module(panel.fxChain, index, moduleId)
        forgetPreset(slot.uid)
    }
    function duplicate(index) {
        const slot = modules[index]
        if (busFull) {
            showNote(Contract.fxChains[bus].name + " is full")
            return
        }
        const uid = panel.addEffect(panel.fxChain, slot.module, index + 1, slotValues(slot.uid))
        if (uid >= 0)
            select(uid)
    }
    // Moves (or copies) the module at `index` to the end of chain `target`.
    function sendTo(index, target, copy) {
        const slot = modules[index]
        const chain = chainIds[target]
        if (slot === undefined || chain === panel.fxChain)
            return
        if (doc.chain(chain).length >= Contract.fxChainSlots) {
            showNote(Contract.fxChains[target].name + " is full")
            return
        }
        const name = Contract.module(slot.module).name
        if (copy) {
            if (panel.addEffect(chain, slot.module, undefined, slotValues(slot.uid)) < 0)
                showNote("No free slot: remove an effect first")
            else
                showNote("Copied " + name + " to " + Contract.fxChains[target].name)
            return
        }
        if (doc.move_chain_module_to(panel.fxChain, index, chain, Contract.fxChainSlots))
            showNote("Moved " + name + " to " + Contract.fxChains[target].name)
    }
    function resetSlot(index) {
        const slot = modules[index]
        controller.set_values(Contract.slotValues(slot.uid, Contract.module(slot.module), {}))
        forgetPreset(slot.uid)
    }
    function copySettings(index) {
        const slot = modules[index]
        clipboard = { module: slot.module, values: slotValues(slot.uid) }
        showNote("Copied " + Contract.module(slot.module).name + " settings")
    }
    function pasteSettings(index) {
        const slot = modules[index]
        if (clipboard === null) {
            showNote("Copy an effect's settings first")
            return
        }
        if (clipboard.module !== slot.module) {
            showNote("Paste needs " + Contract.module(slot.module).name + " settings; these are "
                     + Contract.module(clipboard.module).name + "'s")
            return
        }
        controller.set_values(Contract.slotValues(slot.uid, Contract.module(slot.module), clipboard.values))
        forgetPreset(slot.uid)
    }
    function openSlotMenu(anchor, index) {
        const slot = modules[index]
        if (slot === undefined)
            return
        const items = [{ label: "Duplicate", value: "duplicate" }]
        for (let b = 0; b < chainIds.length; ++b)
            if (b !== bus)
                items.push({ label: "Move to " + Contract.fxChains[b].name, value: "move:" + b })
        items.push({ label: "Copy settings", value: "copy" })
        items.push({ label: "Paste settings", value: "paste" })
        items.push({ label: "Save as preset…", value: "save" })
        items.push({ label: "Reset to default", value: "reset" })
        items.push({ label: "Remove", value: "remove" })
        panel.menu.open(anchor, items, null, panel.theme.pk, value => {
            if (value === "duplicate")
                page.duplicate(index)
            else if (value.indexOf("move:") === 0)
                page.sendTo(index, parseInt(value.substring(5)), false)
            else if (value === "copy")
                page.copySettings(index)
            else if (value === "paste")
                page.pasteSettings(index)
            else if (value === "save")
                page.askPresetName(anchor, slot.uid, slot.module)
            else if (value === "reset")
                page.resetSlot(index)
            else if (value === "remove")
                page.doc.remove_chain_module(page.panel.fxChain, index)
        })
    }
    function openBrowser(anchor, swapIndex) {
        const at = anchor.mapToItem(page, 0, anchor.height + popoverGap)
        browser.swapIndex = swapIndex
        browser.current = swapIndex >= 0 && modules[swapIndex] !== undefined ? modules[swapIndex].module : ""
        browser.x = Math.max(0, Math.min(at.x, page.width - browser.width))
        browser.y = at.y + browser.height > page.height ? Math.max(0, at.y - anchor.height - 2 * popoverGap - browser.height) : at.y
        browser.open()
    }
    function askPresetName(anchor, uid, module) {
        const at = anchor.mapToItem(page, 0, anchor.height + popoverGap)
        namePrompt.uid = uid
        namePrompt.module = module
        namePrompt.x = Math.max(0, Math.min(at.x, page.width - namePrompt.width))
        namePrompt.y = Math.min(at.y, page.height - namePrompt.height)
        namePrompt.open()
    }

    // ---- Row drags -------------------------------------------------------------------------

    function busTabAt(x, y) {
        for (let b = 0; b < busTabs.count; ++b) {
            const tab = busTabs.itemAt(b)
            const p = tab.mapFromItem(page, x, y)
            if (p.x >= 0 && p.y >= 0 && p.x < tab.width && p.y < tab.height)
                return b
        }
        return -1
    }
    // Where a row dropped at page y lands in this chain: before the first
    // row whose middle is below it.
    function insertionAt(y) {
        for (let i = 0; i < rows.count; ++i) {
            const row = rows.itemAt(i)
            const top = row.mapToItem(page, 0, 0).y
            if (y < top + row.height / 2)
                return i
        }
        return rows.count
    }
    function dragStart(index) {
        dragFrom = index
        dragInsert = index
        dragBus = -1
    }
    function dragMove(x, y) {
        const tab = busTabAt(x, y)
        dragBus = tab >= 0 && tab !== bus ? tab : -1
        dragInsert = dragBus >= 0 ? -1 : insertionAt(y)
    }
    function dragEnd(x, y, copy) {
        const from = dragFrom
        dragMove(x, y)
        const target = dragBus
        const insert = dragInsert
        dragFrom = -1
        dragInsert = -1
        dragBus = -1
        if (from < 0)
            return
        if (target >= 0) {
            sendTo(from, target, copy)
            return
        }
        const to = insert > from ? insert - 1 : insert
        if (to >= 0 && to !== from)
            doc.move_chain_module(panel.fxChain, from, to)
    }

    Timer {
        id: noteTimer
        interval: page.noteMs
        onTriggered: page.note = ""
    }

    // ---- The rack ------------------------------------------------------------------------

    Rectangle {
        id: rack
        objectName: "novaFxRack"
        width: page.rackWidth
        height: parent.height
        radius: page.panel.theme.fxRackRadius
        color: page.panel.theme.fxRackFill
        border.color: page.panel.theme.line

        // The buses: a tab each, with its module count and a live meter.
        Row {
            id: busRow
            objectName: "novaBus"
            x: page.panel.theme.fxRackPad
            y: page.panel.theme.fxRackPad
            spacing: page.panel.theme.spaceXs
            Repeater {
                id: busTabs
                model: Contract.fxChains
                delegate: Rectangle {
                    id: busTab
                    required property var modelData
                    required property int index
                    readonly property bool on: page.bus === index
                    readonly property int count: page.doc.chains_revision >= 0 ? page.doc.chain(modelData.id).length : 0
                    readonly property real level: {
                        if (!page.panel.live)
                            return 0
                        const db = Math.max(page.panel.at(Contract.T.meters + 2 * index),
                                            page.panel.at(Contract.T.meters + 2 * index + 1))
                        return Math.max(0, Math.min(1, (db - page.meterFloorDb) / -page.meterFloorDb))
                    }
                    objectName: "novaBus:" + index
                    width: (page.rackWidth - 2 * page.panel.theme.fxRackPad - (Contract.fxChains.length - 1) * page.panel.theme.spaceXs)
                           / Contract.fxChains.length
                    height: page.busTabHeight
                    radius: page.panel.theme.fxRackTileRadius
                    color: on ? page.panel.theme.selectedFill : page.panel.theme.card
                    border.color: page.dragBus === index ? page.panel.theme.am : on ? page.panel.theme.pk : page.panel.theme.line
                    border.width: page.dragBus === index ? page.panel.theme.strokeBold : page.panel.theme.hairline
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.name
                    Accessible.checked: on
                    Text {
                        x: page.panel.theme.fxBusTextX
                        y: page.panel.theme.fxBusNameTop
                        text: busTab.modelData.name
                        color: busTab.on ? page.panel.theme.text : page.panel.theme.dim
                        font.family: page.panel.theme.sans
                        font.pixelSize: page.panel.theme.typeSmall
                        font.weight: Font.ExtraBold
                        font.letterSpacing: page.panel.theme.fxRackNameSpacing
                    }
                    Text {
                        x: page.panel.theme.fxBusTextX
                        y: page.panel.theme.fxBusCountTop
                        text: busTab.count + " of " + Contract.fxChainSlots
                        color: page.panel.theme.faint
                        font.family: page.panel.theme.sans
                        font.pixelSize: page.panel.theme.typeSmall
                    }
                    Rectangle {
                        objectName: "novaBusMeter:" + busTab.index
                        x: parent.width - page.panel.theme.fxBusMeterRight
                        y: page.panel.theme.fxBusMeterInset
                        width: page.panel.theme.fxBusMeterWidth
                        height: parent.height - 2 * page.panel.theme.fxBusMeterInset
                        radius: width / 2
                        color: page.panel.theme.knobTrack
                        Rectangle {
                            y: parent.height * (1 - busTab.level)
                            width: parent.width
                            height: parent.height * busTab.level
                            radius: width / 2
                            color: page.panel.theme.pk
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: page.panel.fxChain = busTab.modelData.id
                    }
                }
            }
        }

        // Routing: what Main is, or the bus's send and return.
        Rectangle {
            id: route
            x: page.panel.theme.fxRackPad
            y: busRow.y + page.busTabHeight + page.panel.theme.spaceMd
            width: page.rackWidth - 2 * page.panel.theme.fxRackPad
            height: page.bus === 0 ? page.routeHeight : page.busRouteHeight
            radius: page.panel.theme.fxRackTileRadius
            color: "transparent"
            border.color: page.panel.theme.line
            Text {
                visible: page.bus === 0
                x: page.panel.theme.fxRouteInset
                y: page.panel.theme.fxRouteTextTop
                width: parent.width - 2 * page.panel.theme.fxRouteInset
                text: "Every voice passes through Main. Bus 1 and Bus 2 take sends from it and return to its output."
                color: page.panel.theme.dim
                font.family: page.panel.theme.sans
                font.pixelSize: page.panel.theme.typeSmall
                wrapMode: Text.WordWrap
            }
            Loader {
                active: page.bus > 0
                visible: active
                x: page.panel.theme.fxRouteKnobsX
                y: page.panel.theme.fxRouteKnobsY
                sourceComponent: Row {
                    spacing: page.panel.theme.spaceMd
                    NovaKnob {
                        panel: page.panel
                        parameterId: "nova.fx.bus." + page.bus + ".send"
                        size: "sm"
                        label: "SEND"
                    }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: page.panel.theme.spaceXs
                        Text {
                            text: "FROM MAIN"
                            color: page.panel.theme.faint
                            font.family: page.panel.theme.sans
                            font.pixelSize: page.panel.theme.typeSmall
                            font.weight: Font.Bold
                            font.letterSpacing: page.panel.theme.fxRouteLabelSpacing
                        }
                        NovaSegs {
                            panel: page.panel
                            parameterId: "nova.fx.bus." + page.bus + ".send_point"
                            labels: ["PRE", "POST"]
                        }
                    }
                    NovaKnob {
                        panel: page.panel
                        parameterId: "nova.fx.bus." + page.bus + ".return"
                        size: "sm"
                        label: "RETURN"
                    }
                }
            }
        }

        // The chain, top to bottom.
        Column {
            id: chainColumn
            objectName: "novaFxChain"
            x: page.panel.theme.fxRackPad
            y: route.y + route.height + page.panel.theme.fxRackPad
            width: page.rackWidth - 2 * page.panel.theme.fxRackPad
            spacing: 0
            FlowNode { theme: page.panel.theme; text: "VOICES IN" }
            Wire { theme: page.panel.theme; height: page.wireHeight }
            Repeater {
                id: rows
                model: page.modules
                delegate: Column {
                    id: rowBlock
                    required property var modelData
                    required property int index
                    width: chainColumn.width
                    Rectangle {
                        objectName: "novaFxInsert:" + rowBlock.index
                        visible: page.dragFrom >= 0 && page.dragInsert === rowBlock.index
                                 && rowBlock.index !== page.dragFrom && rowBlock.index !== page.dragFrom + 1
                        width: parent.width
                        height: page.insertHeight
                        radius: page.panel.theme.radiusXs
                        color: page.panel.theme.am
                    }
                    NovaFxRow {
                        page: page
                        panel: page.panel
                        index: rowBlock.index
                        uid: rowBlock.modelData.uid
                        module: rowBlock.modelData.module
                        width: parent.width
                        height: page.rowHeight
                    }
                    Wire { theme: page.panel.theme; height: page.wireHeight }
                }
            }
            Rectangle {
                visible: page.dragFrom >= 0 && page.dragInsert === page.modules.length
                         && page.dragFrom !== page.modules.length - 1
                width: parent.width
                height: page.insertHeight
                radius: page.panel.theme.radiusXs
                color: page.panel.theme.am
            }
            FlowNode { theme: page.panel.theme; text: page.bus === 0 ? "OUT" : "RETURN TO MAIN" }
            Item { width: page.panel.theme.hairline; height: page.panel.theme.spaceMd }
            // "+ Add effect", or why there is none.
            Rectangle {
                id: addButton
                objectName: "novaAddEffect"
                visible: !page.busFull
                width: parent.width
                height: page.addHeight
                radius: page.panel.theme.fxRackTileRadius
                color: addArea.containsMouse ? page.panel.theme.hoverDeep : "transparent"
                border.color: addArea.containsMouse ? page.panel.theme.pk : page.panel.theme.line2
                Accessible.role: Accessible.Button
                Accessible.name: "Add effect"
                Row {
                    anchors.centerIn: parent
                    spacing: page.panel.theme.spaceMd
                    Text {
                        text: "+ Add effect"
                        color: addArea.containsMouse ? page.panel.theme.pk : page.panel.theme.dim
                        font.family: page.panel.theme.sans
                        font.pixelSize: page.panel.theme.typeLabel
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: (Contract.fxChainSlots - page.modules.length) + " free"
                        color: page.panel.theme.faint
                        font.family: page.panel.theme.mono
                        font.pixelSize: page.panel.theme.typeSmall
                    }
                }
                MouseArea {
                    id: addArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: page.controller.editable
                    onClicked: page.openBrowser(addButton, -1)
                }
            }
            Text {
                objectName: "novaFxFull"
                visible: page.busFull
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: Contract.fxChainSlots + " OF " + Contract.fxChainSlots + " SLOTS · REMOVE ONE TO ADD"
                color: page.panel.theme.faint
                font.family: page.panel.theme.sans
                font.pixelSize: page.panel.theme.typeSmall
                font.weight: Font.Bold
                font.letterSpacing: page.panel.theme.sectionLabelSpacing
            }
        }

        // What the last action did, or why it could not.
        Text {
            objectName: "novaFxNote"
            visible: page.note !== ""
            x: page.panel.theme.fxRackPad
            width: page.rackWidth - 2 * page.panel.theme.fxRackPad
            anchors.bottom: foot.top
            anchors.bottomMargin: page.panel.theme.spaceSm
            text: page.note
            color: page.panel.theme.am
            font.family: page.panel.theme.sans
            font.pixelSize: page.panel.theme.typeBody
            wrapMode: Text.WordWrap
        }

        // Split and oversampling.
        Column {
            id: foot
            x: page.panel.theme.fxRackPad
            anchors.bottom: parent.bottom
            anchors.bottomMargin: page.panel.theme.fxRackPad
            width: page.rackWidth - 2 * page.panel.theme.fxRackPad
            spacing: page.panel.theme.spaceMd
            Rectangle {
                width: parent.width
                height: page.panel.theme.hairline
                color: page.panel.theme.line
            }
            Row {
                visible: page.bus === 0
                spacing: page.panel.theme.spaceMd
                height: page.splitOn ? page.splitHeight : page.panel.theme.controlRowHeight
                FootLabel { theme: page.panel.theme; text: "SPLIT LOW"; anchors.verticalCenter: parent.verticalCenter }
                NovaPill {
                    anchors.verticalCenter: parent.verticalCenter
                    panel: page.panel
                    text: page.splitOn ? "ON" : "OFF"
                    parameterId: "nova.fx.split"
                }
                Loader {
                    active: page.splitOn
                    visible: active
                    anchors.verticalCenter: parent.verticalCenter
                    sourceComponent: NovaKnob {
                        objectName: "novaFxCrossover"
                        panel: page.panel
                        parameterId: "nova.fx.split_frequency"
                        size: "sm"
                        label: "CROSSOVER"
                    }
                }
            }
            Row {
                spacing: page.panel.theme.spaceMd
                FootLabel { theme: page.panel.theme; text: "OVERSAMPLE"; anchors.verticalCenter: parent.verticalCenter }
                NovaSegs {
                    panel: page.panel
                    parameterId: page.bus === 0 ? "nova.fx.main.oversample" : "nova.fx.bus." + page.bus + ".oversample"
                    labels: ["OFF", "2×", "4×"]
                }
            }
        }
    }

    // The rack's signal-flow marks: a node, a wire between rows, a label.
    component FlowNode: Row {
        required property NovaTheme theme
        property alias text: nodeText.text
        spacing: theme.spaceMd
        height: theme.flowNodeHeight
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.theme.flowNodeSize
            height: parent.theme.flowNodeSize
            radius: parent.theme.flowNodeRadius
            color: "transparent"
            border.color: parent.theme.faint
            border.width: parent.theme.flowNodeStroke
        }
        Text {
            id: nodeText
            anchors.verticalCenter: parent.verticalCenter
            color: parent.theme.faint
            font.family: parent.theme.sans
            font.pixelSize: parent.theme.typeSmall
            font.weight: Font.Bold
            font.letterSpacing: parent.theme.flowNodeSpacing
        }
    }
    component Wire: Item {
        id: wire
        required property NovaTheme theme
        width: theme.wireWidth
        Rectangle {
            x: wire.theme.wireX
            width: wire.theme.strokeBold
            height: parent.height
            color: wire.theme.line2
        }
    }
    component FootLabel: Text {
        required property NovaTheme theme
        width: theme.footLabelWidth
        color: theme.faint
        font.family: theme.sans
        font.pixelSize: theme.typeSmall
        font.weight: Font.Bold
        font.letterSpacing: theme.sectionLabelSpacing
    }

    // ---- The modules -------------------------------------------------------------------------

    Flickable {
        id: stack
        objectName: "novaFxStack"
        x: page.rackWidth + page.gap
        width: page.width - x
        height: parent.height
        contentWidth: width
        contentHeight: strips.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        // Scrolls so the strip of `uid` is in view.
        function reveal(uid) {
            for (let i = 0; i < stripRepeater.count; ++i) {
                const strip = stripRepeater.itemAt(i)
                if (strip !== null && strip.uid === uid) {
                    const top = strip.y
                    const bottom = top + strip.height
                    if (top < contentY)
                        contentY = top
                    else if (bottom > contentY + height)
                        contentY = Math.min(Math.max(0, contentHeight - height), top)
                    return
                }
            }
        }

        Column {
            id: strips
            width: stack.width
            spacing: page.panel.theme.fxStripGap
            Repeater {
                id: stripRepeater
                model: page.modules
                delegate: NovaFxModule {
                    required property var modelData
                    page: page
                    panel: page.panel
                    uid: modelData.uid
                    module: modelData.module
                    width: strips.width
                    viewTop: stack.contentY
                    viewBottom: stack.contentY + stack.height
                }
            }
        }

        Text {
            visible: page.modules.length === 0
            y: page.emptyTop
            width: stack.width
            horizontalAlignment: Text.AlignHCenter
            text: "No effects on " + Contract.fxChains[page.bus].name + " yet. Use + Add effect in the rack."
            color: page.panel.theme.faint
            font.family: page.panel.theme.sans
            font.pixelSize: page.panel.theme.typeEmphasis
        }
    }

    // ---- Popovers ---------------------------------------------------------------------------

    // Clicks outside an open popover close it.
    MouseArea {
        anchors.fill: parent
        visible: browser.visible || namePrompt.visible
        z: 49
        onClicked: {
            browser.close()
            namePrompt.close()
        }
    }
    NovaFxBrowser {
        id: browser
        panel: page.panel
        z: 50
        onPicked: moduleId => {
            if (browser.swapIndex >= 0)
                page.swapModule(browser.swapIndex, moduleId)
            else
                page.addModule(moduleId)
        }
    }
    // Names a user preset for one slot's module.
    Rectangle {
        id: namePrompt
        objectName: "novaFxNamePrompt"
        property int uid: -1
        property string module: ""
        function open() {
            field.text = ""
            visible = true
            field.forceActiveFocus()
        }
        function close() { visible = false }
        function save() {
            const name = field.text.trim()
            if (name === "")
                return
            if (page.controller.save_fx_preset(module, name, page.slotValues(uid))) {
                page.rememberPreset(uid, module, name, page.slotValues(uid))
                page.showNote("Saved “" + name + "”")
            } else {
                page.showNote("“" + name + "” can't be a preset name")
            }
            close()
        }
        visible: false
        z: 50
        width: page.promptWidth
        height: page.promptHeight
        radius: page.panel.theme.promptRadius
        color: page.panel.theme.popFill
        border.color: page.panel.theme.line2
        Text {
            x: page.panel.theme.promptPad
            y: page.panel.theme.promptTitleTop
            text: "SAVE " + (namePrompt.module !== "" ? Contract.module(namePrompt.module).name.toUpperCase() : "") + " PRESET AS"
            color: page.panel.theme.faint
            font.family: page.panel.theme.sans
            font.pixelSize: page.panel.theme.typeSmall
            font.weight: Font.Bold
            font.letterSpacing: page.panel.theme.sectionLabelSpacing
        }
        Rectangle {
            x: page.panel.theme.promptPad
            y: page.panel.theme.promptFieldTop
            width: parent.width - 2 * page.panel.theme.promptPad - saveButton.width - page.panel.theme.spaceMd
            height: page.panel.theme.promptFieldHeight
            radius: page.panel.theme.promptFieldRadius
            color: page.panel.theme.wellFill
            border.color: field.activeFocus ? page.panel.theme.pk : page.panel.theme.line2
            TextInput {
                id: field
                objectName: "novaFxPresetName"
                x: page.panel.theme.promptFieldInset
                width: parent.width - 2 * page.panel.theme.promptFieldInset
                anchors.verticalCenter: parent.verticalCenter
                color: page.panel.theme.text
                font.family: page.panel.theme.sans
                font.pixelSize: page.panel.theme.typeLabel
                clip: true
                Keys.onReturnPressed: namePrompt.save()
                Keys.onEnterPressed: namePrompt.save()
                Keys.onEscapePressed: namePrompt.close()
            }
        }
        NovaPill {
            id: saveButton
            objectName: "novaFxPresetSave"
            x: parent.width - width - page.panel.theme.promptPad
            y: page.panel.theme.promptSaveTop
            panel: page.panel
            text: "SAVE"
            onClicked: namePrompt.save()
        }
    }
}
