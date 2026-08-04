#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/FoldingModel.h"

namespace microide::tests {

// A deliberately naive, cache-free fold computation used as the ORACLE for the
// incremental model.
//
// The fold tests used to diff `FoldingModel` against `FoldingModel`: an
// incrementally updated instance against a freshly constructed one. That catches
// splice bugs, but it cannot catch anything wrong with the derivation itself,
// because both sides run it. This re-derives folds from the raw lines with no
// per-line caches, no block words and no windowing -- every structure the model
// uses to be fast is absent here, so a disagreement points straight at one of
// them.
//
// It deliberately does NOT model syntax suppression (brackets inside strings and
// comments); callers that want that compare against a model built with no syntax
// viewport, or assert on specific ranges instead.
inline std::vector<editor::FoldRange> ReferenceFolds(
    const std::vector<std::string>& lines,
    const editor::FoldingModel::ComputeOptions& options) {
  using editor::FoldRange;
  using editor::FoldSource;

  const std::size_t tab_size = options.tab_size == 0 ? 1 : options.tab_size;
  std::vector<FoldRange> out;

  // ---- bracket source: one stack, every byte of every line ----------------
  if (!options.bracket_pairs.empty()) {
    struct Open {
      char close;
      std::size_t line;
    };
    std::vector<Open> stack;
    for (std::size_t line = 0; line < lines.size(); ++line) {
      for (const char byte : lines[line]) {
        for (const auto& pair : options.bracket_pairs) {
          if (pair.first == pair.second) {
            continue;
          }
          if (byte == pair.first) {
            stack.push_back(Open{pair.second, line});
            break;
          }
          if (byte == pair.second) {
            // A closer that does not match the top is discarded, matching the
            // model: an unbalanced `}` must not tear down an unrelated pair.
            if (!stack.empty() && stack.back().close == byte) {
              const Open top = stack.back();
              stack.pop_back();
              if (line > top.line) {
                out.push_back(FoldRange{top.line, line, FoldSource::Bracket});
              }
            }
            break;
          }
        }
      }
    }
  }

  // ---- indent source: a level stack over the non-blank lines --------------
  if (options.use_indent_source) {
    const auto measure = [&](std::string_view text) -> std::size_t {
      std::size_t indent = 0;
      for (const char c : text) {
        if (c == ' ') {
          ++indent;
        } else if (c == '\t') {
          indent += tab_size - (indent % tab_size);
        } else {
          return indent;
        }
      }
      return static_cast<std::size_t>(-1);  // blank / whitespace only
    };
    struct Level {
      std::size_t indent;
      std::size_t line;
    };
    std::vector<Level> stack;
    std::size_t last_nonblank = static_cast<std::size_t>(-1);
    const auto close_to = [&](std::size_t indent) {
      while (!stack.empty() && stack.back().indent >= indent) {
        const Level top = stack.back();
        stack.pop_back();
        if (last_nonblank != static_cast<std::size_t>(-1) && last_nonblank > top.line) {
          out.push_back(FoldRange{top.line, last_nonblank, FoldSource::Indent});
        }
      }
    };
    for (std::size_t line = 0; line < lines.size(); ++line) {
      const std::size_t indent = measure(lines[line]);
      if (indent == static_cast<std::size_t>(-1)) {
        continue;  // blank lines neither open nor close a block
      }
      close_to(indent);
      stack.push_back(Level{indent, line});
      last_nonblank = line;
    }
    // End of document: every level still open genuinely ends at the last
    // non-blank line.
    while (!stack.empty()) {
      const Level top = stack.back();
      stack.pop_back();
      if (last_nonblank != static_cast<std::size_t>(-1) && last_nonblank > top.line) {
        out.push_back(FoldRange{top.line, last_nonblank, FoldSource::Indent});
      }
    }
  }

  // ---- one range per opener: bracket wins, then the widest ----------------
  std::sort(out.begin(), out.end(), [](const FoldRange& a, const FoldRange& b) {
    if (a.opener_line != b.opener_line) {
      return a.opener_line < b.opener_line;
    }
    if (a.source != b.source) {
      return static_cast<int>(a.source) < static_cast<int>(b.source);
    }
    return a.closer_line > b.closer_line;
  });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const FoldRange& a, const FoldRange& b) {
                          return a.opener_line == b.opener_line;
                        }),
            out.end());
  return out;
}

}  // namespace microide::tests
