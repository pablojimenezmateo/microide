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

// What a remote project had open on one machine, for the next machine to pick
// up: the same project is opened from a work laptop and a personal one, and each
// knew only its own tabs while the terminals -- the host's -- were there for
// both. It is left on the host (`state/put`) whenever it changes, and a machine
// that connects is offered what another one left.
//
// It names what means the same thing on every machine: the project's files that
// were open in editor tabs (paths relative to the root, with the caret) and
// which one was in front. It is taken ADDITIVELY -- the files open beside what
// is already open -- so nothing a machine has is closed, moved or overwritten.
// Unsaved text, untitled buffers, compare and merge tabs, files outside the
// project, the split layout and the sizes of things stay where they are. The
// terminals need no record: they are the host's, and a connect already brings
// back the ones no machine is holding (RemoteHostService::OnHostTerminals).
struct RoamingSession {
  struct File {
    std::string path;  // relative to the project root, '/'-separated
    std::size_t line = 0;
    std::size_t column = 0;
    bool active = false;  // the tab that was in front (at most one)

    bool operator==(const File&) const = default;
  };

  // The machine that left it (its host name): the offer says whose it is, and a
  // machine is not offered its own.
  std::string client;
  // Wall clock, seconds since the epoch.
  std::uint64_t saved_at = 0;
  std::vector<File> files;  // tab order, the focused group first

  bool empty() const { return files.empty(); }
};

inline constexpr std::size_t kMaxRoamingFiles = 256;
inline constexpr std::size_t kMaxRoamingPathBytes = 4096;
inline constexpr std::size_t kMaxRoamingClientBytes = 255;

// The roaming part of `session`, the session of the project at `root`.
RoamingSession CaptureRoamingSession(const PersistedProjectSessionState& session,
                                     const std::filesystem::path& root, std::string client,
                                     std::uint64_t saved_at);

// The text kept on the host (JSON).
std::string EncodeRoamingSession(const RoamingSession& roaming);
// Untrusted -- another machine wrote it, and anything on the host could have:
// nullopt unless it is that form, and every path is a relative one that stays
// under the root, within the bounds above.
std::optional<RoamingSession> DecodeRoamingSession(std::string_view text);
// What identifies the content, whoever left it and whenever: two sessions with
// the same fingerprint have the same files and carets.
std::string RoamingSessionFingerprint(const RoamingSession& roaming);
// The same without the carets: which files, in which order, and which in front.
// A caret moves with every keystroke; this changes when a tab opens or closes.
std::string RoamingSessionFileList(const RoamingSession& roaming);

}  // namespace microide::workspace
