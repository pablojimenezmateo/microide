#include "editor/RuntimeSyntaxRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/RuntimeSyntaxData.h"
#include "util/PerformanceTrace.h"
#include "util/RegexUtil.h"
#include "util/StringUtil.h"

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

using CompiledRegex = util::CompiledRegex;
void FindAllRegex(std::string_view text,
                  const CompiledRegex& pattern,
                  std::vector<MatchRange>& matches);

util::RegexMatchData& ReusableMatchData(const CompiledRegex& pattern) {
  thread_local std::unordered_map<const CompiledRegex*, util::RegexMatchData> match_data_by_pattern;
  auto [it, inserted] = match_data_by_pattern.try_emplace(&pattern);
  if (inserted || !it->second.valid()) {
    it->second = pattern.CreateMatchData();
  }
  return it->second;
}

std::string NormalizePattern(std::string_view pattern) {
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

CompiledRegex CompileSyntaxRegex(std::string_view pattern) {
  return CompiledRegex(NormalizePattern(pattern), kRegexCompileOptions);
}

CompiledRegex CompileSyntaxRegex(std::string_view pattern, std::string error_prefix) {
  return CompiledRegex(NormalizePattern(pattern), kRegexCompileOptions, std::move(error_prefix));
}

std::size_t AdvanceToNextCodePoint(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return text.size() + 1;
  }

  std::size_t next = offset + 1;
  while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0u) == 0x80u) {
    ++next;
  }
  return next;
}

std::optional<MatchRange> FindFirstRegex(std::string_view text,
                                         const CompiledRegex& pattern,
                                         const CompiledRegex* skip = nullptr,
                                         bool allow_zero_length = false) {
  if (!pattern.valid()) {
    return std::nullopt;
  }
  if (skip != nullptr && skip->valid()) {
    thread_local std::string masked_buf;
    thread_local std::vector<MatchRange> skip_matches;
    masked_buf.assign(text);
    FindAllRegex(text, *skip, skip_matches);
    for (const MatchRange match : skip_matches) {
      std::fill(masked_buf.begin() + static_cast<std::ptrdiff_t>(match.start),
                masked_buf.begin() + static_cast<std::ptrdiff_t>(match.end), '\0');
    }
    return FindFirstRegex(masked_buf, pattern, nullptr, allow_zero_length);
  }

  auto& match_data = ReusableMatchData(pattern);
  if (!match_data.valid()) {
    return std::nullopt;
  }

  const int rc = pattern.Match(text, 0, match_data);
  if (rc < 0) {
    return std::nullopt;
  }

  util::RegexMatchRange range;
  if (!pattern.CaptureRange(match_data, text.size(), &range) ||
      range.start > range.end || (!allow_zero_length && range.start >= range.end)) {
    return std::nullopt;
  }
  return MatchRange{range.start, range.end};
}

void FindAllRegex(std::string_view text,
                  const CompiledRegex& pattern,
                  std::vector<MatchRange>& matches) {
  matches.clear();
  if (!pattern.valid() || text.empty()) {
    return;
  }

  auto& match_data = ReusableMatchData(pattern);
  if (!match_data.valid()) {
    return;
  }

  for (std::size_t offset = 0; offset <= text.size();) {
    const int rc = pattern.Match(text, offset, match_data);
    if (rc < 0) {
      break;
    }

    util::RegexMatchRange range;
    if (!pattern.CaptureRange(match_data, text.size(), &range)) {
      break;
    }
    if (range.start >= range.end) {
      offset = AdvanceToNextCodePoint(text, range.end);
      continue;
    }

    matches.push_back(MatchRange{range.start, range.end});
    offset = range.end;
  }
}

bool RegexMatches(std::string_view text, const CompiledRegex& pattern) {
  return FindFirstRegex(text, pattern).has_value();
}

struct FiletypeCandidateSet {
  const std::string_view* names = nullptr;
  std::size_t count = 0;
};

template <std::size_t N>
constexpr FiletypeCandidateSet MakeCandidateSet(const std::array<std::string_view, N>& names) {
  return FiletypeCandidateSet{names.data(), names.size()};
}

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
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> pattern_rule_indices_by_parent;
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> region_rule_indices_by_parent;
};

