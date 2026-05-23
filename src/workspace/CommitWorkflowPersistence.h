#pragma once

#include <string>

namespace microide::workspace {

struct PersistedCommitDraftState {
  std::string head_oid;
  std::string branch_name;
  std::string subject;
  std::string body;
};

}  // namespace microide::workspace
