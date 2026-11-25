#include "editor/BreakpointStore.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "editor/EditTypes.h"
#include "util/PathMatch.h"

namespace microide::editor {

namespace {

template <typename Breakpoints>
auto LowerBoundLine(Breakpoints& breakpoints, std::size_t line) {
  return std::lower_bound(breakpoints.begin(), breakpoints.end(), line,
                          [](const Breakpoint& bp, std::size_t value) { return bp.line < value; });
}

template <typename Breakpoints>
auto FindLine(Breakpoints& breakpoints, std::size_t line) {
  const auto it = LowerBoundLine(breakpoints, line);
  return it != breakpoints.end() && it->line == line ? it : breakpoints.end();
}

}

std::string BreakpointStore::PathKey(const std::filesystem::path& path) {
  std::string scratch;
  const std::string_view key = PathKeyView(path, scratch);
  return scratch.empty() ? std::string(key) : std::move(scratch);
}

std::string_view BreakpointStore::PathKeyView(const std::filesystem::path& path,
                                              std::string& scratch) {
  if (path.empty()) {
    return {};
  }
  if (!util::PathTextNeedsNormalizing(path.native())) {
    return path.native();
  }
  scratch = path.lexically_normal().generic_string();
  return scratch;
}

std::vector<Breakpoint>* BreakpointStore::MutableForKey(const std::string& key) {
  const auto it = by_path_.find(key);
  return it == by_path_.end() ? nullptr : &it->second.breakpoints;
}

std::vector<Breakpoint>* BreakpointStore::EnsureFileEntry(const std::filesystem::path& path,
                                                          std::string* key_out) {
  const std::filesystem::path normalized = path.lexically_normal();
  std::string key = PathKey(normalized);
  if (key.empty()) {
    return nullptr;
  }
  FileBreakpoints& entry = by_path_[key];
  if (entry.path.empty()) {
    entry.path = normalized;
  }
  if (key_out != nullptr) {
    *key_out = std::move(key);
  }
  return &entry.breakpoints;
}

bool BreakpointStore::Toggle(const std::filesystem::path& path, std::size_t line) {
  std::string key;
  std::vector<Breakpoint>* const breakpoints = EnsureFileEntry(path, &key);
  if (breakpoints == nullptr) {
    return false;
  }
  const auto it = LowerBoundLine(*breakpoints, line);
  if (it != breakpoints->end() && it->line == line) {
    breakpoints->erase(it);
    if (breakpoints->empty()) {
      by_path_.erase(key);
    }
    revision_.Bump();
    return false;
  }
  breakpoints->insert(it, Breakpoint{.line = line});
  revision_.Bump();
  return true;
}

void BreakpointStore::Set(const std::filesystem::path& path, std::size_t line, bool enabled) {
  std::vector<Breakpoint>* const breakpoints = EnsureFileEntry(path, nullptr);
  if (breakpoints == nullptr) {
    return;
  }
  const auto it = LowerBoundLine(*breakpoints, line);
  if (it != breakpoints->end() && it->line == line) {
    if (it->enabled != enabled) {
      it->enabled = enabled;
      revision_.Bump();
    }
    return;
  }
  breakpoints->insert(it, Breakpoint{.line = line, .enabled = enabled});
  revision_.Bump();
}

bool BreakpointStore::ToggleEnabled(const std::filesystem::path& path, std::size_t line) {
  std::vector<Breakpoint>* breakpoints = MutableForKey(PathKey(path));
  if (breakpoints == nullptr) {
    return false;
  }
  const auto it = FindLine(*breakpoints, line);
  if (it == breakpoints->end()) {
    return false;
  }
  it->enabled = !it->enabled;
  revision_.Bump();
  return true;
}

Breakpoint* BreakpointStore::MutableBreakpoint(const std::filesystem::path& path,
                                               std::size_t line) {
  std::vector<Breakpoint>* const breakpoints = EnsureFileEntry(path, nullptr);
  if (breakpoints == nullptr) {
    return nullptr;
  }
  const auto it = LowerBoundLine(*breakpoints, line);
  if (it != breakpoints->end() && it->line == line) {
    return &(*it);
  }
  return &(*breakpoints->insert(it, Breakpoint{.line = line}));
}

void BreakpointStore::SetOptionalField(const std::filesystem::path& path, std::size_t line,
                                       std::optional<std::string> Breakpoint::*field,
                                       std::optional<std::string> value) {
  if (!value.has_value() && !HasBreakpoint(path, line)) {
    return;
  }
  Breakpoint* bp = MutableBreakpoint(path, line);
  if (bp == nullptr || bp->*field == value) {
    return;
  }
  bp->*field = std::move(value);
  revision_.Bump();
}

void BreakpointStore::SetCondition(const std::filesystem::path& path, std::size_t line,
                                   std::optional<std::string> condition) {
  SetOptionalField(path, line, &Breakpoint::condition, std::move(condition));
}

void BreakpointStore::SetHitCondition(const std::filesystem::path& path, std::size_t line,
                                      std::optional<std::string> hit_condition) {
  SetOptionalField(path, line, &Breakpoint::hit_condition, std::move(hit_condition));
}

void BreakpointStore::SetLogMessage(const std::filesystem::path& path, std::size_t line,
                                    std::optional<std::string> log_message) {
  SetOptionalField(path, line, &Breakpoint::log_message, std::move(log_message));
}

void BreakpointStore::Remove(const std::filesystem::path& path, std::size_t line) {
  const std::string key = PathKey(path);
  std::vector<Breakpoint>* breakpoints = MutableForKey(key);
  if (breakpoints == nullptr) {
    return;
  }
  const auto it = FindLine(*breakpoints, line);
  if (it == breakpoints->end()) {
    return;
  }
  breakpoints->erase(it);
  if (breakpoints->empty()) {
    by_path_.erase(key);
  }
  revision_.Bump();
}

void BreakpointStore::ClearFile(const std::filesystem::path& path) {
  if (by_path_.erase(PathKey(path)) > 0) {
    revision_.Bump();
  }
}

void BreakpointStore::Clear() {
  if (by_path_.empty()) {
    return;
  }
  by_path_.clear();
  revision_.Bump();
}

bool BreakpointStore::HasBreakpoint(const std::filesystem::path& path, std::size_t line) const {
  const auto it = by_path_.find(PathKey(path));
  if (it == by_path_.end()) {
    return false;
  }
  const auto& breakpoints = it->second.breakpoints;
  return FindLine(breakpoints, line) != breakpoints.end();
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
  std::sort(files.begin(), files.end(), [](const FileBreakpoints& lhs, const FileBreakpoints& rhs) {
    return lhs.path.native() < rhs.path.native();
  });
  return files;
}

void BreakpointStore::FillSortedFileViews(std::vector<FileBreakpointsView>* out) const {
  if (out == nullptr) {
    return;
  }
  out->clear();
  out->reserve(by_path_.size());
  for (const auto& [key, entry] : by_path_) {
    out->push_back(FileBreakpointsView{.path = &entry.path, .breakpoints = entry.breakpoints});
  }
  std::sort(out->begin(), out->end(),
            [](const FileBreakpointsView& lhs, const FileBreakpointsView& rhs) {
              return lhs.path->native() < rhs.path->native();
            });
}

void BreakpointStore::ApplyVerification(const std::filesystem::path& path,
                                        const std::vector<VerifiedBreakpoint>& results) {
  std::vector<Breakpoint>* breakpoints = MutableForKey(PathKey(path));
  if (breakpoints == nullptr) {
    return;
  }
  for (const VerifiedBreakpoint& result : results) {
    if (result.line <= 0) {
      continue;
    }
    const std::size_t line = static_cast<std::size_t>(result.line - 1);
    const auto it = FindLine(*breakpoints, line);
    if (it == breakpoints->end()) {
      continue;
    }
    it->verified = result.verified;
    it->adapter_id = result.id;
    it->verify_message = result.message;
  }
  revision_.Bump();
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
                                   [line](const Breakpoint& bp) {
                                     return bp.line == line && bp.adapter_id == 0;
                                   });
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
    for (auto& [key, entry] : by_path_) {
      if (apply_in(entry.breakpoints)) {
        changed = true;
        break;
      }
    }
  }
  if (changed) {
    revision_.Bump();
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
    revision_.Bump();
  }
}

