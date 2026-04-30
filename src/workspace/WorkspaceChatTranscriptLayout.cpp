#include "workspace/WorkspaceShell.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

namespace {

struct ParsedChatLinkTarget {
  bool remote = false;
  std::filesystem::path path;
  std::string url;
  std::size_t line = 0;
  std::size_t column = 0;
};

std::string BuildContinuationPrefix(std::string_view prefix) {
  return std::string(prefix.size(), ' ');
}

std::string RequestStatusLabel(RequestStatus status) {
  switch (status) {
    case RequestStatus::Idle:
      return "Idle";
    case RequestStatus::Queued:
      return "Queued";
    case RequestStatus::Running:
      return "Running";
    case RequestStatus::Streaming:
      return "Streaming";
    case RequestStatus::Succeeded:
      return "Succeeded";
    case RequestStatus::Failed:
      return "Failed";
    case RequestStatus::Cancelled:
      return "Cancelled";
  }
  return "Unknown";
}

std::string MessageRoleLabel(MessageRole role) {
  switch (role) {
    case MessageRole::User:
      return "You";
    case MessageRole::Assistant:
      return "Assistant";
    case MessageRole::System:
      return "System";
  }
  return "Message";
}

std::string FormatDurationMs(std::int64_t duration_ms) {
  if (duration_ms <= 0) {
    return {};
  }
  if (duration_ms < 1000) {
    return std::to_string(duration_ms) + " ms";
  }
  std::ostringstream stream;
  stream.setf(std::ios::fixed);
  stream.precision(duration_ms >= 10'000 ? 0 : 1);
  stream << static_cast<double>(duration_ms) / 1000.0 << " s";
  return stream.str();
}

std::string JoinMetadataParts(const std::vector<std::string>& parts) {
  std::string joined;
  bool first = true;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!first) {
      joined += "  |  ";
    }
    first = false;
    joined += part;
  }
  return joined;
}

