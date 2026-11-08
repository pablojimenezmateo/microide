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

}  // namespace microide::tests::architecture
