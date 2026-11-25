#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "terminal/TerminalCell.h"

namespace microide::terminal {

struct TerminalSearchMatch {
  std::size_t row = 0;
  std::size_t column = 0;
  std::size_t length = 0;
};

struct TerminalSearchQuery {
  std::string needle;
  bool case_sensitive = false;
  bool whole_word = false;

  bool empty() const { return needle.empty(); }
};

TerminalSearchQuery MakeTerminalSearchQuery(std::string_view text,
                                            bool case_sensitive,
                                            bool whole_word);

struct TerminalSearchScratch {
  std::string text;
  std::vector<std::uint32_t> columns;
};

void FlattenTerminalLine(const TerminalLine& line, bool fold_case, TerminalSearchScratch& scratch);

bool FindTerminalLineMatches(const TerminalLine& line,
                             const TerminalSearchQuery& query,
                             std::size_t row,
                             std::size_t max_matches,
                             TerminalSearchScratch& scratch,
                             std::vector<TerminalSearchMatch>& out);

}
