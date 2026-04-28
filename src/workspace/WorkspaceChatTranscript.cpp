#include "workspace/WorkspaceShell.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

enum class LinkParseContext {
  Markdown,
  PlainText,
};

struct ParsedChatLinkTarget {
  bool remote = false;
  std::filesystem::path path;
  std::string url;
  std::size_t line = 0;
  std::size_t column = 0;
};

std::uint64_t HashChatContent(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string_view TrimAscii(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

std::string_view TrimAsciiLeft(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  return text;
}

std::string BuildContinuationPrefix(std::string_view prefix) {
  return std::string(prefix.size(), ' ');
}

bool ParseFragmentLocation(std::string_view fragment,
                           std::size_t* line,
                           std::size_t* column) {
  if (line == nullptr || column == nullptr) {
    return false;
  }
  *line = 0;
  *column = 0;
  fragment = TrimAscii(fragment);
  if (fragment.empty()) {
    return false;
  }
  if (fragment.front() == 'L' || fragment.front() == 'l') {
    fragment.remove_prefix(1);
  }
  const std::size_t column_marker = fragment.find_first_of("Cc:");
  if (column_marker == std::string_view::npos) {
    const auto parsed = util::ParseSize(fragment);
    if (!parsed.has_value()) {
      return false;
    }
    *line = *parsed;
    return true;
  }

  const std::string_view line_text = fragment.substr(0, column_marker);
  std::string_view column_text = fragment.substr(column_marker + 1);
  if (!column_text.empty() &&
      (column_text.front() == 'C' || column_text.front() == 'c')) {
    column_text.remove_prefix(1);
  }
  const auto parsed_line = util::ParseSize(line_text);
  const auto parsed_column = util::ParseSize(column_text);
  if (!parsed_line.has_value() || !parsed_column.has_value()) {
    return false;
  }
  *line = *parsed_line;
  *column = *parsed_column;
  return true;
}

bool ParsePathLocationSuffix(std::string_view text,
                             std::string_view* path_text,
                             std::size_t* line,
                             std::size_t* column) {
  if (path_text == nullptr || line == nullptr || column == nullptr) {
    return false;
  }
  *path_text = text;
  *line = 0;
  *column = 0;

  const std::size_t last_colon = text.rfind(':');
  if (last_colon == std::string_view::npos || last_colon == 0 ||
      last_colon + 1 >= text.size()) {
    return false;
  }

  const auto parsed_tail = util::ParseSize(text.substr(last_colon + 1));
  if (!parsed_tail.has_value()) {
    return false;
  }

  const std::size_t second_last_colon = text.rfind(':', last_colon - 1);
  if (second_last_colon != std::string_view::npos && second_last_colon > 0) {
    const auto parsed_line = util::ParseSize(text.substr(second_last_colon + 1,
                                                         last_colon - second_last_colon - 1));
    if (parsed_line.has_value()) {
      *path_text = text.substr(0, second_last_colon);
      *line = *parsed_line;
      *column = *parsed_tail;
      return true;
    }
  }

  *path_text = text.substr(0, last_colon);
  *line = *parsed_tail;
  return true;
}

bool PathWithinRoot(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
  if (root.empty()) {
    return false;
  }
  const std::filesystem::path normalized_root = root.lexically_normal();
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path relative =
      normalized_candidate.lexically_relative(normalized_root);
  if (relative.empty()) {
    return normalized_candidate == normalized_root;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != "..";
}

std::optional<std::filesystem::path> DecodeFileUriPath(std::string_view encoded) {
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

std::optional<ParsedChatLinkTarget> ParseChatLinkTarget(
    std::string_view raw,
    const std::filesystem::path& project_root,
    LinkParseContext context) {
  raw = TrimAscii(raw);
  if (raw.empty()) {
    return std::nullopt;
  }

  if (raw.starts_with("http://") || raw.starts_with("https://")) {
    return ParsedChatLinkTarget{
        .remote = true,
        .path = {},
        .url = std::string(raw),
    };
  }
  if (raw.find("://") != std::string_view::npos && !raw.starts_with("file://")) {
    return std::nullopt;
  }

  std::size_t fragment_line = 0;
  std::size_t fragment_column = 0;
  std::string_view path_text = raw;
  if (!raw.starts_with("file://")) {
    if (const std::size_t hash = raw.rfind('#'); hash != std::string_view::npos) {
      path_text = raw.substr(0, hash);
      if (!ParseFragmentLocation(raw.substr(hash + 1), &fragment_line, &fragment_column)) {
        fragment_line = 0;
        fragment_column = 0;
      }
    }
  }

  std::size_t suffix_line = 0;
  std::size_t suffix_column = 0;
  std::string_view base_path_text = path_text;
  if (!raw.starts_with("file://")) {
    (void)ParsePathLocationSuffix(path_text, &base_path_text, &suffix_line, &suffix_column);
  }

  ParsedChatLinkTarget parsed;
  parsed.line = fragment_line != 0 ? fragment_line : suffix_line;
  parsed.column = fragment_column != 0 ? fragment_column : suffix_column;

  if (raw.starts_with("file://")) {
    std::string_view encoded = raw.substr(std::string_view("file://").size());
    if (encoded.starts_with("localhost/")) {
      encoded.remove_prefix(std::string_view("localhost").size());
    }
    if (const std::size_t hash = encoded.rfind('#'); hash != std::string_view::npos) {
      if (!ParseFragmentLocation(encoded.substr(hash + 1), &parsed.line, &parsed.column)) {
        parsed.line = 0;
        parsed.column = 0;
      }
      encoded = encoded.substr(0, hash);
    }
    const auto decoded = DecodeFileUriPath(encoded);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    parsed.path = decoded->lexically_normal();
    return parsed;
  }

  if (base_path_text.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path candidate(base_path_text);
  if (candidate.is_relative()) {
    if (project_root.empty()) {
      return std::nullopt;
    }
    parsed.path = (project_root / candidate).lexically_normal();
    if (!PathWithinRoot(project_root, parsed.path)) {
      return std::nullopt;
    }
  } else {
    parsed.path = candidate.lexically_normal();
  }

  if (context == LinkParseContext::PlainText && parsed.line == 0 &&
      parsed.column == 0) {
    return std::nullopt;
  }
  return parsed;
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

const WorkspaceShell::ChatMarkdownDocument& WorkspaceShell::ParsedChatMarkdown(
    std::string_view text) const {
  const std::uint64_t content_hash = HashChatContent(text);
  if (const auto it = chat_markdown_cache_.find(content_hash);
      it != chat_markdown_cache_.end()) {
    return it->second;
  }
  ChatMarkdownDocument document = ParseChatMarkdown(text);
  document.content_hash = content_hash;
  auto [it, inserted] =
      chat_markdown_cache_.emplace(content_hash, std::move(document));
  return it->second;
}

WorkspaceShell::ChatMarkdownDocument WorkspaceShell::ParseChatMarkdown(
    std::string_view text) const {
  ChatMarkdownDocument document;
  document.content_hash = HashChatContent(text);

  auto apply_style = [](std::vector<ChatInlineFragment>* fragments,
                        ChatTextStyle style) {
    if (fragments == nullptr) {
      return;
    }
    for (ChatInlineFragment& fragment : *fragments) {
      if (fragment.style != ChatTextStyle::InlineCode &&
          fragment.style != ChatTextStyle::Link) {
        fragment.style = style;
      }
    }
  };

  auto make_link_target = [](const ParsedChatLinkTarget& parsed) {
    ChatLinkTarget target;
    target.kind = parsed.remote ? ChatLinkTarget::Kind::RemoteUrl
                                : ChatLinkTarget::Kind::LocalFile;
    target.path = parsed.path;
    target.url = parsed.url;
    target.line = parsed.line;
    target.column = parsed.column;
    return target;
  };

  std::function<std::vector<ChatInlineFragment>(std::string_view)> parse_inline;
  parse_inline = [&](std::string_view line) -> std::vector<ChatInlineFragment> {
    std::vector<ChatInlineFragment> fragments;

    auto append_plain_text = [&](std::string_view raw_text) {
      std::size_t index = 0;
      while (index < raw_text.size()) {
        if (std::isspace(static_cast<unsigned char>(raw_text[index])) != 0) {
          std::size_t end = index + 1;
          while (end < raw_text.size() &&
                 std::isspace(static_cast<unsigned char>(raw_text[end])) != 0) {
            ++end;
          }
          fragments.push_back(ChatInlineFragment{
              .text = std::string(raw_text.substr(index, end - index)),
              .style = ChatTextStyle::Normal,
              .link = std::nullopt,
          });
          index = end;
          continue;
        }

        std::size_t end = index + 1;
        while (end < raw_text.size() &&
               std::isspace(static_cast<unsigned char>(raw_text[end])) == 0) {
          ++end;
        }

        std::string_view token = raw_text.substr(index, end - index);
        std::size_t prefix_length = 0;
        while (prefix_length < token.size() &&
               (token[prefix_length] == '(' || token[prefix_length] == '[' ||
                token[prefix_length] == '{' || token[prefix_length] == '<')) {
          ++prefix_length;
        }
        std::size_t suffix_length = 0;
        while (suffix_length < token.size() - prefix_length) {
          const char tail = token[token.size() - suffix_length - 1];
          if (tail == '.' || tail == ',' || tail == ';' || tail == '!' ||
              tail == '?' || tail == ')' || tail == ']' || tail == '}' ||
              tail == '>') {
            ++suffix_length;
            continue;
          }
          break;
        }

        const std::string_view leading = token.substr(0, prefix_length);
        const std::string_view core =
            token.substr(prefix_length, token.size() - prefix_length - suffix_length);
        const std::string_view trailing =
            token.substr(token.size() - suffix_length, suffix_length);

        if (!leading.empty()) {
          fragments.push_back(ChatInlineFragment{
              .text = std::string(leading),
              .style = ChatTextStyle::Normal,
              .link = std::nullopt,
          });
        }

        const auto parsed = ParseChatLinkTarget(core, context_.current_project_state.root,
                                                LinkParseContext::PlainText);
        if (parsed.has_value()) {
          fragments.push_back(ChatInlineFragment{
              .text = std::string(core),
              .style = ChatTextStyle::Link,
              .link = make_link_target(*parsed),
          });
        } else {
          fragments.push_back(ChatInlineFragment{
              .text = std::string(core),
              .style = ChatTextStyle::Normal,
              .link = std::nullopt,
          });
        }

        if (!trailing.empty()) {
          fragments.push_back(ChatInlineFragment{
              .text = std::string(trailing),
              .style = ChatTextStyle::Normal,
              .link = std::nullopt,
          });
        }
        index = end;
      }
    };

    std::string plain;
    auto flush_plain = [&]() {
      if (plain.empty()) {
        return;
      }
      append_plain_text(plain);
      plain.clear();
    };

    auto parse_wrapped_fragment = [&](std::string_view marker,
                                      ChatTextStyle style,
                                      std::size_t* index) -> bool {
      const std::size_t start = *index;
      const std::size_t close = line.find(marker, start + marker.size());
      if (close == std::string_view::npos || close == start + marker.size()) {
        return false;
      }
      flush_plain();
      std::vector<ChatInlineFragment> wrapped =
          parse_inline(line.substr(start + marker.size(),
                                  close - start - marker.size()));
      apply_style(&wrapped, style);
      fragments.insert(fragments.end(),
                       std::make_move_iterator(wrapped.begin()),
                       std::make_move_iterator(wrapped.end()));
      *index = close + marker.size();
      return true;
    };

    for (std::size_t index = 0; index < line.size();) {
      if (line[index] == '`') {
        const std::size_t close = line.find('`', index + 1);
        if (close != std::string_view::npos && close > index + 1) {
          flush_plain();
          fragments.push_back(ChatInlineFragment{
              .text = std::string(line.substr(index + 1, close - index - 1)),
              .style = ChatTextStyle::InlineCode,
              .link = std::nullopt,
          });
          index = close + 1;
          continue;
        }
      }

      if (line.compare(index, 2, "**") == 0 &&
          parse_wrapped_fragment("**", ChatTextStyle::Strong, &index)) {
        continue;
      }
      if (line.compare(index, 2, "__") == 0 &&
          parse_wrapped_fragment("__", ChatTextStyle::Strong, &index)) {
        continue;
      }
      if (line[index] == '*' &&
          parse_wrapped_fragment("*", ChatTextStyle::Emphasis, &index)) {
        continue;
      }
      if (line[index] == '_' &&
          parse_wrapped_fragment("_", ChatTextStyle::Emphasis, &index)) {
        continue;
      }

      if (line[index] == '[') {
        const std::size_t label_end = line.find(']', index + 1);
        if (label_end != std::string_view::npos && label_end + 1 < line.size() &&
            line[label_end + 1] == '(') {
          const std::size_t target_end = line.find(')', label_end + 2);
          if (target_end != std::string_view::npos) {
            const auto parsed = ParseChatLinkTarget(
                line.substr(label_end + 2, target_end - label_end - 2),
                context_.current_project_state.root, LinkParseContext::Markdown);
            if (parsed.has_value()) {
              flush_plain();
              std::vector<ChatInlineFragment> label_fragments =
                  parse_inline(line.substr(index + 1, label_end - index - 1));
              ChatLinkTarget target = make_link_target(*parsed);
              for (ChatInlineFragment& fragment : label_fragments) {
                fragment.link = target;
                if (fragment.style != ChatTextStyle::InlineCode) {
                  fragment.style = ChatTextStyle::Link;
                }
              }
              fragments.insert(fragments.end(),
                               std::make_move_iterator(label_fragments.begin()),
                               std::make_move_iterator(label_fragments.end()));
              index = target_end + 1;
              continue;
            }
          }
        }
      }

      plain.push_back(line[index]);
      ++index;
    }

    flush_plain();
    return fragments;
  };

  const std::vector<std::string> lines = util::SplitLines(text);
  for (std::size_t index = 0; index < lines.size();) {
    const std::string_view raw_line = lines[index];
    const std::string_view trimmed = TrimAscii(raw_line);
    if (trimmed.empty()) {
      ++index;
      continue;
    }

    if (TrimAsciiLeft(raw_line).starts_with("```")) {
      const std::size_t fence_index = index;
      std::vector<std::string> code_lines;
      ++index;
      bool closed = false;
      while (index < lines.size()) {
        if (TrimAsciiLeft(lines[index]).starts_with("```")) {
          closed = true;
          ++index;
          break;
        }
        code_lines.push_back(lines[index]);
        ++index;
      }
      if (!closed) {
        document.blocks.push_back(ChatMarkdownBlock{
            .kind = ChatMarkdownBlock::Kind::Paragraph,
            .level = 0,
            .ordered_list = false,
            .fragments = parse_inline(lines[fence_index]),
            .code_lines = {},
        });
        for (const std::string& code_line : code_lines) {
          document.blocks.push_back(ChatMarkdownBlock{
              .kind = ChatMarkdownBlock::Kind::Paragraph,
              .level = 0,
              .ordered_list = false,
              .fragments = parse_inline(code_line),
              .code_lines = {},
          });
        }
        continue;
      }
      document.blocks.push_back(ChatMarkdownBlock{
          .kind = ChatMarkdownBlock::Kind::CodeBlock,
          .level = 0,
          .ordered_list = false,
          .fragments = {},
          .code_lines = std::move(code_lines),
      });
      continue;
    }

    std::size_t heading_level = 0;
    while (heading_level < trimmed.size() && heading_level < 6 &&
           trimmed[heading_level] == '#') {
      ++heading_level;
    }
    if (heading_level > 0 && heading_level < trimmed.size() &&
        std::isspace(static_cast<unsigned char>(trimmed[heading_level])) != 0) {
      document.blocks.push_back(ChatMarkdownBlock{
          .kind = ChatMarkdownBlock::Kind::Heading,
          .level = heading_level,
          .ordered_list = false,
          .fragments = parse_inline(TrimAscii(trimmed.substr(heading_level))),
          .code_lines = {},
      });
      ++index;
      continue;
    }

    if (trimmed.starts_with(">")) {
      std::string_view quote = trimmed.substr(1);
      if (!quote.empty() &&
          std::isspace(static_cast<unsigned char>(quote.front())) != 0) {
        quote.remove_prefix(1);
      }
      document.blocks.push_back(ChatMarkdownBlock{
          .kind = ChatMarkdownBlock::Kind::Quote,
          .level = 0,
          .ordered_list = false,
          .fragments = parse_inline(quote),
          .code_lines = {},
      });
      ++index;
      continue;
    }

    bool handled_list = false;
    if (trimmed.size() > 2 &&
        (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
        std::isspace(static_cast<unsigned char>(trimmed[1])) != 0) {
      document.blocks.push_back(ChatMarkdownBlock{
          .kind = ChatMarkdownBlock::Kind::ListItem,
          .level = 0,
          .ordered_list = false,
          .fragments = parse_inline(TrimAscii(trimmed.substr(2))),
          .code_lines = {},
      });
      ++index;
      handled_list = true;
    } else {
      std::size_t cursor = 0;
      while (cursor < trimmed.size() &&
             std::isdigit(static_cast<unsigned char>(trimmed[cursor])) != 0) {
        ++cursor;
      }
      if (cursor > 0 && cursor + 1 < trimmed.size() && trimmed[cursor] == '.' &&
          std::isspace(static_cast<unsigned char>(trimmed[cursor + 1])) != 0) {
        const auto item_number = util::ParseSize(trimmed.substr(0, cursor));
        if (item_number.has_value()) {
          document.blocks.push_back(ChatMarkdownBlock{
              .kind = ChatMarkdownBlock::Kind::ListItem,
              .level = *item_number,
              .ordered_list = true,
              .fragments = parse_inline(TrimAscii(trimmed.substr(cursor + 2))),
              .code_lines = {},
          });
          ++index;
          handled_list = true;
        }
      }
    }
    if (handled_list) {
      continue;
    }

    document.blocks.push_back(ChatMarkdownBlock{
        .kind = ChatMarkdownBlock::Kind::Paragraph,
        .level = 0,
        .ordered_list = false,
        .fragments = parse_inline(trimmed),
        .code_lines = {},
    });
    ++index;
  }

  return document;
}

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
