import QtQuick

// A tooltip for the control it is placed in (ADR-0059): what it does, and
// its shortcut. It never takes input from the control.
//     MouseArea { … Tip { text: "Loop the selection"; shortcut: "L" } }
// With no text it reads the control's accessible name (the item's, or its
// parent's when the item is a bare MouseArea), so a control described for
// screen readers is described on hover too.
HoverHandler {
    id: tip
    property string text: ""
    property string shortcut: ""
    // The item the tip points at: the named control by default.
    property Item anchorItem: named
    function nameOf(item) {
        try {
            return item && item.Accessible ? String(item.Accessible.name || "") : ""
        } catch (error) {
            return ""
        }
    }
    readonly property Item named: parent && nameOf(parent).length === 0 && parent.parent
                                  && nameOf(parent.parent).length > 0 ? parent.parent : parent
    readonly property string shown: text.length > 0 ? text : nameOf(anchorItem)
    onHoveredChanged: {
        if (hovered && enabled)
            Tips.request(anchorItem, shown, shortcut)
        else
            Tips.release(anchorItem)
    }
    onShownChanged: if (hovered && Tips.item === anchorItem) Tips.text = shown
    Component.onDestruction: Tips.release(anchorItem)
}
