import QtQuick
import "CrateContract.js" as Contract

// The selected chop's filter as a response sketch on a log frequency scale
// (an ideal two-pole state-variable curve at its cutoff and resonance), in
// the chop's colour; a flat line while the filter is off.
Canvas {
    id: view
    required property var controller
    required property CrateTheme theme
    // The chop's parameter ids by control, or {}.
    property var ids: ({})
    property color colour: theme.accent
    readonly property bool bound: ids.filter !== undefined && ids.filter !== ""
    readonly property var setting: bound && controller.revision >= 0
        ? { mode: controller.choice(ids.filter), cutoff: controller.value(ids.cutoff),
            resonance: controller.value(ids.resonance) }
        : null
    readonly property int points: Math.max(2, Math.floor(width / 2))

    objectName: "crateFilterCurve"
    implicitHeight: theme.viewHeight
    renderStrategy: Canvas.Cooperative
    onSettingChanged: requestPaint()
    onColourChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const s = setting
        if (s === null)
            return
        const top = theme.filterCurvePad
        const bottom = height - theme.filterCurvePad
        const span = Contract.filterCeilingDb - Contract.filterFloorDb
        const yOf = db => top + (Contract.filterCeilingDb - Math.max(Contract.filterFloorDb,
                                  Math.min(Contract.filterCeilingDb, db))) / span * (bottom - top)
        // The 0 dB line.
        ctx.fillStyle = theme.centreLine
        ctx.fillRect(0, Math.round(yOf(0)), width, theme.hairline)
        ctx.beginPath()
        for (let i = 0; i < points; ++i) {
            const fraction = i / (points - 1)
            const db = Contract.filterDb(s.mode, s.cutoff, s.resonance, Contract.fractionHz(fraction))
            const x = fraction * width
            if (i === 0)
                ctx.moveTo(x, yOf(db))
            else
                ctx.lineTo(x, yOf(db))
        }
        ctx.lineWidth = theme.strokeBold
        ctx.strokeStyle = colour
        ctx.globalAlpha = s.mode > 0 ? 1 : theme.filterCurveOffAlpha
        ctx.stroke()
        ctx.globalAlpha = 1
    }
}
