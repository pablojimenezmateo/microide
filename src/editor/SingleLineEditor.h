#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microide::editor {

struct SingleLineSelection {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct SingleLineSnapshot {
  std::string text;
  std::size_t caret = 0;
  std::optional<std::size_t> selection_anchor;
};

class SingleLineEditor {
 public:
  SingleLineEditor() = default;
  explicit SingleLineEditor(std::string text);

  const std::string& text() const { return text_; }
  std::size_t caret() const { return caret_; }
  std::optional<std::size_t> selection_anchor() const { return selection_anchor_; }
  SingleLineSnapshot Snapshot() const;

  void SetText(std::string text);
  void Append(std::string text);
  void SetCaret(std::size_t caret);
  void SetSelectionAnchor(std::optional<std::size_t> selection_anchor);

  std::optional<SingleLineSelection> Selection() const;
  bool HasSelection() const;
  std::string SelectedText() const;

  bool Insert(std::string_view input);
  bool Backspace();
  bool DeleteForward();
  bool MoveLeft(bool extend_selection = false);
  bool MoveRight(bool extend_selection = false);
  bool MoveHome(bool extend_selection = false);
  bool MoveEnd(bool extend_selection = false);
  bool SelectAll();
  bool DeleteSelection();
  std::string CopySelection() const;
  std::optional<std::string> CutSelection();
  bool Paste(std::string_view text);

 private:
  void Normalize();
  void ClearSelection();
  void BeginSelectionIfNeeded(bool extend_selection);

  std::string text_;
  std::size_t caret_ = 0;
  std::optional<std::size_t> selection_anchor_;
};

}  // namespace microide::editor
