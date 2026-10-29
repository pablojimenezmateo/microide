#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "platform/HostPlatform.h"
#include "platform/ProcessLauncher.h"
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

std::optional<TerminalLinkHit> TerminalPanelService::TerminalLinkAtPoint(float x, float y) const {
  return operations_.terminal_link_at_point(x, y);
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
  std::unique_ptr<TerminalPaneState> pane =
      operations_.make_started_pane(std::move(command), {}, {}, {}, {});
  (void)PushTerminalTab(std::move(pane), focus_terminal || panel_already_showing_terminal,
                        focus_terminal, was_visible);
}

bool TerminalPanelService::OpenTerminalOn(std::shared_ptr<const platform::ProcessLauncher> launcher,
                                          std::string label_prefix, std::string command) {
  const bool was_visible = PanelVisible();
  return PushTerminalTab(operations_.make_started_pane(std::move(command), std::move(launcher),
                                                       std::move(label_prefix), {}, {}),
                         /*show=*/true, /*focus=*/true, was_visible);
}

bool TerminalPanelService::OpenTerminalAttached(terminal::HostIdentity identity) {
  if (state_.root.empty()) {
    return false;
  }
  const bool was_visible = PanelVisible();
  return PushTerminalTab(operations_.make_started_pane({}, {}, {}, {}, std::move(identity)),
                         /*show=*/true, /*focus=*/true, was_visible);
}

