#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace microide::editor {

enum class WordClass : std::uint8_t {
  kWhitespace = 0,
  kWord,
  kSeparator,
};

[[nodiscard]] WordClass ClassifyWordCodepointAt(std::string_view text, std::size_t index);

[[nodiscard]] std::size_t WordBoundaryLeft(std::string_view text, std::size_t caret);

[[nodiscard]] std::size_t WordBoundaryRight(std::string_view text, std::size_t caret);

[[nodiscard]] std::size_t DeleteWordBoundaryLeft(std::string_view text, std::size_t caret);

[[nodiscard]] std::size_t DeleteWordBoundaryRight(std::string_view text, std::size_t caret);

struct WordSpan {
  std::size_t start = 0;
  std::size_t end = 0;

  [[nodiscard]] bool empty() const { return start >= end; }
};

[[nodiscard]] WordSpan IdentifierRunAt(std::string_view text, std::size_t index);

[[nodiscard]] WordSpan IdentifierRunTouching(std::string_view text, std::size_t index);

[[nodiscard]] WordSpan WordSelectionRunAt(std::string_view text, std::size_t index);

}
