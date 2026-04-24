#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>

#include "editor/RuntimeSyntaxRegistry.h"
#include "util/SingleLineText.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspaceTextSearch.h"

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

std::string_view LineAtOrEmpty(const std::vector<std::string>& lines, std::size_t index) {
  return index < lines.size() ? std::string_view(lines[index]) : std::string_view{};
}

std::string OutputChannelIdForTask(const TaskSpec& spec) {
  return "task." + spec.id;
}

std::string LspUnavailableMessage(const LspManager& manager,
                                  std::string_view language_id,
                                  std::string_view fallback) {
  if (!language_id.empty()) {
    const std::string id(language_id);
    if (manager.HasServer(id)) {
      const std::string detail = manager.LastServerError(id);
      if (!detail.empty()) {
        return "LSP startup failed for " + id + ": " + detail;
      }
      return "LSP startup failed for " + id;
    }
  }
  if (!fallback.empty()) {
    return std::string(fallback);
  }
  return "No language server available";
}

std::string DetectActiveLanguageId(const editor::TextViewport& viewport) {
  return editor::runtime_syntax::DetectFiletype(viewport.path(), viewport.lines());
}

std::string SerializeChatComposerViewport(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines(), util::LineEnding::LF);
}

void LoadChatComposerViewport(editor::TextViewport* viewport, std::string_view text) {
  if (viewport == nullptr) {
    return;
  }
  viewport->LoadContent(text, {});
  viewport->SetDirty(false);
  viewport->SetViewportSize(4, 40);
  const std::size_t last_line = viewport->line_count() > 0 ? viewport->line_count() - 1 : 0;
  viewport->MoveCursorTo(last_line, viewport->lines().empty() ? 0 : viewport->lines().back().size());
}

std::string NextConversationTitle(const ConversationRegistry& registry) {
  const std::size_t count = registry.conversations().size();
  return count == 0 ? "Chat" : "Chat " + std::to_string(count + 1);
}

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  return util::SerializeLines(viewport.lines(), viewport.line_ending());
}

bool IsUnreservedUriByte(unsigned char ch) {
  return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/';
}

std::string FileUriForPath(const std::filesystem::path& path) {
  const std::string raw = path.lexically_normal().generic_string();
  std::ostringstream encoded;
  encoded << "file://";
  for (unsigned char ch : raw) {
    if (IsUnreservedUriByte(ch)) {
      encoded << static_cast<char>(ch);
      continue;
    }
    encoded << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(ch) << std::nouppercase << std::dec;
  }
  return encoded.str();
}

std::optional<std::filesystem::path> PathFromFileUri(std::string_view uri) {
  static constexpr std::string_view kFileScheme = "file://";
  if (!uri.starts_with(kFileScheme)) {
    return std::nullopt;
  }

  std::string_view encoded = uri.substr(kFileScheme.size());
  if (encoded.starts_with("localhost/")) {
    encoded.remove_prefix(std::string_view("localhost").size());
  }

  std::string decoded;
  decoded.reserve(encoded.size());
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      const auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
      };
      const int hi = hex_value(encoded[i + 1]);
      const int lo = hex_value(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    decoded.push_back(encoded[i]);
  }
  if (decoded.empty()) {
    return std::nullopt;
  }
  return std::filesystem::path(decoded).lexically_normal();
}

editor::DiagnosticSeverity DiagnosticSeverityFromLsp(int severity) {
  switch (severity) {
    case 2:
      return editor::DiagnosticSeverity::Warning;
    case 3:
      return editor::DiagnosticSeverity::Info;
    case 4:
      return editor::DiagnosticSeverity::Hint;
    case 1:
    default:
      return editor::DiagnosticSeverity::Error;
  }
}

std::string JsonValueToArgumentString(const util::JsonValue& value) {
  return value.IsString() ? value.AsString() : util::SerializeJson(value);
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

bool AgentSupportsCapability(const ExternalAgentSpec* agent, std::string_view capability) {
  if (agent == nullptr) {
    return false;
  }
  return std::find(agent->capabilities.begin(), agent->capabilities.end(), capability) !=
         agent->capabilities.end();
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

}  // namespace

const std::vector<WorkspaceOutputChannels::ChannelInfo>& WorkspaceShell::OutputChannels() const {
  return output_channels_.Channels();
}

std::size_t WorkspaceShell::CountOpenBufferViews(const std::filesystem::path& path) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (normalized_path.empty()) {
    return 0;
  }

  std::size_t count = 0;
  for (const auto& tab : context_.current_project_state.open_tabs) {
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      for (const auto& view : tab.editor_state->views) {
        if (EditorViewPath(view) == normalized_path) {
          ++count;
        }
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
      if (tab.compare->right_editable &&
          tab.compare->right_viewport.path().lexically_normal() == normalized_path) {
        ++count;
      }
      continue;
    }
    if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() &&
        tab.merge->result_viewport.path().lexically_normal() == normalized_path) {
      ++count;
    }
  }
  return count;
}

bool WorkspaceShell::HasActiveCompletionProvider() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }
  const std::string language_id = DetectActiveLanguageId(*viewport);
  if (language_id.empty()) {
    return false;
  }
  return completion_registry_.FindProvider(language_id) != nullptr || lsp_manager_.HasServer(language_id);
}

bool WorkspaceShell::HasActiveCodeActionProvider() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }
  const std::string language_id = DetectActiveLanguageId(*viewport);
  if (language_id.empty()) {
    return false;
  }
  return code_action_registry_.FindProvider(language_id) != nullptr || lsp_manager_.HasServer(language_id);
}

bool WorkspaceShell::HasActiveDefinitionProvider() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }
  const std::string language_id = DetectActiveLanguageId(*viewport);
  return !language_id.empty() && lsp_manager_.HasServer(language_id);
}

