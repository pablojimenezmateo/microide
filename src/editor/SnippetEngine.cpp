#include "editor/SnippetEngine.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>
#include <utility>

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
            // Honor escapes inside a choice value: `\,`, `\|`, `\}`, and `\\`
            // insert the literal character instead of ending the choice, so a
            // choice like `${1|a\,b,c|}` yields the two options "a,b" and "c".
            std::string value;
            while (j < body.size() && body[j] != ',' && body[j] != '|') {
              if (body[j] == '\\' && j + 1 < body.size() &&
                  (body[j + 1] == ',' || body[j + 1] == '|' || body[j + 1] == '}' ||
                   body[j + 1] == '\\')) {
                value += body[j + 1];
                j += 2;
              } else {
                value += body[j];
                ++j;
              }
            }
            choices.emplace_back(std::move(value));
            // A choice value must stay single-line: ApplyChoiceForTab records the
            // post-cycle range as `start.column + text.size()` on one line, so a
            // choice carrying a '\n'/'\r' (ReplaceRange would add a line) leaves a
            // stale off-line range and orphans the wrapped text on the next cycle.
            // Reject the malformed snippet (matching VSCode, whose choices cannot
            // contain newlines, and the other parse-cap failure sentinels here).
            if (choices.back().find('\n') != std::string::npos ||
                choices.back().find('\r') != std::string::npos) {
              return SnippetParseResult{};
            }
            if (choices.size() > kMaxChoicesPerPlaceholder) {
              return SnippetParseResult{};
            }
            if (j < body.size() && body[j] == ',') {
              ++j;
            }
          }
          default_text = choices.empty() ? std::string{} : choices.front();
        } else {
          // Honor VSCode-style escapes inside the default text: `\}`, `\$`, and
          // `\\` insert the literal character instead of terminating the
          // placeholder. Without this, a default containing a brace or dollar
          // (e.g. `${1:obj\}}`) truncated at the first raw `}`.
          std::string dt;
          while (j < body.size() && body[j] != '}') {
            if (body[j] == '\\' && j + 1 < body.size() &&
                (body[j + 1] == '}' || body[j + 1] == '$' || body[j + 1] == '\\')) {
              dt += body[j + 1];
              j += 2;
            } else {
              dt += body[j];
              ++j;
            }
          }
          default_text = std::move(dt);
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
      // Bare `$N` tab stop: read ALL consecutive digits. VSCode treats `$10` as
      // tab stop 10, not tab stop 1 followed by a literal '0'; reading a single
      // digit here diverged from that and from the braced `${N}` form above.
      // Share that form's checked accumulation and overflow cap.
      int tab = 0;
      std::size_t j = i + 1;
      while (j < body.size() && IsDigit(body[j])) {
        tab = tab * 10 + (body[j] - '0');
        if (tab > kMaxTabStopId) {
          return SnippetParseResult{};
        }
        ++j;
      }
      SnippetParseResult::Occurrence occ;
      occ.tab_stop = tab;
      occ.start_off = result.expanded.size();
      occ.end_off = result.expanded.size();
      occ.is_final = tab == 0;
      result.occurrences.push_back(std::move(occ));
      if (result.occurrences.size() > kMaxOccurrences) {
        return SnippetParseResult{};
      }
      i = j;
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

// One applied mirror edit of the currently-edited tab: `range_index` names its
// range in the edited tab's vector, `origin_col` is the column on `line` at/after
// which text shifts, and `delta` is the signed byte size change.
struct AppliedMirrorEdit {
  std::size_t range_index = 0;
  std::size_t line = 0;
  std::size_t origin_col = 0;
  std::ptrdiff_t delta = 0;
};

// Fold every mirror edit's column shift into every recorded placeholder range in
// one pass, replacing the old per-edit ShiftPlaceholdersAtOrAfter scan (which
// re-walked ALL tab stops and ALL placeholder ranges once per mirror, so a
// snippet with many mirrors or many tab stops on one line was
// O(active_mirrors * total_placeholders) per keystroke — TD-2026-07-17A-060).
//
// The mirror edits of one tab are disjoint and stay ordered under the shifts, so
// each placeholder endpoint's total shift is the sum of deltas of same-line edits
// whose `origin_col` is <= the endpoint's governing (start) column — independent
// of the high-to-low application order, exactly matching the incremental result.
// Start and end move together (linked mirrors keep their width); the edited tab's
// own mirror additionally grows/shrinks by its own delta, and never shifts its
// own start on its own edit.
static void ApplyBatchedMirrorShifts(SnippetSessionState& session, int edited_tab,
                                     const std::vector<AppliedMirrorEdit>& edits) {
  if (edits.empty()) {
    return;
  }
  // Per-line sorted (origin_col -> running prefix-sum of deltas at/left of it).
  std::unordered_map<std::size_t, std::vector<std::pair<std::size_t, std::ptrdiff_t>>> by_line;
  for (const AppliedMirrorEdit& e : edits) {
    by_line[e.line].emplace_back(e.origin_col, e.delta);
  }
  for (auto& [line, events] : by_line) {
    std::sort(events.begin(), events.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::ptrdiff_t acc = 0;
    for (auto& ev : events) {
      acc += ev.second;
      ev.second = acc;  // sum of deltas with origin_col <= ev.first
    }
  }
  // Sum of deltas whose origin_col <= col on `line` (0 when the line has none).
  const auto shift_at = [&](std::size_t line, std::size_t col) -> std::ptrdiff_t {
    const auto it = by_line.find(line);
    if (it == by_line.end()) {
      return 0;
    }
    const auto& events = it->second;
    std::size_t lo = 0;
    std::size_t hi = events.size();
    while (lo < hi) {
      const std::size_t mid = (lo + hi) / 2;
      if (events[mid].first <= col) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo > 0 ? events[lo - 1].second : 0;
  };
  // The edited tab's mirrors, keyed by range index, with their own edit footprint.
  std::unordered_map<std::size_t, std::pair<std::size_t, std::ptrdiff_t>> own_edit;
  for (const AppliedMirrorEdit& e : edits) {
    own_edit[e.range_index] = {e.origin_col, e.delta};
  }
  const auto shift_column = [](std::size_t& column, std::ptrdiff_t delta) {
    column = static_cast<std::size_t>(
        std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(column) + delta));
  };
  for (auto& [tab, vec] : session.ranges_by_tab) {
    for (std::size_t j = 0; j < vec.size(); ++j) {
      SelectionRange& r = vec[j];
      std::ptrdiff_t start_shift = shift_at(r.start.line, r.start.column);
      std::ptrdiff_t own_delta = 0;
      if (tab == edited_tab) {
        const auto it = own_edit.find(j);
        if (it != own_edit.end()) {
          own_delta = it->second.second;
          // Its own edit must not shift its own start; only exclude it when the
          // edit origin actually falls at/left of the start (rel == 0 case).
          if (it->second.first <= r.start.column) {
            start_shift -= it->second.second;
          }
        }
      }
      shift_column(r.start.column, start_shift);
      shift_column(r.end.column, start_shift + own_delta);
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
  // Apply each mirror's full-range replacement high-to-low (so lower mirrors'
  // recorded columns stay valid), collecting each edit. A choice can change the
  // placeholder's length; the shift origin is the pre-edit end (`r.end`), so a
  // single batched pass then folds every mirror's delta into all recorded ranges
  // instead of rescanning every tab stop per mirror.
  std::vector<AppliedMirrorEdit> edits;
  edits.reserve(order.size());
  for (std::size_t idx : order) {
    const SelectionRange r = ranges[idx];
    viewport.ReplaceRange(r, text, false);
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(text.size()) -
                                 (static_cast<std::ptrdiff_t>(r.end.column) -
                                  static_cast<std::ptrdiff_t>(r.start.column));
    edits.push_back(AppliedMirrorEdit{idx, r.end.line, r.end.column, delta});
  }
  ApplyBatchedMirrorShifts(session, tab, edits);
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
    // Expansion failed: the buffer is unchanged, so the saved carets are still at
    // valid positions and are restored.
    if (!session.saved_secondary_carets.empty()) {
      viewport.SetSecondaryCarets(session.saved_secondary_carets);
    }
    session.saved_secondary_carets.clear();
    if (viewport.UndoGroupActive()) {
      viewport.EndUndoGroup();
    }
    return false;
  }

  // Expansion succeeded: the snippet consumes the pre-expansion multi-cursor
  // state. Discard the saved secondary carets rather than restoring them on
  // commit — the initial replacement (and every later field edit) shifts the
  // buffer, so the saved pre-snippet offsets would land at stale positions.
  // Clearing here makes CommitSnippetSession's restore a no-op.
  session.saved_secondary_carets.clear();

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
    // Drop the linked-edit session before the caller inserts the line break.
    // Leaving it active would retain ranges computed for the pre-newline document,
    // so later tab navigation / mirror edits would operate on stale line/column
    // positions. (VSCode likewise ends the session on a multi-line insert.)
    CommitSnippetSession(viewport, session);
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

  // Insert into each mirror high-to-low (so lower mirrors' recorded columns stay
  // valid), collecting each edit. One batched pass then advances every recorded
  // range at/after each insertion instead of rescanning all tab stops per mirror.
  std::vector<AppliedMirrorEdit> edits;
  edits.reserve(order.size());
  for (std::size_t idx : order) {
    const TextPosition ins{ranges[idx].start.line, ranges[idx].start.column + rel};
    viewport.ReplaceRange(SelectionRange{ins, ins}, text, false);
    edits.push_back(
        AppliedMirrorEdit{idx, ins.line, ins.column, static_cast<std::ptrdiff_t>(text.size())});
  }
  ApplyBatchedMirrorShifts(session, tab, edits);
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

  // Delete from each mirror high-to-low (so lower mirrors' recorded columns stay
  // valid), collecting each edit. One batched pass then pulls every recorded range
  // at/after each deletion left, instead of rescanning all tab stops per mirror.
  std::vector<AppliedMirrorEdit> edits;
  edits.reserve(order.size());
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
    edits.push_back(
        AppliedMirrorEdit{idx, del.line, del.column, -static_cast<std::ptrdiff_t>(width)});
  }
  ApplyBatchedMirrorShifts(session, tab, edits);
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
  // Delete-forward from each mirror high-to-low, collecting each edit, then fold
  // all deletions into every recorded range in one batched pass (see
  // SnippetTryBackspace) instead of rescanning all tab stops per mirror.
  std::vector<AppliedMirrorEdit> edits;
  edits.reserve(order.size());
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
    edits.push_back(
        AppliedMirrorEdit{idx, del.line, del.column, -static_cast<std::ptrdiff_t>(width)});
  }
  ApplyBatchedMirrorShifts(session, tab, edits);
  FocusTabStop(viewport, session, tab);
  return true;
}

}  // namespace microide::editor
