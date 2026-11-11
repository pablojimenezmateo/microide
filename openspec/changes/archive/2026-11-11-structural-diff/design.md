## Context

Before this change a compare build was: line diff → hunks → per hunk, pair
deleted/inserted lines by a similarity DP (`AlignHunkLinesInto`, with a positional
fallback past 65k cells) → per paired row, a token LCS (or a codepoint LCS) for
the changed byte spans, with a per-hunk cell budget and a 64 KB line guard. The
render underlined those spans. The presentation model kept a second copy of every
row's spans that every read resolved to the same values as the model's.

## Goals / Non-Goals

Goals: difftastic's core ideas -- syntax-aware matching, novel-token highlighting,
formatting changes that cost nothing, line alignment driven by matched tokens --
inside the existing compare tab, with speed first (the editable pane rebuilds the
model per keystroke) and no change to what git sees.

Non-goals: a grammar per language (TD-2026-11-11-440), cross-hunk move
detection (TD-443), structural merge, a unified/inline view.

## Decisions

### 1. Two layers: lines decide hunks, syntax refines them

Staging, discarding and patches need line hunks that are exactly git's. The
structural diff therefore never decides WHETHER a line changed, only which of its
tokens are new and how the hunk's lines sit side by side. A line that git sees as
changed and the structural diff finds no novel token in is "layout only" -- a
first-class state (`CompareSideChange::LayoutOnly`) rather than a row the view
hides, so a reviewer still sees and can stage it. Refining per hunk also keeps
the cost proportional to the change, not the file.

Alternative considered: one structural diff over the whole file (difftastic's
shape). Rejected: its hunks would not be git's, and a keystroke would re-diff the
whole file.

### 2. Lexer + bracket matching, not tree-sitter

`DiffSyntax` (comment tokens, bracket pairs, quotes) comes from the existing
`WorkspaceLanguageContract`, so every language the editor knows gets structural
diffs with no new dependency. Unpaired delimiters become punctuation, so a hunk
that opens a block another hunk closes still diffs. Strings and comments are
atoms; a replaced one with ≥20 % word overlap is matched as REPLACED and diffed
word by word, which also absorbs lexing mistakes (an apostrophe read as a quote
costs a coarser span, not a wrong one).

### 3. The search, and keeping it small

Vertices are (next LHS node, next RHS node, entered-delimiter stack), the stack a
persistent arena of groups: one pair entered on both sides (PopBoth) or runs
entered on one side only, as two cons lists so either side can exit alone
(PopEither). Vertex identity is difftastic's approximation -- position, innermost
parent per side, can-pop-either -- so the graph is not multiplied by nesting
depth. Costs are difftastic's own (`src/diff/graph.rs`, `Edge::cost`, checked
against its source): unchanged node 1 + min(40, depth difference), +200 when it
is a `,` `;` or `.`; enter matching delimiters 10 + min(40, depth difference);
replaced string/comment 500 + (100 − similarity), always offered for two
same-kind literals and shown as an edit only above 20 % similarity (otherwise
both are novel); novel atom or delimiter 300. Where this differs from
difftastic: similarity is word-multiset overlap rather than normalized
Levenshtein (linear, and the word diff that follows works on words anyway).

The search is A*, not plain Dijkstra. Every edge but a novel one consumes the
same number of preorder nodes on both sides, and a novel edge consumes one node
on one side at 300, so `h = 300 × |nodes left on LHS − nodes left on RHS|` is
admissible and consistent: the answer is unchanged and lopsided gaps (a big
deletion beside a small insertion) stop exploring the whole product. Every edge
moves f by at most 900 (a 600 replacement or a 300 novel edge, plus 300 of h), so
the queue is a 1024-bucket circular (Dial) queue.

A first measurement (four real files diffed across ~4 months of history,
`microide_diff_bench`, min of runs) put the refinement at 4-20× the old
aligner+intraline cost: the graph is ~N×M over a changed region and an A* step
costs ~100 ns. So, as difftastic itself does before its search, the comparison
is split first (`Matcher`, an explicit work list):

1. equal leading/trailing siblings match outright;
2. one list left on each side with the same delimiters: match the delimiters and
   continue inside (an edit deep in a big block never builds a graph over it);
3. patience anchoring: siblings unique on both sides with equal content split
   the run, recursively;
4. otherwise a weighted LCS over the sibling subtrees (weight = tokens; a lone
   separator weighs nothing) splits at equal subtrees (difftastic's
   `split_unchanged`);
5. a gap of bare atoms with no literal pair is an LCS problem exactly, so the
   flat diff answers it without a graph;
6. only then a search, and only for a gap up to a 16k token product; a larger
   gap (a rewrite, where nesting adds little) takes the flat token LCS at once.
   A 250k vertex budget remains as a safety cap.

The per-search tables start small and only touched queue buckets are cleared:
the first profile showed fixed 1024-entry resets costing more than the searches.
Small flat ranges use an in-place byte-weighted DP rather than the line differ,
and the content interner resets only the buckets it used. Progression on the
`CompareModel.cpp` history diff (refinement ms, min): 24.9 plain Dijkstra ->
13.4 with splitting -> 8.4 with A* -> 5.4 with the sibling LCS and the cheaper
flat path. Final numbers: bench.md.

### 3a. Rebuilds refine only what changed

The editable compare pane rebuilds its model per keystroke, and a keystroke
changes one hunk. `HunkRefinementCache`, held in the model's retained build
scratch, memoizes each hunk's refinement keyed on the exact text of both sides
(hash, then verified against a stored copy) and the build's shared `DiffSyntax`.
It is two flat arenas (previous build, current build) swapped per build, so a
rebuild refines one hunk and allocates nothing: a first, per-entry heap layout
doubled the typing scenario's allocations (598 -> 1222) and was replaced.

### 4. Row alignment from matched tokens

Matched token pairs, weighted by token bytes and aggregated per (left line, right
line), feed a heaviest strictly-increasing chain (sorted pairs + Fenwick
prefix-max, O(P log n)); lines between consecutive anchors are zipped side by
side, as difftastic does. This replaces the similarity DP and its positional
fallback outright.

### 5. Presentation

Left side = removed color, right side = added color, whatever the row kind. A
side gets the row wash and the token-emphasis fill only when it is novel; a
layout-only side keeps a muted stripe; the gutter mark is muted for a layout-only
row and the overview lane draws layout-only runs muted. The emphasis is a FILL
through `AppendColumnFill`, so the grid and proportional row paths share the
geometry selection already uses, and both underline appenders are gone.

### 6. Language resolution

`WorkspaceLanguageContract::ResolveDiffSyntax` memoizes one shared
`DiffSyntax` per language id per contract revision, so a compare tab detects
"rules changed" by pointer identity (`derived_syntax` joins the rebuild
fingerprint). The async open resolves it from the path on the shell thread (the
contract is not the worker's); the derived-state refresh resolves it from the
buffer's language and rebuilds once if content detection disagrees.

## Risks / Trade-offs

- [Cold-open cost] A first build of a heavily changed file costs 2-9x the
  line-era refinement (bench.md); the compare scenarios are at parity, and
  rebuilds hit the hunk cache. TD-2026-11-11-444 holds the remaining leads.
- [Cache memory] The cache holds a copy of each changed hunk's text plus its
  spans for the lifetime of the model: proportional to the change, not the file.
- [Changed alignment] Lines within a hunk now pair by shared tokens; tests that
  pinned the similarity aligner's choices were updated to the structural reading.
- [Lexer limits] TD-2026-11-11-440/441/442.