bool WorkspaceShell::HasActiveReferencesProvider() const {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }
  const std::string language_id = DetectActiveLanguageId(*viewport);
  return !language_id.empty() && lsp_manager_.HasServer(language_id);
}

LspClient* WorkspaceShell::LspClientForViewport(const editor::TextViewport& viewport,
                                                std::string* language_id) {
  if (!lsp_manager_.HasRegisteredServers()) {
    if (language_id != nullptr) {
      language_id->clear();
    }
    return nullptr;
  }
  if (language_id != nullptr) {
    *language_id = DetectActiveLanguageId(viewport);
  }
  const std::string detected_language =
      language_id != nullptr ? *language_id : DetectActiveLanguageId(viewport);
  if (detected_language.empty()) {
    return nullptr;
  }

  LspClient* client = lsp_manager_.GetServer(detected_language);
  if (client == nullptr) {
    return nullptr;
  }
  client->SetDiagnosticsCallback([this](std::string uri,
                                        std::vector<LspClient::Diagnostic> diagnostics) {
    PublishLspDiagnostics(std::move(uri), std::move(diagnostics));
  });
  return client;
}

void WorkspaceShell::EnsureLspDocumentOpen(const editor::TextViewport& viewport,
                                           LspClient& client,
                                           std::string_view language_id) {
  if (viewport.path().empty() || language_id.empty()) {
    return;
  }
  const std::string uri = FileUriForPath(viewport.path());
  if (client.HasOpenDocument(uri)) {
    return;
  }
  client.DidOpen(uri, std::string(language_id), SerializeViewportText(viewport));
}

void WorkspaceShell::PublishLspDiagnostics(std::string uri,
                                           std::vector<LspClient::Diagnostic> diagnostics) {
  const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
  if (!path.has_value()) {
    return;
  }

  std::vector<editor::Diagnostic> converted;
  converted.reserve(diagnostics.size());
  for (const auto& diagnostic : diagnostics) {
    converted.push_back(editor::Diagnostic{
        .range =
            editor::SelectionRange{
                .start =
                    editor::TextPosition{
                        static_cast<std::size_t>(std::max(diagnostic.range.start.line, 0)),
                        static_cast<std::size_t>(std::max(diagnostic.range.start.character, 0)),
                    },
                .end =
                    editor::TextPosition{
                        static_cast<std::size_t>(std::max(diagnostic.range.end.line, 0)),
                        static_cast<std::size_t>(std::max(diagnostic.range.end.character, 0)),
                    },
            },
        .severity = DiagnosticSeverityFromLsp(diagnostic.severity),
        .message = diagnostic.message,
    });
  }

  if (context_.current_project_state.diagnostics_store.ReplaceForOwnerFile(
          "lsp", *path, std::move(converted))) {
    RefreshProblemsSidebar();
    RequestEditorSurfaceRedraw();
  }
}

void WorkspaceShell::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                    const std::vector<std::string>& after_lines) {
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);

  const std::string uri = FileUriForPath(viewport->path());
  const std::string full_text = util::SerializeLines(after_lines, viewport->line_ending());
  if (!client->SupportsIncrementalSync()) {
    client->DidChange(uri, full_text);
    return;
  }

  const std::size_t end_line = before_lines.empty() ? 0 : before_lines.size() - 1;
  const std::size_t end_column = before_lines.empty() ? 0 : before_lines.back().size();
  client->DidChangeIncremental(
      uri,
      LspClient::Range{
          .start = LspClient::Position{0, 0},
          .end =
              LspClient::Position{static_cast<int>(end_line), static_cast<int>(end_column)},
      },
      full_text);
}

const std::vector<std::string>* WorkspaceShell::OutputChannelEntries(std::string_view id) const {
  return output_channels_.Entries(id);
}

void WorkspaceShell::EnsureOutputChannelTabOpen(std::string_view channel_id) {
  if (channel_id.empty()) {
    return;
  }
  auto& tabs = context_.current_project_state.panel.output.open_channel_ids;
  if (std::find(tabs.begin(), tabs.end(), channel_id) == tabs.end()) {
    tabs.emplace_back(channel_id);
  }
}

void WorkspaceShell::CloseOutputChannelTab(std::string_view channel_id) {
  auto& tabs = context_.current_project_state.panel.output.open_channel_ids;
  const auto it = std::find(tabs.begin(), tabs.end(), channel_id);
  if (it == tabs.end()) {
    return;
  }
  const std::size_t closed_index = static_cast<std::size_t>(std::distance(tabs.begin(), it));
  tabs.erase(it);

  if (context_.current_project_state.panel.content != PanelContentKind::Output ||
      context_.current_project_state.panel.output.channel_id != channel_id) {
    return;
  }

  if (!tabs.empty()) {
    const std::size_t next_index = std::min(closed_index, tabs.size() - 1);
    context_.current_project_state.panel.output.channel_id = tabs[next_index];
    return;
  }

  if (ActiveTerminalTab() != nullptr) {
    context_.current_project_state.panel.content = PanelContentKind::Terminal;
    return;
  }

  context_.current_project_state.panel.content = PanelContentKind::None;
  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    context_.current_project_state.surface.focus = FocusTarget::Editor;
  }
}

void WorkspaceShell::ShowOutputChannel(std::string_view id) {
  const std::string channel_id =
      id.empty() ? (context_.current_project_state.panel.output.channel_id.empty()
                        ? std::string("plugins.log")
                        : context_.current_project_state.panel.output.channel_id)
                 : std::string(id);
  std::string channel_label = channel_id;
  for (const auto& channel : output_channels_.Channels()) {
    if (channel.id == channel_id) {
      channel_label = channel.label.empty() ? channel.id : channel.label;
      break;
    }
  }
  output_channels_.EnsureChannel(channel_id, channel_label);
  EnsureOutputChannelTabOpen(channel_id);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestBottomPanelRedraw();
}

