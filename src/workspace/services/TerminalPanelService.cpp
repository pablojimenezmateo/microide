#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <utility>

#include "terminal/TerminalSession.h"
#include "workspace/TabReorder.h"

namespace microide::workspace {

TerminalPanelService::TerminalPanelService(ProjectWorkspaceState& state, Operations operations)
    : state_(state), operations_(std::move(operations)) {}

std::optional<std::string> TerminalPanelService::ReadPrimarySelectionText() const {
  return operations_.read_primary_selection_text();
}

void TerminalPanelService::ClearTerminalSelection() {
  operations_.clear_terminal_selection();
}

void TerminalPanelService::AppendTerminalPendingInput(std::string_view input) {
  operations_.append_terminal_pending_input(input);
}

std::optional<std::string> TerminalPanelService::TerminalUrlAtPoint(float x, float y) const {
  return operations_.terminal_url_at_point(x, y);
}

bool TerminalPanelService::OpenExternalUrl(std::string_view url) const {
  return operations_.open_external_url(url);
}

void TerminalPanelService::SyncPrimarySelectionWithTerminalSelection() {
  operations_.sync_primary_selection_with_terminal_selection();
}

bool TerminalPanelService::PanelShowsTerminal() const {
  return state_.panel.content == PanelContentKind::Terminal && state_.active_terminal_pane() != nullptr;
}

void TerminalPanelService::NotePanelVisibilityChanged(bool was_visible) {
  if (PanelVisible() != was_visible) {
    operations_.note_layout_inputs_changed();
    operations_.request_window_redraw();
  } else if (PanelVisible()) {
    operations_.request_bottom_panel_redraw();
  }
}

void TerminalPanelService::ShowTerminalContent(bool focus) {
  state_.panel.content = PanelContentKind::Terminal;
  if (focus) {
    state_.surface.focus = FocusTarget::Panel;
  }
}

void TerminalPanelService::HidePanel() {
  if (state_.panel.content != PanelContentKind::None) {
    state_.panel.restore_content = state_.panel.content;
  }
  state_.panel.content = PanelContentKind::None;
  if (state_.surface.focus == FocusTarget::Panel) {
    state_.surface.focus = FocusTarget::Editor;
  }
}

// --- strip tabs ---------------------------------------------------------------

void TerminalPanelService::OpenTerminal(std::string command, bool focus_terminal, bool log_feedback) {
  (void) log_feedback;
  if (state_.root.empty()) {
    return;
  }
  const bool was_visible = PanelVisible();
  const bool panel_already_showing_terminal = state_.panel.content == PanelContentKind::Terminal;
  std::unique_ptr<TerminalPaneState> pane = operations_.make_started_pane(std::move(command), {}, {});
  if (pane == nullptr) {
    return;
  }
  state_.terminal_tabs.push_back(MakeTerminalTab(std::move(pane)));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  if (focus_terminal || panel_already_showing_terminal) {
    ShowTerminalContent(focus_terminal);
  }
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
}

bool TerminalPanelService::OpenTerminalOn(std::shared_ptr<const platform::ProcessLauncher> launcher,
                                          std::string label_prefix, std::string command) {
  const bool was_visible = PanelVisible();
  std::unique_ptr<TerminalPaneState> pane = operations_.make_started_pane(
      std::move(command), std::move(launcher), std::move(label_prefix));
  if (pane == nullptr) {
    return false;
  }
  state_.terminal_tabs.push_back(MakeTerminalTab(std::move(pane)));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

void TerminalPanelService::OpenDefaultTerminalForProjectInit() {
  if (!terminal::UsePlaceholderTerminalsForTesting()) {
    OpenTerminal({}, true, false);
    return;
  }
  // Test mode: a bare, unstarted pane — no real shell spawn.
  state_.terminal_tabs.push_back(MakeTerminalTab(std::make_unique<TerminalPaneState>()));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  ShowTerminalContent(true);
}

void TerminalPanelService::CloseTerminalTab(std::size_t index) {
  if (index >= state_.terminal_tabs.size()) {
    return;
  }
  const bool was_visible = PanelVisible();
  state_.terminal_tabs.erase(state_.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (state_.terminal_tabs.empty()) {
    state_.active_terminal_tab_index = 0;
    operations_.clear_terminal_selection();
    if (state_.panel.content == PanelContentKind::Terminal) {
      state_.panel.content = PanelContentKind::None;
      state_.panel.restore_content = PanelContentKind::Terminal;
    }
    if (state_.surface.focus == FocusTarget::Panel) {
      state_.surface.focus = FocusTarget::Editor;
    }
    if (PanelVisible() != was_visible) {
      operations_.note_layout_inputs_changed();
      operations_.request_window_redraw();
    }
    return;
  }
  state_.active_terminal_tab_index =
      std::min(state_.active_terminal_tab_index > index ? state_.active_terminal_tab_index - 1
                                                        : state_.active_terminal_tab_index,
               state_.terminal_tabs.size() - 1);
  // A different tab is on screen now, and its panes may need another grid.
  operations_.note_layout_inputs_changed();
  operations_.request_bottom_panel_redraw();
}

bool TerminalPanelService::MoveActiveTerminalTabTo(std::size_t index) {
  if (!ReorderActive(state_.terminal_tabs, state_.active_terminal_tab_index, index)) {
    return false;
  }
  state_.surface.focus = FocusTarget::Panel;
  return true;
}

bool TerminalPanelService::ActivateTerminalTab(std::size_t index) {
  if (index >= state_.terminal_tabs.size()) {
    return false;
  }
  const bool was_visible = PanelVisible();
  state_.active_terminal_tab_index = index;
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

bool TerminalPanelService::CycleTerminalTab(int delta) {
  const std::size_t count = state_.terminal_tabs.size();
  if (count < 2) {
    return false;
  }
  const std::size_t current = std::min(state_.active_terminal_tab_index, count - 1);
  const auto wrapped = static_cast<std::size_t>(
      ((static_cast<long long>(current) + delta) % static_cast<long long>(count) +
       static_cast<long long>(count)) %
      static_cast<long long>(count));
  return ActivateTerminalTab(wrapped);
}

// --- panes --------------------------------------------------------------------

bool TerminalPanelService::SplitActivePane() {
  TerminalTabState* tab = state_.active_terminal_tab();
  const TerminalPaneState* active = tab != nullptr ? tab->active() : nullptr;
  // A split of a host terminal is another terminal on that host.
  if (tab == nullptr || tab->full() || (state_.root.empty() && (active == nullptr || !active->launcher))) {
    return false;
  }
  std::unique_ptr<TerminalPaneState> pane = operations_.make_started_pane(
      {}, active != nullptr ? active->launcher : nullptr,
      active != nullptr ? active->label_prefix : std::string());
  if (pane == nullptr) {
    return false;
  }
  const bool was_visible = PanelVisible();
  // VS Code puts the new pane after the active one.
  if (!tab->InsertPane(tab->active_pane + 1, std::move(pane))) {
    return false;
  }
  operations_.clear_terminal_selection();
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

bool TerminalPanelService::CloseActivePane() {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || tab->active() == nullptr) {
    return false;
  }
  if (tab->pane_count() <= 1) {
    CloseTerminalTab(state_.active_terminal_tab_index);
    return true;
  }
  operations_.clear_terminal_selection();
  tab->RemovePane(tab->active_pane);
  operations_.note_layout_inputs_changed();
  operations_.request_bottom_panel_redraw();
  return true;
}

bool TerminalPanelService::FocusPaneInDirection(int delta) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || tab->pane_count() < 2) {
    return false;
  }
  const long long target = static_cast<long long>(tab->active_pane) + delta;
  if (target < 0 || target >= static_cast<long long>(tab->pane_count())) {
    return false;
  }
  operations_.clear_terminal_selection();
  tab->active_pane = static_cast<std::size_t>(target);
  state_.surface.focus = FocusTarget::Panel;
  operations_.request_bottom_panel_redraw();
  return true;
}

TerminalPaneState* TerminalPanelService::PaneAt(const WorkspaceLayout& layout, float x, float y) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !PanelShowsTerminal()) {
    return nullptr;
  }
  const TerminalPaneRectsLayout rects = PaneRects(layout);
  const std::size_t index = TerminalPaneIndexAt(rects, x, y);
  return index < tab->pane_count() ? tab->panes[index].get() : nullptr;
}