std::string ChatLinkDebugString(const ParsedChatLinkTarget& target) {
  if (target.remote) {
    return target.url;
  }
  std::string text = target.path.generic_string();
  if (target.line != 0) {
    text += ":" + std::to_string(target.line);
    if (target.column != 0) {
      text += ":" + std::to_string(target.column);
    }
  }
  return text;
}

}  // namespace
WorkspaceShell::ChatTranscriptLayout WorkspaceShell::BuildChatTranscriptLayout(
    const SDL_FRect& sidebar_rect) const {
  ChatTranscriptLayout layout;
  const Conversation* conversation = ActiveConversation();
  const float text_width =
      std::max(24.0f, ChatSidebarTranscriptRect(sidebar_rect).w - 12.0f);
  const std::int64_t in_flight_duration_ms =
      context_.current_project_state.panel.chat.request_in_flight &&
              context_.current_project_state.panel.chat.request_started_ticks != 0
          ? static_cast<std::int64_t>(SDL_GetTicks() -
                                      context_.current_project_state.panel.chat.request_started_ticks)
          : 0;

  auto push_wrapped_row = [&](ChatTranscriptRow::Kind kind,
                              ChatTranscriptRow::Tone tone,
                              MessageRole role,
                              std::string prefix,
                              const std::vector<ChatInlineFragment>& fragments) {
    struct Token {
      std::string text;
      ChatTextStyle style = ChatTextStyle::Normal;
      std::optional<ChatLinkTarget> link;
      bool whitespace = false;
    };

    auto emit_row = [&](std::string current_prefix,
                        std::vector<ChatTranscriptSegment>* segments) {
      layout.rows.push_back(ChatTranscriptRow{
          .kind = kind,
          .tone = tone,
          .role = role,
          .prefix = std::move(current_prefix),
          .segments = std::move(*segments),
      });
      segments->clear();
    };

    std::vector<Token> tokens;
    for (const ChatInlineFragment& fragment : fragments) {
      if (fragment.text.empty()) {
        continue;
      }
      if (fragment.style == ChatTextStyle::InlineCode) {
        tokens.push_back(Token{
            .text = fragment.text,
            .style = fragment.style,
            .link = fragment.link,
            .whitespace = false,
        });
        continue;
      }
      std::size_t index = 0;
      while (index < fragment.text.size()) {
        const bool whitespace =
            std::isspace(static_cast<unsigned char>(fragment.text[index])) != 0;
        std::size_t end = index + 1;
        while (end < fragment.text.size() &&
               (std::isspace(static_cast<unsigned char>(fragment.text[end])) != 0) == whitespace) {
          ++end;
        }
        tokens.push_back(Token{
            .text = fragment.text.substr(index, end - index),
            .style = fragment.style,
            .link = fragment.link,
            .whitespace = whitespace,
        });
        index = end;
      }
    }

    const std::string continuation_prefix = BuildContinuationPrefix(prefix);
    std::string current_prefix = std::move(prefix);
    std::vector<ChatTranscriptSegment> current_segments;
    float current_width = text_renderer_.MeasureWidth(current_prefix);
    bool have_visible_content = false;

    auto append_segment = [&](std::string text,
                              ChatTextStyle style,
                              const std::optional<ChatLinkTarget>& link) {
      if (text.empty()) {
        return;
      }
      if (!current_segments.empty() && current_segments.back().style == style &&
          !current_segments.back().link.has_value() && !link.has_value()) {
        current_segments.back().text += text;
      } else {
        current_segments.push_back(ChatTranscriptSegment{
            .text = std::move(text),
            .style = style,
            .link = link,
        });
      }
    };

    auto flush_line = [&]() {
      emit_row(current_prefix, &current_segments);
      current_prefix = continuation_prefix;
      current_width = text_renderer_.MeasureWidth(current_prefix);
      have_visible_content = false;
    };

    for (const Token& token : tokens) {
      if (token.text.empty()) {
        continue;
      }
      if (token.whitespace) {
        if (!have_visible_content) {
          continue;
        }
        const float token_width = text_renderer_.MeasureWidth(token.text);
        if (current_width + token_width > text_width) {
          flush_line();
          continue;
        }
        append_segment(token.text, token.style, token.link);
        current_width += token_width;
        continue;
      }

      std::string remaining = token.text;
      while (!remaining.empty()) {
        const float remaining_width = text_renderer_.MeasureWidth(remaining);
        if (have_visible_content && current_width + remaining_width > text_width) {
          flush_line();
          continue;
        }
        if (!have_visible_content && current_width + remaining_width > text_width) {
          std::string chunk;
          for (char ch : remaining) {
            const std::string candidate = chunk + ch;
            if (!chunk.empty() &&
                current_width + text_renderer_.MeasureWidth(candidate) > text_width) {
              break;
            }
            chunk = candidate;
          }
          if (chunk.empty()) {
            chunk.push_back(remaining.front());
          }
          append_segment(chunk, token.style, token.link);
          current_width += text_renderer_.MeasureWidth(chunk);
          remaining.erase(0, chunk.size());
          have_visible_content = true;
          if (!remaining.empty()) {
            flush_line();
          }
          continue;
        }

        append_segment(remaining, token.style, token.link);
        current_width += remaining_width;
        have_visible_content = true;
        remaining.clear();
      }
    }

    if (!current_segments.empty() || !current_prefix.empty()) {
      emit_row(current_prefix, &current_segments);
    }
  };

  auto message_metadata = [&](const Message& message,
                              bool pending_assistant_message) {
    std::vector<std::string> parts;
    parts.push_back(MessageRoleLabel(message.role));
    parts.push_back(pending_assistant_message
                        ? context_.current_project_state.panel.chat.status_text
                        : RequestStatusLabel(message.status));
    const std::int64_t duration_ms =
        pending_assistant_message ? in_flight_duration_ms : message.request_duration_ms;
    if (const std::string duration = FormatDurationMs(duration_ms); !duration.empty()) {
      parts.push_back(duration);
    }
    if (!message.provider_id.empty()) {
      parts.push_back(message.provider_id);
    }
    if (!message.model.empty()) {
      parts.push_back(message.model);
    }
    if (!message.timestamp.empty()) {
      parts.push_back(message.timestamp);
    }
    return JoinMetadataParts(parts);
  };

  auto tool_event_summary = [](const ToolEvent& event) {
    std::vector<std::string> parts;
    parts.push_back("Tool");
    parts.push_back(!event.display_name.empty() ? event.display_name : event.tool_id);
    parts.push_back(event.status);
    if (!event.permission_decision.empty()) {
      parts.push_back(event.permission_decision);
    }
    if (!event.capability_scope.empty()) {
      parts.push_back(event.capability_scope);
    }
    if (const std::string duration = FormatDurationMs(event.duration_ms); !duration.empty()) {
      parts.push_back(duration);
    }
    if (!event.arguments_summary.empty()) {
      parts.push_back(event.arguments_summary);
    }
    if (!event.output_summary.empty()) {
      parts.push_back(event.output_summary);
    }
    if (!event.error.empty()) {
      parts.push_back("Error: " + event.error);
    }
    return JoinMetadataParts(parts);
  };

  if (conversation == nullptr || conversation->messages.empty()) {
    layout.rows.push_back(ChatTranscriptRow{
        .kind = ChatTranscriptRow::Kind::Placeholder,
        .tone = ChatTranscriptRow::Tone::Normal,
        .role = MessageRole::Assistant,
        .prefix = {},
        .segments =
            {ChatTranscriptSegment{
                .text = "Ask a question to start a conversation.",
                .style = ChatTextStyle::Muted,
                .link = std::nullopt,
            }},
    });
  } else {
    for (const Message& message : conversation->messages) {
      const bool pending_assistant_message =
          message.role == MessageRole::Assistant &&
          message.id == context_.current_project_state.panel.chat.pending_assistant_message_id &&
          context_.current_project_state.panel.chat.request_in_flight &&
          context_.current_project_state.panel.chat.request_conversation_id == conversation->id;

      push_wrapped_row(ChatTranscriptRow::Kind::Meta, ChatTranscriptRow::Tone::Normal,
                       message.role, {},
                       {ChatInlineFragment{
                           .text = message_metadata(message, pending_assistant_message),
                           .style = ChatTextStyle::Muted,
                           .link = std::nullopt,
                       }});

      const std::string content =
          message.content.empty() && pending_assistant_message ? "Thinking..." : message.content;
      const ChatMarkdownDocument& markdown = ParsedChatMarkdown(content);
      for (const ChatMarkdownBlock& block : markdown.blocks) {
        switch (block.kind) {
          case ChatMarkdownBlock::Kind::Heading:
            push_wrapped_row(ChatTranscriptRow::Kind::Body,
                             ChatTranscriptRow::Tone::Heading, message.role, {},
                             block.fragments);
            break;
          case ChatMarkdownBlock::Kind::Quote:
            push_wrapped_row(ChatTranscriptRow::Kind::Body,
                             ChatTranscriptRow::Tone::Quote, message.role, "> ",
                             block.fragments);
            break;
          case ChatMarkdownBlock::Kind::ListItem: {
            const std::string prefix =
                block.ordered_list ? std::to_string(block.level) + ". " : std::string("- ");
            push_wrapped_row(ChatTranscriptRow::Kind::Body,
                             ChatTranscriptRow::Tone::List, message.role, prefix,
                             block.fragments);
            break;
          }
          case ChatMarkdownBlock::Kind::CodeBlock:
            if (block.code_lines.empty()) {
              layout.rows.push_back(ChatTranscriptRow{
                  .kind = ChatTranscriptRow::Kind::Code,
                  .tone = ChatTranscriptRow::Tone::Normal,
                  .role = message.role,
                  .prefix = {},
                  .segments = {},
              });
              break;
            }
            for (const std::string& code_line : block.code_lines) {
              layout.rows.push_back(ChatTranscriptRow{
                  .kind = ChatTranscriptRow::Kind::Code,
                  .tone = ChatTranscriptRow::Tone::Normal,
                  .role = message.role,
                  .prefix = {},
                  .segments =
                      {ChatTranscriptSegment{
                          .text = code_line,
                          .style = ChatTextStyle::InlineCode,
                          .link = std::nullopt,
                      }},
              });
            }
            break;
          case ChatMarkdownBlock::Kind::Paragraph:
            push_wrapped_row(ChatTranscriptRow::Kind::Body,
                             ChatTranscriptRow::Tone::Normal, message.role, {},
                             block.fragments);
            break;
        }
      }

      for (const ToolEvent& event : message.tool_events) {
        push_wrapped_row(ChatTranscriptRow::Kind::Tool, ChatTranscriptRow::Tone::Normal,
                         message.role, {},
                         {ChatInlineFragment{
                             .text = tool_event_summary(event),
                             .style = ChatTextStyle::Muted,
                             .link = std::nullopt,
                         }});
      }

      if (!message.error.empty() &&
          (message.status == RequestStatus::Failed ||
           message.status == RequestStatus::Cancelled)) {
        push_wrapped_row(ChatTranscriptRow::Kind::Error,
                         ChatTranscriptRow::Tone::Normal, message.role, {},
                         {ChatInlineFragment{
                             .text = "Error: " + message.error,
                             .style = ChatTextStyle::Strong,
                             .link = std::nullopt,
                         }});
      }

      layout.rows.push_back(ChatTranscriptRow{
          .kind = ChatTranscriptRow::Kind::Spacer,
          .tone = ChatTranscriptRow::Tone::Normal,
          .role = message.role,
          .prefix = {},
          .segments = {},
      });
    }
  }

  const auto list_layout =
      ComputeChatSidebarListLayout(sidebar_rect, layout.rows.size());
  const int scroll_row = list_layout.scroll_row;
  for (int row = 0; row < list_layout.visible_rows; ++row) {
    const int line_index = scroll_row + row;
    if (line_index >= static_cast<int>(layout.rows.size())) {
      break;
    }
    const ChatTranscriptRow& transcript_row =
        layout.rows[static_cast<std::size_t>(line_index)];
    if (transcript_row.kind == ChatTranscriptRow::Kind::Spacer) {
      continue;
    }

    const SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
    float text_x = row_rect.x + 6.0f;
    if (!transcript_row.prefix.empty()) {
      text_x += text_renderer_.MeasureWidth(transcript_row.prefix);
    }
    float segment_x = text_x;
    for (const ChatTranscriptSegment& segment : transcript_row.segments) {
      const float segment_width = text_renderer_.MeasureWidth(segment.text);
      if (segment.link.has_value() && segment_width > 0.0f) {
        layout.hit_regions.push_back(ChatTranscriptHitRegion{
            .rect = MakeRect(segment_x, row_rect.y + 2.0f, segment_width,
                             std::max(1.0f, row_rect.h - 4.0f)),
            .target = *segment.link,
        });
      }
      segment_x += segment_width;
    }
  }

  return layout;
}

