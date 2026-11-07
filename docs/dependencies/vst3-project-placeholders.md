# VST3 project placeholder contract (schema 1.7)

This is the durable project-model contract for PI-04. It does not load or execute a plugin.
The application must verify the stored class and binary identity against a fresh scanner
receipt before creating a host instance. The stored bundle path is the original,
advisory location; loading it on project restore is forbidden.

Each track has an ordered `vst3_plugins` array. Array order is insert-chain position.
Each entry contains a stable instance ID, lowercase 32-hex VST3 class ID, vendor,
name, version, original absolute `.vst3` bundle path, lowercase 64-hex binary
SHA-256, canonical base64 component-state bytes, parameter references (`id` and
normalized value), and a `missing` boolean. Unknown fields and malformed values
fail the entire manifest read. Missing placeholders retain their exact identity,
state, parameter references and chain position. Availability is not probed during
decode; `missing` changes only through a typed project command.

Limits are 32 plugin entries per track, 2,048 per project, 4 MiB component state
per instance, 8 MiB total component state, 256 distinct parameter references per
instance, 1,024 UTF-8 bytes for a bundle path and 128 UTF-8 bytes for each
metadata string. Parameter values must be finite and within `[0, 1]`. Paths must
be absolute, end in `.vst3` and contain no dot or parent traversal segments.
Component-state base64 must be canonical; malformed encodings fail before project
publication. The existing 16 MiB manifest limit also applies.

Project commands insert, remove, reorder, replace component state, replace
parameter references, or set missing status. One `apply` call publishes one
revision and undo step; a rejected command publishes none. Undo and redo restore
the complete snapshot, including the original opaque bytes and chain order.
This model records desired processing only; it cannot claim that the current
audio graph contains the plugin.

Schema 1.7 always writes `vst3_plugins`, including an empty array. Readers accept
1.0 through 1.6 without that field and synthesize empty chains; saving such a
project writes 1.7. A 1.7 document missing the field, or an older document
containing it, fails the strict schema check. There is no binary state migration:
opaque bytes stay intact for later host restoration or user relinking.
