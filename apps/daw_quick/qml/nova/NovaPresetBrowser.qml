pragma ComponentBehavior: Bound
import QtQuick

// Nova's preset browser (ADR-0076): a drawer docked at the left of the page
// area, toggled by BROWSE or the preset name; the pages narrow beside it.
// It opens with the list focused, not the search box, so the typing
// keyboard keeps playing notes: ↑/↓ move and load (a browse is an
// audition), Return or Escape close keeping what is loaded. "/" or a click
// enters the search (name, category or tag, any case) and Escape returns to
// the list. Category chips and a favourites filter narrow the factory and
// user presets; the tag chips wait behind "#TAGS" (tags are searched as
// text too). Each row shows its category's colour, name, category and ★,
// with its tags on a line beneath. ★ marks a favourite. The footer saves the sound as a user preset and
// renames or deletes the selected user preset. While any filter is set, the
// preset bar's arrows step through the filtered list (step()).
Rectangle {
    id: browser
    required property var panel
    readonly property var controller: panel.controller
    readonly property var theme: panel.theme

    // The filters, kept while the panel lives.
    property string search: ""
    property string category: ""
    property string tag: ""
    property bool favouritesOnly: false
    // The tag chips are shown (behind the #TAGS chip), or a tag is set.
    property bool showTags: false
    readonly property bool filterActive: browser.search !== "" || browser.category !== "" || browser.tag !== ""
                                         || browser.favouritesOnly
    // The entry the keyboard and the footer act on (an index into entries).
    property int selected: -1
    // [{header}] or {user, name, category, tags, index, key}: the presets
    // the filters let through, factory first, each group under its header.
    readonly property var entries: compute(controller.presets, controller.user_presets, controller.favourites,
                                           browser.search, browser.category, browser.tag, browser.favouritesOnly)
    readonly property bool opened: visible
    // A text field has the keys: letters type instead of playing notes.
    readonly property bool searching: searchField.inputFocused
    readonly property bool naming: nameField.inputFocused
    readonly property bool typing: searching || naming

    // Layout.
    readonly property real pad: theme.browserPad
    readonly property real chipsMaximum: theme.browserChipsMaximum
    // With the tags out, the chips may take up to this share of the drawer.
    readonly property real tagsShare: 0.45
    readonly property real chipHeight: theme.browserChipHeight
    readonly property real chipFontSize: theme.typeCaption
    readonly property real rowHeight: theme.browserRowHeight
    readonly property real taggedRowHeight: theme.browserTaggedRowHeight
    readonly property real headerRowHeight: theme.browserHeaderRowHeight
    readonly property real fieldHeight: theme.browserFieldHeight
    readonly property real actionHeight: theme.browserActionHeight

    objectName: "novaPresetBrowser"
    visible: false
    color: theme.rail
    Accessible.role: Accessible.Pane
    Accessible.name: "Presets"

    function matches(entry, words, wantedCategory, wantedTag, favourites, onlyFavourites) {
        if (wantedCategory !== "" && entry.category !== wantedCategory)
            return false
        if (wantedTag !== "" && entry.tags.indexOf(wantedTag) < 0)
            return false
        if (onlyFavourites && favourites.indexOf(entry.key) < 0)
            return false
        const haystack = (entry.name + " " + entry.category + " " + entry.tags.join(" ")).toLowerCase()
        for (let w = 0; w < words.length; ++w) {
            if (haystack.indexOf(words[w]) < 0)
                return false
        }
        return true
    }
    function compute(presets, users, favourites, text, wantedCategory, wantedTag, onlyFavourites) {
        const words = text.toLowerCase().split(/\s+/).filter(w => w.length > 0)
        const factory = []
        for (let i = 0; i < presets.length; ++i) {
            const p = presets[i]
            const entry = { user: false, name: p.name, category: p.category,
                            tags: p.tags !== undefined ? p.tags : [], index: p.index,
                            key: controller.preset_key(false, p.name) }
            if (matches(entry, words, wantedCategory, wantedTag, favourites, onlyFavourites))
                factory.push(entry)
        }
        const mine = []
        for (let u = 0; u < users.length; ++u) {
            const entry = { user: true, name: users[u].name, category: "User", tags: [], index: -1,
                            key: controller.preset_key(true, users[u].name) }
            if (matches(entry, words, wantedCategory, wantedTag, favourites, onlyFavourites))
                mine.push(entry)
        }
        const out = []
        if (factory.length > 0)
            out.push({ header: "FACTORY · " + factory.length })
        for (const e of factory)
            out.push(e)
        if (mine.length > 0)
            out.push({ header: "USER · " + mine.length })
        for (const e of mine)
            out.push(e)
        return out
    }
    // The entry that is loaded now, or -1.
    function loadedIndex() {
        const userLoaded = controller.preset_category === "User"
        for (let i = 0; i < entries.length; ++i) {
            const e = entries[i]
            if (e.header !== undefined)
                continue
            if (e.user ? userLoaded && e.name === controller.preset_name
                       : !userLoaded && e.index === controller.preset_index)
                return i
        }
        return -1
    }
    function load(i) {
        const e = entries[i]
        if (e === undefined || e.header !== undefined)
            return
        selected = i
        if (e.user)
            controller.apply_user_preset(e.name)
        else
            controller.apply_preset(e.index)
    }
    // Moves the selection through the presets (not the headers), loading it.
    function move(direction) {
        if (entries.length === 0)
            return
        let next = selected >= 0 ? selected : loadedIndex()
        for (let tries = 0; tries < entries.length; ++tries) {
            next = next < 0 ? (direction > 0 ? 0 : entries.length - 1)
                            : (next + direction + entries.length) % entries.length
            if (entries[next].header === undefined) {
                load(next)
                list.positionAt(next)
                return
            }
        }
    }
    // The preset bar's arrows while a filter is set: the next or previous
    // preset in the filtered list, from the one loaded.
    function step(direction) {
        selected = loadedIndex()
        move(direction)
    }
    // Opens with the list focused: the keys keep playing notes.
    function open() {
        selected = loadedIndex()
        visible = true
        searchField.text = browser.search
        focusList()
        list.positionAt(selected)
    }
    function close() {
        visible = false
        panel.forceActiveFocus()
    }
    function toggle() {
        if (opened)
            close()
        else
            open()
    }
    function focusList() {
        list.forceActiveFocus()
    }
    function focusSearch() {
        if (!opened)
            open()
        searchField.focusInput()
    }
    function toggleFavourite(key) {
        controller.set_favourite(key, !controller.is_favourite(key))
    }
    // The footer. Save replaces the loaded user preset, or saves under the
    // typed name; Save As always takes the typed name.
    readonly property var selectedEntry: selected >= 0 && selected < entries.length
        && entries[selected].header === undefined ? entries[selected] : null
    readonly property bool userLoaded: controller.preset_category === "User"
    // Selects the loaded preset and scrolls the list to it, once the list
    // has taken in a save or rename.
    function reveal() {
        selected = loadedIndex()
        list.positionAt(selected)
    }
    function save() {
        const name = userLoaded ? controller.preset_name : nameField.text.trim()
        if (name !== "" && controller.save_user_preset(name)) {
            nameField.text = ""
            Qt.callLater(reveal)
        }
    }
    function saveAs() {
        if (nameField.text.trim() !== "" && controller.save_user_preset(nameField.text.trim())) {
            nameField.text = ""
            Qt.callLater(reveal)
        }
    }
    function rename() {
        if (selectedEntry !== null && selectedEntry.user
                && controller.rename_user_preset(selectedEntry.name, nameField.text.trim())) {
            nameField.text = ""
            Qt.callLater(reveal)
        }
    }
    function remove() {
        if (selectedEntry !== null && selectedEntry.user && controller.delete_user_preset(selectedEntry.name))
            selected = -1
    }

    // The drawer's edge against the pages.
    Rectangle {
        anchors.right: parent.right
        width: browser.theme.hairline
        height: parent.height
        color: browser.theme.line
    }
    // Clicks and wheel turns on the drawer never reach the page under it.
    MouseArea {
        anchors.fill: parent
        onWheel: wheel => { wheel.accepted = true }
    }

    component Chip: Rectangle {
        id: chip
        property string text: ""
        property bool on: false
        property color accent: browser.theme.neutral
        signal clicked()
        implicitWidth: chipLabel.implicitWidth + browser.theme.browserLabelPadding
        implicitHeight: browser.chipHeight
        radius: height / 2
        // Lit: filled in its accent.
        color: on ? accent : chipArea.containsMouse ? browser.theme.hover : "transparent"
        border.color: on ? accent : browser.theme.line2
        activeFocusOnTab: true
        Accessible.role: Accessible.CheckBox
        Accessible.name: text
        Accessible.checked: on
        Keys.onSpacePressed: chip.clicked()
        Keys.onReturnPressed: chip.clicked()
        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.text
            color: chip.on ? browser.theme.bg : browser.theme.dim
            font.family: browser.theme.mono
            font.pixelSize: browser.chipFontSize
            font.weight: Font.DemiBold
            font.letterSpacing: browser.theme.browserChipSpacing
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            // Back to the list after a filter, so the keys play again.
            onClicked: {
                chip.clicked()
                browser.focusList()
            }
        }
    }

    component Field: Rectangle {
        id: field
        property alias text: fieldInput.text
        property string placeholder: ""
        signal edited()
        signal accepted()
        signal moved(int direction)
        signal escaped()
        readonly property bool inputFocused: fieldInput.activeFocus
        function focusInput() {
            fieldInput.forceActiveFocus()
        }
        height: browser.fieldHeight
        radius: browser.theme.browserControlRadius
        color: browser.theme.wellFill
        // Amber while it has the keys, as the header's pill.
        border.color: fieldInput.activeFocus ? browser.theme.am : browser.theme.line2
        TextInput {
            id: fieldInput
            anchors.fill: parent
            anchors.leftMargin: browser.theme.browserFieldInset
            anchors.rightMargin: browser.theme.browserFieldInset
            verticalAlignment: TextInput.AlignVCenter
            color: browser.theme.text
            selectionColor: browser.theme.accentSoft
            font.family: browser.theme.sans
            font.pixelSize: browser.theme.typeLabel
            clip: true
            Accessible.role: Accessible.EditableText
            Accessible.name: field.placeholder
            onTextEdited: field.edited()
            onAccepted: field.accepted()
            Keys.onUpPressed: field.moved(-1)
            Keys.onDownPressed: field.moved(1)
            Keys.onEscapePressed: field.escaped()
        }
        Text {
            anchors.fill: fieldInput
            verticalAlignment: Text.AlignVCenter
            visible: fieldInput.text === ""
            text: field.placeholder
            color: browser.theme.faint
            font.family: browser.theme.sans
            font.pixelSize: browser.theme.typeLabel
        }
    }

    component Action: Rectangle {
        id: action
        property string text: ""
        property bool enabledAction: true
        property color accent: browser.theme.text
        signal clicked()
        implicitWidth: actionLabel.implicitWidth + browser.theme.browserLabelPadding
        height: browser.actionHeight
        radius: browser.theme.browserControlRadius
        opacity: enabledAction ? 1 : browser.theme.browserDisabledOpacity
        color: actionArea.pressed ? browser.theme.wellFill : actionArea.containsMouse ? browser.theme.hover : browser.theme.card2
        border.color: browser.theme.line2
        activeFocusOnTab: enabledAction
        Accessible.role: Accessible.Button
        Accessible.name: text
        Keys.onSpacePressed: if (enabledAction) action.clicked()
        Keys.onReturnPressed: if (enabledAction) action.clicked()
        Text {
            id: actionLabel
            anchors.centerIn: parent
            text: action.text
            color: action.accent
            font.family: browser.theme.mono
            font.pixelSize: browser.chipFontSize
            font.weight: Font.DemiBold
            font.letterSpacing: browser.theme.browserActionSpacing
        }
        MouseArea {
            id: actionArea
            anchors.fill: parent
            hoverEnabled: true
            enabled: action.enabledAction
            onClicked: action.clicked()
        }
    }


    // ---- Search ----------------------------------------------------------------------------

    Field {
        id: searchField
        objectName: "novaPresetSearch"
        x: browser.pad
        y: browser.pad
        width: browser.width - 2 * browser.pad - slashKey.width - closeButton.width - 2 * browser.theme.spaceSm
        placeholder: "Search name, tag, sound…"
        onEdited: browser.search = text
        onMoved: direction => browser.move(direction)
        // Escape and Return give the keys back to the list (and the notes).
        onEscaped: browser.focusList()
        onAccepted: browser.focusList()
    }
    // "/" enters the search.
    Rectangle {
        id: slashKey
        x: searchField.x + searchField.width + browser.theme.spaceSm
        anchors.verticalCenter: searchField.verticalCenter
        width: browser.theme.browserSlashKeyWidth
        height: browser.theme.browserSlashKeyHeight
        radius: browser.theme.radiusMd
        color: "transparent"
        border.color: browser.theme.line2
        Text {
            anchors.centerIn: parent
            text: "/"
            color: browser.theme.faint
            font.family: browser.theme.mono
            font.pixelSize: browser.theme.typeSmall
            font.weight: Font.DemiBold
        }
    }
    Rectangle {
        id: closeButton
        objectName: "novaPresetBrowserClose"
        x: browser.width - browser.pad - width
        anchors.verticalCenter: searchField.verticalCenter
        width: browser.theme.browserCloseSize
        height: browser.theme.browserCloseSize
        radius: browser.theme.browserControlRadius
        color: closeArea.containsMouse ? browser.theme.hover : "transparent"
        Accessible.role: Accessible.Button
        Accessible.name: "Close presets"
        Text {
            anchors.centerIn: parent
            text: "×"
            color: browser.theme.dim
            font.pixelSize: browser.theme.typeDisplay
        }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: browser.close()
        }
    }
    Rectangle {
        y: searchField.y + searchField.height + browser.pad
        width: browser.width
        height: browser.theme.hairline
        color: browser.theme.line
    }

    // ---- Category, tag and favourite filters ---------------------------------------------------

    Flickable {
        id: chipScroller
        objectName: "novaPresetChipScroller"
        x: browser.pad
        y: searchField.y + searchField.height + 2 * browser.pad
        width: browser.width - 2 * browser.pad
        height: Math.min(chips.implicitHeight, browser.showTags ? Math.max(browser.chipsMaximum,
                         browser.height * browser.tagsShare) : browser.chipsMaximum)
        contentWidth: width
        contentHeight: chips.implicitHeight
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Flow {
            id: chips
            width: chipScroller.width
            spacing: browser.theme.browserChipGap
            Chip {
                objectName: "novaPresetCategory:"
                text: "ALL"
                on: browser.category === ""
                onClicked: browser.category = ""
            }
            Repeater {
                model: browser.controller.preset_categories.concat(
                    browser.controller.user_presets.length > 0 ? ["User"] : [])
                delegate: Chip {
                    required property string modelData
                    objectName: "novaPresetCategory:" + modelData
                    text: modelData.toUpperCase()
                    accent: browser.theme.categoryColour(modelData)
                    on: browser.category === modelData
                    onClicked: browser.category = on ? "" : modelData
                }
            }
            Chip {
                objectName: "novaPresetFavourites"
                text: "★"
                accent: browser.theme.am
                on: browser.favouritesOnly
                Accessible.name: "Favourites"
                onClicked: browser.favouritesOnly = !browser.favouritesOnly
            }
            // The tags, behind a toggle: lit while they show or one is set.
            Chip {
                objectName: "novaPresetTagsToggle"
                text: "#TAGS"
                accent: browser.theme.vi
                on: browser.showTags || browser.tag !== ""
                onClicked: {
                    if (browser.tag !== "")
                        browser.tag = ""
                    else
                        browser.showTags = !browser.showTags
                }
            }
            Repeater {
                model: browser.showTags || browser.tag !== "" ? browser.controller.preset_tags : []
                delegate: Chip {
                    required property string modelData
                    objectName: "novaPresetTag:" + modelData
                    text: "#" + modelData
                    accent: browser.theme.vi
                    on: browser.tag === modelData
                    onClicked: browser.tag = on ? "" : modelData
                }
            }
        }
    }
    Rectangle {
        id: chipsRule
        y: chipScroller.y + chipScroller.height + browser.pad
        width: browser.width
        height: browser.theme.hairline
        color: browser.theme.line
    }

    // ---- The presets ---------------------------------------------------------------------------

    ListView {
        id: list
        objectName: "novaPresetList"
        x: 0
        y: chipsRule.y + browser.theme.hairline
        width: browser.width - browser.theme.hairline
        height: footer.y - y
        clip: true
        model: browser.entries.length
        boundsBehavior: Flickable.StopAtBounds
        // The browser's own keys; letters fall through to the typing keyboard.
        keyNavigationEnabled: false
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: "Presets"
        function positionAt(i) {
            if (i >= 0)
                positionViewAtIndex(i, ListView.Contain)
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Slash) {
                browser.focusSearch()
                event.accepted = true
            }
        }
        Keys.onUpPressed: browser.move(-1)
        Keys.onDownPressed: browser.move(1)
        Keys.onReturnPressed: browser.close()
        Keys.onEnterPressed: browser.close()
        Keys.onEscapePressed: browser.close()

        delegate: Rectangle {
            id: row
            required property int index
            readonly property var entry: browser.entries[index]
            readonly property bool header: entry.header !== undefined
            readonly property bool loaded: !header && index === browser.loadedIndex()
            readonly property bool lit: index === browser.selected
            readonly property bool favourite: !header && browser.controller.favourites.indexOf(entry.key) >= 0
            readonly property bool tagged: !header && entry.tags.length > 0
            readonly property color categoryColour: header ? browser.theme.dim : browser.theme.categoryColour(entry.category)
            objectName: header ? "novaPresetHeader:" + index
                               : "novaPresetRow:" + (entry.user ? "user/" : "factory/") + entry.name
            width: ListView.view.width
            height: header ? browser.headerRowHeight : tagged ? browser.taggedRowHeight : browser.rowHeight
            color: header ? "transparent"
                : lit ? browser.theme.selectedFill
                : rowArea.containsMouse ? browser.theme.card : "transparent"
            Accessible.role: header ? Accessible.StaticText : Accessible.ListItem
            Accessible.name: header ? entry.header : entry.name + ", " + entry.category
            Accessible.selected: lit
            Text {
                visible: row.header
                x: browser.theme.spaceLg
                anchors.bottom: parent.bottom
                anchors.bottomMargin: browser.theme.spaceXs
                text: row.header ? row.entry.header : ""
                color: browser.theme.faint
                font.family: browser.theme.sans
                font.pixelSize: browser.theme.typeSmall
                font.weight: Font.Bold
                font.letterSpacing: browser.theme.browserHeaderSpacing
            }
            MouseArea {
                id: rowArea
                anchors.fill: parent
                enabled: !row.header && browser.controller.editable
                hoverEnabled: true
                onClicked: {
                    browser.load(row.index)
                    browser.focusList()
                }
                onDoubleClicked: browser.close()
            }
            // The selected row's edge, in its category's colour.
            Rectangle {
                visible: row.lit && !row.header
                width: browser.theme.strokeBold
                height: parent.height
                color: row.categoryColour
            }
            // The category's colour.
            Rectangle {
                visible: !row.header
                x: browser.theme.spaceLg
                y: (browser.rowHeight - height) / 2
                width: browser.theme.browserDotSize
                height: browser.theme.browserDotSize
                radius: width / 2
                color: row.categoryColour
            }
            Text {
                visible: !row.header
                x: browser.theme.browserNameX
                y: 0
                height: browser.rowHeight
                width: categoryText.x - x - browser.theme.spaceSm
                verticalAlignment: Text.AlignVCenter
                text: row.header ? "" : row.entry.name
                color: browser.theme.text
                font.family: browser.theme.sans
                font.pixelSize: browser.theme.typeEmphasis
                font.weight: row.loaded ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Text {
                id: categoryText
                visible: !row.header
                x: star.x - width - browser.theme.spaceXs
                height: browser.rowHeight
                verticalAlignment: Text.AlignVCenter
                text: row.header ? "" : row.entry.category.toUpperCase()
                color: browser.theme.faint
                font.family: browser.theme.mono
                font.pixelSize: browser.theme.typeCaption
            }
            Rectangle {
                id: star
                objectName: row.header ? "" : "novaPresetStar:" + row.entry.key
                visible: !row.header
                x: row.width - width - browser.theme.spaceSm
                y: (browser.rowHeight - height) / 2
                width: browser.theme.browserStarSize
                height: browser.theme.browserStarSize
                radius: browser.theme.radiusLg
                color: "transparent"
                Accessible.role: Accessible.CheckBox
                Accessible.name: row.header ? "" : "Favourite " + row.entry.name
                Accessible.checked: row.favourite
                Text {
                    anchors.centerIn: parent
                    text: "★"
                    color: row.favourite ? browser.theme.am : browser.theme.faint
                    font.pixelSize: browser.theme.typeLabel
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: browser.toggleFavourite(row.entry.key)
                }
            }
            // The tags, under the name.
            Text {
                visible: row.tagged
                x: browser.theme.browserNameX
                y: browser.rowHeight - browser.theme.browserTagsRaise
                width: row.width - x - browser.pad
                text: row.tagged ? row.entry.tags.join(" · ") : ""
                color: browser.theme.faint
                font.family: browser.theme.mono
                font.pixelSize: browser.theme.typeCaption
                elide: Text.ElideRight
            }
        }
    }
    // The list has the keys: a green edge, as the pill's KEYS PLAY NOTES.
    Rectangle {
        objectName: "novaPresetListFocus"
        visible: list.activeFocus
        y: list.y
        width: browser.theme.strokeBold
        height: list.height
        color: browser.theme.gr
    }
    Text {
        objectName: "novaPresetEmpty"
        x: 0
        y: list.y + browser.headerRowHeight
        width: browser.width
        horizontalAlignment: Text.AlignHCenter
        visible: browser.entries.length === 0
        text: "No presets match"
        color: browser.theme.faint
        font.family: browser.theme.sans
        font.pixelSize: browser.theme.typeLabel
    }

    // ---- Saving: the typed name, then what to do with it ----------------------------------------

    Column {
        id: footer
        x: browser.pad
        y: browser.height - height - browser.pad
        width: browser.width - 2 * browser.pad
        spacing: browser.theme.spaceSm
        Rectangle {
            width: browser.width
            x: -browser.pad
            height: browser.theme.hairline
            color: browser.theme.line
        }
        Field {
            id: nameField
            objectName: "novaPresetSaveName"
            width: parent.width
            placeholder: browser.userLoaded ? "New name for Save As or Rename" : "Name to save as"
            onEscaped: browser.focusList()
            onAccepted: browser.userLoaded ? browser.saveAs() : browser.save()
        }
        Flow {
            width: parent.width
            spacing: browser.theme.spaceSm
            Action {
                objectName: "novaPresetSave"
                text: "SAVE"
                enabledAction: browser.controller.editable && (browser.userLoaded || nameField.text.trim() !== "")
                onClicked: browser.save()
            }
            Action {
                objectName: "novaPresetSaveAs"
                text: "SAVE AS"
                enabledAction: browser.controller.editable && nameField.text.trim() !== ""
                onClicked: browser.saveAs()
            }
            Action {
                objectName: "novaPresetRename"
                text: "RENAME"
                enabledAction: browser.selectedEntry !== null && browser.selectedEntry.user
                               && nameField.text.trim() !== ""
                onClicked: browser.rename()
            }
            Action {
                objectName: "novaPresetDelete"
                text: "DELETE"
                accent: browser.theme.red
                enabledAction: browser.selectedEntry !== null && browser.selectedEntry.user
                onClicked: browser.remove()
            }
        }
        // The keys, in brief.
        Text {
            objectName: "novaPresetKeysHint"
            width: parent.width
            text: "↑ ↓ audition · / search · Esc back to playing · letters play notes unless the search is amber"
            color: browser.theme.faint
            font.family: browser.theme.mono
            font.pixelSize: browser.theme.typeCaption
            wrapMode: Text.WordWrap
        }
    }
}