void WorkspaceShell::ShowChatPanel() {
  if (context_.current_project_state.panel.chat.conversation_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
  }
  LoadChatComposerDraft();
  context_.current_project_state.sidebar.view_id = "chat";
  context_.current_project_state.sidebar.visible = true;
  context_.current_project_state.sidebar.temporary = false;
  context_.current_project_state.sidebar.prev_view_id.clear();
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  context_.current_project_state.surface.focus = FocusTarget::Sidebar;
  RequestSidebarRedraw();
}

Conversation* WorkspaceShell::ActiveConversation() {
  return context_.current_project_state.conversations.GetConversation(
      context_.current_project_state.panel.chat.conversation_id);
}

const Conversation* WorkspaceShell::ActiveConversation() const {
  return context_.current_project_state.conversations.GetConversation(
      context_.current_project_state.panel.chat.conversation_id);
}

bool WorkspaceShell::ActivateChatConversation(std::string_view conversation_id) {
  if (conversation_id.empty() ||
      context_.current_project_state.conversations.GetConversation(std::string(conversation_id)) ==
          nullptr) {
    return false;
  }
  SyncActiveConversationDraft();
  context_.current_project_state.panel.chat.conversation_id = std::string(conversation_id);
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::SyncActiveConversationDraft() {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  conversation->draft = ChatComposerText();
}

void WorkspaceShell::LoadChatComposerDraft() {
  const Conversation* conversation = ActiveConversation();
  LoadChatComposerViewport(&context_.current_project_state.panel.chat.composer,
                          conversation != nullptr ? std::string_view(conversation->draft)
                                                 : std::string_view{});
}

bool WorkspaceShell::CreateChatConversation() {
  SyncActiveConversationDraft();
  context_.current_project_state.panel.chat.conversation_id =
      context_.current_project_state.conversations.CreateConversation(
          NextConversationTitle(context_.current_project_state.conversations), std::string{});
  context_.current_project_state.panel.chat.scroll_row = 0;
  context_.current_project_state.panel.chat.focus_region = ChatPaneFocusRegion::Composer;
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::CancelActiveChatRequest() {
  ChatPanelState& chat = context_.current_project_state.panel.chat;
  Conversation* conversation =
      context_.current_project_state.conversations.GetConversation(chat.request_conversation_id);
  if (conversation == nullptr || !chat.request_in_flight || chat.pending_assistant_message_id.empty()) {
    return false;
  }

  provider_bridge_manager_.CancelRequest(chat.pending_bridge_agent_id, chat.pending_bridge_request_id);
  const std::int64_t duration_ms =
      chat.request_started_ticks == 0
          ? 0
          : static_cast<std::int64_t>(SDL_GetTicks() - chat.request_started_ticks);
  conversation->status = RequestStatus::Cancelled;
  conversation->last_request_duration_ms = duration_ms;
  for (auto& message : conversation->messages) {
    if (message.id == chat.pending_assistant_message_id) {
      message.status = RequestStatus::Cancelled;
      message.request_duration_ms = duration_ms;
      if (message.error.empty()) {
        message.error = "Cancelled";
      }
      break;
    }
  }

  chat.request_in_flight = false;
  chat.request_started_ticks = 0;
  chat.status_text = "Cancelled";
  chat.request_conversation_id.clear();
  chat.pending_assistant_message_id.clear();
  chat.pending_bridge_agent_id.clear();
  chat.pending_bridge_request_id.clear();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::DeleteActiveChatConversation() {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return false;
  }
  const std::string current_id = conversation->id;
  if (context_.current_project_state.panel.chat.request_in_flight &&
      context_.current_project_state.panel.chat.request_conversation_id == current_id) {
    CancelActiveChatRequest();
  }

  const auto& conversations = context_.current_project_state.conversations.conversations();
  std::string next_id;
  for (const Conversation& item : conversations) {
    if (item.id != current_id) {
      next_id = item.id;
      break;
    }
  }
  context_.current_project_state.conversations.DeleteConversation(current_id);
  if (next_id.empty()) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
  } else {
    context_.current_project_state.panel.chat.conversation_id = next_id;
  }
  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  LoadChatComposerDraft();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

std::vector<const AiProviderSpec*> WorkspaceShell::ChatProviders() const {
  std::vector<const AiProviderSpec*> providers;
  for (const AiProviderSpec& provider : ai_provider_registry_.Specs()) {
    const ExternalAgentSpec* agent = external_agent_registry_.FindAgent(provider.id);
    if (AgentSupportsCapability(agent, "chat")) {
      providers.push_back(&provider);
    }
  }
  return providers;
}

std::vector<std::string> WorkspaceShell::ChatModelsForConversation(
    const Conversation& conversation) const {
  std::vector<std::string> models = provider_bridge_manager_.GetModels(conversation.provider_id);
  if (models.empty()) {
    if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(conversation.provider_id);
        provider != nullptr) {
      models = provider->models;
    }
  }
  if (!conversation.model_id.empty() &&
      std::find(models.begin(), models.end(), conversation.model_id) == models.end()) {
    models.push_back(conversation.model_id);
  }
  return models;
}

std::string WorkspaceShell::ChatComposerText() const {
  return SerializeChatComposerViewport(context_.current_project_state.panel.chat.composer);
}

void WorkspaceShell::CycleActiveConversationProvider(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  const auto providers = ChatProviders();
  if (providers.empty()) {
    conversation->provider_id.clear();
    conversation->model_id.clear();
    RequestSidebarRedraw();
    RequestChromeRedraw();
    return;
  }

  auto it = std::find_if(providers.begin(), providers.end(),
                         [&](const AiProviderSpec* provider) {
                           return provider != nullptr && provider->id == conversation->provider_id;
                         });
  std::size_t index = it == providers.end() ? 0 : static_cast<std::size_t>(it - providers.begin());
  const int count = static_cast<int>(providers.size());
  index = static_cast<std::size_t>((static_cast<int>(index) + delta + count * 8) % count);
  conversation->provider_id = providers[index]->id;
  conversation->model_id.clear();
  RequestSidebarRedraw();
  RequestChromeRedraw();
}

void WorkspaceShell::CycleActiveConversationModel(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr || conversation->provider_id.empty()) {
    return;
  }
  const std::vector<std::string> models = ChatModelsForConversation(*conversation);
  if (models.empty()) {
    conversation->model_id.clear();
    RequestSidebarRedraw();
    return;
  }
  auto it = std::find(models.begin(), models.end(), conversation->model_id);
  std::size_t index = it == models.end() ? 0 : static_cast<std::size_t>(it - models.begin());
  const int count = static_cast<int>(models.size());
  index = static_cast<std::size_t>((static_cast<int>(index) + delta + count * 8) % count);
  conversation->model_id = models[index];
  RequestSidebarRedraw();
}

