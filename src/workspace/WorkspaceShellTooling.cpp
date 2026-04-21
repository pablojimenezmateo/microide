#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

#include "editor/RuntimeSyntaxRegistry.h"
#include "workspace/WorkspaceCommandParsing.h"

namespace microide::workspace {

namespace {

std::string GenerateRuntimeMessageId(std::string_view prefix) {
  static std::uint64_t counter = 1;
  return std::string(prefix) + "-" + std::to_string(counter++);
}

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

std::vector<std::string> ParseAgentCommand(std::string_view endpoint) {
  const ParsedCommandLine parsed = ParseCommandLine(endpoint);
  std::vector<std::string> argv;
  argv.reserve(parsed.tokens.size());
  for (const auto& token : parsed.tokens) {
    argv.push_back(token.text);
  }
  return argv;
}

std::string_view LineAtOrEmpty(const std::vector<std::string>& lines, std::size_t index) {
  return index < lines.size() ? std::string_view(lines[index]) : std::string_view{};
}

std::string OutputChannelIdForTask(const TaskSpec& spec) {
  return "task." + spec.id;
}

std::string DetectActiveLanguageId(const WorkspaceShell& shell,
                                   const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

editor::SelectionRange CompletionReplacementRange(const editor::TextViewport& viewport) {
  const std::string_view line = LineAtOrEmpty(viewport.lines(), viewport.cursor_line());
  std::size_t start_column = std::min(viewport.cursor_column(), line.size());
  while (start_column > 0) {
    const char ch = line[start_column - 1];
    const bool identifier_char =
        (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
        ch == '_' || ch == '.' || ch == '/' || ch == '-';
    if (!identifier_char) {
      break;
    }
    --start_column;
  }
  return editor::SelectionRange{
      .start = editor::TextPosition{viewport.cursor_line(), start_column},
      .end = editor::TextPosition{viewport.cursor_line(), viewport.cursor_column()},
  };
}

const ExternalAgentSpec* SelectAgentForCapability(const ExternalAgentRegistry& registry,
                                                  const std::string& capability) {
  if (const AgentSelection* selection = registry.GetSelection(capability);
      selection != nullptr && !selection->preferred_agent.empty()) {
    if (const auto* agent = registry.FindAgent(selection->preferred_agent); agent != nullptr) {
      return agent;
    }
  }
  const auto agents = registry.FindByCapability(capability);
  return agents.empty() ? nullptr : agents.front();
}

void UpdateMessageContent(Conversation* conversation,
                          std::string_view message_id,
                          std::string content) {
  if (conversation == nullptr) {
    return;
  }
  for (auto& message : conversation->messages) {
    if (message.id == message_id) {
      message.content = std::move(content);
      message.timestamp = CurrentUtcTimestamp();
      return;
    }
  }
}

}  // namespace

const std::vector<WorkspaceOutputChannels::ChannelInfo>& WorkspaceShell::OutputChannels() const {
  return output_channels_.Channels();
}

const std::vector<std::string>* WorkspaceShell::OutputChannelEntries(std::string_view id) const {
  return output_channels_.Entries(id);
}

void WorkspaceShell::ShowOutputChannel(std::string_view id) {
  const std::string channel_id =
      id.empty() ? (context_.current_project_state.panel.output.channel_id.empty()
                        ? std::string("plugins.log")
                        : context_.current_project_state.panel.output.channel_id)
                 : std::string(id);
  output_channels_.EnsureChannel(channel_id, channel_id);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestBottomPanelRedraw();
}

void WorkspaceShell::ShowChatPanel() {
  if (context_.current_project_state.panel.chat.conversation_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        conversation_registry_.CreateConversation("Chat", std::string{});
  }
  context_.current_project_state.panel.content = PanelContentKind::Chat;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestBottomPanelRedraw();
}

void WorkspaceShell::ConsumeTaskRuntimeUpdates() {
  const std::optional<WorkspaceTaskRuntime::TaskUpdate> update =
      task_runtime_.ConsumeActiveUpdate();
  if (!update.has_value()) {
    return;
  }

  output_channels_.EnsureChannel(update->channel_id, update->channel_label);
  for (const std::string& line : update->appended_lines) {
    output_channels_.AppendLine(update->channel_id, update->channel_label, line);
  }
  if (update->finished && !update->status_text.empty()) {
    output_channels_.AppendLine(update->channel_id, update->channel_label, update->status_text);
  }
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = update->channel_id;
  RequestBottomPanelRedraw();
}

bool WorkspaceShell::ShowTaskPickerOverlay() {
  context_.current_project_state.overlay.workflow.task_picker.entries.clear();
  context_.current_project_state.overlay.workflow.task_picker.error.clear();
  context_.current_project_state.overlay.workflow.task_picker.selected_index = 0;
  for (const TaskSpec& task : task_registry_.Specs()) {
    context_.current_project_state.overlay.workflow.task_picker.entries.push_back(TaskPickerEntry{
        .id = task.id,
        .label = task.label,
        .group = task.group,
    });
  }
  if (context_.current_project_state.overlay.workflow.task_picker.entries.empty()) {
    context_.current_project_state.overlay.workflow.task_picker.error = "No tasks registered";
  }
  ShowOverlay(OverlayMode::TaskPicker);
  return true;
}

bool WorkspaceShell::RunSelectedTask() {
  if (context_.current_project_state.overlay.workflow.task_picker.entries.empty()) {
    return false;
  }
  const TaskPickerEntry& selected =
      context_.current_project_state.overlay.workflow.task_picker.entries[std::min(
          context_.current_project_state.overlay.workflow.task_picker.selected_index,
          context_.current_project_state.overlay.workflow.task_picker.entries.size() - 1)];
  const TaskSpec* spec = task_registry_.FindTask(selected.id);
  if (spec == nullptr) {
    return false;
  }
  return RunTaskById(spec->id, nullptr);
}

bool WorkspaceShell::RunTaskById(std::string_view id, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const TaskSpec* spec = task_registry_.FindTask(std::string(id));
  if (spec == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown task: " + std::string(id);
    }
    return false;
  }
  task_runtime_.Start(*spec, context_.current_project_state.root);
  const std::string channel_id = OutputChannelIdForTask(*spec);
  output_channels_.EnsureChannel(channel_id, spec->label.empty() ? spec->id : spec->label);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  DismissOverlay(false);
  RequestBottomPanelRedraw();
  return true;
}

bool WorkspaceShell::ShowCompletionOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*this, *viewport);
  std::string provider_error;
  const auto items =
      plugin_runtime_.Host().QueryCompletions(language_id, viewport->path(),
                                              viewport->cursor_line() + 1,
                                              viewport->cursor_column() + 1, {}, &provider_error);
  context_.current_project_state.overlay.workflow.completion.items.clear();
  context_.current_project_state.overlay.workflow.completion.selected_index = 0;
  context_.current_project_state.overlay.workflow.completion.replacement_range =
      CompletionReplacementRange(*viewport);
  context_.current_project_state.overlay.workflow.completion.source = "plugin";
  context_.current_project_state.overlay.workflow.completion.error = provider_error;
  for (const auto& item : items) {
    context_.current_project_state.overlay.workflow.completion.items.push_back(
        CompletionSessionItem{
            .label = item.label,
            .detail = item.detail,
            .documentation = item.documentation,
            .insert_text = item.insert_text,
        });
  }
  if (context_.current_project_state.overlay.workflow.completion.items.empty()) {
    if (error_message != nullptr) {
      *error_message = provider_error.empty() ? "No completions available" : provider_error;
    }
    return false;
  }
  ShowOverlay(OverlayMode::Completion);
  return true;
}

