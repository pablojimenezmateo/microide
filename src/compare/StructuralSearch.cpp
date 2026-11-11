#include "compare/StructuralSearch.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

// The graph search of the structural diff (see StructuralDiff.h): A* over
// (next LHS node, next RHS node, entered-delimiter stack), difftastic's
// formulation, for one gap the matcher could not split further.
namespace microide::compare::detail {

namespace {

// Edge costs: difftastic's (src/diff/graph.rs, Edge::cost). Matching is always
// cheaper than not matching; a match at a different nesting depth costs a
// little more, so between two equal matches the one at the same depth wins; a
// matched separator (`,` `;` `.`) costs enough more that an unchanged name with
// novel punctuation always beats the reverse; entering a matching delimiter
// pair costs more than an unchanged node and less than any novel edge; a
// replaced literal costs more the less similar it is, and less than the two
// novel atoms it stands for.
constexpr std::uint32_t kMaxDepthPenalty = 40;
constexpr std::uint32_t kSeparatorPenalty = 200;
constexpr std::uint32_t kEnterUnchangedCost = 10;
constexpr std::uint32_t kReplacedBaseCost = 500;
constexpr std::uint32_t kNovelCost = 300;
static_assert(kReplacedBaseCost + 100 <= 2 * kNovelCost);
// A replaced literal more similar than this is shown as an edit (its words
// diffed); a less similar one as two novel literals. difftastic's threshold.
constexpr std::uint32_t kMinReplacedSimilarityPct = 20;
// The search is A*: a vertex is queued by f = g + h, where g is its distance
// and h the heuristic below. An edge raises f by its cost plus the change in h,
// at most kNovelCost + kNovelCost, so a circular bucket queue of this size never
// aliases two pending priorities.
constexpr std::uint32_t kBucketCount = 1024;
// Starting size of a search's open-addressed tables (power of two).
constexpr std::size_t kInitialSlots = 64;
static_assert(2 * kNovelCost < kBucketCount);
static_assert(kReplacedBaseCost + 100 + kNovelCost < kBucketCount);

enum class Edge : std::uint8_t {
  Start,
  Unchanged,
  EnterUnchanged,
  Replaced,
  NovelLhs,
  NovelRhs,
  EnterNovelLhs,
  EnterNovelRhs,
  ExitBoth,
  ExitLhs,
  ExitRhs,
};

// The entered-delimiter stack, persistent (every vertex shares its tail with
// its predecessor's). A group is either ONE delimiter pair entered on both
// sides (`both`), or a run of delimiters entered on one side only, kept as two
// independent cons lists so either side can be exited without the other —
// difftastic's PopBoth / PopEither.
struct StackGroup {
  std::uint32_t prev = kNoDiffNode;
  // `both`: the two entered list nodes. Otherwise: cons-list heads.
  std::uint32_t lhs = kNoDiffNode;
  std::uint32_t rhs = kNoDiffNode;
  // The innermost entered list on each side, over the whole stack.
  std::uint32_t lhs_parent = kNoDiffNode;
  std::uint32_t rhs_parent = kNoDiffNode;
  bool both = false;
};

struct Cons {
  std::uint32_t node = kNoDiffNode;
  std::uint32_t next = kNoDiffNode;
};

struct Vertex {
  std::uint32_t lhs = kNoDiffNode;
  std::uint32_t rhs = kNoDiffNode;
  std::uint32_t stack = kNoDiffNode;
  std::uint32_t pred = kNoDiffNode;
  std::uint32_t dist = 0;
  Edge edge = Edge::Start;
  bool done = false;
};

struct QueueEntry {
  std::uint32_t vertex = 0;
  std::uint32_t dist = 0;      // g when queued: a later, shorter g makes this entry stale
  std::uint32_t priority = 0;  // f = g + h
};

struct SearchScratch {
  std::vector<Vertex> vertices;
  // Open-addressed vertex table. Each slot carries its vertex's identity, so a
  // probe compares in place instead of chasing the vertex and its stack groups.
  struct Slot {
    std::uint32_t lhs = kNoDiffNode;
    std::uint32_t rhs = kNoDiffNode;
    std::uint32_t lhs_parent = kNoDiffNode;
    std::uint32_t rhs_parent = kNoDiffNode;
    std::uint32_t vertex = kNoDiffNode;  // kNoDiffNode = empty
    bool can_pop_either = false;
  };
  std::vector<Slot> slots;
  // Similarity of replaced-literal candidates, memoized per (LHS token, RHS
  // token): the search meets the same pair from many vertices.
  std::vector<std::pair<std::uint64_t, std::uint32_t>> similarity_cache;
  std::size_t similarity_cache_used = 0;
  std::vector<StackGroup> groups;
  std::vector<Cons> conses;
  std::array<std::vector<QueueEntry>, kBucketCount> buckets;
  // Buckets that received an entry since the last reset (a bucket appears
  // again only after it emptied, so the list stays short).
  std::vector<std::uint32_t> touched_buckets;
  std::vector<std::string_view> words_a;
  std::vector<std::string_view> words_b;

