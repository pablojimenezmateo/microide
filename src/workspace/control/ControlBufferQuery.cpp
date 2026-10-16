#include "workspace/control/ControlBufferQuery.h"

#include <cstdint>
#include <string>
#include <utility>

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"

namespace microide::workspace {

namespace {

util::JsonValue Int(std::size_t value) { return util::JsonValue(static_cast<std::int64_t>(value)); }

util::JsonValue OneBased(std::size_t value) { return Int(value + 1); }

util::JsonValue CaretJson(editor::TextPosition head, const std::optional<editor::TextPosition>& anchor) {
  util::JsonObject caret;
  caret["line"] = OneBased(head.line);
  caret["column"] = OneBased(head.column);
  const editor::TextPosition from = anchor.value_or(head);
  caret["anchorLine"] = OneBased(from.line);
  caret["anchorColumn"] = OneBased(from.column);
  caret["empty"] = util::JsonValue(from.line == head.line && from.column == head.column);
  return util::JsonValue(std::move(caret));
}

util::JsonValue FoldJson(const editor::FoldRange& range) {
  util::JsonArray pair;
  pair.push_back(OneBased(range.opener_line));
  pair.push_back(OneBased(range.closer_line));
  return util::JsonValue(std::move(pair));
}

}  // namespace

util::JsonValue BuildControlBufferJson(const editor::TextViewport* viewport, const char* kind,
                                       const ControlBufferQueryOptions& options) {
  util::JsonObject object;
  object["available"] = util::JsonValue(viewport != nullptr);
  if (viewport == nullptr) {
    return util::JsonValue(std::move(object));
  }
  object["kind"] = util::JsonValue(std::string(kind));
  object["path"] = util::JsonValue(viewport->path().generic_string());
  object["language"] = util::JsonValue(viewport->language_id());
  object["dirty"] = util::JsonValue(viewport->dirty());
  object["readOnly"] = util::JsonValue(viewport->read_only());
  object["softWrap"] = util::JsonValue(viewport->soft_wrap());
  object["tabSize"] = Int(viewport->tab_size());
  object["softTabs"] = util::JsonValue(viewport->soft_tabs());
  object["revision"] = util::JsonValue(static_cast<std::int64_t>(viewport->content_revision()));
  object["lineCount"] = Int(viewport->line_count());
  object["scrollLine"] = OneBased(viewport->scroll_line());
  object["visibleCount"] = Int(viewport->visible_lines());

  // The text, joined with '\n' (the buffer's own line ending is a save-time
  // property; `editor`/status report it).
  const editor::TextBuffer& lines = viewport->lines();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < lines.LineCount() && bytes <= options.max_text_bytes; ++i) {
    bytes += lines.LineLength(i) + 1;
  }
  if (bytes > options.max_text_bytes) {
    object["textTruncated"] = util::JsonValue(true);
  } else {
    std::string text;
    text.reserve(bytes);
    for (std::size_t i = 0; i < lines.LineCount(); ++i) {
      if (i > 0) {
        text.push_back('\n');
      }
      text.append(lines.LineView(i));
    }
    object["text"] = util::JsonValue(std::move(text));
  }

  // Every caret, primary first, then the secondaries in document order.
  util::JsonArray cursors;
  cursors.push_back(CaretJson(editor::TextPosition{viewport->cursor_line(), viewport->cursor_column()},
                              viewport->has_selection() ? viewport->selection_anchor()
                                                        : std::optional<editor::TextPosition>{}));
  for (const auto& caret : viewport->secondary_caret_range_view()) {
    if (cursors.size() >= options.max_cursors) {
      object["cursorsTruncated"] = util::JsonValue(true);
      break;
    }
    cursors.push_back(CaretJson(caret.position, caret.selection_anchor));
  }
  object["cursors"] = util::JsonValue(std::move(cursors));
  if (std::string selected = viewport->SelectedText(); selected.size() <= options.max_text_bytes) {
    object["selectedText"] = util::JsonValue(std::move(selected));
  } else {
    object["selectedTextTruncated"] = util::JsonValue(true);
  }

  util::JsonArray collapsed;
  util::JsonArray foldable;
  if (const editor::FoldingModel* folds = viewport->folding_model(); folds != nullptr) {
    for (const editor::FoldRange& range : folds->collapsed_ranges()) {
      collapsed.push_back(FoldJson(range));
    }
    for (const editor::FoldRange& range : folds->resolved_ranges()) {
      if (foldable.size() >= options.max_cursors) {
        break;
      }
      foldable.push_back(FoldJson(range));
    }
  }
  object["folded"] = util::JsonValue(std::move(collapsed));
  object["foldable"] = util::JsonValue(std::move(foldable));
  return util::JsonValue(std::move(object));
}

}  // namespace microide::workspace
