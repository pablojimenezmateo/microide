#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalOscClipboard.h"
#include "util/AnsiPalette.h"
#include "util/Hex.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::terminal {

namespace {

// xterm `rgb:RRRR/GGGG/BBBB` color reply (8-bit components widened to 16-bit).
std::string FormatOscRgbReply(util::Rgba8 color) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto component = [](std::uint8_t value) {
    std::string out;
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0xF]);
    out.push_back(kHex[value >> 4]);
    out.push_back(kHex[value & 0xF]);
    return out;
  };
  return "rgb:" + component(color.r) + "/" + component(color.g) + "/" + component(color.b);
}

// Lowercased local machine hostname (short name), or empty if unavailable.
// Used to distinguish a local OSC 7 report from a remote (SSH) shell's report.
std::string LocalHostNameLower() {
#if defined(__unix__) || defined(__APPLE__)
  char buffer[256] = {0};
  if (::gethostname(buffer, sizeof(buffer) - 1) != 0) {
    return {};
  }
  std::string name(buffer);
  // Compare against the short hostname only; drop any DNS domain suffix.
  if (const std::size_t dot = name.find('.'); dot != std::string::npos) {
    name.resize(dot);
  }
  for (char& ch : name) {
    ch = util::ToLowerAsciiChar(static_cast<char>(ch));
  }
  return name;
#else
  return {};
#endif
}

// True when an OSC 7 host component names this machine: empty, `localhost`, or
// the local short hostname. Remote (SSH) shells report a foreign hostname whose
// paths do not exist locally.
bool Osc7HostIsLocal(std::string_view host) {
  if (host.empty()) {
    return true;
  }
  std::string host_lower(host);
  for (char& ch : host_lower) {
    ch = util::ToLowerAsciiChar(static_cast<char>(ch));
  }
  if (const std::size_t dot = host_lower.find('.'); dot != std::string::npos) {
    host_lower.resize(dot);
  }
  if (host_lower == "localhost") {
    return true;
  }
  const std::string local = LocalHostNameLower();
  return !local.empty() && host_lower == local;
}

// Extract the filesystem path from an OSC 7 `file://host/path` payload, decoding
// percent-escapes. Returns empty for a report from a non-local host.
std::string DecodeOsc7Path(std::string_view payload) {
  std::string_view path = payload;
  if (path.rfind("file://", 0) == 0) {
    path.remove_prefix(7);
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos) {
      return {};
    }
    // Reject cwd reports for a non-local host. `file://remote/home/user` refers
    // to a *remote* machine's filesystem; treating it as a local working
    // directory would seed local file operations from a path that does not
    // exist here.
    if (!Osc7HostIsLocal(path.substr(0, slash))) {
      return {};
    }
    path = path.substr(slash);
  }
  return util::PercentDecode(path);
}

// A `file://` link names a path on the machine the program runs on. One for this
// machine is stored as `file://` + the decoded absolute path, which is what the
// click path opens (and what a host terminal's client maps into its tree); any
// other URI, and a file URI for another host, is kept verbatim.
std::string NormalizeLinkUri(std::string_view uri) {
  constexpr std::string_view kFile = "file://";
  if (uri.size() < kFile.size() ||
      !util::EqualsAsciiCaseInsensitive(uri.substr(0, kFile.size()), kFile)) {
    return std::string(uri);
  }
  const std::string_view rest = uri.substr(kFile.size());
  const std::size_t slash = rest.find('/');
  if (slash == std::string_view::npos || !Osc7HostIsLocal(rest.substr(0, slash))) {
    return std::string(uri);
  }
  return std::string(kFile) + util::PercentDecode(rest.substr(slash));
}

}  // namespace

