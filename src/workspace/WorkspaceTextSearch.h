#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextBuffer.h"
#include "editor/TextViewport.h"
#include "util/RegexUtil.h"

namespace microide::workspace {

bool StartsWith(std::string_view text, std::string_view prefix);
std::string ToLower(std::string_view text);

std::vector<std::string> SplitSyntaxLines(std::string_view text);
std::string CollapseWhitespace(std::string_view text);

bool QuerySupportsLiteralReplace(std::string_view query);
bool UsesCaseSensitiveLiteralMatch(std::string_view query);
std::size_t ReplaceLiteralMatchesInText(std::string& content,
                                        std::string_view query,
                                        std::string_view replacement,
                                        bool case_sensitive);

// Regex replace-all over an in-memory text blob, mirroring project search's line
// framing exactly: `content` is split on '\n', a trailing '\r' is excluded from
// each line's match window (terminators are preserved verbatim), and every line is
// substituted independently so `^`/`$` and the replaced set match what the
// per-line search reports. `replacement` uses PCRE2 extended syntax ($1 group refs,
// \n/\t escapes, \U/\L case modifiers — see CompiledRegex::SubstituteInto).
//
// Returns the number of replacements, or std::nullopt if a substitution errored
// (an unsupported replacement escape or a match-limit hit) — the caller aborts and
// surfaces a message. `content` is only reassigned when the count is > 0, so a blob
// with no match is left byte-identical (no needless rewrite/mtime churn).
std::optional<std::size_t> ReplaceRegexMatchesInText(std::string& content,
                                                     const util::CompiledRegex& pattern,
                                                     std::string_view replacement);

// The in-file find widget's two option toggles, threaded through every matcher so
// the literal and regex paths cannot disagree about what the user asked for.
// `case_sensitive` false folds both sides; `whole_word` keeps only matches bounded
// by non-word bytes (util::SearchMatchStandsAlone), applied as a filter rather than
// as a query rewrite so literal and regex mean the same thing by it.
struct BufferSearchOptions {
  bool case_sensitive = false;
  bool whole_word = false;

