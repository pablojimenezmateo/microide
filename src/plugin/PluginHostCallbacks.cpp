#include "plugin/PluginHostInternal.h"

namespace microide::plugin {

PluginHost::Callbacks PluginHost::Impl::BuildRoutedCallbacks() {
  Callbacks routed;

  // Each routed wrapper is assigned only when the corresponding raw callback is
  // wired, so existence checks (`if (callbacks.x)`) keep their original meaning.

  // Reads resolve from the per-call snapshot (or live shell, on the inline path).
  if (raw_callbacks.active_buffer) {
    routed.active_buffer = [this]() { return ResolveActiveBuffer(); };
  }
  if (raw_callbacks.get_setting) {
    routed.get_setting = [this](std::string_view id) { return ResolveSetting(id); };
  }
  if (raw_callbacks.is_command_name_available) {
    // Consulted only during setup-time registration (which a detached reload runs
    // with allow_registration set even though shell writes are deferred).
    routed.is_command_name_available = [this](std::string_view name) -> bool {
      return g_exec.allow_registration && raw_callbacks.is_command_name_available(name);
    };
  }

  // Requests/writes: run directly when the worker holds exclusive shell access,
  // otherwise marshal an owning copy to the UI thread via the mailbox. Closures
  // never capture a lua_State / Lua ref.
  if (raw_callbacks.open_file) {
    routed.open_file = [this](const OpenFileRequest& request) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.open_file(request);
      }
      ApplyHostMutation([this, request]() { raw_callbacks.open_file(request); });
      return true;
    };
  }
  if (raw_callbacks.show_sidebar) {
    routed.show_sidebar = [this](std::string_view id) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.show_sidebar(id);
      }
      ApplyHostMutation([this, id = std::string(id)]() { raw_callbacks.show_sidebar(id); });
      return true;
    };
  }
  if (raw_callbacks.publish_diagnostics) {
    routed.publish_diagnostics = [this](std::string_view owner, const std::filesystem::path& path,
                                        std::vector<editor::Diagnostic> diagnostics) {
      ApplyHostMutation([this, owner = std::string(owner), path,
                         diagnostics = std::move(diagnostics)]() mutable {
        raw_callbacks.publish_diagnostics(owner, path, std::move(diagnostics));
      });
    };
  }
  if (raw_callbacks.clear_file_diagnostics) {
    routed.clear_file_diagnostics = [this](std::string_view owner,
                                           const std::filesystem::path& path) {
      ApplyHostMutation([this, owner = std::string(owner), path]() {
        raw_callbacks.clear_file_diagnostics(owner, path);
      });
    };
  }
  if (raw_callbacks.clear_owner_diagnostics) {
    routed.clear_owner_diagnostics = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_diagnostics(owner); });
    };
  }
  if (raw_callbacks.publish_decorations) {
    routed.publish_decorations = [this](std::string_view owner, const std::filesystem::path& path,
                                        editor::PluginDecorationData decorations) {
      ApplyHostMutation([this, owner = std::string(owner), path,
                         decorations = std::move(decorations)]() mutable {
        raw_callbacks.publish_decorations(owner, path, std::move(decorations));
      });
    };
  }
  if (raw_callbacks.clear_file_decorations) {
    routed.clear_file_decorations = [this](std::string_view owner,
                                           const std::filesystem::path& path) {
      ApplyHostMutation([this, owner = std::string(owner), path]() {
        raw_callbacks.clear_file_decorations(owner, path);
      });
    };
  }
  if (raw_callbacks.clear_owner_decorations) {
    routed.clear_owner_decorations = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_decorations(owner); });
    };
  }
  if (raw_callbacks.publish_surface) {
    routed.publish_surface = [this](std::string_view owner, std::string_view surface_id,
                                    editor::SurfaceContent content) {
      ApplyHostMutation([this, owner = std::string(owner), surface_id = std::string(surface_id),
                         content = std::move(content)]() mutable {
        raw_callbacks.publish_surface(owner, surface_id, std::move(content));
      });
    };
  }
  if (raw_callbacks.clear_surface) {
    routed.clear_surface = [this](std::string_view owner, std::string_view surface_id) {
      ApplyHostMutation(
          [this, owner = std::string(owner), surface_id = std::string(surface_id)]() {
            raw_callbacks.clear_surface(owner, surface_id);
          });
    };
  }
  if (raw_callbacks.clear_owner_surfaces) {
    routed.clear_owner_surfaces = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_owner_surfaces(owner); });
    };
  }
  if (raw_callbacks.decode_raster) {
    routed.decode_raster = [this](std::uint64_t hash, int format, std::vector<std::byte> bytes,
                                  int width, int height) {
      ApplyHostMutation([this, hash, format, bytes = std::move(bytes), width, height]() mutable {
        raw_callbacks.decode_raster(hash, format, std::move(bytes), width, height);
      });
    };
  }
  if (raw_callbacks.apply_workspace_edit) {
    routed.apply_workspace_edit = [this](std::string_view owner,
                                         const WorkspaceEditRequest& request) -> bool {
      if (g_exec.direct) {
        return raw_callbacks.apply_workspace_edit(owner, request);
      }
      // Posted, not awaited: stamp the capturing snapshot's buffer identity and
      // edit generation so the UI thread re-validates against the live buffer at
      // apply time and drops the edit if stale. Report optimistic acceptance.
      WorkspaceEditRequest guarded = request;
      if (g_exec.snapshot != nullptr && g_exec.snapshot->active_buffer.present) {
        guarded.has_staleness_guard = true;
        guarded.guard_path = g_exec.snapshot->active_buffer.path;
        guarded.captured_content_revision = g_exec.snapshot->generation;
      }
      ApplyHostMutation([this, owner = std::string(owner), guarded = std::move(guarded)]() {
        raw_callbacks.apply_workspace_edit(owner, guarded);
      });
      return true;
    };
  }
  if (raw_callbacks.publish_ghost_text) {
    routed.publish_ghost_text = [this](std::string_view owner, const GhostTextRequest& request) {
      ApplyHostMutation([this, owner = std::string(owner), request]() {
        raw_callbacks.publish_ghost_text(owner, request);
      });
    };
  }
  if (raw_callbacks.clear_ghost_text) {
    routed.clear_ghost_text = [this](std::string_view owner) {
      ApplyHostMutation(
          [this, owner = std::string(owner)]() { raw_callbacks.clear_ghost_text(owner); });
    };
  }
  if (raw_callbacks.error_sink) {
    routed.error_sink = [this](const std::string& message) {
      ApplyHostMutation([this, message]() { raw_callbacks.error_sink(message); });
    };
  }
  if (raw_callbacks.log_sink) {
    routed.log_sink = [this](const std::string& message) {
      ApplyHostMutation([this, message]() { raw_callbacks.log_sink(message); });
    };
  }
  if (raw_callbacks.request_status_redraw) {
    routed.request_status_redraw = [this]() {
      ApplyHostMutation([this]() { raw_callbacks.request_status_redraw(); });
    };
  }
  if (raw_callbacks.show_notification) {
    routed.show_notification = [this](const std::string& level, const std::string& message) {
      ApplyHostMutation([this, level, message]() { raw_callbacks.show_notification(level, message); });
    };
  }

  return routed;
}

}  // namespace microide::plugin