std::size_t WorkspaceShell::ChatTranscriptLineCount(
    const SDL_FRect& sidebar_rect) const {
  return BuildChatTranscriptLayout(sidebar_rect).rows.size();
}

bool WorkspaceShell::HasChatTranscriptLinkAtPoint(const SDL_FRect& sidebar_rect,
                                                  float x,
                                                  float y) const {
  const ChatTranscriptLayout layout = BuildChatTranscriptLayout(sidebar_rect);
  return std::any_of(layout.hit_regions.begin(), layout.hit_regions.end(),
                     [&](const ChatTranscriptHitRegion& region) {
                       return Contains(region.rect, x, y);
                     });
}

bool WorkspaceShell::ActivateChatTranscriptLinkAtPoint(const SDL_FRect& sidebar_rect,
                                                       float x,
                                                       float y) {
  const ChatTranscriptLayout layout = BuildChatTranscriptLayout(sidebar_rect);
  const auto it = std::find_if(layout.hit_regions.begin(), layout.hit_regions.end(),
                               [&](const ChatTranscriptHitRegion& region) {
                                 return Contains(region.rect, x, y);
                               });
  if (it == layout.hit_regions.end()) {
    return false;
  }

  if (it->target.kind == ChatLinkTarget::Kind::RemoteUrl) {
    OpenExternalUrlPrompt(it->target.url);
    return true;
  }

  if (it->target.path.empty()) {
    return false;
  }
  OpenFile(it->target.path);
  if (editor::TextViewport* viewport = ActiveEditorViewport(); viewport != nullptr) {
    viewport->MoveCursorTo(it->target.line > 0 ? it->target.line - 1 : 0,
                           it->target.column > 0 ? it->target.column - 1 : 0);
  }
  context_.current_project_state.surface.focus = FocusTarget::Editor;
  return true;
}

