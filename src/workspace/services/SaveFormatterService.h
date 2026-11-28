#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "platform/ProcessLauncher.h"
#include "project/FileIoServices.h"
#include "plugin/PluginHost.h"
#include "util/MainThreadMailbox.h"
#include "util/TaskExecutor.h"
#include "util/Waker.h"

namespace microide::workspace {

class SaveFormatterService {
 public:
  struct Request {
    std::vector<std::string> command;
    std::filesystem::path cwd;
    std::string text;
    std::string formatter_id;
    int timeout_ms = 5000;
    std::function<plugin::PluginHost::SaveParticipantOutcome()> participants;
  };

  struct Completion {
    std::uint64_t id = 0;
    std::string formatter_id;
    bool ok = false;
    bool timed_out = false;
    std::string formatted_text;
    std::string error_text;
    bool cancel_save = false;
    bool participants_timed_out = false;
  };

  SaveFormatterService() = default;
  SaveFormatterService(const SaveFormatterService&) = delete;
  SaveFormatterService& operator=(const SaveFormatterService&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  std::uint64_t Begin(std::string coalesce_key,
                      Request request,
                      const platform::ProcessLauncher& launcher,
                      std::function<void(Completion)> on_complete);

  Completion RunBlocking(Request request, const platform::ProcessLauncher& launcher);

  int DrainCompletions() { return mailbox_.Drain(); }

  void FlushPendingRuns();

  int PendingCount() const { return pending_.load(std::memory_order_acquire); }

 private:
  util::MainThreadMailbox mailbox_;
  std::atomic<int> pending_{0};
  std::atomic<std::uint64_t> next_id_{0};
  util::TaskExecutor executor_{1};
};

inline void SettleTabIoBeforeTeardown(SaveFormatterService& saves, project::FileIoServices& io) {
  saves.FlushPendingRuns();
  io.reads.CancelAllAndFlush();
  io.writes.Flush();
}

}
