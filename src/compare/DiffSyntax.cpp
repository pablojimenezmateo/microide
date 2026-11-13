#include "compare/DiffSyntax.h"

#include <algorithm>
#include <cstdint>
#include <functional>


namespace microide::compare {

const std::shared_ptr<const DiffSyntax>& GenericDiffSyntax() {
  static const std::shared_ptr<const DiffSyntax> kGeneric = [] {
    auto syntax = std::make_shared<DiffSyntax>();
    syntax->delimiters = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
    return std::shared_ptr<const DiffSyntax>(std::move(syntax));
  }();
  return kGeneric;
}

namespace {

bool IsWordByte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte == '$' || byte >= 0x80;
}

bool IsSpaceByte(unsigned char byte) {
  return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\f' || byte == '\v';
}

bool StartsWithAt(std::string_view line, std::size_t column, std::string_view prefix) {
  return !prefix.empty() && line.size() - column >= prefix.size() &&
         line.compare(column, prefix.size(), prefix) == 0;
}

// Lexes consecutive lines into tokens. Multi-line tokens (block comments,
// backtick, triple-quoted and raw strings, here-documents) are only formed when
// the lines are contiguous in memory -- they are whenever they come from one
// buffer, which is every production caller -- because the token's text is one
// view over them.
//
// Two modes over the same recognisers, so they cannot disagree on where a
// multi-line token ends: with a token vector it lexes everything; without one
// (ScanDiffLexState) it only follows what can span lines and steps over the rest
// a byte -- or a word -- at a time.
class Lexer {
 public:
  // `stop_line`: lexing stops before it; the lines from it on are only read to
  // find where a token that started above ends (the scan's look-ahead).
  Lexer(std::span<const std::string_view> lines, const DiffSyntax& syntax,
        std::vector<DiffToken>* tokens, std::vector<std::uint32_t>* delimiter_of,
        std::size_t stop_line)
      : lines_(lines), syntax_(syntax), tokens_(tokens), delimiter_of_(delimiter_of),
        stop_line_(std::min(stop_line, lines.size())) {
    if (tokens_ == nullptr) {
      for (const DiffDelimiterPair& pair : syntax_.block_comments) {
        MarkInteresting(pair.open);
      }
      for (const std::string& prefix : syntax_.line_comments) {
        MarkInteresting(prefix);
      }
      for (const char quote : syntax_.string_quotes) {
        interesting_[static_cast<unsigned char>(quote)] = true;
      }
      if ((syntax_.raw_strings & kDiffHeredoc) != 0) {
        interesting_['<'] = true;
      }
    }
  }

  // Lexes the lines before the stop line, the first from `entry`; the state the
  // stop line starts in.
  DiffLexState Run(const DiffLexState& entry) {
    Position at;
    if (stop_line_ > 0 && !entry.code()) {
      ContinueToken(at, Position{0, 0}, entry, entry.kind == DiffLexState::Kind::BlockComment
                                                    ? DiffTokenKind::Comment
                                                    : DiffTokenKind::String);
    }
    while (at.line < stop_line_) {
      LexFrom(at);
    }
    return end_state_;
  }

 private:
  struct Position {
    std::uint32_t line = 0;
    std::size_t column = 0;
  };

  void MarkInteresting(std::string_view text) {
    if (!text.empty()) {
      interesting_[static_cast<unsigned char>(text.front())] = true;
    }
  }

  // Whether line `line + 1` starts right after line `line` ends, separated
  // only by its line terminator.
  bool ContiguousWithNext(std::uint32_t line) const {
    if (line + 1 >= lines_.size()) {
      return false;
    }
    const auto end = reinterpret_cast<std::uintptr_t>(lines_[line].data() + lines_[line].size());
    const auto next = reinterpret_cast<std::uintptr_t>(lines_[line + 1].data());
    return next > end && next - end <= 2;
  }

