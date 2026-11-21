# derived-damage: measurements

Protocol: base binary built at 64b4f5e61 (before any stage), head at the stage commit; 3 interleaved runs each, 10 iterations, soak paused, same disk (main checkout). Table = median of the three runs' p50. Allocation counts are deterministic.

## Stage 1 (status bar), head 416e91c70 -- 2026-11-21

```
scenario                                                                base p50  head p50     d%  alloc b  alloc h
editor_bracket_match_caret_motion                                          7.001     7.009    0.1      175      175
editor_cjk_scroll_paint                                                  679.252   674.851   -0.6    11087    11087
editor_cjk_scroll_paint :: cjk_scroll_paint.page_down_sweep              659.956   655.428   -0.7    10792    10792
editor_indent_guides_paint                                               111.950   112.828    0.8      436      436
editor_indent_guides_paint :: editor_indent_guides_paint.scroll_paint_    87.845    88.338    0.6       94       94
editor_render_whitespace_paint                                           128.257   129.531    1.0      538      538
editor_render_whitespace_paint :: editor_render_whitespace_paint.scrol   102.291   102.880    0.6      294      294
editor_typing_paint                                                      172.413   177.103    2.7     1636     1636
editor_typing_paint :: typing_paint.160_keystrokes                       167.650   172.197    2.7     1443     1443
menu_hover_paint                                                          88.952    88.867   -0.1      327      327
menu_hover_paint :: menu_hover_paint.160_hover_frames                     87.582    87.514   -0.1      321      321
plugin_status_item_update                                                  6.014     5.960   -0.9    16006    16006
plugin_status_item_update :: status_registry.apply_update                  5.141     5.133   -0.1     8001     8001
scroll_large_file                                                         60.963    61.875    1.5      138      138
scroll_large_file :: scroll_large_file.scroll_burst                       60.948    61.860    1.5       25       25
typing_large_file                                                          3.275     3.281    0.2      104      104
typing_small_file                                                          2.664     2.671    0.3       98       98
```

editor_typing_paint per-run p50: base [177.75, 172.41, 171.85], head [177.10, 184.72, 174.42] -- overlapping; deterministic counters (one in-process run each, 3 iterations): redraw_rect_pixels 764,119,272 -> 738,953,032 (-3.3%), redraw_rects_queued 2038 -> 2049, derived_damage_units 5251, derived_damage_changed 650. Verdict: within budget (no p50 > +5% beyond noise, no new allocations), and it paints less.
