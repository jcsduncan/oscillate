import QtQuick
import "../devicekit" as Kit
import "CrateContract.js" as Contract

// The selected chop's AHDSR as an outline over a soft fill, in the chop's
// colour. Stage widths follow the square root of their times (a 1 ms attack
// still shows), with the sustain drawn at a fixed share.
Kit.EnvelopeView {
    id: view
    // The chop's parameter ids by control ("attack" -> "crate.chop.03.attack"), or {}.
    property var ids: ({})
    property color colour: theme.accent
    readonly property bool bound: ids.attack !== undefined && ids.attack !== ""
    readonly property var stages: {
        if (!bound || controller.revision < 0)
            return null
        return {
            attack: controller.value(ids.attack), hold: controller.value(ids.hold),
            decay: controller.value(ids.decay), sustain: controller.value(ids.sustain),
            release: controller.value(ids.release)
        }
    }
    // The four timed stages share what the sustain leaves.
    readonly property int timedStages: 4
    // The level scale the frame labels: silence and full level, where the
    // plot draws them (stage widths follow the square root of their times,
    // so time takes no scale).
    readonly property real fullLevelPercent: 100
    readonly property var levelTicks: height > 0
        ? [{ at: 1 - plotY(0) / height, label: "0" },
           { at: 1 - plotY(1) / height, label: String(fullLevelPercent) }] : []

    objectName: "crateEnvelope"
    accent: colour
    fillColor: colour
    points: stages === null ? [] : shape(stages)

    function shape(s) {
        const timed = [s.attack, s.hold, s.decay, s.release].map(ms => Math.sqrt(Math.max(0, ms)))
        const total = timed.reduce((a, b) => a + b, 0)
        const free = 1 - Contract.envelopeSustainShare
        const shares = timed.map(w => Math.max(Contract.envelopeMinShare, total > 0 ? w / total * free : free / timedStages))
        const scale = free / shares.reduce((a, b) => a + b, 0)
        const widths = shares.map(w => w * scale)
        let x = 0
        const points = [[0, 0]]
        x += widths[0]; points.push([x, 1])
        x += widths[1]; points.push([x, 1])
        x += widths[2]; points.push([x, s.sustain])
        x += Contract.envelopeSustainShare; points.push([x, s.sustain])
        x += widths[3]; points.push([Math.min(1, x), 0])
        return points
    }
}
