#include "TestSupport.h"

#include "workspace/CompareTabReview.h"
#include "workspace/WorkspaceShellTestAccess.h"
#include "render/Theme.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

#if MICROIDE_HAS_SDL3_TTF

void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}

class SoftwareCanvas final {
 public:
  SoftwareCanvas(int width, int height) {
    surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
    Expect(surface_ != nullptr, "compare diagnostics render test should allocate a software surface");
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
    Expect(renderer_ != nullptr, "compare diagnostics render test should create a software renderer");
  }

  ~SoftwareCanvas() {
    if (renderer_ != nullptr) {
      SDL_DestroyRenderer(renderer_);
    }
    if (surface_ != nullptr) {
      SDL_DestroySurface(surface_);
    }
  }

  SDL_Renderer* renderer() const { return renderer_; }

 private:
  SDL_Surface* surface_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
};

#endif

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

// The compare/ref picker now runs its git query on the background executor and
// populates on a later frame via the git-sidebar wake event. Drive the mailbox
// drain until the picker leaves its loading state (or a 2s deadline elapses).
bool SettleComparePicker(WorkspaceShell& shell) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
    if (!WorkspaceShellTestAccess::ComparePickerLoading(shell)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  WorkspaceShellTestAccess::ConsumeGitSidebarRefresh(shell);
  return !WorkspaceShellTestAccess::ComparePickerLoading(shell);
}

float MaxRectHeight(const std::vector<SDL_FRect>& rects) {
  float max_height = 0.0f;
  for (const SDL_FRect& rect : rects) {
    max_height = std::max(max_height, rect.h);
  }
  return max_height;
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveCompareBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveCompareBlameOverlay(shell);
}

std::optional<microide::editor::EditorBlameOverlay> WaitForActiveMergeBlameOverlay(
    WorkspaceShell& shell,
    std::size_t minimum_line_count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto overlay = WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
    if (overlay.has_value() && overlay->lines.size() >= minimum_line_count) {
      return overlay;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return WorkspaceShellTestAccess::ActiveMergeBlameOverlay(shell);
}

void TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare edit fixture", "compare edit fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.right_editable,
         "working-tree comparison should expose an editable current-state pane");
  Expect(compare.right_view_active,
         "working-tree comparison should focus the editable current-state pane");

  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "text input should edit the compare current-state pane");
  Expect(compare.right_viewport.dirty(),
         "editing the compare current-state pane should mark the tab dirty");
  Expect(WorkspaceShellTestAccess::SaveTab(shell, 0),
         "saving the compare tab should write the current-state buffer");
  Expect(!compare.right_viewport.dirty(),
         "saving the compare tab should clear the dirty state");
  Expect(ReadFile(source).rfind("// note ", 0) == 0,
         "saving the compare tab should persist the edited current-state text");
}

void TestWorkspaceShellCompareClickTogglesEditablePaneFocus() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare click fixture", "compare click fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare click fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float left_x = surface.left_x + 24.0f;
  const float right_x = surface.right_x + 24.0f;

  Expect(compare.right_view_active,
         "compare click fixture should start with the editable pane active");
  Expect(SendMouseDown(shell, left_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare left pane should be handled");
  Expect(!compare.right_view_active,
         "clicking the compare left pane should leave the editable pane inactive");

  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the compare right pane should be handled");
  Expect(compare.right_view_active,
         "clicking the compare right pane should reactivate the editable pane");
}

void TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare band fixture", "compare band fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare band fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float left_x = surface.left_x + 24.0f;

  // Select a non-zero presentation row so a spurious "row 0" hit would be observable.
  const float row2_y = surface.rows_y + surface.line_height * 2.5f;
  Expect(SendMouseDown(shell, left_x, row2_y, SDL_BUTTON_LEFT),
         "clicking a real compare row should be handled");
  const std::size_t selected_after_real_click = compare.selected_row;

  // A click in the band directly above the first row (negative row offset) must be
  // rejected, not truncated to a phantom hit on row 0.
  const float above_y = surface.rows_y - 2.0f;
  Expect(!SendMouseDown(shell, left_x, above_y, SDL_BUTTON_LEFT),
         "clicking just above the first compare row must not be handled as a row hit");
  Expect(compare.selected_row == selected_after_real_click,
         "a click above the first row must not move the compare selection to row 0");
}

void TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare context fixture", "compare context fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  Expect(action_rects.previous_rect.has_value(),
         "middle collapsed context row should expose a previous-context expansion button");
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "expanding collapsed context from the selected run should succeed");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > initial_row_count,
         "expanding collapsed context should reveal additional rows");
  std::optional<std::size_t> updated_collapsed_row;
  const std::size_t updated_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < updated_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      updated_collapsed_row = i;
      break;
    }
  }
  Expect(!updated_collapsed_row.has_value(),
         "Show all should fully expand the selected collapsed run");
}

void TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare hover fixture", "compare hover fixture");
  WriteFile(source, build_text("right a", "right b"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare collapsed-context hover fixture should open");

  std::optional<std::size_t> collapsed_row;
  const std::size_t row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare collapsed-context hover fixture should expose a middle hidden context row");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto action_rects =
      WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, *collapsed_row);
  const float hover_x = action_rects.all_rect.x + action_rects.all_rect.w * 0.5f;
  const float hover_y = action_rects.all_rect.y + action_rects.all_rect.h * 0.5f;
  Expect(WorkspaceShellTestAccess::CursorKindAtIsPointer(shell, hover_x, hover_y),
         "collapsed-context action buttons should advertise a pointer cursor");
  SendMouseMotion(shell, hover_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsPointer(shell),
         "hovering a collapsed-context action button should cache the pointer cursor");

  const float clear_x = surface.right_x + 12.0f;
  SendMouseMotion(shell, clear_x, hover_y, 0);
  Expect(WorkspaceShellTestAccess::CachedCursorIsText(shell),
         "ordinary editable compare content should restore the text cursor");
}

void TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change,
                             std::string_view second_change,
                             std::string_view third_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-a " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle-b " + std::to_string(i) + "\n";
    }
    text += std::string(third_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  WriteFile(source, build_text("left a", "left b", "left c"));

  InitializeGitRepo(root);
  CommitAll(root, "Add compare multi-chunk fixture", "compare multi-chunk fixture");
  WriteFile(source, build_text("right a", "right b", "right c"));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare multi-chunk fixture should open");

  const auto collect_middle_runs = [&](WorkspaceShell& current_shell) {
    std::vector<std::pair<std::size_t, std::size_t>> identities;
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      const auto action_rects =
          WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(current_shell, i);
      if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
        identities.push_back(
            WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i));
      }
    }
    return identities;
  };
  const auto find_row = [&](WorkspaceShell& current_shell,
                            std::pair<std::size_t, std::size_t> identity) {
    const std::size_t row_count =
        WorkspaceShellTestAccess::ActiveComparePresentationRowCount(current_shell);
    for (std::size_t i = 0; i < row_count; ++i) {
      if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(current_shell, i) !=
          microide::compare::ComparePresentationRowKind::CollapsedContext) {
        continue;
      }
      if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(current_shell, i) ==
          identity) {
        return std::optional<std::size_t>(i);
      }
    }
    return std::optional<std::size_t>{};
  };
  const std::vector<std::pair<std::size_t, std::size_t>> middle_runs = collect_middle_runs(shell);
  Expect(middle_runs.size() >= 2,
         "compare multi-chunk fixture should expose at least two independently collapsed middle runs");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::optional<std::size_t> first_row = find_row(shell, middle_runs[0]);
  Expect(first_row.has_value(), "first collapsed middle run should be present before expansion");
  Expect(ExpandCompareCollapsedContext(compare, *first_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the first collapsed middle run");
  const std::size_t after_first_expand =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "the first collapsed middle run should remain expanded");

  const std::optional<std::size_t> second_row = find_row(shell, middle_runs[1]);
  Expect(second_row.has_value(),
         "the second collapsed middle run should remain available after the first expansion");
  Expect(ExpandCompareCollapsedContext(compare, *second_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the second collapsed middle run");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) > after_first_expand,
         "expanding a second collapsed middle run should reveal additional rows");
  Expect(!find_row(shell, middle_runs[0]).has_value(),
         "expanding a second run should not collapse the first one again");
  Expect(!find_row(shell, middle_runs[1]).has_value(),
         "the second collapsed middle run should also remain expanded");
}

void TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const auto build_text = [](std::string_view first_change, std::string_view second_change) {
    std::string text;
    for (int i = 0; i < 24; ++i) {
      text += "prefix " + std::to_string(i) + "\n";
    }
    text += std::string(first_change) + "\n";
    for (int i = 0; i < 30; ++i) {
      text += "middle " + std::to_string(i) + "\n";
    }
    text += std::string(second_change) + "\n";
    for (int i = 0; i < 8; ++i) {
      text += "suffix " + std::to_string(i) + "\n";
    }
    return text;
  };
  const std::string left = build_text("left a", "left b");
  const std::string right = build_text("right a", "right b");
  WriteFile(source, left);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare refresh fixture", "compare refresh fixture");
  WriteFile(source, right);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare refresh fixture should open");

  std::optional<std::size_t> collapsed_row;
  std::pair<std::size_t, std::size_t> collapsed_run_identity{0, 0};
  const std::size_t initial_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < initial_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    const auto action_rects =
        WorkspaceShellTestAccess::ActiveCompareCollapsedContextActionRects(shell, i);
    if (action_rects.previous_rect.has_value() && action_rects.next_rect.has_value()) {
      collapsed_row = i;
      collapsed_run_identity =
          WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i);
      break;
    }
  }
  Expect(collapsed_row.has_value(),
         "compare refresh fixture should expose a collapsed middle run before expansion");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.scroll_row = std::max(0, static_cast<int>(*collapsed_row) - 2);
  Expect(ExpandCompareCollapsedContext(compare, *collapsed_row,
                                       microide::workspace::CompareCollapsedContextAction::ShowAll),
         "ShowAll should expand the collapsed middle run before refresh");
  const std::size_t expanded_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(expanded_row_count > initial_row_count,
         "expanding the middle collapsed run should reveal additional rows");

  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute for compare refresh fixture");
  Expect(WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell) == expanded_row_count,
         "tree refresh should preserve the expanded compare presentation row count");

  bool run_recollapsed = false;
  const std::size_t refreshed_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  for (std::size_t i = 0; i < refreshed_row_count; ++i) {
    if (WorkspaceShellTestAccess::ActiveComparePresentationRowKind(shell, i) !=
        microide::compare::ComparePresentationRowKind::CollapsedContext) {
      continue;
    }
    if (WorkspaceShellTestAccess::ActiveCompareCollapsedRunIdentity(shell, i) ==
        collapsed_run_identity) {
      run_recollapsed = true;
      break;
    }
  }
  Expect(!run_recollapsed,
         "tree refresh should preserve expanded compare runs instead of re-collapsing them");
}

void TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "read-only compare fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");
  Expect(WorkspaceShellTestAccess::ActiveCompare(shell).right_view_active,
         "clicking the read-only compare right pane should make it the active navigable surface");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(WorkspaceShellTestAccess::ExecuteSelectAll(shell),
         "Select All should execute on the read-only compare right pane");
  Expect(WorkspaceShellTestAccess::ExecuteCopySelection(shell),
         "Copy Selection should execute on the read-only compare right pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "copying from the read-only compare right pane should copy its visible text");
}

void TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source).commits;
  Expect(history.size() == 2, "compare shortcut fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "read-only branch comparison should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const float row_y = surface.rows_y + surface.line_height * 0.5f;
  const float right_x = surface.right_x + 24.0f;
  Expect(SendMouseDown(shell, right_x, row_y, SDL_BUTTON_LEFT),
         "clicking the read-only compare right pane should be handled");

  std::string clipboard_text;
  WorkspaceShellTestAccess::SetClipboardTextWriter(
      shell, [&](std::string_view text) {
        clipboard_text = std::string(text);
        return true;
      });

  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL),
         "Ctrl+A should be handled on the read-only compare pane");
  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_CTRL),
         "Ctrl+C should be handled on the read-only compare pane");
  Expect(clipboard_text.find("head line") != std::string::npos,
         "compare shortcut copy should use the active read-only navigable viewport");
}

void TestWorkspaceShellCompareWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  std::string base_text;
  std::string working_text;
  for (int i = 0; i < 120; ++i) {
    base_text += "base line " + std::to_string(i) + "\n";
    working_text += ((i % 5 == 0) ? "changed line " + std::to_string(i) + "\n"
                                  : "base line " + std::to_string(i) + "\n");
  }
  WriteFile(source, base_text);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare wheel fixture", "compare wheel fixture");
  WriteFile(source, working_text);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(workspace::CompareTabPresentationRowCount(compare) >
             static_cast<std::size_t>(surface.visible_rows),
         "compare wheel fixture should overflow the viewport");

  const int before_scroll = compare.scroll_row;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the compare surface should be handled");
  Expect(compare.scroll_row > before_scroll,
         "scrolling the compare surface should advance the visible row");
}

void TestWorkspaceShellCompareWheelScrollsColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line =
      "abcdefghijklmnopqrstuvwxyz9876543210abcdefghijklmnopqrstuvwxyz9876543210\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare horizontal wheel fixture", "compare horizontal wheel fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare horizontal wheel fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare horizontal wheel fixture should overflow horizontally");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling the compare surface should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "horizontal scrolling the compare surface should advance the visible column");
}

void TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns() {
  // Regression: the two panes share one horizontal offset, but it used to be
  // round-tripped through the editable right viewport, which clamps to the right
  // document's longest line. When the overflowing content lives only on the left
  // (a long line shortened/deleted in the working tree), that read-back forced the
  // shared offset back to 0 and horizontal scrolling appeared completely dead.
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";

  // Long line at HEAD (left pane), collapsed to a short line in the working tree
  // (right pane). The right document on its own never overflows the viewport.
  const std::string base_line =
      "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789\n";
  const std::string working_line = "short\n";
  WriteFile(source, base_line);

  InitializeGitRepo(root);
  CommitAll(root, "Add compare left-dominated fixture", "compare left-dominated fixture");
  WriteFile(source, working_line);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 640, 420);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare left-dominated fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare.max_visual_columns > surface.visible_columns,
         "compare left-dominated fixture should overflow horizontally via the left pane");

  const std::size_t before_scroll = compare.horizontal_scroll;
  const float wheel_x = surface.right_x + 24.0f;
  const float wheel_y = surface.rows_y + surface.line_height * 0.5f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, 0, -1),
         "horizontal scrolling a left-dominated compare should be handled");
  Expect(compare.horizontal_scroll > before_scroll,
         "left-pane overflow should still advance the shared horizontal offset");
}

void TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare invalidation fixture", "compare invalidation fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const SDL_FRect editable_rect = WorkspaceShellTestAccess::ActiveCompareEditableRect(shell);
  const SDL_FRect left_rect =
      microide::workspace::MakeRect(surface.left_x, surface.rows_y,
                                    surface.gutter_width + surface.left_width,
                                    static_cast<float>(surface.visible_rows) * surface.line_height);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RIGHT;
  const auto result = shell.HandleEvent(event);
  const auto redraw_rect = result.redraw.SingleRectIfOnlyOne();

  Expect(result.handled, "compare horizontal navigation should be handled");
  Expect(!result.redraw.full && redraw_rect.has_value(),
         "compare horizontal navigation should stay on a partial redraw path");
  Expect(RectsIntersect(*redraw_rect, editable_rect),
         "compare horizontal navigation should repaint the editable pane");
  Expect(!RectsIntersect(*redraw_rect, left_rect),
         "compare horizontal navigation should avoid repainting the historical left pane");
}

