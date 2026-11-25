#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace microide::project {

enum class GlobDoubleStar {
  SegmentAnchored,
  Always,
};

bool GlobMatches(std::string_view pattern, std::string_view text,
                 GlobDoubleStar double_star = GlobDoubleStar::SegmentAnchored);

inline constexpr std::size_t kMaxExpandedGlobPatterns = 256;

inline constexpr std::size_t kMaxBraceExpansionPatternBytes = 4096;

void ExpandGlobBraces(std::string pattern, std::vector<std::string>& out);

class GlobSet {
 public:
  GlobSet() = default;

  static GlobSet Parse(std::string_view comma_separated);

  bool empty() const { return patterns_.empty(); }
  std::size_t size() const { return patterns_.size(); }

  bool Matches(std::string_view normalized_relative_path) const;

  const std::vector<std::string>& patterns() const { return patterns_; }

 private:
  std::vector<std::string> patterns_;
};

}
