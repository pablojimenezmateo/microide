#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "workspace/WorkspaceGitSidebarPresentation.h"
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
constexpr float kGitSidebarSummaryLineHeight = 14.0f;
constexpr float kGitSidebarEntryButtonGap = 4.0f;
constexpr float kGitSidebarEntryButtonHoverPadding = 4.0f;

SDL_FRect ExpandRect(const SDL_FRect& rect, float padding) {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return rect;
  }
  return MakeRect(rect.x - padding, rect.y - padding, rect.w + padding * 2.0f,
                  rect.h + padding * 2.0f);
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

std::vector<std::string> WorkspaceShell::GitSidebarSummaryLines() const {
  std::vector<std::string> lines;

  std::string scm_line = "SCM: Git";
  for (const ScmProviderSpec& provider : scm_registry_.Specs()) {
    scm_line += ", ";
    scm_line += provider.label.empty() ? provider.id : provider.label;
  }
  lines.push_back(std::move(scm_line));

  if (!auth_provider_registry_.Providers().empty()) {
    std::string auth_line = "Accounts: ";
    bool first = true;
    for (const AuthProviderSpec& provider : auth_provider_registry_.Providers()) {
      if (!first) {
        auth_line += ", ";
      }
      first = false;
      const std::size_t session_count =
          static_cast<std::size_t>(std::count_if(auth_provider_registry_.Sessions().begin(),
                                                 auth_provider_registry_.Sessions().end(),
                                                 [&](const AuthSession& session) {
                                                   return session.provider_id == provider.id;
                                                 }));
      auth_line += provider.label.empty() ? provider.id : provider.label;
      if (session_count > 0) {
        auth_line += " (" + std::to_string(session_count) + ")";
      }
    }
    lines.push_back(std::move(auth_line));
  }

  return lines;
}

float WorkspaceShell::GitSidebarListTop(const SDL_FRect& sidebar_rect) const {
  const float summary_height =
      static_cast<float>(GitSidebarSummaryLines().size()) * kGitSidebarSummaryLineHeight;
  return sidebar_rect.y + kGitSidebarActionRowTop + kGitSidebarActionButtonHeight +
         kGitSidebarListGap + summary_height +
         (summary_height > 0.0f ? kGitSidebarListGap * 0.5f : 0.0f);
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
  return ComputeScrollableListLayout(sidebar_rect, sidebar_rect.y + kSidebarHeaderHeight + 6.0f,
                                     line_count, context_.current_project_state.sidebar.scroll_row, kSidebarInset,
                                     kSidebarRowHeight, kSidebarRowHeight - 2.0f);
}

ScrollableListLayout WorkspaceShell::ComputeProblemsSidebarListLayout(
    const SDL_FRect& sidebar_rect,
    std::size_t line_count) const {
  return ComputeTreeSidebarListLayout(sidebar_rect, line_count);
}

ScrollableListLayout WorkspaceShell::ComputeTestsSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                   std::size_t line_count) const {
  return ComputeTreeSidebarListLayout(sidebar_rect, line_count);
}

ScrollableListLayout WorkspaceShell::ComputePluginSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                                    std::size_t line_count) const {
  return ComputeTreeSidebarListLayout(sidebar_rect, line_count);
}

SDL_FRect WorkspaceShell::TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const SDL_FRect refresh_rect = TreeSidebarRefreshButtonRect(sidebar_rect);
  const float button_width = std::max(76.0f, text_renderer_.MeasureWidth("Collapse") + 18.0f);
  return MakeRect(refresh_rect.x - 6.0f - button_width, sidebar_rect.y + 4.0f, button_width,
                  18.0f);
}

SDL_FRect WorkspaceShell::TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return MakeRect(0.0f, 0.0f, 0.0f, 0.0f);
  }

  const float button_width = std::max(72.0f, text_renderer_.MeasureWidth("Refresh") + 18.0f);
  return MakeRect(sidebar_rect.x + sidebar_rect.w - 10.0f - button_width, sidebar_rect.y + 4.0f,
                  button_width, 18.0f);
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
  const float width = std::clamp(text_renderer_.MeasureWidth(label) + 30.0f, 92.0f,
                                 std::max(92.0f, sidebar_rect.w - 20.0f));
  return MakeRect(sidebar_rect.x + 10.0f, sidebar_rect.y + 4.0f, width, 18.0f);
}

