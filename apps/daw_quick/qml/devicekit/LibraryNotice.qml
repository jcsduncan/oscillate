pragma ComponentBehavior: Bound
import QtQuick

// A sampled instrument's library, one part for every panel (issue #171): its
// state in words, and
// - ready: the library's name, its credit and `details`;
// - loading: that it is being read;
// - missing or damaged: why, what the instrument plays once it is installed,
//   the install command with Copy, and Look again.
// Show in folder opens the library's folder, or its nearest existing parent
// before an install, so no panel prints a raw path. `library` is the
// controller's library map ({state, message, directory, name, attribution,
// install_command, required}); a panel without an installed library (Crate,
// which plays a sample of the user's) names what it lacks (`title`,
// `missingText`), gives its own `hint` and leaves the command out. Stacked by default;
// `wide` puts the command and the actions in a second column.
Item {
    id: notice
    required property var controller
    required property KitTheme theme
    property var library: ({})
    // The instrument, in "Bow is silent until its library is installed".
    property string instrument: ""
    // Replaces that sentence when set.
    property string hint: ""
    // A line under the credit when ready: how much is installed.
    property string details: ""
    property bool canReload: true
    property string title: "LIBRARY"
    property string missingText: "NOT INSTALLED"
    property string loadingText: "Reading the library. " + instrument + " plays once it is in memory."
    property bool wide: false
    property real wideShare: 0.5
    property int messageLines: 4
    readonly property string state: library.state !== undefined ? library.state : "missing"
    readonly property bool ready: state === "ready"
    readonly property bool loading: state === "loading"
    readonly property string command: library.install_command !== undefined ? library.install_command : ""
    readonly property string directory: library.directory !== undefined ? library.directory : ""
    readonly property bool showsCommand: !ready && !loading && command !== ""
    readonly property string stateText: ready ? "INSTALLED" : loading ? "LOADING"
        : state === "invalid" ? "DAMAGED" : missingText
    readonly property real gap: theme.noticeGap
    signal reload()

    objectName: theme.prefix + "LibraryNotice"
    implicitHeight: wide ? Math.max(about.implicitHeight, actions.y + actions.implicitHeight)
                         : actions.y + actions.implicitHeight
    Accessible.role: Accessible.Pane
    Accessible.name: notice.instrument + " " + notice.title.toLowerCase() + ": "
        + notice.stateText.toLowerCase()
    Accessible.description: notice.ready ? notice.library.name + ", " + notice.library.attribution
        : notice.library.message !== undefined ? notice.library.message : ""

    // The state, and what the library is or why it is not there.
    Column {
        id: about
        width: notice.wide ? notice.width * notice.wideShare : notice.width
        spacing: notice.gap

        Row {
            id: title
            spacing: notice.gap
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: notice.theme.statusLampSize
                height: width
                radius: width / 2
                color: notice.ready ? notice.theme.live : notice.theme.warning
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: notice.title
                color: notice.theme.ink
                font.family: notice.theme.sans
                font.pixelSize: notice.theme.typeSmall
                font.weight: Font.DemiBold
                font.letterSpacing: notice.theme.subtitleSpacing
            }
            Text {
                objectName: notice.theme.prefix + "LibraryState"
                anchors.verticalCenter: parent.verticalCenter
                text: notice.stateText
                color: notice.ready ? notice.theme.inkDim : notice.theme.warning
                font.family: notice.theme.sans
                font.pixelSize: notice.theme.typeSmall
                font.letterSpacing: notice.theme.subtitleSpacing
            }
        }
        Text {
            objectName: notice.theme.prefix + "LibraryName"
            width: parent.width
            visible: notice.ready
            text: notice.ready ? notice.library.name : ""
            color: notice.theme.ink
            font.family: notice.theme.sans
            font.pixelSize: notice.theme.typeBody
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: notice.theme.prefix + "Attribution"
            width: parent.width
            visible: notice.ready
            text: notice.ready ? notice.library.attribution : ""
            color: notice.theme.accent
            font.family: notice.theme.sans
            font.pixelSize: notice.theme.typeSmall
            wrapMode: Text.WordWrap
        }
        Text {
            width: parent.width
            visible: notice.ready && notice.details !== ""
            text: notice.details
            color: notice.theme.inkDim
            font.family: notice.theme.sans
            font.pixelSize: notice.theme.typeSmall
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: notice.theme.prefix + "LibraryMessage"
            width: parent.width
            visible: !notice.ready
            text: notice.loading
                ? notice.loadingText
                : (notice.library.message !== undefined && notice.library.message !== ""
                   ? notice.library.message + ". " : "")
                  + (notice.hint !== "" ? notice.hint
                     : notice.instrument + " is silent until its library is installed. It plays "
                       + (notice.library.required !== undefined ? notice.library.required : "") + ".")
            color: notice.theme.inkDim
            font.family: notice.theme.sans
            font.pixelSize: notice.theme.typeSmall
            wrapMode: Text.WordWrap
            maximumLineCount: notice.messageLines
            elide: Text.ElideRight
        }
    }

    // How to get it, and where it is.
    Column {
        id: actions
        x: notice.wide ? about.width + 2 * notice.gap : 0
        y: notice.wide ? title.height + notice.gap : about.implicitHeight + notice.gap
        width: notice.width - x
        spacing: notice.gap

        Rectangle {
            width: parent.width
            height: commandText.implicitHeight + 2 * notice.gap
            visible: notice.showsCommand
            radius: notice.gap
            color: notice.theme.well
            border.color: notice.theme.wellEdge
            Text {
                id: commandText
                objectName: notice.theme.prefix + "InstallCommand"
                x: notice.gap
                y: notice.gap
                width: parent.width - 2 * x
                text: notice.command
                color: notice.theme.accent
                font.family: notice.theme.mono
                font.pixelSize: notice.theme.typeSmall
                wrapMode: Text.WrapAnywhere
                Accessible.role: Accessible.StaticText
                Accessible.name: "Install command"
                Accessible.description: text
            }
        }
        Flow {
            width: parent.width
            spacing: notice.gap
            Button {
                theme: notice.theme
                objectName: notice.theme.prefix + "CopyInstall"
                visible: notice.showsCommand
                label: "Copy command"
                onClicked: notice.controller.set_clipboard_text(notice.command)
            }
            Button {
                theme: notice.theme
                objectName: notice.theme.prefix + "LibraryReload"
                visible: !notice.ready && !notice.loading && notice.canReload
                label: "Look again"
                onClicked: notice.reload()
            }
            Button {
                theme: notice.theme
                objectName: notice.theme.prefix + "ShowLibrary"
                visible: notice.directory !== ""
                label: "Show in folder"
                tip: "Open the library's folder in the file manager"
                onClicked: notice.controller.show_library_folder(notice.directory)
            }
        }
    }
}
