#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/GitSidebarCommandCenter.h"
#include "workspace/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceSidebarCoordinator.h"

namespace microide::workspace {

namespace {

constexpr float kSidebarHeaderHeight = 26.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kGitSidebarActionRowTop = 34.0f;
constexpr float kGitSidebarActionButtonHeight = 18.0f;
constexpr float kGitSidebarActionGap = 6.0f;
constexpr float kGitSidebarListGap = 8.0f;
constexpr float kTreeSidebarActionRowTop = 34.0f;
constexpr float kTreeSidebarActionButtonHeight = 18.0f;
constexpr float kTreeSidebarListGap = 8.0f;
constexpr float kGitSidebarSummaryLineHeight = 14.0f;
constexpr float kGitSidebarEntryButtonGap = 4.0f;
constexpr float kGitSidebarEntryButtonHoverPadding = 4.0f;
constexpr float kGitSidebarHeaderMenuButtonSize = 16.0f;
constexpr float kSidebarHeaderCompactButtonSize = 18.0f;

SDL_FRect ExpandRect(const SDL_FRect& rect, float padding) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return rect;
  }
  return MakeRect(rect.x - padding, rect.y - padding, rect.w + padding * 2.0f,
                  rect.h + padding * 2.0f);
}

bool UseCompactTreeHeader(float sidebar_width,
                          float mode_min_width,
                          float collapse_width,
                          float refresh_width) {
  const float minimum_required_width = 10.0f + mode_min_width + 6.0f + collapse_width + 6.0f +
                                       refresh_width + 10.0f;
  return sidebar_width < minimum_required_width;
}

}  // namespace

SDL_FRect WorkspaceShell::GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x + (button_width + kGitSidebarActionGap) * 2.0f, row_rect.y,
                  button_width, row_rect.h);
}

SDL_FRect WorkspaceShell::GitSidebarActionRowRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  return MakeRect(sidebar_rect.x + kSidebarInset, sidebar_rect.y + kGitSidebarActionRowTop,
                  std::max(0.0f, sidebar_rect.w - kSidebarInset * 2.0f),
                  kGitSidebarActionButtonHeight);
}

SDL_FRect WorkspaceShell::GitSidebarStageAllButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x, row_rect.y, button_width, row_rect.h);
}

SDL_FRect WorkspaceShell::GitSidebarDiscardAllButtonRect(const SDL_FRect& sidebar_rect) const {
  const SDL_FRect row_rect = GitSidebarActionRowRect(sidebar_rect);
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width =
      std::max(0.0f, (row_rect.w - kGitSidebarActionGap * 2.0f) / 3.0f);
  return MakeRect(row_rect.x + button_width + kGitSidebarActionGap, row_rect.y, button_width,
                  row_rect.h);
}

std::optional<SDL_FRect> WorkspaceShell::GitSidebarOutgoingBaseButtonRect(
    const SDL_FRect& sidebar_rect) const {
  const auto lines = BuildGitSidebarLines();
  const auto list_layout = ComputeGitSidebarListLayout(sidebar_rect, lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    if (line.kind != GitSidebarLine::Kind::Header ||
        line.section != GitSidebarEntry::Section::Outgoing) {
      continue;
    }

    const int visible_row = static_cast<int>(i) - list_layout.scroll_row;
    if (visible_row < 0 || visible_row >= list_layout.visible_rows) {
      return std::nullopt;
    }

    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, visible_row);
    const float size = std::min(kGitSidebarHeaderMenuButtonSize, row_rect.h - 2.0f);
    return MakeRect(row_rect.x + row_rect.w - size - 4.0f, row_rect.y + (row_rect.h - size) * 0.5f,
                    size, size);
  }
  return std::nullopt;
}

std::vector<std::string> WorkspaceShell::GitSidebarSummaryLines() const {
  std::vector<std::string> lines;
  std::string scm_line = "SCM: Git";
  for (const ScmProviderSpec& provider : scm_registry_.Specs()) {
    scm_line += ", ";
    scm_line += provider.label.empty() ? provider.id : provider.label;
  }
  lines.push_back(std::move(scm_line));

  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
  lines.insert(lines.end(), view_model.summary_lines.begin(), view_model.summary_lines.end());
  return lines;
}

