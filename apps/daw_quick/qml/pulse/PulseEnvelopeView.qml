pragma ComponentBehavior: Bound
import QtQuick
import "../devicekit" as Kit

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

    function timeText(s) {
        return s < 1 ? Math.round(s * 1000) + " ms" : s.toFixed(2) + " s"
    }

    Text {
        x: view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: "AMP ENVELOPE"
        color: view.theme.envelopeLabelInk
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
        font.letterSpacing: view.theme.envelopeLabelSpacing
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.envelopeLabelInset
        y: view.theme.envelopeLabelTop
        text: "−60 dB @ " + view.timeText(view.decay)
        color: view.accent
        font.pixelSize: view.theme.envelopeLabelSize
        font.bold: true
    }
    Text {
        x: view.theme.envelopeLabelInset
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.theme.envelopeLabelBottom
        text: "0"
        color: view.theme.envelopeLabelInk
        font.pixelSize: view.theme.envelopeLabelSize
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: view.theme.envelopeLabelInset
        anchors.bottom: parent.bottom
        anchors.bottomMargin: view.theme.envelopeLabelBottom
        text: view.timeText(view.seconds)
        color: view.theme.envelopeLabelInk
        font.pixelSize: view.theme.envelopeLabelSize
    }
}
