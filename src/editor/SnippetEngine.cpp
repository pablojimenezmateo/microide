#include "editor/SnippetEngine.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <utility>

#include "util/StringUtil.h"

namespace microide::editor {

namespace {

bool IsDigit(char ch) {
  return util::IsAsciiDigit(static_cast<unsigned char>(ch)) != 0;
}

constexpr int kMaxTabStopId = 100000;
constexpr std::size_t kMaxBodyBytes = std::size_t{1} << 16;
constexpr std::size_t kMaxExpandedBytes = std::size_t{1} << 16;
constexpr std::size_t kMaxOccurrences = 4096;
constexpr std::size_t kMaxChoicesPerPlaceholder = 256;

}

void SnippetSessionState::Reset(TextViewport* viewport_restore_secondary_to) {
  if (active && viewport_restore_secondary_to != nullptr && !saved_secondary_carets.empty()) {
    viewport_restore_secondary_to->SetSecondaryCarets(saved_secondary_carets);
  }
  *this = SnippetSessionState{};
}

namespace {

constexpr std::size_t kMaxNestingDepth = 32;

bool IsVariableStart(char ch) {
  return util::IsAsciiAlpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool IsVariableChar(char ch) {
  return util::IsAsciiAlnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

class SnippetBodyParser {
 public:
  SnippetBodyParser(std::string_view body, const SnippetVariableResolver& resolver,
                    SnippetParseResult& out)
      : body_(body), resolver_(resolver), out_(out) {}

  bool Parse() {
    std::size_t pos = 0;
    if (!ParseSequence(pos, false, SnippetParseResult::kNoParent, 0)) {
      return false;
    }
    return RenumberUnknownVariables();
  }

 private:
  bool ParseSequence(std::size_t& pos, bool nested, std::size_t parent, std::size_t depth) {
    while (pos < body_.size()) {
      const char c = body_[pos];
      if (c == '\\' && pos + 1 < body_.size() &&
          (body_[pos + 1] == '$' || body_[pos + 1] == '}' || body_[pos + 1] == '\\')) {
        if (!AppendText(body_[pos + 1])) {
          return false;
        }
        pos += 2;
        continue;
      }
      if (nested && c == '}') {
        ++pos;
        return true;
      }
      if (c == '$') {
        if (!ParseDollar(pos, parent, depth)) {
          return false;
        }
        continue;
      }
      if (!AppendText(c)) {
        return false;
      }
      ++pos;
    }
    return true;
  }

  bool ParseDollar(std::size_t& pos, std::size_t parent, std::size_t depth) {
    std::size_t j = pos + 1;
    if (j < body_.size() && IsDigit(body_[j])) {
      int tab = 0;
      if (!ReadTabId(j, tab)) {
        return false;
      }
      pos = j;
      return AddTabStop(tab, parent);
    }
    if (j < body_.size() && IsVariableStart(body_[j])) {
      const std::string_view name = ReadVariableName(j);
      pos = j;
      return AddVariable(name, std::nullopt, pos, parent, depth);
    }
    if (j >= body_.size() || body_[j] != '{') {
      return LiteralDollar(pos);
    }
    ++j;
    if (j < body_.size() && IsDigit(body_[j])) {
      int tab = 0;
      if (!ReadTabId(j, tab)) {
        return false;
      }
      if (j < body_.size() && body_[j] == '}') {
        pos = j + 1;
        return AddTabStop(tab, parent);
      }
      if (j < body_.size() && body_[j] == '|') {
        pos = j;
        return ParseChoice(pos, tab, parent);
      }
      if (j + 1 < body_.size() && body_[j] == ':' && body_[j + 1] == '|') {
        pos = j + 1;
        return ParseChoice(pos, tab, parent);
      }
      if (j < body_.size() && body_[j] == ':') {
        pos = j + 1;
        return ParsePlaceholder(pos, tab, parent, depth);
      }
      if (j < body_.size() && body_[j] == '/') {
        pos = j;
        SkipToClosingBrace(pos);
        return AddTabStop(tab, parent);
      }
      return LiteralDollar(pos);
    }
    if (j < body_.size() && IsVariableStart(body_[j])) {
      const std::string_view name = ReadVariableName(j);
      if (j < body_.size() && body_[j] == '}') {
        pos = j + 1;
        return AddVariable(name, std::nullopt, pos, parent, depth);
      }
      if (j < body_.size() && body_[j] == ':') {
        pos = j + 1;
        return AddVariable(name, pos, pos, parent, depth);
      }
      if (j < body_.size() && body_[j] == '/') {
        pos = j;
        SkipToClosingBrace(pos);
        return AddVariable(name, std::nullopt, pos, parent, depth);
      }
      return LiteralDollar(pos);
    }
    return LiteralDollar(pos);
  }

  bool LiteralDollar(std::size_t& pos) {
    ++pos;
    return AppendText('$');
  }

  bool ReadTabId(std::size_t& j, int& tab) {
    tab = 0;
    while (j < body_.size() && IsDigit(body_[j])) {
      tab = tab * 10 + (body_[j] - '0');
      if (tab > kMaxTabStopId) {
        return false;
      }
      ++j;
    }
    return true;
  }

  std::string_view ReadVariableName(std::size_t& j) {
    const std::size_t start = j;
    while (j < body_.size() && IsVariableChar(body_[j])) {
      ++j;
    }
    return body_.substr(start, j - start);
  }

  void SkipToClosingBrace(std::size_t& pos) {
    std::size_t depth = 0;
    while (pos < body_.size()) {
      const char c = body_[pos];
      if (c == '\\') {
        pos += 2;
        continue;
      }
      if (c == '{') {
        ++depth;
      } else if (c == '}') {
        if (depth == 0) {
          ++pos;
          return;
        }
        --depth;
      }
      ++pos;
    }
  }

  bool ParsePlaceholder(std::size_t& pos, int tab, std::size_t parent, std::size_t depth) {
    if (depth >= kMaxNestingDepth) {
      return false;
    }
    const std::size_t index = out_.occurrences.size();
    if (!PushOccurrence(tab, parent)) {
      return false;
    }
    if (!ParseSequence(pos, true, index, depth + 1)) {
      return false;
    }
    out_.occurrences[index].end_off = out_.expanded.size();
    return true;
  }

  bool ParseChoice(std::size_t& pos, int tab, std::size_t parent) {
    std::size_t j = pos + 1;
    std::vector<std::string> choices;
    while (j < body_.size()) {
      if (body_[j] == '|') {
        ++j;
        break;
      }
      std::string value;
      while (j < body_.size() && body_[j] != ',' && body_[j] != '|') {
        if (body_[j] == '\\' && j + 1 < body_.size() &&
            (body_[j + 1] == ',' || body_[j + 1] == '|' || body_[j + 1] == '}' ||
             body_[j + 1] == '\\')) {
          value += body_[j + 1];
          j += 2;
        } else {
          value += body_[j];
          ++j;
        }
      }
      if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        return false;
      }
      choices.push_back(std::move(value));
      if (choices.size() > kMaxChoicesPerPlaceholder) {
        return false;
      }
      if (j < body_.size() && body_[j] == ',') {
        ++j;
      }
    }
    if (j < body_.size() && body_[j] == '}') {
      ++j;
    }
    pos = j;
    const std::size_t index = out_.occurrences.size();
    if (!PushOccurrence(tab, parent)) {
      return false;
    }
    if (!choices.empty() && !AppendText(choices.front())) {
      return false;
    }
    out_.occurrences[index].end_off = out_.expanded.size();
    out_.occurrences[index].choices = std::move(choices);
    return true;
  }

  bool AddTabStop(int tab, std::size_t parent) {
    return PushOccurrence(tab, parent);
  }

  bool AddVariable(std::string_view name, std::optional<std::size_t> default_at, std::size_t& pos,
                   std::size_t parent, std::size_t depth) {
    const std::optional<std::string> value = resolver_ ? resolver_(name) : std::nullopt;
    if (value.has_value() && !value->empty()) {
      if (!AppendText(*value)) {
        return false;
      }
      if (default_at.has_value()) {
        pos = *default_at;
        SkipToClosingBrace(pos);
      }
      return true;
    }
    if (value.has_value()) {
      if (!default_at.has_value()) {
        return true;
      }
      if (depth >= kMaxNestingDepth) {
        return false;
      }
      pos = *default_at;
      return ParseSequence(pos, true, parent, depth + 1);
    }
    const int tab = UnknownVariableTab(name);
    if (depth >= kMaxNestingDepth) {
      return false;
    }
    const std::size_t index = out_.occurrences.size();
    if (!PushOccurrence(tab, parent)) {
      return false;
    }
    if (default_at.has_value()) {
      pos = *default_at;
      if (!ParseSequence(pos, true, index, depth + 1)) {
        return false;
      }
    } else if (!AppendText(name)) {
      return false;
    }
    out_.occurrences[index].end_off = out_.expanded.size();
    return true;
  }

  int UnknownVariableTab(std::string_view name) {
    for (std::size_t i = 0; i < unknown_variables_.size(); ++i) {
      if (unknown_variables_[i] == name) {
        return -static_cast<int>(i) - 1;
      }
    }
    unknown_variables_.emplace_back(name);
    return -static_cast<int>(unknown_variables_.size());
  }

  bool RenumberUnknownVariables() {
    if (unknown_variables_.empty()) {
      return true;
    }
    int max_tab = 0;
    for (const auto& occ : out_.occurrences) {
      max_tab = std::max(max_tab, occ.tab_stop);
    }
    if (max_tab > kMaxTabStopId - static_cast<int>(unknown_variables_.size())) {
      return false;
    }
    for (auto& occ : out_.occurrences) {
      if (occ.tab_stop < 0) {
        occ.tab_stop = max_tab - occ.tab_stop;
      }
    }
    return true;
  }

  bool PushOccurrence(int tab, std::size_t parent) {
    SnippetParseResult::Occurrence occ;
    occ.tab_stop = tab;
    occ.start_off = out_.expanded.size();
    occ.end_off = occ.start_off;
    occ.parent = parent;
    out_.occurrences.push_back(std::move(occ));
    return out_.occurrences.size() <= kMaxOccurrences;
  }

  bool AppendText(char c) {
    out_.expanded += c;
    return out_.expanded.size() <= kMaxExpandedBytes;
  }

  bool AppendText(std::string_view text) {
    out_.expanded += text;
    return out_.expanded.size() <= kMaxExpandedBytes;
  }

  std::string_view body_;
  const SnippetVariableResolver& resolver_;
  SnippetParseResult& out_;
  std::vector<std::string> unknown_variables_;
};

}

SnippetParseResult ParseSnippetBody(std::string_view body,
                                    const SnippetVariableResolver& resolve_variable) {
  if (body.size() > kMaxBodyBytes) {
    return SnippetParseResult{};
  }
  SnippetParseResult result;
  SnippetBodyParser parser(body, resolve_variable, result);
  if (!parser.Parse()) {
    return SnippetParseResult{};
  }
  return result;
}

namespace {

std::string FormatLocalTime(const char* format) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  char buffer[64];
  const std::size_t written = std::strftime(buffer, sizeof(buffer), format, &local);
  return std::string(buffer, written);
}

std::uint64_t SnippetRandomBits() {
  static thread_local std::mt19937_64 engine{std::random_device{}()};
  return engine();
}

std::string RandomHexDigits(std::size_t count) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(count);
  std::uint64_t bits = SnippetRandomBits();
  for (std::size_t i = 0; i < count; ++i) {
    if (i % 16 == 0 && i != 0) {
      bits = SnippetRandomBits();
    }
    out.push_back(kDigits[bits & 0xF]);
    bits >>= 4;
  }
  return out;
}

}

