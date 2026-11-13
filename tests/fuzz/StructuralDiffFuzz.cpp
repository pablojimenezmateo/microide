#include "compare/CompareModel.h"
#include "compare/DiffSyntax.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Fuzz the structural diff end to end: the lexer (raw strings, here-documents,
// multi-line comments and strings), the lexical-state scan that seeds each hunk,
// the structural match with its slider pass, and the model build over them.
//
// Input: byte 0 picks the language rules, byte 1 the line the scan stops at;
// the rest is the left text, a NUL, then the right text.
//
// Checked: no crash or UB (ASan); the model's rows cover every line of each
// side once, in order; and a region lexed from the scanned state yields the
// tokens a lex of the whole text yields from that line on.
namespace {

using microide::compare::DiffContentInterner;
using microide::compare::DiffLexState;
using microide::compare::DiffSyntax;
using microide::compare::DiffSyntaxTree;
using microide::compare::DiffTokenKind;

std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::shared_ptr<const DiffSyntax> Syntax(std::uint8_t selector) {
  auto syntax = std::make_shared<DiffSyntax>();
  syntax->delimiters = {{"(", ")"}, {"[", "]"}, {"{", "}"}};
  if ((selector & 1) != 0) {
    syntax->line_comments = {"//"};
    syntax->block_comments = {{"/*", "*/"}};
  } else {
    syntax->line_comments = {"#"};
  }
  syntax->raw_strings = static_cast<std::uint8_t>(selector >> 1) & 0x0F;
  return syntax;
}

bool Kinds(DiffTokenKind a, DiffTokenKind b) {
  const auto cls = [](DiffTokenKind kind) {
    return kind == DiffTokenKind::Open || kind == DiffTokenKind::Close ? DiffTokenKind::Punctuation
                                                                       : kind;
  };
  return cls(a) == cls(b);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size < 2 || size > 16 * 1024) {
    return 0;
  }
  const std::shared_ptr<const DiffSyntax> syntax = Syntax(data[0]);
  const std::string_view body(reinterpret_cast<const char*>(data + 2), size - 2);
  const std::size_t nul = body.find('\0');
  const std::string left(body.substr(0, nul));
  const std::string right(nul == std::string_view::npos ? std::string_view{} : body.substr(nul + 1));

  // The model: rows cover each side's lines once, in order.
  microide::compare::CompareBuildOptions options;
  options.syntax = syntax;
  const microide::compare::CompareModel model =
      microide::compare::BuildCompareModel(left, right, options);
  int next_left = 1;
  int next_right = 1;
  for (const auto& row : model.rows) {
    if (row.left_line != 0 && row.left_line != next_left++) {
      std::abort();
    }
    if (row.right_line != 0 && row.right_line != next_right++) {
      std::abort();
    }
  }

  // The scan agrees with a whole lex from the line it stops at.
  const std::vector<std::string_view> lines = Lines(left);
  if (lines.size() < 2) {
    return 0;
  }
  const std::size_t split = 1 + data[1] % (lines.size() - 1);
  DiffContentInterner whole_interner;
  DiffSyntaxTree whole;
  microide::compare::BuildDiffSyntaxTree(lines, *syntax, whole_interner, whole);
  const DiffLexState entry = microide::compare::ScanDiffLexState(lines, split, *syntax, {});
  const auto below = std::span<const std::string_view>(lines).subspan(split);
  DiffContentInterner region_interner;
  DiffSyntaxTree region;
  microide::compare::BuildDiffSyntaxTree(below, *syntax, region_interner, region, entry);
  std::size_t at = 0;
  for (const auto& token : whole.tokens) {
    if (token.end_line < split) {
      continue;
    }
    const char* begin = token.line >= split ? token.text.data() : below.front().data();
    const std::string_view rest(begin,
                                static_cast<std::size_t>(token.text.data() + token.text.size() - begin));
    if (rest.empty()) {
      continue;
    }
    if (at >= region.tokens.size() || region.tokens[at].text != rest ||
        !Kinds(region.tokens[at].kind, token.kind)) {
      std::abort();
    }
    ++at;
  }
  if (at != region.tokens.size()) {
    std::abort();
  }
  return 0;
}
