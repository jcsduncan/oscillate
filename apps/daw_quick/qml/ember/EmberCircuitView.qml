pragma ComponentBehavior: Bound
import QtQuick
import Oscillate.Format

// The eight voice cards as the engine reports them (ADR-0066): each card's
// VCO B pitch error as a bar from the centre line (sharp up, flat down) and
// DCO A's as a tick, lit while the card sounds. Without telemetry it shows
// the cards at rest.
Rectangle {
    id: view
    required property EmberTheme theme
    // controller.telemetry()'s map, or {}.
    property var live: ({})
    readonly property var cards: live.cards !== undefined ? live.cards : []
    readonly property int cardCount: 8
    // Cents from the centre line to the top or bottom of the well.
    readonly property real range: 20
    // Whether the cards are the engine's own (telemetry) or at rest.
    readonly property bool reporting: cards.length > 0
    // The pitch error scale the frame labels (cents), from this view's own
    // mapping: the plot sits spaceXs inside the well, over the card numbers.
    readonly property var centsMarks: [range, 0, -range]
    readonly property var centsTicks: {
        const t = theme
        const plotHeight = height - 2 * t.spaceXs - t.cardNumberRow
        if (!(height > 0) || !(plotHeight > 0))
            return []
        const mid = plotHeight / 2
        return centsMarks.map(cents => ({
            at: 1 - (t.spaceXs + mid - Math.max(-1, Math.min(1, cents / range)) * (mid - t.cardPlotHeadroom)) / height,
            label: Format.signed_number(cents, 0) }))
    }

    radius: theme.viewRadius
    color: theme.well
    border.color: theme.engraving
    Accessible.role: Accessible.Graphic
    Accessible.name: "Voice cards: pitch against ideal"
    onLiveChanged: plot.requestPaint()

    Canvas {
        id: plot
        anchors.fill: parent
        anchors.margins: view.theme.spaceXs
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = view.theme
            const w = width
            const h = height - t.cardNumberRow
            const mid = h / 2
            const column = w / view.cardCount
            ctx.strokeStyle = view.theme.engraving
            ctx.lineWidth = t.hairline
            ctx.beginPath()
            ctx.moveTo(0, mid)
            ctx.lineTo(w, mid)
            ctx.stroke()
            const y = cents => mid - Math.max(-1, Math.min(1, cents / view.range)) * (mid - t.cardPlotHeadroom)
            for (let c = 0; c < view.cardCount; ++c) {
                const card = view.cards[c]
                const active = card !== undefined && card.active
                const x = c * column + column / 2
                ctx.globalAlpha = active ? 1 : t.cardIdleAlpha
                if (card !== undefined) {
                    ctx.fillStyle = view.theme.ember
                    const top = Math.min(mid, y(card.vco_cents))
                    ctx.fillRect(x - t.cardBarWidth / 2, top, t.cardBarWidth, Math.max(1, Math.abs(y(card.vco_cents) - mid)))
                    ctx.fillStyle = view.theme.teal
                    ctx.fillRect(x - t.cardTickWidth / 2, y(card.dco_cents) - t.cardTickHeight / 2, t.cardTickWidth, t.cardTickHeight)
                }
                ctx.fillStyle = view.theme.faint
                ctx.font = t.typeMicro + "px sans-serif"
                ctx.textAlign = "center"
                ctx.fillText(c + 1, x, height - t.cardNumberInset)
            }
            ctx.globalAlpha = 1
        }
    }
}
