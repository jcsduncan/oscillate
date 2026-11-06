pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The browser: categories, search, tag chips, a card per preset, and a
// preview pane. Double-click a card for its primary action (a new track for
// an instrument or sound, the end of the selected chain for an effect);
// drag a card onto a track or between devices (payload: BrowserCatalogue).
// Wide layouts show categories | results | preview; narrow ones stack them.
//
// Kept for callers of the earlier browser: showInstruments, showBuiltIns,
// filteredNativeEntries (cards keyed by engine id, grouped), the VST3
// eligibility properties, and the object names its tests address.
Rectangle {
    id: browser
    Reveal { target: browser }
    required property var bridge
    required property var selectedTrack
    signal closeRequested()

    readonly property color panel: Theme.browserFill
    readonly property color raised: Theme.panelRaised
    readonly property color line: Theme.fieldEdge
    readonly property color textMain: Theme.textMain
    readonly property color textDim: Theme.textDim
    readonly property color accent: Theme.accent
    readonly property bool wide: width >= wideWidth

    // Layout: categories at the left when wide, results, then the preview.
    readonly property int wideWidth: 980
    readonly property int categoryWidth: 200
    readonly property int columnTop: 18
    readonly property int categoryRowHeight: 36
    readonly property int categoryDotX: 10
    readonly property int categoryDotSize: 8
    readonly property int categoryTextX: 28
    readonly property int categoryCountRight: 10
    readonly property int headingBottomPadding: 10
    readonly property int resultsInset: 20
    readonly property int targetY: 44
    readonly property int targetRightRoom: 100
    readonly property int closeRight: 48
    readonly property int closeY: 16
    readonly property int closeSize: 32
    readonly property int narrowCategoriesY: 66
    readonly property int chipHeight: 26
    readonly property int categoryChipPadding: 20
    readonly property int tagChipPadding: 22
    readonly property int wideSearchY: 70
    readonly property int searchGap: 10
    readonly property int searchHeight: 40
    readonly property real searchRadius: 10
    readonly property int searchTextX: 36
    readonly property int searchIconX: 13
    readonly property int gridInset: 14
    readonly property int cardTargetWidth: 250
    readonly property int cardCellHeight: 140
    readonly property int cardCellPadding: 6
    readonly property int cardActionRight: 10
    readonly property int cardActionPadding: 20
    readonly property int cardActionHeight: 24
    readonly property int kindToggleWidth: 150
    readonly property int scanControlsHeight: 30
    readonly property int refreshX: 160
    readonly property int pathY: 40
    readonly property int pathHeight: 34
    readonly property int pathTextX: 10
    readonly property int pathButtonRoom: 80
    readonly property int addPathWidth: 70
    readonly property int statusY: 82
    readonly property int statusHeight: 32
    readonly property int pluginListY: 120
    readonly property int pluginRowHeight: 76
    readonly property real pluginRowRadius: 10
    readonly property int pluginChipX: 12
    readonly property int pluginChipY: 18
    readonly property int pluginChipSize: 34
    readonly property real pluginChipRadius: 9
    readonly property int pluginTextX: 58
    readonly property int pluginTextRightRoom: 200
    readonly property int pluginNameY: 12
    readonly property int pluginVendorY: 32
    readonly property int pluginStateY: 50
    readonly property int dragHandleRight: 131
    readonly property int dragHandleY: 19
    readonly property int dragHandleWidth: 36
    readonly property int dragHandleHeight: 38
    readonly property int entryActionRight: 88
    readonly property int entryActionY: 18
    readonly property int entryActionWidth: 76
    readonly property int entryActionHeight: 40
    readonly property int footerHeight: 36
    readonly property int previewMaxWidth: 380
    readonly property real previewWidthShare: 0.3
    readonly property int previewNarrowHeight: 172
    readonly property int previewInset: 22
    readonly property int bigChipWideY: 24
    readonly property int bigChipWideSize: 56
    readonly property int bigChipSize: 40
    readonly property int previewTitleGap: 14
    readonly property int previewTitleRightRoom: 60
    readonly property int favouriteSize: 32
    readonly property real aboutLineHeight: 1.3
    readonly property int previewBoxHeight: 64
    readonly property real previewBoxRadius: 12
    readonly property int previewButtonX: 10
    readonly property int previewButtonSize: 44
    readonly property int previewHintX: 66
    readonly property int previewHintRightRoom: 76
    readonly property int presetsGap: 18
    readonly property int maxPresetSiblings: 8
    readonly property int presetRowHeight: 36
    readonly property int actionsWideBottom: 22
    readonly property int actionsBottom: 14
    readonly property int primaryWideHeight: 46
    readonly property int primaryHeight: 38
    readonly property int secondaryWideHeight: 40
    readonly property int secondaryHeight: 32
    readonly property real actionRadius: 10
    readonly property int actionTextPadding: 20

    // The shell opens this on VST3 plugins unless it asks for built-ins
    // (openPluginBrowser(true)); the docked browser starts on built-ins.
    BrowserCatalogue {
        id: catalogue
        bridge: browser.bridge
        selectedTrack: browser.selectedTrack
        showBuiltIns: false
        query: searchInput.text
    }
    property alias showInstruments: catalogue.showInstruments
    property alias showBuiltIns: catalogue.showBuiltIns
    readonly property var nativeEntries: catalogue.nativeCards
    readonly property var filteredNativeEntries: catalogue.nativeResults
    readonly property var selectedEffects: catalogue.selectedEffects
    readonly property string selectedPathId: catalogue.selectedPathId
    readonly property int selectedTrackIndex: catalogue.selectedTrackIndex
    readonly property bool nativeInstrumentEligible: catalogue.nativeInstrumentEligible
    readonly property bool nativeEffectEligible: catalogue.nativeEffectEligible
    readonly property bool newInstrumentEligible: catalogue.newInstrumentEligible
    readonly property bool trackEligible: catalogue.vst3TrackEligible(showInstruments)
    readonly property string targetKind: showInstruments ? "instrument" : "effect"
    readonly property string savedInsertState: {
        const state = bridge.playbackState
        if (state.pluginFault) return "fault · see transport"
        if (state.application === "failed") return bridge.playbackRevisionInactive
            ? "saved, not yet audible · graph failed; see transport" : "unavailable · see transport"
        if (bridge.routedGraphCurrent) return "active"
        if (bridge.playbackRevisionInactive) return "saved, not yet audible · see transport"
        return selectedTrack && (selectedTrack.vst3Plugins || []).length > 0 && selectedTrack.vst3Plugins[0].missing
            ? "missing" : "unverified until Play"
    }
    readonly property var filteredEntries: bridge.vst3Browser.vst3BrowserEntries.filter(entry => {
        const query = searchInput.text.trim().toLowerCase()
        return (entry.kind === targetKind || entry.state !== "ready")
            && (query.length === 0 || (entry.name + " " + entry.vendor + " " + entry.bundle).toLowerCase().includes(query))
    })
    function cardFor(entry) {
        return catalogue.vst3Cards.find(card => card.entry === entry
            || card.key === "vst3:" + (entry.classId || entry.bundle)) || null
    }

    color: panel
    border.color: Theme.line
    border.width: Theme.hairline
    Keys.onEscapePressed: browser.closeRequested()

    // ---- Categories -------------------------------------------------------
    Rectangle {
        id: categoryColumn
        visible: browser.wide
        width: browser.wide ? browser.categoryWidth : 0
        height: parent.height
        color: Theme.canvas
        Column {
            x: Theme.spaceLg; y: browser.columnTop; width: parent.width - 2 * Theme.spaceLg
            spacing: Theme.spaceXxs
            Text { leftPadding: Theme.spaceMd; bottomPadding: browser.headingBottomPadding; text: "BROWSER"; color: browser.textDim; font.pixelSize: Theme.typeBody; font.letterSpacing: Theme.tracking2xl }
            Repeater {
                model: catalogue.categoryNames
                delegate: Rectangle {
                    id: categoryRow
                    required property string modelData
                    readonly property bool current: catalogue.category === modelData && searchInput.text.trim().length === 0
                    objectName: "browserCategory_" + modelData.replace(/ /g, "")
                    width: parent.width; height: browser.categoryRowHeight; radius: Theme.radiusXl
                    color: current ? Theme.browserCategoryCurrent : "transparent"
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData + ", " + catalogue.count(modelData)
                    Keys.onReturnPressed: catalogue.selectCategory(modelData)
                    Keys.onSpacePressed: catalogue.selectCategory(modelData)
                    Rectangle { x: browser.categoryDotX; anchors.verticalCenter: parent.verticalCenter; width: browser.categoryDotSize; height: browser.categoryDotSize; radius: Theme.radiusMd; color: categoryRow.current ? browser.accent : Theme.lineStrong }
                    Text { x: browser.categoryTextX; anchors.verticalCenter: parent.verticalCenter; text: categoryRow.modelData; color: categoryRow.current ? browser.textMain : Theme.browserCategoryText; font.pixelSize: Theme.typeEmphasis; font.weight: categoryRow.current ? Font.DemiBold : Font.Normal }
                    Text { anchors.right: parent.right; anchors.rightMargin: browser.categoryCountRight; anchors.verticalCenter: parent.verticalCenter; text: catalogue.count(categoryRow.modelData) || "—"; color: Theme.textFaint; font.pixelSize: Theme.typeBody; font.family: Theme.monoSystem }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: catalogue.selectCategory(categoryRow.modelData)
                        Tip {}
                    }
                }
            }
        }
    }

    // ---- Results ----------------------------------------------------------
    Item {
        id: center
        x: categoryColumn.width
        width: parent.width - categoryColumn.width - previewPane.widthInLayout
        height: parent.height - (browser.wide ? 0 : previewPane.height)

        Text {
            x: browser.resultsInset; y: browser.columnTop
            text: browser.wide ? catalogue.category : "BROWSER"
            color: browser.textMain
            font.pixelSize: browser.wide ? Theme.typeDisplay : Theme.typeHeading
            font.bold: true
        }
        Text {
            x: browser.resultsInset; y: browser.targetY; width: parent.width - browser.targetRightRoom
            text: browser.selectedPathId === "master" ? "Target · Master"
                : "Target · " + (browser.selectedTrack && browser.selectedTrack.name ? browser.selectedTrack.name : "no track selected")
            color: browser.textDim
            font.pixelSize: Theme.typeBody
            elide: Text.ElideRight
        }
        Rectangle {
            objectName: "closeVst3Browser"
            x: parent.width - browser.closeRight; y: browser.closeY
            width: browser.closeSize; height: browser.closeSize; radius: Theme.radiusXl
            color: browser.raised
            border.color: activeFocus ? browser.accent : browser.line
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Close browser"
            Keys.onReturnPressed: browser.closeRequested()
            Keys.onSpacePressed: browser.closeRequested()
            Text { anchors.centerIn: parent; text: "×"; color: browser.textMain; font.pixelSize: Theme.largeCloseGlyphSize }
            MouseArea {
                anchors.fill: parent
                onClicked: browser.closeRequested()
                Tip {}
            }
        }
        // Narrow layouts pick categories from a chip row.
        Flow {
            id: narrowCategories
            visible: !browser.wide
            x: browser.resultsInset; y: browser.narrowCategoriesY; width: parent.width - 2 * browser.resultsInset
            spacing: Theme.spaceSm
            Repeater {
                model: browser.wide ? [] : catalogue.categoryNames
                delegate: Rectangle {
                    id: chipRow
                    required property string modelData
                    readonly property bool current: catalogue.category === modelData && searchInput.text.trim().length === 0
                    width: chipLabel.implicitWidth + browser.categoryChipPadding; height: browser.chipHeight; radius: height / 2
                    color: current ? Theme.rowHighlight : "transparent"
                    border.color: current ? Theme.chipEdgeCurrent : browser.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData
                    Keys.onReturnPressed: catalogue.selectCategory(modelData)
                    Text { id: chipLabel; anchors.centerIn: parent; text: chipRow.modelData; color: chipRow.current ? browser.textMain : browser.textDim; font.pixelSize: Theme.typeBody }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: catalogue.selectCategory(chipRow.modelData)
                        Tip {}
                    }
                }
            }
        }
        Rectangle {
            id: searchBox
            x: browser.resultsInset; y: browser.wide ? browser.wideSearchY : narrowCategories.y + narrowCategories.height + browser.searchGap
            width: parent.width - 2 * browser.resultsInset; height: browser.searchHeight
            radius: browser.searchRadius; color: browser.raised; border.color: searchInput.activeFocus ? browser.accent : browser.line
            TextInput {
                id: searchInput
                objectName: "vst3BrowserSearch"
                anchors.fill: parent
                anchors.leftMargin: browser.searchTextX
                anchors.rightMargin: Theme.spaceLg
                verticalAlignment: TextInput.AlignVCenter
                color: browser.textMain
                selectionColor: browser.accent
                font.pixelSize: Theme.typeEmphasis
                clip: true
                Accessible.name: "Search instruments, presets and effects"
                Keys.onDownPressed: (catalogue.showBuiltIns ? nativeList : pluginList).forceActiveFocus()
            }
            Text { x: browser.searchIconX; anchors.verticalCenter: parent.verticalCenter; text: "⌕"; color: browser.textDim; font.pixelSize: Theme.typeTitle }
            Text {
                x: browser.searchTextX; anchors.verticalCenter: parent.verticalCenter
                visible: searchInput.text.length === 0
                text: "Search instruments, presets and effects"
                color: browser.textDim
                font.pixelSize: Theme.typeLabel
            }
        }
        Flow {
            id: tagRow
            x: browser.resultsInset; y: searchBox.y + searchBox.height + browser.searchGap; width: parent.width - 2 * browser.resultsInset
            spacing: Theme.spaceSm
            visible: catalogue.showBuiltIns
            Repeater {
                model: catalogue.showBuiltIns ? catalogue.tags : []
                delegate: Rectangle {
                    id: tagChip
                    required property string modelData
                    readonly property bool current: catalogue.tag === modelData
                    width: tagLabel.implicitWidth + browser.tagChipPadding; height: browser.chipHeight; radius: height / 2
                    color: current ? Theme.rowHighlight : "transparent"
                    border.color: current ? Theme.chipEdgeCurrent : browser.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: "Filter " + modelData
                    Keys.onReturnPressed: catalogue.tag = modelData
                    Text { id: tagLabel; anchors.centerIn: parent; text: tagChip.modelData; color: tagChip.current ? browser.textMain : browser.textDim; font.pixelSize: Theme.typeBody }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: catalogue.tag = tagChip.modelData
                        Tip {}
                    }
                }
            }
        }

        // Built-in cards.
        GridView {
            id: nativeList
            objectName: "nativeBrowserResults"
            visible: catalogue.showBuiltIns
            x: browser.gridInset; y: tagRow.y + tagRow.height + Theme.spaceLg
            width: parent.width - 2 * browser.gridInset
            height: Math.max(0, parent.height - y - footer.height)
            clip: true
            cellWidth: width / Math.max(1, Math.floor(width / browser.cardTargetWidth))
            cellHeight: browser.cardCellHeight
            keyNavigationEnabled: true
            model: browser.filteredNativeEntries
            delegate: Item {
                id: nativeRow
                required property var modelData
                required property int index
                objectName: "nativeBrowserEntry_" + modelData.id
                width: nativeList.cellWidth
                height: nativeList.cellHeight
                BrowserCard {
                    id: nativeCard
                    x: browser.cardCellPadding; y: browser.cardCellPadding; width: parent.width - 2 * browser.cardCellPadding; height: parent.height - 2 * browser.cardCellPadding
                    card: nativeRow.modelData
                    catalogue: catalogue
                }
                Rectangle {
                    objectName: "nativeEntryAction"
                    anchors.right: nativeCard.right; anchors.rightMargin: browser.cardActionRight
                    anchors.bottom: nativeCard.bottom; anchors.bottomMargin: Theme.spaceMd
                    width: actionLabel.implicitWidth + browser.cardActionPadding; height: browser.cardActionHeight; radius: height / 2
                    enabled: catalogue.primaryEnabled(nativeRow.modelData)
                    color: enabled ? Theme.accentWash : Theme.disabledFill
                    border.color: activeFocus ? browser.accent : enabled ? Theme.browserActionEdge : browser.line
                    activeFocusOnTab: enabled
                    Accessible.role: Accessible.Button
                    Accessible.name: catalogue.primaryLabel(nativeRow.modelData)
                    Accessible.description: enabled ? "" : nativeRow.modelData.instrument
                        ? "Open a project first" : "Select a track and finish the current edit first"
                    function activate() { catalogue.activate(nativeRow.modelData) }
                    Keys.onReturnPressed: activate()
                    Keys.onSpacePressed: activate()
                    Text { id: actionLabel; anchors.centerIn: parent; text: nativeRow.modelData.instrument ? "NEW TRACK" : "ADD"; color: parent.enabled ? browser.accent : browser.textDim; font.pixelSize: Theme.typeSmall; font.bold: true; font.letterSpacing: Theme.trackingSm }
                    MouseArea {
                        anchors.fill: parent
                        enabled: parent.enabled
                        onClicked: parent.activate()
                        Tip {}
                    }
                }
            }
            Text {
                anchors.centerIn: parent
                visible: nativeList.count === 0
                text: catalogue.category === "Favourites" ? "Star a sound in its preview to keep it here"
                    : catalogue.category === "Recent" ? "Sounds you add appear here"
                    : "Nothing matches this search"
                color: browser.textDim
                font.pixelSize: Theme.typeLabel
            }
        }

        // VST3 plugins: scanner controls and verified entries.
        Item {
            id: vst3Section
            visible: !catalogue.showBuiltIns
            x: browser.resultsInset; y: searchBox.y + searchBox.height + browser.searchGap
            width: parent.width - 2 * browser.resultsInset
            height: parent.height - y - footer.height
            Rectangle {
                objectName: "vst3BrowserKindToggle"
                width: browser.kindToggleWidth; height: browser.scanControlsHeight; radius: Theme.radiusXl
                color: browser.raised
                border.color: activeFocus ? browser.accent : browser.line
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: browser.showInstruments ? "Show effects" : "Show instruments"
                function activate() { catalogue.changingCategory = true; browser.showInstruments = !browser.showInstruments; catalogue.changingCategory = false }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Text { anchors.centerIn: parent; text: browser.showInstruments ? "INSTRUMENTS · SHOW EFFECTS" : "EFFECTS · SHOW INSTRUMENTS"; color: browser.accent; font.pixelSize: Theme.typeCaption; font.bold: true }
                MouseArea {
                    anchors.fill: parent
                    onClicked: parent.activate()
                    Tip {}
                }
            }
            Rectangle {
                objectName: "refreshVst3Browser"
                x: browser.refreshX; width: parent.width - browser.refreshX; height: browser.scanControlsHeight
                radius: Theme.radiusXl
                color: enabled ? Theme.accentWash : browser.raised
                border.color: activeFocus ? browser.accent : browser.line
                enabled: browser.bridge.vst3Browser.vst3BrowserAvailable && !browser.bridge.vst3Browser.vst3BrowserBusy
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: "Scan VST3 plugins"
                Keys.onReturnPressed: browser.bridge.vst3Browser.refresh_vst3_catalogue()
                Keys.onSpacePressed: browser.bridge.vst3Browser.refresh_vst3_catalogue()
                Text { anchors.centerIn: parent; text: browser.bridge.vst3Browser.vst3BrowserBusy ? "SCANNING…" : "SCAN / REFRESH"; color: browser.textMain; font.pixelSize: Theme.typeBody; font.bold: true }
                MouseArea {
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: browser.bridge.vst3Browser.refresh_vst3_catalogue()
                    Tip {}
                }
            }
            Rectangle {
                y: browser.pathY; width: parent.width; height: browser.pathHeight
                radius: Theme.radiusXl; color: browser.raised; border.color: extraPath.activeFocus ? browser.accent : browser.line
                TextInput {
                    id: extraPath
                    objectName: "vst3AdditionalPath"
                    anchors.fill: parent
                    anchors.leftMargin: browser.pathTextX
                    anchors.rightMargin: browser.pathButtonRoom
                    verticalAlignment: TextInput.AlignVCenter
                    color: browser.textMain
                    font.pixelSize: Theme.typeBody
                    clip: true
                    Accessible.name: "Additional VST3 bundle or folder path"
                    onAccepted: browser.bridge.vst3Browser.add_vst3_search_path(text)
                }
                Text {
                    x: browser.pathTextX; anchors.verticalCenter: parent.verticalCenter
                    visible: extraPath.text.length === 0
                    text: "Additional bundle or folder path"
                    color: browser.textDim
                    font.pixelSize: Theme.typeBody
                }
                Rectangle {
                    objectName: "addVst3Path"
                    anchors.right: parent.right
                    width: browser.addPathWidth; height: parent.height
                    radius: Theme.radiusXl
                    color: Theme.accentWash
                    enabled: browser.bridge.vst3Browser.vst3BrowserAvailable && !browser.bridge.vst3Browser.vst3BrowserBusy
                    activeFocusOnTab: enabled
                    Accessible.role: Accessible.Button
                    Accessible.name: "Scan additional VST3 path"
                    Keys.onReturnPressed: browser.bridge.vst3Browser.add_vst3_search_path(extraPath.text)
                    Keys.onSpacePressed: browser.bridge.vst3Browser.add_vst3_search_path(extraPath.text)
                    Text { anchors.centerIn: parent; text: "ADD"; color: browser.textMain; font.pixelSize: Theme.typeSmall; font.bold: true }
                    MouseArea {
                        anchors.fill: parent
                        enabled: parent.enabled
                        onClicked: browser.bridge.vst3Browser.add_vst3_search_path(extraPath.text)
                        Tip {}
                    }
                }
            }
            Text {
                objectName: "vst3BrowserStatus"
                y: browser.statusY; width: parent.width; height: browser.statusHeight
                text: browser.bridge.vst3Browser.vst3BrowserStatus
                color: browser.textDim
                font.pixelSize: Theme.typeBody
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
            }
            ListView {
                id: pluginList
                objectName: "vst3BrowserResults"
                y: browser.pluginListY
                width: parent.width
                height: Math.max(0, parent.height - y)
                clip: true
                spacing: Theme.spaceSm
                keyNavigationEnabled: true
                model: browser.filteredEntries
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    readonly property var entry: modelData
                    readonly property var card: browser.cardFor(modelData)
                    width: pluginList.width
                    height: browser.pluginRowHeight
                    radius: browser.pluginRowRadius
                    color: card && catalogue.selectedKey === card.key ? Theme.selection : Theme.browserCardFill
                    border.color: Theme.line
                    Rectangle { x: browser.pluginChipX; y: browser.pluginChipY; width: browser.pluginChipSize; height: browser.pluginChipSize; radius: browser.pluginChipRadius; color: row.entry.kind === "instrument" ? Theme.browserVst3InstrumentColor : Theme.browserVst3EffectColor
                        Text { anchors.centerIn: parent; text: (row.entry.name || "?").charAt(0); color: Theme.canvas; font.bold: true; font.pixelSize: Theme.typeSubtitle } }
                    Text { x: browser.pluginTextX; y: browser.pluginNameY; width: parent.width - browser.pluginTextRightRoom; text: row.entry.name; color: browser.textMain; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold; elide: Text.ElideRight }
                    Text { x: browser.pluginTextX; y: browser.pluginVendorY; width: parent.width - browser.pluginTextRightRoom; text: row.entry.vendor || row.entry.bundle; color: browser.textDim; font.pixelSize: Theme.typeBody; elide: Text.ElideMiddle }
                    Text { x: browser.pluginTextX; y: browser.pluginStateY; width: parent.width - browser.pluginTextRightRoom; text: row.entry.state + (row.entry.diagnostic ? " · " + row.entry.diagnostic : ""); color: row.entry.state === "ready" ? browser.accent : Theme.pluginStateWarning; font.pixelSize: Theme.typeSmall; elide: Text.ElideRight }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (row.card) catalogue.selectedKey = row.card.key
                        onDoubleClicked: if (row.card) catalogue.activate(row.card)
                    }
                    Rectangle {
                        id: dragHandle
                        objectName: "vst3InstrumentDragHandle"
                        visible: browser.showInstruments && browser.newInstrumentEligible
                            && row.entry.state === "ready" && row.entry.insertable
                        x: parent.width - browser.dragHandleRight; y: browser.dragHandleY
                        width: browser.dragHandleWidth; height: browser.dragHandleHeight; radius: Theme.radiusXl
                        color: Theme.accentWash
                        border.color: browser.line
                        Drag.active: dragMouse.drag.active
                        Drag.dragType: Drag.Automatic
                        Drag.supportedActions: Qt.CopyAction
                        Drag.mimeData: row.card ? {
                            "application/x-oscillate-vst3-instrument": JSON.stringify({ index: row.entry.index, classId: row.entry.classId }),
                            "application/x-oscillate-device": catalogue.dragPayload(row.card)
                        } : { "application/x-oscillate-vst3-instrument": JSON.stringify({ index: row.entry.index, classId: row.entry.classId }) }
                        Text { anchors.centerIn: parent; text: "⠿"; color: browser.accent; font.pixelSize: Theme.dragGlyphSize }
                        MouseArea {
                            id: dragMouse
                            anchors.fill: parent
                            drag.target: dragHandle
                            cursorShape: Qt.OpenHandCursor
                            onReleased: { dragHandle.x = row.width - browser.dragHandleRight; dragHandle.y = browser.dragHandleY }
                        }
                    }
                    Rectangle {
                        objectName: "vst3EntryAction"
                        x: parent.width - browser.entryActionRight; y: browser.entryActionY; width: browser.entryActionWidth; height: browser.entryActionHeight
                        radius: Theme.radiusXl
                        enabled: !browser.bridge.vst3Browser.vst3BrowserBusy && (row.entry.state !== "ready" || (row.entry.insertable && (browser.trackEligible || browser.showInstruments && browser.newInstrumentEligible)))
                        color: enabled ? Theme.accentWash : Theme.disabledFill
                        border.color: activeFocus ? browser.accent : browser.line
                        activeFocusOnTab: enabled
                        Accessible.role: Accessible.Button
                        Accessible.name: row.entry.state !== "ready" ? "Retry " + row.entry.name
                            : row.entry.insertable ? "Insert " + row.entry.name
                            : "Unavailable: " + row.entry.name
                        function activate() {
                            if (row.entry.state !== "ready") {
                                browser.bridge.vst3Browser.retry_vst3_bundle(row.entry.bundle)
                            } else if (browser.targetKind === "instrument") {
                                if (browser.trackEligible)
                                    browser.bridge.vst3Browser.insert_vst3_instrument(row.entry.index, row.entry.classId, browser.selectedTrack.id)
                                else
                                    browser.bridge.vst3Browser.create_vst3_instrument_track(row.entry.index, row.entry.classId)
                            } else {
                                browser.bridge.vst3Browser.insert_vst3_effect(row.entry.index, row.entry.classId, browser.selectedTrack.id)
                            }
                            if (row.card) browser.bridge.note_browser_use(row.card.key)
                        }
                        Keys.onReturnPressed: activate()
                        Keys.onSpacePressed: activate()
                        Text { anchors.centerIn: parent; text: row.entry.state !== "ready" ? "RETRY" : !row.entry.insertable ? "UNAVAIL" : browser.showInstruments && !browser.trackEligible ? "NEW" : "INSERT"; color: browser.textMain; font.pixelSize: Theme.typeSmall; font.bold: true }
                        MouseArea {
                            anchors.fill: parent
                            enabled: parent.enabled
                            onClicked: parent.activate()
                            Tip {}
                        }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: pluginList.count === 0
                    text: browser.bridge.vst3Browser.vst3BrowserBusy ? "Scanning plugins…" : "No compatible plugins match this search"
                    color: browser.textDim
                    font.pixelSize: Theme.typeLabel
                }
            }
        }

        Rectangle {
            id: footer
            anchors.bottom: parent.bottom
            width: parent.width; height: browser.footerHeight
            color: "transparent"
            Rectangle { width: parent.width; height: Theme.hairline; color: Theme.rowDivider }
            Text {
                x: browser.resultsInset; anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * browser.resultsInset
                text: "Double-click creates a track with an instrument, or adds an effect to the target  ·  Drag onto a track or between devices  ·  Space previews"
                color: browser.textDim
                font.pixelSize: Theme.typeBody
                elide: Text.ElideRight
            }
        }
    }

    // ---- Preview ----------------------------------------------------------
    Rectangle {
        id: previewPane
        readonly property var card: catalogue.selectedCard
        readonly property real widthInLayout: browser.wide ? width : 0
        objectName: "browserPreviewPane"
        x: browser.wide ? parent.width - width : 0
        y: browser.wide ? 0 : parent.height - height
        width: browser.wide ? Math.min(browser.previewMaxWidth, parent.width * browser.previewWidthShare) : parent.width
        height: browser.wide ? parent.height : browser.previewNarrowHeight
        color: Theme.previewPaneFill
        border.color: Theme.line
        visible: card !== null

        Rectangle {
            id: bigChip
            x: browser.previewInset; y: browser.wide ? browser.bigChipWideY : Theme.spaceXl
            width: browser.wide ? browser.bigChipWideSize : browser.bigChipSize; height: width; radius: width / 4
            color: previewPane.card ? previewPane.card.color : browser.raised
            Text { anchors.centerIn: parent; text: previewPane.card ? previewPane.card.engineName.charAt(0) : ""; color: Theme.canvas; font.pixelSize: browser.wide ? Theme.previewChipGlyphSize : Theme.typeDisplay; font.bold: true }
        }
        Column {
            x: bigChip.x + bigChip.width + browser.previewTitleGap; y: bigChip.y + Theme.spaceXxs
            width: parent.width - x - browser.previewTitleRightRoom
            spacing: Theme.spaceXxs
            Text { width: parent.width; text: previewPane.card ? previewPane.card.presetName : ""; color: browser.textMain; font.pixelSize: browser.wide ? Theme.previewTitleSize : Theme.typeTitle; font.bold: true; elide: Text.ElideRight }
            Text { width: parent.width; text: previewPane.card ? previewPane.card.engineName + " · " + previewPane.card.kind : ""; color: browser.textDim; font.pixelSize: Theme.typeLabel; elide: Text.ElideRight }
        }
        Rectangle {
            objectName: "browserFavouriteToggle"
            x: parent.width - browser.closeRight; y: bigChip.y + Theme.spaceXs
            width: browser.favouriteSize; height: browser.favouriteSize; radius: width / 2
            color: "transparent"
            border.color: activeFocus ? browser.accent : browser.line
            activeFocusOnTab: true
            readonly property bool on: previewPane.card ? catalogue.isFavourite(previewPane.card.key) : false
            Accessible.role: Accessible.CheckBox
            Accessible.name: "Favourite"
            Accessible.checked: on
            function toggle() { if (previewPane.card) browser.bridge.toggle_browser_favourite(previewPane.card.key) }
            Keys.onReturnPressed: toggle()
            Keys.onSpacePressed: toggle()
            Text { anchors.centerIn: parent; text: parent.on ? "★" : "☆"; color: parent.on ? Theme.amber : browser.textDim; font.pixelSize: Theme.typeTitle }
            MouseArea {
                anchors.fill: parent
                onClicked: parent.toggle()
                Tip {}
            }
        }
        Text {
            id: aboutText
            visible: browser.wide
            x: browser.previewInset; y: bigChip.y + bigChip.height + Theme.spaceXl; width: parent.width - 2 * browser.previewInset
            text: previewPane.card ? previewPane.card.about : ""
            color: Theme.textSoft
            font.pixelSize: Theme.typeEmphasis
            lineHeight: browser.aboutLineHeight
            wrapMode: Text.WordWrap
        }
        Rectangle {
            id: previewBox
            visible: browser.wide
            x: browser.previewInset; y: aboutText.y + aboutText.height + Theme.spaceXl; width: parent.width - 2 * browser.previewInset; height: browser.previewBoxHeight
            radius: browser.previewBoxRadius; color: Theme.canvas; border.color: Theme.line
            Rectangle {
                objectName: "browserPreviewButton"
                x: browser.previewButtonX; anchors.verticalCenter: parent.verticalCenter
                width: browser.previewButtonSize; height: browser.previewButtonSize; radius: width / 2
                enabled: catalogue.canPreview(previewPane.card)
                color: enabled ? (previewPane.card ? previewPane.card.color : browser.accent) : Theme.disabledFill
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: catalogue.previewing ? "Stop preview" : "Preview"
                Keys.onReturnPressed: catalogue.togglePreview(previewPane.card)
                Keys.onSpacePressed: catalogue.togglePreview(previewPane.card)
                Text { anchors.centerIn: parent; text: catalogue.previewing ? "■" : "▶"; color: Theme.canvas; font.pixelSize: Theme.typeSubtitle }
                MouseArea {
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: catalogue.togglePreview(previewPane.card)
                    Tip {}
                }
            }
            Text {
                x: browser.previewHintX; anchors.verticalCenter: parent.verticalCenter; width: parent.width - browser.previewHintRightRoom
                text: catalogue.canPreview(previewPane.card) ? "Plays C3 dry, as saved in the preset"
                    : "Previews play built-in instruments"
                color: browser.textDim
                font.pixelSize: Theme.typeLabel
                wrapMode: Text.WordWrap
            }
        }
        Column {
            visible: browser.wide && catalogue.presetCards.length > 0
            x: browser.previewInset; y: previewBox.y + previewBox.height + browser.presetsGap; width: parent.width - 2 * browser.previewInset
            spacing: Theme.spaceSm
            Text { text: "START FROM A PRESET"; color: browser.textDim; font.pixelSize: Theme.typeBody; font.letterSpacing: Theme.trackingXl }
            Repeater {
                model: browser.wide ? catalogue.presetCards.slice(0, browser.maxPresetSiblings) : []
                delegate: Rectangle {
                    id: sibling
                    required property var modelData
                    width: parent.width; height: browser.presetRowHeight; radius: Theme.radiusXl
                    color: Theme.presetRowFill; border.color: activeFocus ? browser.accent : Theme.line
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.presetName
                    Keys.onReturnPressed: catalogue.activate(modelData)
                    Text { x: Theme.spaceLg; anchors.verticalCenter: parent.verticalCenter; text: sibling.modelData.presetName; color: browser.textMain; font.pixelSize: Theme.typeEmphasis }
                    Text { anchors.right: parent.right; anchors.rightMargin: Theme.spaceLg; anchors.verticalCenter: parent.verticalCenter; text: sibling.modelData.tag; color: browser.textDim; font.pixelSize: Theme.typeBody }
                    MouseArea { anchors.fill: parent; onDoubleClicked: catalogue.activate(sibling.modelData) }
                }
            }
        }
        Column {
            x: browser.previewInset
            y: parent.height - height - (browser.wide ? browser.actionsWideBottom : browser.actionsBottom)
            width: parent.width - 2 * browser.previewInset
            spacing: Theme.spaceMd
            Rectangle {
                objectName: "browserPrimaryAction"
                width: parent.width; height: browser.wide ? browser.primaryWideHeight : browser.primaryHeight; radius: browser.actionRadius
                enabled: catalogue.primaryEnabled(previewPane.card)
                color: enabled ? browser.accent : Theme.disabledFill
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: catalogue.primaryLabel(previewPane.card)
                Keys.onReturnPressed: catalogue.activate(previewPane.card)
                Keys.onSpacePressed: catalogue.activate(previewPane.card)
                Text { anchors.centerIn: parent; width: parent.width - browser.actionTextPadding; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; text: catalogue.primaryLabel(previewPane.card); color: parent.enabled ? Theme.canvas : browser.textDim; font.pixelSize: Theme.typeEmphasis; font.bold: true }
                MouseArea {
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: catalogue.activate(previewPane.card)
                    Tip {}
                }
            }
            Rectangle {
                objectName: "browserSecondaryAction"
                visible: catalogue.secondaryLabel(previewPane.card).length > 0
                width: parent.width; height: browser.wide ? browser.secondaryWideHeight : browser.secondaryHeight; radius: browser.actionRadius
                enabled: catalogue.secondaryEnabled(previewPane.card)
                color: "transparent"
                border.color: activeFocus ? browser.accent : enabled ? Theme.lineStrong : Theme.line
                activeFocusOnTab: enabled
                Accessible.role: Accessible.Button
                Accessible.name: catalogue.secondaryLabel(previewPane.card)
                Keys.onReturnPressed: catalogue.activateSecondary(previewPane.card)
                Keys.onSpacePressed: catalogue.activateSecondary(previewPane.card)
                Text { anchors.centerIn: parent; text: catalogue.secondaryLabel(previewPane.card); color: parent.enabled ? browser.textMain : browser.textDim; font.pixelSize: Theme.typeLabel; font.weight: Font.DemiBold }
                MouseArea {
                    anchors.fill: parent
                    enabled: parent.enabled
                    onClicked: catalogue.activateSecondary(previewPane.card)
                    Tip {}
                }
            }
        }
    }
}