float WorkspaceShell::GitSidebarListTop(const SDL_FRect& sidebar_rect) const {
  const float summary_height =
      static_cast<float>(GitSidebarSummaryLines().size()) * kGitSidebarSummaryLineHeight;
  return sidebar_rect.y + kGitSidebarActionRowTop + kGitSidebarActionButtonHeight +
         kGitSidebarListGap + summary_height +
         (summary_height > 0.0f ? kGitSidebarListGap * 0.5f : 0.0f) + GitSidebarCommitWorkflowHeight() +
         (GitSidebarCommitWorkflowHeight() > 0.0f ? kGitSidebarListGap : 0.0f);
}

float WorkspaceShell::GitSidebarVisibleUnits(const SDL_FRect& sidebar_rect) const {
  return std::max(1.0f, (sidebar_rect.y + sidebar_rect.h - GitSidebarListTop(sidebar_rect)) /
                            kSidebarRowHeight);
}

ScrollableListLayout WorkspaceShell::ComputeProjectSearchSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kProjectSearchResultsTop,
                                     line_count, context_.current_project_state.sidebar.scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeGitSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                 std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, GitSidebarListTop(sidebar_rect), line_count,
                                     context_.current_project_state.sidebar.scroll_row, kSidebarInset, kSidebarRowHeight,
                                     kSidebarRowHeight - 2.0f, 0.0f, 0.0f, true);
}

ScrollableListLayout WorkspaceShell::ComputeTreeSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                  std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect,
                                     sidebar_rect.y + kTreeSidebarActionRowTop +
                                         kTreeSidebarActionButtonHeight + kTreeSidebarListGap,
                                     line_count, context_.current_project_state.sidebar.scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeProblemsSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeTestsSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                   std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputePluginSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                    std::size_t line_count) const {
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row,
                                     kSidebarInset, kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

SDL_FRect WorkspaceShell::TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float refresh_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  const float collapse_width = std::max(76.0f, text_renderer_.MeasureWidth("Collapse") + 18.0f);
  const bool compact_header =
      UseCompactTreeHeader(sidebar_rect.w, 0.0f, collapse_width, refresh_width);
  const float action_row_y = sidebar_rect.y + kTreeSidebarActionRowTop;
  const float action_row_x = sidebar_rect.x + kSidebarInset;
  if (compact_header) {
    return MakeRect(action_row_x, action_row_y,
                    kSidebarHeaderCompactButtonSize, kSidebarHeaderCompactButtonSize);
  }

  return MakeRect(action_row_x, action_row_y, collapse_width,
                  kTreeSidebarActionButtonHeight);
}

SDL_FRect WorkspaceShell::TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  const float collapse_width = std::max(76.0f, text_renderer_.MeasureWidth("Collapse") + 18.0f);
  const bool compact_header =
      UseCompactTreeHeader(sidebar_rect.w, 0.0f, collapse_width, button_width);
  const float action_row_y = sidebar_rect.y + kTreeSidebarActionRowTop;
  const float action_row_x = sidebar_rect.x + kSidebarInset;
  if (compact_header) {
    return MakeRect(action_row_x + kSidebarHeaderCompactButtonSize + 6.0f, action_row_y,
                    kSidebarHeaderCompactButtonSize,
                    kSidebarHeaderCompactButtonSize);
  }

  return MakeRect(action_row_x + collapse_width + 6.0f, action_row_y,
                  button_width, kTreeSidebarActionButtonHeight);
}

std::string WorkspaceShell::SidebarModeControlLabel() const {
  if (const std::optional<SidebarViewInfo> view =
          FindSidebarView(context_.current_project_state.sidebar.view_id, plugin_runtime_.Host());
      view.has_value()) {
    return std::string(view->label);
  }
  if (const SidebarViewSpec* view = FindBuiltinSidebarView(ActiveSidebarMode());
      view != nullptr) {
    return std::string(view->label);
  }
  return "Project";
}