bool WorkspaceShell::ApplySelectedCompletion() {
  auto& session = context_.current_project_state.overlay.workflow.completion;
  if (session.items.empty()) {
    return false;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }

  const CompletionSessionItem& item =
      session.items[std::min(session.selected_index, session.items.size() - 1)];
  const bool was_dirty = viewport->dirty();
  const std::vector<std::string> before_lines = viewport->lines();
  const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
  const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
  if (!viewport->ReplaceRange(session.replacement_range, item.insert_text)) {
    return false;
  }
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, cursor_before);
  }
  ResetCaretBlink();
  RequestActiveEditableChangeRedraw(before_lines, viewport->lines());
  RequestFocusedEditorRedraw();
  if (viewport->dirty() != was_dirty) {
    RequestActiveEditableBlameNeighborhoodRedraw(cursor_before.line, viewport->cursor_line());
    RequestTabStripRedraw();
  }
  DismissOverlay(true);
  return true;
}

bool WorkspaceShell::ShowCodeActionsOverlay(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*this, *viewport);
  const std::optional<editor::SelectionRange> selection = viewport->selection_range();
  const editor::SelectionRange range = selection.value_or(editor::SelectionRange{
      .start = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
      .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
  });
  std::string provider_error;
  const auto items = plugin_runtime_.Host().QueryCodeActions(
      language_id, viewport->path(), range.start.line + 1, range.start.column + 1,
      range.end.line + 1, range.end.column + 1, &provider_error);
  auto& session = context_.current_project_state.overlay.workflow.code_actions;
  session.items.clear();
  session.selected_index = 0;
  session.source = "plugin";
  session.error = provider_error;
  for (const auto& item : items) {
    session.items.push_back(CodeActionSessionItem{
        .title = item.title,
        .command = item.command,
        .arguments = item.arguments,
    });
  }
  if (session.items.empty()) {
    if (error_message != nullptr) {
      *error_message = provider_error.empty() ? "No code actions available" : provider_error;
    }
    return false;
  }
  ShowOverlay(OverlayMode::CodeActions);
  return true;
}

