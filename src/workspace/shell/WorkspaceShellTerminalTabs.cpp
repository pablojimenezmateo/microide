#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/CompareMergeService.h"
#include "workspace/CompareInput.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/git/BranchReviewStateBridge.h"
#include "workspace/persistence/WorkspacePersistenceCoordinator.h"

#include <algorithm>

#include "util/Parse.h"
#include "workspace/SettingFlags.h"
#include "workspace/services/SidebarService.h"
#include "workspace/services/TerminalPanelService.h"
#include "workspace/NativeDialogTestMode.h"
#include "workspace/TerminalAttention.h"
#include "platform/HostIntegration.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"

namespace microide::workspace {

namespace {

std::size_t ResolvedScrollbackLines(const std::optional<std::string>& setting) {
  const int parsed = util::ParseIntOr(setting, 2000);
  return static_cast<std::size_t>(std::clamp(parsed, 200, 100000));
}

}

TerminalPanelService& WorkspaceShell::MakeTerminalPanelService() {
  if (glue_->terminal_panel_service != nullptr) {
    return *glue_->terminal_panel_service;
  }
  const auto start_pane = [this](TerminalPaneState& pane, const platform::ProcessLauncher& project_launcher,
                                 const std::filesystem::path& cwd, const std::string& command,
                                 terminal::HostIdentity attach) {
    if (terminal_event_type_ != 0) {
      pane.session.SetWakeChannel(terminal_event_type_);
    }
    pane.session.SetMaxScrollbackLines(
        ResolvedScrollbackLines(GetSettingValue("terminal.scrollback_lines")));
    pane.session.SetPredictionMode(terminal::TerminalPredictionOverlay::ParseMode(
        GetSettingValue("remote.predict").value_or("adaptive")));
    pane.launch_working_directory = cwd;
    pane.launch_command = command;
    const platform::ProcessLauncher& launcher = pane.launcher != nullptr ? *pane.launcher : project_launcher;
    return terminal::UsePlaceholderTerminalsForTesting() && pane.launcher == nullptr && launcher.is_local()
               ? pane.session.StartPlaceholderForTesting(cwd, command)
               : pane.session.Start(launcher, cwd, command,
                                    launcher.is_local() ? GetSettingValue("terminal.shell").value_or("")
                                                        : std::string(),
                                    std::move(attach));
  };
  glue_->terminal_panel_service = std::make_unique<TerminalPanelService>(
      context_.current_project_state,
      TerminalPanelService::Operations{
          .read_primary_selection_text = [this]() { return ReadPrimarySelectionText(); },
          .clear_terminal_selection = [this]() { ClearTerminalSelection(); },
          .append_terminal_pending_input =
              [this](std::string_view input) { AppendTerminalPendingInput(input); },
          .terminal_link_at_point = [this](float x, float y) { return TerminalLinkAtPoint(x, y); },
          .open_external_url = [this](std::string_view url) { return OpenExternalUrl(url); },
          .open_host_local_url =
              [this](const platform::ProcessLauncher& launcher, std::string_view url) {
                return glue_->remote_host_service != nullptr &&
                       glue_->remote_host_service->OpenHostLocalUrl(launcher, url);
              },
          .sync_primary_selection_with_terminal_selection =
              [this]() { SyncPrimarySelectionWithTerminalSelection(); },
          .make_started_pane =
              [this, start_pane](std::string command,
                                 std::shared_ptr<const platform::ProcessLauncher> launcher,
                                 std::string label_prefix, std::filesystem::path cwd,
                                 terminal::HostIdentity attach) -> std::unique_ptr<TerminalPaneState> {
                if (context_.current_project_state.root.empty() && launcher == nullptr) {
                  return nullptr;
                }
                auto pane = std::make_unique<TerminalPaneState>();
                pane->launcher = std::move(launcher);
                pane->label_prefix = std::move(label_prefix);
                if (!start_pane(*pane, context_.current_project_state.launcher(),
                                cwd.empty() ? context_.current_project_state.root : cwd, command,
                                std::move(attach))) {
                  return nullptr;
                }
                if (pane->session.host_pending() && glue_->remote_host_service != nullptr) {
                  pane->session.SetPendingStatus(glue_->remote_host_service->PendingTerminalLine(
                      context_.current_project_state.root));
                }
                return pane;
              },
          .make_host_pane =
              [start_pane](const ProjectWorkspaceState& state,
                           terminal::HostIdentity attach) -> std::unique_ptr<TerminalPaneState> {
                auto pane = std::make_unique<TerminalPaneState>();
                if (!start_pane(*pane, state.launcher(), state.root, {}, std::move(attach))) {
                  return nullptr;
                }
                return pane;
              },
          .relaunch_pane =
              [this, start_pane](TerminalPaneState& pane) {
                return start_pane(pane, context_.current_project_state.launcher(),
                                  pane.launch_working_directory, pane.launch_command, {});
              },
          .note_layout_inputs_changed = [this]() { NoteLayoutInputsChanged(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .revive_terminals =
              [this]() {
                return SettingFlagEnabled(GetSettingValue("terminal.restore_sessions"), true);
              },
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
      .forget_recent_project =
          [this](const project::ProjectId& id) { recents_service_.ForgetProject(id); },
      .recent_projects = [this]() -> const std::vector<project::ProjectId>& {
        return recents_service_.RecentProjects();
      },
      .recents_revision = [this]() { return recents_service_.revision(); },
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
            if (status_bar_service_.SetSegment(StatusBarSegmentId::Remote, std::move(value))) {
              RequestPreparedFrame();
            }
          },
      .compare_files =
          [this](const std::filesystem::path& left, std::string left_label,
                 const std::filesystem::path& right, std::string right_label) -> std::string {
            std::optional<CompareInput> left_input = ReadFileCompareInput(left, false);
            std::optional<CompareInput> right_input = ReadFileCompareInput(right, true);
            if (!left_input.has_value() || !right_input.has_value()) {
              return "cannot read " + (left_input.has_value() ? right : left).string();
            }
            left_input->label = std::move(left_label);
            right_input->label = std::move(right_label);
            return MakeCompareMergeService().diff_tabs().OpenPlainComparison(std::move(*left_input),
                                                                 std::move(*right_input))
                       ? std::string()
                       : std::string("the comparison did not open");
          },
      .project_reconnected =
          [this](const std::filesystem::path& root) {
            if (context_.current_project_state.root == root) {
              RequestGitSidebarRefresh();
            }
          },
      .start_host_terminals =
          [this](const std::filesystem::path& root) {
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            if (state != nullptr && TerminalPanelService::StartPendingHostPanes(*state) > 0 &&
                state == &context_.current_project_state) {
              RequestPreparedFrame();
            }
          },
      .host_terminals_waiting =
          [this](const std::filesystem::path& root, std::string_view line) {
            if (ProjectWorkspaceState* state = context_.ProjectStateForRoot(root); state != nullptr) {
              TerminalPanelService::SetPendingHostStatus(*state, line);
            }
          },
      .host_terminals_claimed =
          [this](const std::filesystem::path& root) {
            RemoteHostService::HostTerminalClaims claims;
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            if (state == nullptr) {
              return claims;
            }
            for (const auto& tab : state->terminal_tabs) {
              for (const auto& pane : tab->panes) {
                if (pane == nullptr || pane->launcher != nullptr) {
                  continue;
                }
                if (const terminal::HostIdentity identity = pane->session.host_identity(); !identity.empty()) {
                  claims.handles.push_back(identity.handle);
                }
                if (!pane->reattach_unreported) {
                  continue;
                }
                switch (pane->session.host_reattach()) {
                  case terminal::HostReattach::Pending:
                    continue;
                  case terminal::HostReattach::Reattached:
                    ++claims.reattached;
                    break;
                  case terminal::HostReattach::ServerRestarted:
                    ++claims.server_restarted;
                    break;
                  case terminal::HostReattach::Gone:
                    ++claims.gone;
                    break;
                  case terminal::HostReattach::None:
                    break;
                }
                pane->reattach_unreported = false;
              }
            }
            return claims;
          },
      .attach_host_terminal =
          [this](const std::filesystem::path& root, terminal::HostIdentity identity, bool focus) {
            TerminalPanelService& panel = MakeTerminalPanelService();
            if (focus) {
              return context_.current_project_state.root == root &&
                     panel.OpenTerminalAttached(std::move(identity));
            }
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            return state != nullptr && panel.AdoptHostTerminal(*state, std::move(identity));
          },
      .host_terminal_tab =
          [this](const terminal::HostIdentity& identity, bool focus) {
            return MakeTerminalPanelService().FindHostTerminal(identity, focus);
          },
      .type_into_host_terminal =
          [this](const std::filesystem::path& root, const terminal::HostIdentity& identity, std::string text) {
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            if (state == nullptr) {
              return false;
            }
            for (const auto& tab : state->terminal_tabs) {
              for (const auto& pane : tab->panes) {
                if (pane != nullptr && pane->session.running() && pane->session.host_identity() == identity) {
                  pane->follow_tail = true;
                  pane->session.PasteText(text);
                  RequestPreparedFrame();
                  return true;
                }
              }
            }
            return false;
          },
      .recent_terminal_lines =
          [this](const std::filesystem::path& root, const std::function<bool(std::string_view line)>& visit) {
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            if (state == nullptr) {
              return;
            }
            constexpr std::size_t kLines = 200;
            std::string text;
            for (const auto& tab : state->terminal_tabs) {
              for (const auto& pane : tab->panes) {
                if (pane == nullptr || pane->launcher != nullptr || !pane->session.is_host_terminal()) {
                  continue;
                }
                const std::size_t count = pane->session.LineCount();
                for (const terminal::TerminalLine& line :
                     pane->session.SnapshotLineRange(count > kLines ? count - kLines : 0, kLines)) {
                  text.clear();
                  for (const terminal::TerminalCell& cell : line.cells) {
                    text += cell.DisplayText();
                  }
                  if (visit(text)) {
                    return;
                  }
                }
              }
            }
          },
      .host_activity_updated =
          [this]() {
            if (HostViewShown(context_.current_project_state.sidebar)) {
              MakeSidebarService().RefreshHostView();
            }
          },
      .git_status_pushed =
          [this](const std::filesystem::path& root) {
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
      .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
      .open_file_at =
          [this](const std::filesystem::path& path, std::size_t line, std::size_t column) {
            OpenFileAtLocation(path, line, column);
          },
      .review_text =
          [this](const std::filesystem::path& root) {
            const ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            return state != nullptr ? RoamingReviewText(*state) : std::string();
          },
      .take_review =
          [this](const std::filesystem::path& root, std::string_view text) {
            ProjectWorkspaceState* state = context_.ProjectStateForRoot(root);
            if (state == nullptr || !TakeRoamingReview(text, state)) {
              return;
            }
            if (state == &context_.current_project_state) {
              MakePersistenceCoordinator().SaveConfigState();
              RequestPreparedFrame();
              RequestPreparedFrame();
            }
          },
      .host_changes_updated =
          [this](const std::filesystem::path& root) {
            if (context_.current_project_state.root == root &&
                context_.current_project_state.sidebar.view_id == "host-changes") {
              MakeSidebarService().RefreshHostView();
            }
          },
      .conflicts_updated =
          [this](const std::filesystem::path& root) {
            if (context_.current_project_state.root == root &&
                context_.current_project_state.sidebar.view_id == "conflicts") {
              MakeSidebarService().RefreshHostView();
            }
          },
      .open_url = [this](std::string_view url) { return OpenExternalUrl(url); },
      .open_merge =
          [this](const std::filesystem::path& base, const std::filesystem::path& incoming,
                 const std::filesystem::path& current, const std::filesystem::path& output) {
            if (!MakeCompareMergeService().diff_tabs().OpenMergeEditor(base, incoming, current, output)) {
              return std::string("the merge editor did not open");
            }
            if (MergeTabState* merge = ActiveMergeTab(); merge != nullptr && !merge->result_viewport.dirty()) {
              RefreshMergeTabDerivedState(*merge);
            }
            return std::string();
          },
      .project_status_changed =
          [this](const std::filesystem::path& root, const editor::WelcomeRemoteStatus& status) {
            if (ProjectWorkspaceState* state = context_.ProjectStateForRoot(root); state != nullptr) {
              const editor::WelcomeStageState shown = editor::WorstRemoteStage(state->remote_status);
              state->remote_status = status;
              ++state->remote_status_revision;
              if (state == &context_.current_project_state) {
                RequestPreparedFrame();
              }
              if (editor::WorstRemoteStage(status) != shown) {
                RequestPreparedFrame();
              }
            }
          },
      .pending_paths_changed =
          [this](const std::filesystem::path& root, project::SharedPendingPaths pending) {
            if (ProjectWorkspaceState* state = context_.ProjectStateForRoot(root); state != nullptr) {
              state->SetPendingPaths(std::move(pending));
              if (state == &context_.current_project_state) {
                RequestPreparedFrame();
              }
            }
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
  const bool allow_osc52_clipboard =
      SettingFlagEnabled(GetSettingValue("terminal.osc52_clipboard_write"), false);
  const bool terminal_on_screen =
      context_.current_project_state.panel.content == PanelContentKind::Terminal;
  const std::size_t active_index = context_.current_project_state.active_terminal_tab_index;
  bool badge_changed = false;
  const util::Rgba8 default_foreground{theme_.text_primary.r, theme_.text_primary.g,
                                       theme_.text_primary.b, 0xff};
  const util::Rgba8 default_background{theme_.surface_background.r, theme_.surface_background.g,
                                       theme_.surface_background.b, 0xff};
  bool host_terminal_unrecorded = false;
  InteractionState& interaction = context_.interaction_state;
  const auto& front_tabs = context_.current_project_state.terminal_tabs;
  std::optional<bool> alert_setting;
  const auto alert_when_unfocused = [&]() {
    if (!alert_setting.has_value()) {
      alert_setting = SettingFlagEnabled(GetSettingValue("terminal.alert_when_unfocused"), true);
    }
    return *alert_setting;
  };
  const auto drain = [&](const std::filesystem::path& root, std::vector<std::unique_ptr<TerminalTabState>>& tabs,
                         std::optional<std::size_t> on_screen_index, std::string_view project_label) {
    bool marked = false;
    host_terminal_unrecorded = false;
    for (std::size_t index = 0; index < tabs.size(); ++index) {
      TerminalTabState* tab = tabs[index].get();
      if (tab == nullptr) {
        continue;
      }
      const bool tab_on_screen = on_screen_index == index;
      for (const auto& pane : tab->panes) {
        if (pane == nullptr) {
          continue;
        }
        pane->session.SetDefaultColors(default_foreground, default_background);
        const bool output_arrived = pane->session.ConsumeWakeEvent();
        if (pane->launcher == nullptr) {
          if (const std::uint64_t handle = pane->session.host_handle(); handle != pane->seen_host_handle) {
            pane->seen_host_handle = handle;
            host_terminal_unrecorded = host_terminal_unrecorded || handle != 0;
          }
        }
        if (output_arrived && !tab_on_screen && !tab->has_unseen_output) {
          tab->has_unseen_output = true;
          marked = true;
        }
        const bool bell = pane->session.ConsumeBell();
        const std::optional<terminal::TerminalSession::Notification> notification =
            pane->session.ConsumeNotification();
        if (bell || notification.has_value()) {
          const std::string terminal_label = pane->session.LaunchLabel();
          const bool in_front = &tabs == &front_tabs;
          const std::string front_label = in_front ? context_.current_project_state.id.Label() : std::string();
          TerminalAlertOutcome alert = DecideTerminalAlert(TerminalAlert{
              .bell = bell,
              .notification = notification.has_value(),
              .title = notification.has_value() ? std::string_view(notification->title) : std::string_view(),
              .body = notification.has_value() ? std::string_view(notification->body) : std::string_view(),
              .project_label = in_front ? std::string_view(front_label) : project_label,
              .terminal_label = terminal_label,
              .project_in_front = in_front,
              .tab_on_screen = tab_on_screen,
              .window_focused = interaction.window_has_input_focus,
              .alert_when_unfocused = alert_when_unfocused(),
          });
          if (!alert.toast.empty()) {
            Notify(NotificationService::Tone::Info, std::move(alert.toast));
          }
          if (alert.mark_tab && !tab->has_unseen_bell) {
            tab->has_unseen_bell = true;
            marked = true;
          }
          if (alert.flag_window) {
            ++interaction.window_flags_requested;
            if (dialog_window_ != nullptr) {
              SDL_FlashWindow(dialog_window_, SDL_FLASH_UNTIL_FOCUSED);
            }
          }
          if (alert.desktop.has_value() &&
              AdmitDesktopNotification(SDL_GetTicks(), interaction.last_desktop_notification_ms)) {
            ++interaction.desktop_notifications_posted;
            if (!NativeDialogsDisabledForTesting()) {
              project_background_executor_.Post([posted = std::move(*alert.desktop)]() {
                (void)platform::ShowDesktopNotification(posted.summary, posted.body);
              });
            }
          }
        }
        terminal::TerminalHostRequests requests = pane->session.ConsumeHostRequests();
        if (requests.open_url.has_value()) {
          (void)MakeTerminalPanelService().OpenUrlForPane(pane.get(), *requests.open_url);
        }
        if (requests.edit.has_value()) {
          MakeRemoteHostService().BeginTerminalEdit(root, pane->session.host_identity().epoch,
                                                    std::move(*requests.edit));
        }
        if (pane->session.ConsumeOversizedOsc52Dropped()) {
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
            Notify(NotificationService::Tone::Info,
                   "A terminal program tried to set the clipboard (OSC 52). Enable "
                   "\"Allow Terminal Clipboard Writes (OSC 52)\" in Settings to permit it.");
          }
        }
      }
    }
    return marked;
  };
  badge_changed = drain(context_.current_project_state.root, context_.current_project_state.terminal_tabs,
                        terminal_on_screen ? std::optional<std::size_t>(active_index) : std::nullopt,
                        {});
  if (host_terminal_unrecorded) {
    context_.current_project_state.host_terminals_unrecorded = true;
    session_flush_.Note(SDL_GetTicks());
  }
  auto& catalog = context_.project_catalog;
  for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
    if (ProjectWorkspaceState* background = catalog.entries[i].get();
        background != nullptr && i != catalog.active_index && background != &context_.current_project_state) {
      (void)drain(background->root, background->terminal_tabs, std::nullopt, background->id.Label());
      background->host_terminals_unrecorded = background->host_terminals_unrecorded || host_terminal_unrecorded;
    }
  }
  SyncTerminalFocusState();
  if (had_terminal_tabs) {
    const bool reloaded = ReloadProjectIfFilesChanged(false);
    if (!reloaded) {
      RequestAutomaticGitSidebarRefresh();
    }
  }
  if (BottomPanelVisible() != panel_visible_before) {
    NoteLayoutInputsChanged();
  }
  if (panel_visible_before || BottomPanelVisible()) {
    RequestPreparedFrame();
  } else if (badge_changed) {
  }
}

}