struct Registry {
  std::vector<Definition> definitions;
  std::vector<Rule> rules;
  std::uint32_t default_definition_id = 0;
};

struct BuildOutput {
  Registry registry;
  std::size_t loaded_runtime_definition_count = 0;
};

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

SyntaxTokenKind TokenKindForGroupName(std::string_view group_name) {
  if (group_name.empty() || group_name == "default" || group_name == "identifier" ||
      group_name.starts_with("identifier.var") || group_name.starts_with("identifier.builtin")) {
    return SyntaxTokenKind::Plain;
  }

  if (group_name == "todo" || group_name.starts_with("todo") || group_name == "error" ||
      group_name.starts_with("error") || group_name == "underlined" ||
      group_name.starts_with("underlined")) {
    return SyntaxTokenKind::Constant;
  }

  if (group_name.starts_with("comment")) {
    return SyntaxTokenKind::Comment;
  }
  if (group_name == "string" || group_name.starts_with("string") ||
      Contains(group_name, ".string")) {
    return SyntaxTokenKind::String;
  }
  if (Contains(group_name, "number")) {
    return SyntaxTokenKind::Number;
  }
  if (group_name == "preproc" || group_name.starts_with("preproc") ||
      Contains(group_name, "macro")) {
    return SyntaxTokenKind::Preprocessor;
  }
  if (group_name == "symbol.tag" || group_name.starts_with("symbol.tag") ||
      group_name == "statement" || group_name.starts_with("statement") ||
      group_name == "special" || group_name.starts_with("special") ||
      Contains(group_name, "keyword")) {
    return SyntaxTokenKind::Keyword;
  }
  if (group_name == "symbol.operator" || group_name.starts_with("symbol.operator") ||
      group_name == "symbol.brackets" || group_name.starts_with("symbol.brackets") ||
      Contains(group_name, "operator")) {
    return SyntaxTokenKind::Operator;
  }
  if (group_name == "type" || group_name.starts_with("type") ||
      Contains(group_name, ".class") || Contains(group_name, ".struct")) {
    return SyntaxTokenKind::Type;
  }
  if (group_name == "constant" || group_name.starts_with("constant")) {
    return SyntaxTokenKind::Constant;
  }

  return SyntaxTokenKind::Plain;
}

void RecordBuildError(std::vector<std::string>* errors, std::string error_message) {
  if (errors != nullptr && !error_message.empty()) {
    errors->push_back(std::move(error_message));
  }
}

std::string JoinSyntaxPatterns(const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return {};
  }

  std::string result;
  std::size_t total = 0;
  for (const auto& p : patterns) {
    total += p.size() + 4;  // "(?:" + p + ")" + "|"
  }
  result.reserve(total);
  for (std::size_t i = 0; i < patterns.size(); ++i) {
    if (i != 0) {
      result += '|';
    }
    result += "(?:";
    result += patterns[i];
    result += ')';
  }
  return result;
}

bool ValidateCompiledRegex(const CompiledRegex& regex, std::vector<std::string>* errors) {
  if (regex.valid() || regex.error().empty()) {
    return regex.valid();
  }
  RecordBuildError(errors, regex.error());
  return false;
}

CompiledRegex CompileMergedSyntaxRegex(const std::vector<std::string>& patterns,
                                       std::string error_prefix,
                                       std::vector<std::string>* errors) {
  if (patterns.empty()) {
    return {};
  }

  CompiledRegex regex = CompileSyntaxRegex(JoinSyntaxPatterns(patterns), std::move(error_prefix));
  if (!ValidateCompiledRegex(regex, errors)) {
    return {};
  }
  return regex;
}

