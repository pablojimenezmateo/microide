#include "project/FileFinder.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace microide::project {

namespace {

bool RemoveLastUtf8Codepoint(std::string* text) {
  if (text == nullptr || text->empty()) {
    return false;
  }

  std::size_t index = text->size();
  do {
    --index;
  } while (index > 0 &&
           (static_cast<unsigned char>((*text)[index]) & 0xC0u) == 0x80u);
  text->erase(index);
  return true;
}

}  // namespace

void FileFinder::SetIndex(const FileIndex* index) {
  index_ = index;
  results_.clear();
  selected_index_ = 0;
  if (index_ != nullptr && !query_.empty()) {
    Refresh();
  }
}

void FileFinder::SetQuery(std::string query) {
  query_ = std::move(query);
  Refresh();
}

void FileFinder::AppendQueryChar(char character) {
  query_.push_back(character);
  Refresh();
}

void FileFinder::AppendQueryText(std::string_view text) {
  if (text.empty()) {
    return;
  }
  query_.append(text);
  Refresh();
}

void FileFinder::Backspace() {
  if (!RemoveLastUtf8Codepoint(&query_)) {
    return;
  }
  Refresh();
}

void FileFinder::Refresh() {
  results_.clear();
  selected_index_ = 0;

  if (index_ == nullptr) {
    return;
  }

  const std::string lower_query = ToLower(query_);
  for (const auto& path : index_->files()) {
    const std::string path_string = path.string();
    const int score = RankMatch(path_string, lower_query);
    if (score == std::numeric_limits<int>::max()) {
      continue;
    }
    results_.push_back(FileFinderResult{
        .relative_path = path,
        .score = score,
    });
  }

  std::sort(results_.begin(), results_.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score < rhs.score;
    }
    return lhs.relative_path.string() < rhs.relative_path.string();
  });
}

void FileFinder::MoveSelection(int delta) {
  if (results_.empty() || delta == 0) {
    return;
  }

  const int current = static_cast<int>(selected_index_);
  const int max_index = static_cast<int>(results_.size()) - 1;
  selected_index_ = static_cast<std::size_t>(std::clamp(current + delta, 0, max_index));
}

std::optional<std::filesystem::path> FileFinder::SelectedPath() const {
  if (results_.empty() || selected_index_ >= results_.size()) {
    return std::nullopt;
  }
  return results_[selected_index_].relative_path;
}

int FileFinder::SubsequenceScore(const std::string& text, const std::string& query) {
  if (query.empty()) {
    return 0;
  }

  int first_match = -1;
  int previous_index = -1;
  int total_gap = 0;

  for (char query_char : query) {
    bool matched = false;
    for (int i = previous_index + 1; i < static_cast<int>(text.size()); ++i) {
      if (text[static_cast<std::size_t>(i)] != query_char) {
        continue;
      }
      if (first_match < 0) {
        first_match = i;
      }
      if (previous_index >= 0) {
        total_gap += i - previous_index - 1;
      }
      previous_index = i;
      matched = true;
      break;
    }
    if (!matched) {
      return std::numeric_limits<int>::max();
    }
  }

  return total_gap + first_match;
}

int FileFinder::RankMatch(const std::string& path, const std::string& query) {
  if (query.empty()) {
    return static_cast<int>(path.size());
  }

  const std::string lower_path = ToLower(path);
  const std::string file_name = ToLower(std::filesystem::path(path).filename().string());

  const int path_score = SubsequenceScore(lower_path, query);
  const int file_score = SubsequenceScore(file_name, query);
  if (path_score == std::numeric_limits<int>::max() &&
      file_score == std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }

  int score = path_score == std::numeric_limits<int>::max()
                  ? std::numeric_limits<int>::max() / 2
                  : path_score * 3 + static_cast<int>(path.size());

  if (file_score != std::numeric_limits<int>::max()) {
    score = std::min(score, file_score - 20 + static_cast<int>(file_name.size()));
    if (file_name.rfind(query, 0) == 0) {
      score -= 30;
    }
  }

  return score;
}

std::string FileFinder::ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

}  // namespace microide::project
