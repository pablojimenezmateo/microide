#include "workspace/services/TerminalPanelService.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "platform/HostPlatform.h"
#include "platform/ProcessLauncher.h"
#include "terminal/TerminalSession.h"
#include "workspace/TabReorder.h"
#include "workspace/WorkspaceFileDrop.h"

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

std::string TerminalPanelService::DroppedPathText(const TerminalPaneState* pane,
                                                  const std::filesystem::path& local_path) const {
  const platform::ProcessLauncher& launcher =
      pane != nullptr && pane->launcher != nullptr ? *pane->launcher : state_.launcher();
  return TerminalDropText(launcher.ResolveWorkingDirectory(local_path));
}

bool TerminalPanelService::OpenUrlForPane(const TerminalPaneState* pane, std::string_view url) const {
  const platform::ProcessLauncher& launcher =
      pane != nullptr && pane->launcher != nullptr ? *pane->launcher : state_.launcher();
  if (operations_.open_host_local_url && operations_.open_host_local_url(launcher, url)) {
    return true;
  }
  return OpenExternalUrl(url);
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

bool TerminalPanelService::AdoptHostTerminal(ProjectWorkspaceState& state, terminal::HostIdentity identity) {
  if (state.root.empty() || identity.empty() || !operations_.make_host_pane) {
    return false;
  }
  std::unique_ptr<TerminalPaneState> pane = operations_.make_host_pane(state, std::move(identity));
  if (pane == nullptr) {
    return false;
  }
  const bool first = state.terminal_tabs.empty();
  state.terminal_tabs.push_back(MakeTerminalTab(std::move(pane)));
  if (first) {
    state.active_terminal_tab_index = 0;
  }
  // The session the next save writes names it, as it names every started pane.
  state.host_terminals_unrecorded = true;
  if (&state == &state_) {
    operations_.note_layout_inputs_changed();
    operations_.request_bottom_panel_redraw();
  }
  return true;
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
  // The project's terminal is shown, but the keyboard starts in the editor, as in
  // VS Code: the first Ctrl+N or typed letter of a session is not the shell's
  // (TD-2026-10-16-348). Unless the panel comes back MAXIMIZED: then there is no
  // editor on screen to hold the keyboard, and leaving it there made the first
  // frame's SyncPanelMaximizedWithFocus read "the editor took focus" and restore
  // the panel the session had just revived maximized.
  const bool focus_panel = state_.panel.maximized;
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
          // Seated as a row for now; the saved arrangement replaces it below.
          (void)tab->SplitPane(tab->pane_count() - 1, SplitOrientation::Vertical, false,
                               std::move(pane));  // capped by decode
        }
      }
      if (tab == nullptr) {
        continue;
      }
      // The saved arrangement only when it still describes these panes: a pane
      // that failed to start leaves the row the loop above built.
      if (SplitTree saved; !record.split.empty() && saved.Load(record.split) &&
                           saved.leaf_count() == tab->pane_count()) {
        tab->split = saved;
        ++tab->layout_revision;
      }
      tab->active_pane = std::min(record.active_pane, tab->pane_count() - 1);
      state_.terminal_tabs.push_back(std::move(tab));
    }
    if (!state_.terminal_tabs.empty()) {
      state_.active_terminal_tab_index = std::min(layout.active_tab, state_.terminal_tabs.size() - 1);
      ShowTerminalContent(focus_panel);
      operations_.note_layout_inputs_changed();
      return;
    }
  }
  // Test mode stands in for shells this machine starts only: a remote project's
  // terminal is a host terminal (on the test's fake host) in tests as well.
  if (!terminal::UsePlaceholderTerminalsForTesting() ||
      terminal::HostTerminalsFor(state_.launcher()) != nullptr) {
    OpenTerminal({}, focus_panel, false);
    ShowTerminalContent(focus_panel);
    return;
  }
  // Test mode: a bare, unstarted pane — no real shell spawn.
  state_.terminal_tabs.push_back(MakeTerminalTab(std::make_unique<TerminalPaneState>()));
  state_.active_terminal_tab_index = state_.terminal_tabs.size() - 1;
  ShowTerminalContent(focus_panel);
}

