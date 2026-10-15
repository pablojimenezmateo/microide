#pragma once

namespace microide::workspace {

struct PluginReloadRequest {
  bool syntax_definitions = true;
  bool replay_buffer_opens = true;
  bool open_lsp_documents = false;
  // The user asked for this reload (Reload Plugins, `plugins-reload`): its summary
  // is reported -- as command feedback and a toast -- when the reload COMPLETES.
  // Reporting it at dispatch read the previous reload's summary, because the
  // reload runs on the plugin worker (a plugin installed and reloaded at once
  // announced "Loaded 0 plugins", and the next reload "Loaded 1 plugin").
  bool announce = false;
};

}  // namespace microide::workspace
