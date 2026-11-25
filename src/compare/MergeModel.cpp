#include "compare/MergeModel.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <initializer_list>
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

std::vector<SideChange> BuildSideChanges(std::span<const std::string_view> base_lines,
                                         std::span<const std::string_view> variant_lines) {
  util::PerformanceTrace::Scope perf_scope("merge::BuildSideChanges");
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

std::string BuildMergeResultText(const MergeModel& model,
                                 std::string_view separator,
                                 bool bootstrap) {
  std::size_t bytes = 0;
  std::size_t count = 0;
  ForEachMergeResultLine(model, bootstrap, [&](std::string_view line) {
    bytes += line.size();
    ++count;
  });
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

}

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
    model.base_lines = util::SplitLineViews(*model.base_source);
    model.incoming_lines = util::SplitLineViews(*model.incoming_source);
    model.current_lines = util::SplitLineViews(*model.current_source);
  }

  const auto has_phantom = [](const std::string& source) {
    return source.empty() || source.back() == '\n';
  };
  const bool phantoms_agree = has_phantom(*model.base_source) &&
                              has_phantom(*model.incoming_source) &&
                              has_phantom(*model.current_source);
  const auto real_lines = [&](const std::vector<std::string_view>& lines) {
    return std::span<const std::string_view>(lines).first(
        phantoms_agree && !lines.empty() ? lines.size() - 1 : lines.size());
  };
  std::vector<SideChange> incoming_changes =
      BuildSideChanges(real_lines(model.base_lines), real_lines(model.incoming_lines));
  std::vector<SideChange> current_changes =
      BuildSideChanges(real_lines(model.base_lines), real_lines(model.current_lines));

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
    const bool join = group.empty() || c.base_start <= group_max_end;
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
    case MergeChoice::Both:
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

std::string BootstrapMergeResultText(const MergeModel& model, std::string_view separator) {
  return BuildMergeResultText(model, separator, true);
}

std::string MergeResultText(const MergeModel& model, std::string_view separator) {
  return BuildMergeResultText(model, separator, false);
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

MergeChoice ParseMergeChoice(std::string_view label) {
  for (const MergeChoice choice :
       {MergeChoice::Incoming, MergeChoice::Current, MergeChoice::Both,
        MergeChoice::BothCurrentFirst, MergeChoice::BothIncomingFirst}) {
    if (label == MergeChoiceLabel(choice)) {
      return choice;
    }
  }
  return MergeChoice::Base;
}

}
