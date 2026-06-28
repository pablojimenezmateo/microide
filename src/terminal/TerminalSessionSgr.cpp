#include "terminal/TerminalSessionEscapeInternal.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalCsiParser.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

namespace microide::terminal {

namespace {

Uint8 ClampColorComponent(int value) {
  return static_cast<Uint8>(std::clamp(value, 0, 255));
}

// Decode an extended SGR color (codes 38/48/58) from the parameter groups,
// supporting both the legacy semicolon form (`38;5;n`, `38;2;r;g;b`) and the
// ITU T.416 colon form (`38:5:n`, `38:2:r:g:b`, `38:2::r:g:b`). `gi` is advanced
// past any trailing groups consumed by the legacy form.
std::optional<SDL_Color> ParseExtendedSgrColor(const std::vector<std::vector<int>>& groups,
                                               std::size_t& gi) {
  const std::vector<int>& leading = groups[gi];
  std::vector<int> seq;
  if (leading.size() > 1) {
    seq.assign(leading.begin() + 1, leading.end());
  } else {
    if (gi + 1 >= groups.size()) {
      return std::nullopt;
    }
    const int space = groups[gi + 1].empty() ? 0 : groups[gi + 1].front();
    const std::size_t need = space == 2 ? 3 : space == 5 ? 1 : 0;
    ++gi;
    seq.push_back(space);
    for (std::size_t k = 0; k < need && gi + 1 < groups.size(); ++k) {
      ++gi;
      seq.push_back(groups[gi].empty() ? 0 : groups[gi].front());
    }
  }

  if (seq.empty()) {
    return std::nullopt;
  }
  const int space = seq.front();
  if (space == 5 && seq.size() >= 2) {
    return Ansi256Color(seq[1]);
  }
  if (space == 2 && seq.size() >= 4) {
    const std::size_t n = seq.size();
    return MakeTerminalRgbColor(ClampColorComponent(seq[n - 3]), ClampColorComponent(seq[n - 2]),
                                ClampColorComponent(seq[n - 1]));
  }
  return std::nullopt;
}

}  // namespace

void detail::ApplySgrParameters(TerminalStyle& style, std::string_view body) {
  // Reused across SGR sequences on the reader thread so a colored-output burst
  // does not churn the allocator with a fresh nested vector per escape.
  thread_local std::vector<std::vector<int>> groups;
  ParseSgrParametersInto(body, groups);
  for (std::size_t gi = 0; gi < groups.size(); ++gi) {
    const std::vector<int>& group = groups[gi];
    const int code = group.empty() ? 0 : group.front();
    switch (code) {
      case 0:
        style = TerminalStyle{};
        break;
      case 1:
        style.set(cell_attr::kBold, true);
        break;
      case 2:
        style.set(cell_attr::kDim, true);
        break;
      case 3:
        style.set(cell_attr::kItalic, true);
        break;
      case 4:
        if (group.size() > 1 && group[1] == 0) {
          style.set(cell_attr::kUnderline, false);
          style.set(cell_attr::kDoubleUnderline, false);
        } else if (group.size() > 1 && group[1] == 2) {
          style.set(cell_attr::kDoubleUnderline, true);
          style.set(cell_attr::kUnderline, false);
        } else {
          style.set(cell_attr::kUnderline, true);
          style.set(cell_attr::kDoubleUnderline, false);
        }
        break;
      case 5:
      case 6:
        style.set(cell_attr::kBlink, true);
        break;
      case 7:
        style.set(cell_attr::kInverse, true);
        break;
      case 8:
        style.set(cell_attr::kHidden, true);
        break;
      case 9:
        style.set(cell_attr::kStrikethrough, true);
        break;
      case 21:
        style.set(cell_attr::kDoubleUnderline, true);
        style.set(cell_attr::kUnderline, false);
        break;
      case 22:
        style.set(cell_attr::kBold, false);
        style.set(cell_attr::kDim, false);
        break;
      case 23:
        style.set(cell_attr::kItalic, false);
        break;
      case 24:
        style.set(cell_attr::kUnderline, false);
        style.set(cell_attr::kDoubleUnderline, false);
        break;
      case 25:
        style.set(cell_attr::kBlink, false);
        break;
      case 27:
        style.set(cell_attr::kInverse, false);
        break;
      case 28:
        style.set(cell_attr::kHidden, false);
        break;
      case 29:
        style.set(cell_attr::kStrikethrough, false);
        break;
      case 38:
        if (auto color = ParseExtendedSgrColor(groups, gi)) {
          style.foreground = *color;
        }
        break;
      case 39:
        style.foreground.reset();
        break;
      case 48:
        if (auto color = ParseExtendedSgrColor(groups, gi)) {
          style.background = *color;
        }
        break;
      case 49:
        style.background.reset();
        break;
      case 58:
        // Underline color: parse to consume parameters, then discard (the
        // renderer draws underlines in the foreground color).
        (void)ParseExtendedSgrColor(groups, gi);
        break;
      default:
        if (code >= 30 && code <= 37) {
          style.foreground = BasicAnsiColor(code - 30, style.bold());
        } else if (code >= 40 && code <= 47) {
          style.background = BasicAnsiColor(code - 40, false);
        } else if (code >= 90 && code <= 97) {
          style.foreground = BasicAnsiColor(code - 90, true);
        } else if (code >= 100 && code <= 107) {
          style.background = BasicAnsiColor(code - 100, true);
        }
        // Codes 53/55 (overline) and 59 (default underline color) are accepted
        // and ignored; they do not affect any tracked attribute.
        break;
    }
  }
}

}  // namespace microide::terminal
