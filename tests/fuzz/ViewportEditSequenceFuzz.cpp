#include "editor/TextViewport.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using microide::editor::TextViewport;

struct Cursor {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t pos = 0;

  std::uint8_t Byte() { return pos < size ? data[pos++] : 0; }
  std::size_t Bounded(std::size_t bound) { return bound == 0 ? 0 : Byte() % bound; }
  bool Done() const { return pos >= size; }
};

constexpr std::string_view kPieces[] = {
    "a", "word word word word", "\t", "\n", "\n\n", "x\ny", "\xc3\xa9", "\xe4\xbd\xa0\xe5\xa5\xbd",
    "{", "}", "        ", "a very long run of text that wraps at any narrow width at all", "", "\r\n", "(", ")"};

std::string Text(const TextViewport& view) {
  std::string out;
  view.lines().AppendWholeText(out);
  return out;
}

[[noreturn]] void Fail(const char* what, const std::string& detail) {
  std::fprintf(stderr, "ViewportEditSequenceFuzz: %s\n%s\n", what, detail.c_str());
  std::abort();
}

void CheckCaches(const std::vector<TextViewport>& views, const char* op) {
  for (const TextViewport& view : views) {
    const std::string problem = view.VerifyDerivedCachesForTesting();
    if (!problem.empty()) {
      Fail(op, problem);
    }
  }
}

bool TraceEnabled() {
  static const bool enabled = std::getenv("MICROIDE_FUZZ_TRACE") != nullptr;
  return enabled;
}

void Trace(std::size_t view, const char* op, std::size_t line, std::size_t column, const TextViewport& state) {
  if (TraceEnabled()) {
    std::fprintf(stderr, "view %zu %s at %zu:%zu -> %s\n", view, op, line, column, Text(state).c_str());
  }
}

void LayOut(TextViewport& view) {
  for (std::size_t row = 0; row < 24; ++row) {
    (void)view.VisibleWrappedRowLayoutRef(row);
  }
  (void)view.max_visual_columns();
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  Cursor cursor{data, size};
  std::string content;
  for (std::size_t line = 0, lines = 1 + cursor.Bounded(10); line < lines; ++line) {
    if (line > 0) {
      content.push_back('\n');
    }
    for (std::size_t piece = cursor.Bounded(4); piece > 0; --piece) {
      content += kPieces[cursor.Bounded(std::size(kPieces))];
    }
  }
  std::vector<TextViewport> views(1);
  views[0].SetViewportSize(12, 10 + cursor.Bounded(60));
  views[0].LoadContent(content, "/tmp/views.txt");
  const std::string initial = Text(views[0]);
  for (std::size_t i = 1, count = 2 + cursor.Bounded(3); i < count; ++i) {
    views.push_back(views[0]);
  }
  for (TextViewport& view : views) {
    view.SetViewportSize(12, 8 + cursor.Bounded(70));
    view.SetSoftWrap(cursor.Bounded(3) != 0);
  }
  for (int step = 0; step < 64 && !cursor.Done(); ++step) {
    const std::size_t index = cursor.Bounded(views.size());
    TextViewport& view = views[index];
    const std::size_t line = cursor.Bounded(view.line_count());
    const std::size_t column = cursor.Bounded(view.lines().LineLength(line) + 1);
    const char* op = "";
    switch (cursor.Bounded(13)) {
      case 0:
      case 1:
      case 2:
        view.MoveCursorTo(line, column);
        view.InsertText(kPieces[cursor.Bounded(std::size(kPieces))]);
        op = "insert";
        break;
      case 3:
        view.MoveCursorTo(line, column);
        view.InsertNewline();
        op = "newline";
        break;
      case 4:
        view.MoveCursorTo(line, column);
        view.Backspace();
        op = "backspace";
        break;
      case 5:
        view.MoveCursorTo(line, column);
        view.DeleteForward();
        op = "delete";
        break;
      case 6: {
        view.MoveCursorTo(line, column);
        const std::size_t to_line = cursor.Bounded(view.line_count());
        view.MoveCursorTo(to_line, cursor.Bounded(view.lines().LineLength(to_line) + 1), true);
        view.InsertText(kPieces[cursor.Bounded(std::size(kPieces))]);
        op = "replace selection";
        break;
      }
      case 7: {
        const std::string before = Text(view);
        if (view.Undo() && Text(view) == before) {
          Fail("an undo that reports a change left the text as it was", before);
        }
        op = "undo";
        break;
      }
      case 8: {
        const std::string before = Text(view);
        if (view.Redo() && Text(view) == before) {
          Fail("a redo that reports a change left the text as it was", before);
        }
        op = "redo";
        break;
      }
      case 9:
        view.SetSoftWrap(!view.soft_wrap());
        op = "toggle wrap";
        break;
      case 10:
        view.SetViewportSize(12, 8 + cursor.Bounded(70));
        op = "resize";
        break;
      case 11:
        view.MoveCursorTo(line, column);
        view.AddSecondaryCaret(cursor.Bounded(view.line_count()), 0);
        view.InsertText(kPieces[cursor.Bounded(std::size(kPieces))]);
        view.ClearSecondaryCarets();
        op = "multi-caret insert";
        break;
      default:
        view.SetTabSize(1 + cursor.Bounded(8));
        op = "tab size";
        break;
    }
    for (TextViewport& other : views) {
      if ((cursor.Byte() & 1u) != 0u) {
        LayOut(other);
      }
    }
    Trace(index, op, line, column, view);
    CheckCaches(views, op);
  }
  TextViewport& view = views[0];
  int guard = 0;
  while (view.Redo() && ++guard < 4096) {
  }
  const std::string final_text = Text(view);
  guard = 0;
  while (view.Undo() && ++guard < 4096) {
  }
  CheckCaches(views, "undo all");
  if (Text(view) != initial) {
    Fail("undoing every edit does not give back the loaded text", initial + "\n--- got ---\n" + Text(view));
  }
  guard = 0;
  while (view.Redo() && ++guard < 4096) {
  }
  CheckCaches(views, "redo all");
  if (Text(view) != final_text) {
    Fail("redoing every edit does not give back the newest edit's text", final_text + "\n--- got ---\n" + Text(view));
  }
  return 0;
}