bool WorkspaceShell::ExecuteSelectedCodeAction() {
  auto& session = context_.current_project_state.overlay.workflow.code_actions;
  if (session.items.empty()) {
    return false;
  }
  const CodeActionSessionItem& action =
      session.items[std::min(session.selected_index, session.items.size() - 1)];
  if (action.command.empty()) {
    return false;
  }
  std::string error_message;
  const bool executed =
      ExecuteCommandName(action.command, action.arguments, ActionSource::Command, &error_message);
  if (executed) {
    DismissOverlay(true);
  } else {
    session.error = error_message;
    RequestOverlayRedraw();
  }
  return executed;
}

bool WorkspaceShell::DiscoverTestsForActiveBuffer(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*this, *viewport);
  const auto provider_it =
      std::find_if(plugin_runtime_.Host().ContributedTestProviders().begin(),
                   plugin_runtime_.Host().ContributedTestProviders().end(),
                   [&](const auto& provider) { return provider.language_id == language_id; });
  if (provider_it == plugin_runtime_.Host().ContributedTestProviders().end()) {
    if (error_message != nullptr) {
      *error_message = "No test provider registered";
    }
    return false;
  }

  std::vector<plugin::PluginHost::TestCase> discovered;
  std::string provider_error;
  if (!plugin_runtime_.Host().DiscoverTests(provider_it->id, viewport->path(), &discovered,
                                            &provider_error)) {
    if (error_message != nullptr) {
      *error_message = provider_error;
    }
    return false;
  }

  test_controller_.Clear();
  context_.current_project_state.sidebar.tests.entries.clear();
  context_.current_project_state.sidebar.tests.provider_id = provider_it->id;
  context_.current_project_state.sidebar.tests.error.clear();
  for (const auto& test : discovered) {
    test_controller_.RegisterTestItem(TestItem{
        .id = test.id,
        .label = test.label,
        .file = test.file.string(),
        .line = test.line,
        .parent_id = test.parent_id,
    });
  }
  ShowTestsSidebar();
  RefreshTestsSidebar();
  return true;
}