std::optional<std::string> ResolveEditorSnippetVariable(const TextViewport& viewport,
                                                        const SelectionRange& trigger,
                                                        std::string_view name) {
  const std::filesystem::path& path = viewport.path();
  const std::size_t line = std::min(trigger.start.line, viewport.lines().size() - 1);
  if (name == "TM_SELECTED_TEXT") {
    return std::string{};
  }
  if (name == "TM_CURRENT_LINE") {
    return viewport.lines().empty() ? std::string{} : viewport.lines()[line];
  }
  if (name == "TM_CURRENT_WORD") {
    return viewport.TextInRange(trigger);
  }
  if (name == "TM_LINE_INDEX") {
    return std::to_string(trigger.start.line);
  }
  if (name == "TM_LINE_NUMBER") {
    return std::to_string(trigger.start.line + 1);
  }
  if (name == "TM_FILENAME" || name == "RELATIVE_FILEPATH") {
    return path.filename().string();
  }
  if (name == "TM_FILENAME_BASE") {
    return path.stem().string();
  }
  if (name == "TM_DIRECTORY") {
    return path.parent_path().string();
  }
  if (name == "TM_FILEPATH") {
    return path.string();
  }
  if (name == "CLIPBOARD" || name == "WORKSPACE_NAME" || name == "WORKSPACE_FOLDER") {
    return std::string{};
  }
  if (name == "CURSOR_INDEX") {
    return std::string("0");
  }
  if (name == "CURSOR_NUMBER") {
    return std::string("1");
  }
  if (name == "CURRENT_YEAR") return FormatLocalTime("%Y");
  if (name == "CURRENT_YEAR_SHORT") return FormatLocalTime("%y");
  if (name == "CURRENT_MONTH") return FormatLocalTime("%m");
  if (name == "CURRENT_MONTH_NAME") return FormatLocalTime("%B");
  if (name == "CURRENT_MONTH_NAME_SHORT") return FormatLocalTime("%b");
  if (name == "CURRENT_DATE") return FormatLocalTime("%d");
  if (name == "CURRENT_DAY_NAME") return FormatLocalTime("%A");
  if (name == "CURRENT_DAY_NAME_SHORT") return FormatLocalTime("%a");
  if (name == "CURRENT_HOUR") return FormatLocalTime("%H");
  if (name == "CURRENT_MINUTE") return FormatLocalTime("%M");
  if (name == "CURRENT_SECOND") return FormatLocalTime("%S");
  if (name == "CURRENT_TIMEZONE_OFFSET") return FormatLocalTime("%z");
  if (name == "CURRENT_SECONDS_UNIX") {
    return std::to_string(static_cast<long long>(std::time(nullptr)));
  }
  if (name == "RANDOM") {
    std::string digits = std::to_string(SnippetRandomBits() % 1000000);
    return std::string(6 - digits.size(), '0') + digits;
  }
  if (name == "RANDOM_HEX") {
    return RandomHexDigits(6);
  }
  if (name == "UUID") {
    std::string hex = RandomHexDigits(32);
    hex[12] = '4';
    hex[16] = "89ab"[hex[16] & 0x3];
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
  }
  const LanguageContractView& contract = viewport.language_contract_view();
  if (name == "LINE_COMMENT") {
    return contract.line_comment;
  }
  if (name == "BLOCK_COMMENT_START") {
    return contract.block_comment_open;
  }
  if (name == "BLOCK_COMMENT_END") {
    return contract.block_comment_close;
  }
  return std::nullopt;
}

