#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "util/MainThreadMailbox.h"
#include "util/TaskExecutor.h"
#include "util/Waker.h"

namespace microide::workspace {

// Runs a save's contributed formatter OFF the shell thread.
//
// `editor.format_on_save` defaults to ON and the bundled prettier plugin registers a
// formatter for every web filetype, so the formatter subprocess — bounded only by a
// five-second cap — ran inline on the shell thread on every Ctrl+S. Starting node is
// a few hundred milliseconds; that is the whole window frozen, per save. It was also
// the single allowlisted exception to this repo's own "no synchronous subprocess in
// workspace" lint.
//
// The formatter is a pure function of (command, cwd, text), which is exactly why it
// is the piece that moves. Save participants stay on the shell thread — they are
// plugin calls that already hand off to the plugin worker — and so does the write,
// which keeps every existing invariant about who mutates a viewport single-threaded.
class SaveFormatterService {
 public:
  struct Request {
    std::vector<std::string> command;
    std::filesystem::path cwd;
    std::string text;
    std::string formatter_id;
    int timeout_ms = 5000;
  };

  struct Completion {
    std::uint64_t id = 0;
    std::string formatter_id;
    bool ok = false;
    bool timed_out = false;
    // The formatter's stdout. Empty means it produced nothing and the input text
    // stands, which is the same rule the inline path used.
    std::string formatted_text;
  };

  SaveFormatterService() = default;
  SaveFormatterService(const SaveFormatterService&) = delete;
  SaveFormatterService& operator=(const SaveFormatterService&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  // Post one formatter run. Returns the id its completion will carry; 0 when the
  // request names no command (nothing was posted).
  //
  // `on_complete` travels WITH the request rather than being bound once at startup.
  // A callback installed during shell initialization is a callback a code path that
  // skipped initialization does not have, and the failure mode there is the worst
  // kind: the formatter runs, the completion is dropped, and the save the user asked
  // for never writes. Carrying it per request makes that unrepresentable. It is
  // invoked on the SHELL thread, from DrainCompletions or FlushPendingRuns — never
  // from the worker — and exactly once per posted run.
  //
  // `coalesce_key` supersedes an earlier QUEUED run for the same subject — a second
  // Ctrl+S on a tab whose first run has not started yet drops the first, because its
  // text is already stale. A run that has started is left to finish; the caller's own
  // revision guard is what discards its output.
  std::uint64_t Begin(std::string coalesce_key,
                      Request request,
                      const platform::ProcessLauncher& launcher,
                      std::function<void(Completion)> on_complete);

  // Shell thread: run this formatter and wait for it. For a save whose caller cannot
  // proceed until the file is on disk — closing a tab, renaming, quitting. The spawn
  // still happens on the worker (forking from the UI thread with this address space
  // is itself a cost), but the wait is real, and it is the same wait the inline path
  // always paid.
  Completion RunBlocking(Request request, const platform::ProcessLauncher& launcher);

  // Shell thread: deliver every completion that has landed. Returns how many ran.
  int DrainCompletions() { return mailbox_.Drain(); }

  // Shell thread: block until no run is outstanding, then deliver their completions.
  // Everything that must not proceed with the file unwritten — closing a tab,
  // switching or closing a project, quitting, renaming — goes through here. It waits
  // only when a formatter is actually mid-run, so the common path costs a load and a
  // branch.
  void FlushPendingRuns();

  int PendingCount() const { return pending_.load(std::memory_order_acquire); }

 private:
  // One worker. The workload is one formatter at a time per save, and a second
  // thread would only let two formatters race to write the same buffer.
  util::TaskExecutor executor_{1};
  util::MainThreadMailbox mailbox_;
  std::atomic<int> pending_{0};
  std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace microide::workspace