void TestWorkspaceShellCompareSelectionStepInvalidatesRowBand() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "one\ntwo\nthree\nfour\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare row invalidation fixture", "compare row invalidation fixture");
  WriteFile(source, "one\nTWO\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare row invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto previous_row_rect = WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, 0, 1);
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_DOWN;
  const auto result = shell.HandleEvent(event);
  const auto next_row_rect = WorkspaceShellTestAccess::ActiveCompareRowRangeRect(shell, 1, 2);
  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);

  Expect(result.handled, "compare selection step should be handled");
  Expect(!result.redraw.full && !result.redraw.rects.empty(),
         "compare selection step should stay on a partial redraw path");
  Expect(previous_row_rect.has_value() && AnyRectIntersects(result.redraw.rects, *previous_row_rect),
         "compare selection step should repaint the previously selected row");
  Expect(next_row_rect.has_value() && AnyRectIntersects(result.redraw.rects, *next_row_rect),
         "compare selection step should repaint the newly selected row");
  Expect(MaxRectHeight(result.redraw.rects) <
             static_cast<float>(surface.visible_rows) * surface.line_height,
         "compare selection step should redraw less than the full compare surface height");
}

void TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare diagnostics fixture", "compare diagnostics fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare diagnostics fixture should open");
  Expect(WorkspaceShellTestAccess::PublishDiagnostics(
             shell, "diagnostics", source,
             {microide::editor::Diagnostic{
                 .range =
                     microide::editor::SelectionRange{
                         .start = microide::editor::TextPosition{.line = 0, .column = 0},
                         .end = microide::editor::TextPosition{.line = 0, .column = 3},
                     },
                 .severity = microide::editor::DiagnosticSeverity::Error,
                 .message = "compare error",
             }}),
         "compare diagnostics fixture should publish one right-pane diagnostic");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare diagnostics render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int marker_x = static_cast<int>(std::floor(surface.right_x + 3.0f));
  const int marker_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height - 2.0f));
  Expect(SDL_ReadSurfacePixel(pixels, marker_x, marker_y, &r, &g, &b, &a),
         "compare diagnostics render test should read the gutter marker pixel");
  Expect(r == theme.diagnostic_error.r && g == theme.diagnostic_error.g &&
             b == theme.diagnostic_error.b && a == theme.diagnostic_error.a,
         "compare right-pane diagnostics should paint severity markers in the gutter");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellCompareRenderReusesVisibleLayoutCache() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same\tline\nold\tline\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare layout cache fixture", "compare layout cache fixture");
  WriteFile(source, "same\tline\nnew\tline\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare layout cache fixture should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.visible_layout_cache.empty(),
         "compare visible-layout cache should start empty before first render");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  const std::size_t warmed_cache_size = compare.visible_layout_cache.size();
  Expect(warmed_cache_size >= 2,
         "first compare render should warm visible layouts for rendered panes");

  shell.Render(canvas.renderer(), 1280, 720);
  Expect(compare.visible_layout_cache.size() == warmed_cache_size,
         "stable compare frames should reuse cached visible layouts instead of appending more");

  compare.left_content = "same\tline\nolder\tline\n";
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.visible_layout_cache.empty(),
         "compare visible-layout cache should clear when the compare model changes");
}

void TestWorkspaceShellCompareBlameLoadsForWorkingTreePane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "line 1\nline 2\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare blame fixture", "compare blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.right_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveCompareBlameOverlay(shell, 3);
  Expect(overlay.has_value(),
         "clean working-tree comparison should eventually expose compare blame");
  Expect(overlay->lines.size() == 3,
         "compare blame should stay focused on the caret line and adjacent rows");
  Expect(overlay->lines[1].author == "Microide Tests",
         "compare blame should keep the blame author metadata");
  Expect(overlay->lines[1].summary == "Add compare blame fixture",
         "compare blame should keep the blame summary metadata");
}

void TestWorkspaceShellMergeBlameLoadsForResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  const std::filesystem::path base = temp_dir.path() / "base.cpp";
  const std::filesystem::path incoming = temp_dir.path() / "incoming.cpp";
  WriteFile(source, "line 1\ncurrent line\nline 3\nline 4\n");
  WriteFile(base, "line 1\nbase line\nline 3\nline 4\n");
  WriteFile(incoming, "line 1\nincoming line\nline 3\nline 4\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add merge blame fixture", "merge blame fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, source, source),
         "merge editor should open");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(1, 0);

  const auto overlay = WaitForActiveMergeBlameOverlay(shell, 3);
  Expect(overlay.has_value(),
         "clean merge result pane should eventually expose blame");
  Expect(overlay->lines.size() == 3,
         "merge blame should stay focused on the caret line and adjacent rows");
  Expect(overlay->lines[1].author == "Microide Tests",
         "merge blame should keep the blame author metadata");
  Expect(overlay->lines[1].summary == "Add merge blame fixture",
         "merge blame should keep the blame summary metadata");
}

void TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "nested" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare tab fixture", "compare tab fixture");
  WriteFile(source, "zero\none changed\ntwo changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "compare.txt",
         "compare tabs should display only the filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/nested/compare.txt",
         "compare tab tooltip should expose the full relative path");
  const std::string breadcrumb = WorkspaceShellTestAccess::BreadcrumbLabel(shell);
  Expect(breadcrumb.find("src/nested/compare.txt") != std::string::npos,
         "active compare breadcrumbs should keep the relative path");
  Expect(breadcrumb.find("HEAD -> Working tree") != std::string::npos,
         "active compare breadcrumbs should keep the compare refs");
}

void TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "src" / "result.txt";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  WriteFile(current, "top\ncurrent\nbottom\n");
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for compact-tab fixture");

  Expect(WorkspaceShellTestAccess::TabDisplayTitle(shell, 0) == "result.txt",
         "merge tabs should display only the output filename");
  Expect(WorkspaceShellTestAccess::TabTooltipLabel(shell, 0) == "src/result.txt",
         "merge tab tooltip should expose the full relative path");
  Expect(WorkspaceShellTestAccess::BreadcrumbLabel(shell).find("src/result.txt") != std::string::npos,
         "active merge breadcrumbs should keep the relative path");
}

// A conflicted binary/NUL worktree file must NOT build a text merge tab (the compare
// path already refuses these). Building one would let the result viewport later save text
// over binary bytes (TD-2026-07-17A-111). BuildMergeTabEntry classifies the three inputs
// and OpenMergeEditor returns false when any is binary/too-large/unreadable.
void TestWorkspaceShellMergeEditorRefusesBinaryInput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.bin";
  const std::filesystem::path incoming = root / "incoming.bin";
  const std::filesystem::path current = root / "current.bin";
  const std::filesystem::path output = root / "result.bin";
  WriteFile(base, "top\nbase\nbottom\n");
  WriteFile(incoming, "top\nincoming\nbottom\n");
  // Current side carries an embedded NUL — classified as binary, not editable text.
  WriteFile(current, std::string("top\ncur\0rent\nbottom\n", 20));
  WriteFile(output, "top\ncurrent\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(!WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "a binary/NUL merge input must not build a text merge tab");

  // Sanity: with the same paths made all-text, the merge editor opens.
  WriteFile(current, "top\ncurrent\nbottom\n");
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "an all-text merge input should still open the merge editor");
}

void TestWorkspaceShellCompareDividerMatchesMarkerWidth() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider fixture", "compare divider fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(surface.divider_width <= std::ceil(WorkspaceShellTestAccess::TextCharWidth(shell)) + 1.0f,
         "compare divider should stay at roughly one glyph wide");
  Expect(surface.right_x - surface.center_x <= surface.divider_width + 0.5f,
         "compare divider should not reserve extra empty space before the right gutter");
}

void TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows() {
#if !MICROIDE_HAS_SDL3_TTF
  return;
#endif
  EnsureDummySdlVideo();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "same line\nold line\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare divider border fixture", "compare divider border fixture");
  WriteFile(source, "same line\nnew line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare divider border fixture should open");

  SoftwareCanvas canvas(1280, 720);
  shell.Render(canvas.renderer(), 1280, 720);
  SDL_Surface* pixels = SDL_RenderReadPixels(canvas.renderer(), nullptr);
  Expect(pixels != nullptr, "compare divider border render test should read software pixels");

  const auto surface = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  const auto theme = microide::render::MakeDefaultTheme();
  Uint8 r = 0;
  Uint8 g = 0;
  Uint8 b = 0;
  Uint8 a = 0;
  const int border_x = static_cast<int>(std::floor(surface.center_x));
  const int border_y = static_cast<int>(std::floor(surface.rows_y + surface.line_height * 0.5f));
  Expect(SDL_ReadSurfacePixel(pixels, border_x, border_y, &r, &g, &b, &a),
         "compare divider border render test should read the divider border pixel");
  Expect(r == theme.border.r && g == theme.border.g &&
             b == theme.border.b && a == theme.border.a,
         "compare divider should keep a visible left border on unchanged rows");

  SDL_DestroySurface(pixels);
}

void TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz0123456789\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare resize fixture", "compare resize fixture");
  WriteFile(source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "compare resize fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.75f;
  const auto left_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(left_wide.left_visible_columns > left_wide.right_visible_columns,
         "widening the left compare pane should preserve more visible text on the left");

  compare.divider_fraction = 0.25f;
  const auto right_wide = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(right_wide.right_visible_columns > right_wide.left_visible_columns,
         "widening the right compare pane should preserve more visible text on the right");
}

void TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path compare_source = root / "src" / "compare.cpp";
  const std::filesystem::path base = root / "src" / "base.cpp";
  const std::filesystem::path incoming = root / "src" / "incoming.cpp";
  const std::filesystem::path current = root / "src" / "current.cpp";
  const std::filesystem::path output = root / "src" / "output.cpp";

  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz0123456789\n");
  WriteFile(base, "int answer() {\n  return 0;\n}\n");
  WriteFile(incoming, "int answer() {\n  return 1;\n}\n");
  WriteFile(current, "int answer() {\n  return 2;\n}\n");
  WriteFile(output, "int answer() {\n  return 0;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add pane minima fixture", "pane minima fixture");
  WriteFile(compare_source, "abcdefghijklmnopqrstuvwxyz9876543210\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, compare_source, "HEAD", "HEAD"),
         "pane minima compare fixture should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.divider_fraction = 0.0f;
  const auto compare_left_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  compare.divider_fraction = 1.0f;
  const auto compare_right_min = WorkspaceShellTestAccess::ActiveCompareSurfaceLayout(shell);
  Expect(compare_left_min.left_visible_columns >= 1 && compare_left_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum left fraction");
  Expect(compare_right_min.left_visible_columns >= 1 && compare_right_min.right_visible_columns >= 1,
         "compare layout should preserve one visible column per pane at minimum right fraction");

  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "pane minima merge fixture should open");
  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.left_divider_fraction = 0.0f;
  merge.right_divider_fraction = 1.0f;
  const auto merge_surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(merge_surface.visible_columns >= 1,
         "merge layout should preserve one visible column at minimum divider fractions");
}

void TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "conflict.txt";
  WriteFile(base, "line 1\nshared\n");
  WriteFile(incoming, "line 1\nincoming change\nline 3\n");
  WriteFile(current, "line 1\ncurrent change\nline 3\n");
  WriteFile(output, "line 1\nshared\nline 3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1920, 1080);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge toolbar layout fixture should open");

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const auto toolbar = WorkspaceShellTestAccess::MergeToolbarNavigationRects(shell);

  constexpr float kMergeToolbarButtonHeight = 22.0f;
  Expect(surface.header_y >= surface.secondary_button_y + kMergeToolbarButtonHeight + 4.0f,
         "pane headers should sit below the secondary toolbar row");
  Expect(surface.rows_y >= surface.header_y + surface.line_height,
         "merge rows should start below pane headers");
  Expect(toolbar[0].y + toolbar[0].h <= surface.secondary_button_y + 0.5f,
         "primary toolbar should stay on the first row");
  Expect(surface.secondary_button_y + kMergeToolbarButtonHeight <= surface.header_y + 0.5f,
         "secondary toolbar should stay above pane headers");
}

void TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add commit picker fixture", "commit picker fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");
  CommitAll(root, "Edit commit picker fixture", "commit picker fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source),
         "opening the commit picker for a file with history should succeed");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "the commit picker overlay should be visible after opening");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::CommitPicker,
         "the open overlay should be the commit picker");

  // The overlay opens immediately in a loading state; wait for the async git
  // history query to populate the picker before selecting a commit.
  Expect(SettleComparePicker(shell), "the async commit picker query should settle");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "pressing Enter in the commit picker should be handled");

  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell),
         "selecting a commit should dismiss the picker instead of leaving it over the comparison");
  Expect(WorkspaceShellTestAccess::ActiveTabIsCompare(shell),
         "selecting a commit should open and focus the comparison tab");
}

microide::project::GitCommitEntry MakeFakeCommit(int index) {
  microide::project::GitCommitEntry commit;
  const std::string suffix = std::to_string(index);
  commit.hash = "deadbeef" + suffix;
  commit.short_hash = "dead" + suffix;
  commit.subject = "fake commit " + suffix;
  commit.author = "tester";
  commit.relative_date = "just now";
  return commit;
}

void TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source_a = root / "src" / "a.cpp";
  const std::filesystem::path source_b = root / "src" / "b.cpp";

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Injected provider: blocks on `release` until the test lets it return, and
  // returns `call_count` commits so each completion is distinguishable by size.
  std::atomic<bool> release{false};
  std::atomic<int> call_count{0};
  WorkspaceShellTestAccess::SetComparePickerFileHistoryProvider(
      shell,
      [&release, &call_count](const std::filesystem::path&, const std::filesystem::path&) {
        const int this_call = ++call_count;
        while (!release.load(std::memory_order_acquire)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        microide::project::GitFileHistoryResult result;
        for (int i = 0; i < this_call; ++i) {
          result.commits.push_back(MakeFakeCommit(i));
        }
        return result;
      });

  // First open: the overlay must appear immediately in a loading state, BEFORE
  // the (blocked) git query returns. This is the core win of the async change.
  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_a),
         "opening the async commit picker should succeed immediately");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::CommitPicker,
         "the commit picker overlay should be visible while the query is in flight");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should report loading before git returns");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 0,
         "the picker item list should be empty while loading");

  // Wait until the worker has actually entered the (blocked) first provider call
  // so the second open queues behind an in-flight job.
  const auto call_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (call_count.load() < 1 && std::chrono::steady_clock::now() < call_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(call_count.load() >= 1, "the first async history query should have started");

  // Second open supersedes the first (bumps the request generation). When both
  // complete, the stale first completion must be dropped and only the second
  // (two commits) applied.
  Expect(WorkspaceShellTestAccess::OpenComparePickerForPath(shell, source_b),
         "opening a second async commit picker should succeed");
  Expect(WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the superseding picker should also start in a loading state");

  release.store(true, std::memory_order_release);
  Expect(SettleComparePicker(shell), "both async queries should settle");
  Expect(!WorkspaceShellTestAccess::ComparePickerLoading(shell),
         "the picker should leave the loading state once the current query lands");
  Expect(WorkspaceShellTestAccess::ComparePickerItemCount(shell) == 2,
         "only the superseding (second) result should populate the picker, not the stale first");
}

void TestWorkspaceShellCompareRecomputeGate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "int alpha() {\n  return 1;\n}\n");
  InitializeGitRepo(root);
  CommitAll(root, "Add compare gate fixture", "compare gate fixture");
  WriteFile(source, "int beta() {\n  return 2;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);

  // A refresh with no content change must reuse the existing model.
  const std::uint64_t baseline_revision = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision,
         "no-op refreshes must not rebuild the compare model");

  // A left-content change must rebuild exactly once.
  compare.left_content = "int gamma() {\n  return 3;\n}\n";
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == baseline_revision + 1,
         "changed content must rebuild the compare model once");
  const std::uint64_t after_content = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content,
         "refresh after a rebuild with unchanged content must not rebuild again");

  // Toggling a build option changes the model without a content change.
  compare.build_options.ignore_whitespace = !compare.build_options.ignore_whitespace;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_content + 1,
         "toggling ignore-whitespace must rebuild the compare model");

  // An edit to the editable right pane is detected via the viewport's monotonic
  // content_revision (no whole-buffer serialize on the no-op path) and rebuilds once;
  // a subsequent no-op refresh must reuse the model rather than re-serialize + rebuild.
  const std::uint64_t before_right_edit = compare.model_revision;
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "// note "),
         "typing into the editable current-state pane should edit it");
  Expect(compare.model_revision > before_right_edit,
         "editing the right pane must rebuild the compare model");
  const std::uint64_t after_right_edit = compare.model_revision;
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  WorkspaceShellTestAccess::RefreshActiveCompareDerivedState(shell);
  Expect(compare.model_revision == after_right_edit,
         "no-op refreshes after a right-pane edit must not rebuild the compare model");
}

}  // namespace