bool TerminalPanelService::PushTerminalTab(std::unique_ptr<TerminalPaneState> pane, bool show, bool focus,
                                           bool was_visible) {
  if (pane == nullptr) {
    return false;
  }
  state_.terminal_tabs.push_back(MakeTerminalTab(std::move(pane)));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  if (show) {
    ShowTerminalContent(focus);
  }
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

void TerminalPanelService::OpenTerminalsForProjectInit() {
  TerminalLayoutRecord layout = std::move(state_.terminal_layout_to_revive);
  state_.terminal_layout_to_revive = {};
  if (!layout.empty() && (!operations_.revive_terminals || operations_.revive_terminals())) {
    for (TerminalTabRecord& record : layout.tabs) {
      std::unique_ptr<TerminalTabState> tab;
      for (TerminalPaneRecord& pane_record : record.panes) {
        // A directory that is gone starts at the project root, as a fresh one does.
        std::error_code error;
        if (!pane_record.cwd.empty() && !std::filesystem::is_directory(pane_record.cwd, error)) {
          pane_record.cwd.clear();
        }
        std::unique_ptr<TerminalPaneState> pane =
            operations_.make_started_pane(std::move(pane_record.command), {}, {},
                                          std::move(pane_record.cwd), std::move(pane_record.host));
        if (pane == nullptr) {
          continue;
        }
        if (tab == nullptr) {
          tab = MakeTerminalTab(std::move(pane));
        } else {
          (void)tab->InsertPane(tab->pane_count(), std::move(pane));  // capped by decode
        }
      }
      if (tab == nullptr) {
        continue;
      }
      // The saved shares only when they still describe these panes.
      if (record.weights.size() == tab->pane_count() &&
          std::all_of(record.weights.begin(), record.weights.end(),
                      [](float w) { return std::isfinite(w) && w > 0.0f; })) {
        tab->weights.clear();
        for (const float weight : record.weights) {
          tab->weights.push_back(std::max(weight, kTerminalPaneMinShare));
        }
        NormalizeTerminalPaneWeights(tab->weights);
      }
      tab->active_pane = std::min(record.active_pane, tab->pane_count() - 1);
      state_.terminal_tabs.push_back(std::move(tab));
    }
    if (!state_.terminal_tabs.empty()) {
      state_.active_terminal_tab_index = std::min(layout.active_tab, state_.terminal_tabs.size() - 1);
      ShowTerminalContent(/*focus=*/false);
      operations_.note_layout_inputs_changed();
      return;
    }
  }
  // The project's terminal is shown, but the keyboard starts in the editor, as in
  // VS Code: the first Ctrl+N or typed letter of a session is not the shell's
  // (TD-2026-10-16-348).
  // Test mode stands in for shells this machine starts only: a remote project's
  // terminal is a host terminal (on the test's fake host) in tests as well.
  if (!terminal::UsePlaceholderTerminalsForTesting() ||
      terminal::HostTerminalsFor(state_.launcher()) != nullptr) {
    OpenTerminal({}, /*focus_terminal=*/false, false);
    ShowTerminalContent(/*focus=*/false);
    return;
  }
  // Test mode: a bare, unstarted pane — no real shell spawn.
  state_.terminal_tabs.push_back(MakeTerminalTab(std::make_unique<TerminalPaneState>()));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  ShowTerminalContent(/*focus=*/false);
}

TerminalLayoutRecord TerminalPanelService::CaptureLayout(const ProjectWorkspaceState& state) {
  TerminalLayoutRecord layout;
  for (std::size_t t = 0; t < state.terminal_tabs.size(); ++t) {
    const TerminalTabState& tab = *state.terminal_tabs[t];
    TerminalTabRecord record;
    bool all_panes_kept = true;
    for (std::size_t p = 0; p < tab.pane_count(); ++p) {
      const TerminalPaneState& pane = *tab.panes[p];
      if (pane.launcher != nullptr) {
        all_panes_kept = false;
        continue;
      }
      if (p == tab.active_pane) {
        record.active_pane = record.panes.size();
      }
      record.panes.push_back(TerminalPaneRecord{.cwd = PaneWorkingDirectory(state, pane),
                                                .command = pane.launch_command,
                                                .host = pane.session.host_identity()});
    }
    if (record.panes.empty()) {
      continue;
    }
    // A tab that lost an own-launcher pane (the auth tab, a local terminal in a
    // remote project: neither is revived) keeps its panes, evenly: the shares were
    // the whole row's.
    if (all_panes_kept) {
      record.weights.assign(tab.weights.begin(), tab.weights.end());
    }
    if (t == state.active_terminal_tab_index) {
      layout.active_tab = layout.tabs.size();
    }
    layout.tabs.push_back(std::move(record));
  }
  return layout;
}

namespace {

// The first line of `text`: a multi-line invocation's transcript header.
std::string FirstLine(std::string_view text) {
  return std::string(text.substr(0, text.find('\n')));
}


template <typename Visit>
void ForEachProjectHostPane(ProjectWorkspaceState& state, Visit&& visit) {
  for (const std::unique_ptr<TerminalTabState>& tab : state.terminal_tabs) {
    for (const std::unique_ptr<TerminalPaneState>& pane : tab->panes) {
      // A pane with its own launcher (the ssh tab, a local terminal) is not the
      // project's host's.
      if (pane != nullptr && pane->launcher == nullptr) {
        visit(*pane);
      }
    }
  }
}

}  // namespace

void TerminalPanelService::ReleaseHostTerminals(ProjectWorkspaceState& state) const {
  const bool keep = !operations_.revive_terminals || operations_.revive_terminals();
  ForEachProjectHostPane(state, [keep](TerminalPaneState& pane) {
    if (!pane.session.is_host_terminal()) {
      return;  // local, pending (nothing on the host yet), or already let go
    }
    if (!keep || pane.session.host_shell_exited()) {
      pane.session.Stop();
    } else {
      pane.session.DetachFromHost();
    }
  });
}

std::size_t TerminalPanelService::StartPendingHostPanes(ProjectWorkspaceState& state) {
  std::size_t started = 0;
  const platform::ProcessLauncher& launcher = state.launcher();
  ForEachProjectHostPane(state, [&](TerminalPaneState& pane) {
    if (pane.session.host_pending() && pane.session.StartPendingOnHost(launcher)) {
      ++started;
      pane.reattach_unreported = true;  // its outcome is news for the connect's summary
    }
  });
  if (started != 0) {
    state.host_terminals_unrecorded = true;
  }
  return started;
}

void TerminalPanelService::SetPendingHostStatus(ProjectWorkspaceState& state, std::string_view line) {
  ForEachProjectHostPane(state, [line](TerminalPaneState& pane) { pane.session.SetPendingStatus(line); });
}

std::filesystem::path TerminalPanelService::PaneWorkingDirectory(const ProjectWorkspaceState& state,
                                                                const TerminalPaneState& pane) {
  // OSC 7 when the shell sends it; else, for a shell this machine runs, its
  // process's own cwd (bash sends no OSC 7 unless configured to).
  const platform::ProcessLauncher& launcher =
      pane.launcher != nullptr ? *pane.launcher : state.launcher();
  std::filesystem::path cwd = pane.session.reported_working_directory();
  if (cwd.empty() && launcher.is_local()) {
    cwd = platform::ProcessWorkingDirectory(pane.session.child_process_id());
  }
  // A host shell reports a host path; the editor's tree is the mirror.
  if (!cwd.empty() && !launcher.is_local()) {
    cwd = launcher.LocalPathFromHost(std::move(cwd));
  }
  return cwd.empty() ? pane.launch_working_directory : cwd;
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
    } else {
      // The panel stays up showing an output channel, and its tab strip still
      // lists the terminal that just closed.
      operations_.request_bottom_panel_redraw();
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

bool TerminalPanelService::FindHostTerminal(const terminal::HostIdentity& identity, bool focus) {
  for (std::size_t t = 0; t < state_.terminal_tabs.size(); ++t) {
    TerminalTabState& tab = *state_.terminal_tabs[t];
    for (std::size_t p = 0; p < tab.panes.size(); ++p) {
      const TerminalPaneState* pane = tab.panes[p].get();
      // A pane with its own launcher (a local terminal in a remote project) is
      // not a host terminal, whatever it once held.
      if (pane == nullptr || pane->launcher != nullptr || pane->session.host_identity() != identity) {
        continue;
      }
      if (focus) {
        tab.active_pane = p;
        (void)ActivateTerminalTab(t);
      }
      return true;
    }
  }
  return false;
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
  // The new pane starts where the one it splits is now (VS Code's default
  // `terminal.integrated.splitCwd: inherited`). A host terminal's split starts
  // where host terminals start: its directory is the host's, not the tree's.
  std::unique_ptr<TerminalPaneState> pane = operations_.make_started_pane(
      {}, active != nullptr ? active->launcher : nullptr,
      active != nullptr ? active->label_prefix : std::string(),
      active != nullptr && active->launcher == nullptr ? PaneWorkingDirectory(*active)
                                                       : std::filesystem::path(),
      {});
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
  // A pending pane is not an exited one: it starts by itself once connected, and
  // relaunching it would forget the host terminal it is waiting to reattach to.
  if (pane == nullptr || pane->session.running() || pane->session.host_pending() ||
      !operations_.relaunch_pane) {
    return false;
  }
  if (!operations_.relaunch_pane(*pane)) {
    return false;
  }
  operations_.clear_terminal_selection();
  pane->follow_tail = true;
  pane->scroll_row = 0;
  pane->observed_scrollback_trim_total = 0;
  pane->observed_row_shift_sequence = 0;
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

bool TerminalPanelService::RevealTerminal() {
  if (state_.terminal_tabs.empty()) {
    return false;
  }
  const bool was_visible = PanelVisible();
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
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
  const auto restore = [this]() {
    state_.panel.maximized = false;
    operations_.note_layout_inputs_changed();
    operations_.request_window_redraw();
  };
  switch (state_.surface.focus) {
    case FocusTarget::Editor:
    case FocusTarget::Sidebar:
    case FocusTarget::DebugPane:
      restore();
      break;
    case FocusTarget::Overlay:
      // The find widget, completion and code actions are the editor's: with the
      // editor collapsed under a maximized panel they were drawn over the panel's
      // tab strip and transcript, aimed at a buffer nobody could see (the QA
      // monkey's stale terminal highlights, TD-2026-10-22-379, sat under one).
      // Like focusing the editor, they bring it back. A quick-open modal is
      // centered over whatever is there and leaves the panel as it is.
      if (state_.overlay.visible && !OverlayIsQuickOpen(state_.overlay.mode)) {
        restore();
      }
      break;
    case FocusTarget::Panel:
      break;
  }
}

void TerminalPanelService::NoteActiveTabShown() {
  if (state_.panel.content != PanelContentKind::Terminal) {
    return;
  }
  if (TerminalTabState* tab = state_.active_terminal_tab();
      tab != nullptr && (tab->has_unseen_output || tab->has_unseen_bell)) {
    tab->has_unseen_output = false;
    tab->has_unseen_bell = false;
    operations_.request_bottom_panel_redraw();
  }
}

std::optional<TerminalSelectionBounds> TerminalPanelService::ActiveSelectionBounds(const ProjectWorkspaceState& state) {
  const auto* terminal_tab = state.active_terminal_pane();
  if (terminal_tab == nullptr || !terminal_tab->selection_anchor.has_value() ||
      !terminal_tab->selection_head.has_value()) {
    return std::nullopt;
  }

  return NormalizeTerminalSelection(terminal_tab->selection_anchor, terminal_tab->selection_head);
}

bool TerminalPanelService::HasSelection(const ProjectWorkspaceState& state) {
  const auto selection = ActiveSelectionBounds(state);
  return selection.has_value() &&
         (selection->start.row != selection->end.row ||
          selection->start.column != selection->end.column);
}

std::string TerminalPanelService::SelectedText(const ProjectWorkspaceState& state) {
  const auto selection = ActiveSelectionBounds(state);
  if (!selection.has_value()) {
    return {};
  }

  const auto* terminal_tab = state.active_terminal_pane();
  if (terminal_tab == nullptr) {
    return {};
  }

  const std::size_t first_row = selection->start.row;
  const auto lines = terminal_tab->session.SnapshotLineRange(
      first_row, selection->end.row - first_row + 1);
  if (lines.empty()) {
    return {};
  }

  TerminalSelectionBounds rebased = *selection;
  rebased.start.row -= first_row;
  rebased.end.row -= first_row;
  return ExtractTerminalSelectionText(lines, rebased);
}

bool TerminalPanelService::HasLastCommand(const ProjectWorkspaceState& state) {
  // Cheap predicate for action enablement: the exact precondition under which
  // LastCommandText() is guaranteed to return a value. Menu/context enablement
  // must not snapshot + join the whole scrollback transcript just to answer "is Copy Last
  // Command available?" (TD-2026-07-17A-065).
  const auto* terminal_tab = state.active_terminal_pane();
  return terminal_tab != nullptr && terminal_tab->has_last_command &&
         !terminal_tab->last_command_invocation.empty();
}

std::optional<std::string> TerminalPanelService::LastCommandText(const ProjectWorkspaceState& state) {
  if (!HasLastCommand(state)) {
    return std::nullopt;
  }
  const auto* terminal_tab = state.active_terminal_pane();

  if (terminal_tab->session.using_alternate_screen()) {
    return terminal_tab->last_command_invocation;
  }

  const std::size_t line_count = terminal_tab->session.LineCount();
  if (terminal_tab->last_command_start_row >= line_count) {
    return terminal_tab->last_command_invocation;
  }
  // Cap the snapshot to a bounded window of the most-recent-command output. A
  // long-running command can retain output up to the full scrollback cap, so copying
  // every retained row (and then joining a second full transcript) would let the invoke
  // path allocate unboundedly on the UI thread (TD-2026-07-17A-037). Keep the head of the
  // output and mark the tail truncated.
  const std::size_t available = line_count - terminal_tab->last_command_start_row;
  const std::size_t capped_lines = std::min(available, kDefaultLastTerminalCommandMaxLines);
  const bool source_truncated = available > capped_lines;
  const auto lines =
      terminal_tab->session.SnapshotLineRange(terminal_tab->last_command_start_row, capped_lines);
  if (lines.empty()) {
    return terminal_tab->last_command_invocation;
  }

  std::vector<std::string> rows;
  rows.reserve(lines.size());
  for (const auto& line : lines) {
    rows.push_back(TerminalLineText(line));
  }

  std::string transcript = BuildLastTerminalCommandTranscript(
      rows, TrimTrailingTerminalBlanks(terminal_tab->last_command_prompt_prefix),
      FirstLine(terminal_tab->last_command_invocation), source_truncated);

  return transcript.empty() ? std::optional<std::string>(terminal_tab->last_command_invocation)
                            : std::optional<std::string>(std::move(transcript));
}

}  // namespace microide::workspace