void WorkspaceShell::CycleActiveConversationToolMode(int delta) {
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr) {
    return;
  }
  const int count = 3;
  const int current =
      conversation->tool_mode == ToolMode::NoTools ? 0
      : conversation->tool_mode == ToolMode::Ask   ? 1
                                                   : 2;
  const int next = (current + delta + count * 4) % count;
  conversation->tool_mode =
      next == 0 ? ToolMode::NoTools : next == 1 ? ToolMode::Ask : ToolMode::Auto;
  RequestSidebarRedraw();
}

std::string WorkspaceShell::ChatAuthBannerText(const Conversation* conversation) const {
  if (ai_provider_registry_.Specs().empty()) {
    return "No chat providers are available.";
  }
  if (conversation == nullptr || conversation->provider_id.empty()) {
    return "Select a provider to send chat requests.";
  }

  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(conversation->provider_id);
  if (provider == nullptr) {
    return "The selected provider is no longer available.";
  }

  switch (GetProviderAuthStatus(provider->id)) {
    case ProviderAuthStatus::KeyMissing:
      return "Add an API key for " + provider->label + " before sending.";
    case ProviderAuthStatus::KeyInvalid:
      return provider->label + " rejected the stored API key.";
    case ProviderAuthStatus::KeyPresent:
      return "Stored API key for " + provider->label + " has not been validated yet.";
    case ProviderAuthStatus::KeyValid:
    case ProviderAuthStatus::Unknown:
      break;
  }

  const std::vector<std::string> models = ChatModelsForConversation(*conversation);
  if (!conversation->model_id.empty() &&
      !models.empty() &&
      std::find(models.begin(), models.end(), conversation->model_id) == models.end()) {
    return "The selected model is no longer offered by " + provider->label + ".";
  }
  return {};
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
  EnsureOutputChannelTabOpen(update->channel_id);
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
  EnsureOutputChannelTabOpen(channel_id);
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

  const std::string language_id = DetectActiveLanguageId(*viewport);
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
  if (!context_.current_project_state.overlay.workflow.completion.items.empty()) {
    ShowOverlay(OverlayMode::Completion);
    return true;
  }

  LspClient* client = LspClientForViewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(lsp_manager_, language_id, provider_error);
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  EnsureLspDocumentOpen(*viewport, *client, language_id);
  auto& session = context_.current_project_state.overlay.workflow.completion;
  session.items.clear();
  session.selected_index = 0;
  session.replacement_range = CompletionReplacementRange(*viewport);
  session.source = "lsp";
  session.error = "Loading...";
  ShowOverlay(OverlayMode::Completion);
  client->RequestCompletionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::CompletionItem>> items) {
        auto& current_session = context_.current_project_state.overlay.workflow.completion;
        current_session.items.clear();
        current_session.selected_index = 0;
        current_session.source = "lsp";
        if (!items.has_value() || items->empty()) {
          current_session.error = "No completions available";
        } else {
          current_session.error.clear();
          for (const auto& item : *items) {
            current_session.items.push_back(CompletionSessionItem{
                .label = item.label,
                .detail = item.detail,
                .documentation = item.documentation,
                .insert_text = item.insert_text,
            });
          }
        }
        RequestOverlayRedraw();
      });
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

  const std::string language_id = DetectActiveLanguageId(*viewport);
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
  if (!session.items.empty()) {
    ShowOverlay(OverlayMode::CodeActions);
    return true;
  }

  LspClient* client = LspClientForViewport(*viewport, nullptr);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(lsp_manager_, language_id, provider_error);
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }

  EnsureLspDocumentOpen(*viewport, *client, language_id);
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = "Loading...";
  ShowOverlay(OverlayMode::CodeActions);
  client->RequestCodeActionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Range{
          .start = LspClient::Position{static_cast<int>(range.start.line),
                                       static_cast<int>(range.start.column)},
          .end = LspClient::Position{static_cast<int>(range.end.line),
                                     static_cast<int>(range.end.column)},
      },
      [this](std::optional<std::vector<LspClient::CodeAction>> actions) {
        auto& current_session = context_.current_project_state.overlay.workflow.code_actions;
        current_session.items.clear();
        current_session.selected_index = 0;
        current_session.source = "lsp";
        if (!actions.has_value() || actions->empty()) {
          current_session.error = "No code actions available";
        } else {
          current_session.error.clear();
          for (const auto& action : *actions) {
            std::vector<std::string> arguments;
            arguments.reserve(action.arguments.size());
            for (const auto& argument : action.arguments) {
              arguments.push_back(JsonValueToArgumentString(argument));
            }
            current_session.items.push_back(CodeActionSessionItem{
                .title = action.title,
                .command = action.command,
                .arguments = std::move(arguments),
            });
          }
        }
        RequestOverlayRedraw();
      });
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

