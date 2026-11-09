pragma ComponentBehavior: Bound
import QtQuick
import "../shell" as Shell
import Oscillate.Format

// Knock's Patterns area: Loops and Generate tabs
// (role chips over the loop library with thumbnails; the generator's
// styles, sliders and bars), Roll, Mutate and Evolve, the bar tabs, and the
// working pattern as a grid of eight lanes with locks, editable by click.
// Nothing here touches the project until Write to clip, which writes the
// pattern into the track's selected MIDI clip, or a new one at the
// playhead's bar, as one undo step. Audition asks the host to play it.
//
// `patterns` is the panel model's KnockPatternsModel.
Item {
    id: area
    required property KnockTheme theme
    property var patterns: null
    // Pad names for the lanes, and the kit loaded (the controller's preset).
    property var labels: []
    property string kitName: ""
    // Where a new clip goes (the song's playhead, in bars from 0) and the
    // arrangement's selected clips (a MIDI clip on this track is replaced).
    property real playheadBar: 0
    property var selectedClipIds: []
    signal auditionRequested()

    readonly property bool ready: patterns !== null
    readonly property int revision: ready ? patterns.revision : -1
    readonly property int lanes: ready ? patterns.lane_count : 0
    readonly property int steps: ready ? patterns.steps_per_bar : 0
    readonly property int stepsPerBeat: ready ? patterns.steps_per_quarter : 1
    readonly property var cells: ready && revision >= 0 ? patterns.cells : []
    readonly property var locks: ready && revision >= 0 ? patterns.locked : []
    // "loops" or "generate".
    property string tab: "loops"
    // The library filter: "all", "kit" or "style<i>".
    property string role: "all"
    readonly property var shownLoops: {
        if (!ready)
            return []
        const all = patterns.loops
        return all.filter(loop => {
            const own = loop.kit !== "" && loop.kit === area.kitName
            if (area.role === "all")
                return own || loop.kit === ""
            if (area.role === "kit")
                return own
            return loop.kit === "" && "style" + loop.style === area.role
        })
    }

    objectName: "knockPatterns"
    implicitWidth: theme.leftColumnWidth
    implicitHeight: theme.patternsHeight

    function laneName(lane) {
        return labels[lane] !== undefined && labels[lane] !== "" ? labels[lane] : "Pad " + (lane + 1)
    }
    function writeClip() {
        if (ready)
            patterns.write_to_clip(Math.max(0, Math.floor(playheadBar)), selectedClipIds)
    }

    // ---- Small parts ------------------------------------------------------------------

    component Tab: Rectangle {
        id: tabItem
        property string text: ""
        property bool on: false
        signal clicked()
        width: tabCaption.implicitWidth + area.theme.space2xl
        height: area.theme.tabHeight
        color: "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.PageTab
        Accessible.name: text
        Keys.onReturnPressed: clicked()
        Keys.onSpacePressed: clicked()
        Text {
            id: tabCaption
            anchors.centerIn: parent
            text: tabItem.text.toUpperCase()
            color: tabItem.on ? area.theme.ink : area.theme.inkFaint
            font.family: area.theme.sans
            font.pixelSize: area.theme.typeBody
            font.bold: true
            font.letterSpacing: area.theme.capsSpacing
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: area.theme.strokeBold
            color: tabItem.on ? area.theme.strike : (tabItem.activeFocus ? area.theme.line2 : "transparent")
        }
        MouseArea {
            anchors.fill: parent
            onClicked: tabItem.clicked()
        }
    }
    // A pill or a tile: lit when on.
    component Tile: Rectangle {
        id: tile
        property string text: ""
        property bool on: false
        property bool pill: false
        property color accent: area.theme.strike
        signal clicked()
        width: Math.max(area.theme.tileMinWidth, tileCaption.implicitWidth + area.theme.tilePad)
        height: area.theme.tileHeight
        radius: pill ? height / 2 : area.theme.radiusLg
        color: on ? Qt.rgba(accent.r, accent.g, accent.b, area.theme.tileOnAlpha) : area.theme.panel2
        border.color: on ? accent : (activeFocus ? area.theme.line2 : area.theme.line)
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.checked: on
        Keys.onReturnPressed: clicked()
        Keys.onSpacePressed: clicked()
        Text {
            id: tileCaption
            anchors.centerIn: parent
            text: tile.text
            color: tile.on ? tile.accent : area.theme.inkDim
            font.family: area.theme.sans
            font.pixelSize: tile.pill ? area.theme.typeSmall : area.theme.typeBody
            font.bold: true
        }
        MouseArea {
            anchors.fill: parent
            onClicked: tile.clicked()
        }
    }
    // The pattern grid's keyboard cursor: a ring over a lock or a cell.
    component CursorRing: Rectangle {
        anchors.fill: parent
        z: 1
        radius: area.theme.radiusMd
        color: "transparent"
        border.color: area.theme.gridCursorInk
        border.width: area.theme.gridCursorStroke
    }
    component Action: Rectangle {
        id: action
        property string text: ""
        property string tip: text
        property bool strong: false
        property color accent: area.theme.strike
        property bool active: area.ready
        signal clicked()
        width: Math.max(area.theme.actionMinWidth, actionCaption.implicitWidth + area.theme.actionPad)
        height: area.theme.actionHeight
        radius: area.theme.radiusXl
        opacity: active ? 1 : area.theme.disabledOpacity
        color: strong ? accent : (actionArea.containsMouse ? area.theme.strikeSoft : area.theme.panel2)
        border.color: strong ? accent : (activeFocus ? accent : area.theme.line)
        activeFocusOnTab: active
        Accessible.role: Accessible.Button
        Accessible.name: tip
        Keys.onReturnPressed: if (active) clicked()
        Keys.onSpacePressed: if (active) clicked()
        Text {
            id: actionCaption
            anchors.centerIn: parent
            text: action.text
            color: action.strong ? area.theme.caseColor : action.accent
            font.family: area.theme.sans
            font.pixelSize: area.theme.typeLabel
            font.bold: true
        }
        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: action.active
            onClicked: action.clicked()
            Shell.Tip { text: action.tip }
        }
    }
    // A generator setting, 0..1: drag or click along the rail, arrows step.
    component Setting: Item {
        id: setting
        required property string key
        property string label: ""
        readonly property real value: area.ready && area.revision >= 0 ? area.patterns[key] : 0
        readonly property real step: 0.05
        readonly property real labelWidth: area.theme.settingLabelWidth
        readonly property real valueWidth: area.theme.settingValueWidth
        objectName: "knockGen:" + key
        height: area.theme.settingHeight
        activeFocusOnTab: area.ready
        Accessible.role: Accessible.Slider
        Accessible.name: label
        Accessible.description: Format.fraction(value)
        function set(fraction) {
            if (area.ready)
                area.patterns[key] = Math.max(0, Math.min(1, fraction))
        }
        Keys.onLeftPressed: set(value - step)
        Keys.onDownPressed: set(value - step)
        Keys.onRightPressed: set(value + step)
        Keys.onUpPressed: set(value + step)
        Text {
            width: setting.labelWidth
            anchors.verticalCenter: parent.verticalCenter
            text: setting.label
            color: area.theme.inkDim
            font.family: area.theme.sans
            font.pixelSize: area.theme.typeBody
            elide: Text.ElideRight
        }
        Rectangle {
            id: rail
            x: setting.labelWidth + area.theme.spaceXs
            width: setting.width - setting.labelWidth - setting.valueWidth - 2 * area.theme.spaceXs
            anchors.verticalCenter: parent.verticalCenter
            height: area.theme.settingRailHeight
            radius: area.theme.radiusXs
            color: area.theme.track
            Rectangle {
                width: parent.width * setting.value
                height: parent.height
                radius: area.theme.radiusXs
                color: area.theme.strike
            }
            Rectangle {
                x: parent.width * setting.value - width / 2
                anchors.verticalCenter: parent.verticalCenter
                width: area.theme.settingHandle
                height: area.theme.settingHandle
                radius: area.theme.settingHandle / 2
                color: setting.activeFocus ? area.theme.ink : area.theme.strike
            }
            MouseArea {
                anchors.fill: parent
                anchors.topMargin: -area.theme.settingHitSlop
                anchors.bottomMargin: -area.theme.settingHitSlop
                function at(mouseX) {
                    setting.set(mouseX / Math.max(1, rail.width))
                }
                onPressed: mouse => {
                    setting.forceActiveFocus()
                    at(mouse.x)
                }
                onPositionChanged: mouse => {
                    if (pressed)
                        at(mouse.x)
                }
            }
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: Format.fraction(setting.value)
            color: area.theme.ink
            font.family: area.theme.mono
            font.pixelSize: area.theme.typeBody
        }
    }
    // A loop's first bar, cell by cell, accents brightest.
    component Thumb: Canvas {
        id: thumb
        property var cells: []
        renderStrategy: Canvas.Cooperative
        onCellsChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (area.lanes <= 0 || area.steps <= 0)
                return
            const w = width / area.steps
            const h = height / area.lanes
            ctx.fillStyle = area.theme.strike
            for (let lane = 0; lane < area.lanes; ++lane) {
                for (let step = 0; step < area.steps; ++step) {
                    const kind = cells[lane * area.steps + step]
                    if (!kind)
                        continue
                    ctx.globalAlpha = kind === 2 ? 1 : area.theme.thumbHitAlpha
                    ctx.fillRect(step * w, lane * h, Math.max(1, w - 1), Math.max(1, h - 1))
                }
            }
            ctx.globalAlpha = 1
        }
    }

    // ---- Tabs and their panes --------------------------------------------------------

    Item {
        id: tabs
        width: parent.width
        height: area.theme.tabHeight
        Row {
            Tab {
                objectName: "knockTabLoops"
                text: "Loops"
                on: area.tab === "loops"
                onClicked: area.tab = "loops"
            }
            Tab {
                objectName: "knockTabGenerate"
                text: "Generate"
                on: area.tab === "generate"
                onClicked: area.tab = "generate"
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: area.theme.hairline
            color: area.theme.line
            z: -1
        }
    }

    Item {
        id: loopsPane
        objectName: "knockLoopsPane"
        y: tabs.height + area.theme.spaceMd
        width: parent.width
        height: area.theme.loopsPaneHeight
        visible: area.tab === "loops"

        Flow {
            id: roleChips
            width: parent.width
            spacing: area.theme.spaceXs
            Repeater {
                model: {
                    const chips = [{ key: "all", name: "All" }, { key: "kit", name: "This kit" }]
                    const styles = area.ready ? area.patterns.style_names : []
                    for (let i = 0; i < styles.length; ++i)
                        chips.push({ key: "style" + i, name: styles[i] })
                    return chips
                }
                delegate: Tile {
                    required property var modelData
                    objectName: "knockRole:" + modelData.key
                    pill: true
                    text: modelData.name
                    on: area.role === modelData.key
                    onClicked: area.role = modelData.key
                }
            }
        }
        GridView {
            id: loopList
            objectName: "knockLoopList"
            y: roleChips.height + area.theme.spaceSm
            width: parent.width
            height: parent.height - y
            clip: true
            cellWidth: width / 2
            cellHeight: area.theme.loopCellHeight
            boundsBehavior: Flickable.StopAtBounds
            model: area.shownLoops
            delegate: Item {
                id: loopCell
                required property var modelData
                width: loopList.cellWidth
                height: loopList.cellHeight
                Rectangle {
                    id: loopButton
                    objectName: "knockLoop:" + loopCell.modelData.name
                    readonly property bool on: area.ready && area.revision >= 0
                        && area.patterns.loop_name === loopCell.modelData.name
                    x: 0
                    y: 0
                    width: parent.width - area.theme.loopCellGap
                    height: parent.height - area.theme.loopCellGap
                    radius: area.theme.radiusXl
                    color: on ? area.theme.strikeSoft : area.theme.panel2
                    border.color: on ? area.theme.strike : (activeFocus ? area.theme.line2 : area.theme.line)
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: loopCell.modelData.name
                    Keys.onReturnPressed: area.patterns.load_loop(loopCell.modelData.name)
                    Keys.onSpacePressed: area.patterns.load_loop(loopCell.modelData.name)
                    Text {
                        x: area.theme.spaceMd
                        y: area.theme.spaceXs
                        // The name has the cell's whole width; the thumbnail
                        // sits beside the line under it.
                        width: parent.width - 2 * area.theme.spaceMd
                        text: loopCell.modelData.name
                        color: area.theme.ink
                        font.family: area.theme.sans
                        font.pixelSize: area.theme.typeLabel
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        x: area.theme.spaceMd
                        y: area.theme.loopMetaTop
                        text: (loopCell.modelData.kit !== "" ? "kit"
                               : (area.ready ? area.patterns.style_names[loopCell.modelData.style].toLowerCase() : ""))
                            + " · " + loopCell.modelData.bars + (loopCell.modelData.bars > 1 ? " bars" : " bar")
                        color: area.theme.inkFaint
                        font.family: area.theme.mono
                        font.pixelSize: area.theme.typeSmall
                    }
                    Thumb {
                        id: thumbnail
                        anchors.right: parent.right
                        anchors.rightMargin: area.theme.spaceMd
                        y: area.theme.loopThumbTop
                        width: area.theme.loopThumbWidth
                        height: area.theme.loopThumbHeight
                        cells: loopCell.modelData.thumb
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            loopButton.forceActiveFocus()
                            area.patterns.load_loop(loopCell.modelData.name)
                        }
                    }
                }
            }
        }
    }

    Item {
        id: generatePane
        objectName: "knockGeneratePane"
        y: loopsPane.y
        width: parent.width
        height: loopsPane.height
        visible: area.tab === "generate"

        Row {
            id: styleRow
            width: parent.width
            spacing: area.theme.styleSpacing
            // The styles share the row in this many columns.
            readonly property int styleColumns: 5
            Repeater {
                model: area.ready ? area.patterns.style_names : []
                delegate: Tile {
                    required property string modelData
                    required property int index
                    objectName: "knockStyle:" + index
                    width: (styleRow.width - styleRow.spacing * (styleRow.styleColumns - 1)) / styleRow.styleColumns
                    text: modelData
                    on: area.ready && area.revision >= 0 && area.patterns.style === index
                    onClicked: area.patterns.style = index
                }
            }
        }
        Grid {
            id: settingGrid
            y: styleRow.height + area.theme.spaceMd
            width: parent.width
            columns: 2
            columnSpacing: area.theme.settingColumnGap
            rowSpacing: area.theme.spaceSm
            readonly property real cellWidth: (width - columnSpacing) / 2
            Setting { key: "density"; label: "Density"; width: settingGrid.cellWidth }
            Setting { key: "syncopation"; label: "Syncopate"; width: settingGrid.cellWidth }
            Setting { key: "ghosts"; label: "Ghosts"; width: settingGrid.cellWidth }
            Setting { key: "ratchets"; label: "Ratchets"; width: settingGrid.cellWidth }
            Setting { key: "chance"; label: "Chance"; width: settingGrid.cellWidth }
            Setting { key: "variation"; label: "Variation"; width: settingGrid.cellWidth }
        }
        Row {
            y: settingGrid.y + settingGrid.height + area.theme.generateRowGap
            spacing: area.theme.spaceXs
            Text {
                width: area.theme.settingLabelWidth
                anchors.verticalCenter: parent.verticalCenter
                text: "Bars"
                color: area.theme.inkDim
                font.family: area.theme.sans
                font.pixelSize: area.theme.typeBody
            }
            Repeater {
                model: area.ready ? area.patterns.bar_choices : []
                delegate: Tile {
                    required property int modelData
                    objectName: "knockBars:" + modelData
                    text: String(modelData)
                    on: area.ready && area.revision >= 0 && area.patterns.bars === modelData
                    onClicked: area.patterns.bars = modelData
                }
            }
            Text {
                leftPadding: area.theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                objectName: "knockSeed"
                text: area.ready && area.revision >= 0 ? "SEED " + area.patterns.seed : ""
                color: area.theme.inkFaint
                font.family: area.theme.mono
                font.pixelSize: area.theme.typeBody
            }
        }
    }

    // ---- Roll, Mutate, Evolve --------------------------------------------------------

    Row {
        id: actions
        y: loopsPane.y + loopsPane.height + area.theme.spaceMd
        width: parent.width
        spacing: area.theme.spaceSm
        Action {
            objectName: "knockRoll"
            text: "⚄ Roll"
            tip: "A fresh loop; locked lanes stay"
            strong: true
            accent: area.theme.body
            onClicked: area.patterns.roll()
        }
        Action {
            objectName: "knockMutate"
            text: "Mutate"
            tip: "Change a little of every unlocked lane"
            onClicked: area.patterns.mutate()
        }
        Text {
            leftPadding: area.theme.spaceSm
            anchors.verticalCenter: parent.verticalCenter
            text: "EVOLVE"
            color: area.theme.inkDim
            font.family: area.theme.sans
            font.pixelSize: area.theme.typeSmall
            font.bold: true
            font.letterSpacing: area.theme.capsSpacing
        }
        Repeater {
            model: area.ready ? area.patterns.evolve_choices : []
            delegate: Tile {
                required property int modelData
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                objectName: "knockEvolve:" + modelData
                text: modelData === 0 ? "Off" : String(modelData)
                accent: area.theme.coupled
                on: area.ready && area.revision >= 0 && area.patterns.evolve_bars === modelData
                onClicked: area.patterns.evolve_bars = modelData
            }
        }
    }

    // ---- Bars, audition and write ------------------------------------------------------

    Item {
        id: barRow
        y: actions.y + actions.height + area.theme.spaceMd
        width: parent.width
        height: area.theme.actionHeight
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: area.theme.spaceXs
            Text {
                rightPadding: area.theme.spaceXs
                anchors.verticalCenter: parent.verticalCenter
                text: "BAR"
                color: area.theme.inkDim
                font.family: area.theme.sans
                font.pixelSize: area.theme.typeSmall
                font.bold: true
                font.letterSpacing: area.theme.capsSpacing
            }
            Repeater {
                model: area.ready && area.revision >= 0 ? area.patterns.pattern_bars : 0
                delegate: Tile {
                    required property int index
                    objectName: "knockBarTab:" + index
                    width: area.theme.tileMinWidth
                    text: String(index + 1)
                    on: area.ready && area.revision >= 0 && area.patterns.view_bar === index
                    accent: area.theme.ink
                    onClicked: area.patterns.view_bar = index
                }
            }
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: area.theme.spaceSm
            Action {
                objectName: "knockAudition"
                text: "Audition"
                tip: "Hear the pattern on this track's sound (while the song is stopped)"
                onClicked: area.auditionRequested()
            }
            Action {
                objectName: "knockWrite"
                text: "Write to clip"
                tip: "Write the pattern into the selected MIDI clip, or a new one at the playhead"
                strong: true
                onClicked: area.writeClip()
            }
        }
    }

    // ---- The grid ---------------------------------------------------------------------

    // One Tab stop: the arrow keys move a cursor over the locks (column -1)
    // and the cells, Space or Return toggles the lock or cycles the cell.
    // Each cell's hit area is a whole lane row by a whole step; the drawn
    // cell sits inside it, leaving the lane and step gaps.
    Column {
        id: grid
        objectName: "knockGrid"
        y: barRow.y + barRow.height + area.theme.spaceMd
        width: parent.width
        spacing: 0
        activeFocusOnTab: area.ready
        readonly property real lockWidth: area.theme.laneLockWidth
        // The cursor: a lane, and a step or -1 for the lane's lock.
        property int cursorLane: 0
        property int cursorStep: 0
        // The name column is as wide as the longest lane name, short of
        // squeezing the steps below laneCellMinWidth; a name longer still
        // is elided, its tooltip showing it whole.
        readonly property real nameRoom: area.steps > 0
            ? width - lockWidth - nameGap - area.theme.laneCellMinWidth * area.steps : width
        readonly property real nameWidth: {
            let widest = area.theme.laneNameMinWidth
            for (let lane = 0; lane < area.lanes; ++lane)
                widest = Math.max(widest, Math.ceil(nameMetrics.advanceWidth(area.laneName(lane)))
                                  + area.theme.spaceXs)
            return Math.min(widest, Math.max(area.theme.laneNameMinWidth, nameRoom))
        }
        FontMetrics {
            id: nameMetrics
            font.family: area.theme.sans
            font.pixelSize: area.theme.typeSmall
        }
        // Between the lane's name and its first cell.
        readonly property real nameGap: 8
        readonly property real cellWidth: area.steps > 0
            ? (width - lockWidth - nameWidth - nameGap) / area.steps : 0
        readonly property int cursorKind: area.cells[cursorLane * area.steps + cursorStep] !== undefined
            ? area.cells[cursorLane * area.steps + cursorStep] : 0

        Accessible.role: Accessible.Table
        Accessible.name: "Pattern grid"
        Accessible.description: !area.ready ? ""
            : cursorStep < 0
                ? area.laneName(cursorLane) + " lock, " + (area.locks[cursorLane] === true ? "on" : "off")
                : area.laneName(cursorLane) + " step " + (cursorStep + 1) + ", "
                    + area.patterns.cell_names[cursorKind]

        // Moves the cursor to a lane and step (-1: the lock), kept in the grid.
        function moveTo(lane, step) {
            cursorLane = Math.max(0, Math.min(area.lanes - 1, lane))
            cursorStep = Math.max(-1, Math.min(area.steps - 1, step))
        }
        // Toggles the lock or cycles the cell under the cursor.
        function activate() {
            if (cursorStep < 0)
                area.patterns.set_locked(cursorLane, area.locks[cursorLane] !== true)
            else
                area.patterns.cycle_cell(cursorLane, cursorStep)
        }
        Keys.onPressed: event => {
            if (!area.ready)
                return
            if (event.key === Qt.Key_Left)
                moveTo(cursorLane, cursorStep - 1)
            else if (event.key === Qt.Key_Right)
                moveTo(cursorLane, cursorStep + 1)
            else if (event.key === Qt.Key_Up)
                moveTo(cursorLane - 1, cursorStep)
            else if (event.key === Qt.Key_Down)
                moveTo(cursorLane + 1, cursorStep)
            else if (event.key === Qt.Key_Home)
                moveTo(cursorLane, -1)
            else if (event.key === Qt.Key_End)
                moveTo(cursorLane, area.steps - 1)
            else if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                activate()
            else
                return
            event.accepted = true
        }

        Repeater {
            model: area.lanes
            delegate: Row {
                id: laneRow
                required property int index
                readonly property int lane: index
                readonly property bool locked: area.locks[lane] === true
                spacing: 0
                height: area.theme.laneHeight

                // The lock: a padlock drawn in two parts, ochre when on.
                Item {
                    id: lock
                    objectName: "knockLock:" + laneRow.lane
                    width: grid.lockWidth
                    height: parent.height
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: "Keep " + area.laneName(laneRow.lane) + " when rolling"
                    Accessible.checked: laneRow.locked
                    readonly property bool cursor: grid.cursorLane === laneRow.lane && grid.cursorStep < 0
                    readonly property color ink: laneRow.locked ? area.theme.body
                        : (cursor && grid.activeFocus ? area.theme.ink : area.theme.inkFaint)
                    // The padlock: a ring for the shackle over the body.
                    readonly property real shackleX: 4
                    readonly property real shackleY: 3
                    readonly property real shackleSize: 8
                    readonly property real bodyX: 2
                    readonly property real bodyY: 8
                    readonly property real bodyWidth: 12
                    readonly property real bodyHeight: 8
                    Item {
                        anchors.centerIn: parent
                        width: 2 * lock.bodyX + lock.bodyWidth
                        height: lock.bodyY + lock.bodyHeight
                        Rectangle {
                            x: lock.shackleX
                            y: lock.shackleY
                            width: lock.shackleSize
                            height: lock.shackleSize
                            radius: lock.shackleSize / 2
                            color: "transparent"
                            border.color: lock.ink
                            border.width: area.theme.lockStroke
                        }
                        Rectangle {
                            x: lock.bodyX
                            y: lock.bodyY
                            width: lock.bodyWidth
                            height: lock.bodyHeight
                            radius: area.theme.radiusXs
                            color: laneRow.locked ? lock.ink : area.theme.panel
                            border.color: lock.ink
                        }
                    }
                    CursorRing { visible: lock.cursor && grid.activeFocus }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            grid.moveTo(laneRow.lane, -1)
                            grid.forceActiveFocus()
                            area.patterns.set_locked(laneRow.lane, !laneRow.locked)
                        }
                        Shell.Tip { text: laneRow.locked ? "Locked: kept when rolling" : "Lock this lane" }
                    }
                }
                Text {
                    width: grid.nameWidth + grid.nameGap
                    leftPadding: area.theme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    text: area.laneName(laneRow.lane)
                    color: area.theme.inkDim
                    font.family: area.theme.sans
                    font.pixelSize: area.theme.typeSmall
                    elide: Text.ElideRight
                    readonly property bool tipShowsWhole: true
                    Accessible.name: text
                    Shell.Tip {}
                }
                Row {
                    spacing: 0
                    Repeater {
                        model: area.steps
                        delegate: Item {
                            id: cell
                            required property int index
                            readonly property int step: index
                            readonly property int kind: area.cells[laneRow.lane * area.steps + step] !== undefined
                                ? area.cells[laneRow.lane * area.steps + step] : 0
                            readonly property bool beat: step % area.stepsPerBeat === 0
                            objectName: "knockCell:" + laneRow.lane + ":" + step
                            width: grid.cellWidth
                            height: laneRow.height
                            Accessible.role: Accessible.Button
                            Accessible.name: area.laneName(laneRow.lane) + " step " + (step + 1)
                            Accessible.description: area.ready ? area.patterns.cell_names[kind] : ""
                            // The chance hatch's stripes, their angle and how far
                            // they reach (in cell heights); a ratchet's two strokes,
                            // each this fraction of the cell.
                            readonly property int hatchCount: 3
                            readonly property real hatchAngle: 45
                            readonly property real hatchReach: 3
                            readonly property real ratchetStroke: 0.42
                            readonly property real ratchetSecondX: 0.58
                            Rectangle {
                                id: face
                                x: area.theme.stepGap / 2
                                y: area.theme.laneGap / 2
                                width: cell.width - area.theme.stepGap
                                height: cell.height - area.theme.laneGap
                                radius: area.theme.radiusMd
                                clip: true
                                // off, hit, accent, ghost, chance, ratchet
                                color: cell.kind === 0 ? (cell.beat ? area.theme.line : area.theme.panel2)
                                    : cell.kind === 4 || cell.kind === 5 ? area.theme.panel2 : area.theme.strike
                                opacity: cell.kind === 1 ? area.theme.cellHitOpacity
                                    : cell.kind === 3 ? area.theme.cellGhostOpacity : 1
                                border.color: cell.kind === 0 ? area.theme.line : area.theme.strike
                                // Chance: a hatch.
                                Repeater {
                                    model: cell.kind === 4 ? cell.hatchCount : 0
                                    delegate: Rectangle {
                                        required property int index
                                        x: index * face.width / cell.hatchCount
                                        y: -face.height
                                        width: area.theme.strokeBold
                                        height: face.height * cell.hatchReach
                                        rotation: cell.hatchAngle
                                        color: area.theme.strike
                                    }
                                }
                                // Ratchet: two strokes with a gap between.
                                Rectangle {
                                    visible: cell.kind === 5
                                    width: face.width * cell.ratchetStroke
                                    height: face.height
                                    color: area.theme.strike
                                }
                                Rectangle {
                                    visible: cell.kind === 5
                                    x: face.width * cell.ratchetSecondX
                                    width: face.width * cell.ratchetStroke
                                    height: face.height
                                    color: area.theme.strike
                                }
                            }
                            CursorRing {
                                visible: grid.activeFocus && grid.cursorLane === laneRow.lane
                                    && grid.cursorStep === cell.step
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    grid.moveTo(laneRow.lane, cell.step)
                                    grid.forceActiveFocus()
                                    area.patterns.cycle_cell(laneRow.lane, cell.step)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Text {
        objectName: "knockPatternStatus"
        y: grid.y + grid.height + area.theme.spaceMd
        width: parent.width
        text: {
            if (!area.ready || area.revision < 0)
                return ""
            if (area.patterns.message !== "")
                return area.patterns.message
            return "Click a cell to cycle it; lock lanes to keep them. Write to clip writes "
                + area.patterns.written_bars + (area.patterns.written_bars > 1 ? " bars" : " bar")
                + (area.patterns.evolve_bars > 0 ? ", evolving every four." : ".")
        }
        color: area.theme.inkFaint
        font.family: area.theme.sans
        font.pixelSize: area.theme.typeBody
        elide: Text.ElideRight
    }
}