bool TerminalPanelService::ActivatePaneAt(const WorkspaceLayout& layout, float x, float y) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !PanelShowsTerminal()) {
    return false;
  }
  const std::size_t index = TerminalPaneIndexAt(PaneRects(layout), x, y);
  if (index >= tab->pane_count() || index == tab->active_pane) {
    return false;
  }
  operations_.clear_terminal_selection();
  tab->active_pane = index;
  operations_.request_bottom_panel_redraw();
  return true;
}

bool TerminalPanelService::RelaunchActivePane() {
  TerminalPaneState* pane = state_.active_terminal_pane();
  if (pane == nullptr || pane->session.running() || !operations_.relaunch_pane) {
    return false;
  }
  if (!operations_.relaunch_pane(*pane)) {
    return false;
  }
  operations_.clear_terminal_selection();
  pane->follow_tail = true;
  pane->scroll_row = 0;
  pane->observed_scrollback_trim_total = 0;
  pane->has_last_command = false;
  pane->pending_input.clear();
  pane->pending_input_truncated = false;
  // A fresh session starts at a 0x0 grid until the next frame regrids it.
  if (TerminalTabState* tab = state_.active_terminal_tab(); tab != nullptr) {
    ++tab->layout_revision;
  }
  operations_.request_bottom_panel_redraw();
  return true;
}

