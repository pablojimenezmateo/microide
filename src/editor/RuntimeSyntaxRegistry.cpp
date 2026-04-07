#define PCRE2_CODE_UNIT_WIDTH 8

#include "editor/RuntimeSyntaxRegistry.h"

#include <pcre2.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/RuntimeSyntaxData.h"

namespace microide::editor::runtime_syntax {

namespace {

constexpr std::size_t kSignatureDetectLineLimit = 64;
constexpr uint32_t kRegexCompileOptions = PCRE2_UTF | PCRE2_UCP;

struct MatchRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct RegionStartMatch {
  std::uint32_t region_id = 0;
  MatchRange match;
};

class CompiledRegex {
 public:
  CompiledRegex() = default;

  explicit CompiledRegex(std::string_view pattern) : pattern_(NormalizePattern(pattern)) {
    if (pattern_.empty()) {
      return;
    }

    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    code_ = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern_.c_str()), PCRE2_ZERO_TERMINATED,
                          kRegexCompileOptions, &error_code, &error_offset, nullptr);
  }

  ~CompiledRegex() {
    if (code_ != nullptr) {
      pcre2_code_free(code_);
      code_ = nullptr;
    }
  }

  CompiledRegex(const CompiledRegex&) = delete;
  CompiledRegex& operator=(const CompiledRegex&) = delete;

  CompiledRegex(CompiledRegex&& other) noexcept
      : code_(std::exchange(other.code_, nullptr)), pattern_(std::move(other.pattern_)) {}

  CompiledRegex& operator=(CompiledRegex&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (code_ != nullptr) {
      pcre2_code_free(code_);
    }
    code_ = std::exchange(other.code_, nullptr);
    pattern_ = std::move(other.pattern_);
    return *this;
  }

  bool valid() const { return code_ != nullptr; }

  bool Matches(std::string_view text) const { return FindFirst(text).has_value(); }

  std::optional<MatchRange> FindFirst(std::string_view text,
                                      const CompiledRegex* skip = nullptr) const {
    if (!valid()) {
      return std::nullopt;
    }
    if (skip != nullptr && skip->valid()) {
      std::string masked(text);
      for (const MatchRange match : skip->FindAll(text)) {
        std::fill(masked.begin() + static_cast<std::ptrdiff_t>(match.start),
                  masked.begin() + static_cast<std::ptrdiff_t>(match.end), '\0');
      }
      return FindFirst(masked, nullptr);
    }
    return FindFirstFrom(text, 0);
  }

  std::vector<MatchRange> FindAll(std::string_view text) const {
    std::vector<MatchRange> matches;
    if (!valid() || text.empty()) {
      return matches;
    }

    pcre2_match_data* match_data = pcre2_match_data_create_from_pattern(code_, nullptr);
    if (match_data == nullptr) {
      return matches;
    }

    for (std::size_t offset = 0; offset <= text.size();) {
      const int rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(),
                                 offset, 0, match_data, nullptr);
      if (rc < 0) {
        break;
      }

      PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data);
      const std::size_t start = static_cast<std::size_t>(ovector[0]);
      const std::size_t end = static_cast<std::size_t>(ovector[1]);
      if (start >= end || end > text.size()) {
        offset = AdvanceToNextCodePoint(text, end);
        continue;
      }

      matches.push_back(MatchRange{start, end});
      offset = end;
    }

    pcre2_match_data_free(match_data);
    return matches;
  }

 private:
  static std::string NormalizePattern(std::string_view pattern) {
    std::string normalized;
    normalized.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      if (pattern[i] == '\\' && i + 1 < pattern.size() &&
          (pattern[i + 1] == '<' || pattern[i + 1] == '>')) {
        normalized += "\\b";
        ++i;
        continue;
      }
      normalized.push_back(pattern[i]);
    }
    return normalized;
  }

  std::optional<MatchRange> FindFirstFrom(std::string_view text, std::size_t offset) const {
    pcre2_match_data* match_data = pcre2_match_data_create_from_pattern(code_, nullptr);
    if (match_data == nullptr) {
      return std::nullopt;
    }

    const int rc = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(),
                               offset, 0, match_data, nullptr);
    if (rc < 0) {
      pcre2_match_data_free(match_data);
      return std::nullopt;
    }

    PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data);
    const std::size_t start = static_cast<std::size_t>(ovector[0]);
    const std::size_t end = static_cast<std::size_t>(ovector[1]);
    pcre2_match_data_free(match_data);
    if (start >= end || end > text.size()) {
      return std::nullopt;
    }
    return MatchRange{start, end};
  }

  static std::size_t AdvanceToNextCodePoint(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
      return text.size() + 1;
    }

    std::size_t next = offset + 1;
    while (next < text.size() &&
           (static_cast<unsigned char>(text[next]) & 0xc0u) == 0x80u) {
      ++next;
    }
    return next;
  }

  pcre2_code* code_ = nullptr;
  std::string pattern_;
};

