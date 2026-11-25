#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"
#include "util/Revision.h"

namespace microide::editor {

struct AppliedEdit;

struct Breakpoint {
  std::size_t line = 0;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;

  bool verified = false;
  int adapter_id = 0;
  std::string verify_message;

  bool operator==(const Breakpoint& other) const {
    return line == other.line && enabled == other.enabled && condition == other.condition &&
           hit_condition == other.hit_condition && log_message == other.log_message;
  }
};

struct VerifiedBreakpoint {
  int id = 0;
  bool verified = false;
  int line = 0;
  std::string message;
};

class BreakpointStore {
 public:
  struct FileBreakpoints {
    std::filesystem::path path;
    std::vector<Breakpoint> breakpoints;
  };

  bool Toggle(const std::filesystem::path& path, std::size_t line);
  void Set(const std::filesystem::path& path, std::size_t line, bool enabled = true);
  void Remove(const std::filesystem::path& path, std::size_t line);
  bool ToggleEnabled(const std::filesystem::path& path, std::size_t line);

  void SetCondition(const std::filesystem::path& path, std::size_t line,
                    std::optional<std::string> condition);
  void SetHitCondition(const std::filesystem::path& path, std::size_t line,
                       std::optional<std::string> hit_condition);
  void SetLogMessage(const std::filesystem::path& path, std::size_t line,
                     std::optional<std::string> log_message);
  void ClearFile(const std::filesystem::path& path);
  void Clear();

  bool HasBreakpoint(const std::filesystem::path& path, std::size_t line) const;
  const Breakpoint* FindAt(const std::filesystem::path& path, std::size_t line) const;
  const std::vector<Breakpoint>* FindByPath(const std::filesystem::path& path) const;
  const std::vector<Breakpoint>* FindByPathKey(std::string_view path_key) const;

  std::vector<FileBreakpoints> SnapshotAll() const;

  struct FileBreakpointsView {
    const std::filesystem::path* path = nullptr;
    std::span<const Breakpoint> breakpoints;
  };
  void FillSortedFileViews(std::vector<FileBreakpointsView>* out) const;

  void ApplyVerification(const std::filesystem::path& path,
                         const std::vector<VerifiedBreakpoint>& results);
  void ApplyBreakpointEvent(const std::filesystem::path& path, const VerifiedBreakpoint& result);
  void ResetVerification();

  bool ShiftForAppliedEdit(const std::filesystem::path& path, const AppliedEdit& edit);

  void ReplaceAll(std::vector<FileBreakpoints> files);

  std::uint64_t revision() const { return revision_.value(); }

 private:
  void SetOptionalField(const std::filesystem::path& path,
                        std::size_t line,
                        std::optional<std::string> Breakpoint::*field,
                        std::optional<std::string> value);
  static std::string PathKey(const std::filesystem::path& path);
  static std::string_view PathKeyView(const std::filesystem::path& path, std::string& scratch);
  std::vector<Breakpoint>* MutableForKey(const std::string& key);
  std::vector<Breakpoint>* EnsureFileEntry(const std::filesystem::path& path,
                                           std::string* key_out);
  Breakpoint* MutableBreakpoint(const std::filesystem::path& path, std::size_t line);

  std::unordered_map<std::string, FileBreakpoints, util::TransparentStringHash, std::equal_to<>>
      by_path_;
  util::Revision revision_;
};

}