bool AppendRuntimeRule(Registry& registry,
                       const RuntimeSyntaxRuleData& runtime_rule,
                       std::uint32_t parent_region_id,
                       const std::filesystem::path& source_path,
                       std::vector<std::string>* errors) {
  const std::size_t rule_index = registry.rules.size();
  Rule rule;
  rule.kind = runtime_rule.kind;
  rule.group = TokenKindForGroupName(runtime_rule.group_name);
  rule.limit_group = TokenKindForGroupName(
      runtime_rule.limit_group_name.empty() ? runtime_rule.group_name : runtime_rule.limit_group_name);
  rule.parent_region_id = parent_region_id;

  const std::string source_text = source_path.string();
  if (runtime_rule.kind == GeneratedRuleKind::Pattern) {
    rule.pattern = CompileSyntaxRegex(runtime_rule.pattern,
                                      "invalid syntax pattern in " + source_text);
    if (!ValidateCompiledRegex(rule.pattern, errors)) {
      return false;
    }
  } else {
    rule.start = CompileSyntaxRegex(runtime_rule.start_regex,
                                    "invalid syntax region start in " + source_text);
    rule.end = CompileSyntaxRegex(runtime_rule.end_regex,
                                  "invalid syntax region end in " + source_text);
    if (!runtime_rule.skip_regex.empty()) {
      rule.skip = CompileSyntaxRegex(runtime_rule.skip_regex,
                                     "invalid syntax region skip in " + source_text);
    }
    if (!ValidateCompiledRegex(rule.start, errors) || !ValidateCompiledRegex(rule.end, errors) ||
        (!runtime_rule.skip_regex.empty() && !ValidateCompiledRegex(rule.skip, errors))) {
      return false;
    }
  }

  registry.rules.push_back(std::move(rule));

  if (runtime_rule.kind == GeneratedRuleKind::Region) {
    if (!runtime_rule.children.empty()) {
      registry.rules[rule_index].first_child = registry.rules.size();
    }
    const std::uint32_t region_id = static_cast<std::uint32_t>(rule_index + 1);
    for (const auto& child : runtime_rule.children) {
      if (!AppendRuntimeRule(registry, child, region_id, source_path, errors)) {
        return false;
      }
      ++registry.rules[rule_index].child_count;
    }
  }

  return true;
}

bool AppendRuntimeDefinition(Registry& registry,
                             const RuntimeSyntaxDefinitionData& runtime_definition,
                             std::vector<std::string>* errors) {
  const std::size_t rules_before = registry.rules.size();
  const std::size_t definitions_before = registry.definitions.size();

  Definition definition;
  definition.filetype = runtime_definition.filetype;
  definition.filename_regex = CompileMergedSyntaxRegex(
      runtime_definition.filename_patterns,
      "invalid syntax filename matcher in " + runtime_definition.source_path.string(), errors);
  definition.header_regex = CompileMergedSyntaxRegex(
      runtime_definition.header_patterns,
      "invalid syntax header matcher in " + runtime_definition.source_path.string(), errors);
  definition.signature_regex = CompileMergedSyntaxRegex(
      runtime_definition.signature_patterns,
      "invalid syntax signature matcher in " + runtime_definition.source_path.string(), errors);
  if ((!runtime_definition.filename_patterns.empty() && !definition.filename_regex.valid()) ||
      (!runtime_definition.header_patterns.empty() && !definition.header_regex.valid()) ||
      (!runtime_definition.signature_patterns.empty() && !definition.signature_regex.valid())) {
    registry.rules.resize(rules_before);
    registry.definitions.resize(definitions_before);
    return false;
  }

  definition.first_rule = registry.rules.size();
  for (const auto& rule : runtime_definition.rules) {
    if (!AppendRuntimeRule(registry, rule, 0, runtime_definition.source_path, errors)) {
      registry.rules.resize(rules_before);
      registry.definitions.resize(definitions_before);
      return false;
    }
  }
  definition.rule_count = registry.rules.size() - definition.first_rule;
  registry.definitions.push_back(std::move(definition));
  return true;
}

