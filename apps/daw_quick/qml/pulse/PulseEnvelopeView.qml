pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit
import Oscillate.Format

// The selected pad's amplitude over time, from the Pulse kernel's envelope
// laws (controller.pad_envelope), drawn on the display strip's LCD with the
// -60 dB point dashed.
Kit.EnvelopeView {
    id: view
    required property int pad
    required property string decayId

    // The decay shown before the controller has values (seconds).
    readonly property real restingDecay: 0.5
    // Show a little past the -60 dB point so the tail lands inside the
    // view, within these bounds (seconds).
    readonly property real tailMargin: 1.2
    readonly property real shortestView: 0.12
    readonly property real longestView: 4.5
    readonly property int samples: 160
    readonly property real decay: controller.revision >= 0 ? controller.value(decayId) : restingDecay
    readonly property real seconds: Math.max(shortestView, Math.min(longestView, decay * tailMargin))

    objectName: "pulseEnvelope"
    levels: controller.revision >= 0 ? controller.pad_envelope(pad, seconds, samples) : []
    markers: [{ x: Math.min(1, decay / seconds) }]
    Accessible.name: "Amplitude envelope"
    Accessible.description: "Falls 60 dB in " + controller.display(decayId)

    // The time scale for the plot's frame: the start, the middle and the
    // end of the view, placed where the plot draws them.
    readonly property var timeTickShares: [0, 0.5, 1]
    readonly property var timeTicks: width <= 0 ? [] : timeTickShares.map(f => ({
        at: plotX(f) / width, label: f === 0 ? "0" : Format.number(f * seconds) }))

    function timeText(s) {
        return Format.seconds(s)
    }

    Text {
        id: caption
        x: view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: "AMP ENVELOPE"
        color: view.theme.envelopeLabelInk
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
        font.letterSpacing: view.theme.envelopeLabelSpacing
    }
    // Beside the caption, clear of the frame's PREVIEW chip.
    Text {
        x: caption.x + caption.width + view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: "−60 dB @ " + view.timeText(view.decay)
        color: view.accent
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
    }
}
