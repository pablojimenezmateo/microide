#include "editor/SnippetEngine.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "util/StringUtil.h"

namespace microide::editor {

namespace {

bool IsDigit(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch)) != 0;
}

// Hard caps so a hostile or pathological snippet body cannot signed-overflow the
// tab-stop accumulator or make the parser allocate/loop unboundedly. On exceeding
// any cap ParseSnippetBody fails cleanly by returning an empty result (no
// expansion text, no occurrences); the caller treats that as "nothing to insert".
constexpr int kMaxTabStopId = 100000;
constexpr std::size_t kMaxBodyBytes = std::size_t{1} << 16;      // 64 KiB
constexpr std::size_t kMaxExpandedBytes = std::size_t{1} << 16;  // 64 KiB
constexpr std::size_t kMaxOccurrences = 4096;
constexpr std::size_t kMaxChoicesPerPlaceholder = 256;

}  // namespace

void SnippetSessionState::Reset(TextViewport* viewport_restore_secondary_to) {
  if (active && viewport_restore_secondary_to != nullptr && !saved_secondary_carets.empty()) {
    viewport_restore_secondary_to->SetSecondaryCarets(saved_secondary_carets);
  }
  *this = SnippetSessionState{};
}

SnippetParseResult ParseSnippetBody(std::string_view body) {
  SnippetParseResult result;
  result.expanded.clear();
  result.occurrences.clear();

  // Oversized bodies are rejected outright rather than parsed; every downstream
  // buffer (expanded text, occurrence list) is bounded by the body length.
  if (body.size() > kMaxBodyBytes) {
    return SnippetParseResult{};
  }

  for (std::size_t i = 0; i < body.size();) {
    if (body[i] != '$') {
      result.expanded += body[i];
      if (result.expanded.size() > kMaxExpandedBytes) {
        return SnippetParseResult{};
      }
      ++i;
      continue;
    }
    if (i + 1 < body.size() && body[i + 1] == '{') {
      const std::size_t j_start = i + 2;
      int tab = -1;
      std::size_t j = j_start;
      if (j < body.size() && IsDigit(body[j])) {
        tab = 0;
        while (j < body.size() && IsDigit(body[j])) {
          // Checked accumulation: `tab` is <= kMaxTabStopId before each step, so
          // tab*10 + digit cannot signed-overflow (stays well under INT_MAX).
          // Any id past the cap fails the whole parse cleanly.
          tab = tab * 10 + (body[j] - '0');
          if (tab > kMaxTabStopId) {
            return SnippetParseResult{};
          }
          ++j;
        }
      }
      if (tab < 0) {
        result.expanded += '$';
        ++i;
        continue;
      }
      std::string default_text;
      std::vector<std::string> choices;
      if (j < body.size() && body[j] == ':') {
        ++j;
        if (j < body.size() && body[j] == '|') {
          ++j;
          while (j < body.size()) {
            if (body[j] == '|') {
              ++j;
              break;
            }
            const std::size_t start = j;
            while (j < body.size() && body[j] != ',' && body[j] != '|') {
              ++j;
            }
            choices.emplace_back(body.substr(start, j - start));
            if (choices.size() > kMaxChoicesPerPlaceholder) {
              return SnippetParseResult{};
            }
            if (j < body.size() && body[j] == ',') {
              ++j;
            }
          }
          default_text = choices.empty() ? std::string{} : choices.front();
        } else {
          const std::size_t start = j;
          while (j < body.size() && body[j] != '}') {
            ++j;
          }
          default_text = std::string(body.substr(start, j - start));
        }
      }
      if (j < body.size() && body[j] == '}') {
        ++j;
      }

      SnippetParseResult::Occurrence occ;
      occ.tab_stop = tab;
      occ.start_off = result.expanded.size();
      result.expanded += default_text;
      if (result.expanded.size() > kMaxExpandedBytes) {
        return SnippetParseResult{};
      }
      occ.end_off = result.expanded.size();
      occ.is_final = tab == 0;
      occ.choices = std::move(choices);
      result.occurrences.push_back(std::move(occ));
      if (result.occurrences.size() > kMaxOccurrences) {
        return SnippetParseResult{};
      }
      i = j;
      continue;
    }
    if (i + 1 < body.size() && IsDigit(body[i + 1])) {
      const int tab = body[i + 1] - '0';
      SnippetParseResult::Occurrence occ;
      occ.tab_stop = tab;
      occ.start_off = result.expanded.size();
      occ.end_off = result.expanded.size();
      occ.is_final = tab == 0;
      result.occurrences.push_back(std::move(occ));
      if (result.occurrences.size() > kMaxOccurrences) {
        return SnippetParseResult{};
      }
      i += 2;
      continue;
    }
    result.expanded += '$';
    ++i;
  }
  return result;
}