SDL_FRect WorkspaceShell::SidebarModeControlRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const std::string label = SidebarModeControlLabel();
  const float left = sidebar_rect.x + 10.0f;
  const float reserved_right = 10.0f;
  const float max_width =
      std::max(0.0f, sidebar_rect.w - (left - sidebar_rect.x) - reserved_right - 6.0f);
  const float desired_width =
      std::clamp(text_renderer_.MeasureWidth(label) + 30.0f, 92.0f, std::max(92.0f, max_width));
  const float width = std::min(desired_width, max_width);
  if (width <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }
  return MakeRect(left, sidebar_rect.y + 4.0f, width, 18.0f);
}

std::string WorkspaceShell::HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !context_.current_project_state.sidebar.visible ||
      ActiveSidebarMode() != SidebarMode::Git || MenuSurfaceCapturingMouse() ||
      !Contains(sidebar_rect, last_mouse_x_, last_mouse_y_)) {
    return {};
  }

  if (last_mouse_y_ < GitSidebarListTop(sidebar_rect)) {
    return {};
  }

  const auto lines = BuildGitSidebarLines();
  const auto list_layout = ComputeGitSidebarListLayout(sidebar_rect, lines.size());
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    if (line.kind != GitSidebarLine::Kind::Entry || line.entry_index < 0 ||
        static_cast<std::size_t>(line.entry_index) >= context_.current_project_state.sidebar.git.entries.size()) {
      continue;
    }

    const int visible_row = static_cast<int>(i) - list_layout.scroll_row;
    if (visible_row < 0 || visible_row >= list_layout.visible_rows) {
      continue;
    }

    const auto& entry = context_.current_project_state.sidebar.git.entries[static_cast<std::size_t>(line.entry_index)];
    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, visible_row);
    const GitSidebarEntryActionLayout actions = ComputeGitSidebarEntryActionLayout(row_rect, entry);
    if (actions.primary_rect.has_value() &&
        Contains(ExpandRect(*actions.primary_rect, kGitSidebarEntryButtonHoverPadding),
                 last_mouse_x_, last_mouse_y_)) {
      const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
          entry, context_.current_project_state.sidebar.git.repo_available,
          context_.current_project_state.sidebar.git.supports_mutations);
      return availability.unstage ? "Unstage file" : "Stage file";
    }
    if (actions.discard_rect.has_value() &&
        Contains(ExpandRect(*actions.discard_rect, kGitSidebarEntryButtonHoverPadding),
                 last_mouse_x_, last_mouse_y_)) {
      switch (entry.section) {
        case GitSidebarEntry::Section::Staged:
          return "Discard staged changes";
        case GitSidebarEntry::Section::Conflicts:
          return "Discard conflicted changes";
        case GitSidebarEntry::Section::Untracked:
          return "Discard untracked file";
        case GitSidebarEntry::Section::Changed:
          return "Discard unstaged changes";
        case GitSidebarEntry::Section::Outgoing:
          return "Discard is unavailable";
      }
      return "Discard changes";
    }
  }
  return {};
}

std::optional<SDL_FRect> WorkspaceShell::HoveredGitSidebarTooltipRect(const WorkspaceLayout& layout) const {
  const std::string label = HoveredGitSidebarTooltipLabel(layout.sidebar);
  if (label.empty()) {
    return std::nullopt;
  }

  const auto tooltip = detail::BuildTooltipLayout(
      text_renderer_, label, std::max(180.0f, layout.full.w - layout.sidebar.w - 24.0f));
  const float tooltip_x =
      std::clamp(last_mouse_x_ + 12.0f, layout.full.x + 8.0f,
                 layout.full.x + layout.full.w - tooltip.rect.w - 8.0f);
  const float tooltip_y =
      last_mouse_y_ - tooltip.rect.h - 10.0f >= layout.full.y + 8.0f
          ? last_mouse_y_ - tooltip.rect.h - 10.0f
          : std::clamp(last_mouse_y_ + 14.0f, layout.full.y + 8.0f,
                       layout.full.y + layout.full.h - tooltip.rect.h - 8.0f);
  return MakeRect(tooltip_x, tooltip_y, tooltip.rect.w, tooltip.rect.h);
}

