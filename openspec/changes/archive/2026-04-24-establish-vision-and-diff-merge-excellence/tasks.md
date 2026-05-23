## 1. Specs

- [x] 1.1 Create `specs/product-vision/spec.md` with product-thesis, priority-order, built-in-workflow, non-goal, and AI-in-scope requirements
- [x] 1.2 Create `specs/diff-merge-editor/spec.md` with unified-pipeline, no-size-degradation, compare-tab, merge-tab, state-preservation, and measured-diff requirements
- [x] 1.3 Create `specs/ai-workflows/spec.md` with chat-pane, inline-completion, provider-bridge, bounded-context, MCP-permissions, and no-shell-stall requirements
- [x] 1.4 Create `specs/performance-budgets/spec.md` with startup, typing/scroll, idle-CPU, background-work-isolation, and measured-before-merged requirements

## 2. Documentation Alignment

- [x] 2.1 Update `dev-docs/project/implementation-guide.md` to remove "AI/chat surfaces" from the explicit non-goals list and add a "See `openspec/specs/ai-workflows`" pointer in its place
- [x] 2.2 Update `dev-docs/project/implementation-guide.md` "Durable Product Decisions" section to reference `openspec/specs/product-vision` as the authoritative product-thesis source
- [x] 2.3 Update `dev-docs/project/implementation-guide.md` "Explicit Non-Goals" so it no longer contradicts `product-vision` (keep debugger-beyond-first-pass, plugin marketplaces, cloud/collaboration, recent-surfaces, soft wrap, native OS menus)
- [x] 2.4 Update `ROADMAP.md` "Medium-Term Phases > Diff and merge rewrite" section to cite `openspec/specs/diff-merge-editor` as the durable contract and `dev-docs/project/diff-editor-merge-rewrite-plan.md` as the implementation plan
- [x] 2.5 Update `ROADMAP.md` "Next Phase" AI wording to cite `openspec/specs/ai-workflows` instead of treating AI as validation-only work
- [x] 2.6 Update `CLAUDE.md` "Source Of Truth" ordering to include `openspec/specs/` as the product-contract layer above focused subsystem docs
- [x] 2.7 Update `guidelines/architecture.md` to reference `openspec/specs/product-vision` and `openspec/specs/diff-merge-editor` for scope and diff/merge ownership questions
- [x] 2.8 Update `guidelines/performance.md` to reference `openspec/specs/performance-budgets` for the durable budget contract

## 3. AGENTS.md Refresh

- [x] 3.1 Update `AGENTS.md` "Current Project Reality" so it reflects the shipped plugin runtime (not an "active phase") and lists AI workflows alongside editor / compare / merge / search / git / terminal as built-in product pillars
- [x] 3.2 Remove or demote the "Plugin-Phase Rules" section into a shorter "Plugin Rules" block that applies durably instead of framing plugin work as a time-boxed expansion
- [x] 3.3 Add a short "Diff, Merge, and AI" pillar block pointing at `openspec/specs/diff-merge-editor` and `openspec/specs/ai-workflows`
- [x] 3.4 Verify `AGENTS.md` still sits at the top of the source-of-truth order in `CLAUDE.md`, with `openspec/specs/` slotted directly under it as the product-contract layer

## 4. README.md Rewrite

- [x] 4.1 Replace the top of `README.md` with a short product pitch: "fastest low-footprint AI-focused IDE, C++/SDL3, no GPU required, first-class diff and merge, keyboard-first"
- [x] 4.2 Add a "Highlights" section organized by capability (Editing, Diff & Merge, Git, Terminal, AI, Plugins) instead of the current flat "Current State" bullet dump
- [x] 4.3 Preserve the essential keyboard and command reference but move it out of the top of the README into a `## Controls` and `## Commands` section lower down, or extract to a separate `docs/keybindings.md` if it gets too long
- [x] 4.4 Keep the Build section (CMake, SDL3, optional SDL3_ttf, Ubuntu-from-source steps) and the benchmark sections (`microide_diff_bench`, `microide_search_bench`, runtime profiling) but trim duplicated prose
- [x] 4.5 Add a "Scope" section that points at `openspec/specs/product-vision` for the authoritative in-scope / non-goal list
- [x] 4.6 Remove stale or superseded wording: "Initial C++/SDL3 skeleton for the MicroIDE rewrite" is no longer accurate

## 5. Stale docs/ Audit

- [x] 5.1 Audit `dev-docs/archive/chat-pane-plan.md`: the chat pane is shipped per `dev-docs/project/active-work.md` Phase 5. Move it to `dev-docs/archive/chat-pane-plan.md` (create the directory) or delete it if nothing references it
- [x] 5.2 Audit `docs/vscode-extension-compatibility-plan.md`: confirm whether this is aspirational or in scope given the new `product-vision` spec. If aspirational and not tied to a committed phase, archive it; if still a real plan, add a pointer from `ROADMAP.md`
- [x] 5.3 Audit `dev-docs/design/text-surface-unification.md` and `dev-docs/archive/production-tech-debt-review.md`: confirm they still describe active guidance; archive if superseded by `guidelines/` or by the new specs
- [x] 5.4 Keep in place as durable reference: `dev-docs/project/implementation-guide.md` (post-refresh), `dev-docs/project/active-work.md`, `dev-docs/project/known-tech-debt.md`, `dev-docs/platform/macos-support-plan.md`, `dev-docs/performance/performance-findings.md`, `dev-docs/plugins/plugin-runtime-research.md`, `dev-docs/performance/runtime-profiling.md`, `dev-docs/performance/startup-tracing.md`
- [x] 5.5 For each archived doc, add a one-line entry in `ROADMAP.md` or `dev-docs/project/active-work.md` recording "shipped; plan archived at dev-docs/archive/..." so the history is discoverable

## 6. Validation

- [x] 6.1 Run `openspec status --change establish-vision-and-diff-merge-excellence` and confirm all artifacts report `done`
- [x] 6.2 Grep all durable docs (`docs/*.md`, `ROADMAP.md`, `CLAUDE.md`, `guidelines/**/*.md`, `AGENTS.md`, `README.md`) for "AI/chat surfaces" and "AI/chat" listed under non-goals or out-of-scope lists, and confirm none remain
- [x] 6.3 Grep all durable docs for unqualified diff/merge performance claims that contradict `specs/diff-merge-editor` "diff-semantics-do-not-degrade-by-file-size" and correct or remove them
- [x] 6.4 Verify that `microide_diff_bench`, `microide_search_bench`, `MICROIDE_PERF_TRACE`, and `MICROIDE_TRACE_REDRAW` references in `README.md` and `dev-docs/performance/runtime-profiling.md` remain accurate after the alignment pass
- [x] 6.5 Confirm no source files under `src/` are modified by this change (specs + docs only)
- [x] 6.6 Confirm `AGENTS.md`, `CLAUDE.md`, `README.md`, `ROADMAP.md`, and `dev-docs/project/implementation-guide.md` do not contradict each other on (a) AI scope, (b) diff/merge priority, (c) plugin-phase framing (shipped vs active)

## 7. Follow-up Preparation

- [x] 7.1 Draft a one-line summary of each deferred follow-up change referenced by this proposal (unified decorated text-grid pipeline, numeric perf thresholds on a declared reference host, diff-semantic correctness audit for the large-file path) and record it at the bottom of `proposal.md` or in `ROADMAP.md` "Next Phase"
- [x] 7.2 Confirm `dev-docs/project/diff-editor-merge-rewrite-plan.md` exists; if it does not, note its absence as an open follow-up so the diff-merge implementation change has a design landing place