void AppendGeneratedDefinitions(Registry& registry) {
  const std::size_t rule_offset = registry.rules.size();
  registry.rules.reserve(registry.rules.size() + kGeneratedRuleCount);
  for (std::size_t i = 0; i < kGeneratedRuleCount; ++i) {
    const GeneratedRuleData& generated = kGeneratedRules[i];
    Rule rule;
    rule.kind = generated.kind;
    rule.group = TokenKindForGroupName(generated.group_name == nullptr ? "" : generated.group_name);
    rule.limit_group =
        TokenKindForGroupName(generated.limit_group_name == nullptr ? "" : generated.limit_group_name);
    if (generated.pattern != nullptr) {
      rule.pattern = CompileSyntaxRegex(generated.pattern);
    }
    if (generated.start_regex != nullptr) {
      rule.start = CompileSyntaxRegex(generated.start_regex);
    }
    if (generated.end_regex != nullptr) {
      rule.end = CompileSyntaxRegex(generated.end_regex);
    }
    if (generated.skip_regex != nullptr) {
      rule.skip = CompileSyntaxRegex(generated.skip_regex);
    }
    rule.first_child =
        generated.child_count == 0 ? 0 : rule_offset + generated.first_child;
    rule.child_count = generated.child_count;
    rule.parent_region_id = generated.parent_region_id == 0
                                ? 0
                                : static_cast<std::uint32_t>(rule_offset + generated.parent_region_id);
    registry.rules.push_back(std::move(rule));
  }

  registry.definitions.reserve(registry.definitions.size() + kGeneratedDefinitionCount);
  for (std::size_t i = 0; i < kGeneratedDefinitionCount; ++i) {
    const GeneratedDefinitionData& generated = kGeneratedDefinitions[i];
    Definition definition;
    definition.filetype = generated.filetype == nullptr ? "" : generated.filetype;
    if (generated.filename_regex != nullptr) {
      definition.filename_regex = CompileSyntaxRegex(generated.filename_regex);
    }
    if (generated.header_regex != nullptr) {
      definition.header_regex = CompileSyntaxRegex(generated.header_regex);
    }
    if (generated.signature_regex != nullptr) {
      definition.signature_regex = CompileSyntaxRegex(generated.signature_regex);
    }
    definition.first_rule = rule_offset + generated.first_rule;
    definition.rule_count = generated.rule_count;
    registry.definitions.push_back(std::move(definition));
  }
}

void PartitionDefinitionRules(Registry& registry) {
  for (Definition& definition : registry.definitions) {
    definition.pattern_rule_indices_by_parent.clear();
    definition.region_rule_indices_by_parent.clear();
    const std::size_t end_rule = definition.first_rule + definition.rule_count;
    for (std::size_t index = definition.first_rule; index < end_rule; ++index) {
      const Rule& rule = registry.rules[index];
      auto& table = rule.kind == GeneratedRuleKind::Pattern
                        ? definition.pattern_rule_indices_by_parent
                        : definition.region_rule_indices_by_parent;
      table[rule.parent_region_id].push_back(index);
    }
  }
}

Registry BuildGeneratedRegistry() {
  Registry registry;
  AppendGeneratedDefinitions(registry);
  for (std::size_t i = 0; i < registry.definitions.size(); ++i) {
    if (registry.definitions[i].filetype == "unknown") {
      registry.default_definition_id = static_cast<std::uint32_t>(i + 1);
      break;
    }
  }
  if (registry.default_definition_id == 0 && !registry.definitions.empty()) {
    registry.default_definition_id = 1;
  }
  PartitionDefinitionRules(registry);
  return registry;
}

const Registry& BuiltInRegistry() {
  static const Registry registry = BuildGeneratedRegistry();
  return registry;
}

void AppendRegistryWithOffset(Registry& destination, const Registry& source) {
  const std::size_t rule_offset = destination.rules.size();
  const std::size_t definition_offset = destination.definitions.size();

  destination.rules.reserve(destination.rules.size() + source.rules.size());
  for (Rule rule : source.rules) {
    if (rule.first_child != 0) {
      rule.first_child += rule_offset;
    }
    if (rule.parent_region_id != 0) {
      rule.parent_region_id += static_cast<std::uint32_t>(rule_offset);
    }
    destination.rules.push_back(std::move(rule));
  }

  destination.definitions.reserve(destination.definitions.size() + source.definitions.size());
  for (Definition definition : source.definitions) {
    definition.first_rule += rule_offset;
    destination.definitions.push_back(std::move(definition));
  }

  if (destination.default_definition_id == 0 && source.default_definition_id != 0) {
    destination.default_definition_id =
        static_cast<std::uint32_t>(definition_offset + source.default_definition_id);
  }
}

