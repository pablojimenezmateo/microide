#pragma once

#include "architecture/ArchitectureRuleHelpers.h"

#include <filesystem>

namespace microide::tests::architecture {

RuleResult CheckEditorMetricsNameTheirStickyBand(const std::filesystem::path& repo_root);
void RunEditorMetricsStickyBandRuleFixtures();

RuleResult CheckTabSurfacesLayOutInTheFocusedPane(const std::filesystem::path& repo_root);
void RunTabSurfacesRuleFixtures();

}
