pragma ComponentBehavior: Bound
import QtQuick

// Every device panel's header, one layout for all (issue #164):
//
//   WORDMARK subtitle | ◂ preset ▾ ▸ | readout | ............ | status
//   [ mode switches, in a second row ]
//
// The brand is the kit wordmark over its subtitle, or a panel's own
// (`brand`, a Component) where the wordmark is drawn (a logo). The preset
// browser (PresetBrowser) starts at theme.headerBrowserX, clear of the
// brand; a device with no presets passes `presets: false` and may show its
// own `centre` there (a sample's name). The readout (Readout) sits in its
// own box after the browser and hides until a control is touched. The
// status badge (StatusBadge) sits at the right, with any small live
// readings (`tools`: an output meter, a last-note readout) just before it.
// Children declared inside the header are its mode switches, laid out in a
// row under the first.
Item {
    id: header
    required property var controller
    required property KitTheme theme
    // Marks the header for the panel contracts: one per panel.
    readonly property bool deviceHeader: true
    property string wordmark: ""
    property string subtitle: ""
    property Component brand: null
    property bool presets: true
    property Component centre: null
    // The preset browser's list and stepping (see PresetBrowser).
    property var menu: null
    property var opener: null
    property var stepper: null
    property real browserWidth: theme.presetDisplayWidth
    // A panel's own preset tag and name text (the browser's by default).
    property var presetTag: undefined
    property var presetName: undefined
    property Component tools: null
    // A panel's own names for the readout (see Readout).
    property var readoutNameOf: null
    property var readoutValueOf: null
    readonly property alias browser: browserLoader.item
    readonly property alias readout: readout
    readonly property alias status: status
    readonly property alias brandItem: brandLoader.item
    readonly property alias toolsItem: toolsLoader.item
    default property alias modes: modeRow.data
    readonly property bool hasModes: modeRow.children.length > 0
    readonly property real rowHeight: theme.headerHeight

    objectName: theme.prefix + "Header"
    implicitHeight: rowHeight + (hasModes ? theme.headerModeGap + modeRow.height : 0)
    height: implicitHeight

    Component {
        id: wordmarkBrand
        Column {
            spacing: header.theme.subtitleGap
            Text {
                objectName: header.theme.prefix + "Wordmark"
                text: header.wordmark
                color: header.theme.wordmarkInk
                font.family: header.theme.wordmarkFont !== "" ? header.theme.wordmarkFont : Qt.application.font.family
                font.pixelSize: header.theme.wordmarkSize
                font.weight: header.theme.wordmarkWeight
                font.letterSpacing: header.theme.wordmarkSpacing
            }
            Text {
                objectName: header.theme.prefix + "Subtitle"
                visible: header.subtitle !== ""
                text: header.subtitle
                color: header.theme.subtitleInk
                font.pixelSize: header.theme.subtitleSize
                font.bold: true
                font.letterSpacing: header.theme.subtitleSpacing
            }
        }
    }
    Component {
        id: presetBrowser
        PresetBrowser {
            controller: header.controller
            theme: header.theme
            menu: header.menu
            opener: header.opener
            stepper: header.stepper
            displayWidth: header.browserWidth
            Binding on tag { when: header.presetTag !== undefined; value: header.presetTag }
            Binding on name { when: header.presetName !== undefined; value: header.presetName }
        }
    }

    Loader {
        id: brandLoader
        anchors.verticalCenter: firstRow.verticalCenter
        sourceComponent: header.brand !== null ? header.brand : wordmarkBrand
    }
    Item {
        id: firstRow
        width: header.width
        height: header.rowHeight
    }
    Loader {
        id: browserLoader
        x: Math.max(header.theme.headerBrowserX,
                    brandLoader.x + brandLoader.width + header.theme.headerGap)
        anchors.verticalCenter: firstRow.verticalCenter
        sourceComponent: header.presets ? presetBrowser : header.centre
    }
    Readout {
        id: readout
        controller: header.controller
        theme: header.theme
        nameOf: header.readoutNameOf
        valueOf: header.readoutValueOf
        x: browserLoader.x + browserLoader.width + header.theme.headerGap
        width: Math.max(header.theme.readoutMinWidth,
                        Math.min(header.theme.readoutWidth, toolsLoader.x - header.theme.headerGap - x))
        anchors.verticalCenter: firstRow.verticalCenter
    }
    Loader {
        id: toolsLoader
        anchors.right: status.left
        anchors.rightMargin: item !== null ? header.theme.headerGap : 0
        anchors.verticalCenter: firstRow.verticalCenter
        sourceComponent: header.tools
    }
    StatusBadge {
        id: status
        controller: header.controller
        theme: header.theme
        x: header.width - width
        anchors.verticalCenter: firstRow.verticalCenter
    }
    Row {
        id: modeRow
        objectName: header.theme.prefix + "HeaderModes"
        y: header.rowHeight + header.theme.headerModeGap
        spacing: header.theme.headerGap
    }
}
