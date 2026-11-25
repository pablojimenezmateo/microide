#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "project/FileIndex.h"
#include "terminal/TerminalSession.h"
#include "util/AnsiPalette.h"
#include "util/BackgroundTaskCounter.h"
#include "util/Log.h"
#include "util/Waker.h"

int main() {
  microide::terminal::TerminalSession session;
  session.SetWakeChannel(0);
  const microide::util::Rgba8 red = microide::util::BasicAnsiColor(1, false);
  const bool wake_delivered = microide::util::PushWake(0);
  (void)microide::util::ConsumeOwedWake();
  microide::util::IncrementBackgroundTaskCount();
  microide::util::DecrementBackgroundTaskCountAndWake();
  std::error_code ec;
  const std::filesystem::path temp_root = std::filesystem::temp_directory_path(ec);
  if (ec) {
    std::fprintf(stderr, "kernel link probe: no temp directory: %s\n", ec.message().c_str());
    return 1;
  }
  std::filesystem::path scan_root;
  for (int attempt = 0; attempt < 256 && scan_root.empty(); ++attempt) {
    const std::filesystem::path candidate =
        temp_root / ("microide-kernel-link-probe-" + std::to_string(attempt));
    std::error_code claim_error;
    if (std::filesystem::create_directory(candidate, claim_error) && !claim_error) {
      scan_root = candidate;
    }
  }
  if (scan_root.empty()) {
    std::fprintf(stderr, "kernel link probe: could not claim a scan directory under %s\n",
                 temp_root.string().c_str());
    return 1;
  }
  { std::ofstream(scan_root / "one.txt") << "probe\n"; }
  const std::vector<microide::project::ProjectFile> scanned =
      microide::project::FileIndex::ScanFiles(scan_root, false, {});
  std::filesystem::remove_all(scan_root, ec);

  microide::util::Log("microide kernel link probe: ok");
  std::printf("terminal rows=%zu ansi_red=#%02x%02x%02x wake=%d scanned=%zu\n",
              session.SnapshotLines().size(), red.r, red.g, red.b,
              wake_delivered ? 1 : 0, scanned.size());
  struct Fact {
    bool ok;
    const char* description;
  };
  const Fact facts[] = {
      {session.SnapshotLines().size() == 1, "terminal: a fresh session has one line"},
      {red.r == 0xc3, "ansi palette: basic color 1 is the expected red"},
      {!wake_delivered, "waker: a push with no registered channel reports undelivered"},
      {scanned.size() == 1, "file index: the one-file scan tree scanned as one file"},
  };
  bool ok = true;
  for (const Fact& fact : facts) {
    if (!fact.ok) {
      std::fprintf(stderr, "kernel link probe: wrong answer from %s\n", fact.description);
      ok = false;
    }
  }
  return ok ? 0 : 1;
}
