import QtQuick
import Oscillate.Format

// The isolator's response, as it runs: each band's own curve, filled in its
// colour, under the summed curve, on a log-frequency screen from 20 Hz to
// 20 kHz and -40..+6 dB. The bands are the fourth-order Linkwitz-Riley
// splits Deck uses, with the low band behind the high crossover's allpass,
// evaluated as complex responses so the sum is the one you hear.
Canvas {
    id: view
    required property var controller
    required property DeckTheme theme

    readonly property int revision: controller.revision
    readonly property real minDb: -40
    readonly property real maxDb: 6
    readonly property var levels: revision >= 0 ? [band("low"), band("mid"), band("high")] : [1, 1, 1]
    readonly property real lowX: revision >= 0 ? controller.value("deck.low_x") : 250
    readonly property real highX: revision >= 0 ? controller.value("deck.high_x") : 2500

    onRevisionChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    // A band's linear weight: its gain, or silence at the bottom stop or killed.
    function band(name) {
        if (controller.choice("deck." + name + "_kill") === 1)
            return 0
        const db = controller.value("deck." + name)
        return db <= minDb ? 0 : Math.pow(10, db / 20)
    }
    // The scales the frame around the screen labels, from this view's own
    // mapping: frequency marks [Hz, label] and levels in dB.
    readonly property var frequencyMarks: [[20, "20"], [100, "100"], [1000, "1k"], [10000, "10k"], [20000, "20k"]]
    readonly property var levelMarks: [6, 0, -12, -24, -40]
    readonly property var frequencyTicks: width > 0
        ? frequencyMarks.map(mark => ({ at: xOf(mark[0]) / width, label: mark[1] })) : []
    readonly property var levelTicks: height > 0
        ? levelMarks.map(db => ({ at: 1 - yOf(db) / height, label: Format.signed_number(db, 0) })) : []

    function xOf(hz) { return width * Math.log(hz / 20) / Math.log(1000) }
    function yOf(db) { return height * (maxDb - Math.max(minDb, Math.min(maxDb, db))) / (maxDb - minDb) }

    // Complex helpers: [re, im].
    function mul(a, b) { return [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]] }
    function div(a, b) {
        const d = b[0] * b[0] + b[1] * b[1]
        return [(a[0] * b[0] + a[1] * b[1]) / d, (a[1] * b[0] - a[0] * b[1]) / d]
    }
    // Butterworth second order at s = j f / fc, squared: LR4.
    function lr4(f, fc, high) {
        const w = f / fc
        const den = [1 - w * w, Math.SQRT2 * w]
        const num = high ? [-w * w, 0] : [1, 0]
        const one = div(num, den)
        return mul(one, one)
    }
    function responses(f) {
        const lowLp = lr4(f, lowX, false)
        const lowHp = lr4(f, lowX, true)
        const highLp = lr4(f, highX, false)
        const highHp = lr4(f, highX, true)
        const allpass = [highLp[0] + highHp[0], highLp[1] + highHp[1]]
        return [mul(lowLp, allpass), mul(lowHp, highLp), mul(lowHp, highHp)]
    }
    function db(c) {
        const m = Math.sqrt(c[0] * c[0] + c[1] * c[1])
        return m > 0 ? 20 * Math.log10(m) : -200
    }

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.fillStyle = theme.screen
        ctx.fillRect(0, 0, width, height)
        // Graticule: decades and every 6 dB, 0 dB brighter.
        ctx.lineWidth = theme.hairline
        for (const hz of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
            ctx.strokeStyle = (hz === 100 || hz === 1000 || hz === 10000) ? theme.gridStrong : theme.grid
            ctx.beginPath()
            ctx.moveTo(Math.round(xOf(hz)) + theme.hairline / 2, 0)
            ctx.lineTo(Math.round(xOf(hz)) + theme.hairline / 2, height)
            ctx.stroke()
        }
        for (let d = maxDb; d >= minDb; d -= 6) {
            ctx.strokeStyle = d === 0 ? theme.zero : theme.grid
            ctx.beginPath()
            ctx.moveTo(0, Math.round(yOf(d)) + theme.hairline / 2)
            ctx.lineTo(width, Math.round(yOf(d)) + theme.hairline / 2)
            ctx.stroke()
        }
        const columns = Math.max(2, Math.floor(width / 2))
        const sampled = []
        for (let i = 0; i <= columns; ++i) {
            const f = 20 * Math.pow(1000, i / columns)
            sampled.push({ x: width * i / columns, bands: responses(f) })
        }
        // Each band, filled to the floor.
        for (let k = 0; k < 3; ++k) {
            ctx.beginPath()
            ctx.moveTo(0, height)
            for (const s of sampled) {
                const c = s.bands[k]
                ctx.lineTo(s.x, yOf(levels[k] > 0 ? db(c) + 20 * Math.log10(levels[k]) : minDb))
            }
            ctx.lineTo(width, height)
            ctx.closePath()
            ctx.globalAlpha = theme.bandFillAlpha
            ctx.fillStyle = theme.bands[k]
            ctx.fill()
            ctx.globalAlpha = theme.bandLineAlpha
            ctx.strokeStyle = theme.bands[k]
            ctx.lineWidth = theme.hairline
            ctx.stroke()
        }
        // The sum you hear.
        ctx.globalAlpha = 1
        ctx.beginPath()
        for (let i = 0; i < sampled.length; ++i) {
            const b = sampled[i].bands
            const sum = [0, 0]
            for (let k = 0; k < 3; ++k) {
                sum[0] += b[k][0] * levels[k]
                sum[1] += b[k][1] * levels[k]
            }
            const y = yOf(db(sum))
            if (i === 0)
                ctx.moveTo(sampled[i].x, y)
            else
                ctx.lineTo(sampled[i].x, y)
        }
        ctx.strokeStyle = theme.curve
        ctx.lineWidth = theme.strokeBold
        ctx.stroke()
        // The crossovers.
        ctx.setLineDash(theme.crossoverDash)
        ctx.strokeStyle = theme.inkDim
        ctx.lineWidth = theme.hairline
        for (const hz of [lowX, highX]) {
            ctx.beginPath()
            ctx.moveTo(Math.round(xOf(hz)) + theme.hairline / 2, 0)
            ctx.lineTo(Math.round(xOf(hz)) + theme.hairline / 2, height)
            ctx.stroke()
        }
        ctx.setLineDash([])
    }
}