struct Rule {
  GeneratedRuleKind kind = GeneratedRuleKind::Pattern;
  SyntaxTokenKind group = SyntaxTokenKind::Plain;
  SyntaxTokenKind limit_group = SyntaxTokenKind::Plain;
  CompiledRegex pattern;
  CompiledRegex start;
  CompiledRegex end;
  CompiledRegex skip;
  std::size_t first_child = 0;
  std::size_t child_count = 0;
  std::uint32_t parent_region_id = 0;
};

struct Definition {
  std::string filetype;
  CompiledRegex filename_regex;
  CompiledRegex header_regex;
  CompiledRegex signature_regex;
  std::size_t first_rule = 0;
  std::size_t rule_count = 0;
};

struct Registry {
  std::vector<Definition> definitions;
  std::vector<Rule> rules;
  std::uint32_t default_definition_id = 0;
};

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

SyntaxTokenKind TokenKindForGroupName(std::string_view group_name) {
  if (group_name.empty() || group_name == "default" || group_name == "identifier" ||
      StartsWith(group_name, "identifier.var") || StartsWith(group_name, "identifier.builtin")) {
    return SyntaxTokenKind::Plain;
  }

  if (group_name == "todo" || StartsWith(group_name, "todo") || group_name == "error" ||
      StartsWith(group_name, "error") || group_name == "underlined" ||
      StartsWith(group_name, "underlined")) {
    return SyntaxTokenKind::Constant;
  }

  if (StartsWith(group_name, "comment")) {
    return SyntaxTokenKind::Comment;
  }
  if (group_name == "string" || StartsWith(group_name, "string") ||
      Contains(group_name, ".string")) {
    return SyntaxTokenKind::String;
  }
  if (Contains(group_name, "number")) {
    return SyntaxTokenKind::Number;
  }
  if (group_name == "preproc" || StartsWith(group_name, "preproc") ||
      Contains(group_name, "macro")) {
    return SyntaxTokenKind::Preprocessor;
  }
  if (group_name == "symbol.tag" || StartsWith(group_name, "symbol.tag") ||
      group_name == "statement" || StartsWith(group_name, "statement") ||
      group_name == "special" || StartsWith(group_name, "special") ||
      Contains(group_name, "keyword")) {
    return SyntaxTokenKind::Keyword;
  }
  if (group_name == "symbol.operator" || StartsWith(group_name, "symbol.operator") ||
      group_name == "symbol.brackets" || StartsWith(group_name, "symbol.brackets") ||
      Contains(group_name, "operator")) {
    return SyntaxTokenKind::Operator;
  }
  if (group_name == "type" || StartsWith(group_name, "type") ||
      Contains(group_name, ".class") || Contains(group_name, ".struct")) {
    return SyntaxTokenKind::Type;
  }
  if (group_name == "constant" || StartsWith(group_name, "constant")) {
    return SyntaxTokenKind::Constant;
  }

  return SyntaxTokenKind::Plain;
}