  // Searches for `close` from `from`, crossing line boundaries while the lines
  // stay contiguous. With `escapes`, a backslash skips the byte after it.
  bool FindClose(Position from, std::string_view close, bool escapes, Position& end) const {
    std::uint32_t line = from.line;
    std::size_t column = from.column;
    while (true) {
      const std::string_view text = lines_[line];
      while (column < text.size()) {
        if (escapes && text[column] == '\\') {
          column += 2;
          continue;
        }
        if (StartsWithAt(text, column, close)) {
          end = Position{line, column + close.size()};
          return true;
        }
        ++column;
      }
      if (!ContiguousWithNext(line)) {
        return false;
      }
      ++line;
      column = 0;
    }
  }

  // A here-document's body: from the start of line `from`, through the line
  // that is the word alone (after the indentation its form strips).
  bool FindHeredocEnd(std::uint32_t from, const DiffLexState& state, Position& end) const {
    for (std::uint32_t line = from;; ++line) {
      std::string_view text = lines_[line];
      if (state.index == 1) {
        text.remove_prefix(std::min(text.find_first_not_of('\t'), text.size()));
      } else if (state.index == 2) {
        text.remove_prefix(std::min(text.find_first_not_of(" \t"), text.size()));
      }
      if (text == state.delimiter) {
        end = Position{line, lines_[line].size()};
        return true;
      }
      if (!ContiguousWithNext(line)) {
        return false;
      }
    }
  }

  // The end of the region: where an unterminated multi-line token stops.
  Position RegionEnd(std::uint32_t from_line) const {
    std::uint32_t line = from_line;
    while (ContiguousWithNext(line)) {
      ++line;
    }
    return Position{line, lines_[line].size()};
  }

  void Emit(Position begin, Position end, DiffTokenKind kind,
            std::uint32_t delimiter = kNoDiffNode) {
    if (tokens_ == nullptr) {
      return;
    }
    const char* first = lines_[begin.line].data() + begin.column;
    const char* last = lines_[end.line].data() + end.column;
    tokens_->push_back(DiffToken{
        .text = std::string_view(first, static_cast<std::size_t>(last - first)),
        .line = begin.line,
        .column = static_cast<std::uint32_t>(begin.column),
        .end_line = end.line,
        .end_column = static_cast<std::uint32_t>(end.column),
        .kind = kind,
    });
    delimiter_of_->push_back(delimiter);
  }

  // The rest of a multi-line token in `state`, whose text starts at `begin`
  // and whose body is searched from `body`. An unterminated one runs to the
  // region's end, and when that is the last line the region ends inside it.
  void ContinueToken(Position& at, Position body, const DiffLexState& state, DiffTokenKind kind) {
    const Position begin = at;
    Position end;
    bool closed = false;
    char closer[40];
    std::size_t closer_size = 0;
    switch (state.kind) {
      case DiffLexState::Kind::Code:
        return;
      case DiffLexState::Kind::BlockComment:
        closed = FindClose(body, syntax_.block_comments[state.index].close, false, end);
        break;
      case DiffLexState::Kind::TripleString: {
        const char quote = static_cast<char>(state.index);
        const char triple[3] = {quote, quote, quote};
        closed = FindClose(body, std::string_view(triple, 3), true, end);
        break;
      }
      case DiffLexState::Kind::BacktickString:
        closed = FindClose(body, "`", true, end);
        break;
      case DiffLexState::Kind::CppRawString:
        closer[closer_size++] = ')';
        for (const char c : state.delimiter) {
          closer[closer_size++] = c;
        }
        closer[closer_size++] = '"';
        closed = FindClose(body, std::string_view(closer, closer_size), false, end);
        break;
      case DiffLexState::Kind::RustRawString:
        closer[closer_size++] = '"';
        for (std::uint8_t hash = 0; hash < state.index; ++hash) {
          closer[closer_size++] = '#';
        }
        closed = FindClose(body, std::string_view(closer, closer_size), false, end);
        break;
      case DiffLexState::Kind::Heredoc:
        closed = FindHeredocEnd(body.line, state, end);
        break;
    }
    if (!closed) {
      end = RegionEnd(body.line);
    }
    NoteCrossing(begin, end, state);
    if (!closed && end.line + 1 == stop_line_ && stop_line_ == lines_.size()) {
      end_state_ = state;  // runs off the end of everything: still open there
    }
    if (end.line != begin.line || end.column != begin.column) {
      Emit(begin, end, kind);  // an empty here-document body is no token
    }
    at = end;
  }

