#include "editor/WordBoundary.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::editor {
namespace {

std::size_t ScalarLengthAt(std::string_view text, std::size_t index) {
  const std::size_t len = util::Utf8SequenceLength(text, index);
  return len == 0 ? 1 : len;
}

// Round `caret` down to the start of the code point containing it.
//
// Every caret this tree produces is already on a boundary (`ClampTextColumn`
// snaps, and so does `SingleLineEditor::Normalize`), so this is defensive rather
// than load-bearing -- but these are public entry points now, and a right-scan
// starting mid-scalar would classify a continuation byte as a separator and hand
// back a boundary INSIDE a character. As the end of a delete range that splits
// the character.
std::size_t SnapToScalarStart(std::string_view text, std::size_t caret) {
  std::size_t index = std::min(caret, text.size());
  while (index > 0 && index < text.size() &&
         util::IsUtf8ContinuationByte(static_cast<unsigned char>(text[index]))) {
    --index;
  }
  return index;
}

// End of the maximal run of same-class code points containing `index`.
std::size_t SameClassRunEnd(std::string_view text, std::size_t index) {
  const WordClass cls = ClassifyWordCodepointAt(text, index);
  std::size_t end = index;
  while (end < text.size() && ClassifyWordCodepointAt(text, end) == cls) {
    end += ScalarLengthAt(text, end);
  }
  return end;
}

// Start of the maximal run of same-class code points containing `index`.
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

}  // namespace

WordClass ClassifyWordCodepointAt(std::string_view text, std::size_t index) {
  if (index >= text.size()) {
    return WordClass::kWhitespace;
  }
  const char lead = text[index];
  // ASCII fast path: the overwhelmingly common case, and it avoids decoding a
  // scalar per code point on a per-keystroke path.
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
    // Only a run that ENDS at the caret counts. A caret two columns past `foo`
    // touches no word, and reporting `foo` there would highlight an identifier
    // the caret has left.
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
  // Not on identifier content: take the gap between the nearest non-whitespace
  // runs. One scan each way, both bounded by the neighbouring run -- no
  // whole-line pass, because a double-click on a minified line must not walk it.
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
  // A caret inside a separator run sees the same run on both sides, so the two
  // bounds come back crossed; normalising them yields that run, which is what
  // VS Code's `new Range(...)` does with its swapped columns.
  if (start > end) {
    std::swap(start, end);
  }
  return WordSpan{start, end};
}

}  // namespace microide::editor