Registry BuildRegistry() {
  Registry registry;
  registry.rules.reserve(kGeneratedRuleCount);
  for (std::size_t i = 0; i < kGeneratedRuleCount; ++i) {
    const GeneratedRuleData& generated = kGeneratedRules[i];
    Rule rule;
    rule.kind = generated.kind;
    rule.group = TokenKindForGroupName(generated.group_name == nullptr ? "" : generated.group_name);
    rule.limit_group =
        TokenKindForGroupName(generated.limit_group_name == nullptr ? "" : generated.limit_group_name);
    if (generated.pattern != nullptr) {
      rule.pattern = CompiledRegex(generated.pattern);
    }
    if (generated.start_regex != nullptr) {
      rule.start = CompiledRegex(generated.start_regex);
    }
    if (generated.end_regex != nullptr) {
      rule.end = CompiledRegex(generated.end_regex);
    }
    if (generated.skip_regex != nullptr) {
      rule.skip = CompiledRegex(generated.skip_regex);
    }
    rule.first_child = generated.first_child;
    rule.child_count = generated.child_count;
    rule.parent_region_id = static_cast<std::uint32_t>(generated.parent_region_id);
    registry.rules.push_back(std::move(rule));
  }

  registry.definitions.reserve(kGeneratedDefinitionCount);
  for (std::size_t i = 0; i < kGeneratedDefinitionCount; ++i) {
    const GeneratedDefinitionData& generated = kGeneratedDefinitions[i];
    Definition definition;
    definition.filetype = generated.filetype == nullptr ? "" : generated.filetype;
    if (generated.filename_regex != nullptr) {
      definition.filename_regex = CompiledRegex(generated.filename_regex);
    }
    if (generated.header_regex != nullptr) {
      definition.header_regex = CompiledRegex(generated.header_regex);
    }
    if (generated.signature_regex != nullptr) {
      definition.signature_regex = CompiledRegex(generated.signature_regex);
    }
    definition.first_rule = generated.first_rule;
    definition.rule_count = generated.rule_count;
    registry.definitions.push_back(std::move(definition));
    if (registry.default_definition_id == 0 && registry.definitions.back().filetype == "unknown") {
      registry.default_definition_id = static_cast<std::uint32_t>(i + 1);
    }
  }

  if (registry.default_definition_id == 0 && !registry.definitions.empty()) {
    registry.default_definition_id = 1;
  }
  return registry;
}

const Registry& GetRegistry() {
  static const Registry registry = BuildRegistry();
  return registry;
}

const Definition* DefinitionById(const Registry& registry, std::uint32_t definition_id) {
  if (definition_id == 0 || definition_id > registry.definitions.size()) {
    return nullptr;
  }
  return &registry.definitions[definition_id - 1];
}

const Rule* RuleByRegionId(const Registry& registry, std::uint32_t region_id) {
  if (region_id == 0 || region_id > registry.rules.size()) {
    return nullptr;
  }
  const Rule& rule = registry.rules[region_id - 1];
  return rule.kind == GeneratedRuleKind::Region ? &rule : nullptr;
}

void MarkRange(std::vector<SyntaxTokenKind>& tokens,
               std::size_t start,
               std::size_t end,
               SyntaxTokenKind kind) {
  const std::size_t safe_end = std::min(end, tokens.size());
  for (std::size_t i = start; i < safe_end; ++i) {
    tokens[i] = kind;
  }
}

void ApplyPatternRules(const Registry& registry,
                       const Definition& definition,
                       std::uint32_t parent_region_id,
                       std::string_view segment,
                       std::size_t absolute_offset,
                       std::vector<SyntaxTokenKind>& tokens,
                       SyntaxTokenKind base_kind) {
  if (segment.empty()) {
    return;
  }

  if (base_kind != SyntaxTokenKind::Plain) {
    MarkRange(tokens, absolute_offset, absolute_offset + segment.size(), base_kind);
  }

  const std::size_t end_rule = definition.first_rule + definition.rule_count;
  for (std::size_t index = definition.first_rule; index < end_rule; ++index) {
    const Rule& rule = registry.rules[index];
    if (rule.parent_region_id != parent_region_id || rule.kind != GeneratedRuleKind::Pattern ||
        !rule.pattern.valid()) {
      continue;
    }

    for (const MatchRange match : rule.pattern.FindAll(segment)) {
      MarkRange(tokens, absolute_offset + match.start, absolute_offset + match.end, rule.group);
    }
  }
}