  // A token running into the stop line is what that line starts inside.
  void NoteCrossing(Position begin, Position end, const DiffLexState& state) {
    if (begin.line < stop_line_ &&
        (end.line > stop_line_ || (end.line == stop_line_ && end.column > 0))) {
      end_state_ = state;
    }
  }

  bool TryComment(Position& at) {
    const std::string_view text = lines_[at.line];
    for (std::uint8_t index = 0; index < syntax_.block_comments.size(); ++index) {
      const DiffDelimiterPair& pair = syntax_.block_comments[index];
      if (StartsWithAt(text, at.column, pair.open)) {
        ContinueToken(at, Position{at.line, at.column + pair.open.size()},
                      DiffLexState{.kind = DiffLexState::Kind::BlockComment, .index = index},
                      DiffTokenKind::Comment);
        return true;
      }
    }
    for (const std::string& prefix : syntax_.line_comments) {
      if (StartsWithAt(text, at.column, prefix)) {
        const Position end{at.line, text.size()};
        Emit(at, end, DiffTokenKind::Comment);
        at = end;
        return true;
      }
    }
    return false;
  }

  // Raw strings and here-documents. Only at the start of a word: `fooR"x"` is
  // a word and a string, not a raw string.
  bool TryRawString(Position& at) {
    if (syntax_.raw_strings == 0) {
      return false;
    }
    const std::string_view text = lines_[at.line];
    const std::size_t column = at.column;
    if (column > 0 && IsWordByte(static_cast<unsigned char>(text[column - 1]))) {
      return false;
    }
    if ((syntax_.raw_strings & kDiffRawCpp) != 0) {
      std::size_t r = column;
      for (const std::string_view prefix : {"u8", "u", "U", "L"}) {
        if (StartsWithAt(text, column, prefix) && StartsWithAt(text, column + prefix.size(), "R\"")) {
          r = column + prefix.size();
          break;
        }
      }
      if (StartsWithAt(text, r, "R\"")) {
        // d-chars: at most 16, none of space, parentheses, backslash or quote.
        std::size_t open = r + 2;
        while (open < text.size() && open - (r + 2) <= 16 && text[open] != '(' &&
               text[open] != ')' && text[open] != '\\' && text[open] != ' ' && text[open] != '"') {
          ++open;
        }
        if (open < text.size() && text[open] == '(' && open - (r + 2) <= 16) {
          ContinueToken(at, Position{at.line, open + 1},
                        DiffLexState{.kind = DiffLexState::Kind::CppRawString,
                                     .delimiter = text.substr(r + 2, open - (r + 2))},
                        DiffTokenKind::String);
          return true;
        }
      }
    }
    if ((syntax_.raw_strings & kDiffRawRust) != 0) {
      std::size_t r = column;
      if ((StartsWithAt(text, column, "br") || StartsWithAt(text, column, "cr"))) {
        r = column + 1;
      }
      if (r < text.size() && text[r] == 'r') {
        std::size_t quote = r + 1;
        while (quote < text.size() && text[quote] == '#' && quote - (r + 1) < 32) {
          ++quote;
        }
        if (quote < text.size() && text[quote] == '"') {
          ContinueToken(at, Position{at.line, quote + 1},
                        DiffLexState{.kind = DiffLexState::Kind::RustRawString,
                                     .index = static_cast<std::uint8_t>(quote - (r + 1))},
                        DiffTokenKind::String);
          return true;
        }
      }
    }
    if ((syntax_.raw_strings & kDiffHeredoc) != 0 && StartsWithAt(text, column, "<<") &&
        !StartsWithAt(text, column, "<<<")) {
      std::size_t word = column + 2;
      std::uint8_t form = 0;
      if (word < text.size() && (text[word] == '-' || text[word] == '~')) {
        form = text[word] == '-' ? 1 : 2;
        ++word;
      }
      if ((syntax_.raw_strings & kDiffHeredocSpaced) != 0) {
        while (word < text.size() && text[word] == ' ') {
          ++word;
        }
      }
      char quote = 0;
      if (word < text.size() && (text[word] == '\'' || text[word] == '"')) {
        quote = text[word];
        ++word;
      }
      std::size_t word_end = word;
      while (word_end < text.size() && IsWordByte(static_cast<unsigned char>(text[word_end])) &&
             static_cast<unsigned char>(text[word_end]) < 0x80) {
        ++word_end;
      }
      const bool starts_like_word = word < word_end && !(text[word] >= '0' && text[word] <= '9');
      std::size_t marker_end = word_end;
      if (quote != 0) {
        if (marker_end >= text.size() || text[marker_end] != quote) {
          return false;
        }
        ++marker_end;
      }
      if (!starts_like_word) {
        return false;
      }
      // The marker is a token of its line; the body opens on the next one and
      // is read when this line is done (the rest of it is still code).
      const Position marker_end_at{at.line, marker_end};
      Emit(at, marker_end_at, DiffTokenKind::String);
      pending_heredoc_ = DiffLexState{.kind = DiffLexState::Kind::Heredoc,
                                      .index = form,
                                      .delimiter = text.substr(word, word_end - word)};
      at = marker_end_at;
      return true;
    }
    return false;
  }

