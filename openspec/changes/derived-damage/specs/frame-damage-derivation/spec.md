# Spec Delta

## Purpose

How a prepared frame decides which regions of the window to repaint: from what each
surface will paint compared with what it last painted, so a change to anything on
screen is repainted whether or not the code that made it asked for a repaint.

## ADDED Requirements

### Requirement: Damage is derived from paint snapshots

For every migrated surface, frame preparation SHALL build the surface's paint
snapshot -- the complete set of values its paint reads -- at the surface's damage
grain (the whole surface, a strip, or one editor row), and SHALL damage every unit
whose snapshot differs from the snapshot of the frame that last painted it, plus the
old and new rect of any unit that moved, appeared or disappeared.

#### Scenario: A change no code path announced is repainted
- **WHEN** state a migrated surface paints changes and no code requests a repaint
  for it (a background thread, a control command, a plugin, a watcher event)
- **THEN** the next prepared frame repaints that unit, and the QA redraw verifier
  finds the partial frame equal to a full render

#### Scenario: Nothing changed paints nothing
- **WHEN** a frame is prepared and no migrated unit's snapshot changed
- **THEN** frame preparation adds no damage for migrated surfaces

### Requirement: A migrated surface paints only its snapshot

A migrated surface's paint SHALL read only its frame's snapshot and immutable
resources (theme, fonts, glyph caches); it SHALL NOT read live workspace state, the
clock, or another thread's state. The architecture lint SHALL reject a migrated
render unit that does.

#### Scenario: Paint reaching past its snapshot fails the build's lint
- **WHEN** a migrated surface's render unit reads project state, a coordinator or a
  service during paint
- **THEN** the architecture invariants test fails, naming the unit and the access

### Requirement: Snapshots fit the performance budget

Building and comparing snapshots SHALL stay within the paint budgets of
`performance-budgets`: a migration stage SHALL NOT regress a paint scenario's p50 by
more than 5% nor add allocations to a steady-state frame, measured base against head
on the same machine and disk.

#### Scenario: A stage that costs too much does not land
- **WHEN** a stage's base-versus-head perf run shows a paint scenario's p50 up by
  more than 5%, or new steady-state allocations
- **THEN** the stage is redesigned (coarser grain, cached inputs, revision keys)
  before it merges
