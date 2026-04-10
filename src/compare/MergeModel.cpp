#include "compare/MergeModel.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::compare {

namespace {

enum class DiffOpKind {
  Equal,
  Delete,
  Insert,
};

struct DiffOp {
  DiffOpKind kind = DiffOpKind::Equal;
  std::string text;
};

struct SideChange {
  int base_start = 0;
  int base_end = 0;
  std::vector<std::string> lines;
};

enum class MergeSide {
  Incoming,
  Current,
};

struct TaggedChange {
  MergeSide side = MergeSide::Incoming;
  SideChange change;
};

std::vector<std::string> SplitMergeLines(std::string_view content) {
  std::vector<std::string> lines;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }

    lines.emplace_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }

  if (line_start <= content.size()) {
    lines.emplace_back(content.substr(line_start));
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

std::string JoinMergeLines(const std::vector<std::string>& lines) {
  std::ostringstream buffer;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      buffer << '\n';
    }
    buffer << lines[i];
  }
  return buffer.str();
}

std::vector<DiffOp> BuildDiffOps(const std::vector<std::string>& base_lines,
                                 const std::vector<std::string>& variant_lines) {
  const std::size_t base_count = base_lines.size();
  const std::size_t variant_count = variant_lines.size();
  std::vector<int> dp((base_count + 1) * (variant_count + 1), 0);
  const auto at = [&](std::size_t i, std::size_t j) -> int& {
    return dp[i * (variant_count + 1) + j];
  };

  for (std::size_t i = base_count; i-- > 0;) {
    for (std::size_t j = variant_count; j-- > 0;) {
      if (base_lines[i] == variant_lines[j]) {
        at(i, j) = at(i + 1, j + 1) + 1;
      } else {
        at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
      }
    }
  }

  std::vector<DiffOp> ops;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < base_count && j < variant_count) {
    if (base_lines[i] == variant_lines[j]) {
      ops.push_back(DiffOp{DiffOpKind::Equal, base_lines[i]});
      ++i;
      ++j;
    } else if (at(i + 1, j) >= at(i, j + 1)) {
      ops.push_back(DiffOp{DiffOpKind::Delete, base_lines[i]});
      ++i;
    } else {
      ops.push_back(DiffOp{DiffOpKind::Insert, variant_lines[j]});
      ++j;
    }
  }
  while (i < base_count) {
    ops.push_back(DiffOp{DiffOpKind::Delete, base_lines[i++]});
  }
  while (j < variant_count) {
    ops.push_back(DiffOp{DiffOpKind::Insert, variant_lines[j++]});
  }
  return ops;
}

