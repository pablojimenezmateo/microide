#include "workspace/WorkspacePersistenceCoordinator.h"

#include <algorithm>
#include <filesystem>
#include <limits>

#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/StartupTrace.h"
#include "workspace/WorkspaceConversation.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

std::string SerializeRequestStatus(RequestStatus status) {
  switch (status) {
    case RequestStatus::Idle: return "idle";
    case RequestStatus::Queued: return "queued";
    case RequestStatus::Running: return "running";
    case RequestStatus::Streaming: return "streaming";
    case RequestStatus::Succeeded: return "succeeded";
    case RequestStatus::Failed: return "failed";
    case RequestStatus::Cancelled: return "cancelled";
  }
  return "idle";
}

RequestStatus ParseRequestStatus(const std::string& s) {
  if (s == "queued") return RequestStatus::Queued;
  if (s == "running") return RequestStatus::Running;
  if (s == "streaming") return RequestStatus::Streaming;
  if (s == "succeeded") return RequestStatus::Succeeded;
  if (s == "failed") return RequestStatus::Failed;
  if (s == "cancelled") return RequestStatus::Cancelled;
  return RequestStatus::Idle;
}

std::string SerializeToolMode(ToolMode mode) {
  switch (mode) {
    case ToolMode::NoTools: return "no_tools";
    case ToolMode::Ask: return "ask";
    case ToolMode::Auto: return "auto";
  }
  return "ask";
}

ToolMode ParseToolMode(const std::string& s) {
  if (s == "no_tools") return ToolMode::NoTools;
  if (s == "auto") return ToolMode::Auto;
  return ToolMode::Ask;
}

std::string SerializeMessageRole(MessageRole role) {
  switch (role) {
    case MessageRole::User: return "user";
    case MessageRole::Assistant: return "assistant";
    case MessageRole::System: return "system";
  }
  return "user";
}

MessageRole ParseMessageRole(const std::string& s) {
  if (s == "assistant") return MessageRole::Assistant;
  if (s == "system") return MessageRole::System;
  return MessageRole::User;
}

std::vector<Conversation> RestoreConversations(const PersistedChatState& chat,
                                               bool* any_interrupted) {
  std::vector<Conversation> result;
  result.reserve(chat.conversations.size());
  for (const auto& pc : chat.conversations) {
    Conversation conv;
    conv.schema_version = pc.schema_version;
    conv.id = pc.id;
    conv.title = pc.title;
    conv.provider_id = pc.provider_id;
    conv.model_id = pc.model_id;
    conv.tool_mode = ParseToolMode(pc.tool_mode);
    conv.draft = pc.draft;
    conv.system_prompt = pc.system_prompt;
    conv.created_at = pc.created_at;
    conv.updated_at = pc.updated_at;
    conv.last_request_duration_ms = pc.last_request_duration_ms;

    const RequestStatus stored_status = ParseRequestStatus(pc.status);
    if (!IsTerminalRequestStatus(stored_status)) {
      // Non-terminal request interrupted by reload or shutdown.
      conv.status = RequestStatus::Failed;
      if (any_interrupted != nullptr) {
        *any_interrupted = true;
      }
    } else {
      conv.status = stored_status;
    }

    for (const auto& pm : pc.messages) {
      Message msg;
      msg.id = pm.id;
      msg.role = ParseMessageRole(pm.role);
      msg.content = pm.content;
      msg.timestamp = pm.timestamp;
      msg.provider_id = pm.provider_id;
      msg.model = pm.model;
      msg.error = pm.error;
      msg.request_duration_ms = pm.request_duration_ms;
      const RequestStatus msg_status = ParseRequestStatus(pm.status);
      if (!IsTerminalRequestStatus(msg_status)) {
        msg.status = RequestStatus::Failed;
        if (msg.error.empty()) {
          msg.error = "Interrupted by reload or shutdown.";
        }
      } else {
        msg.status = msg_status;
      }
      for (const auto& tool : pm.tool_events) {
        msg.tool_events.push_back(ToolEvent{
            .call_id = tool.call_id,
            .tool_id = tool.tool_id,
            .display_name = tool.display_name,
            .arguments_summary = tool.arguments_summary,
            .status = tool.status,
            .permission_decision = tool.permission_decision,
            .capability_scope = tool.capability_scope,
            .started_at = tool.started_at,
            .finished_at = tool.finished_at,
            .duration_ms = tool.duration_ms,
            .error = tool.error,
            .output_summary = tool.output_summary,
        });
      }
      conv.messages.push_back(std::move(msg));
    }
    result.push_back(std::move(conv));
  }
  return result;
}