BuildOutput BuildRegistry(const std::vector<RuntimeSyntaxDefinitionData>& runtime_definitions,
                          std::vector<std::string>* errors) {
  BuildOutput output;
  if (runtime_definitions.empty()) {
    output.registry = BuiltInRegistry();
    return output;
  }

  for (const auto& definition : runtime_definitions) {
    if (AppendRuntimeDefinition(output.registry, definition, errors)) {
      ++output.loaded_runtime_definition_count;
    }
  }

  AppendRegistryWithOffset(output.registry, BuiltInRegistry());

  for (std::size_t i = 0; i < output.registry.definitions.size(); ++i) {
    if (output.registry.definitions[i].filetype == "unknown") {
      output.registry.default_definition_id = static_cast<std::uint32_t>(i + 1);
      break;
    }
  }

  if (output.registry.default_definition_id == 0 && !output.registry.definitions.empty()) {
    output.registry.default_definition_id = 1;
  }

  PartitionDefinitionRules(output.registry);
  return output;
}

// Empty by default. Populated only when plugins reload syntax definitions
// via ReloadDefinitions(). When empty, GetRegistry() aliases BuiltInRegistry()
// directly, avoiding a multi-MB copy of the generated rule/definition tables.
Registry& MutableRegistry() {
  static Registry registry;
  return registry;
}

std::size_t& MutableRegistryRevision() {
  static std::size_t revision = 1;
  return revision;
}

