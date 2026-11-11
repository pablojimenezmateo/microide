## Why

The compare view highlighted change the way a line differ sees it: a hunk's lines
were paired by a similarity score, and within a paired line a token/codepoint LCS
underlined the bytes that differed. That reads badly for the edits code review is
mostly made of. Wrap a function body in an `if`, and every re-indented line is
underlined as changed; re-wrap a call over four lines, and four lines light up
although nothing was written; add a parameter to a signature, and the aligner
might read the old line as deleted.

[difftastic](https://github.com/Wilfred/difftastic) shows what a reviewer wants to
see instead: it diffs **syntax**, not lines. Each side is parsed into atoms and
delimited lists, the two trees are matched as a shortest-path problem (Dijkstra
over "next unmatched node on each side + the delimiters entered so far", with
cheap edges for equal subtrees and expensive ones for nodes on one side only), and
only the nodes that are genuinely new — NOVEL — are highlighted. Formatting
changes cost nothing because equality is structural.

## What Changes

- **New structural layer in `src/compare`** (SDL-free kernel):
  - `DiffSyntax` — a language's lexical rules (comment syntax, bracket pairs,
    quotes); a lexer and bracket matcher build a syntax forest per hunk side with
    content ids interned across both sides (equal ids ⇔ equal subtrees,
    whitespace-blind).
  - `StructuralDiff` — difftastic's graph search: Dijkstra with a bucket queue
    over a persistent delimiter stack (PopBoth / PopEither), edge costs after
    difftastic, a vertex budget, and a flat token-LCS fallback past it.
  - `HunkRefinement` — per hunk: novel spans per line (replaced strings and
    comments diffed word by word) and the row alignment, a heaviest monotone chain
    of line pairs that share matched tokens with the lines between zipped.
- **`CompareModel`**: the line diff still decides hunks; each hunk is refined
  through `HunkRefinement`. `CompareBuildOptions` gains `syntax` (the language's
  rules) and `refine_hunks` (the patch generator turns it off).
  `CompareSideChange` / `CompareRowIsLayoutOnly` classify each row side as novel,
  layout-only or unchanged.
- **Removed**: the similarity hunk aligner (`AlignHunkLinesInto`, its DP and
  positional fallback), the token and codepoint intraline differs and their
  budgets (~850 lines), the presentation model's inline-span cache (a per-build
  copy of the model's spans that every read resolved back to the model's), and
  the two changed-span underline appenders in `RowDecorationBuilder`.
- **Presentation**: left side reads red, right side green; a side is washed only
  when it carries novel tokens, novel tokens get a stronger fill (through the
  column-fill geometry, so grid and proportional rows share one path), and
  layout-only sides keep a muted stripe and marker. The overview lane mutes
  layout-only runs.
- **Workspace**: `WorkspaceLanguageContract::ResolveDiffSyntax` derives a
  memoized `DiffSyntax` per language from the contract; compare tabs resolve it
  from the right buffer's language and rebuild when it changes.

## Impact

- Specs: `diff-merge-editor` (structural refinement added; size-degradation,
  low-contrast decoration and presentation-detail requirements updated).
- Code: `src/compare/*`, `src/editor/RowDecorationBuilder.*`,
  `src/workspace/WorkspaceLanguageContract.*`, compare tab load/refresh,
  `WorkspaceShellRenderCompare.cpp`, `WorkspaceLayout` overview runs,
  `PatchGenerator`, `tools/CompareBench.cpp`.
- Profile fields `hunk_alignment_ns`/`intraline_ns` and the alignment/intraline
  call counters are replaced by `hunk_refinement_ns`, `structural_hunks`,
  `flat_fallback_hunks` and `graph_vertices`; the perf counter
  `compare.intraline_diff_lines` by `compare.structural_hunks` and
  `compare.structural_graph_vertices`.