std::string WorkspaceShell::HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const {
  if (!last_mouse_position_valid_ || !context_.current_project_state.sidebar.visible ||
      ActiveSidebarMode() != SidebarMode::Git ||
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
      return entry.staged ? "Unstage" : "Stage";
    }
    if (actions.discard_rect.has_value() &&
        Contains(ExpandRect(*actions.discard_rect, kGitSidebarEntryButtonHoverPadding),
                 last_mouse_x_, last_mouse_y_)) {
      return "Discard";
    }
  }
  return {};
}

std::vector<WorkspaceShell::GitSidebarLine> WorkspaceShell::BuildGitSidebarLines() const {
  std::vector<GitSidebarSection> sections;
  sections.reserve(context_.current_project_state.sidebar.git.entries.size());
  for (const auto& entry : context_.current_project_state.sidebar.git.entries) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }

  const auto specs =
      BuildGitSidebarLineSpecs(sections, context_.current_project_state.sidebar.git.repo_available, context_.current_project_state.sidebar.git.base_ref, context_.current_project_state.sidebar.git.base_label);
  std::vector<GitSidebarLine> lines;
  lines.reserve(specs.size());
  for (const GitSidebarLineSpec& spec : specs) {
    lines.push_back(GitSidebarLine{
        .kind = spec.kind == GitSidebarLineKind::Header
                    ? GitSidebarLine::Kind::Header
                    : spec.kind == GitSidebarLineKind::Entry ? GitSidebarLine::Kind::Entry
                                                             : GitSidebarLine::Kind::Empty,
        .section = spec.section == GitSidebarSection::Modified
                       ? GitSidebarEntry::Section::Modified
                       : GitSidebarEntry::Section::Outgoing,
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
  if (entry.section != GitSidebarEntry::Section::Modified || row_rect.w <= 0.0f ||
      row_rect.h <= 0.0f) {
    return layout;
  }

  const auto button_rect = [&](float right_edge, std::string_view label) {
    const float width = std::max(22.0f, text_renderer_.MeasureWidth(label) + 12.0f);
    return MakeRect(right_edge - width, row_rect.y + 1.0f, width, row_rect.h - 2.0f);
  };

  const SDL_FRect primary_rect = button_rect(layout.content_right_edge, entry.staged ? "U" : "S");
  layout.primary_rect = primary_rect;
  layout.content_right_edge = primary_rect.x - kGitSidebarEntryButtonGap;

  const SDL_FRect discard_rect = button_rect(layout.content_right_edge, "D");
  layout.discard_rect = discard_rect;
  layout.content_right_edge = discard_rect.x - 6.0f;
  return layout;
}

std::optional<std::size_t> WorkspaceShell::SelectedGitSidebarLineIndex() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return std::nullopt;
  }

  std::vector<GitSidebarSection> sections;
  sections.reserve(context_.current_project_state.sidebar.git.entries.size());
  for (const auto& entry : context_.current_project_state.sidebar.git.entries) {
    sections.push_back(entry.section == GitSidebarEntry::Section::Modified
                           ? GitSidebarSection::Modified
                           : GitSidebarSection::Outgoing);
  }
  const auto specs =
      BuildGitSidebarLineSpecs(sections, context_.current_project_state.sidebar.git.repo_available, context_.current_project_state.sidebar.git.base_ref, context_.current_project_state.sidebar.git.base_label);
  return FindSelectedGitSidebarLineIndex(specs, context_.current_project_state.sidebar.git.selected_index);
}

const WorkspaceShell::GitSidebarEntry* WorkspaceShell::SelectedGitSidebarEntry() const {
  if (context_.current_project_state.sidebar.git.selected_index >= context_.current_project_state.sidebar.git.entries.size()) {
    return nullptr;
  }
  return &context_.current_project_state.sidebar.git.entries[context_.current_project_state.sidebar.git.selected_index];
}

}  // namespace microide::workspace