  bool TryString(Position& at) {
    const std::uint32_t line = at.line;
    const std::size_t column = at.column;
    const std::string_view text = lines_[line];
    const char quote = text[column];
    if (syntax_.string_quotes.find(quote) == std::string::npos) {
      return false;
    }
    const char triple[3] = {quote, quote, quote};
    if (quote != '`' && StartsWithAt(text, column, std::string_view(triple, 3))) {
      ContinueToken(at, Position{line, column + 3},
                    DiffLexState{.kind = DiffLexState::Kind::TripleString,
                                 .index = static_cast<std::uint8_t>(quote)},
                    DiffTokenKind::String);
      return true;
    }
    // An apostrophe inside a word ("don't") is not a quote.
    if (quote == '\'' && column > 0 && IsWordByte(static_cast<unsigned char>(text[column - 1]))) {
      return false;
    }
    if (quote == '`') {
      // A backtick string may span lines, but only a closed one is a string.
      Position end;
      if (!FindClose(Position{line, column + 1}, "`", true, end)) {
        return false;  // lexed as punctuation by the caller
      }
      NoteCrossing(at, end, DiffLexState{.kind = DiffLexState::Kind::BacktickString});
      Emit(at, end, DiffTokenKind::String);
      at = end;
      return true;
    }
    // Single-line: search this line only.
    std::size_t scan = column + 1;
    while (scan < text.size()) {
      if (text[scan] == '\\') {
        scan += 2;
        continue;
      }
      if (text[scan] == quote) {
        const Position end{line, scan + 1};
        Emit(at, end, DiffTokenKind::String);
        at = end;
        return true;
      }
      ++scan;
    }
    return false;  // lexed as punctuation by the caller
  }

  bool TryDelimiter(Position& at) {
    const std::uint32_t line = at.line;
    const std::size_t column = at.column;
    const std::string_view text = lines_[line];
    for (std::uint32_t index = 0; index < syntax_.delimiters.size(); ++index) {
      const DiffDelimiterPair& pair = syntax_.delimiters[index];
      if (StartsWithAt(text, column, pair.open)) {
        const Position end{line, column + pair.open.size()};
        Emit(at, end, DiffTokenKind::Open, index);
        at = end;
        return true;
      }
      if (StartsWithAt(text, column, pair.close)) {
        const Position end{line, column + pair.close.size()};
        Emit(at, end, DiffTokenKind::Close, index);
        at = end;
        return true;
      }
    }
    return false;
  }

  // Whether a token other than punctuation begins at `column`.
  bool StartsOtherToken(std::string_view text, std::size_t column) const {
    const auto byte = static_cast<unsigned char>(text[column]);
    if (IsWordByte(byte) || IsSpaceByte(byte) ||
        syntax_.string_quotes.find(text[column]) != std::string::npos) {
      return true;
    }
    if ((syntax_.raw_strings & kDiffHeredoc) != 0 && StartsWithAt(text, column, "<<")) {
      return true;
    }
    for (const DiffDelimiterPair& pair : syntax_.delimiters) {
      if (StartsWithAt(text, column, pair.open) || StartsWithAt(text, column, pair.close)) {
        return true;
      }
    }
    for (const DiffDelimiterPair& pair : syntax_.block_comments) {
      if (StartsWithAt(text, column, pair.open)) {
        return true;
      }
    }
    for (const std::string& prefix : syntax_.line_comments) {
      if (StartsWithAt(text, column, prefix)) {
        return true;
      }
    }
    return false;
  }

