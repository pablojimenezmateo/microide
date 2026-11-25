#include "editor/WordBoundary.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::editor {
namespace {

std::size_t ScalarLengthAt(std::string_view text, std::size_t index) {
  const std::size_t len = util::Utf8SequenceLength(text, index);
  return len == 0 ? 1 : len;
}

std::size_t SnapToScalarStart(std::string_view text, std::size_t caret) {
  std::size_t index = std::min(caret, text.size());
  while (index > 0 && index < text.size() &&
         util::IsUtf8ContinuationByte(static_cast<unsigned char>(text[index]))) {
    --index;
  }
  return index;
}

std::size_t SameClassRunEnd(std::string_view text, std::size_t index) {
  const WordClass cls = ClassifyWordCodepointAt(text, index);
  std::size_t end = index;
  while (end < text.size() && ClassifyWordCodepointAt(text, end) == cls) {
    end += ScalarLengthAt(text, end);
  }
  return end;
}

std::size_t SameClassRunStart(std::string_view text, std::size_t index) {
  const WordClass cls = ClassifyWordCodepointAt(text, index);
  std::size_t start = index;
  while (start > 0) {
    const std::size_t previous = util::PreviousUtf8Boundary(text, start);
    if (ClassifyWordCodepointAt(text, previous) != cls) {
      break;
    }
    start = previous;
  }
  return start;
}

}

WordClass ClassifyWordCodepointAt(std::string_view text, std::size_t index) {
  if (index >= text.size()) {
    return WordClass::kWhitespace;
  }
  const char lead = text[index];
  if (static_cast<unsigned char>(lead) < 0x80) {
    if (util::IsAsciiSpace(static_cast<unsigned char>(lead))) {
      return WordClass::kWhitespace;
    }
    return (util::IsAsciiAlnum(static_cast<unsigned char>(lead)) || lead == '_')
               ? WordClass::kWord
               : WordClass::kSeparator;
  }
  const char32_t codepoint =
      util::DecodeUtf8Codepoint(text.substr(index, ScalarLengthAt(text, index)));
  return util::Utf8IsIdentifierCodepoint(codepoint) ? WordClass::kWord : WordClass::kSeparator;
}

std::size_t WordBoundaryLeft(std::string_view text, std::size_t caret) {
  std::size_t index = SnapToScalarStart(text, caret);
  while (index > 0) {
    const std::size_t start = util::PreviousUtf8Boundary(text, index);
    if (ClassifyWordCodepointAt(text, start) != WordClass::kWhitespace) {
      break;
    }
    index = start;
  }
  if (index == 0) {
    return 0;
  }
  const WordClass run =
      ClassifyWordCodepointAt(text, util::PreviousUtf8Boundary(text, index));
  while (index > 0) {
    const std::size_t start = util::PreviousUtf8Boundary(text, index);
    if (ClassifyWordCodepointAt(text, start) != run) {
      break;
    }
    index = start;
  }
  return index;
}

std::size_t WordBoundaryRight(std::string_view text, std::size_t caret) {
  std::size_t index = SnapToScalarStart(text, caret);
  while (index < text.size() &&
         ClassifyWordCodepointAt(text, index) == WordClass::kWhitespace) {
    index += ScalarLengthAt(text, index);
  }
  if (index >= text.size()) {
    return text.size();
  }
  const WordClass run = ClassifyWordCodepointAt(text, index);
  while (index < text.size() && ClassifyWordCodepointAt(text, index) == run) {
    index += ScalarLengthAt(text, index);
  }
  return index;
}

std::size_t DeleteWordBoundaryLeft(std::string_view text, std::size_t caret) {
  std::size_t whitespace_start = SnapToScalarStart(text, caret);
  std::size_t whitespace_count = 0;
  while (whitespace_start > 0) {
    const std::size_t start = util::PreviousUtf8Boundary(text, whitespace_start);
    if (ClassifyWordCodepointAt(text, start) != WordClass::kWhitespace) {
      break;
    }
    whitespace_start = start;
    ++whitespace_count;
  }
  if (whitespace_count >= 2) {
    return whitespace_start;
  }
  return WordBoundaryLeft(text, caret);
}

WordSpan IdentifierRunAt(std::string_view text, std::size_t index) {
  if (index >= text.size()) {
    return WordSpan{};
  }
  index = SnapToScalarStart(text, index);
  if (ClassifyWordCodepointAt(text, index) != WordClass::kWord) {
    return WordSpan{};
  }
  std::size_t start = index;
  while (start > 0) {
    const std::size_t previous = util::PreviousUtf8Boundary(text, start);
    if (ClassifyWordCodepointAt(text, previous) != WordClass::kWord) {
      break;
    }
    start = previous;
  }
  std::size_t end = index;
  while (end < text.size() && ClassifyWordCodepointAt(text, end) == WordClass::kWord) {
    end += ScalarLengthAt(text, end);
  }
  return WordSpan{start, end};
}

std::size_t DeleteWordBoundaryRight(std::string_view text, std::size_t caret) {
  std::size_t whitespace_end = SnapToScalarStart(text, caret);
  std::size_t whitespace_count = 0;
  while (whitespace_end < text.size() &&
         ClassifyWordCodepointAt(text, whitespace_end) == WordClass::kWhitespace) {
    whitespace_end += ScalarLengthAt(text, whitespace_end);
    ++whitespace_count;
  }
  if (whitespace_count >= 2) {
    return whitespace_end;
  }
  return WordBoundaryRight(text, caret);
}

WordSpan IdentifierRunTouching(std::string_view text, std::size_t index) {
  index = SnapToScalarStart(text, index);
  WordSpan span = IdentifierRunAt(text, index);
  if (span.empty() && index > 0) {
    span = IdentifierRunAt(text, util::PreviousUtf8Boundary(text, index));
    if (span.end != index) {
      return WordSpan{};
    }
  }
  return span;
}

WordSpan WordSelectionRunAt(std::string_view text, std::size_t index) {
  index = SnapToScalarStart(text, index);
  if (const WordSpan word = IdentifierRunTouching(text, index); !word.empty()) {
    return word;
  }
  std::size_t start = 0;
  for (std::size_t probe = index; probe > 0;) {
    probe = util::PreviousUtf8Boundary(text, probe);
    if (ClassifyWordCodepointAt(text, probe) != WordClass::kWhitespace) {
      start = SameClassRunEnd(text, probe);
      break;
    }
  }
  std::size_t end = text.size();
  for (std::size_t probe = index; probe < text.size();) {
    if (ClassifyWordCodepointAt(text, probe) != WordClass::kWhitespace) {
      end = SameClassRunStart(text, probe);
      break;
    }
    probe += ScalarLengthAt(text, probe);
  }
  if (start > end) {
    std::swap(start, end);
  }
  return WordSpan{start, end};
}

}
