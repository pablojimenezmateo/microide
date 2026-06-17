# Split `document_->layout_revision` into tiered revisions

- Date: 2026-05-15
- Area: editor, rendering
- Source: §14; openspec change `split-layout-revision-tiers` (commit `a0fdc58`);
  `dev-docs/performance/investigations/performance-bottleneck-deep-dive-{2,3,4}.md`

## Summary

A single `document_->layout_revision` bumped on every edit invalidated four logically independent
caches (visible-line layout, syntax highlight, fold model, presentation); it was split into tiered
revisions so each derived cache keys on the minimum tier it actually depends on.

## Impact

Medium-to-high. A one-character insertion past the visible region used to cascade derived-cache
wipes across the suffix of the document. The round-4 lazy-invalidation cursors made the reset O(1),
but readers still recomputed unnecessarily because scrolls and non-content edits bumped the same
revision.

## Resolution

Closed on 2026-05-15 by `split-layout-revision-tiers` (commit `a0fdc58`). `TextViewport::DocumentState`
now exposes four tiered revisions (`content_revision`, `syntax_revision`, `layout_shape_revision`,
`presentation_revision`) plus an `InvalidationReason` enum on the rewritten
`InvalidateDerivedCaches(reason, start_line)` entry point; every derived cache (wrapped-row layouts,
highlight, bracket-match, indent-guides, occurrence seed/scan, status-bar language, folding
fingerprint) keys on the minimum tier set it depends on.

The architectural-lint test hard-fails on reintroduction of a combined `layout_revision` member on
`DocumentState`. Per-tier perf counters
(`editor.{content,syntax,layout_shape,presentation}_revision_bumps`) are in place. The
`editor_scroll_only_no_content_bump` perf scenario and updated baselines were deferred follow-ups
tracked in that change's `tasks.md`.