  // A here-document whose marker this line held: its body is the next lines.
  void FinishLine(Position& at, std::uint32_t line) {
    if (pending_heredoc_.code()) {
      return;
    }
    const DiffLexState heredoc = pending_heredoc_;
    pending_heredoc_ = {};
    if (line + 1 == stop_line_) {
      end_state_ = heredoc;  // the body starts on the stop line
      return;
    }
    if (!ContiguousWithNext(line)) {
      return;
    }
    at = Position{line + 1, 0};
    ContinueToken(at, at, heredoc, DiffTokenKind::String);
    // The body's last line is the word alone: lexing resumes after it.
    if (at.column == lines_[at.line].size()) {
      at = Position{at.line + 1, 0};
    }
  }

  // Lexes from `at` to the end of its line, or further when a multi-line
  // token carries it onto a later line; leaves `at` where lexing resumes.
  void LexFrom(Position& at) {
    const std::uint32_t line = at.line;
    const std::string_view text = lines_[line];
    const bool scanning = tokens_ == nullptr;
    while (at.line == line && at.column < text.size()) {
      std::size_t& column = at.column;
      const auto byte = static_cast<unsigned char>(text[column]);
      if (IsSpaceByte(byte)) {
        ++column;
        continue;
      }
      if (scanning && !IsWordByte(byte) && !interesting_[byte]) {
        ++column;  // nothing that can span a line starts here
        continue;
      }
      if (TryComment(at) || TryRawString(at) || TryString(at) || (!scanning && TryDelimiter(at))) {
        continue;
      }
      const std::size_t start = column;
      if (IsWordByte(byte)) {
        while (column < text.size() && IsWordByte(static_cast<unsigned char>(text[column]))) {
          ++column;
        }
        Emit(Position{line, start}, Position{line, column}, DiffTokenKind::Word);
        continue;
      }
      // A run of operator bytes is one atom (`->`, `+=`, `!==`), except that a
      // separator stands alone so a list's commas line up one to one.
      ++column;
      if (scanning) {
        continue;
      }
      if (byte != ',' && byte != ';') {
        while (column < text.size() && text[column] != ',' && text[column] != ';' &&
               !StartsOtherToken(text, column)) {
          ++column;
        }
      }
      Emit(Position{line, start}, Position{line, column}, DiffTokenKind::Punctuation);
    }
    if (at.line == line) {
      FinishLine(at, line);
    } else {
      // A multi-line token carried lexing off the marker's line: a region that
      // starts inside that token could not know a here-document was pending, so
      // neither does this (no real code puts a marker before one).
      pending_heredoc_ = {};
    }
    if (at.line == line) {
      at = Position{line + 1, 0};
    }
  }

  std::span<const std::string_view> lines_;
  const DiffSyntax& syntax_;
  std::vector<DiffToken>* tokens_;
  std::vector<std::uint32_t>* delimiter_of_;
  std::size_t stop_line_;
  bool interesting_[256] = {};
  DiffLexState pending_heredoc_;
  DiffLexState end_state_;
};

// How far down the open-delimiter stack a close delimiter looks for its
// partner. Bounded so a region of unbalanced closes stays linear.
constexpr std::size_t kMaxDelimiterRecovery = 16;

struct TreeBuildScratch {
  std::vector<std::uint32_t> delimiter_of;
  std::vector<std::uint32_t> partner;
  std::vector<std::uint32_t> open_stack;
  struct Frame {
    std::uint32_t node = kNoDiffNode;
    std::uint32_t last_child = kNoDiffNode;
  };
  std::vector<Frame> frames;
};

TreeBuildScratch& BuildScratch() {
  thread_local TreeBuildScratch scratch;
  return scratch;
}

// Word-at-a-time (the standard library's string hash), not a byte loop: every
// token of both sides of every hunk is hashed once per refinement.
std::uint64_t AtomHash(const DiffToken& token) {
  return std::hash<std::string_view>{}(token.text) * 0x9E3779B97F4A7C15ULL +
         static_cast<std::uint64_t>(token.kind);
}

std::uint64_t Combine(std::uint64_t hash, std::uint64_t value) {
  hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
  return hash;
}

}  // namespace

