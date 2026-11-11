## 1. Structural core (kernel)

- [x] 1.1 `compare/DiffSyntax.{h,cpp}`: `DiffSyntax` lexical profile + `GenericDiffSyntax()`; lexer (comments, strings incl. multi-line block comments / backtick / triple-quoted, operator runs, separators) and bracket matching with bounded recovery (unpaired delimiters become punctuation); preorder syntax forest; `DiffContentInterner` with exact (hash + structural compare) content ids
- [x] 1.2 `compare/StructuralDiff.{h,cpp}`: Dijkstra over (lhs, rhs, persistent PopBoth/PopEither stack) with a bucket queue, difftastic's vertex identity and edge costs, root prefix/suffix trimming, vertex budget, flat token-LCS fallback (`MatchTokensFlat`)
- [x] 1.3 `compare/HunkRefinement.{h,cpp}`: novel spans per line (replaced literals word-diffed, spans merged across blanks), heaviest monotone chain of matched line pairs with zipped gaps, one-sided hunk fast path, scratch released after huge hunks

## 2. Compare model

- [x] 2.1 Route every hunk through `RefineHunk`; `CompareBuildOptions::syntax` and `refine_hunks`; `CompareSideChange`, `LeftSideChange`/`RightSideChange`, `CompareRowIsLayoutOnly`
- [x] 2.2 Delete the similarity hunk aligner, the token/codepoint intraline differs and their budgets; keep the line weighting the line diff uses
- [x] 2.3 Profile/counters: `hunk_refinement_ns`, `structural_hunks`, `flat_fallback_hunks`, `graph_vertices`; `compare.structural_hunks`, `compare.structural_graph_vertices`; `tools/CompareBench.cpp` reports them
- [x] 2.4 `PatchGenerator` builds line-only models (`refine_hunks = false`)
- [x] 2.5 Delete `ComparePresentationModel::inline_cache`, `RebuildCompareInlineDiffCache`, `CompareInline{Left,Right}Spans` and the presentation build's `model_generation` parameter

## 3. Workspace and presentation

- [x] 3.1 `WorkspaceLanguageContract::ResolveDiffSyntax` (memoized per language id, dropped on revision change)
- [x] 3.2 Compare tabs: syntax resolved on the shell thread for the async load (path) and in the derived-state refresh (buffer language); `derived_syntax` joins the rebuild fingerprint; `MarkCompareModelBuilt` takes the built options
- [x] 3.3 `RowDecorationBuilder`: novel spans become emphasis fills through `AppendColumnFill`; both underline appenders deleted
- [x] 3.4 `WorkspaceShellRenderCompare.cpp`: per-side style (left removed / right added, wash and token fill only when novel, muted stripe and marker when layout-only)
- [x] 3.5 Overview lane: `CompareScrollbarRun::layout_only`, drawn muted

## 4. Tests

- [x] 4.1 `tests/StructuralDiffTests.cpp`: lexer, unbalanced delimiters, layout-blind content ids, block wrap, re-wrap, argument change, comment word diff, re-indented comment, flat fallback, randomized match/row invariants, refinement off
- [x] 4.2 Update `CompareModelTests` (UTF-8 token span, import expansion, context-aware alignment, budget tests rewritten for the vertex budget), `GitServiceTests` fixture counts, `RowDecorationBuilderTests` (fills), `CompareReviewTests`, `WorkspaceShellSharedLayoutTests` (layout-only runs)
- [x] 4.3 Full test shards, `clang-build`, `asan`, `ubsan`, `qa`, `perf-tests`; perf A/B of the compare scenarios and `microide_diff_bench` against `main` (bench.md)

## 4b. Performance (after the first A/B)

- [x] 4b.1 Gap splitting before the search (ends, lone list pair, patience anchors, sibling LCS, atom-only gaps), per-gap search cap
- [x] 4b.2 A* with the node-balance heuristic; small per-search tables; touched-bucket reset
- [x] 4b.3 Edge costs aligned with difftastic's source (`graph.rs`)
- [x] 4b.4 `HunkRefinementCache` in flat double-buffered arenas (rebuilds allocate nothing)
- [x] 4b.5 Graph search split into `StructuralSearch.{h,cpp}`
- [x] 4b.6 Fix: LTO links take their temporaries from the build tree (quota-full /tmp broke the perf build)

## 5. Docs

- [x] 5.1 `known-tech-debt.md`: TD-440 grammar-backed trees, 441 hunk-local lexing, 442 slider correction, 443 cross-hunk moves, 444 cold-build cost
- [x] 5.2 `implementation-guide.md`, `diff-editor-merge-rewrite-plan.md`, `CHANGELOG.md`
