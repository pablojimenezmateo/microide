#include "editor/FunctionBreakpointStore.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace microide::editor {

bool FunctionBreakpointStore::HasName(const std::string& name) const {
  return std::any_of(breakpoints_.begin(), breakpoints_.end(),
                     [&name](const FunctionBreakpoint& bp) { return bp.name == name; });
}

bool FunctionBreakpointStore::Add(std::string name) {
  if (name.empty() || HasName(name)) {
    return false;
  }
  breakpoints_.push_back(FunctionBreakpoint{.name = std::move(name)});
  revision_.Bump();
  return true;
}

void FunctionBreakpointStore::Remove(std::size_t index) {
  if (index >= breakpoints_.size()) {
    return;
  }
  breakpoints_.erase(breakpoints_.begin() + static_cast<std::ptrdiff_t>(index));
  revision_.Bump();
}

bool FunctionBreakpointStore::ToggleEnabled(std::size_t index) {
  if (index >= breakpoints_.size()) {
    return false;
  }
  breakpoints_[index].enabled = !breakpoints_[index].enabled;
  revision_.Bump();
  return true;
}

void FunctionBreakpointStore::SetCondition(std::size_t index, std::optional<std::string> condition) {
  if (index >= breakpoints_.size() || breakpoints_[index].condition == condition) {
    return;
  }
  breakpoints_[index].condition = std::move(condition);
  revision_.Bump();
}

void FunctionBreakpointStore::SetHitCondition(std::size_t index,
                                              std::optional<std::string> hit_condition) {
  if (index >= breakpoints_.size() || breakpoints_[index].hit_condition == hit_condition) {
    return;
  }
  breakpoints_[index].hit_condition = std::move(hit_condition);
  revision_.Bump();
}

void FunctionBreakpointStore::ApplyVerification(
    const std::vector<std::string>& requested_names,
    const std::vector<VerifiedFunctionBreakpoint>& results) {
  const std::size_t count = std::min(requested_names.size(), results.size());
  std::unordered_map<std::string_view, FunctionBreakpoint*> by_name;
  by_name.reserve(breakpoints_.size());
  for (FunctionBreakpoint& bp : breakpoints_) {
    by_name.emplace(std::string_view(bp.name), &bp);
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto it = by_name.find(std::string_view(requested_names[i]));
    if (it == by_name.end()) {
      continue;
    }
    FunctionBreakpoint& bp = *it->second;
    bp.verified = results[i].verified;
    bp.adapter_id = results[i].id;
    bp.verify_message = results[i].message;
  }
  revision_.Bump();
}

bool FunctionBreakpointStore::ApplyBreakpointEvent(const VerifiedFunctionBreakpoint& result) {
  if (result.id == 0) {
    return false;
  }
  const auto it = std::find_if(breakpoints_.begin(), breakpoints_.end(),
                               [&result](const FunctionBreakpoint& bp) {
                                 return bp.adapter_id == result.id;
                               });
  if (it == breakpoints_.end()) {
    return false;
  }
  it->verified = result.verified;
  it->verify_message = result.message;
  revision_.Bump();
  return true;
}

void FunctionBreakpointStore::ResetVerification() {
  bool changed = false;
  for (FunctionBreakpoint& bp : breakpoints_) {
    if (bp.verified || bp.adapter_id != 0 || !bp.verify_message.empty()) {
      bp.verified = false;
      bp.adapter_id = 0;
      bp.verify_message.clear();
      changed = true;
    }
  }
  if (changed) {
    revision_.Bump();
  }
}

void FunctionBreakpointStore::ReplaceAll(std::vector<FunctionBreakpoint> breakpoints) {
  breakpoints_.clear();
  breakpoints_.reserve(breakpoints.size());
  std::unordered_set<std::string_view> seen;
  seen.reserve(breakpoints.size());
  for (FunctionBreakpoint& bp : breakpoints) {
    if (bp.name.empty() || seen.count(std::string_view(bp.name)) != 0) {
      continue;
    }
    bp.verified = false;
    bp.adapter_id = 0;
    bp.verify_message.clear();
    breakpoints_.push_back(std::move(bp));
    seen.insert(std::string_view(breakpoints_.back().name));
  }
  revision_.Bump();
}

}
