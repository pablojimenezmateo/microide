# Benchmark record

Required by `diff-merge-editor` § "Diff And Merge Are Measured". All numbers on
the dev PC under load (the QA soak runs continuously), so runs are interleaved
base/head and compared by minimum (`microide_diff_bench`) or side by side
(`microide_perf`). Base = `main` at `e60493945`; head = branch at `c1341baed`
for allocations and `775bb782a` for the diff bench (the arena commit after it
changes only cache storage, not refinement work).

## `microide_diff_bench` (RelWithDebInfo + LTO, `--runs=15`, min ms)

Each file diffed between its last revision before 2026-07-15 and `e60493945`:
deliberately heavy, months of churn.

| file | hunks | base total | head total | head refinement | graph vertices |
| --- | ---: | ---: | ---: | ---: | ---: |
| `src/compare/CompareModel.cpp` | 110 | 1.51 | 5.82 | 5.43 | 44,623 |
| `tests/perf/PerfMain.cpp` | 139 | 1.51 | 3.91 | 3.41 | 26,221 |
| `src/editor/EditorViewRenderer.cpp` | 65 | 0.82 | 1.61 | 1.34 | 8,767 |
| `dev-docs/project/known-tech-debt.md` | 6 | 1.54 | 13.79 | 13.41 | 106,805 |

A knowing regression on the FIRST build of an extreme diff: 2-4x for code, ~9x
for a Markdown file rewritten end to end. It was 4-20x before the gap
splitting, A* heuristic and flat-path work in this change (the progression is
in design.md § 3). A rebuild of an open compare refines only the changed hunk
(`HunkRefinementCache`). Remaining work: known-tech-debt.md TD-2026-11-11-444.

## `microide_perf` compare scenarios (p50 wall ms, p50 allocations)

| scenario / phase | base ms | head ms | base allocs | head allocs |
| --- | ---: | ---: | ---: | ---: |
| `diff_next_hunk_large_file` / `diff.open_large_compare` | 19.5 - 20.2 | 19.4 - 25.8 | 856 | 866 |
| `diff_next_hunk_large_file` / `diff.next_hunk_burst` | 104.9 - 107.5 | 107.4 - 137.3 | 2 | 2 |
| `diff_stage_hunk_large_patch` / `diff.open_large_patch` | 19.9 - 20.6 | 20.1 - 24.0 | 1477 | 1487.5 |
| `diff_stage_hunk_large_patch` / `diff.stage_hunk` | 21.6 - 22.9 | 23.0 - 29.4 | 1866 | 1868 |
| `compare_type_in_wrapped_diff` / `compare.type_in_wrapped_diff` | 66.7 - 80.3 | 65.7 - 81.9 | 598.5 | 566.5 |
| `compare_scroll_large_fixture` / `compare_large.scroll_burst` | 65.1 - 73.6 | 65.1 - 95.6 | 80 | 80 |
| `compare_tab_open` / total | 2.35 - 2.64 | 2.55 - 3.87 | 54 | 54 |

Wall ranges are across interleaved runs; the high head values coincide with
soak load (the scroll burst does not run the diff at all and moved the same
way). The first head run matched base on every phase. Allocations are
deterministic: +10 on open (the arena's first growth), 32 fewer per typing
iteration.
