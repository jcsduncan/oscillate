.pragma library

// One colour per track macro (ADR-0059), shared by the macro strip and the
// rings on the knobs each macro drives.
const colors = ["#ff9f43", "#4fd1c5", "#a78bfa", "#f472b6", "#facc15", "#60a5fa", "#34d399", "#f87171"]

function color(index) {
    return index >= 0 && index < colors.length ? colors[index] : "#8892a0"
}

// A tooltip line naming the macro driving a parameter, or "" when none (or
// the controller has no macros).
function tipLine(controller, parameterId) {
    if (typeof controller.macro_of !== "function" || controller.macro_revision < 0)
        return ""
    const index = controller.macro_of(parameterId)
    return index >= 0 ? "Macro " + (index + 1) + " drives this\n" : ""
}
