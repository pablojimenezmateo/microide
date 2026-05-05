#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>


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

std::string JoinSummaryParts(const std::vector<std::string>& parts) {
  std::string result;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!result.empty()) {
      result += " · ";
    }
    result += part;
  }
  return result;
}

std::string ContextPolicySummary(const ContextPolicy& policy,
                                 const std::vector<ContextItem>& items) {
  std::vector<std::string> parts;
  parts.push_back(std::to_string(items.size()) + " item" + (items.size() == 1 ? "" : "s"));
  parts.push_back(std::to_string(policy.max_total_bytes / 1024) + "KB max");
  parts.push_back(std::to_string(policy.max_files) + " files");
  if (policy.include_diagnostics) {
    parts.push_back("diagnostics");
  }
  if (policy.include_git_context) {
    parts.push_back("git");
  }
  if (policy.include_project_structure) {
    parts.push_back("structure");
  }
  return JoinSummaryParts(parts);
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

}  // namespace

void WorkspaceShell::ShowPendingToolApprovalPrompt(ProjectWorkspaceState& project) {
  if (!project.panel.chat.pending_tool_approval.has_value()) {
    return;
  }
  const auto& pending = *project.panel.chat.pending_tool_approval;
  const std::string args_detail =
      pending.arguments_summary.empty() ? std::string("No arguments")
                                        : pending.arguments_summary;
  const std::string context_detail =
      ContextPolicySummary(project.panel.chat.active_request.context_policy,
                           project.panel.chat.active_request.context_items);

  RequestPromptRedraw();
  context_.prompts.surface_visible = true;
  context_.prompts.surface_previous_focus = project.surface.focus;
  context_.prompts.surface.kind = PromptSurfaceState::Kind::Confirm;
  context_.prompts.surface.action = PromptSurfaceState::Action::ApproveChatTool;
  context_.prompts.surface.path = project.root;
  context_.prompts.surface.input.SetText({});
  context_.prompts.surface.detail =
      JoinSummaryParts({pending.capability_scope.empty() ? pending.tool_id : pending.capability_scope,
                        "Args: " + args_detail, context_detail});
  context_.prompts.surface.provider_id = pending.provider_id;
  context_.prompts.surface.request_id = pending.request_id;
  context_.prompts.surface.tool_call_id = pending.tool_call_id;
  context_.prompts.surface.tool_id = pending.display_name.empty() ? pending.tool_id
                                                                  : pending.display_name;
  context_.prompts.surface.capability_scope = pending.capability_scope;
  context_.prompts.surface.button_count = 3;
  context_.prompts.surface.selected_button = 0;
  project.surface.focus = FocusTarget::Overlay;
  RequestPromptRedraw();
}