bool WorkspaceShell::RunTests(const std::vector<std::string>& test_ids, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (context_.current_project_state.sidebar.tests.provider_id.empty()) {
    if (error_message != nullptr) {
      *error_message = "No active test provider";
    }
    return false;
  }

  std::vector<plugin::PluginHost::TestRunResult> results;
  std::string provider_error;
  if (!plugin_runtime_.Host().RunTests(context_.current_project_state.sidebar.tests.provider_id,
                                       test_ids, &results, &provider_error)) {
    if (error_message != nullptr) {
      *error_message = provider_error;
    }
    return false;
  }

  for (const auto& result : results) {
    TestResultState state = TestResultState::Queued;
    if (result.state == "passed") {
      state = TestResultState::Passed;
    } else if (result.state == "failed") {
      state = TestResultState::Failed;
    } else if (result.state == "skipped") {
      state = TestResultState::Skipped;
    } else if (result.state == "errored") {
      state = TestResultState::Errored;
    } else if (result.state == "running" || result.state == "in_progress") {
      state = TestResultState::InProgress;
    }
    test_controller_.RecordTestResult(TestResult{
        .test_id = result.test_id,
        .state = state,
        .message = result.message,
        .duration_ms = result.duration_ms,
    });
  }
  RefreshTestsSidebar();
  return true;
}

bool WorkspaceShell::RunAllDiscoveredTests(std::string* error_message) {
  std::vector<std::string> test_ids;
  for (const TestItem& item : test_controller_.TestItems()) {
    test_ids.push_back(item.id);
  }
  if (test_ids.empty()) {
    if (error_message != nullptr) {
      *error_message = "No discovered tests";
    }
    return false;
  }
  return RunTests(test_ids, error_message);
}

bool WorkspaceShell::StartDebugger(std::string_view type, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (type.empty()) {
    if (error_message != nullptr) {
      *error_message = "Debugger type is required";
    }
    return false;
  }
  if (dap_manager_.GetDebugger(std::string(type)) == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown debugger: " + std::string(type);
    }
    return false;
  }
  context_.current_project_state.debug_session.running = true;
  context_.current_project_state.debug_session.type = std::string(type);
  context_.current_project_state.debug_session.channel_id = "debug." + std::string(type);
  context_.current_project_state.debug_session.status_text = "Running";
  output_channels_.AppendLine(context_.current_project_state.debug_session.channel_id,
                              "Debugger " + std::string(type),
                              "Debugger " + std::string(type) + " started");
  ShowOutputChannel(context_.current_project_state.debug_session.channel_id);
  return true;
}

void WorkspaceShell::StopDebugger() {
  if (context_.current_project_state.debug_session.running &&
      !context_.current_project_state.debug_session.channel_id.empty()) {
    output_channels_.AppendLine(context_.current_project_state.debug_session.channel_id,
                                "Debugger " + context_.current_project_state.debug_session.type,
                                "Debugger stopped");
  }
  dap_manager_.ShutdownAll();
  context_.current_project_state.debug_session = DebugSessionState{};
}

