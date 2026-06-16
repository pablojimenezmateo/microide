#include "architecture/ArchitectureFileScanner.h"

#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <utility>

namespace microide::tests::architecture {

std::filesystem::path RepoRoot() {
  return std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).lexically_normal().parent_path();
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

std::size_t LineNumberAt(std::string_view text, std::size_t offset) {
  std::size_t line = 1;
  for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
    }
  }
  return line;
}

enum class LexState {
  Code,
  LineComment,
  BlockComment,
  StringLiteral,
  CharLiteral,
};

bool IsIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

std::vector<bool> BuildCodeMask(std::string_view text) {
  std::vector<bool> is_code(text.size(), true);
  LexState state = LexState::Code;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char n = (i + 1 < text.size()) ? text[i + 1] : '\0';
    switch (state) {
      case LexState::Code:
        if (c == '/' && n == '/') {
          is_code[i] = false;
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::LineComment;
          continue;
        }
        if (c == '/' && n == '*') {
          is_code[i] = false;
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::BlockComment;
          continue;
        }
        if (c == '"') {
          is_code[i] = false;
          state = LexState::StringLiteral;
          continue;
        }
        if (c == '\'') {
          is_code[i] = false;
          state = LexState::CharLiteral;
          continue;
        }
        break;
      case LexState::LineComment:
        is_code[i] = false;
        if (c == '\n') {
          state = LexState::Code;
        }
        break;
      case LexState::BlockComment:
        is_code[i] = false;
        if (c == '*' && n == '/') {
          if (i + 1 < text.size()) {
            is_code[i + 1] = false;
          }
          ++i;
          state = LexState::Code;
        }
        break;
      case LexState::StringLiteral:
        is_code[i] = false;
        if (c == '\\' && i + 1 < text.size()) {
          is_code[i + 1] = false;
          ++i;
          continue;
        }
        if (c == '"') {
          state = LexState::Code;
        }
        break;
      case LexState::CharLiteral:
        is_code[i] = false;
        if (c == '\\' && i + 1 < text.size()) {
          is_code[i + 1] = false;
          ++i;
          continue;
        }
        if (c == '\'') {
          state = LexState::Code;
        }
        break;
    }
  }
  return is_code;
}

std::size_t CountCodeLines(std::string_view text) {
  const std::vector<bool> is_code = BuildCodeMask(text);
  std::size_t count = 0;
  bool line_has_code = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n') {
      if (line_has_code) {
        ++count;
      }
      line_has_code = false;
      continue;
    }
    if (is_code[i] && !std::isspace(static_cast<unsigned char>(c))) {
      line_has_code = true;
    }
  }
  if (line_has_code) {
    ++count;
  }
  return count;
}

std::size_t CountCodeLinesInFile(const std::filesystem::path& path) {
  const std::string text = ReadText(path);
  return CountCodeLines(std::string_view(text));
}

bool MatchesCodeAt(std::string_view text,
                   const std::vector<bool>& is_code,
                   std::size_t pos,
                   std::string_view needle) {
  if (pos + needle.size() > text.size()) {
    return false;
  }
  if (pos > 0 && IsIdentChar(text[pos - 1])) {
    return false;
  }
  if (pos + needle.size() < text.size() && IsIdentChar(text[pos + needle.size()])) {
    return false;
  }
  for (std::size_t i = 0; i < needle.size(); ++i) {
    if (!is_code[pos + i] || text[pos + i] != needle[i]) {
      return false;
    }
  }
  return true;
}

std::vector<std::size_t> FindCodeLiteralOccurrences(std::string_view text,
                                                    std::string_view literal) {
  std::vector<std::size_t> offsets;
  if (literal.empty()) {
    return offsets;
  }
  const auto is_code = BuildCodeMask(text);
  std::size_t pos = text.find(literal);
  while (pos != std::string::npos) {
    bool in_code = true;
    for (std::size_t i = 0; i < literal.size(); ++i) {
      if (pos + i >= is_code.size() || !is_code[pos + i]) {
        in_code = false;
        break;
      }
    }
    if (in_code) {
      offsets.push_back(pos);
    }
    pos = text.find(literal, pos + 1);
  }
  return offsets;
}

