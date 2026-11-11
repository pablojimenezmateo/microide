#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The syntax layer of the structural (difftastic-style) diff.
//
// A changed region of a file is read as a forest of syntax nodes: ATOMS (an
// identifier, a number, an operator, a whole string literal, a whole comment)
// and LISTS (a delimiter pair and the nodes between them). The structural diff
// then compares the two forests instead of the two runs of lines, which is what
// lets it say "these three lines only moved into a new block" or "only this
// argument changed" rather than "these lines differ".
//
// The tree is built by a language-aware LEXER plus bracket matching, not by a
// full grammar: the lexical rules (comment and string syntax, the delimiter
// pairs) come from the language's `DiffSyntax`. That keeps the kernel free of a
// parser dependency and works for every language the editor knows, at the cost
// of the finer structure a real grammar sees (an `if` and its condition are
// siblings here, not a parent and child). See known-tech-debt.md
// TD-2026-11-11-440 for the grammar-backed follow-up.
namespace microide::compare {

struct DiffDelimiterPair {
  std::string open;
  std::string close;
};

// The lexical rules of one language, as the structural diff needs them. Value
// type, built once per language by the host (from its language contract) and
// shared read-only by every compare model of that language.
struct DiffSyntax {
  // Prefixes that start a comment running to the end of the line ("//", "#").
  std::vector<std::string> line_comments;
  // Comment pairs that may span lines ("/*" "*/").
  std::vector<DiffDelimiterPair> block_comments;
  // The nesting delimiters that make a LIST: "(" ")", "[" "]", "{" "}".
  std::vector<DiffDelimiterPair> delimiters;
  // Single-byte string quotes. A quote opens a string that must close on the
  // same line, except a backtick (template / raw strings) and a tripled quote
  // (`"""`, `'''`), which may span lines. A quote with no closing partner is
  // read as an ordinary punctuation atom.
  std::string string_quotes = "\"'`";
};

// The rules used when the host has none for the file's language: the three
// common bracket pairs, the three common quotes, and no comment syntax.
const std::shared_ptr<const DiffSyntax>& GenericDiffSyntax();

inline constexpr std::uint32_t kNoDiffNode = std::numeric_limits<std::uint32_t>::max();

enum class DiffTokenKind : std::uint8_t {
  Word,         // identifier, keyword, number
  Punctuation,  // operator runs and separators
  String,       // a whole string literal, quotes included
  Comment,      // a whole comment, markers included
  Open,         // a matched opening delimiter
  Close,        // a matched closing delimiter
};

// One lexed token of a changed region. A token never contains a line break
// unless it is a multi-line string or comment.
struct DiffToken {
  std::string_view text;
  // Line index within the region (0-based) and byte column in that line, of
  // the first and of the last byte.
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t end_line = 0;
  std::uint32_t end_column = 0;  // exclusive, on `end_line`
  DiffTokenKind kind = DiffTokenKind::Word;
};

// One syntax node, stored in preorder. A node's subtree is the contiguous
// node range [index, subtree_end) and covers the contiguous token range
// [token_begin, token_end); an atom covers exactly one token, a list its open
// delimiter, its children's tokens, and its close delimiter.
struct DiffSyntaxNode {
  std::uint32_t parent = kNoDiffNode;
  std::uint32_t next_sibling = kNoDiffNode;
  std::uint32_t first_child = kNoDiffNode;  // lists only; kNoDiffNode when empty
  std::uint32_t subtree_end = 0;
  std::uint32_t token_begin = 0;
  std::uint32_t token_end = 0;
  // Equal ids <=> structurally equal subtrees (same token texts, same shape),
  // across BOTH trees of one comparison. Whitespace and line breaks between
  // tokens do not take part, which is what makes a re-indented or re-wrapped
  // subtree compare equal.
  std::uint32_t content_id = 0;
  std::uint32_t depth = 0;
  bool is_list = false;
};

struct DiffSyntaxTree {
  std::vector<DiffToken> tokens;
  std::vector<DiffSyntaxNode> nodes;
  // First root-level node, or kNoDiffNode for a region with no tokens. Root
  // nodes are chained through `next_sibling`.
  std::uint32_t first_root = kNoDiffNode;

  void Clear() {
    tokens.clear();
    nodes.clear();
    first_root = kNoDiffNode;
  }
  const DiffToken& OpenToken(const DiffSyntaxNode& node) const { return tokens[node.token_begin]; }
  const DiffToken& CloseToken(const DiffSyntaxNode& node) const {
    return tokens[node.token_end - 1];
  }
};

// Assigns content ids. One interner serves both trees of a comparison, so an id
// means the same subtree on either side; `Reset` before each comparison.
class DiffContentInterner {
 public:
  void Reset();
  // Called once per node, children before parents (the builder guarantees it).
  std::uint32_t Intern(const DiffSyntaxTree& tree, std::uint32_t node_index);

 private:
  struct Entry {
    const DiffSyntaxTree* tree = nullptr;
    std::uint32_t node = 0;
    std::uint64_t hash = 0;
    std::uint32_t next = kNoDiffNode;  // next entry in the same bucket
  };
  bool SameContent(const Entry& entry, const DiffSyntaxTree& tree, std::uint32_t node) const;
  void Grow();

  std::vector<Entry> entries_;
  std::vector<std::uint32_t> buckets_;  // power-of-two sized; kNoDiffNode = empty
  std::vector<std::uint64_t> node_hash_scratch_;
};

// Lexes `lines` (consecutive lines of one buffer, terminators excluded) and
// builds their syntax forest into `tree`, interning every node. Delimiters that
// do not pair up inside the region become punctuation atoms, so a region that
// opens a block and a later region that closes it each still diff sensibly.
void BuildDiffSyntaxTree(std::span<const std::string_view> lines,
                         const DiffSyntax& syntax,
                         DiffContentInterner& interner,
                         DiffSyntaxTree& tree);

}  // namespace microide::compare
