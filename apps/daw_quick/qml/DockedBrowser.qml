pragma ComponentBehavior: Bound
import QtQuick
import "shell"

// The browser docked at the left of the arrangement: search, category
// chips and a compact list. Double-click a sound for a new track, or an
// effect to append it to the selected track's chain; drag either onto a
// track or between devices (payload: BrowserCatalogue.dragPayload, MIME
// application/x-oscillate-device). Right-click a sound for both of its
// actions. Self-contained: place it and give it the bridge and the selected
// track.
Rectangle {
    id: docked
    required property var bridge
    property var selectedTrack: null
    signal expandRequested()

    objectName: "dockedBrowser"
    implicitWidth: Theme.browserDockWidth
    color: Theme.browserFill
    border.color: Theme.line
    border.width: Theme.hairline

    readonly property var tabs: ["Instruments", "Effects", "Utilities"]
    readonly property var moreChips: ["Drums", "Samples", "Racks", "VST3 plugins", "Favourites"]

    BrowserCatalogue {
        id: catalogue
        bridge: docked.bridge
        selectedTrack: docked.selectedTrack
        query: dockedSearch.text
    }
    readonly property var cards: catalogue.cards
    // A sample previewing in time with the song stops when the song stops
    // or pauses (Stop also silences previews when nothing plays).
    readonly property bool songPlaying: bridge.playbackActive && !bridge.playbackPaused
    onSongPlayingChanged: {
        if (!songPlaying && bridge.browserPreview)
            bridge.browserPreview.stop()
    }

    // The right-click menu: the card's primary action (what a double-click
    // does) and its secondary one (swap the selected track's instrument, or
    // put the effect on the master).
    property var menuCard: null
    property real menuX: 0
    property real menuY: 0
    function openMenu(card, x, y) {
        menuCard = card
        menuX = Math.max(Theme.spaceXs, Math.min(width - cardMenu.width - Theme.spaceXs, x))
        menuY = Math.max(Theme.spaceXs, Math.min(height - cardMenu.height - Theme.spaceXs, y))
    }

    Rectangle {
        id: searchField
        x: Theme.spaceLg; y: Theme.spaceLg; width: parent.width - 2 * Theme.spaceLg - previewToggle.width - Theme.spaceSm; height: Theme.browserSearchHeight; radius: Theme.radiusXl
        color: Theme.panelRaised
        border.color: dockedSearch.activeFocus ? Theme.accent : Theme.fieldEdge
        Text { x: Theme.searchIconX; anchors.verticalCenter: parent.verticalCenter; text: "⌕"; color: Theme.textDim; font.pixelSize: Theme.typeSubtitle }
        TextInput {
            id: dockedSearch
            objectName: "dockedBrowserSearch"
            anchors.fill: parent
            anchors.leftMargin: Theme.searchTextInset
            anchors.rightMargin: Theme.spaceMd
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.textMain
            selectionColor: Theme.accent
            font.pixelSize: Theme.typeLabel
            clip: true
            Accessible.name: "Search sounds and effects"
            Keys.onDownPressed: dockedList.forceActiveFocus()
            // Enter runs the best match's double-click action.
            Keys.onReturnPressed: if (docked.cards.length > 0) catalogue.activate(docked.cards[0])
            Keys.onEscapePressed: { text = ""; focus = false }
        }
        Text {
            x: Theme.searchTextInset; anchors.verticalCenter: parent.verticalCenter
            visible: dockedSearch.text.length === 0
            text: "Search: compressor, reverb, kick…"
            color: Theme.textFaint
            font.pixelSize: Theme.typeLabel
        }
    }
    // Auto-preview: selecting a sound plays it (saved per user).
    Rectangle {
        id: previewToggle
        objectName: "dockedBrowserAutoPreview"
        readonly property bool active: catalogue.autoPreview
        x: parent.width - Theme.spaceLg - width; y: Theme.spaceLg; width: Theme.browserSearchHeight; height: Theme.browserSearchHeight; radius: Theme.radiusXl
        color: active ? Theme.accentWash : previewTip.hovered ? Theme.browserToggleHover : Theme.panelRaised
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.fast } }
        Tip {
            id: previewTip
            text: previewToggle.active ? "Previewing: sounds play as you select them. Click to stop"
                                       : "Preview sounds as you select them"
            shortcut: "Space plays one"
        }
        border.color: activeFocus ? Theme.accent : active ? Theme.accentWashEdge : Theme.fieldEdge
        activeFocusOnTab: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: "Preview sounds as you select them"
        Accessible.checked: active
        function toggle() {
            if (docked.bridge.browserPreview)
                docked.bridge.browserPreview.autoPreview = !previewToggle.active
        }
        Keys.onReturnPressed: toggle()
        Keys.onSpacePressed: toggle()
        Text {
            anchors.centerIn: parent
            text: "♪"
            color: previewToggle.active ? Theme.accent : Theme.textDim
            font.pixelSize: Theme.typeSubtitle
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: previewToggle.toggle()
        }
    }
    component CategoryButton: Rectangle {
        id: chip
        required property string modelData
        property bool primary: true
        readonly property bool current: catalogue.category === modelData && dockedSearch.text.trim().length === 0
        objectName: "dockedBrowserCategory_" + modelData.replace(/ /g, "")
        height: primary ? Theme.browserTabHeight : Theme.browserChipHeight
        radius: Theme.radiusLg
        color: current ? Theme.rowHighlight : chipTip.hovered ? Theme.browserChipHover : "transparent"
        Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
        Tip {
            id: chipTip
            text: ({ "Instruments": "Built-in instruments and their presets",
                     "Effects": "Built-in effects, MIDI effects and their presets",
                     "Utilities": "Gain, metering and analysis tools",
                     "Drums": "Drum kits and kicks",
                     "Samples": "Your sample folder; loops preview in time with the song",
                     "Racks": "Saved racks: a track's devices and macros in one",
                     "VST3 plugins": "Scanned VST3 plug-ins",
                     "Favourites": "Sounds you starred in the full browser" })[chip.modelData] || chip.modelData
        }
        border.color: current ? Theme.lineStrong : primary ? Theme.line : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: modelData
        Keys.onReturnPressed: catalogue.selectCategory(modelData)
        Keys.onSpacePressed: catalogue.selectCategory(modelData)
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.modelData === "VST3 plugins" ? "VST3" : chip.modelData === "Favourites" ? "★ Favs" : chip.modelData
            color: chip.current ? Theme.textMain : Theme.textDim
            font.pixelSize: chip.primary ? Theme.typeBody : Theme.typeSmall
            font.weight: chip.primary ? Font.DemiBold : Font.Medium
        }
        MouseArea { anchors.fill: parent; onClicked: catalogue.selectCategory(chip.modelData) }
    }
    Row {
        id: tabRow
        x: Theme.spaceLg; y: searchField.y + searchField.height + Theme.spaceMd
        spacing: Theme.spaceXs
        Repeater {
            model: docked.tabs
            delegate: CategoryButton {
                width: (docked.width - 2 * Theme.spaceLg - (docked.tabs.length - 1) * tabRow.spacing) / docked.tabs.length
            }
        }
    }
    Row {
        id: chipFlow
        x: Theme.spaceMd; y: tabRow.y + tabRow.height + Theme.spaceXs
        spacing: 0
        Repeater {
            model: docked.moreChips
            delegate: CategoryButton {
                primary: false
                width: implicitWidth
                implicitWidth: (docked.width - 2 * Theme.spaceMd) / docked.moreChips.length
            }
        }
    }
    ListView {
        id: dockedList
        objectName: "dockedBrowserResults"
        x: Theme.spaceMd; y: chipFlow.y + chipFlow.height + Theme.spaceSm
        width: parent.width - 2 * Theme.spaceMd
        height: parent.height - y - hint.height
        clip: true
        spacing: Theme.spaceXxs
        keyNavigationEnabled: true
        model: docked.cards
        // Arrow keys select (and, with auto-preview, play) as they move.
        onCurrentIndexChanged: if (activeFocus && currentIndex >= 0 && currentIndex < docked.cards.length)
            catalogue.selectedKey = docked.cards[currentIndex].key
        // While browsing, each job gets a heading above its first card
        // ("Dynamics", "Space"); a search lists best matches first, unheaded.
        readonly property bool headed: dockedSearch.text.trim().length === 0
        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property string heading: catalogue.heading(modelData)
            readonly property bool firstOfHeading: dockedList.headed && heading.length > 0
                && (index === 0 || catalogue.heading(docked.cards[index - 1]) !== heading)
            width: dockedList.width
            height: entry.height + (firstOfHeading ? sectionLabel.height : 0)
            Text {
                id: sectionLabel
                objectName: "dockedBrowserHeading_" + row.heading.replace(/[^A-Za-z]/g, "")
                visible: row.firstOfHeading
                x: Theme.spaceMd; width: parent.width - 2 * Theme.spaceMd; height: Theme.browserHeadingHeight
                verticalAlignment: Text.AlignBottom
                bottomPadding: Theme.spaceXs
                text: row.heading.toUpperCase()
                color: Theme.textFaint
                font.pixelSize: Theme.typeSmall
                font.weight: Font.DemiBold
                font.letterSpacing: Theme.trackingXl
                Accessible.role: Accessible.Heading
                Accessible.name: row.heading
            }
            BrowserCard {
                id: entry
                objectName: "dockedBrowserEntry_" + row.modelData.key
                y: row.firstOfHeading ? sectionLabel.height : 0
                width: parent.width
                height: Theme.browserCompactCardHeight
                compact: true
                card: row.modelData
                catalogue: catalogue
                onMenuRequested: (x, y) => {
                    const point = mapToItem(docked, x, y)
                    docked.openMenu(row.modelData, point.x, point.y)
                }
            }
        }
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.spaceLg
            visible: dockedList.count === 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: catalogue.category === "Favourites" ? "Star sounds in the full browser to keep them here"
                : "Nothing matches"
            color: Theme.textFaint
            font.pixelSize: Theme.typeLabel
        }
    }
    Rectangle {
        id: hint
        anchors.bottom: parent.bottom
        // Sized to its text, with the Full browser button below it (and,
        // for samples, the folder they come from).
        readonly property bool samples: catalogue.category === "Samples" && dockedSearch.text.trim().length === 0
        width: parent.width; height: hintText.implicitHeight + Theme.browserHintButtonsHeight + (samples ? Theme.browserHintFolderHeight : 0)
        color: "transparent"
        Rectangle { width: parent.width; height: Theme.hairline; color: Theme.line }
        Text {
            id: hintText
            x: Theme.spaceLg; y: Theme.browserHintTop; width: parent.width - 2 * Theme.spaceLg
            text: hint.samples
                ? "Samples from " + (docked.bridge.browserPreview ? docked.bridge.browserPreview.sampleFolder : "")
                  + ((docked.bridge.browserPreview && docked.bridge.browserPreview.scanning) ? " (reading…)" : "")
                  + ". Loops play in time with the song. Double-click or drag for a new audio track."
                : "Double-click an instrument for a new track, or an effect to add it to the selected track. Drag onto a track to place; right-click for more."
            color: Theme.textDim
            font.pixelSize: Theme.typeBody
            wrapMode: Text.WordWrap
        }
        Rectangle {
            objectName: "dockedBrowserSampleFolder"
            visible: hint.samples
            x: Theme.spaceLg; y: parent.height - Theme.browserFolderButtonRise; width: parent.width - 2 * Theme.spaceLg; height: Theme.browserButtonHeight; radius: Theme.radiusLg
            color: Theme.panelRaised
            border.color: activeFocus ? Theme.accent : Theme.fieldEdge
            activeFocusOnTab: visible
            Accessible.role: Accessible.Button
            Accessible.name: "Choose the sample folder"
            Tip { text: "Choose the folder the Samples list reads" }
            Keys.onReturnPressed: docked.bridge.browserPreview.choose_folder()
            Keys.onSpacePressed: docked.bridge.browserPreview.choose_folder()
            Text { anchors.centerIn: parent; text: "Sample folder…"; color: Theme.textSoft; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold }
            MouseArea { anchors.fill: parent; onClicked: docked.bridge.browserPreview.choose_folder() }
        }
        Rectangle {
            objectName: "dockedBrowserExpand"
            x: Theme.spaceLg; y: parent.height - Theme.browserExpandButtonRise; width: parent.width - 2 * Theme.spaceLg; height: Theme.browserButtonHeight; radius: Theme.radiusLg
            color: Theme.panelRaised
            border.color: activeFocus ? Theme.accent : Theme.fieldEdge
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: "Open the full browser"
            Tip { text: "Open the full browser: presets, favourites and VST3 plug-ins" }
            Keys.onReturnPressed: docked.expandRequested()
            Keys.onSpacePressed: docked.expandRequested()
            Text { anchors.centerIn: parent; text: "Full browser"; color: Theme.textSoft; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold }
            MouseArea { anchors.fill: parent; onClicked: docked.expandRequested() }
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: docked.menuCard !== null
        z: 9
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: docked.menuCard = null
    }
    Rectangle {
        id: cardMenu
        objectName: "dockedBrowserMenu"
        readonly property var options: docked.menuCard ? [
            { name: "dockedBrowserMenuPrimary", label: catalogue.primaryLabel(docked.menuCard),
              enabled: catalogue.primaryEnabled(docked.menuCard), secondary: false },
            { name: "dockedBrowserMenuSecondary", label: catalogue.secondaryLabel(docked.menuCard),
              enabled: catalogue.secondaryEnabled(docked.menuCard), secondary: true }
        ].filter(option => option.label.length > 0) : []
        visible: docked.menuCard !== null
        x: docked.menuX
        y: docked.menuY
        z: 10
        width: Theme.browserMenuWidth
        height: options.length * Theme.browserMenuRowHeight + 2 * Theme.spaceXs
        radius: Theme.radiusXl
        color: Theme.panelRaised
        border.color: Theme.lineStrong
        Keys.onEscapePressed: docked.menuCard = null
        onVisibleChanged: if (visible) forceActiveFocus()
        transformOrigin: Item.TopLeft
        Reveal { target: cardMenu; fromScale: Motion.popScale; duration: Motion.fast }
        Repeater {
            model: cardMenu.options
            delegate: Rectangle {
                id: option
                required property var modelData
                required property int index
                objectName: modelData.name
                x: Theme.spaceXs
                y: Theme.spaceXs + index * Theme.browserMenuRowHeight
                width: parent.width - 2 * Theme.spaceXs
                height: Theme.browserMenuRowHeight
                radius: Theme.controlRadius
                enabled: modelData.enabled
                color: enabled && (activeFocus || optionArea.containsMouse) ? Theme.rowHighlight : "transparent"
                Behavior on color { enabled: Motion.enabled; ColorAnimation { duration: Motion.hover } }
                activeFocusOnTab: true
                Accessible.role: Accessible.MenuItem
                Accessible.name: modelData.label
                function activate() {
                    const card = docked.menuCard
                    docked.menuCard = null
                    if (option.modelData.secondary)
                        catalogue.activateSecondary(card)
                    else
                        catalogue.activate(card)
                }
                Keys.onReturnPressed: activate()
                Keys.onSpacePressed: activate()
                Keys.onEscapePressed: docked.menuCard = null
                Text {
                    x: Theme.browserMenuTextInset
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 2 * Theme.browserMenuTextInset
                    elide: Text.ElideRight
                    text: option.modelData.label
                    color: option.enabled ? Theme.textMain : Theme.textDisabled
                    font.pixelSize: Theme.typeLabel
                }
                MouseArea {
                    id: optionArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: option.activate()
                }
            }
        }
    }
}
