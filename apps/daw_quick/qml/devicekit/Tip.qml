import QtQuick
import "../shell" as Shell

// A kit widget's tooltip: a thin wrapper over the shell's one tooltip
// (Tips, drawn by the TipLayer), so panels and the shell look alike. The
// widget decides when it shows (`shown`, usually its hover); the first line
// names the control and its value, `detail` adds lines below it. Without a
// TipLayer (a panel loaded on its own) nothing is drawn.
Item {
    id: tip
    // Kept for the widgets that pass one; the shell's look is used.
    required property KitTheme theme
    property bool shown: false
    property string text: ""
    property string detail: ""
    property string shortcut: ""
    // The control the tip describes: the item it is declared in.
    readonly property Item anchorItem: parent
    readonly property string fullText: detail !== "" ? text + "\n" + detail : text

    visible: false
    width: 0
    height: 0
    onShownChanged: {
        if (shown && anchorItem)
            Shell.Tips.request(anchorItem, fullText, shortcut)
        else
            Shell.Tips.release(anchorItem)
    }
    onFullTextChanged: if (shown && Shell.Tips.item === anchorItem) Shell.Tips.text = fullText
    Component.onDestruction: Shell.Tips.release(anchorItem)
}
