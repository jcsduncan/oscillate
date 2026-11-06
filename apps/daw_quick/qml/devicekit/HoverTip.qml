import QtQuick
import "../shell" as Shell

// A tooltip for a panel's own control (a pill, a pad, an LED), where the
// kit widgets carry theirs built in: the shell's Tip, reading the control's
// spoken name unless `text` is given.
Shell.Tip {
    // Kept so panels can pass their theme; the shell's look is used.
    property KitTheme theme: null
}
