#include "terminal/TerminalSession.h"

#include "terminal/TerminalAnsiColors.h"
#include "terminal/TerminalOscClipboard.h"
#include "util/Hex.h"
#include "util/Parse.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include "util/StringUtil.h"
#endif

namespace microide::terminal {

namespace {

// xterm `rgb:RRRR/GGGG/BBBB` color reply (8-bit components widened to 16-bit).
std::string FormatOscRgbReply(SDL_Color color) {
  static constexpr char kHex[] = "0123456789abcdef";
  const auto component = [](Uint8 value) {
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
  // avoids a startup timeout. Colors mirror the built-in dark palette.
  if (command == "10" || command == "11" || command == "12") {
    if (payload.find('?') != std::string_view::npos) {
      const SDL_Color foreground = BasicAnsiColor(7, true);
      const SDL_Color background = BasicAnsiColor(0, false);
      const SDL_Color color = command == "11" ? background : foreground;
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
                      FormatOscRgbReply(Ansi256Color(index)) + "\x1b\\");
    }
    return;
  }

  // OSC 8 (hyperlinks), 9 (notifications), 133 (shell-integration prompt marks),
  // and palette resets (104/110/111/112) are accepted and intentionally ignored
  // so they never corrupt the screen.
}

}  // namespace microide::terminal
