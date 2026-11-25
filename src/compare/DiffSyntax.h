#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::compare {

struct DiffDelimiterPair {
  std::string open;
  std::string close;
};

struct DiffSyntax {
  std::vector<std::string> line_comments;
  std::vector<DiffDelimiterPair> block_comments;
  std::vector<DiffDelimiterPair> delimiters;
  std::string string_quotes = "\"'`";
  std::uint8_t raw_strings = 0;
};

enum DiffRawStrings : std::uint8_t {
  kDiffRawCpp = 1u << 0,
  kDiffRawRust = 1u << 1,
  kDiffHeredoc = 1u << 2,
  kDiffHeredocSpaced = 1u << 3,
};

struct DiffLexState {
  enum class Kind : std::uint8_t {
    Code,
    BlockComment,
    TripleString,
    BacktickString,
    CppRawString,
    RustRawString,
    Heredoc,
  };
  Kind kind = Kind::Code;
  std::uint8_t index = 0;
  std::string_view delimiter;

  bool code() const { return kind == Kind::Code; }
  friend bool operator==(const DiffLexState& a, const DiffLexState& b) {
    return a.kind == b.kind && a.index == b.index && a.delimiter == b.delimiter;
  }
};

const std::shared_ptr<const DiffSyntax>& GenericDiffSyntax();

inline constexpr std::uint32_t kNoDiffNode = std::numeric_limits<std::uint32_t>::max();

enum class DiffTokenKind : std::uint8_t {
  Word,
  Punctuation,
  String,
  Comment,
  Open,
  Close,
};

struct DiffToken {
  std::string_view text;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  std::uint32_t end_line = 0;
  std::uint32_t end_column = 0;
  DiffTokenKind kind = DiffTokenKind::Word;
};

struct DiffSyntaxNode {
  std::uint32_t parent = kNoDiffNode;
  std::uint32_t next_sibling = kNoDiffNode;
  std::uint32_t first_child = kNoDiffNode;
  std::uint32_t subtree_end = 0;
  std::uint32_t token_begin = 0;
  std::uint32_t token_end = 0;
  std::uint32_t content_id = 0;
  std::uint32_t depth = 0;
  bool is_list = false;
};

struct DiffSyntaxTree {
  std::vector<DiffToken> tokens;
  std::vector<DiffSyntaxNode> nodes;
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

class DiffContentInterner {
 public:
  void Reset();
  std::uint32_t Intern(const DiffSyntaxTree& tree, std::uint32_t node_index);

 private:
  struct Entry {
    const DiffSyntaxTree* tree = nullptr;
    std::uint32_t node = 0;
    std::uint64_t hash = 0;
    std::uint32_t next = kNoDiffNode;
  };
  bool SameContent(const Entry& entry, const DiffSyntaxTree& tree, std::uint32_t node) const;
  void Grow();

  std::vector<Entry> entries_;
  std::vector<std::uint32_t> buckets_;
  std::vector<std::uint64_t> node_hash_scratch_;
};

void BuildDiffSyntaxTree(std::span<const std::string_view> lines,
                         const DiffSyntax& syntax,
                         DiffContentInterner& interner,
                         DiffSyntaxTree& tree,
                         const DiffLexState& entry = {});

DiffLexState ScanDiffLexState(std::span<const std::string_view> lines,
                              std::size_t stop_line,
                              const DiffSyntax& syntax,
                              DiffLexState state);

}