TextPosition PositionAfterOffsetInExpanded(TextPosition trigger_start,
                                           std::string_view expanded_flat,
                                           std::size_t offset) {
  std::size_t line = trigger_start.line;
  std::size_t col = trigger_start.column;
  for (std::size_t k = 0; k < offset && k < expanded_flat.size(); ++k) {
    const char c = expanded_flat[k];
    if (c == '\r') {
      // ReplaceRange inserts NormalizeLineEndings(expanded), which collapses both
      // a lone \r and a \r\n pair to a single \n. Mirror that here so tab-stop
      // positions computed against the raw body land on the inserted text: treat
      // \r as a line break and swallow a following \n (within the counted range).
      ++line;
      col = 0;
      if (k + 1 < offset && k + 1 < expanded_flat.size() && expanded_flat[k + 1] == '\n') {
        ++k;
      }
    } else if (c == '\n') {
      ++line;
      col = 0;
    } else {
      ++col;
    }
  }
  return TextPosition{line, col};
}

static std::vector<int> BuildNavigateOrder(const std::vector<SnippetParseResult::Occurrence>& occ) {
  std::set<int> tabs;
  for (const auto& o : occ) {
    tabs.insert(o.tab_stop);
  }
  std::vector<int> positives;
  for (int t : tabs) {
    if (t != 0) {
      positives.push_back(t);
    }
  }
  std::sort(positives.begin(), positives.end());
  if (tabs.count(0) != 0) {
    positives.push_back(0);
  }
  return positives;
}

static void FocusTabRange(TextViewport& viewport, const SelectionRange& r) {
  viewport.ClearSelection();
  if (r.start.line == r.end.line && r.start.column == r.end.column) {
    viewport.MoveCursorTo(r.start.line, r.start.column, false);
    return;
  }
  viewport.MoveCursorTo(r.start.line, r.start.column, false);
  viewport.MoveCursorTo(r.end.line, r.end.column, true);
}

static void FocusTabStop(TextViewport& viewport, SnippetSessionState& session, int tab) {
  const auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end() || it->second.empty()) {
    return;
  }
  FocusTabRange(viewport, it->second.front());
}

static const SelectionRange* RangeContaining(const std::vector<SelectionRange>& ranges,
                                             const TextPosition& p) {
  for (const auto& r : ranges) {
    if (r.start.line != r.end.line) {
      continue;
    }
    if (p.line != r.start.line) {
      continue;
    }
    if (p.column >= r.start.column && p.column <= r.end.column) {
      return &r;
    }
  }
  return nullptr;
}

static std::size_t RelativeColumnInRange(const SelectionRange& r, const TextPosition& p) {
  if (p.line != r.start.line) {
    return 0;
  }
  return p.column - r.start.column;
}

// Shift every recorded placeholder range — across ALL tab stops, not just the one
// being edited — that begins at/after `at` on the same line by `delta` columns. An
// edit inside one placeholder moves the text of every later placeholder on that
// line; without shifting the OTHER tab stops' recorded ranges too, FocusTabStop
// would later jump to a stale column. `skip_tab`/`skip_index` names the range the
// edit happened inside (its own end is adjusted by the caller).
static void ShiftPlaceholdersAtOrAfter(SnippetSessionState& session, const TextPosition& at,
                                       std::ptrdiff_t delta, int skip_tab,
                                       std::size_t skip_index) {
  for (auto& [tab, vec] : session.ranges_by_tab) {
    for (std::size_t j = 0; j < vec.size(); ++j) {
      if (tab == skip_tab && j == skip_index) {
        continue;
      }
      SelectionRange& other = vec[j];
      if (other.start.line != at.line || other.start.column < at.column) {
        continue;
      }
      const auto shift = [delta](std::size_t& column) {
        column = static_cast<std::size_t>(
            std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(column) + delta));
      };
      shift(other.start.column);
      shift(other.end.column);
    }
  }
}

