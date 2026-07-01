#include "editor/BreakpointStore.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

std::string BreakpointStore::PathKey(const std::filesystem::path& path) {
  return path.empty() ? std::string{} : path.lexically_normal().generic_string();
}

void BreakpointStore::BumpRevision() {
  if (++revision_ == 0) {
    revision_ = 1;
  }
}

std::vector<Breakpoint>* BreakpointStore::MutableForKey(const std::string& key) {
  const auto it = by_path_.find(key);
  return it == by_path_.end() ? nullptr : &it->second.breakpoints;
}

bool BreakpointStore::Toggle(const std::filesystem::path& path, std::size_t line) {
  const std::filesystem::path normalized = path.lexically_normal();
  const std::string key = PathKey(normalized);
  if (key.empty()) {
    return false;
  }
  auto& entry = by_path_[key];
  if (entry.path.empty()) {
    entry.path = normalized;
  }
  auto& breakpoints = entry.breakpoints;
  const auto it = std::lower_bound(
      breakpoints.begin(), breakpoints.end(), line,
      [](const Breakpoint& bp, std::size_t value) { return bp.line < value; });
  if (it != breakpoints.end() && it->line == line) {
    breakpoints.erase(it);
    if (breakpoints.empty()) {
      by_path_.erase(key);
    }
    BumpRevision();
    return false;
  }
  breakpoints.insert(it, Breakpoint{.line = line});
  BumpRevision();
  return true;
}

void BreakpointStore::Set(const std::filesystem::path& path, std::size_t line, bool enabled) {
  const std::filesystem::path normalized = path.lexically_normal();
  const std::string key = PathKey(normalized);
  if (key.empty()) {
    return;
  }
  auto& entry = by_path_[key];
  if (entry.path.empty()) {
    entry.path = normalized;
  }
  auto& breakpoints = entry.breakpoints;
  const auto it = std::lower_bound(
      breakpoints.begin(), breakpoints.end(), line,
      [](const Breakpoint& bp, std::size_t value) { return bp.line < value; });
  if (it != breakpoints.end() && it->line == line) {
    if (it->enabled != enabled) {
      it->enabled = enabled;
      BumpRevision();
    }
    return;
  }
  breakpoints.insert(it, Breakpoint{.line = line, .enabled = enabled});
  BumpRevision();
}

bool BreakpointStore::ToggleEnabled(const std::filesystem::path& path, std::size_t line) {
  std::vector<Breakpoint>* breakpoints = MutableForKey(PathKey(path));
  if (breakpoints == nullptr) {
    return false;
  }
  const auto it = std::find_if(breakpoints->begin(), breakpoints->end(),
                               [line](const Breakpoint& bp) { return bp.line == line; });
  if (it == breakpoints->end()) {
    return false;
  }
  it->enabled = !it->enabled;
  BumpRevision();
  return true;
}

Breakpoint* BreakpointStore::MutableBreakpoint(const std::filesystem::path& path,
                                               std::size_t line) {
  const std::filesystem::path normalized = path.lexically_normal();
  const std::string key = PathKey(normalized);
  if (key.empty()) {
    return nullptr;
  }
  auto& entry = by_path_[key];
  if (entry.path.empty()) {
    entry.path = normalized;
  }
  auto& breakpoints = entry.breakpoints;
  const auto it = std::lower_bound(
      breakpoints.begin(), breakpoints.end(), line,
      [](const Breakpoint& bp, std::size_t value) { return bp.line < value; });
  if (it != breakpoints.end() && it->line == line) {
    return &(*it);
  }
  return &(*breakpoints.insert(it, Breakpoint{.line = line}));
}

void BreakpointStore::SetCondition(const std::filesystem::path& path, std::size_t line,
                                   std::optional<std::string> condition) {
  Breakpoint* bp = MutableBreakpoint(path, line);
  if (bp == nullptr || bp->condition == condition) {
    return;
  }
  bp->condition = std::move(condition);
  BumpRevision();
}

void BreakpointStore::SetHitCondition(const std::filesystem::path& path, std::size_t line,
                                      std::optional<std::string> hit_condition) {
  Breakpoint* bp = MutableBreakpoint(path, line);
  if (bp == nullptr || bp->hit_condition == hit_condition) {
    return;
  }
  bp->hit_condition = std::move(hit_condition);
  BumpRevision();
}

void BreakpointStore::SetLogMessage(const std::filesystem::path& path, std::size_t line,
                                    std::optional<std::string> log_message) {
  Breakpoint* bp = MutableBreakpoint(path, line);
  if (bp == nullptr || bp->log_message == log_message) {
    return;
  }
  bp->log_message = std::move(log_message);
  BumpRevision();
}

void BreakpointStore::Remove(const std::filesystem::path& path, std::size_t line) {
  const std::string key = PathKey(path);
  std::vector<Breakpoint>* breakpoints = MutableForKey(key);
  if (breakpoints == nullptr) {
    return;
  }
  const auto it = std::lower_bound(
      breakpoints->begin(), breakpoints->end(), line,
      [](const Breakpoint& bp, std::size_t value) { return bp.line < value; });
  if (it == breakpoints->end() || it->line != line) {
    return;
  }
  breakpoints->erase(it);
  if (breakpoints->empty()) {
    by_path_.erase(key);
  }
  BumpRevision();
}

void BreakpointStore::ClearFile(const std::filesystem::path& path) {
  if (by_path_.erase(PathKey(path)) > 0) {
    BumpRevision();
  }
}

void BreakpointStore::Clear() {
  if (by_path_.empty()) {
    return;
  }
  by_path_.clear();
  BumpRevision();
}

