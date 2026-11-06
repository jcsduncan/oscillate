pragma ComponentBehavior: Bound
import QtQuick

// Energy through the song: short-term loudness per bar as a white line over
// the song's sections (drops tinted teal, each section labelled). When a
// later drop sits under the first, an amber dashed line at the first drop's
// level shows by how much.
Canvas {
    id: energyView
    required property MixCheckTheme style
    // [{ bar, lufs }]
    property var energy: []
    // [{ name, firstBar, lastBar, lufs }]
    property var sections: []

    readonly property real headroomLu: 1.5
    readonly property real dropGapLu: 0.05
    // The scale before a first measurement, in LUFS.
    readonly property real fallbackQuietLufs: -14
    readonly property real fallbackLoudLufs: -6

    implicitHeight: style.energyHeight
    onEnergyChanged: requestPaint()
    onSectionsChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    Accessible.role: Accessible.Graphic
    Accessible.name: "Loudness per bar through the song's sections"

    function isDrop(section) {
        return /^drop/i.test(style.text(section ? section.name : ""))
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        const style = energyView.style
        const w = width
        const h = height
        const points = style.list(energy)
        const parts = style.list(sections)
        if (w <= 0 || h <= 0 || (points.length === 0 && parts.length === 0))
            return

        let firstBar = Infinity
        let lastBar = -Infinity
        let quiet = Infinity
        let loud = -Infinity
        for (let i = 0; i < points.length; ++i) {
            const bar = Number(style.map(points[i]).bar)
            const lufs = Number(style.map(points[i]).lufs)
            if (isFinite(bar)) {
                firstBar = Math.min(firstBar, bar)
                lastBar = Math.max(lastBar, bar)
            }
            if (isFinite(lufs)) {
                quiet = Math.min(quiet, lufs)
                loud = Math.max(loud, lufs)
            }
        }
        for (let i = 0; i < parts.length; ++i) {
            const a = Number(style.map(parts[i]).firstBar)
            const b = Number(style.map(parts[i]).lastBar)
            if (isFinite(a))
                firstBar = Math.min(firstBar, a)
            if (isFinite(b))
                lastBar = Math.max(lastBar, b)
        }
        if (!isFinite(firstBar) || !(lastBar > firstBar))
            return
        if (!isFinite(quiet)) {
            quiet = fallbackQuietLufs
            loud = fallbackLoudLufs
        }
        quiet -= headroomLu
        loud += headroomLu
        const xOfBar = bar => (bar - firstBar) / (lastBar - firstBar) * w
        const yOf = lufs => style.energyPlotTop + (loud - lufs) / (loud - quiet)
                            * (h - style.energyPlotTop - style.energyPlotBottom)

        // Sections.
        ctx.font = "bold " + style.typeSmall + "px \"" + style.sansFamily + "\""
        for (let i = 0; i < parts.length; ++i) {
            const part = style.map(parts[i])
            const a = Number(part.firstBar)
            const b = Number(part.lastBar)
            if (!isFinite(a) || !isFinite(b))
                continue
            const x0 = xOfBar(a)
            const x1 = xOfBar(b)
            if (isDrop(part)) {
                ctx.fillStyle = style.alpha(style.accent, style.energyDropAlpha)
                ctx.fillRect(x0, 0, x1 - x0, h)
            }
            ctx.fillStyle = style.line
            ctx.fillRect(Math.round(x0), 0, style.hairline, h)
            ctx.fillStyle = style.textFaint
            ctx.fillText(style.text(part.name), x0 + style.spaceXs, style.plotLabelBaseline)
        }

        // Loudness per bar.
        let started = false
        ctx.beginPath()
        for (let i = 0; i < points.length; ++i) {
            const bar = Number(style.map(points[i]).bar)
            const lufs = Number(style.map(points[i]).lufs)
            if (!isFinite(bar) || !isFinite(lufs))
                continue
            if (started) {
                ctx.lineTo(xOfBar(bar), yOf(lufs))
            } else {
                ctx.moveTo(xOfBar(bar), yOf(lufs))
                started = true
            }
        }
        ctx.strokeStyle = style.afterCurve
        ctx.lineWidth = style.energyLineWidth
        ctx.stroke()

        // A later drop quieter than the first.
        const drops = []
        for (let i = 0; i < parts.length; ++i) {
            if (isDrop(parts[i]) && isFinite(Number(style.map(parts[i]).lufs)))
                drops.push(parts[i])
        }
        if (drops.length >= 2) {
            const first = drops[0]
            let quietest = drops[1]
            for (let i = 2; i < drops.length; ++i) {
                if (Number(drops[i].lufs) < Number(quietest.lufs))
                    quietest = drops[i]
            }
            const gap = Number(first.lufs) - Number(quietest.lufs)
            if (gap > dropGapLu) {
                const y = yOf(Number(first.lufs))
                ctx.strokeStyle = style.amber
                ctx.lineWidth = style.hairline
                ctx.setLineDash(style.energyDropDash)
                ctx.beginPath()
                ctx.moveTo(xOfBar(Number(first.firstBar)), y)
                ctx.lineTo(xOfBar(Number(quietest.lastBar)), y)
                ctx.stroke()
                ctx.setLineDash([])
                ctx.fillStyle = style.amber
                ctx.font = "bold " + style.typeBody + "px \"" + style.monoFamily + "\""
                const quietestStart = xOfBar(Number(quietest.firstBar))
                ctx.fillText("−" + gap.toFixed(1) + " LU", Math.min(w - style.energyGapLabelWidth, quietestStart + style.spaceXs),
                             y - style.spaceSm)
            }
        }
    }
}