bool WorkspaceShell::GoToLspDefinition(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(lsp_manager_, language_id, {});
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  client->RequestGoToDefinitionAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        if (!locations.has_value() || locations->empty()) {
          output_channels_.AppendLine("lsp.definition", "LSP Definition", "No definition found");
          ShowOutputChannel("lsp.definition");
          return;
        }
        const std::optional<std::filesystem::path> path = PathFromFileUri(locations->front().uri);
        if (!path.has_value()) {
          return;
        }
        if (!OpenFileInNewTab(*path)) {
          return;
        }
        if (editor::TextViewport* active = ActiveEditorViewport(); active != nullptr) {
          active->MoveCursorTo(
              static_cast<std::size_t>(std::max(locations->front().range.start.line, 0)),
              static_cast<std::size_t>(std::max(locations->front().range.start.character, 0)));
          ResetCaretBlink();
          RequestFocusedEditorRedraw();
        }
      });
  return true;
}

bool WorkspaceShell::FindLspReferences(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    if (error_message != nullptr) {
      *error_message = "No active file";
    }
    return false;
  }

  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    const std::string failure = LspUnavailableMessage(lsp_manager_, language_id, {});
    output_channels_.AppendLine("lsp.log", "LSP Log", failure);
    ShowOutputChannel("lsp.log");
    if (error_message != nullptr) {
      *error_message = failure;
    }
    return false;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  client->RequestFindReferencesAsync(
      FileUriForPath(viewport->path()),
      LspClient::Position{static_cast<int>(viewport->cursor_line()),
                          static_cast<int>(viewport->cursor_column())},
      true,
      [this](std::optional<std::vector<LspClient::Location>> locations) {
        output_channels_.Clear("lsp.references");
        if (!locations.has_value() || locations->empty()) {
          output_channels_.AppendLine("lsp.references", "LSP References", "No references found");
          ShowOutputChannel("lsp.references");
          return;
        }
        std::map<std::filesystem::path, std::vector<std::string>> file_line_cache;
        for (std::size_t location_index = 0; location_index < locations->size(); ++location_index) {
          const auto& location = (*locations)[location_index];
          const std::optional<std::filesystem::path> path = PathFromFileUri(location.uri);
          if (!path.has_value()) {
            continue;
          }
          const std::string label =
              context_.current_project_state.root.empty()
                  ? path->generic_string()
                  : std::filesystem::relative(*path, context_.current_project_state.root)
                        .generic_string();
          output_channels_.AppendLine(
              "lsp.references", "LSP References",
              label + ":" + std::to_string(location.range.start.line + 1) + ":" +
                  std::to_string(location.range.start.character + 1));

          const auto lines_it = file_line_cache.find(*path);
          const std::vector<std::string>* file_lines = nullptr;
          if (lines_it != file_line_cache.end()) {
            file_lines = &lines_it->second;
          } else if (const auto text = util::ReadTextFile(*path); text.has_value()) {
            file_lines = &file_line_cache.emplace(*path, util::SplitLines(*text)).first->second;
          }
          if (file_lines == nullptr || file_lines->empty()) {
            continue;
          }

          const std::size_t target_line = static_cast<std::size_t>(
              std::max(location.range.start.line + 1, 1));
          const std::size_t first_line = target_line > 1 ? target_line - 1 : 1;
          const std::size_t last_line = target_line + 1;
          for (std::size_t line_number = first_line; line_number <= last_line; ++line_number) {
            if (line_number == 0 || line_number > file_lines->size()) {
              continue;
            }
            output_channels_.AppendLine(
                "lsp.references", "LSP References",
                std::string(line_number == target_line ? " > " : "   ") +
                    std::to_string(line_number) + " | " + (*file_lines)[line_number - 1]);
          }
          if (location_index + 1 < locations->size()) {
            output_channels_.AppendLine("lsp.references", "LSP References", "");
          }
        }
        ShowOutputChannel("lsp.references");
      });
  return true;
}