// --- geometry -----------------------------------------------------------------

TerminalPaneRectsLayout TerminalPanelService::PaneRects(const WorkspaceLayout& layout) const {
  const TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || state_.panel.content != PanelContentKind::Terminal) {
    return {};
  }
  return tab->PaneRects(BottomPanelContentRect(layout));
}

SDL_FRect TerminalPanelService::ActivePaneBodyRect(const WorkspaceLayout& layout) const {
  const TerminalTabState* tab = state_.active_terminal_tab();
  const SDL_FRect body = BottomPanelContentRect(layout);
  return tab != nullptr ? tab->ActivePaneRect(body) : body;
}

std::optional<TerminalPaneDividerRect> TerminalPanelService::DividerAt(const WorkspaceLayout& layout,
                                                                       float x,
                                                                       float y) const {
  for (const TerminalPaneDividerRect& divider : PaneRects(layout).dividers) {
    const SDL_FRect& r = divider.rect;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      return divider;
    }
  }
  return std::nullopt;
}

bool TerminalPanelService::ResizeActiveTabDivider(std::size_t boundary, float first_share) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !tab->ResizeDivider(boundary, first_share)) {
    return false;
  }
  operations_.request_bottom_panel_redraw();
  return true;
}

bool TerminalPanelService::ResetActiveTabDivider(std::size_t boundary) {
  return ResizeActiveTabDivider(boundary, 0.5f);
}

// --- panel --------------------------------------------------------------------

void TerminalPanelService::TogglePanel() {
  const bool was_visible = PanelVisible();
  if (was_visible) {
    HidePanel();
  } else {
    PanelContentKind content = state_.panel.restore_content;
    if (content == PanelContentKind::None) {
      content = PanelContentKind::Terminal;
    }
    if (content == PanelContentKind::Terminal) {
      if (state_.terminal_tabs.empty()) {
        OpenTerminal({}, true, false);
        return;
      }
      ShowTerminalContent(true);
    } else {
      state_.panel.content = content;
      state_.surface.focus = FocusTarget::Panel;
    }
  }
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
}

void TerminalPanelService::ToggleTerminal() {
  const bool was_visible = PanelVisible();
  if (PanelShowsTerminal() && state_.surface.focus == FocusTarget::Panel) {
    HidePanel();
    operations_.note_layout_inputs_changed();
    NotePanelVisibilityChanged(was_visible);
    return;
  }
  if (state_.terminal_tabs.empty()) {
    OpenTerminal({}, true, false);
    return;
  }
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
}

void TerminalPanelService::SetPanelMaximized(bool maximized) {
  const bool was_visible = PanelVisible();
  if (maximized && !PanelVisible()) {
    // Maximizing a hidden panel means "show me the terminal, large".
    if (state_.terminal_tabs.empty()) {
      state_.panel.maximized = true;
      OpenTerminal({}, true, false);
      return;
    }
    ShowTerminalContent(true);
  }
  if (maximized) {
    state_.surface.focus = FocusTarget::Panel;
  }
  state_.panel.maximized = maximized;
  operations_.note_layout_inputs_changed();
  if (PanelVisible() != was_visible || PanelVisible()) {
    operations_.request_window_redraw();
  }
}

void TerminalPanelService::TogglePanelMaximized() {
  SetPanelMaximized(!state_.panel.maximized);
}

void TerminalPanelService::SyncPanelMaximizedWithFocus() {
  if (!state_.panel.maximized || !PanelVisible()) {
    return;
  }
  switch (state_.surface.focus) {
    case FocusTarget::Editor:
    case FocusTarget::Sidebar:
    case FocusTarget::DebugPane:
      state_.panel.maximized = false;
      operations_.note_layout_inputs_changed();
      operations_.request_window_redraw();
      break;
    case FocusTarget::Panel:
    case FocusTarget::Overlay:
      break;
  }
}

void TerminalPanelService::NoteActiveTabShown() {
  if (state_.panel.content != PanelContentKind::Terminal) {
    return;
  }
  if (TerminalTabState* tab = state_.active_terminal_tab(); tab != nullptr && tab->has_unseen_output) {
    tab->has_unseen_output = false;
    operations_.request_bottom_panel_redraw();
  }
}

}  // namespace microide::workspace