std::vector<SideChange> BuildSideChanges(const std::vector<std::string>& base_lines,
                                         const std::vector<std::string>& variant_lines) {
  const std::vector<DiffOp> ops = BuildDiffOps(base_lines, variant_lines);
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
        change.lines.push_back(ops[op_index].text);
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

bool ChangesInteract(const SideChange& lhs, const SideChange& rhs) {
  const bool lhs_insertion = lhs.base_start == lhs.base_end;
  const bool rhs_insertion = rhs.base_start == rhs.base_end;
  if (lhs_insertion && rhs_insertion) {
    return lhs.base_start == rhs.base_start;
  }
  return lhs.base_start < rhs.base_end && rhs.base_start < lhs.base_end;
}

std::vector<std::string> SliceBaseLines(const std::vector<std::string>& base_lines,
                                        int start,
                                        int end) {
  const int safe_start = std::clamp(start, 0, static_cast<int>(base_lines.size()));
  const int safe_end = std::clamp(end, safe_start, static_cast<int>(base_lines.size()));
  return std::vector<std::string>(base_lines.begin() + safe_start, base_lines.begin() + safe_end);
}

std::vector<std::string> ApplySideChangesToSlice(const std::vector<std::string>& base_lines,
                                                 int start,
                                                 int end,
                                                 std::vector<SideChange> changes) {
  std::sort(changes.begin(), changes.end(), [](const SideChange& lhs, const SideChange& rhs) {
    if (lhs.base_start != rhs.base_start) {
      return lhs.base_start < rhs.base_start;
    }
    return lhs.base_end < rhs.base_end;
  });

  std::vector<std::string> lines;
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

std::vector<TaggedChange> TaggedChanges(const std::vector<SideChange>& changes, MergeSide side) {
  std::vector<TaggedChange> tagged;
  tagged.reserve(changes.size());
  for (const SideChange& change : changes) {
    tagged.push_back(TaggedChange{
        .side = side,
        .change = change,
    });
  }
  return tagged;
}

}  // namespace

MergeModel BuildMergeModel(const std::string& base,
                           const std::string& incoming,
                           const std::string& current) {
  MergeModel model;
  model.base_lines = SplitMergeLines(base);
  model.incoming_lines = SplitMergeLines(incoming);
  model.current_lines = SplitMergeLines(current);

  const std::vector<SideChange> incoming_changes =
      BuildSideChanges(model.base_lines, model.incoming_lines);
  const std::vector<SideChange> current_changes =
      BuildSideChanges(model.base_lines, model.current_lines);

  std::vector<TaggedChange> all_changes = TaggedChanges(incoming_changes, MergeSide::Incoming);
  const std::vector<TaggedChange> current_tagged =
      TaggedChanges(current_changes, MergeSide::Current);
  all_changes.insert(all_changes.end(), current_tagged.begin(), current_tagged.end());
  std::sort(all_changes.begin(), all_changes.end(), [](const TaggedChange& lhs, const TaggedChange& rhs) {
    if (lhs.change.base_start != rhs.change.base_start) {
      return lhs.change.base_start < rhs.change.base_start;
    }
    if (lhs.change.base_end != rhs.change.base_end) {
      return lhs.change.base_end < rhs.change.base_end;
    }
    return lhs.side < rhs.side;
  });

  std::vector<bool> consumed(all_changes.size(), false);
  for (std::size_t i = 0; i < all_changes.size(); ++i) {
    if (consumed[i]) {
      continue;
    }

    std::vector<SideChange> hunk_incoming_changes;
    std::vector<SideChange> hunk_current_changes;
    std::vector<std::size_t> group = {i};
    consumed[i] = true;

    bool expanded = true;
    while (expanded) {
      expanded = false;
      for (std::size_t candidate = 0; candidate < all_changes.size(); ++candidate) {
        if (consumed[candidate]) {
          continue;
        }
        const auto interacts = std::any_of(
            group.begin(), group.end(), [&](std::size_t group_index) {
              return ChangesInteract(all_changes[group_index].change, all_changes[candidate].change);
            });
        if (!interacts) {
          continue;
        }
        consumed[candidate] = true;
        group.push_back(candidate);
        expanded = true;
      }
    }

    int base_start = all_changes[group.front()].change.base_start;
    int base_end = all_changes[group.front()].change.base_end;
    for (std::size_t group_index : group) {
      base_start = std::min(base_start, all_changes[group_index].change.base_start);
      base_end = std::max(base_end, all_changes[group_index].change.base_end);
      if (all_changes[group_index].side == MergeSide::Incoming) {
        hunk_incoming_changes.push_back(all_changes[group_index].change);
      } else {
        hunk_current_changes.push_back(all_changes[group_index].change);
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
    model.hunks.push_back(std::move(hunk));
  }

  std::sort(model.hunks.begin(), model.hunks.end(), [](const MergeHunk& lhs, const MergeHunk& rhs) {
    if (lhs.base_start != rhs.base_start) {
      return lhs.base_start < rhs.base_start;
    }
    return lhs.base_end < rhs.base_end;
  });
  for (std::size_t i = 0; i < model.hunks.size(); ++i) {
    model.hunks[i].index = static_cast<int>(i);
  }

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

std::vector<std::string> MergeChoiceLines(const MergeHunk& hunk, MergeChoice choice) {
  switch (choice) {
    case MergeChoice::Base:
      return hunk.base_lines;
    case MergeChoice::Incoming:
      return hunk.incoming_lines;
    case MergeChoice::Current:
      return hunk.current_lines;
    case MergeChoice::Both:
      if (hunk.incoming_lines == hunk.current_lines) {
        return hunk.incoming_lines;
      }
      if (hunk.incoming_lines == hunk.base_lines) {
        return hunk.current_lines;
      }
      if (hunk.current_lines == hunk.base_lines) {
        return hunk.incoming_lines;
      }
      {
        std::vector<std::string> lines = hunk.incoming_lines;
        lines.insert(lines.end(), hunk.current_lines.begin(), hunk.current_lines.end());
        return lines;
      }
    default:
      return MergeChoiceLines(hunk, BootstrapMergeChoice(hunk));
  }
}

std::vector<std::string> BootstrapMergeResultLines(const MergeModel& model) {
  std::vector<std::string> lines;
  int base_cursor = 0;
  for (const MergeHunk& hunk : model.hunks) {
    for (int line = base_cursor; line < hunk.base_start; ++line) {
      lines.push_back(model.base_lines[static_cast<std::size_t>(line)]);
    }
    const std::vector<std::string> hunk_lines = MergeChoiceLines(hunk, BootstrapMergeChoice(hunk));
    lines.insert(lines.end(), hunk_lines.begin(), hunk_lines.end());
    base_cursor = hunk.base_end;
  }
  for (int line = base_cursor; line < static_cast<int>(model.base_lines.size()); ++line) {
    lines.push_back(model.base_lines[static_cast<std::size_t>(line)]);
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

std::string BootstrapMergeResultText(const MergeModel& model) {
  return JoinMergeLines(BootstrapMergeResultLines(model));
}

std::vector<std::string> MergeResultLines(const MergeModel& model) {
  std::vector<std::string> lines;
  int base_cursor = 0;
  for (const MergeHunk& hunk : model.hunks) {
    for (int line = base_cursor; line < hunk.base_start; ++line) {
      lines.push_back(model.base_lines[static_cast<std::size_t>(line)]);
    }
    const std::vector<std::string> hunk_lines = MergeChoiceLines(hunk, hunk.choice);
    lines.insert(lines.end(), hunk_lines.begin(), hunk_lines.end());
    base_cursor = hunk.base_end;
  }
  for (int line = base_cursor; line < static_cast<int>(model.base_lines.size()); ++line) {
    lines.push_back(model.base_lines[static_cast<std::size_t>(line)]);
  }
  if (lines.empty()) {
    lines.push_back("");
  }
  return lines;
}

std::string MergeResultText(const MergeModel& model) {
  return JoinMergeLines(MergeResultLines(model));
}

MergeDisplayModel BuildMergeDisplayModel(const MergeModel& model) {
  MergeDisplayModel display;
  const std::vector<std::string> result_lines = MergeResultLines(model);

  int base_cursor = 0;
  int incoming_line = 1;
  int result_line = 1;
  int current_line = 1;
  for (const MergeHunk& hunk : model.hunks) {
    for (int line = base_cursor; line < hunk.base_start; ++line) {
      const std::string& text = model.base_lines[static_cast<std::size_t>(line)];
      display.rows.push_back(MergeDisplayRow{
          .incoming_text = text,
          .result_text = text,
          .current_text = text,
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

    const std::vector<std::string> hunk_result_lines = MergeChoiceLines(hunk, hunk.choice);
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
          .incoming_text = has_incoming ? hunk.incoming_lines[row] : std::string{},
          .result_text = has_result ? hunk_result_lines[row] : std::string{},
          .current_text = has_current ? hunk.current_lines[row] : std::string{},
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
    display.hunks.push_back(CompareHunk{
        .index = hunk.index,
        .start_row = start_row,
        .end_row = static_cast<int>(display.rows.size()) - 1,
    });
    base_cursor = hunk.base_end;
  }

  for (int line = base_cursor; line < static_cast<int>(model.base_lines.size()); ++line) {
    const std::string& text = model.base_lines[static_cast<std::size_t>(line)];
    display.rows.push_back(MergeDisplayRow{
        .incoming_text = text,
        .result_text = text,
        .current_text = text,
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

  if (display.rows.empty() && !result_lines.empty()) {
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
  }
  return "base";
}

}  // namespace microide::compare
