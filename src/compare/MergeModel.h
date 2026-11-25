#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeConflictKind.h"

namespace microide::compare {

enum class MergeChoice {
  Base,
  Incoming,
  Current,
  Both,
  BothCurrentFirst,
  BothIncomingFirst,
};

struct MergeHunk {
  int index = 0;
  int base_start = 0;
  int base_end = 0;
  std::vector<std::string_view> base_lines;
  std::vector<std::string_view> incoming_lines;
  std::vector<std::string_view> current_lines;
  bool conflict = false;
  MergeChoice choice = MergeChoice::Base;
  MergeChoice bootstrap_choice = MergeChoice::Base;
};

struct MergeModel {
  CompareTextBuffer base_source = EmptyCompareText();
  CompareTextBuffer incoming_source = EmptyCompareText();
  CompareTextBuffer current_source = EmptyCompareText();
  std::vector<std::string_view> base_lines;
  std::vector<std::string_view> incoming_lines;
  std::vector<std::string_view> current_lines;
  std::vector<MergeHunk> hunks;
  MergeFileConflictMetadata file_conflict;
};

MergeModel BuildMergeModel(const std::string& base,
                           const std::string& incoming,
                           const std::string& current);
MergeChoice BootstrapMergeChoice(const MergeHunk& hunk);

struct MergeChoiceLineSpans {
  std::span<const std::string_view> first;
  std::span<const std::string_view> second;

  std::size_t size() const { return first.size() + second.size(); }
};
MergeChoiceLineSpans MergeChoiceLineViews(const MergeHunk& hunk, MergeChoice choice);

std::vector<std::string> MergeChoiceLines(const MergeHunk& hunk, MergeChoice choice);
std::size_t MergeChoiceLineCount(const MergeHunk& hunk, MergeChoice choice);
std::string BootstrapMergeResultText(const MergeModel& model,
                                     std::string_view separator = "\n");
std::string MergeResultText(const MergeModel& model, std::string_view separator = "\n");
const char* MergeChoiceLabel(MergeChoice choice);
MergeChoice ParseMergeChoice(std::string_view label);

}