static void ApplyChoiceForTab(TextViewport& viewport,
                              SnippetSessionState& session,
                              int tab,
                              const std::string& text) {
  auto& ranges = session.ranges_by_tab[tab];
  std::vector<std::size_t> order(ranges.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto& ra = ranges[a];
    const auto& rb = ranges[b];
    if (ra.start.line != rb.start.line) {
      return ra.start.line > rb.start.line;
    }
    return ra.start.column > rb.start.column;
  });
  for (std::size_t idx : order) {
    SelectionRange r = ranges[idx];
    viewport.ReplaceRange(r, text, false);
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(text.size()) -
                                 (static_cast<std::ptrdiff_t>(r.end.column) -
                                  static_cast<std::ptrdiff_t>(r.start.column));
    ranges[idx].end.column = r.start.column + text.size();
    // A choice can change the placeholder's length; every OTHER placeholder after
    // this mirror on the same line (a different tab stop, or a sibling mirror) must
    // shift by that delta or its recorded range goes stale. `r.end` is the pre-edit
    // end, so start >= r.end catches exactly what moved.
    ShiftPlaceholdersAtOrAfter(session, r.end, delta, tab, idx);
  }
}

bool ExpandSnippetAtSelection(TextViewport& viewport,
                              SnippetSessionState& session,
                              const SelectionRange& trigger_range,
                              std::string_view snippet_body) {
  session.Reset(&viewport);

  const SnippetParseResult parsed = ParseSnippetBody(snippet_body);
  const SelectionRange trigger = TextViewport::NormalizeRange(trigger_range);

  session.saved_secondary_carets = viewport.secondary_carets();
  viewport.ClearSecondaryCarets();

  if (!viewport.ReplaceRange(trigger, parsed.expanded, true)) {
    if (!session.saved_secondary_carets.empty()) {
      viewport.SetSecondaryCarets(session.saved_secondary_carets);
    }
    session.saved_secondary_carets.clear();
    if (viewport.UndoGroupActive()) {
      viewport.EndUndoGroup();
    }
    return false;
  }

  if (parsed.occurrences.empty()) {
    session.navigate_order.clear();
    session.navigate_index = 0;
    session.active = false;
    CommitSnippetSession(viewport, session);
    return true;
  }

  for (const auto& occ : parsed.occurrences) {
    const TextPosition a = PositionAfterOffsetInExpanded(trigger.start, parsed.expanded, occ.start_off);
    const TextPosition b = PositionAfterOffsetInExpanded(trigger.start, parsed.expanded, occ.end_off);
    session.ranges_by_tab[occ.tab_stop].push_back(SelectionRange{a, b});
    if (!occ.choices.empty()) {
      session.choices_by_tab[occ.tab_stop] = occ.choices;
      session.choice_index_by_tab[occ.tab_stop] = 0;
    }
  }

  session.navigate_order = BuildNavigateOrder(parsed.occurrences);
  session.navigate_index = 0;
  session.active = true;

  while (session.navigate_index < session.navigate_order.size() &&
         session.navigate_order[session.navigate_index] == 0) {
    ++session.navigate_index;
  }

  if (!session.navigate_order.empty() && session.navigate_index < session.navigate_order.size()) {
    FocusTabStop(viewport, session, session.navigate_order[session.navigate_index]);
  } else if (session.ranges_by_tab.count(0) != 0) {
    FocusTabStop(viewport, session, 0);
    CommitSnippetSession(viewport, session);
  } else {
    CommitSnippetSession(viewport, session);
  }
  return true;
}

void CommitSnippetSession(TextViewport& viewport, SnippetSessionState& session) {
  session.Reset(&viewport);
  if (viewport.UndoGroupActive()) {
    viewport.EndUndoGroup();
  }
}

static bool CaretInsideCurrentTab(const TextViewport& viewport, const SnippetSessionState& session) {
  if (!session.active || session.navigate_index >= session.navigate_order.size()) {
    return true;
  }
  const int tab = session.navigate_order[session.navigate_index];
  const auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end()) {
    return false;
  }
  const TextPosition p{viewport.cursor_line(), viewport.cursor_column()};
  return RangeContaining(it->second, p) != nullptr;
}

void SnippetOnCaretMoved(TextViewport& viewport, SnippetSessionState& session) {
  if (!session.active) {
    return;
  }
  if (CaretInsideCurrentTab(viewport, session)) {
    return;
  }
  CommitSnippetSession(viewport, session);
}