bool BreakpointStore::HasBreakpoint(const std::filesystem::path& path, std::size_t line) const {
  const auto it = by_path_.find(PathKey(path));
  if (it == by_path_.end()) {
    return false;
  }
  const auto& breakpoints = it->second.breakpoints;
  const auto bp = std::lower_bound(
      breakpoints.begin(), breakpoints.end(), line,
      [](const Breakpoint& b, std::size_t value) { return b.line < value; });
  return bp != breakpoints.end() && bp->line == line;
}

const std::vector<Breakpoint>* BreakpointStore::FindByPath(
    const std::filesystem::path& path) const {
  return FindByPathKey(PathKey(path));
}

const std::vector<Breakpoint>* BreakpointStore::FindByPathKey(std::string_view path_key) const {
  const auto it = by_path_.find(path_key);
  return it == by_path_.end() ? nullptr : &it->second.breakpoints;
}

std::vector<BreakpointStore::FileBreakpoints> BreakpointStore::SnapshotAll() const {
  std::vector<FileBreakpoints> files;
  files.reserve(by_path_.size());
  for (const auto& [key, entry] : by_path_) {
    files.push_back(FileBreakpoints{.path = entry.path, .breakpoints = entry.breakpoints});
  }
  // Deterministic order for callers/tests.
  std::sort(files.begin(), files.end(), [](const FileBreakpoints& lhs, const FileBreakpoints& rhs) {
    // native() is a const reference; avoids per-comparison string allocations.
    return lhs.path.native() < rhs.path.native();
  });
  return files;
}

void BreakpointStore::ApplyVerification(const std::filesystem::path& path,
                                        const std::vector<VerifiedBreakpoint>& results) {
  std::vector<Breakpoint>* breakpoints = MutableForKey(PathKey(path));
  if (breakpoints == nullptr) {
    return;
  }
  for (const VerifiedBreakpoint& result : results) {
    if (result.line <= 0) {
      continue;  // nothing to match on
    }
    // Match by the requested line, not by array index: the user may have toggled
    // another breakpoint in this file while the response was in flight, so the
    // current store order need not align with the request order.
    const std::size_t line = static_cast<std::size_t>(result.line - 1);
    const auto it = std::find_if(breakpoints->begin(), breakpoints->end(),
                                 [line](const Breakpoint& bp) { return bp.line == line; });
    if (it == breakpoints->end()) {
      continue;  // line was removed while the response was in flight
    }
    it->verified = result.verified;
    it->adapter_id = result.id;
    it->verify_message = result.message;
  }
  BumpRevision();
}

void BreakpointStore::ApplyBreakpointEvent(const std::filesystem::path& path,
                                           const VerifiedBreakpoint& result) {
  auto apply_in = [&result](std::vector<Breakpoint>& breakpoints) -> bool {
    Breakpoint* target = nullptr;
    if (result.id != 0) {
      const auto it = std::find_if(breakpoints.begin(), breakpoints.end(),
                                   [&result](const Breakpoint& bp) {
                                     return bp.adapter_id == result.id;
                                   });
      if (it != breakpoints.end()) {
        target = &(*it);
      }
    }
    if (target == nullptr && result.line > 0) {
      const std::size_t line = static_cast<std::size_t>(result.line - 1);
      const auto it = std::find_if(breakpoints.begin(), breakpoints.end(),
                                   [line](const Breakpoint& bp) { return bp.line == line; });
      if (it != breakpoints.end()) {
        target = &(*it);
      }
    }
    if (target == nullptr) {
      return false;
    }
    target->verified = result.verified;
    if (result.id != 0) {
      target->adapter_id = result.id;
    }
    target->verify_message = result.message;
    return true;
  };

  bool changed = false;
  if (!path.empty()) {
    if (std::vector<Breakpoint>* breakpoints = MutableForKey(PathKey(path));
        breakpoints != nullptr) {
      changed = apply_in(*breakpoints);
    }
  } else {
    // No source path on the event: locate the breakpoint by adapter id anywhere.
    for (auto& [key, entry] : by_path_) {
      if (apply_in(entry.breakpoints)) {
        changed = true;
        break;
      }
    }
  }
  if (changed) {
    BumpRevision();
  }
}

void BreakpointStore::ResetVerification() {
  bool changed = false;
  for (auto& [key, entry] : by_path_) {
    for (Breakpoint& bp : entry.breakpoints) {
      if (bp.verified || bp.adapter_id != 0 || !bp.verify_message.empty()) {
        bp.verified = false;
        bp.adapter_id = 0;
        bp.verify_message.clear();
        changed = true;
      }
    }
  }
  if (changed) {
    BumpRevision();
  }
}

void BreakpointStore::ReplaceAll(std::vector<FileBreakpoints> files) {
  by_path_.clear();
  for (auto& file : files) {
    const std::filesystem::path normalized = file.path.lexically_normal();
    const std::string key = PathKey(normalized);
    if (key.empty() || file.breakpoints.empty()) {
      continue;
    }
    std::sort(file.breakpoints.begin(), file.breakpoints.end(),
              [](const Breakpoint& lhs, const Breakpoint& rhs) { return lhs.line < rhs.line; });
    // Drop any persisted transient state defensively.
    for (Breakpoint& bp : file.breakpoints) {
      bp.verified = false;
      bp.adapter_id = 0;
      bp.verify_message.clear();
    }
    by_path_[key] = FileEntry{.path = normalized, .breakpoints = std::move(file.breakpoints)};
  }
  BumpRevision();
}

}  // namespace microide::editor
