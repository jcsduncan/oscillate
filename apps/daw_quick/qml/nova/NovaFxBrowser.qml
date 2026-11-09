pragma ComponentBehavior: Bound
import QtQuick
import "NovaContract.js" as Contract

// The effect browser (NOVA-UI-11): search, the categories, and every
// module with a line on what it does. "+ Add effect" opens it to add;
// a strip's name opens it to swap that module (`swapIndex` ≥ 0), with the
// current module marked. Picking emits picked(id) and closes.
Rectangle {
    id: browser
    required property var panel
    property int swapIndex: -1
    property string current: ""
    property string category: Contract.fxCategories[0]
    readonly property var shown: {
        const query = search.text.trim().toLowerCase()
        return Contract.modules.filter(m => (category === Contract.fxCategories[0] || m.category === category)
                                            && (query === "" || (m.name + " " + m.blurb).toLowerCase().indexOf(query) >= 0))
    }
    signal picked(string moduleId)

    function open() {
        search.text = ""
        category = Contract.fxCategories[0]
        visible = true
        search.forceActiveFocus()
    }
    function close() { visible = false }
    function choose(moduleId) {
        close()
        picked(moduleId)
    }

    objectName: "novaFxBrowser"
    visible: false
    width: panel.theme.fxBrowserWidth
    height: panel.theme.fxBrowserHeight
    radius: panel.theme.fxBrowserRadius
    color: panel.theme.popFill
    border.color: panel.theme.line2
    Accessible.role: Accessible.Dialog
    Accessible.name: swapIndex >= 0 ? "Swap effect" : "Add effect"

    // Esc closes the browser from any control in it.
    Keys.onEscapePressed: close()

    // Swallow clicks so they do not reach the dismiss layer below.
    MouseArea {
        readonly property string pointerOnlyReason: "swallows clicks on the browser's bare plate so they do not close it"
        anchors.fill: parent
    }

    Rectangle {
        x: browser.panel.theme.fxBrowserPad
        y: browser.panel.theme.fxBrowserPad
        width: parent.width - 2 * browser.panel.theme.fxBrowserPad
        height: browser.panel.theme.fxSearchHeight
        radius: browser.panel.theme.radiusXl
        color: browser.panel.theme.wellFill
        border.color: search.activeFocus ? browser.panel.theme.pk : browser.panel.theme.line2
        TextInput {
            id: search
            objectName: "novaFxBrowserSearch"
            x: browser.panel.theme.fxSearchInset
            width: parent.width - 2 * browser.panel.theme.fxSearchInset
            anchors.verticalCenter: parent.verticalCenter
            color: browser.panel.theme.text
            font.family: browser.panel.theme.sans
            font.pixelSize: browser.panel.theme.typeLabel
            clip: true
            Keys.onEscapePressed: browser.close()
            Keys.onReturnPressed: if (browser.shown.length > 0) browser.choose(browser.shown[0].id)
        }
        Text {
            visible: search.text === ""
            x: browser.panel.theme.fxSearchInset
            anchors.verticalCenter: parent.verticalCenter
            text: browser.swapIndex >= 0 ? "Swap for…" : "Search effects"
            color: browser.panel.theme.faint
            font.family: browser.panel.theme.sans
            font.pixelSize: browser.panel.theme.typeLabel
        }
    }

    Column {
        x: browser.panel.theme.fxBrowserPad
        y: browser.panel.theme.fxBrowserListTop
        width: browser.panel.theme.fxCategoryWidth
        spacing: browser.panel.theme.spaceXxs
        Repeater {
            model: Contract.fxCategories
            delegate: Rectangle {
                id: categoryTile
                required property string modelData
                readonly property bool on: browser.category === modelData
                objectName: "novaFxBrowserCategory:" + modelData
                width: parent.width
                height: browser.panel.theme.fxCategoryHeight
                radius: browser.panel.theme.fxCategoryRadius
                color: on ? browser.panel.theme.soft(browser.panel.theme.pk) : "transparent"
                border.color: activeFocus ? browser.panel.theme.pk : "transparent"
                border.width: browser.panel.theme.strokeBold
                activeFocusOnTab: true
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData
                Accessible.checked: on
                Keys.onReturnPressed: browser.category = modelData
                Keys.onSpacePressed: browser.category = modelData
                Text {
                    x: browser.panel.theme.fxTileInset
                    anchors.verticalCenter: parent.verticalCenter
                    text: categoryTile.modelData.toUpperCase()
                    color: categoryTile.on ? browser.panel.theme.pk : browser.panel.theme.dim
                    font.family: browser.panel.theme.sans
                    font.pixelSize: browser.panel.theme.typeSmall
                    font.weight: Font.Bold
                    font.letterSpacing: browser.panel.theme.fxCategorySpacing
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: browser.category = categoryTile.modelData
                }
            }
        }
    }

    Flickable {
        x: browser.panel.theme.fxBrowserGridLeft
        y: browser.panel.theme.fxBrowserListTop
        width: parent.width - x - browser.panel.theme.fxBrowserPad
        height: parent.height - y - browser.panel.theme.fxBrowserPad
        contentHeight: grid.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Grid {
            id: grid
            width: parent.width
            columns: 2
            spacing: browser.panel.theme.spaceSm
            Repeater {
                model: browser.shown
                delegate: Rectangle {
                    id: tile
                    required property var modelData
                    readonly property bool isCurrent: modelData.id === browser.current
                    objectName: "novaFxBrowserItem:" + modelData.id
                    width: (grid.width - grid.spacing) / 2
                    height: browser.panel.theme.fxTileHeight
                    radius: browser.panel.theme.fxTileRadius
                    color: browser.panel.theme.card
                    border.color: tileArea.containsMouse || isCurrent || activeFocus ? browser.panel.theme.pk : browser.panel.theme.line
                    border.width: activeFocus ? browser.panel.theme.strokeBold : browser.panel.theme.hairline
                    activeFocusOnTab: true
                    Keys.onReturnPressed: browser.choose(modelData.id)
                    Keys.onSpacePressed: browser.choose(modelData.id)
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.name
                    Accessible.description: modelData.blurb
                    Text {
                        x: browser.panel.theme.fxTileInset
                        y: browser.panel.theme.fxTileTitleTop
                        text: tile.modelData.name.toUpperCase() + (tile.isCurrent ? "  · NOW" : "")
                        color: browser.panel.theme.text
                        font.family: browser.panel.theme.sans
                        font.pixelSize: browser.panel.theme.typeBody
                        font.weight: Font.ExtraBold
                        font.letterSpacing: browser.panel.theme.fxTileTitleSpacing
                    }
                    Text {
                        x: browser.panel.theme.fxTileInset
                        y: browser.panel.theme.fxTileBlurbTop
                        width: parent.width - 2 * browser.panel.theme.fxTileInset
                        text: tile.modelData.blurb
                        color: browser.panel.theme.faint
                        font.family: browser.panel.theme.sans
                        font.pixelSize: browser.panel.theme.typeSmall
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                    MouseArea {
                        id: tileArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: browser.choose(tile.modelData.id)
                    }
                }
            }
        }
    }
}
