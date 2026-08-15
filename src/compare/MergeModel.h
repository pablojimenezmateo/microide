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
  // Views into the owning MergeModel's three source buffers, like the model's own
  // line vectors. See MergeModel.
  std::vector<std::string_view> base_lines;
  std::vector<std::string_view> incoming_lines;
  std::vector<std::string_view> current_lines;
  bool conflict = false;
  MergeChoice choice = MergeChoice::Base;
  MergeChoice bootstrap_choice = MergeChoice::Base;
};

struct MergeModel {
  // The three sides' bytes, shared. Every line vector below — the model's and
  // every hunk's — is a view INTO these, so the model owns one buffer per side
  // instead of one `std::string` per line: opening the 12,850-line merge fixture
  // allocated 38,553 strings, 98 % of that scenario's whole open
  // (TD-2026-08-15-246).
  //
  // `shared_ptr<const std::string>` and not a plain member, for the reason
  // `CompareTextBuffer`'s own comment gives: the string OBJECT's address is then
  // fixed for the buffer's life, so a model copy or move never relocates the
  // bytes the views point at. A plain `std::string` member would relocate a short
  // one (small-string buffer) on every move and dangle every view with it.
  CompareTextBuffer base_source = EmptyCompareText();
  CompareTextBuffer incoming_source = EmptyCompareText();
  CompareTextBuffer current_source = EmptyCompareText();
  std::vector<std::string_view> base_lines;
  std::vector<std::string_view> incoming_lines;
  std::vector<std::string_view> current_lines;
  std::vector<MergeHunk> hunks;
  MergeFileConflictMetadata file_conflict;
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

// The lines a hunk contributes under `choice`, as spans INTO the hunk's own
// vectors. The `Both*` choices concatenate two runs, which is why there are two
// spans; every other choice leaves `second` empty.
//
// This is the shared core of `MergeChoiceLines` and `MergeChoiceLineCount`,
// which were two byte-identical decision trees kept in sync by hand — and of the
// result-text builder, which no longer has to materialize a line vector to
// measure or emit one (TD-2026-08-15-239).
struct MergeChoiceLineSpans {
  std::span<const std::string_view> first;
  std::span<const std::string_view> second;

  std::size_t size() const { return first.size() + second.size(); }
};
MergeChoiceLineSpans MergeChoiceLineViews(const MergeHunk& hunk, MergeChoice choice);

std::vector<std::string> MergeChoiceLines(const MergeHunk& hunk, MergeChoice choice);
// Allocation-free count of what MergeChoiceLines(hunk, choice) would return.
// Use at size-only callsites to avoid materializing and copying the line vector.
std::size_t MergeChoiceLineCount(const MergeHunk& hunk, MergeChoice choice);
std::vector<std::string> BootstrapMergeResultLines(const MergeModel& model);
std::string BootstrapMergeResultText(const MergeModel& model,
                                     std::string_view separator = "\n");
std::vector<std::string> MergeResultLines(const MergeModel& model);
std::string MergeResultText(const MergeModel& model, std::string_view separator = "\n");
MergeDisplayModel BuildMergeDisplayModel(const MergeModel& model);
const char* MergeChoiceLabel(MergeChoice choice);

}  // namespace microide::compare
