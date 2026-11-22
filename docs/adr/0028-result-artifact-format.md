# 0028. Result artifact format: JSON, polylines in degrees

- **Status:** Accepted
- **Date:** 2026-11-22
- **Related:** 0008, 0015, 0016, 0020, 0025, 0027

## Context

FINALIZE writes one result artifact per job (0016). The C++ worker writes
it, the Python client reads it, and people inspect it. Its polylines make up
most of it: for 5,000 customers, perhaps a few hundred thousand points (to
be measured).

## Decision

- **Format.** One UTF-8 JSON file with a `schema_version`, specified in
  `docs/formats/result.md` together with a golden file.
- **Polylines.** Arrays of `[latitude, longitude]` in degrees with 7
  decimals, the graph's own precision (0027), so every point maps back to
  the graph's integers exactly.
- **Parameters.** Every solver parameter is listed by name with the value
  used, defaults included, so a result does not depend on knowing what a
  build's defaults were.
- **Writer.** vrp-core writes the file without a JSON library: it holds only
  numbers and escaped strings.

## Alternatives considered

- **Protobuf, as on the control plane (0008):** compact, but not readable
  without the schema and generated code.
- **GeoJSON:** map tools read it directly, but the convergence histories and
  workers do not fit its model; the client converts routes in a few lines.
- **The graph files' binary container (0027):** smallest, but needs a
  reader in the client for a file of a few MB.

## Consequences

- Gain: readable, parsed by any language, defined by the spec rather than
  by either implementation.
- Cost: several times larger than a binary file; a spec and a golden file
  to maintain.
- Revisit: compression, such as gzip, if downloads become slow.

## Confirmation

The C++ writer reproduces the golden file byte for byte in CI, and the
client's tests read the same file.
