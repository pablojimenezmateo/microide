#include "compare/DiffSyntax.h"

#include <algorithm>
#include <cstdint>

#include "util/Fnv1a.h"

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
// backtick and triple-quoted strings) are only formed when the lines are
// contiguous in memory — they are whenever they come from one buffer, which is
// every production caller — because the token's text is one view over them.
class Lexer {
 public:
  Lexer(std::span<const std::string_view> lines, const DiffSyntax& syntax,
        std::vector<DiffToken>& tokens, std::vector<std::uint32_t>& delimiter_of)
      : lines_(lines), syntax_(syntax), tokens_(tokens), delimiter_of_(delimiter_of) {}

  void Run() {
    Position at;
    while (at.line < lines_.size()) {
      LexFrom(at);
    }
  }

 private:
  struct Position {
    std::uint32_t line = 0;
    std::size_t column = 0;
  };

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

  // The end of the region: where an unterminated multi-line comment or
  // triple-quoted string stops.
  Position RegionEnd(std::uint32_t from_line) const {
    std::uint32_t line = from_line;
    while (ContiguousWithNext(line)) {
      ++line;
    }
    return Position{line, lines_[line].size()};
  }

  void Emit(Position begin, Position end, DiffTokenKind kind,
            std::uint32_t delimiter = kNoDiffNode) {
    const char* first = lines_[begin.line].data() + begin.column;
    const char* last = lines_[end.line].data() + end.column;
    tokens_.push_back(DiffToken{
        .text = std::string_view(first, static_cast<std::size_t>(last - first)),
        .line = begin.line,
        .column = static_cast<std::uint32_t>(begin.column),
        .end_line = end.line,
        .end_column = static_cast<std::uint32_t>(end.column),
        .kind = kind,
    });
    delimiter_of_.push_back(delimiter);
  }

  bool TryComment(Position& at) {
    const std::uint32_t line = at.line;
    const std::size_t column = at.column;
    const std::string_view text = lines_[line];
    for (const DiffDelimiterPair& pair : syntax_.block_comments) {
      if (!StartsWithAt(text, column, pair.open)) {
        continue;
      }
      Position end;
      if (!FindClose(Position{line, column + pair.open.size()}, pair.close, false, end)) {
        end = RegionEnd(line);
      }
      Emit(at, end, DiffTokenKind::Comment);
      at = end;
      return true;
    }
    for (const std::string& prefix : syntax_.line_comments) {
      if (StartsWithAt(text, column, prefix)) {
        const Position end{line, text.size()};
        Emit(at, end, DiffTokenKind::Comment);
        at = end;
        return true;
      }
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
    const std::string_view triple_view(triple, 3);
    if (quote != '`' && StartsWithAt(text, column, triple_view)) {
      Position end;
      if (!FindClose(Position{line, column + 3}, triple_view, true, end)) {
        end = RegionEnd(line);
      }
      Emit(at, end, DiffTokenKind::String);
      at = end;
      return true;
    }
    // An apostrophe inside a word ("don't") is not a quote.
    if (quote == '\'' && column > 0 && IsWordByte(static_cast<unsigned char>(text[column - 1]))) {
      return false;
    }
    const std::string_view close(&text[column], 1);
    Position end;
    bool found = false;
    if (quote == '`') {
      found = FindClose(Position{line, column + 1}, close, true, end);
    } else {
      // Single-line: search this line only.
      std::size_t scan = column + 1;
      while (scan < text.size()) {
        if (text[scan] == '\\') {
          scan += 2;
          continue;
        }
        if (text[scan] == quote) {
          end = Position{line, scan + 1};
          found = true;
          break;
        }
        ++scan;
      }
    }
    if (!found) {
      return false;  // lexed as punctuation by the caller
    }
    Emit(at, end, DiffTokenKind::String);
    at = end;
    return true;
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

  // Lexes from `at` to the end of its line, or further when a multi-line
  // token carries it onto a later line; leaves `at` where lexing resumes.
  void LexFrom(Position& at) {
    const std::uint32_t line = at.line;
    const std::string_view text = lines_[line];
    while (at.line == line && at.column < text.size()) {
      std::size_t& column = at.column;
      const auto byte = static_cast<unsigned char>(text[column]);
      if (IsSpaceByte(byte)) {
        ++column;
        continue;
      }
      if (TryComment(at) || TryString(at) || TryDelimiter(at)) {
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
      if (byte != ',' && byte != ';') {
        while (column < text.size() && text[column] != ',' && text[column] != ';' &&
               !StartsOtherToken(text, column)) {
          ++column;
        }
      }
      Emit(Position{line, start}, Position{line, column}, DiffTokenKind::Punctuation);
    }
    if (at.line == line) {
      at = Position{line + 1, 0};
    }
  }

  std::span<const std::string_view> lines_;
  const DiffSyntax& syntax_;
  std::vector<DiffToken>& tokens_;
  std::vector<std::uint32_t>& delimiter_of_;
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

std::uint64_t AtomHash(const DiffToken& token) {
  return util::Fnv1aBytes(util::Fnv1aValue(util::kFnv1aOffsetBasis,
                                           static_cast<std::uint64_t>(token.kind)),
                          token.text);
}

}  // namespace

void DiffContentInterner::Reset() {
  entries_.clear();
  std::fill(buckets_.begin(), buckets_.end(), kNoDiffNode);
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
    hash = util::Fnv1aBytes(hash, tree.CloseToken(node).text);
    for (std::uint32_t child = node.first_child; child != kNoDiffNode;
         child = tree.nodes[child].next_sibling) {
      hash = util::Fnv1aValue(hash, tree.nodes[child].content_id);
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

void BuildDiffSyntaxTree(std::span<const std::string_view> lines,
                         const DiffSyntax& syntax,
                         DiffContentInterner& interner,
                         DiffSyntaxTree& tree) {
  tree.Clear();
  TreeBuildScratch& scratch = BuildScratch();
  scratch.delimiter_of.clear();
  if (lines.empty()) {
    return;
  }
  Lexer(lines, syntax, tree.tokens, scratch.delimiter_of).Run();

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
