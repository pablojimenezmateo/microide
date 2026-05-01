#pragma once

namespace microide::workspace {

struct PluginReloadRequest {
  bool syntax_definitions = true;
  bool replay_buffer_opens = true;
  bool open_lsp_documents = false;
};

}  // namespace microide::workspace
