#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

// An editor pane's metrics computed with a literal 0 for the sticky-scroll band
// (`ComputeMetrics(renderer, viewport, rect, 0, ...)`) lay rows out as if no band
// were pinned over them. Five call sites had that bug -- the IME area, the typed
// row's repaint rect, right-click targets, hover cards and the cursor's scrollbar
// test (TD-2026-11-08-434) -- so a 0 there must say, in a comment just above,
// why the band does not matter (the two-step sizing that resolves it, a
// columns-only question, a surface with no band); otherwise pass the viewport's
// `sticky_band_rows()`. Safety-plan W9.
RuleResult CheckEditorMetricsNameTheirStickyBand(const std::filesystem::path& repo_root);
void RunEditorMetricsStickyBandRuleFixtures();

// A compare, a merge, the find widget and the debug toolbar lay out against the
// focused pane (`WorkspaceShell::ActiveTabSurface`, `FocusedGroupSurface`), never
// against the editor COLUMN (`layout.editor_surface`). In a split the column is a
// different rect: a diff anchored to it painted over the other panes under their
// tab strips, and the toolbar sat on another pane's buttons (TD-2026-11-11-439,
// soak 3cea0cf87f72 / 83dfd7f84e2f). Flags those layout calls whose surface
// argument names the column.
RuleResult CheckTabSurfacesLayOutInTheFocusedPane(const std::filesystem::path& repo_root);
void RunTabSurfacesRuleFixtures();

}  // namespace microide::tests::architecture
