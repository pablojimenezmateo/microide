#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::project {

class PendingPaths {
 public:
  struct Child {
    std::string_view name;
    bool is_directory = false;
  };

  explicit PendingPaths(std::vector<std::string> files);
  PendingPaths(const PendingPaths&) = delete;
  PendingPaths& operator=(const PendingPaths&) = delete;

  const std::vector<std::string>& files() const { return files_; }
  bool empty() const { return files_.empty(); }
  std::span<const Child> ChildrenOf(std::string_view directory) const;

 private:
  std::vector<std::string> files_;
  std::unordered_map<std::string, std::vector<Child>, util::TransparentStringHash, std::equal_to<>>
      children_;
};

using SharedPendingPaths = std::shared_ptr<const PendingPaths>;

}