PersistedChatState BuildPersistedChatState(const ConversationRegistry& registry,
                                           const std::string& active_conversation_id) {
  PersistedChatState chat;
  chat.active_conversation_id = active_conversation_id;
  for (const auto& conv : registry.conversations()) {
    PersistedConversationState pc;
    pc.schema_version = conv.schema_version;
    pc.id = conv.id;
    pc.title = conv.title;
    pc.provider_id = conv.provider_id;
    pc.model_id = conv.model_id;
    pc.status = SerializeRequestStatus(conv.status);
    pc.tool_mode = SerializeToolMode(conv.tool_mode);
    pc.draft = conv.draft;
    pc.system_prompt = conv.system_prompt;
    pc.created_at = conv.created_at;
    pc.updated_at = conv.updated_at;
    pc.last_request_duration_ms = conv.last_request_duration_ms;
    for (const auto& msg : conv.messages) {
      PersistedMessageState pm;
      pm.id = msg.id;
      pm.role = SerializeMessageRole(msg.role);
      pm.content = msg.content;
      pm.timestamp = msg.timestamp;
      pm.provider_id = msg.provider_id;
      pm.model = msg.model;
      pm.status = SerializeRequestStatus(msg.status);
      pm.request_duration_ms = msg.request_duration_ms;
      pm.error = msg.error;
      for (const auto& tool : msg.tool_events) {
        pm.tool_events.push_back(PersistedMessageState::PersistedToolEventState{
            .call_id = tool.call_id,
            .tool_id = tool.tool_id,
            .display_name = tool.display_name,
            .arguments_summary = tool.arguments_summary,
            .status = tool.status,
            .permission_decision = tool.permission_decision,
            .capability_scope = tool.capability_scope,
            .started_at = tool.started_at,
            .finished_at = tool.finished_at,
            .duration_ms = tool.duration_ms,
            .error = tool.error,
            .output_summary = tool.output_summary,
        });
      }
      pc.messages.push_back(std::move(pm));
    }
    chat.conversations.push_back(std::move(pc));
  }
  return chat;
}

}  // namespace

std::filesystem::path PersistenceCoordinator::SessionStatePath() const {
  return CurrentProjectState().root.empty() ? std::filesystem::path{}
                                            : operations_.project_state_directory() / "session";
}

