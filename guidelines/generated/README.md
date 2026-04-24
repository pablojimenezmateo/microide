# Generated Artifacts

Purpose: record rules for checked-in generated files and reference artifacts used by `microide`.

## Examples

- generated syntax snapshots
- benchmark outputs kept as references
- screenshots or comparison images used by docs
- trace captures or profiling reference data
- fixture manifests generated from committed scripts

## Rules

- Record the generator, script, or source command.
- Record the generation date when the artifact is intended as a durable reference.
- Record the key inputs, config, or fixture source.
- Treat generated files as supporting references, not the source of truth for behavior.
- When regeneration is repeatable, prefer checking in the script or instructions alongside the artifact.
