#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <limits>

#include "util/SingleLineText.h"
#include "workspace/WorkspaceTextSearch.h"

namespace microide::workspace {

namespace {

constexpr Uint64 kToolApprovalTimeoutMs = 30000;

std::string CurrentUtcTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    return {};
  }
  return buffer;
}

std::string CollapseWhitespaceForSummary(std::string_view text) {
  std::string out;
  bool in_space = false;
  for (char ch : text) {
    const bool whitespace = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (whitespace) {
      if (!in_space && !out.empty()) {
        out.push_back(' ');
      }
      in_space = true;
      continue;
    }
    in_space = false;
    out.push_back(ch);
  }
  return out;
}

std::string TruncateSummary(std::string text, std::size_t max_length = 160) {
  if (text.size() <= max_length) {
    return text;
  }
  text.resize(max_length - 3);
  text += "...";
  return text;
}

std::string ToolOutputSummary(std::string_view output_json) {
  return TruncateSummary(CollapseWhitespaceForSummary(output_json));
}

void UpdateMessageContent(Conversation* conversation,
                          std::string_view message_id,
                          std::string content) {
  if (conversation == nullptr) {
    return;
  }
  const auto build_render_line = [](MessageRole role, std::string_view raw_content) {
    const std::string_view prefix =
        role == MessageRole::Assistant ? std::string_view{"Assistant"}
        : role == MessageRole::User   ? std::string_view{"You"}
                                       : std::string_view{"System"};
    const std::string collapsed = CollapseWhitespace(raw_content);
    std::string line;
    line.reserve(prefix.size() + 2 + collapsed.size());
    line += prefix;
    line += ": ";
    line += collapsed;
    return line;
  };
  for (auto& message : conversation->messages) {
    if (message.id == message_id) {
      message.content = std::move(content);
      message.render_line = build_render_line(message.role, message.content);
      message.timestamp = CurrentUtcTimestamp();
      return;
    }
  }
}

ToolEvent* FindToolEvent(Message* message, std::string_view call_id) {
  if (message == nullptr) {
    return nullptr;
  }
  for (auto& event : message->tool_events) {
    if (event.call_id == call_id) {
      return &event;
    }
  }
  return nullptr;
}

Message* FindMessage(Conversation* conversation, std::string_view message_id) {
  if (conversation == nullptr) {
    return nullptr;
  }
  for (auto& message : conversation->messages) {
    if (message.id == message_id) {
      return &message;
    }
  }
  return nullptr;
}

RequestStatus ParseBridgeTerminalStatus(std::string_view status_text) {
  if (status_text == "succeeded") {
    return RequestStatus::Succeeded;
  }
  if (status_text == "cancelled") {
    return RequestStatus::Cancelled;
  }
  return RequestStatus::Failed;
}