std::vector<std::size_t> FindTryCatchStoViolations(std::string_view text) {
  std::vector<std::size_t> violations;
  const auto is_code = BuildCodeMask(text);
  std::size_t i = 0;
  while (i < text.size()) {
    if (!MatchesCodeAt(text, is_code, i, "try")) {
      ++i;
      continue;
    }
    std::size_t j = i + 3;
    while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) {
      ++j;
    }
    if (j >= text.size() || text[j] != '{' || !is_code[j]) {
      i = j;
      continue;
    }

    std::size_t depth = 1;
    std::size_t k = j + 1;
    bool has_sto = false;
    while (k < text.size() && depth > 0) {
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stoi") {
        has_sto = true;
      }
      if (is_code[k] && k + 10 <= text.size() && text.substr(k, 10) == "std::stoll") {
        has_sto = true;
      }
      if (is_code[k] && k + 11 <= text.size() && text.substr(k, 11) == "std::stoull") {
        has_sto = true;
      }
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stof") {
        has_sto = true;
      }
      if (is_code[k] && k + 9 <= text.size() && text.substr(k, 9) == "std::stod") {
        has_sto = true;
      }
      if (is_code[k] && text[k] == '{') {
        ++depth;
      } else if (is_code[k] && text[k] == '}') {
        --depth;
      }
      ++k;
    }
    std::size_t after = k;
    while (after < text.size() && std::isspace(static_cast<unsigned char>(text[after]))) {
      ++after;
    }
    if (has_sto && MatchesCodeAt(text, is_code, after, "catch")) {
      violations.push_back(i);
    }
    // Advance one token at a time so nested try/catch blocks are also scanned.
    ++i;
  }
  return violations;
}

// Build a per-byte mask flagging positions inside `#ifdef MICROIDE_TESTING` blocks.
// Test-only backdoor access (e.g. `friend struct TestAccess`) is an accepted exception
// to the workspace no-friend policy; everything outside those guards is still policed.
std::vector<bool> BuildTestingGuardMask(const std::string& text) {
  std::vector<bool> mask(text.size(), false);
  std::size_t depth = 0;
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const std::size_t line_end = text.find('\n', cursor);
    const std::size_t end = (line_end == std::string::npos) ? text.size() : line_end;
    std::size_t scan = cursor;
    while (scan < end && std::isspace(static_cast<unsigned char>(text[scan]))) {
      ++scan;
    }
    if (scan < end && text[scan] == '#') {
      ++scan;
      while (scan < end && std::isspace(static_cast<unsigned char>(text[scan]))) {
        ++scan;
      }
      const std::string_view line(text.data() + scan, end - scan);
      if (line.starts_with("ifdef MICROIDE_TESTING") ||
          line.starts_with("if defined(MICROIDE_TESTING)")) {
        ++depth;
      } else if (depth > 0 && line.starts_with("endif")) {
        --depth;
      }
    }
    if (depth > 0) {
      for (std::size_t i = cursor; i < end; ++i) {
        mask[i] = true;
      }
    }
    if (line_end == std::string::npos) {
      break;
    }
    cursor = line_end + 1;
  }
  return mask;
}

std::optional<std::string> ExtractBraceDelimitedBody(const std::string& text,
                                                     std::size_t open_brace_index) {
  if (open_brace_index >= text.size() || text[open_brace_index] != '{') {
    return std::nullopt;
  }
  const auto is_code = BuildCodeMask(text);
  std::size_t depth = 0;
  for (std::size_t i = open_brace_index; i < text.size(); ++i) {
    if (i < is_code.size() && !is_code[i]) {
      continue;
    }
    if (text[i] == '{') {
      ++depth;
    } else if (text[i] == '}') {
      --depth;
      if (depth == 0) {
        return text.substr(open_brace_index + 1, i - open_brace_index - 1);
      }
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::size_t>> ExtractMemberFunctionBodyWithOffset(
    const std::string& text, std::string_view signature_needle) {
  const std::size_t sig = text.find(signature_needle);
  if (sig == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t open = text.find('{', sig);
  if (open == std::string::npos || open + 1 >= text.size()) {
    return std::nullopt;
  }
  const auto body = ExtractBraceDelimitedBody(text, open);
  if (!body.has_value()) {
    return std::nullopt;
  }
  return std::pair<std::string, std::size_t>{*body, open + 1};
}

}  // namespace microide::tests::architecture
