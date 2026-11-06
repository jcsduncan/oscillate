.pragma library

// Voice Lanes <-> the document: the one place the panel names the document
// model's (controller.document) analogue-lane invokables. A lane is `kind`
// ("notes" or "unison"), `osc` (the oscillator index for unison, 0 for
// notes) and a lane id (NovaContract.voiceLanes); each voice holds a value
// -1..1, seeded unless painted.

var notes = "notes"
var unison = "unison"

// Changes whenever a lane may read differently: a painted value or the
// seed, Age and tolerances behind the seeded ones.
function revision(doc, controller) {
    return doc.analogue_revision + controller.revision
}

// `count` voices' {value, edited}.
function read(doc, kind, osc, lane, count) {
    return doc.analogueLane(kind, osc, lane, count)
}

// One voice's value: one edit, or an update inside an open gesture.
function write(doc, kind, osc, lane, voice, value) {
    doc.setAnalogueLaneValue(kind, osc, lane, voice, value)
}

// Every voice's value in `values` as one undo step.
function writeAll(doc, kind, osc, lane, values) {
    doc.begin_edit()
    for (var v = 0; v < values.length; ++v)
        doc.setAnalogueLaneValue(kind, osc, lane, v, values[v])
    doc.commit_edit()
}

// Back to seed: drops the lane's painted values.
function clear(doc, kind, osc, lane) {
    doc.clearAnalogueLane(kind, osc, lane)
}

// A paint drag is one document gesture (begin, updates, commit).
function begin(doc) { doc.begin_edit() }
function commit(doc) { doc.commit_edit() }
function cancel(doc) { doc.cancel_edit() }