  friend bool operator==(const BufferSearchOptions&, const BufferSearchOptions&) = default;
};

// Whole-buffer regex scan for the in-file find widget (navigation + overview
// ruler). Matches run over the '\n'-joined buffer (NOT per line), so a pattern can
// span line breaks — `\n`, `^`/`$` under PCRE2_MULTILINE, or `foo\nbar` — and a
// returned SelectionRange may span multiple lines. Uses the shared match engine
// (util::FindNextRegexMatchInLine). Caps the retained set at kMaxBufferSearchMatches,
// setting `*truncated` when the cap trims matches.
std::vector<editor::SelectionRange> FindRegexSearchMatches(const editor::TextBuffer& buffer,
                                                           const util::CompiledRegex& pattern,
                                                           BufferSearchOptions options = {},
                                                           bool* truncated = nullptr);

// Splits a (whole-buffer, possibly multi-line) match set into single-line highlight
// fragments the editor renderer can draw per row. A fragment on a line that is NOT
// the match's final line carries `end.column == LineLength(line) + 1` — one past the
// content — which the renderer draws as a newline marker so an otherwise invisible
// `\n`-spanning match is visible at the line end. Preserves the input's ascending
// (line, column) order so the renderer can binary-search each row's slice.
std::vector<editor::SelectionRange> SplitRegexMatchHighlightFragments(
    const editor::TextBuffer& buffer, const std::vector<editor::SelectionRange>& matches);
// TD-2026-07-17A-029: the stored buffer-search match set is scanned in full by the
// editor overview ruler and reassigned on the shell path per query update, so a
// one-character query in a large minified/generated buffer could allocate millions of
// ranges and make every keystroke scale with match count. Cap the retained match set;
// next/previous navigation is unaffected because it re-scans via
// FindNextLiteralMatchAfterSeedWrapOnce rather than reading this vector. When the cap
// trims matches, `*truncated` (when provided) is set.
inline constexpr std::size_t kMaxBufferSearchMatches = 100000;

std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const std::vector<std::string>& lines,
    std::string_view query,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

// Same all-occurrences case-insensitive search, but scanned directly over the
// buffer's zero-copy `LineView` accessor -- no whole-document snapshot vector is
// materialized. This is the find-as-you-type cold path (first keystroke / a query
// that does not extend the previous one).
std::vector<editor::SelectionRange> FindLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

// Find-as-you-type fast path. `previous` must be the complete match set for some
// query that `query` extends (see `QueryExtendsCaseInsensitive`), taken over the
// *current* buffer contents. Because every occurrence of the longer `query` is
// also an occurrence of the shorter prefix, the new match set is a subset of
// `previous`: this returns that subset in O(|previous| * |query|), independent of
// document size. Every kept match is re-validated against the buffer, so a stale
// `previous` can only drop matches, never invent them.
std::vector<editor::SelectionRange> RefineLiteralSearchMatches(
    const editor::TextBuffer& buffer,
    std::string_view query,
    const std::vector<editor::SelectionRange>& previous,
    BufferSearchOptions options = {},
    bool* truncated = nullptr);

// True when `query` equals `prefix` followed by zero or more characters, compared
// case-insensitively (ASCII) -- i.e. `query` is `prefix` with more typed onto the
// end. Gates whether `RefineLiteralSearchMatches` may be used.
bool QueryExtendsCaseInsensitive(std::string_view prefix, std::string_view query);

std::optional<std::size_t> FindLiteralNeedleInLine(std::string_view haystack,
                                                   std::size_t start_from,
                                                   std::string_view needle,
                                                   bool case_sensitive);

// TD-2026-07-17A-031: "Add Cursor at All Matches" scans the whole buffer for
// every occurrence of the seed selection and installs a ranged secondary caret
// at each. Cap the number of installed carets at a product-sized ceiling so a
// dense single-line match set cannot install an unbounded caret vector.
inline constexpr std::size_t kMaxAddCursorAtAllMatches = 10000;

struct AddCursorMatchScan {
  std::vector<editor::SelectionRange> ranges;
  bool truncated = false;
};

// Collect a ranged secondary caret at every literal occurrence of `needle` in
// `buffer`, excluding the seeded selection at (seed_line, seed_column). The
// case-insensitive path folds each line ONCE (and the needle once) instead of
// re-folding the whole line for every match, so a dense single-line match set
// is O(line_bytes) per line rather than O(matches * line_bytes). The scan stops
// after `max_matches` ranges and reports it via `truncated`. Folding is
// length-preserving, so folded byte offsets are valid columns in the original
// line. (TD-2026-07-17A-031.)
AddCursorMatchScan CollectAddCursorMatchRanges(const editor::TextBuffer& buffer,
                                               std::size_t seed_line,
                                               std::size_t seed_column,
                                               std::string_view needle,
                                               bool case_sensitive,
                                               std::size_t max_matches =
                                                   kMaxAddCursorAtAllMatches);

/// Returns the start column of the next occurrence after scanning forward from
/// `(seed_line, seed_end_col)`, then wrapping once from the beginning of the
/// document. On `seed_line` after the wrap, matches before `seed_end_col` count,
/// excluding the seeded span at `(seed_line, seed_start_col)`.
/// Returns `nullopt` when `seed_start_col > seed_end_col` or either column is
/// out of range for the seed line.
std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const std::vector<std::string>& lines,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive);

// Same seed-relative next-match scan over a TextBuffer's zero-copy `LineView`, so
// the Ctrl-D "add cursor at next match" path does not snapshot the whole document
// into a vector<std::string> on every press.
std::optional<editor::TextPosition> FindNextLiteralMatchAfterSeedWrapOnce(
    const editor::TextBuffer& buffer,
    std::size_t seed_line,
    std::size_t seed_start_col,
    std::size_t seed_end_col,
    std::string_view needle,
    bool case_sensitive);

}  // namespace microide::workspace
