#include "perf/PerfHarness.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "workspace/shell/WorkspaceShell.h"

namespace microide::tests::perf {
namespace {

const std::filesystem::path kSplitProject = "tests/perf/fixtures/settings_tabs_project";

bool SplitInto(ScenarioContext& context, std::string_view command, std::string_view relative) {
  std::string line;
  line.append(command);
  line.push_back(' ');
  line.append(relative);
  return context.ExecuteCommand(line);
}

constexpr int kMaxTabsPerPane = 8;

bool BuildFourPaneGrid(ScenarioContext& context) {
  return SplitInto(context, "split-right", "src/unit_02.cpp") &&
         SplitInto(context, "split-down", "src/unit_03.cpp") &&
         context.ExecuteCommand("focus-other-group") &&
         SplitInto(context, "split-down", "src/unit_04.cpp");
}

void RunEditorSplitGridWorkout(ScenarioContext& context) {
  if (!RequireFixture(context, kSplitProject, "editor_split_grid_workout")) {
    return;
  }
  (void)context.Open(kSplitProject);
  for (int guard = 0; context.EditorGroupCount() > 1 && guard < 16; ++guard) {
    for (int tab_guard = 0; tab_guard < kMaxTabsPerPane; ++tab_guard) {
      context.CloseActiveTab();
    }
    if (!context.ExecuteCommand("close-group")) {
      break;
    }
  }
  context.OpenTab(kSplitProject / "src" / "unit_01.cpp");
  context.PumpFrames(2);
  if (context.EditorGroupCount() != 1) {
    context.SkipScenario("editor_split_grid_workout: could not collapse back to one pane (" +
                         std::to_string(context.EditorGroupCount()) + " left)");
    return;
  }

  bool built = false;
  context.Measure("split.create_grid", [&] { built = BuildFourPaneGrid(context); });
  context.PumpFrames(2);
  if (!built || context.EditorGroupCount() != 4) {
    context.SkipScenario("editor_split_grid_workout: the grid ended with " +
                         std::to_string(context.EditorGroupCount()) +
                         " panes instead of 4, so nothing four-pane was measured");
    return;
  }

  context.Measure("split.frame_burst", [&] { context.PumpFrames(30); });

  const std::size_t divider_count = context.EditorSplitDividerCount();
  if (divider_count == 0) {
    context.SkipScenario("editor_split_grid_workout: a four-pane grid reported no dividers");
    return;
  }
  const SDL_FRect divider = context.EditorSplitDividerRect(0);
  if (divider.w <= 0.0f || divider.h <= 0.0f) {
    context.SkipScenario("editor_split_grid_workout: divider 0 has no geometry to drag");
    return;
  }
  const float divider_x = divider.x + divider.w * 0.5f;
  const float divider_y = divider.y + divider.h * 0.5f;
  const SDL_FRect editor_pane = context.EditorGroupSurfaceRect(0);
  const float pane_area = editor_pane.w * editor_pane.h;
  if (pane_area <= 0.0f) {
    context.SkipScenario("editor_split_grid_workout: pane 0 has no geometry");
    return;
  }

  constexpr int kDividerMotionEvents = 240;
  int divider_full_redraws = 0;
  double divider_damage = 0.0;
  context.MousePress(divider_x, divider_y);
  context.Measure("split.divider_drag", [&] {
    for (int i = 0; i < kDividerMotionEvents; ++i) {
      const int period = kDividerMotionEvents / 4;
      const int phase = i % (2 * period);
      const float t = phase < period ? static_cast<float>(phase) / static_cast<float>(period)
                                     : 2.0f - static_cast<float>(phase) / static_cast<float>(period);
      const float x = divider_x + (t - 0.5f) * editor_pane.w * 0.4f;
      const ScenarioContext::PumpedFrameDamage frame = context.PumpPartialFrame(context.MouseMoveDragging(x, divider_y));
      if (frame.full) {
        ++divider_full_redraws;
      }
      divider_damage += frame.area;
    }
  });
  context.MouseRelease(divider_x, divider_y);
  context.PumpFrames(1);

  if (divider_damage <= 0.0) {
    throw std::runtime_error(
        "editor_split_grid_workout: the divider drag produced no damage at all, so it "
        "measured nothing");
  }
  const double divider_areas_per_event =
      divider_damage / (static_cast<double>(kDividerMotionEvents) * static_cast<double>(pane_area));
  if (divider_full_redraws != 0 || divider_areas_per_event > 5.5) {
    throw std::runtime_error(
        "editor_split_grid_workout: the divider drag asked for " +
        std::to_string(divider_full_redraws) + " full-window repaints and " +
        std::to_string(divider_areas_per_event) +
        " pane areas per motion event; a divider drag must stay scoped to the panes it "
        "resizes");
  }

  double type_damage = 0.0;
  int type_full_redraws = 0;
  const SDL_FRect focused_pane =
      context.EditorGroupSurfaceRect(context.EditorGroupCount() - 1);
  const double focused_pane_area =
      static_cast<double>(focused_pane.w) * static_cast<double>(focused_pane.h);
  constexpr int kKeystrokes = 120;
  context.Measure("split.type_in_focused_pane", [&] {
    for (int i = 0; i < kKeystrokes; ++i) {
      const ScenarioContext::PumpedFrameDamage frame = context.PumpPartialFrame(context.Type("x"));
      if (frame.full) {
        ++type_full_redraws;
      }
      type_damage += frame.area;
    }
  });

  if (type_damage <= 0.0) {
    throw std::runtime_error(
        "editor_split_grid_workout: typing damaged nothing, so the repaint-scope check "
        "measured nothing");
  }
  const double type_areas_per_key =
      focused_pane_area > 0.0
          ? type_damage / (static_cast<double>(kKeystrokes) * focused_pane_area)
          : 0.0;
  if (type_full_redraws != 0 || type_areas_per_key > 1.5) {
    throw std::runtime_error(
        "editor_split_grid_workout: typing in one pane asked for " +
        std::to_string(type_full_redraws) + " full-window repaints and " +
        std::to_string(type_areas_per_key) +
        " focused-pane areas per keystroke; an edit in one pane must not repaint the "
        "others");
  }
}

const ScenarioRegistration g_perf_editor_split_grid_workout({Scenario{
    .name = "editor_split_grid_workout",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_p50_percent = tolerance::kJitterWallP50,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunEditorSplitGridWorkout,
}});

}
}