bool WorkspaceShell::LoginAuthProvider(std::string_view provider_id,
                                       const std::vector<std::string>& scopes,
                                       std::string* error_message) {
  plugin::PluginHost::AuthSessionData session;
  if (!plugin_runtime_.Host().LoginAuthProvider(provider_id, scopes, &session, error_message)) {
    return false;
  }
  auth_provider_registry_.AddSession(AuthSession{
      .id = session.id,
      .provider_id = std::string(provider_id),
      .account = session.account,
      .access_token = session.access_token,
      .scopes = session.scopes,
  });
  if (!session.access_token.empty()) {
    secret_storage_.Store(std::string(provider_id) + "." + session.id, session.access_token);
  }
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Logged in as " + session.account);
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

bool WorkspaceShell::RefreshAuthSession(std::string_view provider_id,
                                        std::string_view session_id,
                                        std::string* error_message) {
  plugin::PluginHost::AuthSessionData session;
  if (!plugin_runtime_.Host().RefreshAuthSession(provider_id, session_id, &session,
                                                 error_message)) {
    return false;
  }
  auth_provider_registry_.RemoveSession(std::string(session_id));
  auth_provider_registry_.AddSession(AuthSession{
      .id = session.id,
      .provider_id = std::string(provider_id),
      .account = session.account,
      .access_token = session.access_token,
      .scopes = session.scopes,
  });
  if (!session.access_token.empty()) {
    secret_storage_.Store(std::string(provider_id) + "." + session.id, session.access_token);
  }
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Refreshed session " + session.id);
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

bool WorkspaceShell::LogoutAuthSession(std::string_view provider_id,
                                       std::string_view session_id,
                                       std::string* error_message) {
  if (!plugin_runtime_.Host().LogoutAuthSession(provider_id, session_id, error_message)) {
    return false;
  }
  auth_provider_registry_.RemoveSession(std::string(session_id));
  secret_storage_.Delete(std::string(provider_id) + "." + std::string(session_id));
  output_channels_.AppendLine("auth." + std::string(provider_id), "Auth " + std::string(provider_id),
                              "Logged out session " + std::string(session_id));
  ShowOutputChannel("auth." + std::string(provider_id));
  RequestChromeRedraw();
  RequestSidebarRedraw();
  return true;
}

bool WorkspaceShell::StartChatRequest(std::string message, std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (message.empty()) {
    message = context_.current_project_state.panel.chat.composer;
  }
  if (message.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat message is empty";
    }
    return false;
  }

  const ExternalAgentSpec* agent = SelectAgentForCapability(external_agent_registry_, "chat");
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio chat agent is available";
    }
    return false;
  }
  const std::vector<std::string> command = ParseAgentCommand(agent->endpoint);
  if (command.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat agent endpoint is empty";
    }
    return false;
  }

  ShowChatPanel();
  Conversation* conversation =
      conversation_registry_.GetConversation(context_.current_project_state.panel.chat.conversation_id);
  if (conversation == nullptr) {
    context_.current_project_state.panel.chat.conversation_id =
        conversation_registry_.CreateConversation("Chat", std::string{});
    conversation =
        conversation_registry_.GetConversation(context_.current_project_state.panel.chat.conversation_id);
  }
  const std::string user_id = GenerateRuntimeMessageId("user");
  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  conversation->provider_id = agent->id;
  conversation_registry_.AddMessage(
      conversation->id,
      Message{
          .id = user_id,
          .role = MessageRole::User,
          .content = message,
          .timestamp = CurrentUtcTimestamp(),
          .model = {},
      });
  conversation_registry_.AddMessage(
      conversation->id,
      Message{
          .id = assistant_id,
          .role = MessageRole::Assistant,
          .content = {},
          .timestamp = CurrentUtcTimestamp(),
          .model = agent->id,
      });
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.status_text = "Waiting for " + agent->label;
  context_.current_project_state.panel.chat.composer.clear();

  std::string prompt = message;
  if (const std::optional<std::string> selection = SelectionTextWithContext();
      selection.has_value() && !selection->empty()) {
    prompt += "\n\nSelected context:\n" + *selection + "\n";
  }
  ai_runtime_.Start(WorkspaceAiRuntime::Request{
      .capability = "chat",
      .command = command,
      .cwd = context_.current_project_state.root,
      .stdin_text = prompt,
  });
  RequestBottomPanelRedraw();
  return true;
}

void WorkspaceShell::ConsumeAiRuntimeUpdates() {
  const std::optional<WorkspaceAiRuntime::Update> update = ai_runtime_.ConsumeActiveUpdate();
  if (!update.has_value()) {
    return;
  }

  if (context_.current_project_state.panel.chat.request_in_flight) {
    Conversation* conversation =
        conversation_registry_.GetConversation(context_.current_project_state.panel.chat.conversation_id);
    if (!context_.current_project_state.panel.chat.pending_assistant_message_id.empty()) {
      UpdateMessageContent(conversation,
                           context_.current_project_state.panel.chat.pending_assistant_message_id,
                           update->chunk);
    }
    if (update->finished) {
      context_.current_project_state.panel.chat.request_in_flight = false;
      context_.current_project_state.panel.chat.status_text = update->status_text;
      context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    }
    RequestBottomPanelRedraw();
    return;
  }

  if (context_.current_project_state.inline_completion.request_in_flight) {
    context_.current_project_state.inline_completion.request_in_flight = false;
    context_.current_project_state.inline_completion.text = update->chunk;
    context_.current_project_state.inline_completion.visible =
        update->succeeded && !update->chunk.empty();
    context_.current_project_state.inline_completion.error =
        update->succeeded ? std::string{} : update->status_text;
    RequestFocusedEditorRedraw();
  }
}