std::vector<WorkspaceShell::GitSidebarLine> WorkspaceShell::BuildGitSidebarLines() const {
  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
  const auto specs = BuildGitSidebarLineSpecs(view_model);
  std::vector<GitSidebarLine> lines;
  lines.reserve(specs.size());
  for (const GitSidebarLineSpec& spec : specs) {
    lines.push_back(GitSidebarLine{
        .kind = spec.kind == GitSidebarLineKind::Header
                    ? GitSidebarLine::Kind::Header
                    : spec.kind == GitSidebarLineKind::Entry ? GitSidebarLine::Kind::Entry
                                                             : GitSidebarLine::Kind::Empty,
        .section = spec.section,
        .label = spec.label,
        .entry_index = spec.entry_index,
    });
  }
  return lines;
}

WorkspaceShell::GitSidebarEntryActionLayout WorkspaceShell::ComputeGitSidebarEntryActionLayout(
    const SDL_FRect& row_rect,
    const GitSidebarEntry& entry) const {
  GitSidebarEntryActionLayout layout;
  layout.content_right_edge = row_rect.x + row_rect.w - 8.0f;
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f) {
    return layout;
  }

  const GitSidebarActionAvailability availability = GitSidebarActionAvailabilityForEntry(
      entry, context_.current_project_state.sidebar.git.repo_available,
      context_.current_project_state.sidebar.git.supports_mutations);
  if (!availability.stage && !availability.unstage && !availability.discard) {
    return layout;
  }

  const auto button_rect = [&](float right_edge, std::string_view label) {
    const float width = std::max(22.0f, text_renderer_.MeasureWidth(label) + 12.0f);
    return MakeRect(right_edge - width, row_rect.y + 1.0f, width, row_rect.h - 2.0f);
  };

  if (availability.stage || availability.unstage) {
    const SDL_FRect primary_rect = button_rect(layout.content_right_edge,
                                               availability.unstage ? "Unstage" : "Stage");
    layout.primary_rect = primary_rect;
    layout.content_right_edge = primary_rect.x - kGitSidebarEntryButtonGap;
  }
  if (availability.discard) {
    const SDL_FRect discard_rect = button_rect(layout.content_right_edge, "Discard");
    layout.discard_rect = discard_rect;
    layout.content_right_edge = discard_rect.x - 6.0f;
  }
  return layout;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return std::nullopt;
  }

  const GitSidebarViewModel view_model =
      BuildGitSidebarViewModel(context_.current_project_state.sidebar.git,
                             context_.current_project_state.root,
                             context_.current_project_state.branch_review);
  const auto specs = BuildGitSidebarLineSpecs(view_model);
  return FindSelectedGitSidebarLineIndex(specs, context_.current_project_state.sidebar.git.selected_index);
}

const WorkspaceShell::GitSidebarEntry* WorkspaceShell::SelectedGitSidebarEntry() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return nullptr;
  }
  return &context_.current_project_state.sidebar.git.entries[context_.current_project_state.sidebar.git.selected_index];
}

void WorkspaceShell::SetGitOutgoingBaseChoice(OutgoingBaseChoice choice) {
  context_.current_project_state.sidebar.git.outgoing_base_choice = std::move(choice);
  MakePersistenceCoordinator().SaveSessionState();
  RequestGitSidebarRefresh(GitSidebarRefreshScope::Full);
  ConsumeGitSidebarRefresh();
}

void WorkspaceShell::OpenGitOutgoingBasePrompt() {
  OpenPromptSurface(PromptSurfaceState::Action::SetGitOutgoingBaseRef,
                    PromptSurfaceState::Kind::TextInput,
                    context_.current_project_state.root,
                    context_.current_project_state.sidebar.git.outgoing_base_choice.custom_ref);
}

}  // namespace microide::workspace
