# clip-discipline: measurements

## 0.1 The class (ledger, 2026-11-21 ~18:30)

Open QA-STALE signatures: 720 -- `outside` 380, `in-clip` 178, `clip-edge` 162.
The class is `tools/qa/findings.py::stale_clip_relation`: the first stale pixel,
in logical units, against the frame's clips.

Seen in the last 72 h: 154 open clip-edge/in-clip signatures, 403 sightings.

By scale (cell): 1 -- 246, 1.25 -- 47, 1.5 -- 45, 1.75 -- 33, 2 -- 32.
Fractional scales are 39%: rounding is a cause, not the main one.

By surface and relation (sightings):

| surface | clip-edge | in-clip |
|---|---|---|
| editorSurface | 76 | 54 |
| bottomPanel | 62 | 33 |
| sidebar | 2 | 58 |
| tabStrip | 48 | 5 |
| rightPane | 21 | 12 |
| elsewhere | 20 | 8 |
| breadcrumb | 0 | 4 |

Top signatures (72 h sightings): eab90dac1acf sidebar in-clip 34, 053365e38e57
tabStrip clip-edge 21, bfa3565938ee bottomPanel clip-edge 19, 9d861c6a3908
editorSurface in-clip 19, dfaecf57f6db tabStrip clip-edge 14, 0cb638ffcb39 sidebar
in-clip 13, 2a3dfdcfdf2d editorSurface clip-edge 12, b4be0b6bdba5 divider:editor
clip-edge 11.

## Sample (newest sighting of each)

| sig | stale line (first px vs clips) | cause |
|---|---|---|
| 053365e38e57 | first 342,27 at scale 1; clips 289+53 and 345: the 3 px between two tabs | overhang + unowned gap: a tab label's last glyph painted into the gap (crop viewed) |
| dfaecf57f6db | first 684 = 609+75, clips with gaps at 684-687 and 759-765 | unowned gap between tabs |
| bfa3565938ee | first 1084 = clip 1076+8 (terminal cursor blink, scheduled) | overhang: a wide emoji under the cursor is wider than the one cell the blink damages (crop viewed) |
| 2a3dfdcfdf2d | first (1346,342)/1.5 = (897,228) = bottom of clip 896,205,13,23 (caret blink) | edge rounding at 1.5 |
| b4be0b6bdba5 | 132 = 122+10, scale 1 (divider:editor) | edge rounding at 1 |
| 050407e2649e | 850 = 832+18, scale 1 (divider:panel) | edge rounding at 1 |
| d6c26d0b9495 | 1136 = 1118+18, scale 1 (menu) | edge rounding at 1 |
| b5c2a351989c | 1008/1.5 = 672 = 212+460 (debug tab) | edge rounding at 1.5 |
| eab90dac1acf | in a 0,0,2560,1440 clip (scale .75) | missing fill: the git panel's new message painted over the old one, both visible in the partial frame (crop viewed) |
| 0cb638ffcb39 | in a 0,0,2048,976 clip | missing fill (same panel, same shape) |
| 9d861c6a3908 | in clip 828,199,156,28 at .75 | not classified from the crop yet |
| 6cabd77096c8 | top-left corner of clip 151,177,109,23 | not classified from the crop yet |

Crops: `crop_stale.py SIGHTING_DIR FRAME X Y W H OUT` writes retained | reference |
diff, magnified, from a sighting's `frame-N-{retained,reference}.png` (kept in the
session scratchpad; task 0.2 moves it under `tools/qa`).

## Clip use today

19 `ScopedRenderClip` scopes in `src/`. Render TUs with many draw calls and none:
`DebugPaneRender.cpp`, `WorkspaceShellRenderOverlay.cpp`, `EditorViewRenderer.cpp`,
`WorkspaceShellRenderPrompts.cpp`, `WorkspaceShellRenderCompare.cpp`.

## 1.6 spike: a clip per element on the software renderer (2026-11-21)

Standalone SDL3 program, offscreen software renderer, 1440 wide; per row a fill
and a 40-character `SDL_RenderDebugText` run; 5 rounds of 400 frames (45 rows) and
2000 frames (12 rows), with and without `SDL_SetRenderClipRect` around each row
under a frame clip.

| rows | per-round delta (us/row) |
|---|---|
| 45 | -0.27, +0.10, +1.71, +0.23, -0.05 |
| 12 | -1.15, -0.42, -1.93, -0.52, -0.56 |

In the noise: no fallback for D1. The in-app A/B of the tab-strip stage confirms
it on the real paint.

## Found by the recorder on its first run

`DrawCloseGlyph` drew its 7x7 glyph centred on a tab's empty close rect -- at the
window's origin -- for a tab too narrow for a close button
(`QaFlowRegression/APaneNarrowedToItsMinimumPaintsNothingOutsideIt`); the strip
clip had hidden it.

## 1.6 Stage 1 perf A/B (2026-11-21)

Protocol: base 5684bd2c5 (snap only, before paint elements), head 1263bd2c2
(paint elements, recorder hooks, tab strips, lint); `microide_perf` built in two
worktrees under `build/` with the `microide-perf` preset; soak paused for the runs
(`soak.py pause`, resumed by the script's exit trap); 3 rounds, 10 iterations,
each scenario run base/head alternately with the order flipped per round. Table =
median of the three rounds' p50 wall time. Allocations are deterministic.

```
scenario                                  base p50  head p50     d%  alloc b  alloc h
editor_tab_drag_burst                        8.119     8.139    0.2    10263    10263
editor_typing_paint                        173.305   174.658    0.8     1644     1644
menu_hover_paint                            89.232    88.906   -0.4      327      327
multi_tab_cycle                            133.674   133.982    0.2     4364     4364
project_tab_strip_layout_hittest             5.058     4.952   -2.1      187      187
scroll_large_file                           61.146    61.288    0.2      146      146
```

Per-round p50: editor_typing_paint base [175.38, 172.73, 173.31] head [174.66,
174.75, 174.16]; multi_tab_cycle base [136.90, 133.67, 133.30] head [134.98,
133.97, 133.98] -- overlapping. `scroll_large_file` fails its committed
allocation gate (146 vs 129) on BOTH binaries: pre-existing, not this change.

Verdict: within budget -- no p50 beyond noise, no allocation added (the recorder
and the per-tab clips cost nothing measurable with recording off).