std::string RequestStatusText(RequestStatus status) {
  switch (status) {
    case RequestStatus::Idle: return "Idle";
    case RequestStatus::Queued: return "Queued";
    case RequestStatus::Running: return "Running";
    case RequestStatus::Streaming: return "Streaming";
    case RequestStatus::Succeeded: return "Succeeded";
    case RequestStatus::Failed: return "Failed";
    case RequestStatus::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

std::string ToolPermissionDecisionLabel(ToolPermissionLevel permission) {
  switch (permission) {
    case ToolPermissionLevel::PromptRequired:
      return "approved";
    case ToolPermissionLevel::Allowed:
      return "policy_allow";
    case ToolPermissionLevel::AllowedWithinContext:
      return "policy_allow_context";
    case ToolPermissionLevel::Denied:
      return "denied";
  }
  return "approved";
}

ProjectWorkspaceState* FindProjectForChatRequest(WorkspaceContext& context,
                                                 std::string_view agent_id,
                                                 std::string_view request_id,
                                                 bool* active_project) {
  if (context.current_project_state.panel.chat.request_in_flight &&
      context.current_project_state.panel.chat.pending_bridge_agent_id == agent_id &&
      context.current_project_state.panel.chat.pending_bridge_request_id == request_id) {
    if (active_project != nullptr) {
      *active_project = true;
    }
    return &context.current_project_state;
  }

  for (std::size_t i = 0; i < context.project_catalog.entries.size(); ++i) {
    ProjectWorkspaceState* project = context.project_catalog.entries[i].get();
    if (project == nullptr) {
      continue;
    }
    if (!project->panel.chat.request_in_flight ||
        project->panel.chat.pending_bridge_agent_id != agent_id ||
        project->panel.chat.pending_bridge_request_id != request_id) {
      continue;
    }
    if (active_project != nullptr) {
      *active_project = context.HasActiveProjectCatalogEntry() && i == context.project_catalog.active_index;
    }
    return project;
  }

  if (active_project != nullptr) {
    *active_project = false;
  }
  return nullptr;
}

}  // namespace

void WorkspaceShell::ConsumeProviderBridgeUpdates() {
  ExpirePendingToolApprovals();
  while (true) {
    const auto update = provider_bridge_manager_.ConsumeChatUpdate();
    if (!update.has_value()) {
      break;
    }
    bool handled = false;

    bool active_project = false;
    if (ProjectWorkspaceState* chat_project =
            FindProjectForChatRequest(context_, update->agent_id, update->request_id,
                                      &active_project);
        chat_project != nullptr) {
      ChatPanelState& chat = chat_project->panel.chat;
      Conversation* conversation =
          chat_project->conversations.GetConversation(chat.request_conversation_id);
      Message* assistant = FindMessage(conversation, chat.pending_assistant_message_id);

      if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Chunk) {
        if (assistant != nullptr) {
          UpdateMessageContent(conversation, chat.pending_assistant_message_id, update->chunk);
        }
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        RequestChromeRedraw();
        handled = true;
      } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::ToolCall) {
        if (assistant != nullptr) {
          const std::string arguments_summary =
              !update->arguments_summary.empty()
                  ? update->arguments_summary
                  : TruncateSummary(CollapseWhitespaceForSummary(update->arguments_json));
          ToolEvent* tool_event = FindToolEvent(assistant, update->tool_call_id);
          if (tool_event == nullptr) {
            assistant->tool_events.push_back(ToolEvent{
                .call_id = update->tool_call_id,
                .tool_id = update->tool_id,
                .display_name = !update->display_name.empty() ? update->display_name
                                                              : update->tool_id,
                .arguments_summary = arguments_summary,
                .status = "Pending approval",
                .permission_decision = "pending",
                .capability_scope = !update->capability_scope.empty() ? update->capability_scope
                                                                      : update->tool_id,
                .started_at = CurrentUtcTimestamp(),
                .finished_at = {},
                .duration_ms = 0,
                .error = {},
                .output_summary = {},
            });
            tool_event = &assistant->tool_events.back();
          }

          const ToolPermissionLevel permission =
              mcp_tool_registry_.CheckPermission(update->tool_id, update->agent_id);
          const std::string capability_scope =
              !update->capability_scope.empty() ? update->capability_scope : update->tool_id;
          const bool remembered = std::any_of(
              chat.remembered_tool_approvals.begin(), chat.remembered_tool_approvals.end(),
              [&](const ChatPanelState::RememberedToolApproval& approval) {
                return approval.capability_scope == capability_scope;
              });

          chat.pending_tool_approval = ChatPanelState::PendingToolApproval{
              .conversation_id = chat.request_conversation_id,
              .assistant_message_id = chat.pending_assistant_message_id,
              .bridge_agent_id = chat.pending_bridge_agent_id,
              .bridge_request_id = chat.pending_bridge_request_id,
              .tool_call_id = update->tool_call_id,
              .tool_id = update->tool_id,
              .display_name = !update->display_name.empty() ? update->display_name
                                                            : update->tool_id,
              .arguments_json = update->arguments_json,
              .arguments_summary = arguments_summary,
              .capability_scope = capability_scope,
              .requested_ticks = SDL_GetTicks(),
              .expires_at_ticks = SDL_GetTicks() + kToolApprovalTimeoutMs,
          };

          const ToolMode tool_mode = chat.active_request.tool_mode;
          const auto finish_tool_event = [&](std::string status,
                                             std::string decision,
                                             std::string error_message,
                                             std::string output_summary) {
            if (tool_event != nullptr) {
              tool_event->status = std::move(status);
              tool_event->permission_decision = std::move(decision);
              tool_event->finished_at = CurrentUtcTimestamp();
              tool_event->duration_ms =
                  static_cast<std::int64_t>(SDL_GetTicks() - chat.pending_tool_approval->requested_ticks);
              tool_event->error = std::move(error_message);
              tool_event->output_summary = std::move(output_summary);
            }
          };

          const auto deny_tool = [&](std::string reason, std::string decision) {
            provider_bridge_manager_.SendToolDenied(
                chat.pending_bridge_agent_id, chat.pending_bridge_request_id, update->tool_call_id,
                reason);
            finish_tool_event("Denied", std::move(decision), reason, {});
            chat.pending_tool_approval.reset();
            chat.status_text = reason;
          };

          const auto run_tool = [&](std::string decision) {
            if (tool_event != nullptr) {
              tool_event->status = "Running";
              tool_event->permission_decision = decision;
            }
            std::string output_json;
            std::string error_message;
            const bool succeeded = plugin_runtime_.Host().InvokeMcpTool(
                update->tool_id, update->arguments_json, &output_json, &error_message);
            if (!succeeded) {
              deny_tool(error_message.empty() ? std::string("Tool execution failed")
                                              : error_message,
                        "failed");
              return;
            }
            provider_bridge_manager_.SendToolResult(
                chat.pending_bridge_agent_id, chat.pending_bridge_request_id, update->tool_call_id,
                output_json);
            finish_tool_event("Completed", std::move(decision), {}, ToolOutputSummary(output_json));
            chat.pending_tool_approval.reset();
            chat.status_text = "Tool completed";
          };

          if (tool_mode == ToolMode::NoTools) {
            deny_tool("Tool use is disabled for this conversation", "disabled");
          } else if (permission == ToolPermissionLevel::Denied) {
            deny_tool("Tool access denied by host policy", "denied");
          } else if (remembered) {
            run_tool("session");
          } else if (tool_mode == ToolMode::Auto &&
                     (permission == ToolPermissionLevel::Allowed ||
                      permission == ToolPermissionLevel::AllowedWithinContext)) {
            run_tool(ToolPermissionDecisionLabel(permission));
          } else {
            chat.status_text = "Waiting for tool approval";
            if (active_project &&
                (!context_.prompts.surface_visible ||
                 context_.prompts.surface.action != PromptSurfaceState::Action::ApproveChatTool)) {
              ShowPendingToolApprovalPrompt(*chat_project);
            }
          }
        }
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        RequestChromeRedraw();
        handled = true;
      } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Done) {
        const RequestStatus terminal_status = ParseBridgeTerminalStatus(update->terminal_status);
        const std::int64_t duration_ms =
            chat.request_started_ticks == 0
                ? 0
                : static_cast<std::int64_t>(SDL_GetTicks() - chat.request_started_ticks);
        if (assistant != nullptr && !update->chunk.empty()) {
          UpdateMessageContent(conversation, chat.pending_assistant_message_id, update->chunk);
        }
        if (conversation != nullptr) {
          conversation->status = terminal_status;
          conversation->last_request_duration_ms = duration_ms;
        }
        if (assistant != nullptr) {
          assistant->status = terminal_status;
          assistant->request_duration_ms = duration_ms;
          if (terminal_status != RequestStatus::Succeeded && !update->status_text.empty()) {
            assistant->error = update->status_text;
          }
        }
        if (chat.pending_tool_approval.has_value() &&
            chat.pending_tool_approval->bridge_agent_id == update->agent_id &&
            chat.pending_tool_approval->bridge_request_id == update->request_id) {
          if (ToolEvent* tool_event =
                  FindToolEvent(assistant, chat.pending_tool_approval->tool_call_id);
              tool_event != nullptr && tool_event->finished_at.empty()) {
            tool_event->status = terminal_status == RequestStatus::Cancelled ? "Cancelled"
                                                                             : "Failed";
            tool_event->permission_decision = "cancelled";
            tool_event->finished_at = CurrentUtcTimestamp();
            tool_event->duration_ms =
                static_cast<std::int64_t>(SDL_GetTicks() - chat.pending_tool_approval->requested_ticks);
            if (tool_event->error.empty()) {
              tool_event->error =
                  !update->status_text.empty() ? update->status_text : std::string("Cancelled");
            }
          }
          chat.pending_tool_approval.reset();
        }
        if (context_.prompts.surface_visible &&
            context_.prompts.surface.action == PromptSurfaceState::Action::ApproveChatTool &&
            context_.prompts.surface.bridge_agent_id == update->agent_id &&
            context_.prompts.surface.bridge_request_id == update->request_id) {
          DismissPromptSurface(true);
        }
        chat.request_in_flight = false;
        chat.request_started_ticks = 0;
        chat.status_text = !update->status_text.empty() ? update->status_text
                                                        : RequestStatusText(terminal_status);
        chat.request_conversation_id.clear();
        chat.pending_assistant_message_id.clear();
        chat.pending_bridge_agent_id.clear();
        chat.pending_bridge_request_id.clear();
        chat.active_request = ChatPanelState::RequestSnapshot{};
        if (active_project) {
          chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        if (chat.status_text.empty()) {
          chat.status_text = RequestStatusText(terminal_status);
        }
        RequestChromeRedraw();
        handled = true;
      }
    } else {
      auto apply_inline_update = [&](ProjectWorkspaceState* project, bool active) {
        if (project == nullptr) {
          return false;
        }
        InlineCompletionState& inline_completion = project->inline_completion;
        if (!inline_completion.request_in_flight ||
            inline_completion.pending_bridge_agent_id != update->agent_id ||
            inline_completion.pending_bridge_request_id != update->request_id) {
          return false;
        }
        if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Chunk) {
          inline_completion.text += update->chunk;
        } else if (update->kind == WorkspaceProviderBridgeManager::ChatUpdate::Kind::Done) {
          inline_completion.text += update->chunk;
          inline_completion.request_in_flight = false;
          inline_completion.visible =
              update->terminal_status == "succeeded" && !inline_completion.text.empty();
          inline_completion.error =
              update->terminal_status == "succeeded" ? std::string{} : update->status_text;
          inline_completion.pending_bridge_agent_id.clear();
          inline_completion.pending_bridge_request_id.clear();
        }
        if (active) {
          RequestFocusedEditorRedraw();
        }
        return true;
      };

      if (apply_inline_update(&context_.current_project_state, true)) {
        handled = true;
      } else {
        for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
          if (context_.HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
            continue;
          }
          if (apply_inline_update(context_.project_catalog.entries[i].get(), false)) {
            handled = true;
            break;
          }
        }
      }
    }

    if (!handled) {
      RequestChromeRedraw();
    }
  }
}



}  // namespace microide::workspace
