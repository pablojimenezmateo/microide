#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "util/Revision.h"

namespace microide::editor {

struct FunctionBreakpoint {
  std::string name;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;

  bool verified = false;
  int adapter_id = 0;
  std::string verify_message;

  bool operator==(const FunctionBreakpoint& other) const {
    return name == other.name && enabled == other.enabled && condition == other.condition &&
           hit_condition == other.hit_condition;
  }
};

struct VerifiedFunctionBreakpoint {
  int id = 0;
  bool verified = false;
  std::string message;
};

class FunctionBreakpointStore {
 public:
  bool Add(std::string name);
  void Remove(std::size_t index);
  bool ToggleEnabled(std::size_t index);
  void SetCondition(std::size_t index, std::optional<std::string> condition);
  void SetHitCondition(std::size_t index, std::optional<std::string> hit_condition);

  bool Empty() const { return breakpoints_.empty(); }
  std::size_t Count() const { return breakpoints_.size(); }
  const std::vector<FunctionBreakpoint>& All() const { return breakpoints_; }
  bool HasName(const std::string& name) const;

  void ApplyVerification(const std::vector<std::string>& requested_names,
                         const std::vector<VerifiedFunctionBreakpoint>& results);
  bool ApplyBreakpointEvent(const VerifiedFunctionBreakpoint& result);
  void ResetVerification();

  void ReplaceAll(std::vector<FunctionBreakpoint> breakpoints);

  std::uint64_t revision() const { return revision_.value(); }

 private:

  std::vector<FunctionBreakpoint> breakpoints_;
  util::Revision revision_;
};

}
