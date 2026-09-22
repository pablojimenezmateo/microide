// Proves the kernel links with no windowing library present.
//
// This binary is deliberately almost empty. Its value is the LINK: it pulls in every
// object in microide_kernel and links against nothing but libc++ and pcre2, so a
// kernel translation unit that calls an SDL function — or any shell-layer function —
// fails here with an undefined reference. The architecture lint reads includes and
// the split precompiled header catches bare SDL type names; neither can see a call
// that reached the kernel through a header that is itself clean.
//
// It is also the skeleton of the headless agent the remote-projects design needs
// (dev-docs/design/remote-projects.md § 8, G1): "the kernel runs without a window"
// stops being a claim once something actually runs it without one.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "project/FileIndex.h"
#include "terminal/TerminalSession.h"
#include "util/AnsiPalette.h"
#include "util/BackgroundTaskCounter.h"
#include "util/Log.h"
#include "util/Waker.h"

int main() {
  // Touch one entry point per kernel area so the link is not satisfied by dead-code
  // elimination of an object nothing references.
  microide::terminal::TerminalSession session;
  session.SetWakeChannel(0);
  const microide::util::Rgba8 red = microide::util::BasicAnsiColor(1, false);
  const bool wake_delivered = microide::util::PushWake(0);
  (void)microide::util::ConsumeOwedWake();
  microide::util::IncrementBackgroundTaskCount();
  microide::util::DecrementBackgroundTaskCountAndWake();
  // A real (tiny) project scan: the point is that the filesystem half of the kernel
  // runs, not that it is fast, so the tree is one file the probe makes itself.
  std::error_code ec;
  const std::filesystem::path scan_root =
      std::filesystem::temp_directory_path(ec) / "microide-kernel-link-probe";
  std::filesystem::remove_all(scan_root, ec);
  std::filesystem::create_directories(scan_root, ec);
  { std::ofstream(scan_root / "one.txt") << "probe\n"; }
  const std::vector<microide::project::ProjectFile> scanned =
      microide::project::FileIndex::ScanFiles(scan_root, false, {});
  std::filesystem::remove_all(scan_root, ec);

  microide::util::Log("microide kernel link probe: ok");
  std::printf("terminal rows=%zu ansi_red=#%02x%02x%02x wake=%d scanned=%zu\n",
              session.SnapshotLines().size(), red.r, red.g, red.b,
              wake_delivered ? 1 : 0, scanned.size());
  // Every one of these is a fact about a DIFFERENT kernel area, so a probe reduced to
  // a bare `return 0` by a later edit fails instead of passing hollow.
  const bool ok = session.SnapshotLines().size() == 1 && red.r == 0xc3 && !wake_delivered &&
                  scanned.size() == 1;
  if (!ok) {
    std::fprintf(stderr, "kernel link probe: a kernel entry point returned the wrong thing\n");
    return 1;
  }
  return 0;
}