bool WorkspaceShell::RequestInlineCompletion(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }
  const ExternalAgentSpec* agent =
      SelectAgentForCapability(external_agent_registry_, "inline-completion");
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio inline completion agent is available";
    }
    return false;
  }
  const std::vector<std::string> command = ParseAgentCommand(agent->endpoint);
  if (command.empty()) {
    if (error_message != nullptr) {
      *error_message = "Inline completion agent endpoint is empty";
    }
    return false;
  }

  inline_completion_registry_.Clear();
  context_.current_project_state.inline_completion.visible = false;
  context_.current_project_state.inline_completion.request_in_flight = true;
  context_.current_project_state.inline_completion.start_line = viewport->cursor_line();
  context_.current_project_state.inline_completion.start_column = viewport->cursor_column();
  context_.current_project_state.inline_completion.provider_id = agent->id;
  context_.current_project_state.inline_completion.text.clear();
  context_.current_project_state.inline_completion.error.clear();

  std::ostringstream prompt;
  prompt << "Complete the code at line " << (viewport->cursor_line() + 1) << ", column "
         << (viewport->cursor_column() + 1) << ". Return only the completion text.\n\n";
  const auto& lines = viewport->lines();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    prompt << lines[i] << '\n';
  }
  ai_runtime_.Start(WorkspaceAiRuntime::Request{
      .capability = "inline-completion",
      .command = command,
      .cwd = context_.current_project_state.root,
      .stdin_text = prompt.str(),
  });
  return true;
}

bool WorkspaceShell::AcceptInlineCompletion() {
  if (!context_.current_project_state.inline_completion.visible ||
      context_.current_project_state.inline_completion.text.empty()) {
    return false;
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return false;
  }

  const std::vector<std::string> before_lines = viewport->lines();
  const std::optional<editor::SelectionRange> selection_before = viewport->selection_range();
  const editor::TextPosition cursor_before{viewport->cursor_line(), viewport->cursor_column()};
  viewport->ReplaceRange(
      editor::SelectionRange{
          .start = editor::TextPosition{
              context_.current_project_state.inline_completion.start_line,
              context_.current_project_state.inline_completion.start_column,
          },
          .end = editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
      },
      context_.current_project_state.inline_completion.text);
  if (auto* compare_tab = ActiveCompareTab();
      compare_tab != nullptr && viewport == &compare_tab->right_viewport) {
    RefreshCompareTabDerivedState(*compare_tab);
    SyncCompareSelectionFromViewport(*compare_tab, true);
  }
  if (auto* merge_tab = ActiveMergeTab();
      merge_tab != nullptr && viewport == &merge_tab->result_viewport) {
    UpdateMergeTrackingAfterViewportEdit(*merge_tab, before_lines, selection_before, cursor_before);
  }
  DismissInlineCompletion();
  RequestFocusedEditorRedraw();
  return true;
}

void WorkspaceShell::DismissInlineCompletion() {
  context_.current_project_state.inline_completion = InlineCompletionState{};
  RequestFocusedEditorRedraw();
}

bool WorkspaceShell::InvokeMcpTool(std::string_view tool_id,
                                   std::string_view input_json,
                                   std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const ToolPermissionLevel permission = mcp_tool_registry_.CheckPermission(std::string(tool_id), "*");
  if (permission == ToolPermissionLevel::Denied) {
    if (error_message != nullptr) {
      *error_message = "Tool access denied";
    }
    return false;
  }

  std::string output_json;
  if (!plugin_runtime_.Host().InvokeMcpTool(tool_id, input_json, &output_json, error_message)) {
    return false;
  }
  output_channels_.AppendLine("mcp." + std::string(tool_id), std::string(tool_id), output_json);
  ShowOutputChannel("mcp." + std::string(tool_id));
  return true;
}

}  // namespace microide::workspace
