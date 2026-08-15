#include "compare/MergeModel.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"

namespace microide::compare {

namespace {

struct SideChange {
  int base_start = 0;
  int base_end = 0;
  // Views into the variant side's source buffer, via the diff ops' own views.
  std::vector<std::string_view> lines;
};

enum class MergeSide {
  Incoming,
  Current,
};

struct TaggedChange {
  MergeSide side = MergeSide::Incoming;
  SideChange change;
};

std::vector<SideChange> BuildSideChanges(const std::vector<std::string_view>& base_lines,
                                         const std::vector<std::string_view>& variant_lines) {
  // Scoped: BuildMergeModel is one synchronous 50 ms call on the shell path when a
  // merge tab opens, and until this scope existed there was no way to say which of
  // its three phases (split, the two diffs, hunk grouping) that time was.
  util::PerformanceTrace::Scope perf_scope("merge::BuildSideChanges");
  // BuildLineDiffOps consumes views, and the model's line vectors ARE views now,
  // so it takes them directly. This used to copy both sides into freshly built
  // view vectors -- two more whole-file vectors per side, on top of the owned
  // strings that existed only to be viewed.
  const std::vector<DiffOp> ops = BuildLineDiffOps(base_lines, variant_lines);
  std::vector<SideChange> changes;
  int base_line = 0;
  for (std::size_t op_index = 0; op_index < ops.size(); ++op_index) {
    if (ops[op_index].kind == DiffOpKind::Equal) {
      ++base_line;
      continue;
    }

    SideChange change;
    change.base_start = base_line;
    while (op_index < ops.size() && ops[op_index].kind != DiffOpKind::Equal) {
      if (ops[op_index].kind == DiffOpKind::Delete) {
        ++base_line;
      } else if (ops[op_index].kind == DiffOpKind::Insert) {
        change.lines.emplace_back(ops[op_index].text);
      }
      ++op_index;
    }
    change.base_end = base_line;
    changes.push_back(std::move(change));
    if (op_index >= ops.size()) {
      break;
    }
    --op_index;
  }
  return changes;
}

std::vector<std::string_view> SliceBaseLines(const std::vector<std::string_view>& base_lines,
                                             int start,
                                             int end) {
  const int safe_start = std::clamp(start, 0, static_cast<int>(base_lines.size()));
  const int safe_end = std::clamp(end, safe_start, static_cast<int>(base_lines.size()));
  return std::vector<std::string_view>(base_lines.begin() + safe_start,
                                       base_lines.begin() + safe_end);
}

// `changes` is taken by mutable reference and sorted IN PLACE. It used to be a
// by-value parameter fed an lvalue, so every call deep-copied a vector of
// SideChange — each of which owns a vector<string> of the changed lines. On a
// merge with many hunks that copy was paid once per hunk per side, on the
// synchronous shell path that opens a merge tab.
std::vector<std::string_view> ApplySideChangesToSlice(
    const std::vector<std::string_view>& base_lines,
    int start,
    int end,
    std::vector<SideChange>& changes) {
  std::sort(changes.begin(), changes.end(), [](const SideChange& lhs, const SideChange& rhs) {
    if (lhs.base_start != rhs.base_start) {
      return lhs.base_start < rhs.base_start;
    }
    return lhs.base_end < rhs.base_end;
  });

  std::vector<std::string_view> lines;
  int cursor = start;
  for (const SideChange& change : changes) {
    const int unchanged_end = std::clamp(change.base_start, cursor, end);
    for (int line = cursor; line < unchanged_end; ++line) {
      lines.push_back(base_lines[static_cast<std::size_t>(line)]);
    }
    lines.insert(lines.end(), change.lines.begin(), change.lines.end());
    cursor = std::clamp(change.base_end, unchanged_end, end);
  }
  for (int line = cursor; line < end; ++line) {
    lines.push_back(base_lines[static_cast<std::size_t>(line)]);
  }
  return lines;
}

// Consumes `changes`: tagging is a relabel, not a copy. Each SideChange owns the
// changed lines, so copying here duplicated one side of the diff wholesale.
std::vector<TaggedChange> TaggedChanges(std::vector<SideChange>&& changes, MergeSide side) {
  std::vector<TaggedChange> tagged;
  tagged.reserve(changes.size());
  for (SideChange& change : changes) {
    tagged.push_back(TaggedChange{
        .side = side,
        .change = std::move(change),
    });
  }
  return tagged;
}

// Walks the resolved result line-by-line, as views into the model, without
// materializing anything. `bootstrap` selects the hunk choice: the recorded one,
// or the one BootstrapMergeChoice would pick before the user has chosen.
//
// The four public result builders are the same walk with different sinks. They
// used to be four separate copies of it, each one materializing a
// `vector<std::string>` of every result line -- the text forms then handed that
// to JoinLines, which copied every line into the joined buffer and freed all of
// them. On a 25,700-line merge that was 25,700 owned strings to produce one
// (TD-2026-08-15-239).
template <typename Fn>
void ForEachMergeResultLine(const MergeModel& model, bool bootstrap, Fn&& fn) {
  const auto emit_span = [&fn](std::span<const std::string_view> lines) {
    for (std::string_view line : lines) {
      fn(line);
    }
  };
  int base_cursor = 0;
  for (const MergeHunk& hunk : model.hunks) {
    for (int line = base_cursor; line < hunk.base_start; ++line) {
      fn(std::string_view(model.base_lines[static_cast<std::size_t>(line)]));
    }
    const MergeChoiceLineSpans spans =
        MergeChoiceLineViews(hunk, bootstrap ? BootstrapMergeChoice(hunk) : hunk.choice);
    emit_span(spans.first);
    emit_span(spans.second);
    base_cursor = hunk.base_end;
  }
  for (int line = base_cursor; line < static_cast<int>(model.base_lines.size()); ++line) {
    fn(std::string_view(model.base_lines[static_cast<std::size_t>(line)]));
  }
}

std::vector<std::string> CollectMergeResultLines(const MergeModel& model, bool bootstrap) {
  std::size_t count = 0;
  ForEachMergeResultLine(model, bootstrap, [&count](std::string_view) { ++count; });
  std::vector<std::string> lines;
  lines.reserve(count);
  ForEachMergeResultLine(model, bootstrap,
                         [&lines](std::string_view line) { lines.emplace_back(line); });
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

std::string BuildMergeResultText(const MergeModel& model,
                                 std::string_view separator,
                                 bool bootstrap) {
  // Two passes over the same walk: bytes, then bytes. Every line is already
  // resident in `model`, so the total is bounded by what is in memory and cannot
  // wrap -- unlike util::JoinLines, which takes an arbitrary span and has to
  // saturate.
  std::size_t bytes = 0;
  std::size_t count = 0;
  ForEachMergeResultLine(model, bootstrap, [&](std::string_view line) {
    bytes += line.size();
    ++count;
  });
  // An empty result is one empty line, which joins to the empty string.
  if (count == 0) {
    return {};
  }
  std::string text;
  text.reserve(bytes + (count - 1) * separator.size());
  bool first = true;
  ForEachMergeResultLine(model, bootstrap, [&](std::string_view line) {
    if (!first) {
      text.append(separator);
    }
    first = false;
    text.append(line);
  });
  return text;
}

}  // namespace

MergeModel BuildMergeModel(const std::string& base,
                           const std::string& incoming,
                           const std::string& current) {
  util::PerformanceTrace::Scope perf_scope("merge::BuildMergeModel");
  MergeModel model;
  model.base_source = MakeCompareText(base);
  model.incoming_source = MakeCompareText(incoming);
  model.current_source = MakeCompareText(current);
  {
    util::PerformanceTrace::Scope split_scope("merge::BuildMergeModel::SplitLines");
    // Views into the model's own shared buffers, not owned strings. Splitting
    // into owned lines was one allocation per line PER SIDE, and every consumer
    // of them -- the diff, the hunks, the renderers -- only ever read them.
    model.base_lines = util::SplitLineViews(*model.base_source);
    model.incoming_lines = util::SplitLineViews(*model.incoming_source);
    model.current_lines = util::SplitLineViews(*model.current_source);
  }

  std::vector<SideChange> incoming_changes =
      BuildSideChanges(model.base_lines, model.incoming_lines);
  std::vector<SideChange> current_changes =
      BuildSideChanges(model.base_lines, model.current_lines);

  std::vector<TaggedChange> all_changes =
      TaggedChanges(std::move(incoming_changes), MergeSide::Incoming);
  std::vector<TaggedChange> current_tagged =
      TaggedChanges(std::move(current_changes), MergeSide::Current);
  all_changes.reserve(all_changes.size() + current_tagged.size());
  all_changes.insert(all_changes.end(), std::make_move_iterator(current_tagged.begin()),
                     std::make_move_iterator(current_tagged.end()));
  std::sort(all_changes.begin(), all_changes.end(), [](const TaggedChange& lhs, const TaggedChange& rhs) {
    if (lhs.change.base_start != rhs.change.base_start) {
      return lhs.change.base_start < rhs.change.base_start;
    }
    if (lhs.change.base_end != rhs.change.base_end) {
      return lhs.change.base_end < rhs.change.base_end;
    }
    return lhs.side < rhs.side;
  });

  // Single linear pass over the sorted change list. After sort-by base_start,
  // any change whose base_start is < group_max_end overlaps the group's union
  // interval and therefore interacts with at least one existing member (the
  // one contributing the running max). The lone asymmetry is two insertions
  // at the same base column: their (start == end) means they fail the strict
  // `<` gate, but ChangesInteract still pairs them. The sort order keeps such
  // insertions adjacent, so a single "previous-was-an-insertion-at-same-start"
  // check covers that case without rescanning the group.
  // Reused across groups: one hunk's partition buffers are the next hunk's, and a
  // merge with hundreds of hunks otherwise allocated two vectors per hunk.
  std::vector<SideChange> hunk_incoming_changes;
  std::vector<SideChange> hunk_current_changes;
  auto flush_group = [&](const std::vector<std::size_t>& group) {
    if (group.empty()) return;
    int base_start = all_changes[group.front()].change.base_start;
    int base_end = all_changes[group.front()].change.base_end;
    hunk_incoming_changes.clear();
    hunk_current_changes.clear();
    for (std::size_t group_index : group) {
      base_start = std::min(base_start, all_changes[group_index].change.base_start);
      base_end = std::max(base_end, all_changes[group_index].change.base_end);
      // Moved, not copied: a flushed group's entries are never read again — the
      // scan pushes `i` only AFTER flushing, and the one backward look
      // (`all_changes[group.back()]`) is always at an element of the group still
      // being accumulated.
      if (all_changes[group_index].side == MergeSide::Incoming) {
        hunk_incoming_changes.push_back(std::move(all_changes[group_index].change));
      } else {
        hunk_current_changes.push_back(std::move(all_changes[group_index].change));
      }
    }

    MergeHunk hunk;
    hunk.index = static_cast<int>(model.hunks.size());
    hunk.base_start = base_start;
    hunk.base_end = base_end;
    hunk.base_lines = SliceBaseLines(model.base_lines, base_start, base_end);
    hunk.incoming_lines =
        ApplySideChangesToSlice(model.base_lines, base_start, base_end, hunk_incoming_changes);
    hunk.current_lines =
        ApplySideChangesToSlice(model.base_lines, base_start, base_end, hunk_current_changes);
    hunk.conflict = hunk.incoming_lines != hunk.current_lines &&
                    hunk.incoming_lines != hunk.base_lines &&
                    hunk.current_lines != hunk.base_lines;
    hunk.choice = BootstrapMergeChoice(hunk);
    hunk.bootstrap_choice = hunk.choice;
    model.hunks.push_back(std::move(hunk));
  };

  std::vector<std::size_t> group;
  int group_max_end = 0;
  for (std::size_t i = 0; i < all_changes.size(); ++i) {
    const SideChange& c = all_changes[i].change;
    const bool c_is_insertion = c.base_start == c.base_end;
    bool join = false;
    if (group.empty()) {
      join = true;
    } else if (c.base_start < group_max_end) {
      join = true;
    } else if (c_is_insertion) {
      const SideChange& prev = all_changes[group.back()].change;
      const bool prev_is_insertion = prev.base_start == prev.base_end;
      if (prev_is_insertion && prev.base_start == c.base_start) {
        join = true;
      }
    }
    if (!join) {
      flush_group(group);
      group.clear();
      group_max_end = 0;
    }
    group.push_back(i);
    group_max_end = std::max(group_max_end, c.base_end);
  }
  flush_group(group);

  std::sort(model.hunks.begin(), model.hunks.end(), [](const MergeHunk& lhs, const MergeHunk& rhs) {
    if (lhs.base_start != rhs.base_start) {
      return lhs.base_start < rhs.base_start;
    }
    return lhs.base_end < rhs.base_end;
  });
  for (std::size_t i = 0; i < model.hunks.size(); ++i) {
    model.hunks[i].index = static_cast<int>(i);
  }

  util::AddPerformanceCounter(util::PerfCounterId::MergeModelBuilds);
  util::AddPerformanceCounter(util::PerfCounterId::MergeModelConflictsFound, model.hunks.size());
  return model;
}

MergeChoice BootstrapMergeChoice(const MergeHunk& hunk) {
  if (hunk.incoming_lines == hunk.current_lines) {
    return MergeChoice::Incoming;
  }
  if (hunk.incoming_lines == hunk.base_lines) {
    return MergeChoice::Current;
  }
  if (hunk.current_lines == hunk.base_lines) {
    return MergeChoice::Incoming;
  }
  return MergeChoice::Base;
}

MergeChoiceLineSpans MergeChoiceLineViews(const MergeHunk& hunk, MergeChoice choice) {
  switch (choice) {
    case MergeChoice::Base:
      return {hunk.base_lines, {}};
    case MergeChoice::Incoming:
      return {hunk.incoming_lines, {}};
    case MergeChoice::Current:
      return {hunk.current_lines, {}};
    case MergeChoice::Both:
    case MergeChoice::BothIncomingFirst:
      if (hunk.incoming_lines == hunk.current_lines) {
        return {hunk.incoming_lines, {}};
      }
      if (hunk.incoming_lines == hunk.base_lines) {
        return {hunk.current_lines, {}};
      }
      if (hunk.current_lines == hunk.base_lines) {
        return {hunk.incoming_lines, {}};
      }
      return {hunk.incoming_lines, hunk.current_lines};
    case MergeChoice::BothCurrentFirst:
      if (hunk.incoming_lines == hunk.current_lines) {
        return {hunk.current_lines, {}};
      }
      if (hunk.incoming_lines == hunk.base_lines) {
        return {hunk.current_lines, {}};
      }
      if (hunk.current_lines == hunk.base_lines) {
        return {hunk.incoming_lines, {}};
      }
      return {hunk.current_lines, hunk.incoming_lines};
    default:
      return MergeChoiceLineViews(hunk, BootstrapMergeChoice(hunk));
  }
}

std::vector<std::string> MergeChoiceLines(const MergeHunk& hunk, MergeChoice choice) {
  const MergeChoiceLineSpans spans = MergeChoiceLineViews(hunk, choice);
  std::vector<std::string> lines;
  lines.reserve(spans.size());
  lines.insert(lines.end(), spans.first.begin(), spans.first.end());
  lines.insert(lines.end(), spans.second.begin(), spans.second.end());
  return lines;
}

std::size_t MergeChoiceLineCount(const MergeHunk& hunk, MergeChoice choice) {
  return MergeChoiceLineViews(hunk, choice).size();
}

std::vector<std::string> BootstrapMergeResultLines(const MergeModel& model) {
  return CollectMergeResultLines(model, /*bootstrap=*/true);
}

std::string BootstrapMergeResultText(const MergeModel& model, std::string_view separator) {
  return BuildMergeResultText(model, separator, /*bootstrap=*/true);
}

std::vector<std::string> MergeResultLines(const MergeModel& model) {
  return CollectMergeResultLines(model, /*bootstrap=*/false);
}

std::string MergeResultText(const MergeModel& model, std::string_view separator) {
  return BuildMergeResultText(model, separator, /*bootstrap=*/false);
}

MergeDisplayModel BuildMergeDisplayModel(const MergeModel& model) {
  MergeDisplayModel display;

  int base_cursor = 0;
  int incoming_line = 1;
  int result_line = 1;
  int current_line = 1;
  // Reused across hunks: one hunk's flattened choice lines are the next hunk's.
  std::vector<std::string_view> hunk_result_scratch;
  // A run of base lines no hunk touches: identical in all three panes, so every
  // *_changed flag is false and all three line numbers advance together. Emitted
  // both for the gap ahead of each hunk and for the tail after the last one.
  const auto emit_unchanged_rows = [&](int from_line, int to_line) {
    for (int line = from_line; line < to_line; ++line) {
      const std::string_view text = model.base_lines[static_cast<std::size_t>(line)];
      display.rows.push_back(MergeDisplayRow{
          .incoming_text = std::string(text),
          .result_text = std::string(text),
          .current_text = std::string(text),
          .incoming_line = incoming_line++,
          .result_line = result_line++,
          .current_line = current_line++,
          .hunk = -1,
          .conflict = false,
          .incoming_changed = false,
          .result_changed = false,
          .current_changed = false,
      });
    }
  };
  for (const MergeHunk& hunk : model.hunks) {
    emit_unchanged_rows(base_cursor, hunk.base_start);

    // Views, like every other line list here: the display model copies them into
    // its own owned rows below, and comparing them against the base needs no copy.
    const MergeChoiceLineSpans result_spans = MergeChoiceLineViews(hunk, hunk.choice);
    hunk_result_scratch.clear();
    hunk_result_scratch.reserve(result_spans.size());
    hunk_result_scratch.insert(hunk_result_scratch.end(), result_spans.first.begin(),
                               result_spans.first.end());
    hunk_result_scratch.insert(hunk_result_scratch.end(), result_spans.second.begin(),
                               result_spans.second.end());
    const std::vector<std::string_view>& hunk_result_lines = hunk_result_scratch;
    const bool incoming_changed = hunk.incoming_lines != hunk.base_lines;
    const bool result_changed = hunk_result_lines != hunk.base_lines;
    const bool current_changed = hunk.current_lines != hunk.base_lines;
    const int start_row = static_cast<int>(display.rows.size());
    const std::size_t row_count =
        std::max({hunk.incoming_lines.size(), hunk_result_lines.size(), hunk.current_lines.size()});
    for (std::size_t row = 0; row < row_count; ++row) {
      const bool has_incoming = row < hunk.incoming_lines.size();
      const bool has_result = row < hunk_result_lines.size();
      const bool has_current = row < hunk.current_lines.size();
      display.rows.push_back(MergeDisplayRow{
          .incoming_text = has_incoming ? std::string(hunk.incoming_lines[row]) : std::string{},
          .result_text = has_result ? std::string(hunk_result_lines[row]) : std::string{},
          .current_text = has_current ? std::string(hunk.current_lines[row]) : std::string{},
          .incoming_line = has_incoming ? incoming_line++ : 0,
          .result_line = has_result ? result_line++ : 0,
          .current_line = has_current ? current_line++ : 0,
          .hunk = hunk.index,
          .conflict = hunk.conflict,
          .incoming_changed = incoming_changed,
          .result_changed = result_changed,
          .current_changed = current_changed,
      });
    }
    // A hunk whose selected incoming/current/result lines are all empty
    // (row_count == 0, e.g. an auto-resolved both-sides deletion) contributes no
    // display rows. Recording it would yield end_row = start_row - 1 (an inverted
    // range) that navigation/minimap code reads as a zero-or-negative span. Skip
    // the display hunk when no rows were emitted.
    if (static_cast<int>(display.rows.size()) > start_row) {
      display.hunks.push_back(CompareHunk{
          .index = hunk.index,
          .start_row = start_row,
          .end_row = static_cast<int>(display.rows.size()) - 1,
      });
    }
    base_cursor = hunk.base_end;
  }

  emit_unchanged_rows(base_cursor, static_cast<int>(model.base_lines.size()));

  // Only materialize the full merge result for the empty-display fallback; a
  // non-empty display (the common case) never needs it, so a large clean merge
  // no longer allocates the whole output on every display rebuild.
  if (display.rows.empty()) {
    const std::vector<std::string> result_lines = MergeResultLines(model);
    for (const std::string& text : result_lines) {
      display.rows.push_back(MergeDisplayRow{
          .incoming_text = text,
          .result_text = text,
          .current_text = text,
          .incoming_line = incoming_line++,
          .result_line = result_line++,
          .current_line = current_line++,
      });
    }
  }

  return display;
}

const char* MergeChoiceLabel(MergeChoice choice) {
  switch (choice) {
    case MergeChoice::Base:
      return "base";
    case MergeChoice::Incoming:
      return "incoming";
    case MergeChoice::Current:
      return "current";
    case MergeChoice::Both:
      return "both";
    case MergeChoice::BothCurrentFirst:
      return "both-current-first";
    case MergeChoice::BothIncomingFirst:
      return "both-incoming-first";
  }
  return "base";
}

}  // namespace microide::compare