void DiffContentInterner::Reset() {
  // Clear only the buckets this comparison used: the table keeps the size the
  // largest comparison grew it to, and one refinement resets it per hunk, so
  // refilling all of it made every small hunk pay for the largest.
  const std::size_t mask = buckets_.size() - 1;
  for (const Entry& entry : entries_) {
    buckets_[entry.hash & mask] = kNoDiffNode;
  }
  entries_.clear();
}

void DiffContentInterner::Grow() {
  const std::size_t size = buckets_.empty() ? 1024 : buckets_.size() * 2;
  buckets_.assign(size, kNoDiffNode);
  for (std::uint32_t index = 0; index < entries_.size(); ++index) {
    Entry& entry = entries_[index];
    const std::size_t bucket = entry.hash & (size - 1);
    entry.next = buckets_[bucket];
    buckets_[bucket] = index;
  }
}

bool DiffContentInterner::SameContent(const Entry& entry, const DiffSyntaxTree& tree,
                                      std::uint32_t node_index) const {
  const DiffSyntaxTree& other = *entry.tree;
  const DiffSyntaxNode& a = other.nodes[entry.node];
  const DiffSyntaxNode& b = tree.nodes[node_index];
  if (a.is_list != b.is_list) {
    return false;
  }
  const DiffToken& a_open = other.OpenToken(a);
  const DiffToken& b_open = tree.OpenToken(b);
  if (a_open.kind != b_open.kind || a_open.text != b_open.text) {
    return false;
  }
  if (!a.is_list) {
    return true;
  }
  if (other.CloseToken(a).text != tree.CloseToken(b).text) {
    return false;
  }
  std::uint32_t a_child = a.first_child;
  std::uint32_t b_child = b.first_child;
  while (a_child != kNoDiffNode && b_child != kNoDiffNode) {
    if (other.nodes[a_child].content_id != tree.nodes[b_child].content_id) {
      return false;
    }
    a_child = other.nodes[a_child].next_sibling;
    b_child = tree.nodes[b_child].next_sibling;
  }
  return a_child == kNoDiffNode && b_child == kNoDiffNode;
}

std::uint32_t DiffContentInterner::Intern(const DiffSyntaxTree& tree, std::uint32_t node_index) {
  const DiffSyntaxNode& node = tree.nodes[node_index];
  std::uint64_t hash = AtomHash(tree.OpenToken(node));
  if (node.is_list) {
    hash = Combine(hash, std::hash<std::string_view>{}(tree.CloseToken(node).text));
    for (std::uint32_t child = node.first_child; child != kNoDiffNode;
         child = tree.nodes[child].next_sibling) {
      hash = Combine(hash, tree.nodes[child].content_id);
    }
  }
  if (entries_.size() * 2 >= buckets_.size()) {
    Grow();
  }
  const std::size_t bucket = hash & (buckets_.size() - 1);
  for (std::uint32_t index = buckets_[bucket]; index != kNoDiffNode; index = entries_[index].next) {
    if (entries_[index].hash == hash && SameContent(entries_[index], tree, node_index)) {
      return index;
    }
  }
  const auto id = static_cast<std::uint32_t>(entries_.size());
  entries_.push_back(Entry{.tree = &tree, .node = node_index, .hash = hash, .next = buckets_[bucket]});
  buckets_[bucket] = id;
  return id;
}

DiffLexState ScanDiffLexState(std::span<const std::string_view> lines,
                              std::size_t stop_line,
                              const DiffSyntax& syntax,
                              DiffLexState state) {
  if (stop_line == 0 || lines.empty()) {
    return state;
  }
  return Lexer(lines, syntax, nullptr, nullptr, stop_line).Run(state);
}