void TerminalSession::HandleOscSequenceLocked(std::string_view sequence) {
  if (sequence.empty() || sequence.front() != ']') {
    return;
  }

  if (const auto clipboard = DecodeOsc52ClipboardPayload(sequence)) {
    pending_clipboard_text_ = *clipboard;
    return;
  }

  const std::string_view body = sequence.substr(1);
  const std::size_t separator = body.find(';');
  if (separator == std::string_view::npos) {
    return;
  }

  const std::string_view command = body.substr(0, separator);
  const std::string_view payload = body.substr(separator + 1);

  if (command == "0" || command == "1" || command == "2") {
    const std::string title = SanitizeOscTitle(payload);
    launch_label_ = title.empty() ? default_launch_label_ : title;
    return;
  }

  if (command == "7") {
    // Working-directory report: OSC 7 ; file://host/path
    std::string decoded = DecodeOsc7Path(payload);
    if (!decoded.empty()) {
      reported_working_directory_ = std::filesystem::path(std::move(decoded));
    }
    return;
  }

  // Default foreground / background / cursor color queries. Applications use
  // these (especially OSC 11) to detect light vs dark backgrounds; answering
  // avoids a startup timeout. The answer is what the host actually paints
  // (SetDefaultColors): a fixed dark reply under a light theme made Claude Code
  // and friends pick their dark palette on a white background.
  if (command == "10" || command == "11" || command == "12") {
    if (payload.find('?') != std::string_view::npos) {
      const util::Rgba8 color = command == "11" ? default_background_ : default_foreground_;
      SendBytesLocked("\x1b]" + std::string(command) + ";" + FormatOscRgbReply(color) + "\x1b\\");
    }
    return;
  }

  if (command == "4") {
    // Palette query: OSC 4 ; index ; ?  -> reply with the indexed color.
    const std::size_t inner = payload.find(';');
    if (inner != std::string_view::npos &&
        payload.find('?', inner) != std::string_view::npos) {
      const int index = static_cast<int>(std::clamp<std::int64_t>(
          util::ParseInt64(payload.substr(0, inner)).value_or(0), 0, 255));
      SendBytesLocked("\x1b]4;" + std::to_string(index) + ";" +
                      FormatOscRgbReply(util::Ansi256Color(index)) + "\x1b\\");
    }
    return;
  }

  // Notifications: OSC 9 ; <body> (iTerm2) and OSC 777 ; notify ; <title> ; <body>
  // (urxvt, Ghostty). OSC 9 is overloaded — ConEmu/Windows Terminal send
  // `9 ; <n> ; ...` subcommands (4 is a progress bar) — and a numbered
  // subcommand is not a message for the user.
  const auto sanitized = [](std::string_view text) {
    constexpr std::size_t kMaxNotificationBytes = 512;
    std::string out;
    out.reserve(std::min(text.size(), kMaxNotificationBytes));
    for (const char c : text.substr(0, kMaxNotificationBytes)) {
      out.push_back(static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
    }
    return out;
  };
  if (command == "9") {
    const std::size_t digits = payload.find_first_not_of("0123456789");
    const bool subcommand = digits != 0 && digits != std::string_view::npos && payload[digits] == ';';
    if (!subcommand && !payload.empty()) {
      pending_notification_ = Notification{.title = {}, .body = sanitized(payload)};
    }
    return;
  }
  if (command == "777") {
    if (payload.rfind("notify;", 0) == 0) {
      const std::string_view rest = payload.substr(7);
      const std::size_t split = rest.find(';');
      pending_notification_ =
          Notification{.title = sanitized(rest.substr(0, split)),
                       .body = split == std::string_view::npos ? std::string()
                                                               : sanitized(rest.substr(split + 1))};
    }
    return;
  }

  // Hyperlinks: OSC 8 ; params ; URI opens a link on the pen, an empty URI closes
  // it. The params (`id=…`) only group cells for hover; identical URIs share one
  // table entry, which is all the grouping a click needs.
  if (command == "8") {
    const std::size_t split = payload.find(';');
    const std::string_view uri =
        split == std::string_view::npos ? std::string_view{} : payload.substr(split + 1);
    current_link_ = uri.empty() ? 0 : InternLinkLocked(NormalizeLinkUri(uri));
    return;
  }

  // 133 (shell-integration prompt marks) and palette resets (104/110/111/112)
  // are accepted and intentionally ignored so they never corrupt the screen.
}

std::uint16_t TerminalSession::InternLinkLocked(std::string_view uri) {
  if (const std::uint16_t id = links_.Intern(uri); id != 0 || !links_.full()) {
    return id;
  }
  // Full: keep only what a buffer still shows. The pen's own link is live too.
  std::vector<bool> live(TerminalLinkTable::kMaxLinks + 1, false);
  live[current_link_] = true;
  const auto mark = [&live](const std::deque<TerminalLine>& lines) {
    for (const TerminalLine& line : lines) {
      for (const TerminalCell& cell : line.cells) {
        live[cell.link] = true;
      }
    }
  };
  mark(lines_);
  mark(primary_screen_.lines);
  mark(alternate_screen_.lines);
  live[0] = false;
  links_.Retain(live);
  return links_.Intern(uri);
}

std::string TerminalSession::LinkUri(std::uint16_t link) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::string(links_.Uri(link));
}

}  // namespace microide::terminal