bool PersistenceCoordinator::RestoreSessionState() {
  util::StartupTrace::Scope trace_scope("WorkspaceShell::RestoreSessionState");
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::RestoreSessionState");
  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty() || operations_.persistence_service == nullptr) {
    return false;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = CurrentProjectState().sidebar.visible;
  persisted_session.sidebar_width = CurrentProjectState().sidebar.width;
  persisted_session.bottom_panel_height = CurrentProjectState().panel.height;
  persisted_session.active_tab_index = CurrentProjectState().active_tab_index;
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::ParseSessionFile");
    if (!operations_.persistence_service->LoadProjectSession(session_path, &persisted_session)) {
      return false;
    }
  }

  auto& state = CurrentProjectState();
  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::ResetState");
    state.open_tabs.clear();
    state.active_tab_index = 0;
    state.overlay.visible = false;
    state.panel.command_mode = false;
    state.overlay.workflow.compare_picker.matches.clear();
    state.overlay.workflow.compare_picker.commits.clear();
    state.overlay.workflow.compare_picker.selected_index = 0;
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RebuildTabs");
    for (const PersistedEditorTabState& persisted_tab : persisted_session.tabs) {
    if (persisted_tab.kind == "compare") {
      std::filesystem::path compare_path = persisted_tab.compare_path;
      if (compare_path.is_relative()) {
        compare_path = state.root / compare_path;
      }
      compare_path = compare_path.lexically_normal();

      if (compare_path.empty() || persisted_tab.compare_commit_hash.empty() ||
          persisted_tab.compare_commit_short_hash.empty()) {
        continue;
      }

      const project::GitCommitEntry commit{
          .hash = persisted_tab.compare_commit_hash,
          .short_hash = persisted_tab.compare_commit_short_hash,
          .subject = {},
      };
      std::optional<TabEntry> compare_tab;
      if (!persisted_tab.compare_right_ref.empty()) {
        auto resolve_path = [&](std::filesystem::path path, const std::filesystem::path& fallback) {
          if (path.empty()) {
            return fallback;
          }
          if (path.is_relative()) {
            path = state.root / path;
          }
          return path.lexically_normal();
        };
        CompareTabState compare_state;
        compare_state.path = compare_path;
        compare_state.left_path = resolve_path(persisted_tab.compare_left_path, compare_path);
        compare_state.right_path = resolve_path(persisted_tab.compare_right_path, compare_path);
        compare_state.commit_hash = persisted_tab.compare_commit_hash;
        compare_state.right_ref = persisted_tab.compare_right_ref;
        compare_state.left_label = persisted_tab.compare_commit_short_hash;
        compare_state.right_label =
            persisted_tab.compare_right_label.empty()
                ? (persisted_tab.compare_right_ref == "WORKTREE" ? "Working tree"
                                                                 : persisted_tab.compare_right_ref)
                : persisted_tab.compare_right_label;
        compare_state.selected_row = persisted_tab.compare_selected_row;
        compare_state.scroll_row = static_cast<int>(std::min<std::size_t>(
            persisted_tab.compare_scroll_row,
            static_cast<std::size_t>(std::numeric_limits<int>::max())));
        compare_state.horizontal_scroll = persisted_tab.compare_horizontal_scroll;
        compare_state.persistable = true;
        compare_tab = operations_.build_compare_tab_from_state(compare_path, compare_state);
      } else {
        compare_tab = operations_.build_compare_tab_from_commit(compare_path, commit,
                                                                persisted_tab.compare_selected_row);
      }
      if (!compare_tab.has_value()) {
        continue;
      }
      compare_tab->compare->scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.compare_scroll_row,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
      compare_tab->compare->horizontal_scroll = persisted_tab.compare_horizontal_scroll;
      state.open_tabs.push_back(std::move(*compare_tab));
      continue;
    }
    if (persisted_tab.kind == "merge") {
      auto resolve_path = [&](std::filesystem::path path) {
        if (path.is_relative()) {
          path = state.root / path;
        }
        return path.lexically_normal();
      };

      const std::filesystem::path merge_base = resolve_path(persisted_tab.merge_base_path);
      const std::filesystem::path merge_incoming = resolve_path(persisted_tab.merge_incoming_path);
      const std::filesystem::path merge_current = resolve_path(persisted_tab.merge_current_path);
      const std::filesystem::path merge_output = resolve_path(persisted_tab.merge_output_path);
      if (merge_base.empty() || merge_incoming.empty() || merge_current.empty() ||
          merge_output.empty()) {
        continue;
      }

      auto merge_tab =
          operations_.build_merge_tab_entry(merge_base, merge_incoming, merge_current, merge_output);
      if (!merge_tab.has_value() || !merge_tab->merge.has_value()) {
        continue;
      }

      const auto parse_choice = [](std::string_view text) {
        if (text == "base") {
          return compare::MergeChoice::Base;
        }
        if (text == "incoming") {
          return compare::MergeChoice::Incoming;
        }
        if (text == "current") {
          return compare::MergeChoice::Current;
        }
        if (text == "both") {
          return compare::MergeChoice::Both;
        }
        return compare::MergeChoice::Base;
      };

      auto& merge_state = merge_tab->merge.value();
      for (std::size_t i = 0;
           i < merge_state.model.hunks.size() && i < persisted_tab.merge_hunk_choices.size(); ++i) {
        if (persisted_tab.merge_hunk_choices[i].empty()) {
          continue;
        }
        merge_state.model.hunks[i].choice = parse_choice(persisted_tab.merge_hunk_choices[i]);
      }
      merge_state.left_divider_fraction = persisted_tab.merge_left_divider_fraction;
      merge_state.right_divider_fraction = persisted_tab.merge_right_divider_fraction;
      operations_.refresh_merge_tab_derived_state(merge_state);
      merge_state.selected_hunk =
          merge_state.conflicts.empty()
              ? 0
              : std::min(persisted_tab.merge_selected_hunk, merge_state.conflicts.size() - 1);
      merge_state.scroll_row = static_cast<int>(std::min<std::size_t>(
          persisted_tab.merge_scroll_row,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
      merge_state.horizontal_scroll = persisted_tab.merge_horizontal_scroll;
      merge_state.result_viewport.SetScrollLine(
          static_cast<std::size_t>(std::max(0, merge_state.scroll_row)));
      merge_state.result_viewport.SetHorizontalScroll(merge_state.horizontal_scroll);
      merge_state.scroll_row = static_cast<int>(merge_state.result_viewport.scroll_line());
      state.open_tabs.push_back(std::move(*merge_tab));
      continue;
    }

    TabEntry::EditorTabState editor_state;
    editor_state.active_leaf_id = persisted_tab.active_leaf_id;

    for (const PersistedEditorViewState& persisted_view : persisted_tab.views) {
      std::filesystem::path view_path = persisted_view.path;
      if (!view_path.empty() && view_path.is_relative()) {
        view_path = state.root / view_path;
      }
      view_path = view_path.lexically_normal();

      if (persisted_view.dirty_snapshot) {
        editor::TextViewport restored_view;
        restored_view.LoadContent(
            util::SerializeLines(persisted_view.buffer_lines, persisted_view.line_ending), view_path,
            persisted_view.line_ending);
        restored_view.MoveCursorTo(persisted_view.cursor_line, persisted_view.cursor_column);
        restored_view.SetScrollLine(persisted_view.scroll_line);
        restored_view.SetHorizontalScroll(persisted_view.horizontal_scroll);
        restored_view.SetDirty(true);
        operations_.apply_editor_preferences(restored_view);
        editor_state.views.push_back(TabEntry::EditorTabState::EditorViewState{
            .leaf_id = persisted_view.leaf_id,
            .viewport = std::move(restored_view),
            .restored_path = view_path,
            .restored_cursor_line = persisted_view.cursor_line,
            .restored_cursor_column = persisted_view.cursor_column,
            .restored_scroll_line = persisted_view.scroll_line,
            .restored_horizontal_scroll = persisted_view.horizontal_scroll,
            .needs_restore = false,
        });
        continue;
      }

      if (view_path.empty() || !std::filesystem::exists(view_path)) {
        continue;
      }
      editor_state.views.push_back(TabEntry::EditorTabState::EditorViewState{
          .leaf_id = persisted_view.leaf_id,
          .viewport = editor::TextViewport{},
          .restored_path = view_path,
          .restored_cursor_line = persisted_view.cursor_line,
          .restored_cursor_column = persisted_view.cursor_column,
          .restored_scroll_line = persisted_view.scroll_line,
          .restored_horizontal_scroll = persisted_view.horizontal_scroll,
          .needs_restore = true,
      });
    }

    if (editor_state.views.empty()) {
      continue;
    }

    if (!persisted_tab.split_nodes.empty()) {
      std::vector<PersistedSplitNodeState> split_nodes = persisted_tab.split_nodes;
      std::sort(split_nodes.begin(), split_nodes.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.path.size() != rhs.path.size()) {
          return lhs.path.size() < rhs.path.size();
        }
        return lhs.path < rhs.path;
      });

      for (const PersistedSplitNodeState& node_state : split_nodes) {
        auto make_node = [&]() {
          auto node = std::make_unique<TabEntry::EditorTabState::EditorSplitNode>();
          node->leaf_id = node_state.leaf_id;
          node->size_fraction = std::max(0.0f, node_state.size_fraction);
          if (node_state.orientation == "vertical") {
            node->orientation = EditorSplitOrientation::Vertical;
          } else if (node_state.orientation == "horizontal") {
            node->orientation = EditorSplitOrientation::Horizontal;
          } else {
            node->orientation = EditorSplitOrientation::None;
          }
          return node;
        };

        if (node_state.path.empty()) {
          editor_state.split_root = make_node();
          continue;
        }

        std::vector<std::size_t> parent_path(node_state.path.begin(), node_state.path.end() - 1);
        auto* parent = operations_.find_editor_split_node(editor_state.split_root.get(), parent_path);
        if (parent == nullptr) {
          continue;
        }
        const std::size_t child_index = node_state.path.back();
        if (parent->children.size() <= child_index) {
          parent->children.resize(child_index + 1);
        }
        parent->children[child_index] = make_node();
      }
    }

    operations_.normalize_editor_split_tree(editor_state);
    const TabEntry::EditorTabState::EditorViewState* active_view =
        operations_.find_editor_view_state(editor_state, editor_state.active_leaf_id);
    if (active_view == nullptr) {
      active_view = &editor_state.views.front();
      editor_state.active_leaf_id = editor_state.views.front().leaf_id;
    }

    const std::filesystem::path tab_path = operations_.editor_view_path(*active_view);
    state.open_tabs.push_back(TabEntry{
        .kind = TabEntry::Kind::Editor,
        .path = tab_path,
        .title = tab_path.empty() ? "untitled" : tab_path.filename().string(),
        .editor_state = std::move(editor_state),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
  }
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RestoreLayoutState");
    state.sidebar.visible = persisted_session.sidebar_visible;
    state.sidebar.width = persisted_session.sidebar_width;
    state.panel.height = persisted_session.bottom_panel_height;
  }

  // Restore conversations; convert any non-terminal states to failed.
  if (!persisted_session.chat.conversations.empty()) {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::RestoreConversations");
    bool any_interrupted = false;
    std::vector<Conversation> restored =
        RestoreConversations(persisted_session.chat, &any_interrupted);
    state.conversations.SetConversations(std::move(restored));
    state.panel.chat.conversation_id = persisted_session.chat.active_conversation_id;
    if (any_interrupted) {
      state.panel.chat.has_restore_warning = true;
      state.panel.chat.status_text = "Interrupted by reload or shutdown.";
    }
    // Validate that active conversation id exists.
    if (!state.panel.chat.conversation_id.empty() &&
        state.conversations.GetConversation(state.panel.chat.conversation_id) == nullptr) {
      state.panel.chat.conversation_id.clear();
    }
  }

  if (state.open_tabs.empty()) {
    state.text_viewport.SetPlaceholderText(
        "microide\n\n"
        "Project loaded.\n"
        "Use the sidebar to open files.\n");
    state.surface.focus = state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
    return true;
  }

  {
    util::PerformanceTrace::Scope scope("WorkspaceShell::RestoreSessionState::FinalizeState");
    const std::size_t active_index =
        std::min(persisted_session.active_tab_index, state.open_tabs.size() - 1);
    state.active_tab_index = active_index;
    state.surface.focus = state.sidebar.visible ? FocusTarget::Sidebar : FocusTarget::Editor;
  }
  return true;
}