bool SnippetNavigateTab(TextViewport& viewport, SnippetSessionState& session, bool backward) {
  if (!session.active || session.navigate_order.empty()) {
    return false;
  }
  if (session.navigate_index >= session.navigate_order.size()) {
    return false;
  }
  const int cur = session.navigate_order[session.navigate_index];

  if (!backward) {
    const auto ch_it = session.choices_by_tab.find(cur);
    if (ch_it != session.choices_by_tab.end() && ch_it->second.size() > 1) {
      std::size_t& idx = session.choice_index_by_tab[cur];
      if (idx + 1 < ch_it->second.size()) {
        ++idx;
        ApplyChoiceForTab(viewport, session, cur, ch_it->second[idx]);
        FocusTabStop(viewport, session, cur);
        return true;
      }
    }

    std::size_t next = session.navigate_index + 1;
    while (next < session.navigate_order.size() && session.navigate_order[next] == 0) {
      ++next;
    }
    if (next >= session.navigate_order.size()) {
      if (session.ranges_by_tab.count(0) != 0) {
        FocusTabStop(viewport, session, 0);
      }
      CommitSnippetSession(viewport, session);
      return true;
    }
    session.navigate_index = next;
    const int ntab = session.navigate_order[session.navigate_index];
    if (ntab == 0) {
      FocusTabStop(viewport, session, 0);
      CommitSnippetSession(viewport, session);
      return true;
    }
    FocusTabStop(viewport, session, ntab);
    return true;
  }

  // Shift+Tab: if can cycle choice back
  const auto ch_it = session.choices_by_tab.find(cur);
  if (ch_it != session.choices_by_tab.end() && ch_it->second.size() > 1) {
    std::size_t& idx = session.choice_index_by_tab[cur];
    if (idx > 0) {
      --idx;
      ApplyChoiceForTab(viewport, session, cur, ch_it->second[idx]);
      FocusTabStop(viewport, session, cur);
      return true;
    }
  }
  if (session.navigate_index == 0) {
    return true;
  }
  --session.navigate_index;
  const int ptab = session.navigate_order[session.navigate_index];
  FocusTabStop(viewport, session, ptab);
  return true;
}

bool SnippetHandleEscape(TextViewport& viewport, SnippetSessionState& session) {
  if (!session.active) {
    return false;
  }
  CommitSnippetSession(viewport, session);
  return true;
}

bool SnippetTryInsertText(TextViewport& viewport, SnippetSessionState& session, std::string_view text) {
  if (!session.active || text.empty()) {
    return false;
  }
  // The mirror fast path only knows how to shift columns on a single line: it
  // advances end.column by text.size() and shifts later same-line placeholders.
  // A payload containing a line break (a raw '\n', or a '\r'/'\r\n' that
  // ReplaceRange would normalize to a newline) would make every following
  // placeholder's line/column ranges stale. Decline the fast path so the caller
  // falls back to normal editing (VSCode likewise drops the linked-edit session
  // on a multi-line insert). Returning false does not double-insert: the caller
  // performs the single normal insert itself.
  if (text.find('\n') != std::string_view::npos ||
      text.find('\r') != std::string_view::npos) {
    return false;
  }
  if (session.navigate_index >= session.navigate_order.size()) {
    return false;
  }
  const int tab = session.navigate_order[session.navigate_index];
  const auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end() || it->second.empty()) {
    return false;
  }
  const TextPosition p{viewport.cursor_line(), viewport.cursor_column()};
  const SelectionRange* ref = RangeContaining(it->second, p);
  if (ref == nullptr) {
    return false;
  }
  const std::size_t rel = RelativeColumnInRange(*ref, p);

  std::vector<std::size_t> order(it->second.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  auto& ranges = it->second;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto& ra = ranges[a];
    const auto& rb = ranges[b];
    if (ra.start.line != rb.start.line) {
      return ra.start.line > rb.start.line;
    }
    return ra.start.column + rel > rb.start.column + rel;
  });

  for (std::size_t idx : order) {
    const TextPosition ins{ranges[idx].start.line, ranges[idx].start.column + rel};
    viewport.ReplaceRange(SelectionRange{ins, ins}, text, false);
    ranges[idx].end.column += text.size();
    // The insertion shifts every column at or after `ins` on this line to the right.
    // Every OTHER placeholder to the right — whether a mirror of THIS tab stop or a
    // different tab stop sharing the line — must have BOTH its start and end columns
    // advanced, or its recorded range goes stale and FocusTabStop later jumps to the
    // wrong column.
    ShiftPlaceholdersAtOrAfter(session, ins, static_cast<std::ptrdiff_t>(text.size()), tab, idx);
  }
  FocusTabStop(viewport, session, tab);
  return true;
}

