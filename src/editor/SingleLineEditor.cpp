#include "editor/SingleLineEditor.h"

#include <algorithm>
#include <cctype>

#include "editor/EditTypes.h"
#include "util/StringUtil.h"

namespace microide::editor {

namespace {

// A single-line surface must never hold CR/LF. Rather than concatenating the
// lines (`foo\nbar` -> `foobar`, which silently corrupts a search needle / rename
// target), collapse each run of line breaks into a single space and drop leading/
// trailing runs, so `foo\nbar` -> `foo bar` and a whole-line-copy's trailing '\n'
// leaves no stray space. Returns empty when the input was only line breaks.
std::string CollapseLineBreaksToSpaces(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  bool pending_break = false;
  bool wrote_any = false;
  for (const char ch : input) {
    if (ch == '\r' || ch == '\n') {
      pending_break = true;
      continue;
    }
    if (pending_break && wrote_any) {
      out.push_back(' ');
    }
    pending_break = false;
    out.push_back(ch);
    wrote_any = true;
  }
  return out;
}

std::size_t ClampCaret(std::string_view text, std::size_t offset) {
  offset = std::min(offset, text.size());
  // Snap to a UTF-8 boundary: mouse hit-testing, tests, or plugin-driven surfaces
  // can request a caret in the middle of a multibyte character, and the next
  // insert/delete there would split the code point and corrupt the field. Walk
  // back off any continuation byte (0b10xxxxxx) to the start of its scalar.
  while (offset > 0 && offset < text.size() &&
         (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) {
    --offset;
  }
  return offset;
}

// Whether the code point beginning at byte `index` is identifier content. Unlike
// the byte-wise IsIdentifierByte this decodes the whole scalar, so non-ASCII
// letters (é, ä, CJK, …) count as word characters and word motion does not stop
// at every multibyte boundary.
bool IsIdentifierCodepointAt(std::string_view text, std::size_t index) {
  const std::size_t len = util::Utf8SequenceLength(text, index);
  const std::size_t clamped_len = (len == 0) ? 1 : len;
  return util::Utf8IsIdentifierCodepoint(
      util::DecodeUtf8Codepoint(text.substr(index, clamped_len)));
}

// Previous word edge: skip non-word code points immediately left of `caret`, then the word.
std::size_t WordBoundaryLeft(std::string_view text, std::size_t caret) {
  std::size_t index = std::min(caret, text.size());
  while (index > 0) {
    const std::size_t start = util::PreviousUtf8Boundary(text, index);
    if (IsIdentifierCodepointAt(text, start)) {
      break;
    }
    index = start;
  }
  while (index > 0) {
    const std::size_t start = util::PreviousUtf8Boundary(text, index);
    if (!IsIdentifierCodepointAt(text, start)) {
      break;
    }
    index = start;
  }
  return index;
}

// Next word edge: skip non-word code points at `caret`, then the word.
std::size_t WordBoundaryRight(std::string_view text, std::size_t caret) {
  std::size_t index = std::min(caret, text.size());
  const auto advance = [&]() {
    const std::size_t len = util::Utf8SequenceLength(text, index);
    index += (len == 0) ? 1 : len;
  };
  while (index < text.size() && !IsIdentifierCodepointAt(text, index)) {
    advance();
  }
  while (index < text.size() && IsIdentifierCodepointAt(text, index)) {
    advance();
  }
  return index;
}

}  // namespace

SingleLineEditor::SingleLineEditor(std::string text) {
  SetText(std::move(text));
}

SingleLineSnapshot SingleLineEditor::Snapshot() const {
  return SingleLineSnapshot{
      .text = text_,
      .caret = caret_,
      .selection_anchor = selection_anchor_,
  };
}

void SingleLineEditor::SetText(std::string text) {
  text_ = std::move(text);
  caret_ = text_.size();
  selection_anchor_.reset();
}

void SingleLineEditor::Append(std::string text) {
  if (text.empty()) {
    return;
  }
  // Share Insert's single-line sanitization: a single-line field must never store
  // CR/LF, even via this public helper. Collapse line-break runs to single spaces
  // so a future caller cannot smuggle control bytes past the field's invariant.
  if (text.find_first_of("\r\n") != std::string::npos) {
    text = CollapseLineBreaksToSpaces(text);
    if (text.empty()) {
      return;
    }
  }
  Normalize();
  text_.append(text);
  caret_ = text_.size();
  selection_anchor_.reset();
}

void SingleLineEditor::SetCaret(std::size_t caret) {
  caret_ = caret;
  Normalize();
}

void SingleLineEditor::SetSelectionAnchor(std::optional<std::size_t> selection_anchor) {
  selection_anchor_ = selection_anchor;
  Normalize();
}

std::optional<SingleLineSelection> SingleLineEditor::Selection() const {
  if (!selection_anchor_.has_value() || *selection_anchor_ == caret_) {
    return std::nullopt;
  }
  return SingleLineSelection{
      .start = std::min(*selection_anchor_, caret_),
      .end = std::max(*selection_anchor_, caret_),
  };
}

bool SingleLineEditor::HasSelection() const {
  return Selection().has_value();
}

std::string SingleLineEditor::SelectedText() const {
  const auto selection = Selection();
  if (!selection.has_value()) {
    return {};
  }
  return text_.substr(selection->start, selection->end - selection->start);
}

bool SingleLineEditor::Insert(std::string_view input) {
  if (input.empty()) {
    return false;
  }
  // A single-line surface must never hold line breaks. Clipboard paste (Ctrl+V, or
  // a paste delivered as a text-input event on some platforms — both funnel here)
  // can carry CR/LF: a whole-line copy includes a trailing '\n', and multi-line
  // clipboard content brings more. Inserting them raw stores control bytes that
  // render as garbage cells and corrupt the field's parsed value (search needle,
  // goto-line target, rename text, ...). Collapse line-break runs to single spaces
  // (so `foo\nbar` pastes as `foo bar`, not `foobar`) before inserting; typed
  // single characters never contain them, so normal input is unaffected. Recurse
  // once with the sanitized, newline-free text.
  if (input.find_first_of("\r\n") != std::string_view::npos) {
    const std::string sanitized = CollapseLineBreaksToSpaces(input);
    if (sanitized.empty()) {
      return false;
    }
    return Insert(sanitized);
  }
  Normalize();
  (void)DeleteSelection();
  text_.insert(caret_, input);
  caret_ += input.size();
  selection_anchor_.reset();
  return true;
}

bool SingleLineEditor::Backspace() {
  Normalize();
  if (DeleteSelection()) {
    return true;
  }
  if (caret_ == 0) {
    return false;
  }
  const std::size_t previous = util::PreviousUtf8Boundary(text_, caret_);
  text_.erase(previous, caret_ - previous);
  caret_ = previous;
  return true;
}

bool SingleLineEditor::DeleteForward() {
  Normalize();
  if (DeleteSelection()) {
    return true;
  }
  if (caret_ >= text_.size()) {
    return false;
  }
  const std::size_t next = util::NextUtf8Boundary(text_, caret_);
  text_.erase(caret_, next - caret_);
  return true;
}

bool SingleLineEditor::MoveLeft(bool extend_selection) {
  Normalize();
  if (!extend_selection && HasSelection()) {
    caret_ = Selection()->start;
    selection_anchor_.reset();
    return true;
  }
  if (caret_ == 0) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = util::PreviousUtf8Boundary(text_, caret_);
  Normalize();
  return true;
}

bool SingleLineEditor::MoveRight(bool extend_selection) {
  Normalize();
  if (!extend_selection && HasSelection()) {
    caret_ = Selection()->end;
    selection_anchor_.reset();
    return true;
  }
  if (caret_ >= text_.size()) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = util::NextUtf8Boundary(text_, caret_);
  Normalize();
  return true;
}

bool SingleLineEditor::MoveWordLeft(bool extend_selection) {
  Normalize();
  if (!extend_selection && HasSelection()) {
    caret_ = Selection()->start;
    selection_anchor_.reset();
    return true;
  }
  if (caret_ == 0) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = WordBoundaryLeft(text_, caret_);
  Normalize();
  return true;
}

bool SingleLineEditor::MoveWordRight(bool extend_selection) {
  Normalize();
  if (!extend_selection && HasSelection()) {
    caret_ = Selection()->end;
    selection_anchor_.reset();
    return true;
  }
  if (caret_ >= text_.size()) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = WordBoundaryRight(text_, caret_);
  Normalize();
  return true;
}

bool SingleLineEditor::DeleteWordLeft() {
  Normalize();
  if (DeleteSelection()) {
    return true;
  }
  if (caret_ == 0) {
    return false;
  }
  const std::size_t target = WordBoundaryLeft(text_, caret_);
  text_.erase(target, caret_ - target);
  caret_ = target;
  return true;
}

bool SingleLineEditor::DeleteWordRight() {
  Normalize();
  if (DeleteSelection()) {
    return true;
  }
  if (caret_ >= text_.size()) {
    return false;
  }
  const std::size_t target = WordBoundaryRight(text_, caret_);
  text_.erase(caret_, target - caret_);
  return true;
}

bool SingleLineEditor::MoveHome(bool extend_selection) {
  Normalize();
  if (caret_ == 0 && (!extend_selection || !HasSelection())) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = 0;
  Normalize();
  return true;
}

bool SingleLineEditor::MoveEnd(bool extend_selection) {
  Normalize();
  if (caret_ == text_.size() && (!extend_selection || !HasSelection())) {
    return false;
  }
  BeginSelectionIfNeeded(extend_selection);
  caret_ = text_.size();
  Normalize();
  return true;
}

bool SingleLineEditor::SelectWordAt(std::size_t byte_offset) {
  Normalize();
  const std::size_t clamped = std::min(byte_offset, text_.size());
  std::size_t anchor = clamped;
  if (clamped < text_.size() && IsIdentifierByte(text_[clamped])) {
    // primary position straddles a word character
  } else if (clamped > 0 && IsIdentifierByte(text_[clamped - 1])) {
    anchor = clamped - 1;
  } else {
    return false;
  }
  std::size_t start = anchor;
  std::size_t end = anchor;
  while (start > 0 && IsIdentifierByte(text_[start - 1])) {
    --start;
  }
  while (end < text_.size() && IsIdentifierByte(text_[end])) {
    ++end;
  }
  if (start >= end) {
    return false;
  }
  selection_anchor_ = start;
  caret_ = end;
  return true;
}

bool SingleLineEditor::SelectAll() {
  Normalize();
  if (text_.empty()) {
    caret_ = 0;
    selection_anchor_.reset();
    return false;
  }
  selection_anchor_ = 0;
  caret_ = text_.size();
  return true;
}

bool SingleLineEditor::DeleteSelection() {
  Normalize();
  const auto selection = Selection();
  if (!selection.has_value()) {
    return false;
  }
  text_.erase(selection->start, selection->end - selection->start);
  caret_ = selection->start;
  selection_anchor_.reset();
  return true;
}

std::string SingleLineEditor::CopySelection() const {
  return SelectedText();
}

std::optional<std::string> SingleLineEditor::CutSelection() {
  const std::string selection = SelectedText();
  if (selection.empty() || !DeleteSelection()) {
    return std::nullopt;
  }
  return selection;
}

bool SingleLineEditor::Paste(std::string_view text) {
  return Insert(text);
}

void SingleLineEditor::Normalize() {
  caret_ = ClampCaret(text_, caret_);
  if (selection_anchor_.has_value()) {
    *selection_anchor_ = ClampCaret(text_, *selection_anchor_);
    if (*selection_anchor_ == caret_) {
      selection_anchor_.reset();
    }
  }
}

void SingleLineEditor::ClearSelection() {
  selection_anchor_.reset();
}

void SingleLineEditor::BeginSelectionIfNeeded(bool extend_selection) {
  if (extend_selection) {
    if (!selection_anchor_.has_value()) {
      selection_anchor_ = caret_;
    }
    return;
  }
  ClearSelection();
}

}  // namespace microide::editor
