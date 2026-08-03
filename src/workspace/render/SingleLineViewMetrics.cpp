#include "workspace/render/SingleLineViewMetrics.h"

#include <algorithm>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "render/TextRenderer.h"
#include "util/StringUtil.h"

namespace microide::workspace {

SingleLineViewMetrics ComputeSingleLineViewMetrics(const render::TextRenderer& text_renderer,
                                                   const editor::SingleLineEditor& state,
                                                   std::string_view prefix,
                                                   float available_width) {
  // The displayed text is the virtual concatenation of `prefix` (a tiny ASCII
  // decorator, "" or "> ") and the editor body. We never materialize that
  // concatenation: the prefix is always a complete string, so no codepoint ever
  // straddles the seam, and we can address the two segments by global byte offset.
  const std::string_view body = state.text();
  const std::size_t prefix_len = prefix.size();
  const std::size_t total_len = prefix_len + body.size();
  const std::size_t cursor_byte = std::min(prefix_len + state.caret(), total_len);

  // View of the virtual text [start, end) when it lies within a single segment.
  const auto segment_view = [&](std::size_t start, std::size_t end) -> std::string_view {
    if (end <= prefix_len) {
      return prefix.substr(start, end - start);
    }
    return body.substr(start - prefix_len, end - start);
  };
  // Next UTF-8 boundary in the virtual text. The seam (== prefix_len) is itself a
  // valid boundary because the prefix is a complete string.
  const auto next_boundary = [&](std::size_t pos) -> std::size_t {
    if (pos < prefix_len) {
      return util::NextUtf8Boundary(prefix, pos);
    }
    return prefix_len + util::NextUtf8Boundary(body, pos - prefix_len);
  };

  // Measure each codepoint from 0..cursor_byte exactly once, storing (start, width).
  // Walking backward through this array to find view_start avoids re-measuring.
  struct CharEntry {
    std::size_t start;
    float width;
  };
  // Reuse capacity across calls; multiple text-input surfaces (overlay, prompt, sidebar) each call
  // this helper per frame, and reallocating the inner vector ~5×/frame is wasted work.
  thread_local std::vector<CharEntry> before_cursor_scratch;
  std::vector<CharEntry>& before_cursor = before_cursor_scratch;
  before_cursor.clear();
  before_cursor.reserve(64);
  for (std::size_t pos = 0; pos < cursor_byte;) {
    const std::size_t next = next_boundary(pos);
    before_cursor.push_back({pos, text_renderer.MeasureWidth(segment_view(pos, next))});
    pos = next;
  }

  // Walk backward through stored widths to find the leftmost byte that still lets
  // [view_start..cursor] fit in available_width — no extra MeasureWidth calls needed.
  float cursor_x = 0.0f;
  std::size_t view_start_idx = before_cursor.size();
  for (auto i = before_cursor.size(); i > 0; --i) {
    if (cursor_x + before_cursor[i - 1].width > available_width) {
      break;
    }
    cursor_x += before_cursor[i - 1].width;
    view_start_idx = i - 1;
  }
  // If not even the single glyph immediately left of the caret fits (a field
  // narrower than one codepoint), view_start_idx stays == size(); indexing it
  // would read past the end (reserve()'d capacity → uninitialized CharEntry, UB).
  // Start the view at the caret in that case (empty left side).
  const std::size_t view_start =
      view_start_idx < before_cursor.size() ? before_cursor[view_start_idx].start : cursor_byte;

  // Walk forward from cursor_byte, measuring each codepoint once, until full.
  float right_accum = cursor_x;
  std::size_t view_end = cursor_byte;
  while (view_end < total_len) {
    const std::size_t next = next_boundary(view_end);
    const float char_w = text_renderer.MeasureWidth(segment_view(view_end, next));
    if (right_accum + char_w > available_width) {
      break;
    }
    right_accum += char_w;
    view_end = next;
  }

  std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
  if (const auto sel = state.Selection(); sel.has_value()) {
    const std::size_t sel_start_full = prefix.size() + sel->start;
    const std::size_t sel_end_full = prefix.size() + sel->end;
    if (sel_start_full < view_end && sel_end_full > view_start) {
      const std::size_t clamped_start = std::max(sel_start_full, view_start) - view_start;
      const std::size_t clamped_end = std::min(sel_end_full, view_end) - view_start;
      if (clamped_start < clamped_end) {
        selection_bytes = {clamped_start, clamped_end};
      }
    }
  }

  // Assemble the visible slice. It is the function's owned output payload (it
  // escapes in TextInputVisual), so a copy here is required, not a hot-path
  // temporary. Only the rare prefix+body straddle needs two appends.
  std::string displayed_text;
  if (view_end > view_start) {
    if (view_end <= prefix_len || view_start >= prefix_len) {
      displayed_text.assign(segment_view(view_start, view_end));
    } else {
      displayed_text.reserve(view_end - view_start);
      displayed_text.append(prefix.substr(view_start));
      displayed_text.append(body.substr(0, view_end - prefix_len));
    }
  }

  return SingleLineViewMetrics{
      .displayed_text = std::move(displayed_text),
      .cursor_x = cursor_x,
      .selection_bytes = selection_bytes,
  };
}

}  // namespace microide::workspace