bool SnippetTryBackspace(TextViewport& viewport, SnippetSessionState& session) {
  if (!session.active) {
    return false;
  }
  if (session.navigate_index >= session.navigate_order.size()) {
    return false;
  }
  const int tab = session.navigate_order[session.navigate_index];
  auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end() || it->second.empty()) {
    return false;
  }
  const TextPosition p{viewport.cursor_line(), viewport.cursor_column()};
  if (p.column == 0) {
    return false;
  }
  // Backspace removes the whole UTF-8 code point ENDING at the caret. Scan back
  // to its start so multi-byte code points (é, emoji) are never split mid-byte.
  // The caret line is always a valid buffer line (cursor_line() is bounded).
  const std::string& caret_line = viewport.lines()[p.line];
  const std::size_t cp_start = util::PreviousUtf8Boundary(caret_line, p.column);
  const TextPosition prev{p.line, cp_start};
  const SelectionRange* ref = RangeContaining(it->second, prev);
  if (ref == nullptr) {
    return false;
  }
  // Relative byte offset of the code point's START within the placeholder; every
  // linked mirror holds identical bytes, so this maps onto the same code point.
  const std::size_t rel_del = RelativeColumnInRange(*ref, prev);

  auto& ranges = it->second;
  std::vector<std::size_t> order(ranges.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto& ra = ranges[a];
    const auto& rb = ranges[b];
    if (ra.start.line != rb.start.line) {
      return ra.start.line > rb.start.line;
    }
    return ra.start.column + rel_del > rb.start.column + rel_del;
  });

  for (std::size_t idx : order) {
    const auto& r = ranges[idx];
    const TextPosition del{r.start.line, r.start.column + rel_del};
    if (del.column >= r.end.column) {
      continue;
    }
    // Delete the FULL code point starting at `del` in this mirror. Mirrors hold
    // identical bytes, so its width matches the reference code point; recompute it
    // per mirror from that mirror's own line so nothing is split mid-byte. Clamp
    // to the placeholder end so the span never spills past the mirror.
    const std::string& mirror_line = viewport.lines()[del.line];
    std::size_t width = util::NextUtf8Boundary(mirror_line, del.column) - del.column;
    if (width < 1) {
      width = 1;
    }
    if (del.column + width > r.end.column) {
      width = r.end.column - del.column;
    }
    viewport.ReplaceRange(
        SelectionRange{del, TextPosition{del.line, del.column + width}}, "", false);
    ranges[idx].end.column -= width;
    // Mirror SnippetTryInsertText: the deletion pulls every column at/after `del`
    // on this line `width` bytes to the left, so every OTHER placeholder there (a
    // same-tab mirror or a different tab stop sharing the line) must shift too, or
    // its recorded range goes stale and FocusTabStop later jumps to the wrong column.
    ShiftPlaceholdersAtOrAfter(session, del, -static_cast<std::ptrdiff_t>(width), tab, idx);
  }
  FocusTabStop(viewport, session, tab);
  return true;
}

bool SnippetTryDeleteForward(TextViewport& viewport, SnippetSessionState& session) {
  if (!session.active) {
    return false;
  }
  if (session.navigate_index >= session.navigate_order.size()) {
    return false;
  }
  const int tab = session.navigate_order[session.navigate_index];
  auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end() || it->second.empty()) {
    return false;
  }
  const TextPosition p{viewport.cursor_line(), viewport.cursor_column()};
  const SelectionRange* ref = RangeContaining(it->second, p);
  if (ref == nullptr) {
    return false;
  }
  if (p.column >= ref->end.column) {
    return false;
  }
  const std::size_t rel_del = RelativeColumnInRange(*ref, p);
  auto& ranges = it->second;
  std::vector<std::size_t> order(ranges.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const auto& ra = ranges[a];
    const auto& rb = ranges[b];
    if (ra.start.line != rb.start.line) {
      return ra.start.line > rb.start.line;
    }
    return ra.start.column + rel_del > rb.start.column + rel_del;
  });
  for (std::size_t idx : order) {
    const auto& r = ranges[idx];
    const TextPosition del{r.start.line, r.start.column + rel_del};
    if (del.column >= r.end.column) {
      continue;
    }
    // Delete-forward removes the whole UTF-8 code point STARTING at `del`. Compute
    // its byte width from this mirror's own line and clamp to the placeholder end
    // so a multi-byte code point is deleted whole, never split mid-byte.
    const std::string& mirror_line = viewport.lines()[del.line];
    std::size_t width = util::NextUtf8Boundary(mirror_line, del.column) - del.column;
    if (width < 1) {
      width = 1;
    }
    if (del.column + width > r.end.column) {
      width = r.end.column - del.column;
    }
    viewport.ReplaceRange(
        SelectionRange{del, TextPosition{del.line, del.column + width}}, "", false);
    ranges[idx].end.column -= width;
    // See SnippetTryBackspace: shift every other placeholder at/after `del` on this
    // line left by `width` so cross-tab and sibling-mirror ranges stay accurate.
    ShiftPlaceholdersAtOrAfter(session, del, -static_cast<std::ptrdiff_t>(width), tab, idx);
  }
  FocusTabStop(viewport, session, tab);
  return true;
}

}  // namespace microide::editor
