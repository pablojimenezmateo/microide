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
    snapshot.settings = ResolveSettingsSnapshot();
  }
  return snapshot;
}

// Build (or reuse) the shared immutable resolved-settings block. The result is
// cached and only rebuilt when the host's settings revision advances (any setting
// mutation/reset/layer-bind) or the contributed setting specs change (a plugin
// reload bumps status_view_revision). This turns the per-call O(settings) resolve
// + copy on hot paths (cursor move, selection, buffer change, completions, hovers)
// into a shared-pointer copy when nothing changed -- TD-2026-07-17A-076.
std::shared_ptr<const ResolvedPluginSettings> PluginHost::Impl::ResolveSettingsSnapshot() const {
  // Without a revision signal the cache cannot be proven fresh, so fall back to
  // re-resolving each capture (never serve a possibly-stale cached value).
  const bool have_revision = static_cast<bool>(raw_callbacks.settings_revision);
  const std::uint64_t settings_revision = have_revision ? raw_callbacks.settings_revision() : 0;
  if (have_revision && cached_settings_ != nullptr &&
      cached_settings_revision_ == settings_revision &&
      cached_settings_specs_revision_ == status_view_revision) {
    return cached_settings_;
  }

  // Iterate the published (UI-thread-owned) setting specs, not the live
  // worker-owned `settings` vector which may be mid-rewrite during a detached
  // reload -- mirrors the published_.project_root read in CaptureSnapshot.
  auto resolved = std::make_shared<ResolvedPluginSettings>();
  resolved->reserve(published_.settings.size());
  for (const auto& spec : published_.settings) {
    if (const std::optional<std::string> value = raw_callbacks.get_setting(spec.id);
        value.has_value()) {
      resolved->emplace_back(spec.id, *value);
    }
  }

  std::shared_ptr<const ResolvedPluginSettings> immutable = std::move(resolved);
  if (have_revision) {
    cached_settings_ = immutable;
    cached_settings_revision_ = settings_revision;
    cached_settings_specs_revision_ = status_view_revision;
  }
  return immutable;
}

void PluginHost::Impl::RunOnWorkerDetached(PluginHostSnapshot snapshot, std::function<void()> fn,
                                           std::string coalesce_key) {
  if (worker_ == nullptr) {
    // No worker wired: run inline with exclusive access (legacy behavior).
    ExecuteWithContext(&snapshot, /*direct=*/true, /*allow_registration=*/true, fn);
    return;
  }
  worker_->EnsureStarted();
  auto task = [this, snapshot = std::move(snapshot), fn = std::move(fn)]() mutable {
    ExecuteWithContext(&snapshot, /*direct=*/false, /*allow_registration=*/false, fn);
  };
  // Latest-only when keyed: a superseding cursor/selection event drops the older queued
  // job for the same buffer so a slow plugin observes the latest state, not a backlog
  // (TD-2026-07-17A-078). Keyed jobs still append at the tail, so a preceding FIFO
  // buffer_change stays ahead of a later coalesced cursor move.
  if (coalesce_key.empty()) {
    worker_->Post(std::move(task));
  } else {
    worker_->PostLatest(std::move(coalesce_key), std::move(task));
  }
}

}  // namespace microide::plugin