std::optional<RegionStartMatch> FindEarliestRegionStart(const Registry& registry,
                                                        const Definition& definition,
                                                        std::uint32_t parent_region_id,
                                                        std::string_view segment,
                                                        std::size_t search_limit) {
  std::optional<RegionStartMatch> best_match;
  const std::size_t end_rule = definition.first_rule + definition.rule_count;
  for (std::size_t index = definition.first_rule; index < end_rule; ++index) {
    const Rule& rule = registry.rules[index];
    if (rule.parent_region_id != parent_region_id || rule.kind != GeneratedRuleKind::Region ||
        !rule.start.valid()) {
      continue;
    }

    const std::optional<MatchRange> match = rule.start.FindFirst(
        segment, rule.skip.valid() ? &rule.skip : nullptr);
    if (!match.has_value() || match->start >= search_limit) {
      continue;
    }

    if (!best_match.has_value() || match->start < best_match->match.start) {
      best_match = RegionStartMatch{
          .region_id = static_cast<std::uint32_t>(index + 1),
          .match = *match,
      };
    }
  }
  return best_match;
}

std::size_t HighlightRegion(const Registry& registry,
                            std::uint32_t definition_id,
                            std::uint32_t region_id,
                            std::string_view line,
                            std::size_t cursor,
                            std::vector<SyntaxTokenKind>& tokens,
                            SyntaxState* end_state);

std::size_t HighlightTopLevel(const Registry& registry,
                              std::uint32_t definition_id,
                              std::string_view line,
                              std::size_t cursor,
                              std::vector<SyntaxTokenKind>& tokens,
                              SyntaxState* end_state) {
  const Definition* definition = DefinitionById(registry, definition_id);
  if (definition == nullptr) {
    if (end_state != nullptr) {
      *end_state = SyntaxState{definition_id, 0};
    }
    return line.size();
  }

  while (cursor < line.size()) {
    const std::string_view tail = line.substr(cursor);
    const auto next_region = FindEarliestRegionStart(registry, *definition, 0, tail, tail.size());
    const std::size_t segment_end =
        next_region.has_value() ? cursor + next_region->match.start : line.size();
    ApplyPatternRules(registry, *definition, 0, line.substr(cursor, segment_end - cursor), cursor,
                      tokens, SyntaxTokenKind::Plain);

    if (!next_region.has_value()) {
      if (end_state != nullptr) {
        *end_state = SyntaxState{definition_id, 0};
      }
      return line.size();
    }

    const Rule* region = RuleByRegionId(registry, next_region->region_id);
    if (region == nullptr) {
      break;
    }

    MarkRange(tokens, cursor + next_region->match.start, cursor + next_region->match.end,
              region->limit_group);
    cursor += next_region->match.end;
    cursor = HighlightRegion(registry, definition_id, next_region->region_id, line, cursor, tokens,
                             end_state);
    if (end_state != nullptr && end_state->region_id != 0) {
      return cursor;
    }
  }

  if (end_state != nullptr) {
    *end_state = SyntaxState{definition_id, 0};
  }
  return line.size();
}

std::size_t HighlightRegion(const Registry& registry,
                            std::uint32_t definition_id,
                            std::uint32_t region_id,
                            std::string_view line,
                            std::size_t cursor,
                            std::vector<SyntaxTokenKind>& tokens,
                            SyntaxState* end_state) {
  const Rule* region = RuleByRegionId(registry, region_id);
  if (region == nullptr) {
    if (end_state != nullptr) {
      *end_state = SyntaxState{definition_id, 0};
    }
    return cursor;
  }

  while (cursor <= line.size()) {
    const std::string_view tail = line.substr(cursor);
    const std::optional<MatchRange> end_match =
        region->end.FindFirst(tail, region->skip.valid() ? &region->skip : nullptr);
    const bool closes_immediately = end_match.has_value() && end_match->start == 0;
    const std::size_t search_limit = end_match.has_value() ? end_match->start : tail.size();
    const Definition* definition = DefinitionById(registry, definition_id);
    const auto next_region = closes_immediately || definition == nullptr
                                 ? std::optional<RegionStartMatch>{}
                                 : FindEarliestRegionStart(registry, *definition, region_id, tail,
                                                           search_limit);
    const std::size_t segment_end =
        next_region.has_value() ? next_region->match.start : search_limit;

    if (definition != nullptr) {
      ApplyPatternRules(registry, *definition, region_id, tail.substr(0, segment_end), cursor,
                        tokens, region->group);
    } else if (region->group != SyntaxTokenKind::Plain) {
      MarkRange(tokens, cursor, cursor + segment_end, region->group);
    }

    if (next_region.has_value()) {
      const Rule* nested_region = RuleByRegionId(registry, next_region->region_id);
      if (nested_region == nullptr) {
        break;
      }

      MarkRange(tokens, cursor + next_region->match.start, cursor + next_region->match.end,
                nested_region->limit_group);
      cursor += next_region->match.end;
      cursor = HighlightRegion(registry, definition_id, next_region->region_id, line, cursor,
                               tokens, end_state);
      if (end_state != nullptr && end_state->region_id != 0) {
        return cursor;
      }
      continue;
    }

    if (end_match.has_value()) {
      MarkRange(tokens, cursor + end_match->start, cursor + end_match->end, region->limit_group);
      if (end_state != nullptr) {
        *end_state = SyntaxState{definition_id, 0};
      }
      return cursor + end_match->end;
    }

    if (end_state != nullptr) {
      *end_state = SyntaxState{definition_id, region_id};
    }
    return line.size();
  }

  if (end_state != nullptr) {
    *end_state = SyntaxState{definition_id, 0};
  }
  return line.size();
}