// H11/J41: an unreadable or binary working-tree file must NOT open as an editable
// compare showing "whole file deleted" (which could then save a false empty file).
// A genuinely-missing working-tree file is a real deletion and still opens (empty).
// Distinct files per case avoid compare-tab reuse collisions on the same path.
void TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path missing_file = root / "src" / "missing.cpp";
  const std::filesystem::path binary_file = root / "src" / "binary.cpp";
  const std::filesystem::path locked_file = root / "src" / "locked.cpp";
  WriteFile(missing_file, "int alpha() {\n  return 1;\n}\n");
  WriteFile(binary_file, "int beta() {\n  return 2;\n}\n");
  WriteFile(locked_file, "int gamma() {\n  return 3;\n}\n");

  InitializeGitRepo(root);
  CommitAll(root, "Add compare reject fixture", "compare reject fixture");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);

  // Missing working-tree file: a legitimate deletion -> opens as an (empty) diff.
  std::filesystem::remove(missing_file);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, missing_file, "HEAD", "HEAD"),
         "a missing working-tree file should still open as a whole-file-deleted diff");

  // Binary working-tree file (early AND late NUL): must be refused, not rendered as
  // deleted/empty text.
  std::string binary_bytes;
  binary_bytes.push_back('\0');            // early NUL
  binary_bytes.append("PNG\r\n\x1a\n binary body ");
  binary_bytes.push_back('\0');            // late NUL
  binary_bytes.append(" trailing");
  WriteFile(binary_file, binary_bytes);
  Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, binary_file, "HEAD", "HEAD"),
         "a binary working-tree file must not open as an editable text compare");

  // Unreadable working-tree file: must be refused (not treated as empty). chmod 000
  // is bypassed by root, so guard the assertion when the harness runs privileged.
  std::error_code ec;
  std::filesystem::permissions(locked_file, std::filesystem::perms::none,
                               std::filesystem::perm_options::replace, ec);
  if (!ec) {
    std::ifstream probe(locked_file, std::ios::binary);
    const bool actually_locked = !probe.good();
    probe.close();
    if (actually_locked) {
      Expect(!WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, locked_file, "HEAD", "HEAD"),
             "an unreadable working-tree file must not open as an empty compare");
    }
    std::filesystem::permissions(locked_file, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
  }
}

// Regression: syntax highlighting reaches hunks deep in a collapsed large-file
// diff. The tokenizer window is derived in MODEL space from the visible
// presentation rows (a collapsed run maps a low presentation index to a high model
// row), with a continued redraw catching the cumulative tokenizer up. Previously
// the window used the presentation scroll position, capping tokenization far below
// the visible model rows, so deep rows drew unhighlighted.
void TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "big.cpp";
  // A large file whose long middle is unchanged (so it collapses) with edits at the
  // top and bottom, so the bottom hunk sits at a high MODEL row behind a collapsed
  // run of a much smaller PRESENTATION height.
  std::string base;
  for (int i = 0; i < 300; ++i) {
    base += "int v" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
  }
  WriteFile(source, base);
  InitializeGitRepo(root);
  CommitAll(root, "big base fixture", "big base fixture");
  std::string edited = base;
  edited.replace(edited.find("int v0 = 0;"), std::string("int v0 = 0;").size(), "int v0 = 999;");
  edited.replace(edited.find("int v299 = 299;"), std::string("int v299 = 299;").size(),
                 "int v299 = 42;");
  WriteFile(source, edited);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "large collapsed-diff comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t model_row_count = compare.model.rows.size();
  const std::size_t presentation_row_count =
      WorkspaceShellTestAccess::ActiveComparePresentationRowCount(shell);
  Expect(model_row_count > presentation_row_count + 100,
         "the unchanged middle should collapse so model rows far exceed presentation rows");

  // Scroll to the bottom so the deep (high-model-row) hunk is on screen.
  compare.scroll_row = static_cast<int>(presentation_row_count);

  // Render repeatedly to let the per-frame-capped cumulative tokenizer catch up.
  SoftwareCanvas canvas(1280, 720);
  for (int frame = 0; frame < 20; ++frame) {
    shell.Render(canvas.renderer(), 1280, 720);
  }

  auto& tokenized_compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(tokenized_compare.syntax_rows_tokenized == tokenized_compare.model.rows.size(),
         "tokenization reaches the deepest visible model row behind the collapsed run");
  // A deep content row (the bottom v299 change) is highlighted — not left blank.
  // Scan the tail (the very last model row is the phantom trailing empty line).
  bool deep_row_has_tokens = false;
  const std::size_t tail_start = tokenized_compare.model.rows.size() > 8
                                     ? tokenized_compare.model.rows.size() - 8
                                     : 0;
  for (std::size_t row = tail_start; row < tokenized_compare.model.rows.size(); ++row) {
    if (!tokenized_compare.left_tokens_by_row[row].empty() ||
        !tokenized_compare.right_tokens_by_row[row].empty()) {
      deep_row_has_tokens = true;
      break;
    }
  }
  Expect(deep_row_has_tokens,
         "a deep content row behind the collapsed run has syntax tokens (is highlighted)");
}

void RegisterWorkspaceShellCompareTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/CompareSyntaxReachesDeepCollapsedRows",
          TestWorkspaceShellCompareSyntaxReachesDeepCollapsedRows);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareIsEditableAndSaves",
          TestWorkspaceShellWorkingTreeCompareIsEditableAndSaves);
  AddTest(tests, "WorkspaceShell/WorkingTreeCompareRejectsBinaryAndUnreadable",
          TestWorkspaceShellWorkingTreeCompareRejectsBinaryAndUnreadable);
  AddTest(tests, "WorkspaceShell/CompareClickTogglesEditablePaneFocus",
          TestWorkspaceShellCompareClickTogglesEditablePaneFocus);
  AddTest(tests, "WorkspaceShell/CompareClickAboveFirstRowIsNotHandled",
          TestWorkspaceShellCompareClickAboveFirstRowIsNotHandled);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsExpandHiddenRows",
          TestWorkspaceShellCompareCollapsedContextButtonsExpandHiddenRows);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextButtonsHoverAsInteractive",
          TestWorkspaceShellCompareCollapsedContextButtonsHoverAsInteractive);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionPersistsAcrossChunks",
          TestWorkspaceShellCompareCollapsedContextExpansionPersistsAcrossChunks);
  AddTest(tests, "WorkspaceShell/CompareCollapsedContextExpansionSurvivesTreeRefresh",
          TestWorkspaceShellCompareCollapsedContextExpansionSurvivesTreeRefresh);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareRightPaneSupportsSelectAllAndCopy",
          TestWorkspaceShellReadOnlyCompareRightPaneSupportsSelectAllAndCopy);
  AddTest(tests, "WorkspaceShell/ReadOnlyCompareShortcutCopyUsesNavigableViewport",
          TestWorkspaceShellReadOnlyCompareShortcutCopyUsesNavigableViewport);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsRows",
          TestWorkspaceShellCompareWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsColumns",
          TestWorkspaceShellCompareWheelScrollsColumns);
  AddTest(tests, "WorkspaceShell/CompareWheelScrollsLeftDominatedColumns",
          TestWorkspaceShellCompareWheelScrollsLeftDominatedColumns);
  AddTest(tests, "WorkspaceShell/CompareHorizontalNavigationInvalidatesEditablePane",
          TestWorkspaceShellCompareHorizontalNavigationInvalidatesEditablePane);
  AddTest(tests, "WorkspaceShell/CompareSelectionStepInvalidatesRowBand",
          TestWorkspaceShellCompareSelectionStepInvalidatesRowBand);
  AddTest(tests, "WorkspaceShell/CompareRenderPaintsDiagnosticGutterMarkers",
          TestWorkspaceShellCompareRenderPaintsDiagnosticGutterMarkers);
  AddTest(tests, "WorkspaceShell/CompareRenderReusesVisibleLayoutCache",
          TestWorkspaceShellCompareRenderReusesVisibleLayoutCache);
  AddTest(tests, "WorkspaceShell/CompareBlameLoadsForWorkingTreePane",
          TestWorkspaceShellCompareBlameLoadsForWorkingTreePane);
  AddTest(tests, "WorkspaceShell/MergeBlameLoadsForResultPane",
          TestWorkspaceShellMergeBlameLoadsForResultPane);
  AddTest(tests, "WorkspaceShell/CompareTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellCompareTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeTabUsesFilenameOnlyLabelAndTooltip",
          TestWorkspaceShellMergeTabUsesFilenameOnlyLabelAndTooltip);
  AddTest(tests, "WorkspaceShell/MergeEditorRefusesBinaryInput",
          TestWorkspaceShellMergeEditorRefusesBinaryInput);
  AddTest(tests, "WorkspaceShell/CompareDividerMatchesMarkerWidth",
          TestWorkspaceShellCompareDividerMatchesMarkerWidth);
  AddTest(tests, "WorkspaceShell/CompareRenderKeepsDividerBorderOnUnchangedRows",
          TestWorkspaceShellCompareRenderKeepsDividerBorderOnUnchangedRows);
  AddTest(tests, "WorkspaceShell/ComparePaneResizeKeepsWiderPaneTextVisible",
          TestWorkspaceShellComparePaneResizeKeepsWiderPaneTextVisible);
  AddTest(tests, "WorkspaceShell/CompareAndMergePaneMinimaPreserveVisibleColumns",
          TestWorkspaceShellCompareAndMergePaneMinimaPreserveVisibleColumns);
  AddTest(tests, "WorkspaceShell/MergeToolbarLayoutClearsPaneHeaders",
          TestWorkspaceShellMergeToolbarLayoutClearsPaneHeaders);
  AddTest(tests, "WorkspaceShell/CompareRecomputeGate", TestWorkspaceShellCompareRecomputeGate);
  AddTest(tests, "WorkspaceShell/CommitPickerDismissesAfterOpeningCompare",
          TestWorkspaceShellCommitPickerDismissesAfterOpeningCompare);
  AddTest(tests, "WorkspaceShell/ComparePickerOpensAsyncAndDropsStaleResult",
          TestWorkspaceShellComparePickerOpensAsyncAndDropsStaleResult);
}

}  // namespace microide::tests
