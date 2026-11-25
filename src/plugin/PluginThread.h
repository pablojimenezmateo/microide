#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <utility>

#include "plugin/PluginThreadTypes.h"
#include "util/MainThreadMailbox.h"
#include "util/SerialWorkQueue.h"

namespace microide::plugin {

class PluginThread {
 public:
  PluginThread() = default;
  ~PluginThread() = default;

  PluginThread(const PluginThread&) = delete;
  PluginThread& operator=(const PluginThread&) = delete;

  void SetWakeChannel(util::WakeChannel channel) { mailbox_.SetWakeChannel(channel); }

  void EnsureStarted() { inbound_.EnsureStarted(); }
  bool started() const { return inbound_.started(); }

  void Post(std::function<void()> task) { inbound_.Post(std::move(task)); }
  void PostLatest(std::string key, std::function<void()> task) {
    inbound_.PostLatest(std::move(key), std::move(task));
  }
  void PostFront(std::function<void()> task) { inbound_.PostFront(std::move(task)); }

  void PostToMain(PluginMainThreadAction action) { mailbox_.Post(std::move(action)); }

  int DrainMainThreadActions() { return mailbox_.Drain(); }

  void ClearMainThreadActions() { mailbox_.Clear(); }

  int PendingMainThreadActionCount() const { return mailbox_.PendingCount(); }

  void Shutdown(std::chrono::milliseconds deadline = std::chrono::milliseconds(2000)) {
    inbound_.Shutdown(deadline);
  }

  void Drain() { inbound_.Drain(); }

 private:
  util::MainThreadMailbox mailbox_;
  util::SerialWorkQueue inbound_{util::SerialWorkQueue::StartMode::kLazy, {}, {}, "plugin"};
};

}
