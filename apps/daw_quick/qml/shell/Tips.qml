pragma Singleton
import QtQuick

// One tooltip for the whole shell (ADR-0059). A control asks with
// request() when hovered (Tip does this) and lets go with release(); the
// TipLayer draws it. A tip appears after `delay`; moving to another control
// while one shows is immediate.
QtObject {
    id: tips
    readonly property int delay: 600
    // How long after a tip closes the next one still shows at once.
    readonly property int warmFor: 400
    property Item item: null
    property string text: ""
    property string shortcut: ""
    property bool shown: false
    // True for a moment after a tip closes, so the next one is immediate.
    property bool warm: false

    function request(target, label, keys) {
        if (!target || !label || label.length === 0)
            return
        item = target
        text = label
        shortcut = keys || ""
        if (shown || warm) {
            shown = true
            wait.stop()
        } else {
            wait.restart()
        }
    }
    function release(target) {
        if (target !== item)
            return
        wait.stop()
        if (shown) {
            warm = true
            cool.restart()
        }
        shown = false
        item = null
    }
    // A press hides the tip until the pointer comes back.
    function dismiss() {
        wait.stop()
        shown = false
    }

    property Timer wait: Timer {
        interval: tips.delay
        onTriggered: tips.shown = tips.item !== null
    }
    property Timer cool: Timer {
        interval: tips.warmFor
        onTriggered: tips.warm = false
    }
}