void PersistenceCoordinator::SaveSessionState() {
  if (CurrentProjectState().root.empty()) {
    return;
  }

  operations_.sync_active_editor_tab();

  const std::filesystem::path session_path = SessionStatePath();
  if (session_path.empty()) {
    return;
  }

  PersistedProjectSessionState persisted_session;
  persisted_session.sidebar_visible = CurrentProjectState().sidebar.visible;
  persisted_session.sidebar_width = CurrentProjectState().sidebar.width;
  persisted_session.bottom_panel_height = CurrentProjectState().panel.height;
  persisted_session.active_tab_index = 0;

  auto& state = CurrentProjectState();
  for (std::size_t tab_index = 0; tab_index < state.open_tabs.size(); ++tab_index) {
    auto& tab = state.open_tabs[tab_index];
    std::optional<PersistedEditorTabState> persisted_tab;
    if (tab.kind == TabEntry::Kind::Compare) {
      persisted_tab = BuildPersistedCompareTabState(tab);
    } else if (tab.kind == TabEntry::Kind::Merge) {
      persisted_tab = BuildPersistedMergeTabState(tab);
    } else {
      persisted_tab = BuildPersistedEditorTabState(tab_index, tab);
    }
    if (!persisted_tab.has_value()) {
      continue;
    }
    if (tab_index == state.active_tab_index) {
      persisted_session.active_tab_index = persisted_session.tabs.size();
    }
    persisted_session.tabs.push_back(std::move(*persisted_tab));
  }

  persisted_session.chat = BuildPersistedChatState(
      CurrentProjectState().conversations,
      CurrentProjectState().panel.chat.conversation_id);

  operations_.persistence_service->SaveProjectSession(session_path, persisted_session);
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedCompareTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Compare || !tab.compare.has_value() ||
      !tab.compare->persistable) {
    return std::nullopt;
  }

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "compare";
  persisted_tab.compare_path = tab.compare->path.lexically_normal();
  persisted_tab.compare_left_path = tab.compare->left_path.lexically_normal();
  persisted_tab.compare_right_path = tab.compare->right_path.lexically_normal();
  persisted_tab.compare_commit_hash = tab.compare->commit_hash;
  persisted_tab.compare_commit_short_hash = tab.compare->left_label;
  persisted_tab.compare_right_ref = tab.compare->right_ref;
  persisted_tab.compare_right_label = tab.compare->right_label;
  persisted_tab.compare_selected_row = tab.compare->selected_row;
  persisted_tab.compare_scroll_row = static_cast<std::size_t>(std::max(0, tab.compare->scroll_row));
  persisted_tab.compare_horizontal_scroll = tab.compare->horizontal_scroll;
  return persisted_tab;
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedMergeTabState(
    const TabEntry& tab) const {
  if (tab.kind != TabEntry::Kind::Merge || !tab.merge.has_value() ||
      !tab.merge->persistable) {
    return std::nullopt;
  }

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "merge";
  persisted_tab.merge_base_path = tab.merge->base_path.lexically_normal();
  persisted_tab.merge_incoming_path = tab.merge->incoming_path.lexically_normal();
  persisted_tab.merge_current_path = tab.merge->current_path.lexically_normal();
  persisted_tab.merge_output_path = tab.merge->output_path.lexically_normal();
  persisted_tab.merge_selected_hunk = tab.merge->selected_hunk;
  persisted_tab.merge_scroll_row = static_cast<std::size_t>(std::max(0, tab.merge->scroll_row));
  persisted_tab.merge_horizontal_scroll = tab.merge->horizontal_scroll;
  persisted_tab.merge_left_divider_fraction = tab.merge->left_divider_fraction;
  persisted_tab.merge_right_divider_fraction = tab.merge->right_divider_fraction;
  persisted_tab.merge_hunk_choices.reserve(tab.merge->model.hunks.size());
  for (const auto& hunk : tab.merge->model.hunks) {
    persisted_tab.merge_hunk_choices.push_back(compare::MergeChoiceLabel(hunk.choice));
  }
  return persisted_tab;
}

