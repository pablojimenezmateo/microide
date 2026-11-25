#include "plugin/PluginHostInternal.h"

namespace microide::plugin {

PluginHostSnapshot PluginHost::Impl::CaptureSnapshot() const {
  PluginHostSnapshot snapshot;
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
      snapshot.generation = active->content_revision;
    }
  }
  if (raw_callbacks.get_setting) {
    snapshot.settings = ResolveSettingsSnapshot();
  }
  return snapshot;
}

std::shared_ptr<const ResolvedPluginSettings> PluginHost::Impl::ResolveSettingsSnapshot() const {
  const bool have_revision = static_cast<bool>(raw_callbacks.settings_revision);
  const std::uint64_t settings_revision = have_revision ? raw_callbacks.settings_revision() : 0;
  if (have_revision && cached_settings_ != nullptr &&
      cached_settings_revision_ == settings_revision &&
      cached_settings_specs_revision_ == status_view_revision) {
    return cached_settings_;
  }

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
    ExecuteWithContext(&snapshot, true, true, fn);
    return;
  }
  worker_->EnsureStarted();
  auto task = [this, snapshot = std::move(snapshot), fn = std::move(fn)]() mutable {
    ExecuteWithContext(&snapshot, false, false, fn);
  };
  if (coalesce_key.empty()) {
    worker_->Post(std::move(task));
  } else {
    worker_->PostLatest(std::move(coalesce_key), std::move(task));
  }
}

}