TerminalLayoutRecord TerminalPanelService::CaptureLayout(const ProjectWorkspaceState& state) {
  TerminalLayoutRecord layout;
  for (std::size_t t = 0; t < state.terminal_tabs.size(); ++t) {
    const TerminalTabState& tab = *state.terminal_tabs[t];
    TerminalTabRecord record;
    // Panes that run through their own launcher (the ssh authentication tab, a
    // local terminal in a remote project) are not revived; the tree loses their
    // leaves the way closing them would, so the rest keep their arrangement.
    SplitTree kept = tab.split;
    for (std::size_t p = tab.pane_count(); p > 0; --p) {
      const TerminalPaneState& pane = *tab.panes[p - 1];
      if (pane.launcher != nullptr && kept.leaf_count() > 1) {
        kept.RemoveLeaf(p - 1);
      }
    }
    for (std::size_t p = 0; p < tab.pane_count(); ++p) {
      const TerminalPaneState& pane = *tab.panes[p];
      if (pane.launcher != nullptr) {
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
    if (kept.leaf_count() == record.panes.size()) {
      record.split = kept.Flatten();
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

bool TerminalPanelService::HasUnseenTerminalActivity(const ProjectWorkspaceState& state) {
  return std::any_of(state.terminal_tabs.begin(), state.terminal_tabs.end(), [](const auto& tab) {
    return tab != nullptr && (tab->has_unseen_output || tab->has_unseen_bell);
  });
}

bool TerminalPanelService::HasHostOpenInFlight(const ProjectWorkspaceState& state) {
  for (const std::unique_ptr<TerminalTabState>& tab : state.terminal_tabs) {
    for (const std::unique_ptr<TerminalPaneState>& pane : tab->panes) {
      if (pane != nullptr && pane->launcher == nullptr && pane->session.is_host_terminal() &&
          pane->session.running() && pane->session.host_identity().empty()) {
        return true;
      }
    }
  }
  return false;
}

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

bool TerminalPanelService::CanJoinTabs(std::size_t source, std::size_t target) const {
  const auto& tabs = state_.terminal_tabs;
  return source != target && source < tabs.size() && target < tabs.size() && tabs[source] != nullptr &&
         tabs[target] != nullptr && !tabs[source]->panes.empty() && !tabs[target]->panes.empty() &&
         tabs[source]->pane_count() + tabs[target]->pane_count() <= kMaxTerminalPanes;
}

bool TerminalPanelService::JoinTabIntoPane(std::size_t source,
                                           std::size_t target,
                                           std::size_t target_pane,
                                           PaneDropZone zone) {
  const PaneDropSplit split = SplitForPaneDropZone(zone);
  if (!split.edge || !CanJoinTabs(source, target) || target_pane >= state_.terminal_tabs[target]->pane_count()) {
    return false;
  }
  TerminalTabState& into = *state_.terminal_tabs[target];
  if (!into.GraftTab(target_pane, split.orientation, split.before, *state_.terminal_tabs[source])) {
    return false;
  }
  const bool was_visible = PanelVisible();
  // The emptied source tab goes; the target's index shifts down past it.
  state_.terminal_tabs.erase(state_.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(source));
  state_.active_terminal_tab_index = target > source ? target - 1 : target;
  // The selection named rows of a pane that may have moved; panes keep their own
  // sessions, so nothing else needs to follow.
  operations_.clear_terminal_selection();
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
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

bool TerminalPanelService::SplitActivePane(SplitOrientation orientation) {
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
  // VS Code puts the new pane after (right of, or below) the active one.
  if (!tab->SplitPane(tab->active_pane, orientation, /*before=*/false, std::move(pane))) {
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

bool TerminalPanelService::UnsplitActivePane() {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || tab->pane_count() < 2) {
    return false;
  }
  std::unique_ptr<TerminalPaneState> pane = tab->RemovePane(tab->active_pane);
  if (pane == nullptr) {
    return false;
  }
  operations_.clear_terminal_selection();
  const bool was_visible = PanelVisible();
  const std::size_t at = state_.active_terminal_tab_index + 1;
  state_.terminal_tabs.insert(state_.terminal_tabs.begin() + static_cast<std::ptrdiff_t>(at),
                              MakeTerminalTab(std::move(pane)));
  state_.active_terminal_tab_index = at;
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

bool TerminalPanelService::FocusPaneInDirection(PaneDirection direction) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || tab->pane_count() < 2) {
    return false;
  }
  // By the arrangement on screen, not by index: in a grid the next index can sit
  // below. Adjacency depends only on the tree's proportions, so it is answered
  // over a fixed square rather than the panel body -- which a hidden panel (a
  // verb asked for by name shows it below) does not have.
  constexpr SDL_FRect kShapeOnly{0.0f, 0.0f, 4096.0f, 4096.0f};
  const SplitRegionsLayout rects = tab->PaneRects(kShapeOnly);
  const std::size_t target = AdjacentSplitRegion(
      std::span<const SDL_FRect>(rects.panes.data(), rects.panes.size()), tab->active_pane, direction);
  if (target == kNoSplitRegion || target >= tab->pane_count()) {
    return false;
  }
  operations_.clear_terminal_selection();
  tab->active_pane = target;
  // Shown, as switching terminal tabs shows it: setting the focus alone left the
  // keyboard on a hidden panel when it was asked for by name (soak c0c5719bdbce).
  const bool was_visible = PanelVisible();
  ShowTerminalContent(true);
  operations_.note_layout_inputs_changed();
  NotePanelVisibilityChanged(was_visible);
  return true;
}

TerminalPaneState* TerminalPanelService::PaneAt(const WorkspaceLayout& layout, float x, float y) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !PanelShowsTerminal()) {
    return nullptr;
  }
  const SplitRegionsLayout rects = PaneRects(layout);
  const std::size_t index = SplitRegionIndexAt(rects, x, y);
  return index < tab->pane_count() ? tab->panes[index].get() : nullptr;
}

bool TerminalPanelService::ActivatePaneAt(const WorkspaceLayout& layout, float x, float y) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !PanelShowsTerminal()) {
    return false;
  }
  const std::size_t index = SplitRegionIndexAt(PaneRects(layout), x, y);
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

SplitRegionsLayout TerminalPanelService::PaneRects(const WorkspaceLayout& layout) const {
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

std::optional<SplitDividerRect> TerminalPanelService::DividerAt(const WorkspaceLayout& layout,
                                                                       float x,
                                                                       float y) const {
  for (const SplitDividerRect& divider : PaneRects(layout).dividers) {
    const SDL_FRect& r = divider.rect;
    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
      return divider;
    }
  }
  return std::nullopt;
}

bool TerminalPanelService::ResizeActiveTabDivider(std::uint8_t node, std::size_t boundary, float first_share) {
  TerminalTabState* tab = state_.active_terminal_tab();
  if (tab == nullptr || !tab->ResizeDivider(node, boundary, first_share)) {
    return false;
  }
  operations_.request_bottom_panel_redraw();
  return true;
}

bool TerminalPanelService::ResetActiveTabDivider(std::uint8_t node, std::size_t boundary) {
  return ResizeActiveTabDivider(node, boundary, 0.5f);
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
  if (maximized != state_.panel.maximized) {
    state_.panel.maximized_seen_focus.reset();  // a new maximize: the next frame sets the baseline
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
    state_.panel.maximized_seen_focus.reset();
    return;
  }
  const FocusTarget focus = state_.surface.focus;
  const bool hidden_surface =
      focus == FocusTarget::Editor || focus == FocusTarget::Sidebar || focus == FocusTarget::DebugPane;
  const bool editor_overlay = focus == FocusTarget::Overlay && state_.overlay.visible &&
                              !OverlayIsQuickOpen(state_.overlay.mode);
  // Edge-triggered (TD-2026-11-01-427 item 3): the first frame that sees the
  // panel maximized takes the focus as it found it -- a session restore, a
  // project switch back, a tab hydrated on load all arrive with the keyboard
  // wherever their state put it, and reading that as "the editor took focus"
  // restored the panel the user had left maximized. A keyboard on a surface the
  // maximized panel hides goes to the panel instead: the one thing on screen.
  if (!state_.panel.maximized_seen_focus.has_value()) {
    if (hidden_surface) {
      state_.surface.focus = FocusTarget::Panel;
      operations_.request_window_redraw();
    }
    state_.panel.maximized_seen_focus = state_.surface.focus;
    state_.panel.maximized_seen_editor_overlay = editor_overlay;
    return;
  }
  const bool moved = focus != *state_.panel.maximized_seen_focus ||
                     editor_overlay != state_.panel.maximized_seen_editor_overlay;
  state_.panel.maximized_seen_focus = focus;
  state_.panel.maximized_seen_editor_overlay = editor_overlay;
  if (!moved) {
    return;
  }
  // Then a MOVE of the keyboard to another surface brings it back.
  const auto restore = [this]() {
    state_.panel.maximized = false;
    state_.panel.maximized_seen_focus.reset();
    operations_.note_layout_inputs_changed();
    operations_.request_window_redraw();
  };
  switch (focus) {
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
      if (editor_overlay) {
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
  const auto* terminal_tab = state.active_terminal_pane();
  if (!selection.has_value() || terminal_tab == nullptr) {
    return {};
  }
  // Read in reused chunks, so a selection over the whole scrollback costs one
  // chunk of rows, not one vector per selected row. Each chunk carries the row
  // after it, which says whether its last row's line break is real. The PTY
  // reader can trim or (host terminal) shift rows between two chunks; a read
  // that saw either is taken again, and the last attempt stands, as the single
  // snapshot this replaced stood whatever moved after it.
  constexpr std::size_t kChunkRows = 256;
  const terminal::TerminalSession& session = terminal_tab->session;
  std::vector<terminal::TerminalLine> chunk;
  for (int attempt = 0;; ++attempt) {
    const std::uint64_t trims = session.ScrollbackTrimTotal();
    const std::uint64_t shifts = session.RowShiftSequence();
    TerminalSelectionTextBuilder builder(*selection);
    bool next_row_continues = false;
    for (std::size_t row = selection->start.row; row <= selection->end.row;) {
      const std::size_t count = std::min(kChunkRows, selection->end.row - row + 1);
      session.SnapshotLineRangeInto(row, count + 1, chunk);
      const std::size_t fed = std::min(count, chunk.size());
      bool more = fed == count;
      for (std::size_t i = 0; i < fed && more; ++i) {
        more = builder.AddRow(row + i, chunk[i]);
      }
      next_row_continues = more && chunk.size() > count && chunk[count].wrapped_from_previous;
      if (!more) {
        break;
      }
      row += count;
    }
    if (attempt == 2 || (trims == session.ScrollbackTrimTotal() && shifts == session.RowShiftSequence())) {
      return builder.Finish(next_row_continues);
    }
  }
}

bool TerminalPanelService::SelectAll() {
  TerminalPaneState* pane = state_.active_terminal_pane();
  if (pane == nullptr) {
    return false;
  }
  // Everything up to the last row that shows text: the grid below the prompt is
  // padding, and a copy must not end in a run of empty lines. Only the screen's
  // worth of rows at the end can be that padding.
  const std::size_t line_count = pane->session.LineCount();
  const std::size_t tail = std::min(line_count, std::max<std::size_t>(1, pane->session.rows()));
  const std::vector<terminal::TerminalLine> last_rows = pane->session.SnapshotLineRange(line_count - tail, tail);
  for (std::size_t index = last_rows.size(); index > 0; --index) {
    if (const auto bounds = TerminalLineBoundsAt(last_rows[index - 1], line_count - tail + index - 1);
        bounds.has_value()) {
      pane->mouse_selecting = false;
      pane->selection_unit = TerminalSelectionUnit::Cell;
      pane->selection_origin = TerminalSelectionBounds{.start = {0, 0}, .end = {0, 0}};
      pane->selection_anchor = TerminalSelectionPoint{0, 0};
      pane->selection_head = bounds->end;
      SyncPrimarySelectionWithTerminalSelection();
      return true;
    }
  }
  return false;
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