  // Large graphs are rare; do not keep their memory alive for the next,
  // ordinary, comparison.
  void ReleaseIfLarge() {
    constexpr std::size_t kRetainVertices = 1u << 16;
    if (vertices.capacity() > kRetainVertices) {
      std::vector<Vertex>().swap(vertices);
      std::vector<Slot>().swap(slots);
      std::vector<StackGroup>().swap(groups);
      std::vector<Cons>().swap(conses);
      for (std::vector<QueueEntry>& bucket : buckets) {
        std::vector<QueueEntry>().swap(bucket);
      }
    }
  }
};

SearchScratch& Scratch() {
  thread_local SearchScratch scratch;
  return scratch;
}

bool IsWordByte(unsigned char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80;
}

void SplitWords(std::string_view text, std::vector<std::string_view>& words) {
  words.clear();
  std::size_t index = 0;
  while (index < text.size()) {
    if (!IsWordByte(static_cast<unsigned char>(text[index]))) {
      ++index;
      continue;
    }
    const std::size_t start = index;
    while (index < text.size() && IsWordByte(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    words.push_back(text.substr(start, index - start));
  }
}

// How much of two literals' words they share, as a percentage of their word
// bytes. Order-insensitive on purpose: it only decides whether the two are
// "the same literal, edited"; the ordered word diff that marks what changed
// runs later.
std::uint32_t LiteralSimilarityPct(std::string_view a, std::string_view b) {
  SearchScratch& scratch = Scratch();
  SplitWords(a, scratch.words_a);
  SplitWords(b, scratch.words_b);
  std::size_t total = 0;
  for (const std::string_view word : scratch.words_a) {
    total += word.size();
  }
  for (const std::string_view word : scratch.words_b) {
    total += word.size();
  }
  if (total == 0) {
    return 0;
  }
  std::sort(scratch.words_a.begin(), scratch.words_a.end());
  std::sort(scratch.words_b.begin(), scratch.words_b.end());
  std::size_t common = 0;
  auto left = scratch.words_a.begin();
  auto right = scratch.words_b.begin();
  while (left != scratch.words_a.end() && right != scratch.words_b.end()) {
    if (*left < *right) {
      ++left;
    } else if (*right < *left) {
      ++right;
    } else {
      common += 2 * left->size();
      ++left;
      ++right;
    }
  }
  return static_cast<std::uint32_t>(common * 100 / total);
}

// difftastic's looks_like_punctuation: the separators whose match says little.
bool IsSeparatorAtom(const DiffSyntaxTree& tree, const DiffSyntaxNode& node) {
  if (node.is_list) {
    return false;
  }
  const std::string_view text = tree.OpenToken(node).text;
  return text == "," || text == ";" || text == ".";
}

std::uint32_t DepthDifference(const DiffSyntaxNode& a, const DiffSyntaxNode& b) {
  const std::uint32_t difference = a.depth > b.depth ? a.depth - b.depth : b.depth - a.depth;
  return std::min(difference, kMaxDepthPenalty);
}

class Search {
 public:
  Search(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, SiblingScope lhs_scope,
         SiblingScope rhs_scope, std::size_t budget)
      : lhs_(lhs), rhs_(rhs), lhs_scope_(lhs_scope), rhs_scope_(rhs_scope), budget_(budget),
        s_(Scratch()),
        lhs_scope_end_(ScopeEnd(lhs, lhs_scope)),
        rhs_scope_end_(ScopeEnd(rhs, rhs_scope)) {}

  // Returns false when the vertex budget ran out.
  bool Run(std::uint32_t lhs_start, std::uint32_t rhs_start, StructuralMatch& match,
           std::size_t& vertex_count) {
    s_.vertices.clear();
    s_.groups.clear();
    s_.conses.clear();
    // Most searches are small gaps, many per build: start every table small
    // (they grow by rehashing) and clear only the queue buckets the previous
    // search touched. Resetting fixed 1024-entry tables per search cost more
    // than the searches themselves.
    s_.slots.assign(kInitialSlots, SearchScratch::Slot{});
    s_.similarity_cache.clear();
    s_.similarity_cache_used = 0;
    for (const std::uint32_t bucket : s_.touched_buckets) {
      s_.buckets[bucket].clear();
    }
    s_.touched_buckets.clear();
    pending_ = 0;

    const std::uint32_t start = Insert(lhs_start, rhs_start, kNoDiffNode);
    s_.vertices[start].dist = 0;
    const std::uint32_t start_priority = Heuristic(lhs_start, rhs_start, kNoDiffNode);
    Push(start, 0, start_priority);
    std::uint32_t cursor = start_priority;
    std::uint32_t end = kNoDiffNode;
    while (pending_ > 0) {
      std::vector<QueueEntry>& bucket = s_.buckets[cursor % kBucketCount];
      if (bucket.empty()) {
        ++cursor;
        continue;
      }
      const QueueEntry entry = bucket.back();
      bucket.pop_back();
      --pending_;
      Vertex& vertex = s_.vertices[entry.vertex];
      if (vertex.done || vertex.dist != entry.dist) {
        continue;
      }
      vertex.done = true;
      if (vertex.lhs == kNoDiffNode && vertex.rhs == kNoDiffNode && vertex.stack == kNoDiffNode) {
        end = entry.vertex;
        break;
      }
      Expand(entry.vertex);
      if (s_.vertices.size() > budget_) {
        vertex_count = s_.vertices.size();
        s_.ReleaseIfLarge();
        return false;
      }
    }
    vertex_count = s_.vertices.size();
    if (end == kNoDiffNode) {
      s_.ReleaseIfLarge();
      return false;  // unreachable in a well-formed graph; fall back rather than guess
    }
    for (std::uint32_t at = end; s_.vertices[at].edge != Edge::Start; at = s_.vertices[at].pred) {
      const Vertex& vertex = s_.vertices[at];
      const Vertex& from = s_.vertices[vertex.pred];
      switch (vertex.edge) {
        case Edge::Unchanged:
          MarkSubtreesUnchanged(lhs_, from.lhs, rhs_, from.rhs, match);
          break;
        case Edge::EnterUnchanged: {
          const DiffSyntaxNode& a = lhs_.nodes[from.lhs];
          const DiffSyntaxNode& b = rhs_.nodes[from.rhs];
          match.lhs_partner[a.token_begin] = b.token_begin;
          match.rhs_partner[b.token_begin] = a.token_begin;
          match.lhs_partner[a.token_end - 1] = b.token_end - 1;
          match.rhs_partner[b.token_end - 1] = a.token_end - 1;
          break;
        }
        case Edge::Replaced: {
          // The edge cost encodes the similarity (kReplacedBaseCost + 100 - pct).
          const std::uint32_t similarity = kReplacedBaseCost + 100 - (vertex.dist - from.dist);
          if (similarity <= kMinReplacedSimilarityPct) {
            break;  // too different to read as an edit: both literals stay novel
          }
          const std::uint32_t a = lhs_.nodes[from.lhs].token_begin;
          const std::uint32_t b = rhs_.nodes[from.rhs].token_begin;
          match.lhs_partner[a] = b;
          match.rhs_partner[b] = a;
          match.lhs_replaced[a] = 1;
          match.rhs_replaced[b] = 1;
          break;
        }
        default:
          break;
      }
    }
    s_.ReleaseIfLarge();
    return true;
  }

 private:
  std::uint32_t NextLhs(std::uint32_t node) const { return Next(lhs_, node, lhs_scope_); }
  std::uint32_t NextRhs(std::uint32_t node) const { return Next(rhs_, node, rhs_scope_); }
  // The next sibling, with the scope's end read as the end of the list: the
  // search never walks past the run it was given.
  static std::uint32_t Next(const DiffSyntaxTree& tree, std::uint32_t node, SiblingScope scope) {
    const DiffSyntaxNode& entry = tree.nodes[node];
    if (entry.parent == scope.parent && entry.next_sibling == scope.end) {
      return kNoDiffNode;
    }
    return entry.next_sibling;
  }

  std::uint32_t LhsParent(std::uint32_t stack) const {
    return stack == kNoDiffNode ? kNoDiffNode : s_.groups[stack].lhs_parent;
  }
  std::uint32_t RhsParent(std::uint32_t stack) const {
    return stack == kNoDiffNode ? kNoDiffNode : s_.groups[stack].rhs_parent;
  }

  // The vertex identity. Like difftastic, two vertices with different stacks
  // but the same position, innermost parents and pop-either state are the
  // same vertex: the cheaper path to it wins and its stack is the one kept.
  // Comparing whole stacks would multiply the graph by the nesting depth.
  struct Key {
    std::uint32_t lhs, rhs, lhs_parent, rhs_parent;
    bool can_pop_either;
    bool operator==(const Key&) const = default;
  };
  Key KeyOf(std::uint32_t lhs, std::uint32_t rhs, std::uint32_t stack) const {
    return Key{lhs, rhs, LhsParent(stack), RhsParent(stack),
               stack != kNoDiffNode && !s_.groups[stack].both};
  }
  // murmur3's 64-bit finalizer: the table masks the LOW bits, and a plain
  // multiply chain leaves those depending only on the inputs' low bits, which
  // clustered the linear probing badly.
  static std::uint64_t Mix(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
  }
  static std::size_t HashKey(const Key& key) {
    const std::uint64_t position = (std::uint64_t{key.lhs} << 32) | key.rhs;
    const std::uint64_t parents = (std::uint64_t{key.lhs_parent} << 32) | key.rhs_parent;
    return static_cast<std::size_t>(Mix(position ^ Mix(parents + (key.can_pop_either ? 1 : 0))));
  }

  static bool SlotMatches(const SearchScratch::Slot& slot, const Key& key) {
    return slot.lhs == key.lhs && slot.rhs == key.rhs && slot.lhs_parent == key.lhs_parent &&
           slot.rhs_parent == key.rhs_parent && slot.can_pop_either == key.can_pop_either;
  }

  // Finds the vertex with this identity, or creates it (undiscovered).
  std::uint32_t Insert(std::uint32_t lhs, std::uint32_t rhs, std::uint32_t stack) {
    if ((s_.vertices.size() + 1) * 2 > s_.slots.size()) {
      Rehash(s_.slots.size() * 2);
    }
    const Key key = KeyOf(lhs, rhs, stack);
    const std::size_t mask = s_.slots.size() - 1;
    std::size_t index = HashKey(key) & mask;
    while (s_.slots[index].vertex != kNoDiffNode) {
      if (SlotMatches(s_.slots[index], key)) {
        return s_.slots[index].vertex;
      }
      index = (index + 1) & mask;
    }
    const auto vertex = static_cast<std::uint32_t>(s_.vertices.size());
    s_.vertices.push_back(Vertex{.lhs = lhs, .rhs = rhs, .stack = stack,
                                 .dist = std::numeric_limits<std::uint32_t>::max()});
    s_.slots[index] = SearchScratch::Slot{.lhs = key.lhs,
                                          .rhs = key.rhs,
                                          .lhs_parent = key.lhs_parent,
                                          .rhs_parent = key.rhs_parent,
                                          .vertex = vertex,
                                          .can_pop_either = key.can_pop_either};
    return vertex;
  }

  void Rehash(std::size_t size) {
    std::vector<SearchScratch::Slot> old(size, SearchScratch::Slot{});
    old.swap(s_.slots);
    const std::size_t mask = size - 1;
    for (const SearchScratch::Slot& slot : old) {
      if (slot.vertex == kNoDiffNode) {
        continue;
      }
      const Key key{slot.lhs, slot.rhs, slot.lhs_parent, slot.rhs_parent, slot.can_pop_either};
      std::size_t index = HashKey(key) & mask;
      while (s_.slots[index].vertex != kNoDiffNode) {
        index = (index + 1) & mask;
      }
      s_.slots[index] = slot;
    }
  }

  // LiteralSimilarityPct, memoized per token pair for this search.
  std::uint32_t Similarity(std::uint32_t lhs_token, std::uint32_t rhs_token) {
    const std::uint64_t key = (std::uint64_t{lhs_token} << 32) | rhs_token;
    if (s_.similarity_cache.empty()) {
      s_.similarity_cache.assign(kInitialSlots, {~std::uint64_t{0}, 0});
    }
    if ((s_.similarity_cache_used + 1) * 2 > s_.similarity_cache.size()) {
      std::vector<std::pair<std::uint64_t, std::uint32_t>> old(s_.similarity_cache.size() * 2,
                                                               {~std::uint64_t{0}, 0});
      old.swap(s_.similarity_cache);
      for (const auto& entry : old) {
        if (entry.first != ~std::uint64_t{0}) {
          StoreSimilarity(entry.first, entry.second);
        }
      }
    }
    const std::size_t mask = s_.similarity_cache.size() - 1;
    for (std::size_t index = HashKey(Key{lhs_token, rhs_token, 0, 0, false}) & mask;;
         index = (index + 1) & mask) {
      const auto& entry = s_.similarity_cache[index];
      if (entry.first == key) {
        return entry.second;
      }
      if (entry.first == ~std::uint64_t{0}) {
        break;
      }
    }
    const std::uint32_t similarity =
        LiteralSimilarityPct(lhs_.tokens[lhs_token].text, rhs_.tokens[rhs_token].text);
    StoreSimilarity(key, similarity);
    ++s_.similarity_cache_used;
    return similarity;
  }

  void StoreSimilarity(std::uint64_t key, std::uint32_t similarity) {
    const std::size_t mask = s_.similarity_cache.size() - 1;
    std::size_t index =
        HashKey(Key{static_cast<std::uint32_t>(key >> 32), static_cast<std::uint32_t>(key), 0, 0,
                    false}) &
        mask;
    while (s_.similarity_cache[index].first != ~std::uint64_t{0}) {
      index = (index + 1) & mask;
    }
    s_.similarity_cache[index] = {key, similarity};
  }

  void Push(std::uint32_t vertex, std::uint32_t dist, std::uint32_t priority) {
    std::vector<QueueEntry>& bucket = s_.buckets[priority % kBucketCount];
    if (bucket.empty()) {
      s_.touched_buckets.push_back(priority % kBucketCount);
    }
    bucket.push_back(QueueEntry{vertex, dist, priority});
    ++pending_;
  }

  // The preorder index one past the scope: where "the rest of this side" ends.
  static std::uint32_t ScopeEnd(const DiffSyntaxTree& tree, SiblingScope scope) {
    if (scope.end != kNoDiffNode) {
      return scope.end;
    }
    return scope.parent != kNoDiffNode ? tree.nodes[scope.parent].subtree_end
                                       : static_cast<std::uint32_t>(tree.nodes.size());
  }

  // How many nodes one side still has to consume: preorder is contiguous, so it
  // is the distance from the next node to process to the end of the scope.
  // With no next node the side has finished its innermost entered list, and
  // continues after that list's subtree.
  static std::uint32_t Remaining(const DiffSyntaxTree& tree, std::uint32_t node,
                                 std::uint32_t parent, std::uint32_t scope_end) {
    const std::uint32_t position =
        node != kNoDiffNode ? node
                            : (parent != kNoDiffNode ? tree.nodes[parent].subtree_end : scope_end);
    return scope_end > position ? scope_end - position : 0;
  }

  // A lower bound on the cost still to pay. Every edge but a novel one consumes
  // the same number of nodes on both sides (an unchanged subtree is the same
  // size on each), and a novel edge consumes one node on one side at
  // kNovelCost. So the difference in what the two sides have left can only be
  // closed by that many novel edges: admissible, and consistent (an edge
  // changes h by at most its own cost), which keeps A* exact.
  std::uint32_t Heuristic(std::uint32_t lhs, std::uint32_t rhs, std::uint32_t stack) const {
    const std::uint32_t lhs_left = Remaining(lhs_, lhs, LhsParent(stack), lhs_scope_end_);
    const std::uint32_t rhs_left = Remaining(rhs_, rhs, RhsParent(stack), rhs_scope_end_);
    const std::uint32_t difference = lhs_left > rhs_left ? lhs_left - rhs_left : rhs_left - lhs_left;
    return difference * kNovelCost;
  }

  void Relax(std::uint32_t from, std::uint32_t lhs, std::uint32_t rhs, std::uint32_t stack,
             Edge edge, std::uint32_t cost) {
    const std::uint32_t dist = s_.vertices[from].dist + cost;
    const std::uint32_t index = Insert(lhs, rhs, stack);
    Vertex& vertex = s_.vertices[index];
    if (vertex.done || vertex.dist <= dist) {
      return;
    }
    vertex.dist = dist;
    vertex.pred = from;
    vertex.edge = edge;
    vertex.stack = stack;
    Push(index, dist, dist + Heuristic(lhs, rhs, stack));
  }

  std::uint32_t PushBoth(std::uint32_t stack, std::uint32_t lhs, std::uint32_t rhs) {
    s_.groups.push_back(StackGroup{.prev = stack, .lhs = lhs, .rhs = rhs, .lhs_parent = lhs,
                                   .rhs_parent = rhs, .both = true});
    return static_cast<std::uint32_t>(s_.groups.size() - 1);
  }

  // Enters a one-sided delimiter on the LHS (`lhs_side`) or the RHS.
  std::uint32_t PushOne(std::uint32_t stack, std::uint32_t node, bool lhs_side) {
    const bool extend = stack != kNoDiffNode && !s_.groups[stack].both;
    StackGroup group;
    if (extend) {
      group = s_.groups[stack];
    } else {
      group.prev = stack;
      group.lhs_parent = LhsParent(stack);
      group.rhs_parent = RhsParent(stack);
    }
    s_.conses.push_back(Cons{node, lhs_side ? group.lhs : group.rhs});
    const auto cons = static_cast<std::uint32_t>(s_.conses.size() - 1);
    if (lhs_side) {
      group.lhs = cons;
      group.lhs_parent = node;
    } else {
      group.rhs = cons;
      group.rhs_parent = node;
    }
    group.both = false;
    s_.groups.push_back(group);
    return static_cast<std::uint32_t>(s_.groups.size() - 1);
  }

  // Exits the innermost one-sided delimiter on one side. Returns the new stack
  // and the exited list node.
  std::uint32_t PopOne(std::uint32_t stack, bool lhs_side, std::uint32_t& exited) {
    StackGroup group = s_.groups[stack];
    const std::uint32_t head = lhs_side ? group.lhs : group.rhs;
    exited = s_.conses[head].node;
    const std::uint32_t rest = s_.conses[head].next;
    (lhs_side ? group.lhs : group.rhs) = rest;
    if (group.lhs == kNoDiffNode && group.rhs == kNoDiffNode) {
      return group.prev;
    }
    const std::uint32_t outer = lhs_side ? LhsParent(group.prev) : RhsParent(group.prev);
    (lhs_side ? group.lhs_parent : group.rhs_parent) =
        rest != kNoDiffNode ? s_.conses[rest].node : outer;
    s_.groups.push_back(group);
    return static_cast<std::uint32_t>(s_.groups.size() - 1);
  }

  void Expand(std::uint32_t from) {
    const Vertex vertex = s_.vertices[from];
    const std::uint32_t stack = vertex.stack;
    const StackGroup* group = stack == kNoDiffNode ? nullptr : &s_.groups[stack];

    if (vertex.lhs == kNoDiffNode && vertex.rhs == kNoDiffNode && group != nullptr &&
        group->both) {
      Relax(from, NextLhs(group->lhs), NextRhs(group->rhs), group->prev, Edge::ExitBoth, 0);
    }
    if (vertex.lhs == kNoDiffNode && group != nullptr && !group->both &&
        group->lhs != kNoDiffNode) {
      std::uint32_t exited = kNoDiffNode;
      const std::uint32_t next_stack = PopOne(stack, true, exited);
      Relax(from, NextLhs(exited), vertex.rhs, next_stack, Edge::ExitLhs, 0);
    }
    // `group` may dangle after PopOne grew the arena; re-read.
    group = stack == kNoDiffNode ? nullptr : &s_.groups[stack];
    if (vertex.rhs == kNoDiffNode && group != nullptr && !group->both &&
        group->rhs != kNoDiffNode) {
      std::uint32_t exited = kNoDiffNode;
      const std::uint32_t next_stack = PopOne(stack, false, exited);
      Relax(from, vertex.lhs, NextRhs(exited), next_stack, Edge::ExitRhs, 0);
    }

    if (vertex.lhs != kNoDiffNode && vertex.rhs != kNoDiffNode) {
      const DiffSyntaxNode& a = lhs_.nodes[vertex.lhs];
      const DiffSyntaxNode& b = rhs_.nodes[vertex.rhs];
      const std::uint32_t depth_difference = DepthDifference(a, b);
      if (a.content_id == b.content_id) {
        const std::uint32_t cost = 1 + depth_difference +
                                   (IsSeparatorAtom(lhs_, a) ? kSeparatorPenalty : 0);
        Relax(from, NextLhs(vertex.lhs), NextRhs(vertex.rhs), stack, Edge::Unchanged, cost);
        // An equal subtree is best crossed whole; entering it can only match
        // the same tokens at a higher price.
        return;
      }
      if (a.is_list && b.is_list && lhs_.OpenToken(a).text == rhs_.OpenToken(b).text &&
          lhs_.CloseToken(a).text == rhs_.CloseToken(b).text) {
        const std::uint32_t next_stack = PushBoth(stack, vertex.lhs, vertex.rhs);
        Relax(from, a.first_child, b.first_child, next_stack, Edge::EnterUnchanged,
              kEnterUnchangedCost + depth_difference);
      }
      if (!a.is_list && !b.is_list) {
        const DiffToken& a_token = lhs_.OpenToken(a);
        const DiffToken& b_token = rhs_.OpenToken(b);
        if (IsLiteral(a_token) && a_token.kind == b_token.kind) {
          const std::uint32_t similarity = Similarity(a.token_begin, b.token_begin);
          Relax(from, NextLhs(vertex.lhs), NextRhs(vertex.rhs), stack, Edge::Replaced,
                kReplacedBaseCost + (100 - similarity));
        }
      }
    }

    if (vertex.lhs != kNoDiffNode) {
      const DiffSyntaxNode& a = lhs_.nodes[vertex.lhs];
      if (a.is_list) {
        const std::uint32_t next_stack = PushOne(stack, vertex.lhs, true);
        Relax(from, a.first_child, vertex.rhs, next_stack, Edge::EnterNovelLhs, kNovelCost);
      } else {
        Relax(from, NextLhs(vertex.lhs), vertex.rhs, stack, Edge::NovelLhs, kNovelCost);
      }
    }
    if (vertex.rhs != kNoDiffNode) {
      const DiffSyntaxNode& b = rhs_.nodes[vertex.rhs];
      if (b.is_list) {
        const std::uint32_t next_stack = PushOne(stack, vertex.rhs, false);
        Relax(from, vertex.lhs, b.first_child, next_stack, Edge::EnterNovelRhs, kNovelCost);
      } else {
        Relax(from, vertex.lhs, NextRhs(vertex.rhs), stack, Edge::NovelRhs, kNovelCost);
      }
    }
  }

  const DiffSyntaxTree& lhs_;
  const DiffSyntaxTree& rhs_;
  SiblingScope lhs_scope_;
  SiblingScope rhs_scope_;
  std::size_t budget_;
  SearchScratch& s_;
  std::size_t pending_ = 0;
  std::uint32_t lhs_scope_end_;
  std::uint32_t rhs_scope_end_;
};


}  // namespace

bool IsPunctuationAtom(const DiffSyntaxTree& tree, const DiffSyntaxNode& node) {
  if (node.is_list) {
    return false;
  }
  const DiffToken& token = tree.OpenToken(node);
  return token.kind == DiffTokenKind::Punctuation && token.text.size() == 1;
}

bool IsLiteral(const DiffToken& token) {
  return token.kind == DiffTokenKind::String || token.kind == DiffTokenKind::Comment;
}

void MarkSubtreesUnchanged(const DiffSyntaxTree& lhs, std::uint32_t lhs_node,
                           const DiffSyntaxTree& rhs, std::uint32_t rhs_node,
                           StructuralMatch& match) {
  const DiffSyntaxNode& a = lhs.nodes[lhs_node];
  const DiffSyntaxNode& b = rhs.nodes[rhs_node];
  // Equal content ids mean the same token sequence, so the ranges are the same
  // length and line up one to one.
  const std::uint32_t count = a.token_end - a.token_begin;
  for (std::uint32_t offset = 0; offset < count; ++offset) {
    match.lhs_partner[a.token_begin + offset] = b.token_begin + offset;
    match.rhs_partner[b.token_begin + offset] = a.token_begin + offset;
  }
}

bool SearchGap(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, SiblingScope lhs_scope,
               SiblingScope rhs_scope, std::uint32_t lhs_first, std::uint32_t rhs_first,
               std::size_t budget, StructuralMatch& match, std::size_t& vertices) {
  return Search(lhs, rhs, lhs_scope, rhs_scope, budget).Run(lhs_first, rhs_first, match, vertices);
}

}  // namespace microide::compare::detail
