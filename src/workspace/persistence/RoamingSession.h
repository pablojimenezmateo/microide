#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::workspace {

struct RoamingSession {
  struct File {
    std::string path;
    std::size_t line = 0;
    std::size_t column = 0;
    bool active = false;

    bool operator==(const File&) const = default;
  };

  std::string client;
  std::uint64_t saved_at = 0;
  std::vector<File> files;

  bool empty() const { return files.empty(); }
};

inline constexpr std::size_t kMaxRoamingFiles = 256;
inline constexpr std::size_t kMaxRoamingPathBytes = 4096;
inline constexpr std::size_t kMaxRoamingClientBytes = 255;

RoamingSession CaptureRoamingSession(const PersistedProjectSessionState& session,
                                     const std::filesystem::path& root, std::string client,
                                     std::uint64_t saved_at);

std::string EncodeRoamingSession(const RoamingSession& roaming);
std::optional<RoamingSession> DecodeRoamingSession(std::string_view text);
std::string RoamingSessionFingerprint(const RoamingSession& roaming);
std::string RoamingSessionFileList(const RoamingSession& roaming);

}