const Registry& GetRegistry() {
  const Registry& mutable_registry = MutableRegistry();
  return mutable_registry.definitions.empty() ? BuiltInRegistry() : mutable_registry;
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

  const auto rule_it = definition.pattern_rule_indices_by_parent.find(parent_region_id);
  if (rule_it == definition.pattern_rule_indices_by_parent.end()) {
    return;
  }
  thread_local std::vector<MatchRange> matches;
  for (const std::size_t index : rule_it->second) {
    const Rule& rule = registry.rules[index];
    if (!rule.pattern.valid()) {
      continue;
    }

    FindAllRegex(segment, rule.pattern, matches);
    for (const MatchRange match : matches) {
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
  const auto rule_it = definition.region_rule_indices_by_parent.find(parent_region_id);
  if (rule_it == definition.region_rule_indices_by_parent.end()) {
    return std::nullopt;
  }
  for (const std::size_t index : rule_it->second) {
    const Rule& rule = registry.rules[index];
    if (!rule.start.valid()) {
      continue;
    }

    const std::optional<MatchRange> match =
        FindFirstRegex(segment, rule.start, rule.skip.valid() ? &rule.skip : nullptr);
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
        FindFirstRegex(tail, region->end, region->skip.valid() ? &region->skip : nullptr, true);
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
  static constexpr std::array<std::string_view, 1> kCCandidates = {"c"};
  static constexpr std::array<std::string_view, 1> kCMakeCandidates = {"cmake"};
  static constexpr std::array<std::string_view, 1> kCPlusPlusCandidates = {"c++"};
  static constexpr std::array<std::string_view, 1> kCSharpCandidates = {"csharp"};
  static constexpr std::array<std::string_view, 1> kDockerfileCandidates = {"dockerfile"};
  static constexpr std::array<std::string_view, 1> kGoCandidates = {"go"};
  static constexpr std::array<std::string_view, 1> kGoModCandidates = {"gomod"};
  static constexpr std::array<std::string_view, 1> kHcCandidates = {"hc"};
  static constexpr std::array<std::string_view, 1> kJavaCandidates = {"java"};
  static constexpr std::array<std::string_view, 1> kJavaScriptCandidates = {"javascript"};
  static constexpr std::array<std::string_view, 1> kJsonCandidates = {"json"};
  static constexpr std::array<std::string_view, 1> kKotlinCandidates = {"kotlin"};
  static constexpr std::array<std::string_view, 1> kLuaCandidates = {"lua"};
  static constexpr std::array<std::string_view, 1> kMakefileCandidates = {"makefile"};
  static constexpr std::array<std::string_view, 1> kMarkdownCandidates = {"markdown"};
  static constexpr std::array<std::string_view, 1> kMesonCandidates = {"meson"};
  static constexpr std::array<std::string_view, 1> kObjectiveCCandidates = {"objective-c"};
  static constexpr std::array<std::string_view, 1> kPython2Candidates = {"python2"};
  static constexpr std::array<std::string_view, 1> kPythonCandidates = {"python"};
  static constexpr std::array<std::string_view, 1> kRubyCandidates = {"ruby"};
  static constexpr std::array<std::string_view, 1> kRustCandidates = {"rust"};
  static constexpr std::array<std::string_view, 1> kShellCandidates = {"shell"};
  static constexpr std::array<std::string_view, 1> kSwiftCandidates = {"swift"};
  static constexpr std::array<std::string_view, 1> kTomlCandidates = {"toml"};
  static constexpr std::array<std::string_view, 1> kTypeScriptCandidates = {"typescript"};
  static constexpr std::array<std::string_view, 1> kYamlCandidates = {"yaml"};
  static constexpr std::array<std::string_view, 2> kCPlusPlusHHCandidates = {"c++", "hc"};
  static constexpr std::array<std::string_view, 2> kObjectiveCMCandidates = {"objective-c",
                                                                              "octave"};
  static constexpr std::array<std::string_view, 4> kHeaderCandidates = {"c", "c++", "hc",
                                                                         "objective-c"};

  const auto definition_id_for_filetype = [&](std::string_view filetype) -> std::uint32_t {
    for (std::size_t i = 0; i < registry.definitions.size(); ++i) {
      if (registry.definitions[i].filetype == filetype) {
        return static_cast<std::uint32_t>(i + 1);
      }
    }
    return 0;
  };
  const auto resolve_from_matches = [&](const std::vector<std::uint32_t>& matches,
                                        std::string_view signature_scope_label) {
    if (matches.empty()) {
      return registry.default_definition_id;
    }
    if (matches.size() == 1 || lines == nullptr) {
      return matches.front();
    }

    const std::size_t line_limit = std::min(lines->size(), kSignatureDetectLineLimit);
    util::PerformanceTrace::Scope signature_scope(signature_scope_label);
    for (const std::uint32_t definition_id : matches) {
      const Definition* definition = DefinitionById(registry, definition_id);
      if (definition == nullptr || !definition->signature_regex.valid()) {
        continue;
      }
      for (std::size_t i = 0; i < line_limit; ++i) {
        if (RegexMatches((*lines)[i], definition->signature_regex)) {
          return definition_id;
        }
      }
    }

    return matches.front();
  };
  const auto try_fast_candidates =
      [&](FiletypeCandidateSet candidates) -> std::optional<std::uint32_t> {
    if (candidates.names == nullptr || candidates.count == 0) {
      return std::nullopt;
    }

    const std::string path_text = path.generic_string();
    util::PerformanceTrace::Scope fast_scope(
        "RuntimeSyntaxRegistry::DetectDefinitionId::FastFilenameMatches");
    std::vector<std::uint32_t> matches;
    matches.reserve(candidates.count);
    for (std::size_t i = 0; i < candidates.count; ++i) {
      const std::uint32_t definition_id = definition_id_for_filetype(candidates.names[i]);
      if (definition_id == 0) {
        continue;
      }
      const Definition* definition = DefinitionById(registry, definition_id);
      if (definition == nullptr || !definition->filename_regex.valid()) {
        continue;
      }
      if (RegexMatches(path_text, definition->filename_regex)) {
        matches.push_back(definition_id);
      }
    }
    if (matches.empty()) {
      return std::nullopt;
    }
    return resolve_from_matches(matches,
                                "RuntimeSyntaxRegistry::DetectDefinitionId::FastSignatureScan");
  };
  const auto fast_candidate_set_for_path =
      [&](const std::filesystem::path& candidate_path) -> std::optional<FiletypeCandidateSet> {
    const std::string lower_name = util::ToLowerAscii(candidate_path.filename().string());
    const std::string lower_extension = util::ToLowerAscii(candidate_path.extension().string());

    if (lower_name == "cmakelists.txt") {
      return MakeCandidateSet(kCMakeCandidates);
    }
    if (lower_name == "dockerfile" || lower_name == "containerfile") {
      return MakeCandidateSet(kDockerfileCandidates);
    }
    if (lower_name == "go.mod") {
      return MakeCandidateSet(kGoModCandidates);
    }
    if (lower_name == "makefile") {
      return MakeCandidateSet(kMakefileCandidates);
    }
    if (lower_name == "meson.build" || lower_name == "meson_options.txt" ||
        lower_name == "meson.options") {
      return MakeCandidateSet(kMesonCandidates);
    }

    if (lower_extension == ".c") {
      return MakeCandidateSet(kCCandidates);
    }
    if (lower_extension == ".cc" || lower_extension == ".cpp" || lower_extension == ".cxx" ||
        lower_extension == ".hpp" || lower_extension == ".hxx") {
      return MakeCandidateSet(kCPlusPlusCandidates);
    }
    if (lower_extension == ".cs") {
      return MakeCandidateSet(kCSharpCandidates);
    }
    if (lower_extension == ".def" || lower_extension == ".h" || lower_extension == ".ii") {
      return MakeCandidateSet(kHeaderCandidates);
    }
    if (lower_extension == ".go") {
      return MakeCandidateSet(kGoCandidates);
    }
    if (lower_extension == ".hc") {
      return MakeCandidateSet(kHcCandidates);
    }
    if (lower_extension == ".hh") {
      return MakeCandidateSet(kCPlusPlusHHCandidates);
    }
    if (lower_extension == ".java") {
      return MakeCandidateSet(kJavaCandidates);
    }
    if (lower_extension == ".js" || lower_extension == ".mjs" || lower_extension == ".cjs") {
      return MakeCandidateSet(kJavaScriptCandidates);
    }
    if (lower_extension == ".json") {
      return MakeCandidateSet(kJsonCandidates);
    }
    if (lower_extension == ".kt" || lower_extension == ".kts") {
      return MakeCandidateSet(kKotlinCandidates);
    }
    if (lower_extension == ".lua") {
      return MakeCandidateSet(kLuaCandidates);
    }
    if (lower_extension == ".m") {
      return MakeCandidateSet(kObjectiveCMCandidates);
    }
    if (lower_extension == ".markdown" || lower_extension == ".md" || lower_extension == ".mkd" ||
        lower_extension == ".mkdn") {
      return MakeCandidateSet(kMarkdownCandidates);
    }
    if (lower_extension == ".mm") {
      return MakeCandidateSet(kObjectiveCCandidates);
    }
    if (lower_extension == ".py" || lower_extension == ".py3" || lower_extension == ".pyw") {
      return MakeCandidateSet(kPythonCandidates);
    }
    if (lower_extension == ".py2") {
      return MakeCandidateSet(kPython2Candidates);
    }
    if (lower_extension == ".rb" || lower_extension == ".rake") {
      return MakeCandidateSet(kRubyCandidates);
    }
    if (lower_extension == ".rs") {
      return MakeCandidateSet(kRustCandidates);
    }
    if (lower_extension == ".sh" || lower_extension == ".bash") {
      return MakeCandidateSet(kShellCandidates);
    }
    if (lower_extension == ".swift") {
      return MakeCandidateSet(kSwiftCandidates);
    }
    if (lower_extension == ".toml") {
      return MakeCandidateSet(kTomlCandidates);
    }
    if (lower_extension == ".ts" || lower_extension == ".tsx") {
      return MakeCandidateSet(kTypeScriptCandidates);
    }
    if (lower_extension == ".yaml" || lower_extension == ".yml") {
      return MakeCandidateSet(kYamlCandidates);
    }

    return std::nullopt;
  };

  if (const auto fast_candidates = fast_candidate_set_for_path(path); fast_candidates.has_value()) {
    if (const auto fast_match = try_fast_candidates(*fast_candidates); fast_match.has_value()) {
      return *fast_match;
    }
  }

  const std::string path_text = path.generic_string();
  const std::string_view header_line =
      lines != nullptr && !lines->empty() ? std::string_view(lines->front()) : first_line;

  std::vector<std::uint32_t> filename_matches;
  std::vector<std::uint32_t> header_matches;
  filename_matches.reserve(registry.definitions.size());
  header_matches.reserve(registry.definitions.size());

  {
    util::PerformanceTrace::Scope generic_scope(
        "RuntimeSyntaxRegistry::DetectDefinitionId::GenericFilenameHeaderMatches");
    for (std::size_t i = 0; i < registry.definitions.size(); ++i) {
      const Definition& definition = registry.definitions[i];
      const std::uint32_t definition_id = static_cast<std::uint32_t>(i + 1);

      if (definition.filename_regex.valid() && RegexMatches(path_text, definition.filename_regex)) {
        filename_matches.push_back(definition_id);
        continue;
      }
      if (filename_matches.empty() && definition.header_regex.valid() &&
          RegexMatches(header_line, definition.header_regex)) {
        header_matches.push_back(definition_id);
      }
    }
  }

  const std::vector<std::uint32_t>& matches =
      filename_matches.empty() ? header_matches : filename_matches;
  return resolve_from_matches(matches,
                              "RuntimeSyntaxRegistry::DetectDefinitionId::GenericSignatureScan");
}

}  // namespace

RuntimeSyntaxReloadResult ReloadDefinitions(
    const std::vector<RuntimeSyntaxDefinitionData>& definitions,
    std::vector<std::string>* errors) {
  std::vector<std::string> local_errors;
  BuildOutput build = BuildRegistry(definitions, &local_errors);
  MutableRegistry() = std::move(build.registry);
  ++MutableRegistryRevision();
  if (errors != nullptr) {
    *errors = local_errors;
  }
  return RuntimeSyntaxReloadResult{
      .built_in_definition_count = kGeneratedDefinitionCount,
      .plugin_definition_count = build.loaded_runtime_definition_count,
      .error_count = local_errors.size(),
  };
}

void EnsureInitialized() {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::EnsureInitialized");
  // Touch BuiltInRegistry() directly so the static magic-init runs here
  // even when MutableRegistry() is empty (the lazy-alias case).
  (void)BuiltInRegistry();
}

std::size_t RegistryRevision() {
  return MutableRegistryRevision();
}

SyntaxState DetectState(const std::filesystem::path& path, const std::vector<std::string>& lines) {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::DetectState");
  const Registry& registry = GetRegistry();
  return SyntaxState{
      .definition_id = DetectDefinitionId(registry, path, &lines, {}),
      .region_id = 0,
  };
}

std::string DetectFiletype(const std::filesystem::path& path, const std::vector<std::string>& lines) {
  const Registry& registry = GetRegistry();
  const std::uint32_t definition_id = DetectDefinitionId(registry, path, &lines, {});
  const Definition* definition = DefinitionById(registry, definition_id);
  return definition == nullptr ? std::string{} : definition->filetype;
}

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line) {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::HighlightLine");
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

  {
    util::PerformanceTrace::Scope top_scope("RuntimeSyntaxRegistry::HighlightLine::TopLevel");
    HighlightTopLevel(registry, definition_id, line, cursor, result.tokens, &result.end_state);
  }
  return result;
}

SyntaxState AdvanceState(std::string_view line,
                         const std::filesystem::path& path,
                         const SyntaxState& state,
                         std::string_view first_line) {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::AdvanceState");
  const Registry& registry = GetRegistry();
  const std::uint32_t definition_id =
      state.definition_id != 0 ? state.definition_id
                               : DetectDefinitionId(registry, path, nullptr, first_line);
  SyntaxState end_state{definition_id, 0};
  if (line.empty()) {
    return end_state;
  }

  std::vector<SyntaxTokenKind> no_tokens;
  std::size_t cursor = 0;
  if (state.region_id != 0 && state.definition_id == definition_id) {
    cursor = HighlightRegion(registry, definition_id, state.region_id, line, 0, no_tokens,
                             &end_state);
    if (end_state.region_id != 0) {
      return end_state;
    }
  }

  {
    util::PerformanceTrace::Scope top_scope("RuntimeSyntaxRegistry::AdvanceState::TopLevel");
    HighlightTopLevel(registry, definition_id, line, cursor, no_tokens, &end_state);
  }
  return end_state;
}

}  // namespace microide::editor::runtime_syntax
