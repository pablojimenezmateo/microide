#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"

namespace microide::compare {

enum class MergeChoice {
  Base,
  Incoming,
  Current,
  Both,
};

struct MergeHunk {
  int index = 0;
  int base_start = 0;
  int base_end = 0;
  std::vector<std::string> base_lines;
  std::vector<std::string> incoming_lines;
  std::vector<std::string> current_lines;
  bool conflict = false;
  MergeChoice choice = MergeChoice::Base;
};

struct MergeModel {
  std::vector<std::string> base_lines;
  std::vector<std::string> incoming_lines;
  std::vector<std::string> current_lines;
  std::vector<MergeHunk> hunks;
};

struct MergeDisplayRow {
  std::string incoming_text;
  std::string result_text;
  std::string current_text;
  int incoming_line = 0;
  int result_line = 0;
  int current_line = 0;
  int hunk = -1;
  bool conflict = false;
  bool incoming_changed = false;
  bool result_changed = false;
  bool current_changed = false;
};

struct MergeDisplayModel {
  std::vector<MergeDisplayRow> rows;
  std::vector<CompareHunk> hunks;
};

MergeModel BuildMergeModel(const std::string& base,
                           const std::string& incoming,
                           const std::string& current);
MergeChoice BootstrapMergeChoice(const MergeHunk& hunk);
std::vector<std::string> MergeChoiceLines(const MergeHunk& hunk, MergeChoice choice);
std::vector<std::string> BootstrapMergeResultLines(const MergeModel& model);
std::string BootstrapMergeResultText(const MergeModel& model,
                                     std::string_view separator = "\n");
std::vector<std::string> MergeResultLines(const MergeModel& model);
std::string MergeResultText(const MergeModel& model, std::string_view separator = "\n");
MergeDisplayModel BuildMergeDisplayModel(const MergeModel& model);
const char* MergeChoiceLabel(MergeChoice choice);

}  // namespace microide::compare