TextPosition PositionAfterOffsetInExpanded(TextPosition trigger_start,
                                           std::string_view expanded_flat,
                                           std::size_t offset) {
  std::size_t line = trigger_start.line;
  std::size_t col = trigger_start.column;
  for (std::size_t k = 0; k < offset && k < expanded_flat.size(); ++k) {
    const char c = expanded_flat[k];
    if (c == '\r') {
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

struct AppliedMirrorEdit {
  std::size_t range_index = 0;
  std::size_t line = 0;
  std::size_t origin_col = 0;
  std::ptrdiff_t delta = 0;
};

static const SnippetNestedLink* ParentLinkOf(const SnippetSessionState& session, int tab,
                                            std::size_t index) {
  for (const SnippetNestedLink& link : session.nested_links) {
    if (link.tab == tab && link.index == index) {
      return &link;
    }
  }
  return nullptr;
}

static bool IsNestedInsideTab(const SnippetSessionState& session, int tab, std::size_t index,
                              int ancestor_tab) {
  for (std::size_t hops = 0; hops <= session.nested_links.size(); ++hops) {
    const SnippetNestedLink* link = ParentLinkOf(session, tab, index);
    if (link == nullptr) {
      return false;
    }
    if (link->parent_tab == ancestor_tab) {
      return true;
    }
    tab = link->parent_tab;
    index = link->parent_index;
  }
  return false;
}

static void DropPlaceholdersNestedInTab(SnippetSessionState& session, int edited_tab) {
  std::vector<int> emptied_tabs;
  for (auto& [tab, ranges] : session.ranges_by_tab) {
    std::vector<std::size_t> new_index(ranges.size(), 0);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
      if (IsNestedInsideTab(session, tab, i, edited_tab)) {
        new_index[i] = static_cast<std::size_t>(-1);
        continue;
      }
      new_index[i] = kept;
      ranges[kept++] = ranges[i];
    }
    if (kept == ranges.size()) {
      continue;
    }
    ranges.resize(kept);
    std::vector<SnippetNestedLink> links;
    links.reserve(session.nested_links.size());
    for (SnippetNestedLink link : session.nested_links) {
      if (link.tab == tab) {
        if (new_index[link.index] == static_cast<std::size_t>(-1)) {
          continue;
        }
        link.index = new_index[link.index];
      }
      if (link.parent_tab == tab) {
        if (new_index[link.parent_index] == static_cast<std::size_t>(-1)) {
          continue;
        }
        link.parent_index = new_index[link.parent_index];
      }
      links.push_back(link);
    }
    session.nested_links = std::move(links);
    if (kept == 0) {
      emptied_tabs.push_back(tab);
    }
  }
  for (const int tab : emptied_tabs) {
    session.ranges_by_tab.erase(tab);
    session.choices_by_tab.erase(tab);
    session.choice_index_by_tab.erase(tab);
    for (std::size_t i = 0; i < session.navigate_order.size();) {
      if (session.navigate_order[i] != tab) {
        ++i;
        continue;
      }
      session.navigate_order.erase(session.navigate_order.begin() + static_cast<std::ptrdiff_t>(i));
      if (i < session.navigate_index) {
        --session.navigate_index;
      }
    }
  }
}

static void ApplyBatchedMirrorShifts(SnippetSessionState& session, int edited_tab,
                                     const std::vector<AppliedMirrorEdit>& edits) {
  if (edits.empty()) {
    return;
  }
  struct AncestorAdjust {
    int tab = 0;
    std::size_t index = 0;
    std::ptrdiff_t end_extra = 0;
    std::ptrdiff_t start_exclude = 0;
  };
  std::vector<AncestorAdjust> ancestor_adjust;
  if (!session.nested_links.empty()) {
    DropPlaceholdersNestedInTab(session, edited_tab);
    for (const AppliedMirrorEdit& e : edits) {
      int tab = edited_tab;
      std::size_t index = e.range_index;
      for (std::size_t hops = 0; hops <= session.nested_links.size(); ++hops) {
        const SnippetNestedLink* link = ParentLinkOf(session, tab, index);
        if (link == nullptr) {
          break;
        }
        tab = link->parent_tab;
        index = link->parent_index;
        const auto it = session.ranges_by_tab.find(tab);
        if (it == session.ranges_by_tab.end() || index >= it->second.size()) {
          break;
        }
        const SelectionRange& ancestor = it->second[index];
        AncestorAdjust* adjust = nullptr;
        for (AncestorAdjust& a : ancestor_adjust) {
          if (a.tab == tab && a.index == index) {
            adjust = &a;
          }
        }
        if (adjust == nullptr) {
          ancestor_adjust.push_back(AncestorAdjust{.tab = tab, .index = index});
          adjust = &ancestor_adjust.back();
        }
        if (e.line == ancestor.end.line) {
          adjust->end_extra += e.delta;
        }
        if (e.line == ancestor.start.line && e.origin_col == ancestor.start.column) {
          adjust->start_exclude += e.delta;
        }
      }
    }
  }
  struct ShiftEvent {
    std::size_t line = 0;
    std::size_t origin_col = 0;
    std::ptrdiff_t shift = 0;
  };
  std::vector<ShiftEvent> events;
  events.reserve(edits.size());
  for (const AppliedMirrorEdit& e : edits) {
    events.push_back(ShiftEvent{.line = e.line, .origin_col = e.origin_col, .shift = e.delta});
  }
  std::sort(events.begin(), events.end(), [](const ShiftEvent& a, const ShiftEvent& b) {
    return a.line != b.line ? a.line < b.line : a.origin_col < b.origin_col;
  });
  for (std::size_t i = 0; i < events.size();) {
    std::ptrdiff_t acc = 0;
    std::size_t j = i;
    while (j < events.size() && events[j].line == events[i].line) {
      acc += events[j].shift;
      events[j].shift = acc;
      ++j;
    }
    i = j;
  }
  const auto shift_at = [&](std::size_t line, std::size_t col) -> std::ptrdiff_t {
    const auto it = std::upper_bound(
        events.begin(), events.end(), std::pair<std::size_t, std::size_t>{line, col},
        [](const std::pair<std::size_t, std::size_t>& value, const ShiftEvent& e) {
          return value.first != e.line ? value.first < e.line : value.second < e.origin_col;
        });
    if (it == events.begin()) {
      return 0;
    }
    const ShiftEvent& last_at_or_left = *(it - 1);
    return last_at_or_left.line == line ? last_at_or_left.shift : 0;
  };
  struct OwnEdit {
    std::size_t origin_col = 0;
    std::ptrdiff_t delta = 0;
    bool present = false;
  };
  std::size_t max_range_index = 0;
  for (const AppliedMirrorEdit& e : edits) {
    max_range_index = std::max(max_range_index, e.range_index);
  }
  std::vector<OwnEdit> own_edit(max_range_index + 1);
  for (const AppliedMirrorEdit& e : edits) {
    own_edit[e.range_index] = OwnEdit{.origin_col = e.origin_col, .delta = e.delta,
                                      .present = true};
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
      if (tab == edited_tab && j < own_edit.size() && own_edit[j].present) {
        const OwnEdit& own = own_edit[j];
        own_delta = own.delta;
        if (own.origin_col <= r.start.column) {
          start_shift -= own.delta;
        }
      }
      std::ptrdiff_t end_extra = 0;
      for (const AncestorAdjust& a : ancestor_adjust) {
        if (a.tab == tab && a.index == j) {
          start_shift -= a.start_exclude;
          end_extra = a.end_extra;
        }
      }
      shift_column(r.start.column, start_shift);
      shift_column(r.end.column, start_shift + own_delta + end_extra);
    }
  }
}

static void StampSessionRevision(const TextViewport& viewport, SnippetSessionState& session) {
  session.tracked_content_revision = viewport.content_revision();
}

static std::vector<std::size_t> MirrorIndicesBottomUp(const std::vector<SelectionRange>& ranges) {
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
  return order;
}

static void ApplyChoiceForTab(TextViewport& viewport,
                              SnippetSessionState& session,
                              int tab,
                              const std::string& text) {
  auto& ranges = session.ranges_by_tab[tab];
  const std::vector<std::size_t> order = MirrorIndicesBottomUp(ranges);
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
  StampSessionRevision(viewport, session);
}

bool ExpandSnippetAtSelection(TextViewport& viewport,
                              SnippetSessionState& session,
                              const SelectionRange& trigger_range,
                              std::string_view snippet_body) {
  session.Reset(&viewport);

  const SelectionRange trigger = TextViewport::NormalizeRange(trigger_range);
  const SnippetParseResult parsed =
      ParseSnippetBody(snippet_body, [&](std::string_view name) {
        return ResolveEditorSnippetVariable(viewport, trigger, name);
      });

  session.saved_secondary_carets = viewport.secondary_carets();
  viewport.ClearSecondaryCarets();

  if (!viewport.ReplaceRange(trigger, parsed.expanded, true) && viewport.read_only()) {
    if (!session.saved_secondary_carets.empty()) {
      viewport.SetSecondaryCarets(session.saved_secondary_carets);
    }
    session.saved_secondary_carets.clear();
    if (viewport.UndoGroupActive()) {
      viewport.EndUndoGroup();
    }
    return false;
  }

  session.saved_secondary_carets.clear();

  if (parsed.occurrences.empty()) {
    session.navigate_order.clear();
    session.navigate_index = 0;
    session.active = false;
    CommitSnippetSession(viewport, session);
    return true;
  }

  std::vector<std::size_t> slot_index(parsed.occurrences.size(), 0);
  for (std::size_t i = 0; i < parsed.occurrences.size(); ++i) {
    const auto& occ = parsed.occurrences[i];
    const TextPosition a = PositionAfterOffsetInExpanded(trigger.start, parsed.expanded, occ.start_off);
    const TextPosition b = PositionAfterOffsetInExpanded(trigger.start, parsed.expanded, occ.end_off);
    auto& ranges = session.ranges_by_tab[occ.tab_stop];
    slot_index[i] = ranges.size();
    ranges.push_back(SelectionRange{a, b});
    if (!occ.choices.empty()) {
      session.choices_by_tab[occ.tab_stop] = occ.choices;
      session.choice_index_by_tab[occ.tab_stop] = 0;
    }
    if (occ.parent != SnippetParseResult::kNoParent) {
      const auto& parent = parsed.occurrences[occ.parent];
      session.nested_links.push_back(SnippetNestedLink{
          .tab = occ.tab_stop,
          .index = slot_index[i],
          .parent_tab = parent.tab_stop,
          .parent_index = slot_index[occ.parent],
      });
    }
  }

  session.navigate_order = BuildNavigateOrder(parsed.occurrences);
  session.navigate_index = 0;
  session.active = true;
  StampSessionRevision(viewport, session);

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

void SnippetOnBufferChanged(TextViewport& viewport, SnippetSessionState& session) {
  if (!session.active || session.tracked_content_revision == viewport.content_revision()) {
    return;
  }
  CommitSnippetSession(viewport, session);
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

struct MirrorEditTarget {
  std::size_t mirror_index = 0;
  std::size_t rel_start = 0;
  std::size_t rel_end = 0;
};

static std::optional<MirrorEditTarget> FocusedMirrorTarget(const TextViewport& viewport,
                                                           const std::vector<SelectionRange>& ranges) {
  if (const std::optional<SelectionRange> selection = viewport.selection_range()) {
    if (selection->start.line != selection->end.line) {
      return std::nullopt;
    }
    for (std::size_t i = 0; i < ranges.size(); ++i) {
      const SelectionRange& r = ranges[i];
      if (r.start.line != r.end.line || selection->start.line != r.start.line) {
        continue;
      }
      if (selection->start.column >= r.start.column && selection->end.column <= r.end.column) {
        return MirrorEditTarget{i, selection->start.column - r.start.column,
                                selection->end.column - r.start.column};
      }
    }
    return std::nullopt;
  }
  const TextPosition caret{viewport.cursor_line(), viewport.cursor_column()};
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const SelectionRange& r = ranges[i];
    if (r.start.line != r.end.line || caret.line != r.start.line) {
      continue;
    }
    if (caret.column >= r.start.column && caret.column <= r.end.column) {
      const std::size_t rel = caret.column - r.start.column;
      return MirrorEditTarget{i, rel, rel};
    }
  }
  return std::nullopt;
}

static void ReplaceInMirrors(TextViewport& viewport, SnippetSessionState& session, int tab,
                             const MirrorEditTarget& target, std::string_view text) {
  auto& ranges = session.ranges_by_tab[tab];
  const std::vector<std::size_t> order = MirrorIndicesBottomUp(ranges);
  std::vector<AppliedMirrorEdit> edits;
  edits.reserve(order.size());
  for (std::size_t idx : order) {
    const SelectionRange r = ranges[idx];
    const std::size_t start_col = std::min(r.start.column + target.rel_start, r.end.column);
    const std::size_t end_col = std::min(r.start.column + target.rel_end, r.end.column);
    if (start_col == end_col && text.empty()) {
      continue;
    }
    viewport.ReplaceRange(
        SelectionRange{TextPosition{r.start.line, start_col}, TextPosition{r.start.line, end_col}},
        text, false);
    const std::ptrdiff_t delta = static_cast<std::ptrdiff_t>(text.size()) -
                                 static_cast<std::ptrdiff_t>(end_col - start_col);
    edits.push_back(AppliedMirrorEdit{idx, r.start.line, end_col, delta});
  }
  ApplyBatchedMirrorShifts(session, tab, edits);
  const SelectionRange& edited = ranges[target.mirror_index];
  viewport.ClearSelection();
  viewport.MoveCursorTo(edited.start.line,
                        std::min(edited.start.column + target.rel_start + text.size(),
                                 edited.end.column),
                        false);
  StampSessionRevision(viewport, session);
}

static std::vector<SelectionRange>* FocusedTabRanges(SnippetSessionState& session, int* tab_out) {
  if (!session.active || session.navigate_index >= session.navigate_order.size()) {
    return nullptr;
  }
  const int tab = session.navigate_order[session.navigate_index];
  const auto it = session.ranges_by_tab.find(tab);
  if (it == session.ranges_by_tab.end() || it->second.empty()) {
    return nullptr;
  }
  *tab_out = tab;
  return &it->second;
}

bool SnippetTryInsertText(TextViewport& viewport, SnippetSessionState& session, std::string_view text) {
  if (!session.active || text.empty()) {
    return false;
  }
  if (text.find('\n') != std::string_view::npos ||
      text.find('\r') != std::string_view::npos) {
    CommitSnippetSession(viewport, session);
    return false;
  }
  int tab = 0;
  const std::vector<SelectionRange>* ranges = FocusedTabRanges(session, &tab);
  if (ranges == nullptr) {
    return false;
  }
  const std::optional<MirrorEditTarget> target = FocusedMirrorTarget(viewport, *ranges);
  if (!target.has_value()) {
    return false;
  }
  ReplaceInMirrors(viewport, session, tab, *target, text);
  return true;
}

bool SnippetTryBackspace(TextViewport& viewport, SnippetSessionState& session) {
  int tab = 0;
  const std::vector<SelectionRange>* ranges = FocusedTabRanges(session, &tab);
  if (ranges == nullptr) {
    return false;
  }
  std::optional<MirrorEditTarget> target = FocusedMirrorTarget(viewport, *ranges);
  if (!target.has_value()) {
    return false;
  }
  if (target->rel_start == target->rel_end) {
    if (target->rel_start == 0) {
      return false;
    }
    const SelectionRange& r = (*ranges)[target->mirror_index];
    const std::string& caret_line = viewport.lines()[r.start.line];
    target->rel_start =
        util::PreviousUtf8Boundary(caret_line, r.start.column + target->rel_end) - r.start.column;
  }
  ReplaceInMirrors(viewport, session, tab, *target, std::string_view{});
  return true;
}

bool SnippetTryDeleteForward(TextViewport& viewport, SnippetSessionState& session) {
  int tab = 0;
  const std::vector<SelectionRange>* ranges = FocusedTabRanges(session, &tab);
  if (ranges == nullptr) {
    return false;
  }
  std::optional<MirrorEditTarget> target = FocusedMirrorTarget(viewport, *ranges);
  if (!target.has_value()) {
    return false;
  }
  if (target->rel_start == target->rel_end) {
    const SelectionRange& r = (*ranges)[target->mirror_index];
    if (r.start.column + target->rel_start >= r.end.column) {
      return false;
    }
    const std::string& caret_line = viewport.lines()[r.start.line];
    target->rel_end = std::min(util::NextUtf8Boundary(caret_line, r.start.column + target->rel_start),
                               r.end.column) -
                      r.start.column;
  }
  ReplaceInMirrors(viewport, session, tab, *target, std::string_view{});
  return true;
}

}
