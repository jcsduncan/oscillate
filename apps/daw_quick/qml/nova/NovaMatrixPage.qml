pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract
import Oscillate.Format

// The MATRIX page (NOVA-UI-12): one row per route, including those made by
// dragging. Source, aux source (a multiplier), amount (bipolar bar, the
// route's flat parameter), curve (drag to bend), UNI/BI, destination,
// output scale and delete; + ADD ROUTE and a source filter. From the
// keyboard every control takes Tab: Return opens a menu, the arrows move a
// bar or bend the curve (Shift coarser), Delete returns it to its default.
NovaCard {
    id: matrix
    // "", "env.", "lfo.", "macro.", "perf" or "voice.".
    property string filter: ""
    readonly property var doc: panel.doc
    readonly property var controller: panel.controller

    objectName: "novaMatrix"
    title: "MOD MATRIX"
    titleColor: panel.theme.vi
    sub: "EVERY ROUTE, INCLUDING THOSE MADE BY DRAGGING · UP TO 256"

    // An arrow key's direction (+1 or -1) and whether Shift makes it coarse,
    // or 0 for any other key.
    function keyDirection(event) {
        if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
            return 1
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
            return -1
        return 0
    }
    function keyStep(event) {
        return (event.modifiers & Qt.ShiftModifier) !== 0 ? panel.theme.matrixKeyCoarseStep : panel.theme.matrixKeyStep
    }
    function matches(source) {
        if (filter === "")
            return true
        if (filter === "perf")
            return Contract.perfSources.indexOf(source) >= 0
        return source.indexOf(filter) === 0
    }
    function sourceItems(withNone) {
        const items = withNone ? [{ label: "—", value: "" }] : []
        const groups = [["Envelopes", Contract.envSources], ["LFOs", Contract.lfoSources],
                        ["Macros", Contract.macroSources], ["Performance", Contract.perfSources],
                        ["Voice", Contract.voiceSources], ["Arp", Contract.arpSources]]
        for (let g = 0; g < groups.length; ++g) {
            items.push({ label: groups[g][0], value: "", header: true })
            for (let i = 0; i < groups[g][1].length; ++i)
                items.push({ label: panel.sourceName(groups[g][1][i]), value: groups[g][1][i] })
        }
        return items
    }
    // Every modulation destination in use: effect slots only while their
    // module is in a chain, route amounts only of routes that exist.
    function destinationItems() {
        const used = {}
        const chains = ["fx.main", "fx.bus.1", "fx.bus.2"]
        for (let c = 0; c < chains.length; ++c) {
            const modules = doc.chain(chains[c])
            for (let m = 0; m < modules.length; ++m)
                used[modules[m].uid] = true
        }
        const items = []
        let group = ""
        const ids = controller.modulatable_ids()
        for (let i = 0; i < ids.length; ++i) {
            const id = ids[i]
            if (id.indexOf("nova.perf.") === 0)
                continue
            const fx = /^nova\.fx\.(\d+)\./.exec(id)
            if (fx !== null && !used[parseInt(fx[1])])
                continue
            const mod = /^nova\.mod\.(\d+)\.amount$/.exec(id)
            if (mod !== null && Object.keys(doc.route(parseInt(mod[1]))).length === 0)
                continue
            const info = controller.parameter(id)
            if (info.group !== group) {
                group = info.group
                items.push({ label: group, value: "", header: true })
            }
            items.push({ label: info.name, value: id })
        }
        return items
    }

    NovaPill {
        id: addRoute
        objectName: "novaAddRoute"
        panel: matrix.panel
        text: "+ ADD ROUTE"
        accent: matrix.panel.theme.vi
        anchors.right: filterPill.left
        anchors.rightMargin: matrix.panel.theme.spaceSm
        y: matrix.panel.theme.matrixHeaderTop
        onClicked: matrix.panel.menu.open(addRoute, matrix.sourceItems(false), null, matrix.panel.theme.vi, source =>
            matrix.panel.menu.open(addRoute, matrix.destinationItems(), null, matrix.panel.theme.vi, destination => {
                const slot = matrix.doc.add_route(source, destination, 0.3)
                if (slot >= 0)
                    matrix.panel.touchRoute(matrix.doc.route(slot), null)
            }))
    }
    NovaPill {
        id: filterPill
        panel: matrix.panel
        text: "FILTER: " + (matrix.filter === "" ? "ALL" : matrix.filter === "env." ? "ENVELOPES"
              : matrix.filter === "lfo." ? "LFOS" : matrix.filter === "macro." ? "MACROS"
              : matrix.filter === "voice." ? "VOICE" : "PERFORMANCE") + " ▾"
        anchors.right: parent.right
        anchors.rightMargin: matrix.panel.theme.cardInsetX
        y: matrix.panel.theme.matrixHeaderTop
        onClicked: matrix.panel.menu.open(filterPill, [{ label: "All", value: "" }, { label: "Envelopes", value: "env." },
                                                       { label: "LFOs", value: "lfo." }, { label: "Macros", value: "macro." },
                                                       { label: "Performance", value: "perf" }, { label: "Voice", value: "voice." }],
                                          matrix.filter, matrix.panel.theme.vi, value => matrix.filter = value)
    }

    // Column layout, shared by the header and rows: the destination (0)
    // takes the width the others leave.
    readonly property var columns: [140, 22, 140, 250, 70, 104, 0, 150, 40]
    readonly property real destinationMinimum: 160
    // Fixed sizes: the amount bar, the curve, the scale bar; the empty
    // note's top; the curve's inset, samples and the drag that bends it.
    readonly property real amountWidth: 180
    readonly property real curveWidth: 46
    readonly property real curveHeight: 20
    readonly property real curveInset: 2
    readonly property int curveSamples: 30
    readonly property real bendPixels: 60
    readonly property real scaleWidth: 90
    readonly property real emptyTop: 140
    // Below these the page is scaled down to fit (NovaPanel's Page).
    readonly property real minimumWidth: columns.reduce((a, b) => a + b, 0) + 2 * panel.theme.cardInsetX + destinationMinimum
    readonly property real minimumHeight: 300
    readonly property var columnX: {
        const xs = []
        let x = panel.theme.cardInsetX
        const fixed = columns.reduce((a, b) => a + b, 0)
        for (let i = 0; i < columns.length; ++i) {
            xs.push(x)
            x += columns[i] === 0 ? width - 2 * panel.theme.cardInsetX - fixed : columns[i]
        }
        return xs
    }

    Repeater {
        model: ["SOURCE", "×", "AUX SOURCE", "AMOUNT", "CURVE", "POLARITY", "DESTINATION", "OUTPUT", ""]
        delegate: Text {
            required property string modelData
            required property int index
            x: matrix.columnX[index] + matrix.panel.theme.matrixTextInset
            y: matrix.panel.theme.matrixLabelTop
            text: modelData
            color: matrix.panel.theme.faint
            font.family: matrix.panel.theme.sans
            font.pixelSize: matrix.panel.theme.typeSmall
            font.weight: Font.ExtraBold
            font.letterSpacing: matrix.panel.theme.headingSpacing
        }
    }
    Rectangle {
        x: matrix.panel.theme.cardInsetX
        y: matrix.panel.theme.matrixRuleTop
        width: matrix.width - 2 * matrix.panel.theme.cardInsetX
        height: matrix.panel.theme.hairline
        color: matrix.panel.theme.line2
    }

    Text {
        visible: matrix.doc.route_count === 0
        anchors.horizontalCenter: parent.horizontalCenter
        y: matrix.emptyTop
        text: "No routes yet. Drag a ✥ source onto any knob, or use + ADD ROUTE."
        color: matrix.panel.theme.faint
        font.family: matrix.panel.theme.sans
        font.pixelSize: matrix.panel.theme.typeEmphasis
    }

    ListView {
        id: rows
        objectName: "novaMatrixRows"
        x: 0
        y: matrix.panel.theme.matrixRowsTop
        width: matrix.width
        height: matrix.height - y - matrix.panel.theme.spaceMd
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: matrix.doc.routes
        delegate: Item {
            id: row
            required property var modelData
            required property string rowId
            readonly property int slot: modelData.slot
            readonly property bool shown: matrix.matches(modelData.source)
            readonly property real amount: matrix.controller.revision >= 0 ? matrix.controller.value(modelData.amount_id) : 0
            objectName: "novaMatrixRow:" + slot
            width: rows.width
            height: shown ? matrix.panel.theme.matrixRowHeight : 0
            visible: shown

            Rectangle {
                x: matrix.panel.theme.cardInsetX
                y: parent.height - matrix.panel.theme.hairline
                width: parent.width - 2 * matrix.panel.theme.cardInsetX
                height: matrix.panel.theme.hairline
                color: matrix.panel.theme.line
            }

            // Source.
            Rectangle {
                id: sourceChip
                x: matrix.columnX[0] + matrix.panel.theme.matrixControlInset
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(matrix.columns[0] - 2 * matrix.panel.theme.matrixControlInset, sourceRow.implicitWidth + matrix.panel.theme.matrixChipPad)
                height: matrix.panel.theme.controlRowHeight
                radius: matrix.panel.theme.radiusLg
                color: matrix.panel.theme.wellFill
                border.color: activeFocus ? matrix.panel.theme.vi : matrix.panel.theme.viBorder
                border.width: activeFocus ? matrix.panel.theme.strokeBold : matrix.panel.theme.hairline
                activeFocusOnTab: matrix.controller.editable
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Route source"
                Accessible.description: matrix.panel.sourceName(row.modelData.source)
                function open() {
                    matrix.panel.menu.open(sourceChip, matrix.sourceItems(false), row.modelData.source,
                                           matrix.panel.theme.vi,
                                           value => matrix.doc.set_route(row.slot, { source: value }))
                }
                Keys.onReturnPressed: open()
                Keys.onSpacePressed: open()
                // The whole chip opens the menu; the ✥ handle over it drags.
                MouseArea {
                    anchors.fill: parent
                    enabled: matrix.controller.editable
                    onClicked: sourceChip.open()
                }
                Row {
                    id: sourceRow
                    x: matrix.panel.theme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: matrix.panel.theme.matrixChipGap
                    NovaHandle {
                        panel: matrix.panel
                        source: row.modelData.source
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: matrix.panel.sourceName(row.modelData.source)
                        color: matrix.panel.theme.text
                        font.family: matrix.panel.theme.sans
                        font.pixelSize: matrix.panel.theme.typeSmall
                        font.weight: Font.Bold
                    }
                }
            }
            Text {
                x: matrix.columnX[1] + matrix.panel.theme.matrixBarInset
                anchors.verticalCenter: parent.verticalCenter
                text: "×"
                color: matrix.panel.theme.faint
                font.pixelSize: matrix.panel.theme.typeLabel
            }
            NovaDropdown {
                x: matrix.columnX[2] + matrix.panel.theme.matrixControlInset
                anchors.verticalCenter: parent.verticalCenter
                width: matrix.columns[2] - matrix.panel.theme.matrixAuxRoom
                panel: matrix.panel
                text: row.modelData.aux === "" ? "—" : matrix.panel.sourceName(row.modelData.aux)
                entries: matrix.sourceItems(true)
                currentValue: row.modelData.aux
                onPicked: value => matrix.doc.set_route(row.slot, { aux: value })
            }

            // Amount: a bipolar bar on the route's flat parameter.
            Rectangle {
                id: amountBar
                objectName: "novaMatrixAmount:" + row.slot
                x: matrix.columnX[3] + matrix.panel.theme.matrixBarInset
                anchors.verticalCenter: parent.verticalCenter
                width: matrix.amountWidth
                height: matrix.panel.theme.matrixAmountHeight
                radius: matrix.panel.theme.radiusMd
                color: matrix.panel.theme.wellFill
                border.color: activeFocus ? matrix.panel.theme.vi : matrix.panel.theme.line
                border.width: activeFocus ? matrix.panel.theme.strokeBold : matrix.panel.theme.hairline
                activeFocusOnTab: matrix.controller.editable
                Accessible.role: Accessible.Slider
                Accessible.name: "Route amount"
                Accessible.description: Format.fraction(row.amount)
                Keys.onPressed: event => {
                    const direction = matrix.keyDirection(event)
                    if (direction !== 0)
                        matrix.controller.set_value(row.modelData.amount_id,
                            Math.max(-1, Math.min(1, row.amount + direction * matrix.keyStep(event))))
                    else if (event.key === Qt.Key_Delete)
                        matrix.controller.set_value(row.modelData.amount_id, 0)
                    else
                        return
                    matrix.panel.touchRoute(row.modelData, null)
                    event.accepted = true
                }
                Rectangle {
                    x: row.amount < 0 ? parent.width / 2 * (1 + row.amount) : parent.width / 2
                    y: matrix.panel.theme.matrixAmountInset
                    width: Math.abs(row.amount) * parent.width / 2
                    height: parent.height - 2 * matrix.panel.theme.matrixAmountInset
                    radius: matrix.panel.theme.radiusXs
                    color: matrix.panel.theme.vi
                }
                Rectangle {
                    x: parent.width / 2
                    y: -matrix.panel.theme.matrixAmountInset
                    width: matrix.panel.theme.hairline
                    height: parent.height + 2 * matrix.panel.theme.matrixAmountInset
                    color: matrix.panel.theme.line2
                }
                MouseArea {
                    property real startX: 0
                    property real startValue: 0
                    anchors.fill: parent
                    anchors.topMargin: -matrix.panel.theme.hitOutset(parent.height)
                    anchors.bottomMargin: -matrix.panel.theme.hitOutset(parent.height)
                    preventStealing: true
                    cursorShape: Qt.SizeHorCursor
                    enabled: matrix.controller.editable
                    onPressed: mouse => {
                        startX = mouse.x
                        startValue = row.amount
                        matrix.controller.begin_gesture(row.modelData.amount_id)
                        matrix.panel.touchRoute(row.modelData, null)
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            matrix.controller.set_value(row.modelData.amount_id,
                                Math.max(-1, Math.min(1, startValue + (mouse.x - startX) / (amountBar.width / 2))))
                    }
                    onReleased: matrix.controller.end_gesture(row.modelData.amount_id)
                    onCanceled: matrix.controller.cancel_gesture(row.modelData.amount_id)
                    onDoubleClicked: matrix.controller.set_value(row.modelData.amount_id, 0)
                }
            }
            Text {
                x: amountBar.x + amountBar.width + matrix.panel.theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                text: Format.fraction(row.amount)
                color: matrix.panel.theme.vi
                font.family: matrix.panel.theme.mono
                font.pixelSize: matrix.panel.theme.typeSmall
            }

            // Curve: drag up or down to bend the route's response.
            Canvas {
                id: curve
                x: matrix.columnX[4] + matrix.panel.theme.matrixTextInset
                anchors.verticalCenter: parent.verticalCenter
                width: matrix.curveWidth
                height: matrix.curveHeight
                readonly property real tension: row.modelData.tension
                onTensionChanged: requestPaint()
                activeFocusOnTab: matrix.controller.editable
                Accessible.role: Accessible.Slider
                Accessible.name: "Route curve"
                Accessible.description: Format.fraction(tension) + ". Up and Down bend it, Delete straightens it"
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                        // Up bends as a drag up does.
                        const direction = event.key === Qt.Key_Up ? -1 : 1
                        matrix.doc.set_route(row.slot, { tension: Math.max(-1, Math.min(1,
                            tension + direction * matrix.panel.theme.matrixBendKeyStep)) })
                    } else if (event.key === Qt.Key_Delete) {
                        matrix.doc.set_route(row.slot, { tension: 0 })
                    } else {
                        return
                    }
                    event.accepted = true
                }
                Rectangle {
                    visible: curve.activeFocus
                    anchors.fill: parent
                    radius: matrix.panel.theme.radiusMd
                    color: "transparent"
                    border.color: matrix.panel.theme.vi
                    border.width: matrix.panel.theme.strokeBold
                }
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.strokeStyle = matrix.panel.theme.vi
                    ctx.lineWidth = matrix.panel.theme.matrixCurveLineWidth
                    ctx.beginPath()
                    const inset = matrix.curveInset
                    for (let i = 0; i <= matrix.curveSamples; ++i) {
                        const t = i / matrix.curveSamples
                        const x = inset + t * (width - 2 * inset)
                        const y = height - inset - Contract.bend(t, tension) * (height - 2 * inset)
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                    ctx.stroke()
                }
                MouseArea {
                    property real startY: 0
                    property real startTension: 0
                    anchors.fill: parent
                    anchors.topMargin: -matrix.panel.theme.hitOutset(parent.height)
                    anchors.bottomMargin: -matrix.panel.theme.hitOutset(parent.height)
                    preventStealing: true
                    cursorShape: Qt.SizeVerCursor
                    enabled: matrix.controller.editable
                    onPressed: mouse => {
                        startY = mouse.y
                        startTension = row.modelData.tension
                        matrix.doc.begin_edit()
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            matrix.doc.set_route(row.slot, { tension: Math.max(-1, Math.min(1, startTension + (mouse.y - startY) / matrix.bendPixels)) })
                    }
                    onReleased: matrix.doc.commit_edit()
                    onCanceled: matrix.doc.cancel_edit()
                    onDoubleClicked: matrix.doc.set_route(row.slot, { tension: 0 })
                }
            }

            NovaSegs {
                x: matrix.columnX[5] + matrix.panel.theme.matrixControlInset
                anchors.verticalCenter: parent.verticalCenter
                panel: matrix.panel
                labels: ["UNI", "BI"]
                current: row.modelData.bipolar ? 1 : 0
                accent: matrix.panel.theme.vi
                onPicked: index => matrix.doc.set_route(row.slot, { bipolar: index === 1 })
            }

            // Destination: click to retarget.
            Row {
                id: destination
                x: matrix.columnX[6] + matrix.panel.theme.matrixTextInset
                anchors.verticalCenter: parent.verticalCenter
                spacing: matrix.panel.theme.spaceSm
                Text {
                    text: matrix.panel.destinationName(row.modelData.destination)
                    color: matrix.panel.theme.text
                    font.family: matrix.panel.theme.sans
                    font.pixelSize: matrix.panel.theme.typeBody
                    font.weight: Font.Bold
                }
                Text {
                    text: matrix.panel.destinationGroup(row.modelData.destination)
                    color: matrix.panel.theme.faint
                    font.family: matrix.panel.theme.sans
                    font.pixelSize: matrix.panel.theme.typeBody
                }
            }
            MouseArea {
                id: destinationArea
                anchors.fill: destination
                anchors.topMargin: -matrix.panel.theme.hitOutset(destination.height)
                anchors.bottomMargin: -matrix.panel.theme.hitOutset(destination.height)
                enabled: matrix.controller.editable
                cursorShape: Qt.PointingHandCursor
                activeFocusOnTab: matrix.controller.editable
                Accessible.role: Accessible.ComboBox
                Accessible.name: "Route destination"
                Accessible.description: matrix.panel.destinationName(row.modelData.destination)
                function open() {
                    matrix.panel.menu.open(destination, matrix.destinationItems(), row.modelData.destination,
                                           matrix.panel.theme.vi,
                                           value => matrix.doc.set_route(row.slot, { destination: value }))
                }
                Keys.onReturnPressed: open()
                Keys.onSpacePressed: open()
                onClicked: open()
                Rectangle {
                    visible: destinationArea.activeFocus
                    anchors.fill: parent
                    radius: matrix.panel.theme.radiusMd
                    color: "transparent"
                    border.color: matrix.panel.theme.vi
                    border.width: matrix.panel.theme.strokeBold
                }
            }

            // Output scale (document data): a small bar.
            Rectangle {
                id: scaleBar
                objectName: "novaMatrixScale:" + row.slot
                x: matrix.columnX[7] + matrix.panel.theme.matrixBarInset
                anchors.verticalCenter: parent.verticalCenter
                width: matrix.scaleWidth
                height: matrix.panel.theme.matrixScaleHeight
                radius: height / 2
                color: matrix.panel.theme.wellFill
                border.color: activeFocus ? matrix.panel.theme.vi : matrix.panel.theme.line
                border.width: activeFocus ? matrix.panel.theme.strokeBold : matrix.panel.theme.hairline
                activeFocusOnTab: matrix.controller.editable
                Accessible.role: Accessible.Slider
                Accessible.name: "Route output scale"
                Accessible.description: Format.fraction(row.modelData.scale)
                Keys.onPressed: event => {
                    const direction = matrix.keyDirection(event)
                    if (direction === 0)
                        return
                    matrix.doc.set_route(row.slot, { scale: Math.max(0, Math.min(1,
                        row.modelData.scale + direction * matrix.keyStep(event))) })
                    event.accepted = true
                }
                Rectangle {
                    width: parent.width * row.modelData.scale
                    height: parent.height
                    radius: height / 2
                    color: matrix.panel.theme.vi
                    opacity: matrix.panel.theme.matrixScaleOpacity
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: -matrix.panel.theme.matrixScaleHit
                    anchors.rightMargin: -matrix.panel.theme.matrixScaleHit
                    anchors.topMargin: -matrix.panel.theme.hitOutset(parent.height)
                    anchors.bottomMargin: -matrix.panel.theme.hitOutset(parent.height)
                    preventStealing: true
                    cursorShape: Qt.SizeHorCursor
                    enabled: matrix.controller.editable
                    function scaleAt(x) { return Math.max(0, Math.min(1, (x - matrix.panel.theme.matrixScaleHit) / scaleBar.width)) }
                    onPressed: mouse => {
                        matrix.doc.begin_edit()
                        matrix.doc.set_route(row.slot, { scale: scaleAt(mouse.x) })
                    }
                    onPositionChanged: mouse => {
                        if (pressed)
                            matrix.doc.set_route(row.slot, { scale: scaleAt(mouse.x) })
                    }
                    onReleased: matrix.doc.commit_edit()
                    onCanceled: matrix.doc.cancel_edit()
                }
            }
            Text {
                x: scaleBar.x + scaleBar.width + matrix.panel.theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                text: Format.fraction(row.modelData.scale)
                color: matrix.panel.theme.dim
                font.family: matrix.panel.theme.mono
                font.pixelSize: matrix.panel.theme.typeSmall
            }

            NovaPill {
                objectName: "novaMatrixDelete:" + row.slot
                x: matrix.columnX[8] + matrix.panel.theme.matrixBarInset
                anchors.verticalCenter: parent.verticalCenter
                panel: matrix.panel
                text: "✕"
                onClicked: matrix.doc.remove_route(row.slot)
            }
        }
    }
}
