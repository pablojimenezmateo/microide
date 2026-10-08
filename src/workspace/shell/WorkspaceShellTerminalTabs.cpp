#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/CompareInput.h"
#include "workspace/services/RemoteHostService.h"

#include <algorithm>

#include "util/Parse.h"
#include "workspace/SettingFlags.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"

namespace microide::workspace {

// The terminal strip's tab, pane and panel verbs live on TerminalPanelService;
// this TU keeps only what needs the shell itself — the session launch (project
// launcher, settings, wake channel), the focus-event sync and the per-tick
// session drain that also owns the clipboard and notification side effects.

namespace {

std::size_t ResolvedScrollbackLines(const std::optional<std::string>& setting) {
  const int parsed = util::ParseIntOr(setting, 2000);
  return static_cast<std::size_t>(std::clamp(parsed, 200, 100000));
}

}  // namespace

TerminalPanelService& WorkspaceShell::MakeTerminalPanelService() {
  if (glue_->terminal_panel_service != nullptr) {
    return *glue_->terminal_panel_service;
  }
  // Starts a session the way every terminal in this project starts: the
  // project's launcher (a remote project's terminal is this same call and its
  // shell runs on the host, TD-2026-09-22-301), the project's `terminal.shell`,
  // the scrollback cap and the shell's wake channel.
  const auto start_pane = [this](TerminalPaneState& pane, const std::filesystem::path& cwd,
                                 const std::string& command) {
    if (terminal_event_type_ != 0) {
      pane.session.SetWakeChannel(terminal_event_type_);
    }
    pane.session.SetMaxScrollbackLines(
        ResolvedScrollbackLines(GetSettingValue("terminal.scrollback_lines")));
    pane.session.SetPredictionMode(terminal::TerminalPredictionOverlay::ParseMode(
        GetSettingValue("remote.predict").value_or("adaptive")));
    pane.launch_working_directory = cwd;
    pane.launch_command = command;
    // A pane on a host runs through that host's launcher, and a host's shell is
    // the host's login shell: `terminal.shell` names a program on THIS machine.
    const bool on_host = pane.launcher != nullptr;
    const platform::ProcessLauncher& launcher =
        on_host ? *pane.launcher : context_.current_project_state.launcher();
    return terminal::UsePlaceholderTerminalsForTesting() && !on_host
               ? pane.session.StartPlaceholderForTesting(cwd, command)
               : pane.session.Start(launcher, cwd, command,
                                    on_host ? std::string()
                                            : GetSettingValue("terminal.shell").value_or(""));
  };
  glue_->terminal_panel_service = std::make_unique<TerminalPanelService>(
      context_.current_project_state,
      TerminalPanelService::Operations{
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .clear_terminal_selection = [this]() { ClearTerminalSelection(); },
          .append_terminal_pending_input =
              [this](std::string_view input) { AppendTerminalPendingInput(input); },
          .terminal_url_at_point = [this](float x, float y) { return TerminalUrlAtPoint(x, y); },
          .open_external_url = [this](std::string_view url) { return OpenExternalUrl(url); },
          .sync_primary_selection_with_terminal_selection =
              [this]() { SyncPrimarySelectionWithTerminalSelection(); },
          .make_started_pane =
              [this, start_pane](std::string command,
                                 std::shared_ptr<const platform::ProcessLauncher> launcher,
                                 std::string label_prefix) -> std::unique_ptr<TerminalPaneState> {
                if (context_.current_project_state.root.empty() && launcher == nullptr) {
                  return nullptr;
                }
                auto pane = std::make_unique<TerminalPaneState>();
                pane->launcher = std::move(launcher);
                pane->label_prefix = std::move(label_prefix);
                if (!start_pane(*pane, context_.current_project_state.root, command)) {
                  return nullptr;
                }
                return pane;
              },
          .relaunch_pane =
              [start_pane](TerminalPaneState& pane) {
                return start_pane(pane, pane.launch_working_directory, pane.launch_command);
              },
          .note_layout_inputs_changed = [this]() { NoteLayoutInputsChanged(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .request_bottom_panel_redraw = [this]() { RequestBottomPanelRedraw(); },
      });
  return *glue_->terminal_panel_service;
}

RemoteHostService& WorkspaceShell::MakeRemoteHostService() {
  if (glue_->remote_host_service != nullptr) {
    return *glue_->remote_host_service;
  }
  glue_->remote_host_service = std::make_unique<RemoteHostService>(RemoteHostService::Operations{
      .notify = [this](NotificationService::Request request) { Notify(std::move(request)); },
      .dismiss_notification =
          [this](std::string_view key) {
            if (notification_service_.DismissKey(key)) {
              RequestFullRedraw();
            }
          },
      .open_terminal =
          [this](std::shared_ptr<const platform::ProcessLauncher> launcher, std::string label_prefix,
                 std::string command) {
            return MakeTerminalPanelService().OpenTerminalOn(std::move(launcher),
                                                             std::move(label_prefix),
                                                             std::move(command));
          },
      .setting = [this](std::string_view key) { return GetSettingValue(std::string(key)); },
      .set_status_segment =
          [this](StatusBarSegmentValue value) {
            status_bar_service_.SetSegment(StatusBarSegmentId::Remote, std::move(value));
          },
      .request_redraw = [this]() { RequestFullRedraw(); },
      .compare_files =
          [this](const std::filesystem::path& left, std::string left_label,
                 const std::filesystem::path& right, std::string right_label) -> std::string {
            std::optional<CompareInput> left_input = ReadFileCompareInput(left, /*editable=*/false);
            std::optional<CompareInput> right_input = ReadFileCompareInput(right, /*editable=*/true);
            if (!left_input.has_value() || !right_input.has_value()) {
              return "cannot read " + (left_input.has_value() ? right : left).string();
            }
            left_input->label = std::move(left_label);
            right_input->label = std::move(right_label);
            return MakeCompareMergeService().OpenPlainComparison(std::move(*left_input),
                                                                 std::move(*right_input))
                       ? std::string()
                       : std::string("the comparison did not open");
          },
      .project_reconnected =
          [this](const std::filesystem::path& root) {
            // A git refresh that failed while the link was down left a stale or
            // failed snapshot; nothing else would ask again until a file changes.
            if (context_.current_project_state.root == root) {
              RequestGitSidebarRefresh();
            }
          },
      .show_output =
          [this](std::string_view id, std::string_view label, std::string_view text) {
            output_channels_.Clear(id);
            for (std::size_t start = 0; start < text.size();) {
              const std::size_t end = std::min(text.find('\n', start), text.size());
              output_channels_.AppendLine(id, label, std::string(text.substr(start, end - start)));
              start = end + 1;
            }
            (void)ActionCoordinator(MakeActionContext())
                .Execute(ActionId::ShowOutput, {std::string(id)}, ActionSource::Shortcut);
          },
  });
  glue_->remote_host_service->SetWakeChannel(project_file_event_type_);
  return *glue_->remote_host_service;
}

WorkspaceShell::TerminalPaneState* WorkspaceShell::ActiveTerminalPane() {
  return context_.current_project_state.active_terminal_pane();
}

const WorkspaceShell::TerminalPaneState* WorkspaceShell::ActiveTerminalPane() const {
  return context_.current_project_state.active_terminal_pane();
}

std::optional<std::size_t> WorkspaceShell::FocusedTerminalTabIndex() const {
  if (!context_.interaction_state.window_has_input_focus ||
      CurrentTextInputSurface() != TextInputSurface::Terminal ||
      context_.current_project_state.active_terminal_pane() == nullptr) {
    return std::nullopt;
  }
  return context_.current_project_state.active_terminal_tab_index;
}

void WorkspaceShell::SyncTerminalFocusState() {
  const std::optional<std::size_t> focused_index = FocusedTerminalTabIndex();
  const auto& tabs = context_.current_project_state.terminal_tabs;
  for (std::size_t index = 0; index < tabs.size(); ++index) {
    const TerminalTabState* tab = tabs[index].get();
    if (tab == nullptr) {
      continue;
    }
    for (std::size_t pane_index = 0; pane_index < tab->panes.size(); ++pane_index) {
      TerminalPaneState* pane = tab->panes[pane_index].get();
      if (pane == nullptr) {
        continue;
      }
      // Only the active pane of the focused tab has the keyboard.
      const bool should_focus = focused_index.has_value() && *focused_index == index &&
                                pane_index == tab->active_pane && pane->session.WantsFocusEvents();
      if (pane->focus_events_active == should_focus) {
        continue;
      }
      pane->session.SendFocusEvent(should_focus);
      pane->focus_events_active = should_focus;
    }
  }
}

void WorkspaceShell::ConsumeTerminalSessionUpdates() {
  const bool panel_visible_before = BottomPanelVisible();
  const std::size_t tab_count_before = context_.current_project_state.terminal_tabs.size();
  const bool had_terminal_tabs = tab_count_before > 0;
  // OSC 52 lets a program running in the terminal set the system clipboard. That
  // is a silent poisoning vector (any output could swap a copied command for a
  // malicious one), so honor it only when the user has opted in. The pending text
  // is still drained either way so it can't accumulate.
  const bool allow_osc52_clipboard =
      SettingFlagEnabled(GetSettingValue("terminal.osc52_clipboard_write"), false);
  const bool terminal_on_screen =
      context_.current_project_state.panel.content == PanelContentKind::Terminal;
  const std::size_t active_index = context_.current_project_state.active_terminal_tab_index;
  bool badge_changed = false;
  const auto& tabs = context_.current_project_state.terminal_tabs;
  // What the bottom panel paints an unstyled cell with (resolve_terminal_colors):
  // programs ask for it to pick their own light or dark palette.
  const util::Rgba8 default_foreground{theme_.text_primary.r, theme_.text_primary.g,
                                       theme_.text_primary.b, 0xff};
  const util::Rgba8 default_background{theme_.surface_background.r, theme_.surface_background.g,
                                       theme_.surface_background.b, 0xff};
  for (std::size_t index = 0; index < tabs.size(); ++index) {
    TerminalTabState* tab = tabs[index].get();
    if (tab == nullptr) {
      continue;
    }
    const bool tab_on_screen = terminal_on_screen && index == active_index;
    for (const auto& pane : tab->panes) {
      if (pane == nullptr) {
        continue;
      }
      pane->session.SetDefaultColors(default_foreground, default_background);
      const bool output_arrived = pane->session.ConsumeWakeEvent();
      // Output in a tab the user cannot see lights its strip tab up, so an agent
      // finishing in a background tab (or behind a hidden panel) is visible
      // without switching to it. The frame that shows the tab clears it.
      if (output_arrived && !tab_on_screen && !tab->has_unseen_output) {
        tab->has_unseen_output = true;
        badge_changed = true;
      }
      // Consumed whether or not the tab is on screen, so a bell rung while the
      // user was watching does not mark the tab later.
      if (pane->session.ConsumeBell() && !tab_on_screen && !tab->has_unseen_bell) {
        tab->has_unseen_bell = true;
        badge_changed = true;
      }
      if (pane->session.ConsumeOversizedOsc52Dropped()) {
        // An OSC 52 clipboard write that overran the escape-sequence buffer was
        // dropped. Surface it rather than fail silently so the user knows their
        // (too-large) clipboard write did not land.
        Notify(NotificationService::Tone::Info,
               "A terminal program tried to set the clipboard (OSC 52), but the payload "
               "was too large and was ignored.");
      }
      const std::optional<std::string> clipboard_text =
          pane->session.ConsumePendingClipboardText();
      if (clipboard_text.has_value()) {
        if (allow_osc52_clipboard) {
          WriteClipboardText(*clipboard_text);
        } else {
          // Surface every blocked write so it is not a silent regression for users
          // who rely on OSC 52 yank-to-clipboard (tmux/vim over SSH). The toast
          // service coalesces/expires duplicates, so this cannot flood the UI.
          Notify(NotificationService::Tone::Info,
                 "A terminal program tried to set the clipboard (OSC 52). Enable "
                 "\"Allow Terminal Clipboard Writes (OSC 52)\" in Settings to permit it.");
        }
      }
    }
  }
  // Exited terminal tabs are intentionally retained until the user closes them,
  // so the `[process exited]` marker and prior command output stay inspectable
  // (mirrors VS Code's task-terminal exit behavior). TD-2026-07-17A-130.
  SyncTerminalFocusState();
  if (had_terminal_tabs) {
    const bool reloaded = ReloadProjectIfFilesChanged(false);
    if (!reloaded) {
      RequestAutomaticGitSidebarRefresh();
    }
  }
  if (BottomPanelVisible() != panel_visible_before ||
      context_.current_project_state.terminal_tabs.size() != tab_count_before) {
    if (BottomPanelVisible() != panel_visible_before) {
      NoteLayoutInputsChanged();
    }
    RequestWindowRedraw();
  } else if (panel_visible_before) {
    RequestBottomPanelRedraw();
  } else if (badge_changed) {
    // The panel is hidden, so there is no strip to repaint; the badge shows the
    // next time it is. Nothing to do now.
  }
}

}  // namespace microide::workspace
