#include "plugin/PluginDiagnosticsInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <cctype>
#include <vector>

#include "editor/DiagnosticsStore.h"

namespace microide::plugin::diagnostics_interop {
namespace {

std::filesystem::path ResolveRuntimePath(const std::filesystem::path& project_root,
                                         const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  if (path.is_absolute() || project_root.empty()) {
    return path.lexically_normal();
  }
  return (project_root / path).lexically_normal();
}

std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

bool ParseDiagnosticSeverity(std::string_view raw_value, editor::DiagnosticSeverity* severity) {
  if (severity == nullptr) {
    return false;
  }
  const std::string value = ToLowerAscii(raw_value);
  if (value == "error") {
    *severity = editor::DiagnosticSeverity::Error;
    return true;
  }
  if (value == "warning" || value == "warn") {
    *severity = editor::DiagnosticSeverity::Warning;
    return true;
  }
  if (value == "info" || value == "information") {
    *severity = editor::DiagnosticSeverity::Info;
    return true;
  }
  if (value == "hint") {
    *severity = editor::DiagnosticSeverity::Hint;
    return true;
  }
  return false;
}

bool ReadDiagnosticTable(lua_State* state,
                         int table_index,
                         editor::Diagnostic* diagnostic,
                         std::string* error_message) {
  if (diagnostic == nullptr) {
    if (error_message != nullptr) {
      *error_message = "diagnostic output pointer is required";
    }
    return false;
  }

  const int absolute_index = lua_absindex(state, table_index);

  lua_getfield(state, absolute_index, "message");
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = "diagnostic message must be a string";
    }
    lua_pop(state, 1);
    return false;
  }
  diagnostic->message = lua_tostring(state, -1);
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "line");
  if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
    if (error_message != nullptr) {
      *error_message = "diagnostic line must be a positive integer";
    }
    lua_pop(state, 1);
    return false;
  }
  const lua_Integer line = lua_tointeger(state, -1);
  lua_pop(state, 1);

  lua_getfield(state, absolute_index, "column");
  if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
    if (error_message != nullptr) {
      *error_message = "diagnostic column must be a positive integer";
    }
    lua_pop(state, 1);
    return false;
  }
  const lua_Integer column = lua_tointeger(state, -1);
  lua_pop(state, 1);

  lua_Integer end_line = line;
  lua_getfield(state, absolute_index, "end_line");
  if (lua_isinteger(state, -1)) {
    end_line = lua_tointeger(state, -1);
  }
  lua_pop(state, 1);

  lua_Integer end_column = column + 1;
  lua_getfield(state, absolute_index, "end_column");
  if (lua_isinteger(state, -1)) {
    end_column = lua_tointeger(state, -1);
  }
  lua_pop(state, 1);

  if (end_line <= 0 || end_column <= 0) {
    if (error_message != nullptr) {
      *error_message = "diagnostic end positions must be positive integers";
    }
    return false;
  }
  if (end_line < line || (end_line == line && end_column < column)) {
    if (error_message != nullptr) {
      *error_message = "diagnostic end position must not precede the start position";
    }
    return false;
  }

  diagnostic->range =
      editor::SelectionRange{
          .start = editor::TextPosition{
              .line = static_cast<std::size_t>(line - 1),
              .column = static_cast<std::size_t>(column - 1),
          },
          .end = editor::TextPosition{
              .line = static_cast<std::size_t>(end_line - 1),
              .column = static_cast<std::size_t>(end_column - 1),
          },
      };
  diagnostic->severity = editor::DiagnosticSeverity::Error;
  lua_getfield(state, absolute_index, "severity");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_isstring(state, -1) ||
      !ParseDiagnosticSeverity(lua_tostring(state, -1), &diagnostic->severity)) {
    if (error_message != nullptr) {
      *error_message = "diagnostic severity must be one of: error, warning, info, hint";
    }
    lua_pop(state, 1);
    return false;
  }
  lua_pop(state, 1);
  return true;
}

}  // namespace

bool PublishDiagnostics(lua_State* state,
                        std::string_view plugin_id,
                        const std::filesystem::path& current_project_root,
                        std::string_view raw_path,
                        int diagnostics_index,
                        const PluginHost::Callbacks& callbacks,
                        std::string* error_message) {
  if (!callbacks.publish_diagnostics) {
    if (error_message != nullptr) {
      *error_message = "diagnostics API unavailable";
    }
    return false;
  }

  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "diagnostic path must not be empty";
    }
    return false;
  }

  const int absolute_index = lua_absindex(state, diagnostics_index);
  const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, absolute_index));
  std::vector<editor::Diagnostic> diagnostics;
  diagnostics.reserve(static_cast<std::size_t>(count));
  for (lua_Integer i = 1; i <= count; ++i) {
    lua_rawgeti(state, absolute_index, i);
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) {
        *error_message = "diagnostic entries must be tables";
      }
      lua_pop(state, 1);
      return false;
    }
    editor::Diagnostic diagnostic;
    if (!ReadDiagnosticTable(state, -1, &diagnostic, error_message)) {
      lua_pop(state, 1);
      return false;
    }
    diagnostics.push_back(std::move(diagnostic));
    lua_pop(state, 1);
  }

  callbacks.publish_diagnostics(std::string(plugin_id), path, std::move(diagnostics));
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ClearDiagnostics(std::string_view plugin_id,
                      const std::optional<std::filesystem::path>& path,
                      const PluginHost::Callbacks& callbacks,
                      std::string* error_message) {
  if (path.has_value()) {
    if (!callbacks.clear_file_diagnostics) {
      if (error_message != nullptr) {
        *error_message = "diagnostics API unavailable";
      }
      return false;
    }
    callbacks.clear_file_diagnostics(std::string(plugin_id), path->lexically_normal());
  } else {
    if (!callbacks.clear_owner_diagnostics) {
      if (error_message != nullptr) {
        *error_message = "diagnostics API unavailable";
      }
      return false;
    }
    callbacks.clear_owner_diagnostics(std::string(plugin_id));
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

}  // namespace microide::plugin::diagnostics_interop

#endif