std::optional<PersistedEditorTabState>
PersistenceCoordinator::BuildPersistedEditorTabState(std::size_t /*tab_index*/,
                                                     TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      tab.editor_state->views.empty()) {
    return std::nullopt;
  }

  auto& editor_state = tab.editor_state.value();
  operations_.normalize_editor_split_tree(editor_state);

  PersistedEditorTabState persisted_tab;
  persisted_tab.kind = "editor";
  persisted_tab.active_leaf_id = editor_state.active_leaf_id;
  for (const auto& view : editor_state.views) {
    const editor::TextViewport* persisted_viewport = &view.viewport;
    const std::filesystem::path normalized_path =
        view.needs_restore ? view.restored_path.lexically_normal()
                           : persisted_viewport->path().lexically_normal();
    const bool dirty_snapshot = !view.needs_restore && persisted_viewport->dirty();
    if (normalized_path.empty()) {
      if (!dirty_snapshot) {
        continue;
      }
    } else if (view.needs_restore && !dirty_snapshot) {
      continue;
    }
    const std::size_t cursor_line =
        view.needs_restore ? view.restored_cursor_line : persisted_viewport->cursor_line();
    const std::size_t cursor_column =
        view.needs_restore ? view.restored_cursor_column : persisted_viewport->cursor_column();
    const std::size_t scroll_line =
        view.needs_restore ? view.restored_scroll_line : persisted_viewport->scroll_line();
    const std::size_t horizontal_scroll =
        view.needs_restore ? view.restored_horizontal_scroll
                           : persisted_viewport->horizontal_scroll();
    persisted_tab.views.push_back(PersistedEditorViewState{
        .leaf_id = view.leaf_id,
        .path = normalized_path,
        .cursor_line = cursor_line,
        .cursor_column = cursor_column,
        .scroll_line = scroll_line,
        .horizontal_scroll = horizontal_scroll,
        .dirty_snapshot = dirty_snapshot,
        .line_ending = persisted_viewport->line_ending(),
        .buffer_lines = dirty_snapshot ? persisted_viewport->lines() : std::vector<std::string>{},
    });
  }

  std::vector<std::size_t> node_path;
  const auto collect_split_node =
      [&](auto&& self,
          const TabEntry::EditorTabState::EditorSplitNode* node) -> void {
    if (node == nullptr) {
      return;
    }

    std::string orientation = "leaf";
    if (!node->IsLeaf()) {
      orientation = node->orientation == EditorSplitOrientation::Horizontal ? "horizontal"
                                                                            : "vertical";
    }
    persisted_tab.split_nodes.push_back(PersistedSplitNodeState{
        .path = node_path,
        .orientation = orientation,
        .size_fraction = node->size_fraction,
        .leaf_id = node->leaf_id,
    });
    for (std::size_t child_index = 0; child_index < node->children.size(); ++child_index) {
      node_path.push_back(child_index);
      self(self, node->children[child_index].get());
      node_path.pop_back();
    }
  };
  collect_split_node(collect_split_node, editor_state.split_root.get());
  return persisted_tab;
}

}  // namespace microide::workspace
