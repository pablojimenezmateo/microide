#include "terminal/TerminalPredictionOverlay.h"

#include <string>

namespace microide::terminal {
namespace {

bool PredictableChar(const TerminalKeyPress& key) {
  return key.key == TerminalKeyPress::Key::Char && !key.ctrl && !key.alt && !key.super &&
         key.codepoint >= 0x20 && key.codepoint != 0x7f && key.codepoint <= 0x10ffff &&
         // Wide and combining glyphs would need the width tables; a narrow glyph
         // is the case that matters (typing at a prompt).
         key.codepoint < 0x1100;
}

bool PredictableErase(const TerminalKeyPress& key) {
  return key.key == TerminalKeyPress::Key::Backspace && !key.ctrl && !key.alt && !key.super &&
         !key.shift;
}

TerminalCell GlyphCell(char32_t codepoint) {
  std::string utf8;
  if (codepoint < 0x80) {
    utf8.push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    utf8.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    utf8.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    utf8.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    utf8.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  TerminalCell cell;
  cell.SetUtf8(utf8);
  return cell;
}

bool BlankCell(const TerminalCell& cell) {
  return cell.length == 0 || (cell.length == 1 && cell.bytes[0] == ' ');
}

}  // namespace

bool TerminalPredictionOverlay::drawing() const {
  switch (mode_) {
    case Mode::Never:
      return false;
    case Mode::Always:
      return !suppressed_;
    case Mode::Adaptive:
      return !suppressed_ && rtt_.has_value() && *rtt_ > kAdaptiveThreshold;
  }
  return false;
}

void TerminalPredictionOverlay::Clear() {
  pending_.clear();
  suppressed_ = false;
  blocked_until_ = 0;
  drawn_ = false;
}

bool TerminalPredictionOverlay::Typed(const TerminalInputEvent& event, View view) {
  if (mode_ == Mode::Never) {
    return false;
  }
  const bool key = event.kind == TerminalInputEvent::Kind::Key;
  const bool insert = key && PredictableChar(event.key);
  const bool erase = key && PredictableErase(event.key);
  if (!insert && !erase) {
    // Enter, an arrow, a paste, a click: where the cursor goes next is the
    // program's business.
    blocked_until_ = event.seq;
    return false;
  }
  if (blocked_until_ != 0 || pending_.size() >= kMaxPending) {
    return false;
  }
  pending_.push_back(Prediction{.seq = event.seq,
                                .erase = erase,
                                .glyph = insert ? GlyphCell(event.key.codepoint) : TerminalCell{}});
  const bool withdrew = Withdraw(view);
  return Place(view) || withdrew;
}

bool TerminalPredictionOverlay::Withdraw(View view) {
  if (!drawn_) {
    return false;
  }
  drawn_ = false;
  if (saved_row_ < view.lines.size()) {
    view.lines[saved_row_] = std::move(saved_line_);
  }
  saved_line_ = TerminalLine{};
  view.cursor_column = saved_cursor_column_;
  return true;
}

bool TerminalPredictionOverlay::Judge(std::uint64_t echo_ack, View view) {
  while (!pending_.empty() && pending_.front().seq <= echo_ack) {
    const Prediction prediction = pending_.front();
    pending_.pop_front();
    if (!prediction.placed || prediction.row_from_end == 0 ||
        prediction.row_from_end > view.lines.size()) {
      continue;  // never drawn, or its row is gone: nothing to judge against
    }
    const TerminalLine& line = view.lines[view.lines.size() - prediction.row_from_end];
    const TerminalCell shown =
        prediction.column < line.cells.size() ? line.cells[prediction.column] : TerminalCell{};
    const bool confirmed = prediction.erase
                               ? BlankCell(shown)
                               : shown.DisplayText() == prediction.glyph.DisplayText();
    if (confirmed) {
      suppressed_ = false;
      continue;
    }
    // The program did not echo what we drew (a password prompt, a key bound to
    // something else): nothing pending can be trusted either.
    pending_.clear();
    suppressed_ = true;
    break;
  }
  if (blocked_until_ != 0 && echo_ack >= blocked_until_) {
    blocked_until_ = 0;
  }
  return Place(view);
}

bool TerminalPredictionOverlay::Place(View view) {
  if (pending_.empty() || view.lines.empty()) {
    return false;
  }
  const bool draw = drawing() && view.cursor_visible && !view.alternate_screen &&
                    view.cursor_row < view.lines.size();
  std::size_t column = view.cursor_column;
  const std::size_t row = std::min(view.cursor_row, view.lines.size() - 1);
  for (Prediction& prediction : pending_) {
    prediction.placed = false;
    if (prediction.erase) {
      if (column == 0) {
        break;
      }
      --column;
    } else if (column + 1 >= view.columns) {
      break;  // the right margin: where the glyph lands is the program's wrap
    }
    prediction.column = column;
    prediction.row_from_end = view.lines.size() - row;
    prediction.placed = true;
    if (!prediction.erase) {
      ++column;
    }
  }
  if (!draw) {
    return false;
  }
  saved_row_ = row;
  saved_line_ = view.lines[row];
  saved_cursor_column_ = view.cursor_column;
  drawn_ = true;
  TerminalLine& line = view.lines[row];
  for (const Prediction& prediction : pending_) {
    if (!prediction.placed) {
      break;
    }
    if (line.cells.size() <= prediction.column) {
      line.cells.resize(prediction.column + 1);
    }
    // In the cell's own colours; a pending glyph is underlined, an erase is
    // simply blank.
    TerminalCell& cell = line.cells[prediction.column];
    const TerminalStyle style = cell.style;
    cell = prediction.erase ? TerminalCell{} : prediction.glyph;
    cell.style = style;
    cell.style.set(cell_attr::kUnderline, !prediction.erase);
    cell.style.set(cell_attr::kWideTrailing, false);
  }
  view.cursor_column = column;
  return true;
}

}  // namespace microide::terminal
