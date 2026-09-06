// Performance scenarios for the n-way editor grid (`EditorSplitTree`).
//
// The split tree shipped with no perf scenario at all. That is the shape
// dev-docs/project/validation-traps.md warns about: the feature was measured by
// hand during its own session and the harness was thrown away, so every hot path
// it introduced -- the per-pane tab strips, the per-pane render loop, the
// divider drag that rebuilds the pane rects on every motion event -- is gated by
// nothing. A split is also the one layout where a *scoped* repaint stops being
// an optimisation and becomes a correctness-adjacent property: typing in one
// pane must not repaint the other, or a four-pane window pays 4x the fill rate
// of a one-pane window for every keystroke.
//
// Four things are pinned here:
//
//  1. `split.create_grid` -- building a four-pane grid from one pane. Structural
//     tree edits plus three file opens; the allocation count is the oracle.
//  2. `split.frame_burst` -- steady-state frames with four panes live. This is
//     what says whether a pane costs per FRAME rather than per edit.
//  3. `split.divider_drag` -- the per-motion-event cost of dragging a divider.
//     `ComputeEditorGroupRects` runs on every one of these, and the scenario
//     asserts the drag never asks for a full-window repaint.
//  4. `split.type_in_focused_pane` -- a keystroke burst in one pane of four,
//     with a hard invariant on repaint SCOPE: the damage per keystroke must stay
//     inside roughly one pane, not the whole editor surface. A headless
//     event-cost measurement is blind to that, and it is the regression with the
//     largest real-world effect (TD-2026-08-14-212's lesson, applied to splits).
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

// Open `path` in a new pane split off the focused one. Returns false when the
// command did not run, which the callers turn into a skip rather than a silently
// smaller grid -- a two-pane scenario wearing a four-pane baseline is exactly the
// vacuous green this file exists to avoid.
bool SplitInto(ScenarioContext& context, std::string_view command, std::string_view relative) {
  std::string line;
  line.append(command);
  line.push_back(' ');
  line.append(relative);
  return context.ExecuteCommand(line);
}

// The four-pane grid every phase below runs on: one vertical split, then a
// horizontal split inside each column. That is two orientations and a branch of
// branches, so the layout walk is exercised at depth rather than in its
// single-branch fast case.
// Tabs this scenario ever opens in one pane; the collapse below closes at most
// this many per pane before closing the pane itself.
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
  // The shell is reused across iterations, so a grid built by the previous one is
  // still standing. Collapse back to a single pane first: without this the second
  // iteration measures a seven-pane grid against a four-pane baseline, which the
  // harness would happily report as a PASS on a number that means nothing.
  // Drop each pane's tabs before closing it. The last phase below types 120
  // characters into the focused pane, so on every iteration after the first that
  // pane holds a dirty buffer whose only view it is -- and `close-group` now
  // raises Save / Discard / Cancel rather than dropping those edits silently
  // (the VS Code behaviour this shell adopted). That command reports success
  // having only ASKED, so the loop below ran its guard out against a prompt and
  // left the four-pane grid standing: the scenario skipped, and a skipped
  // baseline-gated scenario fails the gate. `CloseActiveTab` is the
  // no-prompt close, which is what setup wants -- the edits are throwaway
  // keystrokes in a shared fixture that must never be saved.
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

  // Steady-state frames with four panes live. Nothing changes between them, so
  // an allocation here is per-frame chrome restating itself -- the shape
  // TD-2026-08-14-229 found on the single-pane path, now with four strips.
  context.Measure("split.frame_burst", [&] { context.PumpFrames(30); });

  // The divider drag. `ComputeEditorGroupRects` is documented as running three
  // times per motion event, so this is where a heap allocation in the layout walk
  // would show up multiplied.
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
      // A triangle sweep across the middle 40 % of the split's span, which stays
      // inside the 0.1/0.9 clamp so every event is a real resize rather than a
      // no-op against the stop.
      const int period = kDividerMotionEvents / 4;
      const int phase = i % (2 * period);
      const float t = phase < period ? static_cast<float>(phase) / static_cast<float>(period)
                                     : 2.0f - static_cast<float>(phase) / static_cast<float>(period);
      const float x = divider_x + (t - 0.5f) * editor_pane.w * 0.4f;
      const workspace::WorkspaceShell::RenderInvalidation redraw =
          context.MouseMoveDragging(x, divider_y);
      if (redraw.full) {
        ++divider_full_redraws;
      }
      for (const SDL_FRect& rect : redraw.rects) {
        divider_damage += static_cast<double>(rect.w) * static_cast<double>(rect.h);
      }
    }
  });
  context.MouseRelease(divider_x, divider_y);
  context.PumpFrames(1);

  // A divider drag genuinely resizes both panes it touches, so its damage is
  // legitimately pane-sized; what it must never be is the WINDOW. Divider 0 is
  // the root's, whose pair is BOTH columns, so the honest damage is the editor
  // area plus the tab-strip slice above it — measured 4.49 pane areas here. The
  // whole window reads 6.53 (sidebar, menu bar, project strip, bottom panel,
  // status bar and debug pane, none of which a divider inside the editor column
  // moves), which is what this bound was probed against: it was 6.53 before the
  // scoping fix and the check fired, per validation-traps.md.
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

  // Typing in one pane of four. The other three panes show different files and
  // nothing in them changed, so the repaint must stay inside the focused pane.
  // This is the property that decides whether a four-pane window costs 1x or 4x
  // per keystroke.
  double type_damage = 0.0;
  int type_full_redraws = 0;
  const SDL_FRect focused_pane =
      context.EditorGroupSurfaceRect(context.EditorGroupCount() - 1);
  const double focused_pane_area =
      static_cast<double>(focused_pane.w) * static_cast<double>(focused_pane.h);
  constexpr int kKeystrokes = 120;
  context.Measure("split.type_in_focused_pane", [&] {
    for (int i = 0; i < kKeystrokes; ++i) {
      const workspace::WorkspaceShell::RenderInvalidation redraw = context.Type("x");
      if (redraw.full) {
        ++type_full_redraws;
      }
      for (const SDL_FRect& rect : redraw.rects) {
        type_damage += static_cast<double>(rect.w) * static_cast<double>(rect.h);
      }
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
  // One keystroke damages the edited line plus the pane's chrome (breadcrumb,
  // strip, status bar), which together stay well under one pane area. 1.5 leaves
  // room for that chrome while failing an order of magnitude below "repaint the
  // whole editor surface", which is 4 pane areas here and would read as 4.0.
  // Probed by lowering the bound until it fired, per validation-traps.md.
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
    // warmup: the first pass pays the project's cold open (file-index build,
    // first watch batch) and every opened file's first title/width measurement,
    // which dwarfs the four measured phases and would otherwise govern p95/max
    // on its own -- the same shape as editor_tab_drag_burst.
    .warmup_iterations = 1,
    // Frame-pumping dominates two of the four phases, and ~85 % of a pumped
    // frame's wall on this harness is its own 1920x1080 software present
    // (dev-docs/performance). The wall half is therefore reported, not trusted;
    // what gates is the four phase allocation counts and the two repaint-scope
    // invariants asserted in the body, all of which are deterministic and
    // clock-independent.
    .tolerance_p50_percent = tolerance::kJitterWallP50,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunEditorSplitGridWorkout,
}});

}  // namespace
}  // namespace microide::tests::perf