bool WorkspaceShell::ResolveChatToolApprovalPrompt(bool allow, bool remember_for_session) {
  if (!context_.prompts.surface_visible ||
      context_.prompts.surface.action != PromptSurfaceState::Action::ApproveChatTool) {
    return false;
  }

  ProjectWorkspaceState* project = nullptr;
  if (context_.current_project_state.root == context_.prompts.surface.path) {
    project = &context_.current_project_state;
  } else {
    for (const auto& entry : context_.project_catalog.entries) {
      if (entry != nullptr && entry->root == context_.prompts.surface.path) {
        project = entry.get();
        break;
      }
    }
  }
  if (project == nullptr || !project->panel.chat.pending_tool_approval.has_value()) {
    DismissPromptSurface(true);
    return false;
  }

  ChatPanelState& chat = project->panel.chat;
  const auto pending = *chat.pending_tool_approval;
  if (pending.provider_id != context_.prompts.surface.provider_id ||
      pending.request_id != context_.prompts.surface.request_id ||
      pending.tool_call_id != context_.prompts.surface.tool_call_id) {
    DismissPromptSurface(true);
    return false;
  }

  Conversation* conversation = project->conversations.GetConversation(pending.conversation_id);
  Message* assistant = FindMessage(conversation, pending.assistant_message_id);
  ToolEvent* tool_event = FindToolEvent(assistant, pending.tool_call_id);
  const Uint64 now = SDL_GetTicks();

  if (!allow) {
    ai_provider_runtime_service_.SendToolDenied(pending.provider_id, pending.request_id,
                                                pending.tool_call_id, "Tool approval denied");
    if (tool_event != nullptr) {
      tool_event->status = "Denied";
      tool_event->permission_decision = "denied";
      tool_event->finished_at = CurrentUtcTimestamp();
      tool_event->duration_ms = pending.requested_ticks == 0
                                    ? 0
                                    : static_cast<std::int64_t>(now - pending.requested_ticks);
      tool_event->error = "Tool approval denied";
    }
    chat.status_text = "Tool approval denied";
  } else {
    if (remember_for_session) {
      const auto it = std::find_if(
          chat.remembered_tool_approvals.begin(), chat.remembered_tool_approvals.end(),
          [&](const ChatPanelState::RememberedToolApproval& approval) {
            return approval.capability_scope == pending.capability_scope;
          });
      if (it == chat.remembered_tool_approvals.end()) {
        chat.remembered_tool_approvals.push_back(ChatPanelState::RememberedToolApproval{
            .capability_scope = pending.capability_scope,
            .tool_id = pending.tool_id,
            .display_name = pending.display_name,
            .granted_at_ticks = now,
        });
      }
    }

    if (tool_event != nullptr) {
      tool_event->status = "Running";
      tool_event->permission_decision = remember_for_session ? "session" : "approved";
    }

    std::string output_json;
    std::string error_message;
    const bool succeeded = plugin_runtime_.Host().InvokeMcpTool(
        pending.tool_id, pending.arguments_json, &output_json, &error_message);
    if (succeeded) {
      ai_provider_runtime_service_.SendToolResult(pending.provider_id, pending.request_id,
                                                  pending.tool_call_id, output_json);
      if (tool_event != nullptr) {
        tool_event->status = "Completed";
        tool_event->finished_at = CurrentUtcTimestamp();
        tool_event->duration_ms = pending.requested_ticks == 0
                                      ? 0
                                      : static_cast<std::int64_t>(now - pending.requested_ticks);
        tool_event->output_summary = ToolOutputSummary(output_json);
      }
      chat.status_text = "Tool completed";
    } else {
      const std::string denied_reason =
          error_message.empty() ? std::string("Tool execution failed") : error_message;
      ai_provider_runtime_service_.SendToolDenied(pending.provider_id, pending.request_id,
                                                  pending.tool_call_id, denied_reason);
      if (tool_event != nullptr) {
        tool_event->status = "Failed";
        tool_event->finished_at = CurrentUtcTimestamp();
        tool_event->duration_ms = pending.requested_ticks == 0
                                      ? 0
                                      : static_cast<std::int64_t>(now - pending.requested_ticks);
        tool_event->error = denied_reason;
      }
      chat.status_text = denied_reason;
    }
  }

  chat.pending_tool_approval.reset();
  DismissPromptSurface(true);
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::ExpirePendingToolApprovals() {
  const Uint64 now = SDL_GetTicks();
  auto expire_project = [&](ProjectWorkspaceState& project) {
    ChatPanelState& chat = project.panel.chat;
    if (!chat.pending_tool_approval.has_value() ||
        chat.pending_tool_approval->expires_at_ticks == 0 ||
        now < chat.pending_tool_approval->expires_at_ticks) {
      return;
    }

    const auto pending = *chat.pending_tool_approval;
    Conversation* conversation = project.conversations.GetConversation(pending.conversation_id);
    Message* assistant = FindMessage(conversation, pending.assistant_message_id);
    if (ToolEvent* event = FindToolEvent(assistant, pending.tool_call_id); event != nullptr) {
      event->status = "Denied";
      event->permission_decision = "expired";
      event->finished_at = CurrentUtcTimestamp();
      event->duration_ms = pending.requested_ticks == 0
                               ? 0
                               : static_cast<std::int64_t>(now - pending.requested_ticks);
      event->error = "Tool approval timed out";
    }
    ai_provider_runtime_service_.SendToolDenied(pending.provider_id, pending.request_id,
                                                pending.tool_call_id, "Tool approval timed out");
    chat.pending_tool_approval.reset();
    if (context_.prompts.surface_visible &&
        context_.prompts.surface.action == PromptSurfaceState::Action::ApproveChatTool &&
        context_.prompts.surface.provider_id == pending.provider_id &&
        context_.prompts.surface.request_id == pending.request_id &&
        context_.prompts.surface.tool_call_id == pending.tool_call_id) {
      DismissPromptSurface(true);
    }
    chat.status_text = "Tool approval timed out";
  };

  expire_project(context_.current_project_state);
  for (const auto& entry : context_.project_catalog.entries) {
    if (entry != nullptr) {
      expire_project(*entry);
    }
  }
}


}  // namespace microide::workspace