bool WorkspaceShell::DiscoverTestsForActiveBuffer(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = DetectActiveLanguageId(*viewport);
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
    message = ChatComposerText();
  }
  if (message.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat message is empty";
    }
    return false;
  }

  ShowChatPanel();
  if (context_.current_project_state.panel.chat.request_in_flight) {
    if (error_message != nullptr) {
      *error_message = "Another chat request is already running in this project";
    }
    return false;
  }
  Conversation* conversation =
      ActiveConversation();
  if (conversation == nullptr) {
    context_.current_project_state.panel.chat.conversation_id =
        context_.current_project_state.conversations.CreateConversation(
            NextConversationTitle(context_.current_project_state.conversations), std::string{});
    conversation = ActiveConversation();
  }

  const ExternalAgentSpec* agent = nullptr;
  if (conversation != nullptr && !conversation->provider_id.empty()) {
    const ExternalAgentSpec* preferred =
        external_agent_registry_.FindAgent(conversation->provider_id);
    if (AgentSupportsCapability(preferred, "chat")) {
      agent = preferred;
    }
  }
  if (agent == nullptr) {
    agent = SelectAgentForCapability(external_agent_registry_, "chat");
  }
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio chat agent is available";
    }
    return false;
  }

  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
    if (auth_status == ProviderAuthStatus::KeyMissing ||
        auth_status == ProviderAuthStatus::KeyInvalid) {
      if (error_message != nullptr) {
        *error_message = ChatAuthBannerText(conversation);
      }
      return false;
    }
  }

  const std::string user_id = GenerateRuntimeMessageId("user");
  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  conversation->provider_id = agent->id;
  conversation->draft.clear();
  {
    Message user_msg;
    user_msg.id = user_id;
    user_msg.role = MessageRole::User;
    user_msg.content = message;
    user_msg.timestamp = CurrentUtcTimestamp();
    user_msg.status = RequestStatus::Succeeded;
    context_.current_project_state.conversations.AddMessage(conversation->id, user_msg);
  }
  {
    Message assistant_msg;
    assistant_msg.id = assistant_id;
    assistant_msg.role = MessageRole::Assistant;
    assistant_msg.timestamp = CurrentUtcTimestamp();
    assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : agent->id;
    assistant_msg.status = RequestStatus::Running;
    context_.current_project_state.conversations.AddMessage(conversation->id, assistant_msg);
  }
  conversation->status = RequestStatus::Running;
  conversation->last_request_duration_ms = 0;
  context_.current_project_state.panel.chat.request_conversation_id = conversation->id;
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.request_started_ticks = SDL_GetTicks();
  context_.current_project_state.panel.chat.status_text = "Waiting for " + agent->label;
  LoadChatComposerViewport(&context_.current_project_state.panel.chat.composer, {});
  const auto mark_failed = [&](std::string_view reason) {
    conversation->status = RequestStatus::Failed;
    for (auto& item : conversation->messages) {
      if (item.id == assistant_id) {
        item.status = RequestStatus::Failed;
        item.error = std::string(reason);
        break;
      }
    }
    context_.current_project_state.panel.chat.status_text = std::string(reason);
  };

  if (agent->command.empty()) {
    if (error_message != nullptr) {
      *error_message = "Chat agent command is empty";
    }
    mark_failed("Chat agent command is empty");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::string api_key;
  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr && !provider->api_key_name.empty()) {
    api_key = secret_storage_.Retrieve(provider->api_key_name).value_or("");
  }
  if (!provider_bridge_manager_.IsBridgeRunning(agent->id) &&
      !provider_bridge_manager_.StartBridge(agent->id,
                                            agent->command,
                                            api_key,
                                            context_.current_project_state.root)) {
    if (error_message != nullptr) {
      *error_message = "Failed to start chat agent bridge";
    }
    mark_failed("Failed to start chat agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::vector<std::pair<std::string, std::string>> bridge_messages;
  if (conversation != nullptr) {
    for (const auto& msg : conversation->messages) {
      if (msg.role == MessageRole::User && msg.id != user_id) {
        bridge_messages.emplace_back("user", msg.content);
      } else if (msg.role == MessageRole::Assistant &&
                 msg.status == RequestStatus::Succeeded) {
        bridge_messages.emplace_back("assistant", msg.content);
      } else if (msg.id == user_id) {
        bridge_messages.emplace_back("user", msg.content);
        break;
      }
    }
  }
  if (bridge_messages.empty() || bridge_messages.back().first != "user") {
    bridge_messages.emplace_back("user", message);
  }

  const std::string request_id = GenerateRuntimeMessageId("bridge-req");
  const std::string model_id = conversation != nullptr ? conversation->model_id : "";
  const std::string system_prompt = conversation != nullptr ? conversation->system_prompt : "";
  const std::string tool_mode_str = [&]() -> std::string {
    if (conversation == nullptr) return "no_tools";
    switch (conversation->tool_mode) {
      case ToolMode::NoTools: return "no_tools";
      case ToolMode::Ask:     return "ask";
      case ToolMode::Auto:    return "auto";
    }
    return "no_tools";
  }();

  context_.current_project_state.panel.chat.pending_bridge_agent_id = agent->id;
  context_.current_project_state.panel.chat.pending_bridge_request_id = request_id;
  if (!provider_bridge_manager_.SendChat(agent->id,
                                         request_id,
                                         bridge_messages,
                                         model_id,
                                         system_prompt,
                                         tool_mode_str)) {
    if (error_message != nullptr) {
      *error_message = "Failed to send chat request to agent bridge";
    }
    mark_failed("Failed to send chat request to agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_agent_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_request_id.clear();
    return false;
  }

  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

bool WorkspaceShell::RetryActiveChatRequest(std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  Conversation* conversation = ActiveConversation();
  if (conversation == nullptr || context_.current_project_state.panel.chat.request_in_flight) {
    return false;
  }

  auto last_user_it =
      std::find_if(conversation->messages.rbegin(), conversation->messages.rend(),
                   [](const Message& message) { return message.role == MessageRole::User; });
  if (last_user_it == conversation->messages.rend()) {
    if (error_message != nullptr) {
      *error_message = "No earlier user turn is available to retry";
    }
    return false;
  }
  const std::string message = last_user_it->content;

  const ExternalAgentSpec* agent = nullptr;
  if (!conversation->provider_id.empty()) {
    const ExternalAgentSpec* preferred =
        external_agent_registry_.FindAgent(conversation->provider_id);
    if (AgentSupportsCapability(preferred, "chat")) {
      agent = preferred;
    }
  }
  if (agent == nullptr) {
    agent = SelectAgentForCapability(external_agent_registry_, "chat");
  }
  if (agent == nullptr || agent->protocol != "stdio") {
    if (error_message != nullptr) {
      *error_message = "No stdio chat agent is available";
    }
    return false;
  }

  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr) {
    const ProviderAuthStatus auth_status = GetProviderAuthStatus(provider->id);
    if (auth_status == ProviderAuthStatus::KeyMissing ||
        auth_status == ProviderAuthStatus::KeyInvalid) {
      if (error_message != nullptr) {
        *error_message = ChatAuthBannerText(conversation);
      }
      return false;
    }
  }

  const std::string assistant_id = GenerateRuntimeMessageId("assistant");
  Message assistant_msg;
  assistant_msg.id = assistant_id;
  assistant_msg.role = MessageRole::Assistant;
  assistant_msg.timestamp = CurrentUtcTimestamp();
  assistant_msg.model = !conversation->model_id.empty() ? conversation->model_id : agent->id;
  assistant_msg.status = RequestStatus::Running;
  context_.current_project_state.conversations.AddMessage(conversation->id, assistant_msg);

  conversation->status = RequestStatus::Running;
  conversation->last_request_duration_ms = 0;
  context_.current_project_state.panel.chat.request_conversation_id = conversation->id;
  context_.current_project_state.panel.chat.pending_assistant_message_id = assistant_id;
  context_.current_project_state.panel.chat.request_in_flight = true;
  context_.current_project_state.panel.chat.request_started_ticks = SDL_GetTicks();
  context_.current_project_state.panel.chat.status_text = "Retrying with " + agent->label;
  const auto mark_failed = [&](std::string_view reason) {
    conversation->status = RequestStatus::Failed;
    for (auto& item : conversation->messages) {
      if (item.id == assistant_id) {
        item.status = RequestStatus::Failed;
        item.error = std::string(reason);
        break;
      }
    }
    context_.current_project_state.panel.chat.status_text = std::string(reason);
  };

  std::string api_key;
  if (const AiProviderSpec* provider = ai_provider_registry_.FindProvider(agent->id);
      provider != nullptr && !provider->api_key_name.empty()) {
    api_key = secret_storage_.Retrieve(provider->api_key_name).value_or("");
  }
  if (!provider_bridge_manager_.IsBridgeRunning(agent->id) &&
      !provider_bridge_manager_.StartBridge(agent->id, agent->command, api_key,
                                            context_.current_project_state.root)) {
    if (error_message != nullptr) {
      *error_message = "Failed to start chat agent bridge";
    }
    mark_failed("Failed to start chat agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    return false;
  }

  std::vector<std::pair<std::string, std::string>> bridge_messages;
  for (const auto& msg : conversation->messages) {
    if (msg.role == MessageRole::User) {
      bridge_messages.emplace_back("user", msg.content);
      if (msg.id == last_user_it->id) {
        break;
      }
    } else if (msg.role == MessageRole::Assistant &&
               msg.status == RequestStatus::Succeeded) {
      bridge_messages.emplace_back("assistant", msg.content);
    }
  }
  if (bridge_messages.empty() || bridge_messages.back().first != "user") {
    bridge_messages.emplace_back("user", message);
  }

  const std::string request_id = GenerateRuntimeMessageId("bridge-req");
  const std::string tool_mode_str =
      conversation->tool_mode == ToolMode::NoTools ? "no_tools"
      : conversation->tool_mode == ToolMode::Ask   ? "ask"
                                                   : "auto";
  context_.current_project_state.panel.chat.pending_bridge_agent_id = agent->id;
  context_.current_project_state.panel.chat.pending_bridge_request_id = request_id;
  if (!provider_bridge_manager_.SendChat(agent->id, request_id, bridge_messages,
                                         conversation->model_id, conversation->system_prompt,
                                         tool_mode_str)) {
    if (error_message != nullptr) {
      *error_message = "Failed to send chat request to agent bridge";
    }
    mark_failed("Failed to send chat request to agent bridge");
    context_.current_project_state.panel.chat.request_in_flight = false;
    context_.current_project_state.panel.chat.request_started_ticks = 0;
    context_.current_project_state.panel.chat.request_conversation_id.clear();
    context_.current_project_state.panel.chat.pending_assistant_message_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_agent_id.clear();
    context_.current_project_state.panel.chat.pending_bridge_request_id.clear();
    return false;
  }

  context_.current_project_state.panel.chat.scroll_row = std::numeric_limits<int>::max();
  RequestSidebarRedraw();
  RequestChromeRedraw();
  return true;
}

void WorkspaceShell::ConsumeProviderBridgeUpdates() {
  while (true) {
    const auto update = provider_bridge_manager_.ConsumeChatUpdate();
    if (!update.has_value()) {
      break;
    }
    bool handled = false;
    const auto apply_update = [&](ProjectWorkspaceState* project,
                                  bool active_project) -> bool {
      if (project == nullptr) {
        return false;
      }

      ChatPanelState& chat = project->panel.chat;
      if (chat.request_in_flight &&
          chat.pending_bridge_agent_id == update->agent_id &&
          chat.pending_bridge_request_id == update->request_id) {
        Conversation* conversation =
            project->conversations.GetConversation(chat.request_conversation_id);
        if (!chat.pending_assistant_message_id.empty()) {
          UpdateMessageContent(conversation, chat.pending_assistant_message_id, update->chunk);
        }

        if (update->finished) {
          const RequestStatus terminal_status =
              update->succeeded ? RequestStatus::Succeeded : RequestStatus::Failed;
          const std::int64_t duration_ms =
              chat.request_started_ticks == 0
                  ? 0
                  : static_cast<std::int64_t>(SDL_GetTicks() - chat.request_started_ticks);
          if (conversation != nullptr) {
            conversation->status = terminal_status;
            conversation->last_request_duration_ms = duration_ms;
            for (auto& msg : conversation->messages) {
              if (msg.id == chat.pending_assistant_message_id) {
                msg.status = terminal_status;
                msg.request_duration_ms = duration_ms;
                if (!update->succeeded && !update->status_text.empty()) {
                  msg.error = update->status_text;
                }
                break;
              }
            }
          }
          chat.request_in_flight = false;
          chat.request_started_ticks = 0;
          chat.status_text = update->status_text;
          chat.request_conversation_id.clear();
          chat.pending_assistant_message_id.clear();
          chat.pending_bridge_agent_id.clear();
          chat.pending_bridge_request_id.clear();
        }

        if (active_project) {
          project->panel.chat.scroll_row = std::numeric_limits<int>::max();
          RequestSidebarRedraw();
        }
        RequestChromeRedraw();
        return true;
      }

      InlineCompletionState& inline_completion = project->inline_completion;
      if (inline_completion.request_in_flight &&
          inline_completion.pending_bridge_agent_id == update->agent_id &&
          inline_completion.pending_bridge_request_id == update->request_id) {
        inline_completion.text += update->chunk;
        if (update->finished) {
          inline_completion.request_in_flight = false;
          inline_completion.visible = update->succeeded && !inline_completion.text.empty();
          inline_completion.error = update->succeeded ? std::string{} : update->status_text;
          inline_completion.pending_bridge_agent_id.clear();
          inline_completion.pending_bridge_request_id.clear();
        }
        if (active_project) {
          RequestFocusedEditorRedraw();
        }
        return true;
      }
      return false;
    };

    if (apply_update(&context_.current_project_state, true)) {
      handled = true;
    } else {
      for (std::size_t i = 0; i < context_.project_catalog.entries.size(); ++i) {
        if (context_.HasActiveProjectCatalogEntry() && i == context_.project_catalog.active_index) {
          continue;
        }
        if (apply_update(context_.project_catalog.entries[i].get(), false)) {
          handled = true;
          break;
        }
      }
    }
    if (!handled) {
      RequestChromeRedraw();
    }
  }
}

bool WorkspaceShell::SetProviderApiKey(std::string_view provider_id,
                                       std::string_view api_key,
                                       std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  if (provider == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Unknown provider: " + std::string(provider_id);
    }
    return false;
  }
  const std::string key = !provider->api_key_name.empty()
                              ? provider->api_key_name
                              : std::string(provider_id) + ".api_key";
  if (!secret_storage_.Store(key, std::string(api_key))) {
    if (error_message != nullptr) {
      *error_message = "Failed to store API key for " + std::string(provider_id);
    }
    return false;
  }
  // If there is a running bridge for this provider, restart it with the new key.
  const ExternalAgentSpec* agent =
      external_agent_registry_.FindAgent(std::string(provider_id));
  if (agent != nullptr && !agent->command.empty()) {
    provider_bridge_manager_.StartBridge(agent->id,
                                         agent->command,
                                         std::string(api_key),
                                         context_.current_project_state.root);
  }
  return true;
}

bool WorkspaceShell::ClearProviderApiKey(std::string_view provider_id,
                                         std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  const std::string key = (provider != nullptr && !provider->api_key_name.empty())
                              ? provider->api_key_name
                              : std::string(provider_id) + ".api_key";
  secret_storage_.Delete(key);
  provider_bridge_manager_.StopBridge(std::string(provider_id));
  return true;
}

ProviderAuthStatus WorkspaceShell::GetProviderAuthStatus(std::string_view provider_id) const {
  const AiProviderSpec* provider = ai_provider_registry_.FindProvider(std::string(provider_id));
  if (provider == nullptr) {
    return ProviderAuthStatus::Unknown;
  }
  const std::string key = !provider->api_key_name.empty()
                              ? provider->api_key_name
                              : std::string(provider_id) + ".api_key";
  if (!secret_storage_.Contains(key)) {
    return ProviderAuthStatus::KeyMissing;
  }
  // If a bridge is running, return the bridge's reported auth status.
  const ProviderAuthStatus bridge_status =
      provider_bridge_manager_.GetAuthStatus(std::string(provider_id));
  if (bridge_status != ProviderAuthStatus::Unknown &&
      bridge_status != ProviderAuthStatus::KeyMissing) {
    return bridge_status;
  }
  return ProviderAuthStatus::KeyPresent;
}

void WorkspaceShell::ConsumeLspCallbacks() {
  lsp_manager_.DrainCallbacks();
  RequestFullRedraw();
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
  if (agent->command.empty()) {
    if (error_message != nullptr) {
      *error_message = "Inline completion agent command is empty";
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
  context_.current_project_state.inline_completion.pending_bridge_agent_id.clear();
  context_.current_project_state.inline_completion.pending_bridge_request_id.clear();

  std::ostringstream prompt;
  prompt << "Complete the code at line " << (viewport->cursor_line() + 1) << ", column "
         << (viewport->cursor_column() + 1) << ". Return only the completion text.\n\n";
  const auto& lines = viewport->lines();
  for (std::size_t i = 0; i < lines.size(); ++i) {
    prompt << lines[i] << '\n';
  }
  if (!provider_bridge_manager_.IsBridgeRunning(agent->id) &&
      !provider_bridge_manager_.StartBridge(agent->id,
                                            agent->command,
                                            {},
                                            context_.current_project_state.root)) {
    context_.current_project_state.inline_completion.request_in_flight = false;
    if (error_message != nullptr) {
      *error_message = "Failed to start inline completion agent bridge";
    }
    return false;
  }
  const std::string request_id = GenerateRuntimeMessageId("bridge-inline");
  context_.current_project_state.inline_completion.pending_bridge_agent_id = agent->id;
  context_.current_project_state.inline_completion.pending_bridge_request_id = request_id;
  if (!provider_bridge_manager_.SendChat(agent->id,
                                         request_id,
                                         {{"user", prompt.str()}},
                                         {},
                                         {},
                                         "no_tools")) {
    context_.current_project_state.inline_completion.request_in_flight = false;
    context_.current_project_state.inline_completion.pending_bridge_agent_id.clear();
    context_.current_project_state.inline_completion.pending_bridge_request_id.clear();
    if (error_message != nullptr) {
      *error_message = "Failed to send inline completion request to agent bridge";
    }
    return false;
  }
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