std::uint32_t DetectDefinitionId(const Registry& registry,
                                 const std::filesystem::path& path,
                                 const std::vector<std::string>* lines,
                                 std::string_view first_line) {
  const std::string path_text = path.generic_string();
  const std::string_view header_line =
      lines != nullptr && !lines->empty() ? std::string_view(lines->front()) : first_line;

  std::vector<std::uint32_t> filename_matches;
  std::vector<std::uint32_t> header_matches;
  filename_matches.reserve(registry.definitions.size());
  header_matches.reserve(registry.definitions.size());

  for (std::size_t i = 0; i < registry.definitions.size(); ++i) {
    const Definition& definition = registry.definitions[i];
    const std::uint32_t definition_id = static_cast<std::uint32_t>(i + 1);

    if (definition.filename_regex.valid() && definition.filename_regex.Matches(path_text)) {
      filename_matches.push_back(definition_id);
      continue;
    }
    if (filename_matches.empty() && definition.header_regex.valid() &&
        definition.header_regex.Matches(header_line)) {
      header_matches.push_back(definition_id);
    }
  }

  const std::vector<std::uint32_t>& matches =
      filename_matches.empty() ? header_matches : filename_matches;
  if (matches.empty()) {
    return registry.default_definition_id;
  }
  if (matches.size() == 1 || lines == nullptr) {
    return matches.front();
  }

  const std::size_t line_limit = std::min(lines->size(), kSignatureDetectLineLimit);
  for (const std::uint32_t definition_id : matches) {
    const Definition* definition = DefinitionById(registry, definition_id);
    if (definition == nullptr || !definition->signature_regex.valid()) {
      continue;
    }
    for (std::size_t i = 0; i < line_limit; ++i) {
      if (definition->signature_regex.Matches((*lines)[i])) {
        return definition_id;
      }
    }
  }

  return matches.front();
}

}  // namespace

SyntaxState DetectState(const std::filesystem::path& path, const std::vector<std::string>& lines) {
  const Registry& registry = GetRegistry();
  return SyntaxState{
      .definition_id = DetectDefinitionId(registry, path, &lines, {}),
      .region_id = 0,
  };
}

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line) {
  const Registry& registry = GetRegistry();

  const std::uint32_t definition_id =
      state.definition_id != 0 ? state.definition_id
                               : DetectDefinitionId(registry, path, nullptr, first_line);

  HighlightedLine result;
  result.tokens.assign(line.size(), SyntaxTokenKind::Plain);
  result.end_state = SyntaxState{definition_id, 0};
  if (line.empty()) {
    return result;
  }

  std::size_t cursor = 0;
  if (state.region_id != 0 && state.definition_id == definition_id) {
    cursor = HighlightRegion(registry, definition_id, state.region_id, line, 0, result.tokens,
                             &result.end_state);
    if (result.end_state.region_id != 0) {
      return result;
    }
  }

  HighlightTopLevel(registry, definition_id, line, cursor, result.tokens, &result.end_state);
  return result;
}

}  // namespace microide::editor::runtime_syntax
