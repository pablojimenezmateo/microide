#include "editor/RuntimeSyntaxRegistry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/HighlightPrefetch.h"
#include "editor/RuntimeSyntaxData.h"
#include "util/PerformanceTrace.h"
#include "util/RegexUtil.h"
#include "util/StringUtil.h"

namespace microide::editor::runtime_syntax {

namespace {

constexpr std::size_t kSignatureDetectLineLimit = 64;
// PCRE2_MATCH_INVALID_UTF (PCRE2 >= 10.34) makes a subject containing invalid
// UTF-8 matchable: PCRE2 treats each bad byte as an unmatchable barrier instead
// of failing the whole call. Without it every pcre2_match against such a line
// returns PCRE2_ERROR_UTF8_ERR*, which the search helpers below read as "no
// match" — so one stray byte (a Latin-1 source file, a binary-ish fixture, a
// mis-encoded comment) silently dropped ALL syntax highlighting for that line.
// The text reader only rejects embedded NULs, so such buffers do reach here.
#ifdef PCRE2_MATCH_INVALID_UTF
constexpr uint32_t kRegexCompileOptions = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
#else
constexpr uint32_t kRegexCompileOptions = PCRE2_UTF | PCRE2_UCP;
#endif

// Byte-oriented twin of the options above, used for lines that contain no byte
// >= 0x80.
//
// PCRE2_UTF and PCRE2_UCP only change behaviour at code points >= 0x80: UTF
// makes a multi-byte sequence one character, UCP makes `\b`, `\w`, `\d`, `\s`
// and the POSIX classes Unicode-aware. On an all-ASCII subject neither can
// observe a difference, so the two compilations of one pattern produce
// identical match sets -- verified across every built-in rule regex (1906
// distinct literals) over 20k ASCII fixture lines, zero divergences.
//
// They are not equally fast. UCP alone costs ~50% of the highlight path and UTF
// another ~17%, because both push the matcher off its byte-oriented fast paths.
// Real source files are overwhelmingly ASCII, so each rule regex is compiled
// twice and the line picks: byte-mode when the line is ASCII, the Unicode one
// when it is not, which keeps `\b` correct around accented identifiers instead
// of trading that away for the speed.
constexpr uint32_t kAsciiRegexCompileOptions = 0;

// A signature is an opening marker -- a shebang, an XML declaration, a modeline --
// so it lives in the first bytes of a head line. Nothing in the registry looks for
// one a kilobyte in, and a match past this cap is deliberately out of scope.
//
// The line-count bound alone does NOT bound the read: "64 head lines" is 64
// *short* lines only in a file with line breaks. A minified bundle or a
// newline-free JSON blob is ONE line of many megabytes, so an unbounded read would
// touch the whole document -- per call, on the shell thread, on every keystroke
// (DetectState runs from the highlight path). The byte cap is the bound that
// actually holds (TD-2026-08-05-133).
constexpr std::size_t kSignatureDetectLineBytes = 4096;

struct MatchRange {
  std::size_t start = 0;
  std::size_t end = 0;
};

struct RegionStartMatch {
  std::uint32_t region_id = 0;
  MatchRange match;
};

using CompiledRegex = util::CompiledRegex;
// `at_line_start` is false when `text` is a mid-line segment (e.g. the tail after
// a region on the same line). A `^`-anchored rule must NOT treat such a segment's
// position 0 as a line start, so PCRE2_NOTBOL is passed. Patterns without `^` are
// unaffected (NOTBOL only changes the circumflex assertion).
void FindAllRegex(std::string_view text,
                  const CompiledRegex& pattern,
                  std::size_t match_slot,
                  std::vector<MatchRange>& matches,
                  bool at_line_start = true);

// Per-thread cache of pcre2 match-data blocks, one slot per compiled regex in
// the registry.
//
// The highlighter runs one regex per pattern rule per line -- ~40 executions on
// a C++ line -- and each needs its pattern-shaped match data. This used to be an
// `unordered_map` keyed on the CompiledRegex address, so every one of those
// executions paid a hash and a probe. Every regex the hot path can reach already
// lives at a known index in `registry.rules`, so the slot is derivable
// arithmetically and the lookup becomes an array read.
//
// Slot layout (see MatchSlot* below): rules occupy `rules.size() * kSlotsPerRule`
// slots, definitions the block after them. The vector is sized from the live
// registry and dropped whenever the registry revision advances: a syntax reload
// destroys the old CompiledRegex objects, and a match-data block sized for the
// old pattern must never be reused for the new one at the same index
// (TD-2026-07-17A-115 -- the same hazard the address-keyed map had).
enum : std::size_t {
  // Four regexes per rule, each with a Unicode and an ASCII compilation (see
  // kAsciiRegexCompileOptions); the ASCII variants sit at +kRuleSlotAscii.
  kSlotsPerRule = 8,
  kRuleSlotPattern = 0,
  kRuleSlotStart = 1,
  kRuleSlotEnd = 2,
  kRuleSlotSkip = 3,
  kRuleSlotAscii = 4,
  kSlotsPerDefinition = 3,
  kDefinitionSlotFilename = 0,
  kDefinitionSlotHeader = 1,
  kDefinitionSlotSignature = 2,
};

constexpr std::size_t kNoMatchSlot = static_cast<std::size_t>(-1);

std::size_t RuleMatchSlot(std::size_t rule_index, std::size_t kind) {
  return rule_index * kSlotsPerRule + kind;
}

util::RegexMatchData& ReusableMatchData(const CompiledRegex& pattern, std::size_t slot) {
  thread_local std::vector<util::RegexMatchData> match_data_slots;
  thread_local std::size_t cached_revision = 0;
  // Callers outside the registry's own tables (and any future one that forgets a
  // slot) share this scratch block; it is re-created per call only when the
  // previous holder was a different pattern, which never happens on a hot path.
  thread_local util::RegexMatchData unslotted;
  const std::size_t revision = RegistryRevision();
  if (revision != cached_revision) {
    match_data_slots.clear();
    unslotted = util::RegexMatchData();
    cached_revision = revision;
  }
  if (slot == kNoMatchSlot) {
    unslotted = pattern.CreateMatchData();
    return unslotted;
  }
  if (slot >= match_data_slots.size()) {
    match_data_slots.resize(slot + 1);
  }
  util::RegexMatchData& data = match_data_slots[slot];
  if (!data.valid()) {
    data = pattern.CreateMatchData();
  }
  return data;
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

// The byte-mode twin (see kAsciiRegexCompileOptions). Compiled without an error
// prefix and never validated: a pattern that only compiles under UTF (e.g. one
// naming a code point above U+00FF) simply has no ASCII fast path, and every
// line falls back to the Unicode compilation. A failure here is not a user error.
CompiledRegex CompileAsciiSyntaxRegex(std::string_view pattern) {
  return CompiledRegex(NormalizePattern(pattern), kAsciiRegexCompileOptions);
}

// Picks the compilation to run against this segment and the match-data slot that
// goes with it. `ascii_subject` is decided once per line: every segment handed to
// a rule is a slice of that line, and a slice of an ASCII string is ASCII.
struct RuleRegexChoice {
  const CompiledRegex* regex = nullptr;
  std::size_t slot = 0;
};

RuleRegexChoice ChooseRuleRegex(const CompiledRegex& unicode,
                                const CompiledRegex& ascii,
                                std::size_t rule_index,
                                std::size_t kind,
                                bool ascii_subject) {
  if (ascii_subject && ascii.valid()) {
    return {&ascii, RuleMatchSlot(rule_index, kind + kRuleSlotAscii)};
  }
  return {&unicode, RuleMatchSlot(rule_index, kind)};
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

// `match_slot` / `skip_slot` index the per-thread match-data arena (see
// ReusableMatchData); pass kNoMatchSlot for a regex that does not live in the
// registry's rule/definition tables.
std::optional<MatchRange> FindFirstRegex(std::string_view text,
                                         const CompiledRegex& pattern,
                                         std::size_t match_slot,
                                         const CompiledRegex* skip = nullptr,
                                         std::size_t skip_slot = kNoMatchSlot,
                                         bool allow_zero_length = false,
                                         bool at_line_start = true) {
  if (!pattern.valid()) {
    return std::nullopt;
  }
  if (skip != nullptr && skip->valid()) {
    thread_local std::vector<MatchRange> skip_matches;
    FindAllRegex(text, *skip, skip_slot, skip_matches, at_line_start);
    if (skip_matches.empty()) {
      // No skip regions on this segment → masking would be the identity
      // (masked_buf == text). Search the raw text directly, skipping the O(n)
      // buffer copy and the second pattern pass. This is the common case on the
      // highlight hot path: the region's tail is re-searched at every cursor step
      // (each nested-region open/close), but most steps span text with no escape
      // to mask. Provably identical to the masked path below when there is nothing
      // to mask. (This does not change the O(n²) worst case a region with many
      // nested children still re-scans the shrinking tail but it removes the copy
      // and halves the pattern scans on the dominant escape-free segments.)
      return FindFirstRegex(text, pattern, match_slot, nullptr, kNoMatchSlot, allow_zero_length,
                            at_line_start);
    }
    thread_local std::string masked_buf;
    masked_buf.assign(text);
    for (const MatchRange match : skip_matches) {
      std::fill(masked_buf.begin() + static_cast<std::ptrdiff_t>(match.start),
                masked_buf.begin() + static_cast<std::ptrdiff_t>(match.end), '\0');
    }
    return FindFirstRegex(masked_buf, pattern, match_slot, nullptr, kNoMatchSlot, allow_zero_length,
                          at_line_start);
  }

  auto& match_data = ReusableMatchData(pattern, match_slot);
  if (!match_data.valid()) {
    return std::nullopt;
  }

  const std::uint32_t match_options = at_line_start ? 0u : PCRE2_NOTBOL;
  const int rc = pattern.Match(text, 0, match_data, match_options);
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

// Per-call match budget for a single rule on a single line. Lines are allowed up
// to ~100 KiB, so a rule that matches single characters could otherwise push
// ~100k matches, and a definition with many such rules multiplies that on the
// highlight hot path. Beyond this the line is left partially highlighted —
// acceptable degradation for a pathological pattern, and far above any real
// token count on a visible line.
constexpr std::size_t kMaxMatchesPerRulePerLine = 8192;

void FindAllRegex(std::string_view text,
                  const CompiledRegex& pattern,
                  std::size_t match_slot,
                  std::vector<MatchRange>& matches,
                  bool at_line_start) {
  matches.clear();
  if (!pattern.valid() || text.empty()) {
    return;
  }

  auto& match_data = ReusableMatchData(pattern, match_slot);
  if (!match_data.valid()) {
    return;
  }

  // NOTBOL only matters for the first attempt (offset 0): a `^` can only assert at
  // subject start, so at offset>0 it is inert. Pass it whenever the segment does
  // not begin at a true line start so `^` does not match a mid-line segment head.
  const std::uint32_t match_options = at_line_start ? 0u : PCRE2_NOTBOL;
  for (std::size_t offset = 0;
       offset <= text.size() && matches.size() < kMaxMatchesPerRulePerLine;) {
    const int rc = pattern.Match(text, offset, match_data, match_options);
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

bool RegexMatches(std::string_view text,
                  const CompiledRegex& pattern,
                  std::size_t match_slot = kNoMatchSlot) {
  return FindFirstRegex(text, pattern, match_slot).has_value();
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
  // Compiled regexes. For lazily-built definitions (built-in generated rules)
  // these start empty and are populated on first highlight of the owning
  // definition (see EnsureDefinitionCompiled) from the *_src back-pointers
  // below; `mutable` so the compile can happen through the const registry
  // reference returned by GetRegistry(). Eager (plugin/runtime) rules populate
  // these at build time and leave the *_src pointers null.
  mutable CompiledRegex pattern;
  mutable CompiledRegex start;
  mutable CompiledRegex end;
  mutable CompiledRegex skip;
  // Byte-mode twins used for all-ASCII lines (see kAsciiRegexCompileOptions).
  // Empty when the pattern only compiles under UTF; the Unicode one then serves
  // every line.
  mutable CompiledRegex pattern_ascii;
  mutable CompiledRegex start_ascii;
  mutable CompiledRegex end_ascii;
  mutable CompiledRegex skip_ascii;
  // Non-owning back-pointers into the static kGeneratedRules string literals,
  // set only for built-in lazily-compiled rules. Stable for the process
  // lifetime; nullptr for eager runtime rules.
  const char* pattern_src = nullptr;
  const char* start_src = nullptr;
  const char* end_src = nullptr;
  const char* skip_src = nullptr;
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
  // True for built-in generated definitions whose rule regexes are compiled
  // lazily via the owning registry's definition_compile_flags entry. Plugin /
  // runtime definitions stay eager (false) so their regexes are validated at
  // reload time.
  bool lazy_rules = false;
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> pattern_rule_indices_by_parent;
  std::unordered_map<std::uint32_t, std::vector<std::size_t>> region_rule_indices_by_parent;
};

struct Registry {
  std::vector<Definition> definitions;
  std::vector<Rule> rules;
  std::uint32_t default_definition_id = 0;
  // One guard per definition (indexed by definition_id - 1), sized in
  // PartitionDefinitionRules. std::once_flag is non-movable/non-copyable, so it
  // cannot live inside Definition (which is moved and copied by value during
  // registry construction); a vector<once_flag> is movable (buffer steal) but
  // not copyable, which makes Registry move-only. `mutable` so EnsureDefinition-
  // Compiled can std::call_once through a const registry reference.
  mutable std::vector<std::once_flag> definition_compile_flags;
};

// Match-data slot for a definition-level regex (filename / header / signature),
// in the block after the rules' slots. Detection scans every definition's
// filename regex per file open, so these need the same reuse the rules get --
// without a slot each execution would allocate its own match-data block.
std::size_t DefinitionMatchSlot(const Registry& registry,
                                std::size_t definition_index,
                                std::size_t kind) {
  return registry.rules.size() * kSlotsPerRule + definition_index * kSlotsPerDefinition + kind;
}

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
  // Each pattern emits "(?:" + p + ")" (p.size() + 4), plus one '|' between every
  // adjacent pair (patterns.size() - 1 separators). The old reserve folded the '|'
  // into the per-pattern count, leaving it short by one separator per join and
  // forcing a reallocation on the final append.
  std::size_t total = patterns.size() - 1;  // '|' separators (patterns non-empty)
  for (const auto& p : patterns) {
    total += p.size() + 4;  // "(?:" + p + ")"
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
    rule.pattern_ascii = CompileAsciiSyntaxRegex(runtime_rule.pattern);
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
    rule.start_ascii = CompileAsciiSyntaxRegex(runtime_rule.start_regex);
    rule.end_ascii = CompileAsciiSyntaxRegex(runtime_rule.end_regex);
    if (!runtime_rule.skip_regex.empty()) {
      rule.skip_ascii = CompileAsciiSyntaxRegex(runtime_rule.skip_regex);
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
    // Defer regex compilation: retain the source pointers and compile them the
    // first time the owning definition is highlighted (EnsureDefinitionCompiled).
    // These literals live in static storage for the whole process lifetime.
    rule.pattern_src = generated.pattern;
    rule.start_src = generated.start_regex;
    rule.end_src = generated.end_regex;
    rule.skip_src = generated.skip_regex;
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
    // Rule regexes for built-in definitions are compiled lazily; detection
    // regexes above stay eager so DetectFiletype/DetectState never pay the
    // rule-compile cost.
    definition.lazy_rules = true;
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
  // Fresh, per-registry lazy-compile guards. Whole-vector move-assign (never
  // resize()) because vector<once_flag> cannot relocate its elements. Runs at
  // the end of every build/reload, so both BuiltInRegistry() and each reloaded
  // MutableRegistry() get an independent set of flags sized to their definitions.
  registry.definition_compile_flags = std::vector<std::once_flag>(registry.definitions.size());
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
  // No plugin definitions: return an EMPTY registry and let GetRegistry() alias
  // BuiltInRegistry() — which is exactly what it does when MutableRegistry() is
  // empty, and what the "avoiding a multi-MB copy" comment on MutableRegistry()
  // has always claimed happens.
  //
  // It did not. This branch used to clone the built-in tables element-wise into
  // `output.registry`, and the caller then moved that clone INTO
  // MutableRegistry() — which is no longer empty, so the alias never applied.
  // Every reload therefore copied ~3,600 rules and rebuilt both per-definition
  // rule-index maps, on the shell thread, to produce a registry identical to the
  // one already sitting in a function-local static: 1.7 ms at launch and again on
  // any project switch whose syntax fingerprint moved, for nothing.
  //
  // Aliasing also KEEPS the lazily compiled rule regexes. A clone copies each
  // Rule's `mutable CompiledRegex` but gets a fresh once_flag table from
  // PartitionDefinitionRules, so the compiled state was reachable but the guards
  // said "not compiled yet" — every filetype recompiled its rules on the first
  // line highlighted after a reload.
  if (runtime_definitions.empty()) {
    return output;
  }
  // With plugin definitions present the clone is unavoidable: their rules are
  // prepended, so every built-in rule/definition index shifts. Registry is
  // move-only (its per-definition once_flag guards cannot be copied), so clone
  // element-wise via AppendRegistryWithOffset rather than copy-assigning it. The
  // trailing PartitionDefinitionRules gives the clone its own guard table.

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

// Atomic so the syntax-highlight cache-invalidation read (ReusableMatchData, which
// can run on the background prefetch worker) is well-defined against the main
// thread's post-reload increment.
std::atomic<std::size_t>& MutableRegistryRevision() {
  static std::atomic<std::size_t> revision{1};
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

// Compile a lazily-built (built-in) definition's rule regexes on first use.
// std::call_once serializes concurrent callers (background prefetch worker vs.
// the main render cache-miss path) and publishes the compiled regexes with a
// happens-before edge, independent of RegistryMutex. A no-op for eager
// (plugin/runtime) definitions, which compiled their regexes at reload time.
void EnsureDefinitionCompiled(const Registry& registry, std::uint32_t definition_id) {
  if (definition_id == 0 || definition_id > registry.definitions.size()) {
    return;
  }
  const Definition& definition = registry.definitions[definition_id - 1];
  if (!definition.lazy_rules ||
      definition_id > registry.definition_compile_flags.size()) {
    return;
  }
  std::call_once(registry.definition_compile_flags[definition_id - 1], [&]() {
    // Scoped: this is a one-time per-filetype cost paid inside the first frame
    // that highlights a line of it, i.e. on the file-open path, and it doubled
    // when the ASCII twins were added. Without a scope it hid inside
    // HighlightLine's self time on exactly the frame a user waits for.
    util::PerformanceTrace::Scope compile_scope("RuntimeSyntaxRegistry::CompileDefinitionRules");
    const std::size_t end_rule = definition.first_rule + definition.rule_count;
    for (std::size_t index = definition.first_rule;
         index < end_rule && index < registry.rules.size(); ++index) {
      const Rule& rule = registry.rules[index];
      if (rule.pattern_src != nullptr) {
        rule.pattern = CompileSyntaxRegex(rule.pattern_src);
        rule.pattern_ascii = CompileAsciiSyntaxRegex(rule.pattern_src);
      }
      if (rule.start_src != nullptr) {
        rule.start = CompileSyntaxRegex(rule.start_src);
        rule.start_ascii = CompileAsciiSyntaxRegex(rule.start_src);
      }
      if (rule.end_src != nullptr) {
        rule.end = CompileSyntaxRegex(rule.end_src);
        rule.end_ascii = CompileAsciiSyntaxRegex(rule.end_src);
      }
      if (rule.skip_src != nullptr) {
        rule.skip = CompileSyntaxRegex(rule.skip_src);
        rule.skip_ascii = CompileAsciiSyntaxRegex(rule.skip_src);
      }
    }
  });
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
                       SyntaxTokenKind base_kind,
                       bool ascii_subject) {
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

    const RuleRegexChoice pattern =
        ChooseRuleRegex(rule.pattern, rule.pattern_ascii, index, kRuleSlotPattern, ascii_subject);
    FindAllRegex(segment, *pattern.regex, pattern.slot, matches,
                 /*at_line_start=*/absolute_offset == 0);
    for (const MatchRange match : matches) {
      MarkRange(tokens, absolute_offset + match.start, absolute_offset + match.end, rule.group);
    }
  }
}

std::optional<RegionStartMatch> FindEarliestRegionStart(const Registry& registry,
                                                        const Definition& definition,
                                                        std::uint32_t parent_region_id,
                                                        std::string_view segment,
                                                        std::size_t search_limit,
                                                        bool at_line_start,
                                                        bool ascii_subject) {
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

    const RuleRegexChoice start =
        ChooseRuleRegex(rule.start, rule.start_ascii, index, kRuleSlotStart, ascii_subject);
    const RuleRegexChoice skip =
        ChooseRuleRegex(rule.skip, rule.skip_ascii, index, kRuleSlotSkip, ascii_subject);
    const std::optional<MatchRange> match = FindFirstRegex(
        segment, *start.regex, start.slot, skip.regex->valid() ? skip.regex : nullptr, skip.slot,
        /*allow_zero_length=*/false, at_line_start);
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

// Highlights one line, maintaining an explicit stack of open multi-line
// regions instead of relying on C++ recursion. This unifies the old
// HighlightTopLevel/HighlightRegion twins and — crucially — carries the full
// region nesting across line boundaries: when an inner region closes mid-line
// control returns to its real parent rather than to the top level. `end_state`
// receives the definition id plus the region stack still open at end-of-line.
void HighlightLineScoped(const Registry& registry,
                         std::uint32_t definition_id,
                         const std::uint32_t* initial_stack,
                         std::uint8_t initial_depth,
                         std::string_view line,
                         std::vector<SyntaxTokenKind>& tokens,
                         SyntaxState* end_state,
                         bool want_tokens) {
  const Definition* definition = DefinitionById(registry, definition_id);
  // Compile this definition's rule regexes (if lazily built) before any of the
  // rule.pattern/start/end.valid() checks below — an uncompiled rule would
  // otherwise be silently skipped. Covers the definition's whole rule range,
  // including its nested region children.
  EnsureDefinitionCompiled(registry, definition_id);

  // Decided once for the whole line and passed to every rule: each rule's
  // subject is a slice of `line`, and a slice of an all-ASCII string is
  // all-ASCII, so one scan settles it for the ~40 executions that follow.
  const bool ascii_line = util::IsAllAscii(line);

  std::uint32_t stack[SyntaxState::kMaxRegionDepth] = {};
  std::size_t depth = std::min<std::size_t>(initial_depth, SyntaxState::kMaxRegionDepth);
  for (std::size_t i = 0; i < depth; ++i) {
    stack[i] = initial_stack[i];
  }

  const auto write_state = [&]() {
    if (end_state == nullptr) {
      return;
    }
    *end_state = SyntaxState{};
    end_state->definition_id = definition_id;
    end_state->region_depth = static_cast<std::uint8_t>(depth);
    for (std::size_t i = 0; i < depth; ++i) {
      end_state->region_stack[i] = stack[i];
    }
  };

  std::size_t cursor = 0;
  while (cursor <= line.size()) {
    const std::uint32_t region_id = depth == 0 ? 0u : stack[depth - 1];
    const Rule* region = region_id == 0 ? nullptr : RuleByRegionId(registry, region_id);
    if (region_id != 0 && region == nullptr) {
      // Dangling region id (e.g. after a definition reload): drop it and resume
      // in the parent scope rather than spinning.
      --depth;
      continue;
    }
    if (region_id == 0 && cursor >= line.size()) {
      break;
    }

    const std::string_view tail = line.substr(cursor);
    std::optional<MatchRange> end_match;
    if (region != nullptr) {
      // region_id is the rule index plus one (see RuleByRegionId).
      const std::size_t region_rule_index = region_id - 1;
      const RuleRegexChoice region_end = ChooseRuleRegex(region->end, region->end_ascii,
                                                        region_rule_index, kRuleSlotEnd, ascii_line);
      const RuleRegexChoice region_skip = ChooseRuleRegex(
          region->skip, region->skip_ascii, region_rule_index, kRuleSlotSkip, ascii_line);
      end_match = FindFirstRegex(tail, *region_end.regex, region_end.slot,
                                 region_skip.regex->valid() ? region_skip.regex : nullptr,
                                 region_skip.slot,
                                 /*allow_zero_length=*/true, /*at_line_start=*/cursor == 0);
    }
    const bool closes_immediately = end_match.has_value() && end_match->start == 0;
    const std::size_t search_limit =
        region != nullptr && end_match.has_value() ? end_match->start : tail.size();
    const std::optional<RegionStartMatch> next_region =
        (closes_immediately || definition == nullptr)
            ? std::optional<RegionStartMatch>{}
            : FindEarliestRegionStart(registry, *definition, region_id, tail, search_limit,
                                      /*at_line_start=*/cursor == 0, ascii_line);
    const std::size_t segment_end =
        next_region.has_value() ? next_region->match.start : search_limit;

    // Pattern-rule application only writes tokens (it never affects region/depth
    // state — see FindEarliestRegionStart / end_match for that), so in state-only
    // mode (AdvanceState replaying to establish resume-state) skip it entirely.
    // Otherwise every pattern regex would run over the whole segment and MarkRange
    // its results into an empty vector — a pure no-op that doubled regex work on
    // the visible screen's leading lines and wasted it outright for the whole
    // [checkpoint .. viewport-top] replay prefix (up to 512 lines synchronously).
    if (want_tokens) {
      const SyntaxTokenKind base_kind =
          region != nullptr ? region->group : SyntaxTokenKind::Plain;
      if (definition != nullptr) {
        ApplyPatternRules(registry, *definition, region_id, tail.substr(0, segment_end), cursor,
                          tokens, base_kind, ascii_line);
      } else if (region != nullptr && region->group != SyntaxTokenKind::Plain) {
        MarkRange(tokens, cursor, cursor + segment_end, region->group);
      }
    }

    if (next_region.has_value()) {
      const Rule* nested_region = RuleByRegionId(registry, next_region->region_id);
      if (nested_region == nullptr) {
        break;
      }
      MarkRange(tokens, cursor + next_region->match.start, cursor + next_region->match.end,
                nested_region->limit_group);
      const std::size_t advanced = cursor + next_region->match.end;
      if (depth < SyntaxState::kMaxRegionDepth) {
        stack[depth++] = next_region->region_id;
      } else if (advanced == cursor) {
        // Depth saturated and no forward progress: stop to avoid spinning.
        break;
      }
      cursor = advanced;
      continue;
    }

    if (region != nullptr && end_match.has_value()) {
      MarkRange(tokens, cursor + end_match->start, cursor + end_match->end, region->limit_group);
      cursor += end_match->end;
      --depth;  // region closed: resume in the parent scope
      continue;
    }

    // No nested start and no closing match on this line: the current scope
    // continues onto the next line.
    write_state();
    return;
  }

  write_state();
}

// `lines`, when present, is the document itself -- NOT a materialized head. The
// signature scan is reached only when a filename match is ambiguous (two or more
// candidate definitions), which for an ordinary `.cpp`/`.py`/`.rs` path is never;
// materializing 64 head strings up front paid ~65 allocations per call for a scan
// that usually does not run. Detection runs once per content revision, i.e. per
// keystroke, and this was 25 % of the allocations in an edit burst
// (TD-2026-08-06-159). Read the head through LineWindow, one bounded line at a
// time into a reused scratch buffer, at the point the scan actually needs it.
std::uint32_t DetectDefinitionId(const Registry& registry,
                                 const std::filesystem::path& path,
                                 const LineSpan* lines,
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
    // One buffer for the whole scan. LineWindow only writes into it when the
    // source cannot serve the window contiguously (an edited piece-tree line), so
    // on an unedited buffer this stays at zero allocations for any line count.
    std::string window;
    for (const std::uint32_t definition_id : matches) {
      const Definition* definition = DefinitionById(registry, definition_id);
      if (definition == nullptr || !definition->signature_regex.valid()) {
        continue;
      }
      const std::size_t signature_slot =
          DefinitionMatchSlot(registry, definition_id - 1, kDefinitionSlotSignature);
      for (std::size_t i = 0; i < line_limit; ++i) {
        const std::string_view head_line =
            lines->LineWindow(i, 0, kSignatureDetectLineBytes, window);
        if (RegexMatches(head_line, definition->signature_regex, signature_slot)) {
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
      if (RegexMatches(path_text, definition->filename_regex,
                       DefinitionMatchSlot(registry, definition_id - 1, kDefinitionSlotFilename))) {
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
  // Same bound the signature scan uses, and for the same reason: a header regex
  // looks at an opening marker, and the "first line" of a minified bundle is the
  // whole file. `header_window` must outlive `header_line`, which may view into it.
  std::string header_window;
  const std::string_view header_line =
      lines != nullptr && !lines->empty()
          ? lines->LineWindow(0, 0, kSignatureDetectLineBytes, header_window)
          : first_line;

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

      if (definition.filename_regex.valid() &&
          RegexMatches(path_text, definition.filename_regex,
                       DefinitionMatchSlot(registry, i, kDefinitionSlotFilename))) {
        filename_matches.push_back(definition_id);
        continue;
      }
      if (filename_matches.empty() && definition.header_regex.valid() &&
          RegexMatches(header_line, definition.header_regex,
                       DefinitionMatchSlot(registry, i, kDefinitionSlotHeader))) {
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

// Guards registry reassignment in ReloadDefinitions against concurrent reads by
// the background highlight worker. Main-thread readers (render/compare paths)
// never run concurrently with a reload — both are on the main thread — so they
// read lock-free; the worker takes a shared lock for the duration of a batch.
// static: TU-local, so the symbol does not leak into the link namespace where an
// identically-named helper elsewhere would be an ODR hazard.
static std::shared_mutex& RegistryMutex() {
  static std::shared_mutex mutex;
  return mutex;
}

RuntimeSyntaxReloadResult ReloadDefinitions(
    const std::vector<RuntimeSyntaxDefinitionData>& definitions,
    std::vector<std::string>* errors) {
  std::vector<std::string> local_errors;
  std::size_t loaded_runtime_definition_count = 0;
  {
    // Hold the exclusive lock across the *build*, not just the swap. BuildRegistry
    // clones BuiltInRegistry element-wise (AppendRegistryWithOffset copies each
    // Rule, including its lazily-compiled `mutable CompiledRegex` fields and the
    // per-definition once_flag guards). Those fields are written in place by
    // EnsureDefinitionCompiled while a background highlight worker holds only a
    // *shared* lock, so cloning them from the reload thread without exclusion would
    // race the compile. Taking the unique lock first drains all shared readers and
    // gives the clone a happens-before edge to any regexes they compiled.
    std::unique_lock<std::shared_mutex> lock(RegistryMutex());
    BuildOutput build = BuildRegistry(definitions, &local_errors);
    loaded_runtime_definition_count = build.loaded_runtime_definition_count;
    MutableRegistry() = std::move(build.registry);
    ++MutableRegistryRevision();
  }
  if (errors != nullptr) {
    *errors = local_errors;
  }
  return RuntimeSyntaxReloadResult{
      .built_in_definition_count = kGeneratedDefinitionCount,
      .plugin_definition_count = loaded_runtime_definition_count,
      .error_count = local_errors.size(),
  };
}

namespace {

// The optional warm thread started by WarmInBackground(). A plain handle rather
// than an atomic or a mutex-guarded one: both entry points are documented
// main-thread-only, and the actual cross-thread synchronization is the magic
// static inside BuiltInRegistry() — a main-thread EnsureInitialized() racing the
// warm thread blocks on that guard and gets the same object either way.
//
// The holder exists for the join: ~std::thread on a still-joinable handle calls
// std::terminate, and a startup that fails before WorkspaceShell::Initialize
// (SDL_Init, window or renderer creation) returns from main without ever
// reaching EnsureInitialized. Joining here makes that path an ordinary exit.
struct WarmThreadHolder {
  std::thread thread;
  ~WarmThreadHolder() {
    if (thread.joinable()) {
      thread.join();
    }
  }
};

std::thread& WarmThread() {
  static WarmThreadHolder holder;
  return holder.thread;
}

}  // namespace

void WarmInBackground() {
  std::thread& thread = WarmThread();
  if (thread.joinable()) {
    return;  // already warming (or warmed and not yet joined)
  }
  thread = std::thread([]() {
    util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::WarmInBackground");
    (void)BuiltInRegistry();
  });
}

void EnsureInitialized() {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::EnsureInitialized");
  // Join first: with a warm in flight this is where the main thread waits for
  // whatever is left of it, and the scope above is what makes that wait visible
  // instead of it looking like the build got cheaper.
  if (std::thread& thread = WarmThread(); thread.joinable()) {
    thread.join();
  }
  // Touch BuiltInRegistry() directly so the static magic-init runs here
  // even when MutableRegistry() is empty (the lazy-alias case).
  (void)BuiltInRegistry();
}

std::size_t RegistryRevision() {
  return MutableRegistryRevision().load(std::memory_order_acquire);
}

SyntaxState DetectState(const std::filesystem::path& path, LineSpan lines) {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::DetectState");
  const Registry& registry = GetRegistry();
  // The span is handed straight through: DetectDefinitionId reads a bounded head
  // from it only if a filename match turns out to be ambiguous.
  return SyntaxState{
      .definition_id = DetectDefinitionId(registry, path, &lines, {}),
  };
}

SyntaxState DetectState(const std::filesystem::path& path, std::string_view text) {
  // Bounded head only, as views into `text`. The alternative — splitting the whole
  // blob into owned lines at the call site — was one allocation per line of the
  // document to feed a scan that reads sixty-four of them.
  const std::vector<std::string_view> head =
      util::SplitLineViews(text, kSignatureDetectLineLimit);
  return DetectState(path, LineSpan(head));
}

std::string DetectFiletype(const std::filesystem::path& path, LineSpan lines) {
  const Registry& registry = GetRegistry();
  const std::uint32_t definition_id = DetectDefinitionId(registry, path, &lines, {});
  const Definition* definition = DefinitionById(registry, definition_id);
  return definition == nullptr ? std::string{} : definition->filetype;
}

std::string DetectFiletype(const std::filesystem::path& path) {
  const Registry& registry = GetRegistry();
  const std::uint32_t definition_id = DetectDefinitionId(registry, path, nullptr, {});
  const Definition* definition = DefinitionById(registry, definition_id);
  return definition == nullptr ? std::string{} : definition->filetype;
}

void CompileDefinition(std::uint32_t definition_id) {
  // Idempotent (std::call_once) and bounds-checked inside EnsureDefinitionCompiled;
  // a no-op for id 0, an out-of-range id, or an eager/already-compiled definition.
  EnsureDefinitionCompiled(GetRegistry(), definition_id);
}

HighlightedLine HighlightLine(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::string_view first_line) {
  HighlightedLine result;
  result.end_state = HighlightLineInto(line, path, state, &result.tokens, first_line);
  return result;
}

SyntaxState HighlightLineInto(std::string_view line,
                              const std::filesystem::path& path,
                              const SyntaxState& state,
                              std::vector<SyntaxTokenKind>* tokens,
                              std::string_view first_line) {
  util::PerformanceTrace::Scope perf_scope("RuntimeSyntaxRegistry::HighlightLine");
  const Registry& registry = GetRegistry();

  const std::uint32_t definition_id =
      state.definition_id != 0 ? state.definition_id
                               : DetectDefinitionId(registry, path, nullptr, first_line);

  SyntaxState end_state{};
  end_state.definition_id = definition_id;
  if (line.empty() || line.size() > kMaxHighlightLineBytes) {
    tokens->clear();
    // Leave `tokens` EMPTY rather than filling `line.size()` Plain entries. Every
    // consumer already treats a short/absent token vector as Plain at that column
    // (DecoratedTextGridRenderer, BracketScanner, FoldingModel, the auto-pair
    // language behaviour all bounds-check), and the disabled-highlighting path in
    // TextViewport::HighlightedLineTokens has always returned an empty vector for
    // exactly that reason — so this is the same rendering, without the buffer.
    // Filling it made the refusal to tokenize cost what tokenizing would have: a
    // minified 8 MB single-line file allocated and memset 8 MB per token-cache
    // miss, ~1.6 ms of a ~15 ms keystroke, and cached it per line.
    // Carry the incoming open-region stack forward instead of dropping it. An
    // empty line contains no delimiter, and a line too long to scan is far more
    // likely to sit inside a region than to open/close one — resetting to top
    // level here would resume the rest of the file as code, so e.g. a blank line
    // inside a block comment / multi-line string mis-highlights everything after.
    if (state.definition_id == definition_id && state.region_depth > 0) {
      end_state = state;  // definition_id already matches
    }
    return end_state;  // empty, or too long to tokenize affordably: leave Plain
  }

  // Past the guard: the scoped highlighter writes through MarkRange, which only
  // overwrites existing entries, so the vector must be pre-sized to the line here.
  // `assign` on a vector the caller already owns reuses its buffer whenever the
  // capacity fits, which is the point of the Into form.
  //
  // Round the capacity up first. The Into form's hot callers recycle buffers
  // through a pool (`SurfaceTokenWindow`, the editor's highlight cache), and a
  // buffer sized EXACTLY to the line it last held fits the next line only when
  // that line is no longer — so with exact sizing a pool of buffers reallocates
  // on nearly every reuse. It was 81 % of `diff.next_hunk_burst`. Rounding to a
  // power of two collapses the pool onto a handful of sizes, at a bounded ≤2x
  // capacity cost on a now-one-byte element (TD-2026-08-17-261's sweep).
  if (tokens->capacity() < line.size()) {
    tokens->reserve(std::bit_ceil(std::max<std::size_t>(line.size(), 64)));
  }
  tokens->assign(line.size(), SyntaxTokenKind::Plain);

  // Resume the previous line's open-region stack only when the definition still
  // matches; otherwise start fresh at the top level.
  const bool resume = state.definition_id == definition_id && state.region_depth > 0;
  HighlightLineScoped(registry, definition_id, resume ? state.region_stack : nullptr,
                      resume ? state.region_depth : 0, line, *tokens, &end_state,
                      /*want_tokens=*/true);
  return end_state;
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
  SyntaxState end_state{};
  end_state.definition_id = definition_id;
  if (line.empty() || line.size() > kMaxHighlightLineBytes) {
    // Carry the incoming open-region stack forward (see HighlightLine): a blank or
    // over-long line must not reset a multi-line region to top level, which would
    // mis-highlight the remainder of the file.
    if (state.definition_id == definition_id && state.region_depth > 0) {
      end_state = state;  // definition_id already matches
    }
    return end_state;  // see kMaxHighlightLineBytes: skip scanning a huge line
  }

  std::vector<SyntaxTokenKind> no_tokens;
  const bool resume = state.definition_id == definition_id && state.region_depth > 0;
  HighlightLineScoped(registry, definition_id, resume ? state.region_stack : nullptr,
                      resume ? state.region_depth : 0, line, no_tokens, &end_state,
                      /*want_tokens=*/false);
  return end_state;
}

}  // namespace microide::editor::runtime_syntax

namespace microide::editor {

// Worker-thread entry point for background highlight prefetch. Holds the
// registry's shared lock for the whole batch so a concurrent plugin reload
// cannot reassign the registry mid-tokenize. Touches no TextViewport state —
// only the request's own immutable snapshot and the shared registry.
HighlightPrefetchResult ComputeHighlightPrefetch(const HighlightPrefetchRequest& request) {
  HighlightPrefetchResult result;
  result.viewport = request.viewport;
  result.content_revision = request.content_revision;
  result.syntax_revision = request.syntax_revision;
  result.start_line = request.start_line;
  result.tokens.reserve(request.lines.size());
  result.end_states.reserve(request.lines.size());

  std::shared_lock<std::shared_mutex> lock(runtime_syntax::RegistryMutex());
  SyntaxState state = request.start_state;
  for (const std::string& line : request.lines) {
    HighlightedLine highlighted = SyntaxHighlighter::HighlightLine(line, request.path, state);
    state = highlighted.end_state;
    result.tokens.push_back(std::move(highlighted.tokens));
    result.end_states.push_back(state);
  }
  return result;
}

HighlightCheckpointResult ComputeHighlightCheckpoints(const HighlightCheckpointRequest& request) {
  HighlightCheckpointResult result;
  result.viewport = request.viewport;
  result.content_revision = request.content_revision;
  result.syntax_revision = request.syntax_revision;
  // `first_line` is a checkpoint boundary (cp_index * interval). The first state
  // we can report is the boundary at first_line + interval, i.e. checkpoint index
  // (first_line / interval) + 1. Guard against a zero interval defensively.
  const std::size_t interval = request.checkpoint_interval == 0 ? 1 : request.checkpoint_interval;
  result.first_checkpoint_index = (request.first_line / interval) + 1;
  result.checkpoint_states.reserve(request.lines.size() / interval + 1);

  std::shared_lock<std::shared_mutex> lock(runtime_syntax::RegistryMutex());
  SyntaxState state = request.start_state;
  for (std::size_t i = 0; i < request.lines.size(); ++i) {
    // AdvanceState updates the resume state without materializing per-line tokens
    // (cheaper than HighlightLine; we only need the checkpoint snapshots).
    state = SyntaxHighlighter::AdvanceState(request.lines[i], request.path, state);
    const std::size_t next_line = request.first_line + i + 1;
    if (next_line % interval == 0) {
      // `state` is now the end-of-line state for next_line - 1, i.e. the state
      // *before* next_line == checkpoints_[next_line / interval].
      result.checkpoint_states.push_back(state);
    }
  }
  return result;
}

}  // namespace microide::editor