void BuildDiffSyntaxTree(std::span<const std::string_view> lines,
                         const DiffSyntax& syntax,
                         DiffContentInterner& interner,
                         DiffSyntaxTree& tree,
                         const DiffLexState& entry) {
  tree.Clear();
  TreeBuildScratch& scratch = BuildScratch();
  scratch.delimiter_of.clear();
  if (lines.empty()) {
    return;
  }
  (void)Lexer(lines, syntax, &tree.tokens, &scratch.delimiter_of, lines.size()).Run(entry);

  // Pair delimiters. A close looks a bounded way down the stack for its open;
  // opens it skips over, and closes with no open, are not delimiters after all.
  std::vector<std::uint32_t>& partner = scratch.partner;
  partner.assign(tree.tokens.size(), kNoDiffNode);
  std::vector<std::uint32_t>& stack = scratch.open_stack;
  stack.clear();
  for (std::uint32_t index = 0; index < tree.tokens.size(); ++index) {
    const DiffTokenKind kind = tree.tokens[index].kind;
    if (kind == DiffTokenKind::Open) {
      stack.push_back(index);
    } else if (kind == DiffTokenKind::Close) {
      const std::size_t floor = stack.size() > kMaxDelimiterRecovery
                                    ? stack.size() - kMaxDelimiterRecovery
                                    : 0;
      for (std::size_t depth = stack.size(); depth > floor; --depth) {
        const std::uint32_t open = stack[depth - 1];
        if (scratch.delimiter_of[open] == scratch.delimiter_of[index]) {
          partner[open] = index;
          partner[index] = open;
          stack.resize(depth - 1);
          break;
        }
      }
    }
  }
  for (std::uint32_t index = 0; index < tree.tokens.size(); ++index) {
    DiffToken& token = tree.tokens[index];
    if ((token.kind == DiffTokenKind::Open || token.kind == DiffTokenKind::Close) &&
        partner[index] == kNoDiffNode) {
      token.kind = DiffTokenKind::Punctuation;
    }
  }

  tree.nodes.reserve(tree.tokens.size());
  std::vector<TreeBuildScratch::Frame>& frames = scratch.frames;
  frames.clear();
  frames.push_back({});  // the root level
  const auto link = [&](std::uint32_t node_index) {
    TreeBuildScratch::Frame& frame = frames.back();
    DiffSyntaxNode& node = tree.nodes[node_index];
    node.parent = frame.node;
    node.depth = static_cast<std::uint32_t>(frames.size() - 1);
    if (frame.last_child == kNoDiffNode) {
      if (frame.node == kNoDiffNode) {
        tree.first_root = node_index;
      } else {
        tree.nodes[frame.node].first_child = node_index;
      }
    } else {
      tree.nodes[frame.last_child].next_sibling = node_index;
    }
    frame.last_child = node_index;
  };
  for (std::uint32_t index = 0; index < tree.tokens.size(); ++index) {
    const DiffTokenKind kind = tree.tokens[index].kind;
    if (kind == DiffTokenKind::Close) {
      const std::uint32_t list = frames.back().node;
      frames.pop_back();
      DiffSyntaxNode& node = tree.nodes[list];
      node.token_end = index + 1;
      node.subtree_end = static_cast<std::uint32_t>(tree.nodes.size());
      node.content_id = interner.Intern(tree, list);
      continue;
    }
    const auto node_index = static_cast<std::uint32_t>(tree.nodes.size());
    tree.nodes.push_back(DiffSyntaxNode{
        .subtree_end = node_index + 1,
        .token_begin = index,
        .token_end = index + 1,
        .is_list = kind == DiffTokenKind::Open,
    });
    link(node_index);
    if (kind == DiffTokenKind::Open) {
      frames.push_back(TreeBuildScratch::Frame{.node = node_index});
    } else {
      tree.nodes[node_index].content_id = interner.Intern(tree, node_index);
    }
  }
  // Do not keep a huge region's working arrays alive for the next one.
  constexpr std::size_t kRetainTokens = 1u << 16;
  if (partner.capacity() > kRetainTokens) {
    std::vector<std::uint32_t>().swap(scratch.delimiter_of);
    std::vector<std::uint32_t>().swap(scratch.partner);
  }
}

}  // namespace microide::compare