std::vector<std::string> WorkspaceShell::ChatTranscriptDebugLines(
    const SDL_FRect& sidebar_rect) const {
  const ChatTranscriptLayout layout = BuildChatTranscriptLayout(sidebar_rect);
  std::vector<std::string> lines;
  lines.reserve(layout.rows.size());
  for (const ChatTranscriptRow& row : layout.rows) {
    std::string line = row.prefix;
    for (const ChatTranscriptSegment& segment : row.segments) {
      line += segment.text;
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

std::optional<SDL_FRect> WorkspaceShell::FindChatTranscriptLinkRect(
    const SDL_FRect& sidebar_rect,
    std::string_view match) const {
  const ChatTranscriptLayout layout = BuildChatTranscriptLayout(sidebar_rect);
  for (const ChatTranscriptHitRegion& region : layout.hit_regions) {
    ParsedChatLinkTarget debug_target;
    debug_target.remote = region.target.kind == ChatLinkTarget::Kind::RemoteUrl;
    debug_target.path = region.target.path;
    debug_target.url = region.target.url;
    debug_target.line = region.target.line;
    debug_target.column = region.target.column;
    if (ChatLinkDebugString(debug_target).find(match) != std::string::npos) {
      return region.rect;
    }
  }
  return std::nullopt;
}


}  // namespace microide::workspace
