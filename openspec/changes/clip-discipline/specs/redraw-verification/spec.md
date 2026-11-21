# Spec Delta

## ADDED Requirements

### Requirement: Paint overruns are reported where they happen

With the verifier on, a paint scope SHALL record each draw call that falls outside
its element's rect, once per element per frame, naming the element and the
overrun. A redraw test SHALL fail on a recorded overrun, and the QA driver SHALL
file one as a `paint-overrun` finding keyed on the element's kind. With the
verifier off, recording SHALL cost nothing beyond the scope's clip.

#### Scenario: An element draws one pixel past its rect
- **WHEN** a test paints an element whose fill extends one pixel past its layout rect
- **THEN** the redraw test fails naming the element and the one-pixel overrun, without a stale frame being needed to find it
