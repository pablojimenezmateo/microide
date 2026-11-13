#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "compare/StructuralDiff.h"

// Slider correction (TD-2026-11-11-442), difftastic's `fix_all_sliders` in this
// tree's terms. A novel run of nodes can often sit at several positions at the
// same cost -- inserting an item into a list whose neighbours are equal, or a
// block whose closing brace equals the next block's -- and the search's
// tie-break picks one deterministically but not the one a reader expects. This
// pass moves novelty only between EQUAL nodes, so which tokens are unchanged is
// the same set of texts on each side; the line diff, the hunks and the row
// alignment never read it.
namespace microide::compare {
namespace {

constexpr std::uint8_t kNovel = 0;
constexpr std::uint8_t kWhole = 1;  // every token partnered, unreplaced, onto one equal subtree
constexpr std::uint8_t kMixed = 2;

struct Side {
  const DiffSyntaxTree& tree;
  std::vector<std::uint32_t>& partner;
  const std::vector<std::uint8_t>& replaced;
  const DiffSyntaxTree& other;
  std::vector<std::uint32_t>& other_partner;
};

struct SliderScratch {
  std::vector<std::uint32_t> other_node_at_token;  // node of `other` whose token_begin is the token
  std::vector<std::uint8_t> state;
  std::vector<std::uint32_t> run;
};

SliderScratch& Scratch() {
  thread_local SliderScratch scratch;
  return scratch;
}

std::uint8_t Classify(const Side& side, const std::vector<std::uint32_t>& other_node_at_token,
                      std::uint32_t node_index) {
  const DiffSyntaxNode& node = side.tree.nodes[node_index];
  bool any = false;
  bool all = true;
  for (std::uint32_t token = node.token_begin; token < node.token_end; ++token) {
    const bool partnered = side.partner[token] != kNoDiffNode && side.replaced[token] == 0;
    any = any || side.partner[token] != kNoDiffNode;
    all = all && partnered;
  }
  if (!any) {
    return kNovel;
  }
  if (!all) {
    return kMixed;
  }
  // The partners must be exactly one equal subtree on the other side, in order.
  const std::uint32_t first = side.partner[node.token_begin];
  const std::uint32_t other_index = other_node_at_token[first];
  if (other_index == kNoDiffNode) {
    return kMixed;
  }
  const DiffSyntaxNode& other = side.other.nodes[other_index];
  if (other.content_id != node.content_id) {
    return kMixed;
  }
  for (std::uint32_t offset = 0; offset < node.token_end - node.token_begin; ++offset) {
    if (side.partner[node.token_begin + offset] != other.token_begin + offset) {
      return kMixed;
    }
  }
  return kWhole;
}

// Gives `from`'s whole-subtree partner to `to` (an equal subtree), leaving
// `from` novel.
void MovePartner(const Side& side, std::uint32_t from, std::uint32_t to) {
  const DiffSyntaxNode& a = side.tree.nodes[from];
  const DiffSyntaxNode& b = side.tree.nodes[to];
  for (std::uint32_t offset = 0; offset < a.token_end - a.token_begin; ++offset) {
    const std::uint32_t other_token = side.partner[a.token_begin + offset];
    side.partner[a.token_begin + offset] = kNoDiffNode;
    side.partner[b.token_begin + offset] = other_token;
    side.other_partner[other_token] = b.token_begin + offset;
  }
}

bool StartsLine(const DiffSyntaxTree& tree, std::uint32_t token) {
  return token == 0 || tree.tokens[token - 1].end_line != tree.tokens[token].line;
}

bool EndsLine(const DiffSyntaxTree& tree, std::uint32_t token) {
  return token + 1 >= tree.tokens.size() || tree.tokens[token + 1].line != tree.tokens[token].end_line;
}

// One sibling run: every maximal novel stretch slides to its best equivalent
// position. Positions are equivalent when the node stepped over equals the one
// that takes its place: sliding up past matched `run[s-1]` needs it equal to
// `run[e-1]`; down past matched `run[e]` needs it equal to `run[s]`.
void FixSiblingRun(const Side& side, std::vector<std::uint32_t>& run, std::vector<std::uint8_t>& state) {
  const DiffSyntaxTree& tree = side.tree;
  const auto id = [&](std::size_t index) { return tree.nodes[run[index]].content_id; };
  std::size_t index = 0;
  while (index < run.size()) {
    if (state[index] != kNovel) {
      ++index;
      continue;
    }
    std::size_t s = index;
    std::size_t e = index;
    while (e < run.size() && state[e] == kNovel) {
      ++e;
    }
    const std::size_t length = e - s;
    // The reach in each direction.
    std::size_t up = 0;
    while (s >= up + 1 && state[s - up - 1] == kWhole && id(s - up - 1) == id(e - up - 1)) {
      ++up;
    }
    std::size_t down = 0;
    while (e + down < run.size() && state[e + down] == kWhole && id(e + down) == id(s + down)) {
      ++down;
    }
    if (up == 0 && down == 0) {
      index = e;
      continue;
    }
    // Rank each start position, best first: a stretch that is whole lines
    // (reads as lines added); then one touching fewer lines (novelty straddling
    // a line break when it need not); then one ending its line; then the
    // latest position (an item appended after its separator, a new block's own
    // closing brace -- git's "slide down" for the same reason).
    std::size_t best = s;
    int best_score = std::numeric_limits<int>::min();
    for (std::size_t start = s - up; start <= s + down; ++start) {
      const std::uint32_t first_token = tree.nodes[run[start]].token_begin;
      const std::uint32_t last_token = tree.nodes[run[start + length - 1]].token_end - 1;
      const bool starts = StartsLine(tree, first_token);
      const bool ends = EndsLine(tree, last_token);
      const int lines = static_cast<int>(tree.tokens[last_token].end_line - tree.tokens[first_token].line);
      const int score = (starts && ends ? 1 << 20 : 0) - lines * 4 + (ends ? 2 : 0);
      if (score >= best_score) {
        best_score = score;
        best = start;
      }
    }
    // Slide one node at a time so every intermediate state is a valid match.
    std::size_t at = s;
    while (at > best) {
      MovePartner(side, run[at - 1], run[at - 1 + length]);
      state[at - 1 + length] = kWhole;
      state[at - 1] = kNovel;
      --at;
    }
    while (at < best) {
      MovePartner(side, run[at + length], run[at]);
      state[at] = kWhole;
      state[at + length] = kNovel;
      ++at;
    }
    index = std::max(e, best + length);
  }
}

// A novel list wrapping exactly one matched list with the same delimiters: the
// novelty belongs to the inner pair (the reader's "a new block around this"),
// not to the outer one that shares its lines with the unchanged context.
void FixNestedSliders(const Side& side) {
  const DiffSyntaxTree& tree = side.tree;
  for (std::uint32_t index = 0; index < tree.nodes.size(); ++index) {
    const DiffSyntaxNode& outer = tree.nodes[index];
    if (!outer.is_list || outer.first_child == kNoDiffNode) {
      continue;
    }
    const DiffSyntaxNode& inner = tree.nodes[outer.first_child];
    if (!inner.is_list || inner.next_sibling != kNoDiffNode) {
      continue;
    }
    const std::uint32_t outer_open = outer.token_begin;
    const std::uint32_t outer_close = outer.token_end - 1;
    const std::uint32_t inner_open = inner.token_begin;
    const std::uint32_t inner_close = inner.token_end - 1;
    if (side.partner[outer_open] != kNoDiffNode || side.partner[outer_close] != kNoDiffNode ||
        side.partner[inner_open] == kNoDiffNode || side.partner[inner_close] == kNoDiffNode ||
        side.replaced[inner_open] != 0 || side.replaced[inner_close] != 0 ||
        tree.tokens[outer_open].text != tree.tokens[inner_open].text ||
        tree.tokens[outer_close].text != tree.tokens[inner_close].text) {
      continue;
    }
    // A valid swap: nothing sits between the two opens or between the two
    // closes, so the partners stay in order.
    const std::uint32_t open_partner = side.partner[inner_open];
    const std::uint32_t close_partner = side.partner[inner_close];
    side.partner[outer_open] = open_partner;
    side.partner[outer_close] = close_partner;
    side.partner[inner_open] = kNoDiffNode;
    side.partner[inner_close] = kNoDiffNode;
    side.other_partner[open_partner] = outer_open;
    side.other_partner[close_partner] = outer_close;
  }
}

void FixSide(const Side& side) {
  SliderScratch& scratch = Scratch();
  // Nested first: it only swaps delimiter pairs, and the sibling pass then sees
  // the final novelty of every list.
  FixNestedSliders(side);
  scratch.other_node_at_token.assign(side.other.tokens.size(), kNoDiffNode);
  for (std::uint32_t index = 0; index < side.other.nodes.size(); ++index) {
    scratch.other_node_at_token[side.other.nodes[index].token_begin] = index;
  }
  const auto fix_run = [&](std::uint32_t first) {
    scratch.run.clear();
    for (std::uint32_t node = first; node != kNoDiffNode; node = side.tree.nodes[node].next_sibling) {
      scratch.run.push_back(node);
    }
    if (scratch.run.size() < 2) {
      return;
    }
    scratch.state.resize(scratch.run.size());
    bool novel = false;
    for (std::size_t index = 0; index < scratch.run.size(); ++index) {
      scratch.state[index] = Classify(side, scratch.other_node_at_token, scratch.run[index]);
      novel = novel || scratch.state[index] == kNovel;
    }
    if (novel) {
      FixSiblingRun(side, scratch.run, scratch.state);
    }
  };
  fix_run(side.tree.first_root);
  for (const DiffSyntaxNode& node : side.tree.nodes) {
    if (node.is_list && node.first_child != kNoDiffNode) {
      fix_run(node.first_child);
    }
  }
}

}  // namespace

void FixStructuralSliders(const DiffSyntaxTree& lhs, const DiffSyntaxTree& rhs, StructuralMatch& match) {
  FixSide(Side{lhs, match.lhs_partner, match.lhs_replaced, rhs, match.rhs_partner});
  FixSide(Side{rhs, match.rhs_partner, match.rhs_replaced, lhs, match.lhs_partner});
}

}  // namespace microide::compare
