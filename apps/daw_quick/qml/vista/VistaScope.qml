import QtQuick

// The vectorscope: mid up, side across (left channel upper left, right
// upper right), so a mono source is a vertical line, a wide one a broad
// cloud and an out-of-phase one lies on its side. Live points (mid, side
// pairs from Vista's telemetry) glow and fade over a few frames; the scale follows the
// level so quiet material still fills the glass. With nothing playing it
// shows a preview cloud for the settings: a reference source, half
// correlated, at the panel's width.
Item {
    id: scope
    required property VistaTheme theme
    // Flat [mid, side, mid, side, ...] from the newest telemetry frame.
    property var points: []
    property bool live: false
    // Side-to-mid amplitude of the preview cloud.
    property real previewRatio: 0.58
    // Points drawn on the last live paint, for tests.
    property int pointCount: 0
    // Smoothed peak the live scale follows.
    property real peak: 0.25
    // Glass geometry: where the sheen's centre sits (fraction of the
    // centre's height) and its inner radius (fraction of the glass), the
    // rings' radii, and the rim letters' distance outside the glass.
    readonly property real sheenY: 0.8
    readonly property real sheenRadius: 0.1
    readonly property var rings: [0.25, 0.5, 0.75]
    readonly property real topLetterGap: 7
    readonly property real rimLetterGap: 8
    // Live scale: never below -60 dBFS, smoothing on the way down, and the
    // fraction of the glass the peak fills.
    readonly property real peakFloor: 0.001
    readonly property real peakRelease: 0.92
    readonly property real peakFollow: 0.08
    readonly property real liveFill: 0.82
    // Preview cloud: its points, the fraction of the glass a unit spread
    // fills, and how fast its glow falls off from the centre.
    readonly property int previewPoints: 900
    readonly property real previewFill: 0.27
    readonly property real previewFalloff: 3

    objectName: "vistaScope"
    readonly property string plotExemptReason: "a vectorscope: its M/S and L/R axes are lettered on the glass and its scale follows the level, so it has no unit scale; the card note says signal or preview"
    implicitWidth: theme.scopeDefaultSize
    implicitHeight: theme.scopeDefaultSize

    onPointsChanged: if (live) trace.requestPaint()
    onLiveChanged: {
        trace.clear = true
        trace.requestPaint()
    }
    onPreviewRatioChanged: if (!live) trace.requestPaint()

    // The glass: a dark disc, rings, the M and S axes and the L and R
    // diagonals, lettered round the rim.
    Canvas {
        id: glass
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const t = scope.theme
            const c = width / 2
            const r = width / 2 - t.scopeRim
            const fill = ctx.createRadialGradient(c, c * scope.sheenY, r * scope.sheenRadius, c, c, r)
            fill.addColorStop(0, t.glassCentre)
            fill.addColorStop(1, t.well)
            ctx.fillStyle = fill
            ctx.beginPath()
            ctx.arc(c, c, r, 0, Math.PI * 2)
            ctx.fill()
            ctx.strokeStyle = t.gridStrong
            ctx.lineWidth = t.hairline
            ctx.stroke()
            ctx.strokeStyle = t.grid
            for (const k of scope.rings) {
                ctx.beginPath()
                ctx.arc(c, c, r * k, 0, Math.PI * 2)
                ctx.stroke()
            }
            // Axes: M vertical, S horizontal, L and R on the diagonals.
            const axis = (angle, strong) => {
                ctx.strokeStyle = strong ? t.gridStrong : t.grid
                ctx.beginPath()
                ctx.moveTo(c - Math.cos(angle) * r, c - Math.sin(angle) * r)
                ctx.lineTo(c + Math.cos(angle) * r, c + Math.sin(angle) * r)
                ctx.stroke()
            }
            axis(Math.PI / 2, true)
            axis(0, false)
            axis(Math.PI / 4, false)
            axis(-Math.PI / 4, false)
            ctx.fillStyle = t.inkDim
            ctx.font = "bold " + t.typeCaption + "px '" + t.sans + "'"
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            ctx.fillText("M", c, c - r - scope.topLetterGap)
            ctx.fillText("−S", c - r - scope.rimLetterGap, c)
            ctx.fillText("+S", c + r + scope.rimLetterGap, c)
            ctx.fillStyle = t.inkFaint
            const d = (r + scope.rimLetterGap) / Math.SQRT2
            ctx.fillText("L", c - d, c - d)
            ctx.fillText("R", c + d, c - d)
        }
    }

    // The trace, painted over itself so old points fade.
    Canvas {
        id: trace
        property bool clear: true
        anchors.fill: parent
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            const t = scope.theme
            const c = width / 2
            const r = width / 2 - t.scopeRim
            if (clear || !scope.live) {
                ctx.reset()
                clear = false
            } else {
                // Persistence: fade what was drawn before.
                ctx.globalCompositeOperation = "destination-out"
                ctx.fillStyle = t.scopePersistenceFade
                ctx.fillRect(0, 0, width, height)
                ctx.globalCompositeOperation = "source-over"
            }
            ctx.save()
            ctx.beginPath()
            ctx.arc(c, c, r - t.hairline, 0, Math.PI * 2)
            ctx.clip()
            if (scope.live) {
                const p = scope.points
                const n = Math.floor(p.length / 2)
                let loudest = 0
                for (let i = 0; i < n; ++i)
                    loudest = Math.max(loudest, Math.abs(p[2 * i]), Math.abs(p[2 * i + 1]))
                // Fast to grow, slow to shrink, never below -60 dBFS.
                scope.peak = Math.max(scope.peakFloor, loudest > scope.peak ? loudest
                                                                  : scope.peak * scope.peakRelease + loudest * scope.peakFollow)
                const k = scope.liveFill * r / scope.peak
                // Each point twice: a soft ice halo, then a glacier core.
                const halo = t.scopeHaloDot
                const core = t.scopeCoreDot
                ctx.fillStyle = t.scopeHalo
                for (let i = 0; i < n; ++i)
                    ctx.fillRect(c - p[2 * i + 1] * k - halo / 2, c - p[2 * i] * k - halo / 2, halo, halo)
                ctx.fillStyle = t.scopeCore
                for (let i = 0; i < n; ++i)
                    ctx.fillRect(c - p[2 * i + 1] * k - core / 2, c - p[2 * i] * k - core / 2, core, core)
                scope.pointCount = n
            } else {
                // A deterministic cloud: Gaussian mid, side at the ratio.
                let state = 12345 >>> 0
                const uniform = () => {
                    state = (Math.imul(state, 1664525) + 1013904223) >>> 0
                    return ((state >>> 8) + 0.5) / 16777216
                }
                const gauss = () => Math.sqrt(-2 * Math.log(uniform())) * Math.cos(2 * Math.PI * uniform())
                const scale = scope.previewFill * r / Math.max(1, scope.previewRatio)
                const dot = t.scopePreviewDot
                for (let i = 0; i < scope.previewPoints; ++i) {
                    const m = gauss()
                    const s = gauss() * scope.previewRatio
                    const a = t.previewAlphaFloor + t.previewAlphaPeak * Math.exp(-(m * m + s * s) / scope.previewFalloff)
                    ctx.fillStyle = Qt.rgba(t.accentSoft.r, t.accentSoft.g, t.accentSoft.b, a)
                    ctx.fillRect(c - s * scale - dot / 2, c - m * scale - dot / 2, dot, dot)
                }
                scope.pointCount = 0
            }
            ctx.restore()
        }
    }
}