bool BreakpointStore::ShiftForAppliedEdit(const std::filesystem::path& path,
                                          const AppliedEdit& edit) {
  std::string key_scratch;
  const auto entry_it = by_path_.find(PathKeyView(path, key_scratch));
  if (entry_it == by_path_.end() || entry_it->second.breakpoints.empty()) {
    return false;
  }
  TextPosition before_start = edit.range_before.start;
  TextPosition before_end = edit.range_before.end;
  if (before_end.line < before_start.line ||
      (before_end.line == before_start.line && before_end.column < before_start.column)) {
    std::swap(before_start, before_end);
  }
  const std::size_t s = before_start.line;
  const std::size_t e = before_end.line;
  const std::size_t newline_count = static_cast<std::size_t>(
      std::count(edit.replacement_text.begin(), edit.replacement_text.end(), '\n'));
  const std::size_t new_end_line = s + newline_count;
  if (s == e && new_end_line == e) {
    return false;
  }
  const std::ptrdiff_t delta =
      static_cast<std::ptrdiff_t>(new_end_line) - static_cast<std::ptrdiff_t>(e);

  std::vector<Breakpoint>& breakpoints = entry_it->second.breakpoints;
  bool changed = false;
  for (Breakpoint& bp : breakpoints) {
    const std::size_t line = bp.line;
    std::size_t next = line;
    if (line <= s) {
      next = line;
    } else if (line > e) {
      const std::ptrdiff_t shifted = static_cast<std::ptrdiff_t>(line) + delta;
      next = shifted < 0 ? 0 : static_cast<std::size_t>(shifted);
    } else {
      next = new_end_line;
    }
    if (next != line) {
      bp.line = next;
      changed = true;
    }
  }
  if (!changed) {
    return false;
  }
  std::stable_sort(breakpoints.begin(), breakpoints.end(),
                   [](const Breakpoint& a, const Breakpoint& b) { return a.line < b.line; });
  breakpoints.erase(
      std::unique(breakpoints.begin(), breakpoints.end(),
                  [](const Breakpoint& a, const Breakpoint& b) { return a.line == b.line; }),
      breakpoints.end());
  revision_.Bump();
  return true;
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
    for (Breakpoint& bp : file.breakpoints) {
      bp.verified = false;
      bp.adapter_id = 0;
      bp.verify_message.clear();
    }
    by_path_[key] = FileBreakpoints{.path = normalized, .breakpoints = std::move(file.breakpoints)};
  }
  revision_.Bump();
}

}
