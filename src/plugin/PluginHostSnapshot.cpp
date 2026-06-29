#include "plugin/PluginHostInternal.h"

namespace microide::plugin {

PluginHostSnapshot PluginHost::Impl::CaptureSnapshot() const {
  PluginHostSnapshot snapshot;
  // Read the published root (UI-thread-owned); the live current_project_root is
  // worker-owned and may be mid-rewrite during a detached reload.
  snapshot.project_root = published_.project_root;
  if (raw_callbacks.active_buffer) {
    if (const std::optional<PluginHost::ActiveBuffer> active = raw_callbacks.active_buffer();
        active.has_value() && !active->path.empty()) {
      snapshot.active_buffer = PluginHostSnapshot::ActiveBuffer{
          .path = active->path,
          .line = active->line,
          .column = active->column,
          .present = true,
      };
      // Stamp the active buffer's edit generation so a deferred async edit can be
      // dropped if the buffer advances before the mailbox drain applies it.
      snapshot.generation = active->content_revision;
    }
  }
  if (raw_callbacks.get_setting) {
    // Iterate the published (UI-thread-owned) setting specs, not the live
    // worker-owned `settings` vector which may be mid-rewrite during a detached
    // reload -- mirrors the published_.project_root read above.
    snapshot.settings.reserve(published_.settings.size());
    for (const auto& spec : published_.settings) {
      if (const std::optional<std::string> value = raw_callbacks.get_setting(spec.id);
          value.has_value()) {
        snapshot.settings.emplace_back(spec.id, *value);
      }
    }
  }
  return snapshot;
}

void PluginHost::Impl::RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn) {
  if (worker_ == nullptr) {
    // No worker wired: run inline with exclusive access (legacy behavior).
    ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/true, fn);
    return;
  }
  worker_->EnsureStarted();
  worker_->Post([this, snapshot = std::move(snapshot), fn = std::move(fn)]() mutable {
    ExecuteWithContext(&snapshot, /*direct=*/false, /*allow_registration=*/false, fn);
  });
}

}  // namespace microide::plugin
