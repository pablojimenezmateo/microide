# Spec Delta

## Purpose

How a workspace element is painted so that a partial frame and a full frame draw
the same pixels: inside its own scope on its layout rect, with one device-pixel
snapping rule for clips, fills and damage, and with any draw outside the rect
reported as a defect.

## ADDED Requirements

### Requirement: An element paints inside its own scope

A render function in a migrated surface SHALL draw each element (a row, a tab, a
cell, a divider, a button, a separator) inside a paint scope on that element's
layout rect. The scope SHALL clip drawing to the rect, intersected with the
frame's clip. An element declared opaque SHALL fill its rect with its background
before anything else is drawn in it.

#### Scenario: A label longer than its tab
- **WHEN** a tab's label is wider than the tab
- **THEN** the label is fitted to the tab before it is drawn, and no pixel of it falls outside the tab in a full or a partial frame

#### Scenario: Text replaced in an opaque row
- **WHEN** an opaque row's text changes and only that row is repainted
- **THEN** the partial frame shows the new text alone, as a full frame does

### Requirement: One device-pixel rule for clips, fills and damage

The device-pixel rect of a clip, of a fill inside a paint scope and of a damage
request for the same logical rect SHALL be computed by one function, so that they
cover the same device pixels at every UI scale.

#### Scenario: A divider at a fractional scale
- **WHEN** a divider at scale 1.5 is damaged and repainted
- **THEN** the damage covers every device pixel the divider's fill touches

### Requirement: Elements tile their strip

The elements of a strip (tab strips, the panel tab row, toolbars) SHALL tile it:
every pixel of the strip belongs to exactly one element, gaps and separators
included.

#### Scenario: The gap between two tabs
- **WHEN** the pointer moves from one tab to the next
- **THEN** the pixels between them belong to an element that is repainted with them, and the partial frame matches a full one

### Requirement: A wide glyph widens its cell

A glyph wider than its terminal or editor cell SHALL widen that cell's rect, and
so its damage, to the glyph's advance.

#### Scenario: The cursor on an emoji
- **WHEN** the terminal cursor blinks on a two-column emoji
- **THEN** the blink's damage covers the whole glyph
