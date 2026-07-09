#include "plugin/PluginDiagnosticsInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <cctype>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginPathInterop.h"
#include "util/StringUtil.h"

namespace microide::plugin::diagnostics_interop {
namespace {

using path_interop::ResolveRuntimePath;

bool ParseDiagnosticSeverity(std::string_view raw_value, editor::DiagnosticSeverity* severity) {
  if (severity == nullptr) {
    return false;
  }
  const std::string value = util::ToLowerAscii(raw_value);
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

  const std::optional<std::string> message =
      lua_interop::ReadOptionalStringField(state, absolute_index, "message");
  if (!message.has_value()) {
    if (error_message != nullptr) {
      *error_message = "diagnostic message must be a string";
    }
    return false;
  }
  diagnostic->message = *message;

  const std::optional<lua_Integer> line_field =
      lua_interop::ReadOptionalIntegerField(state, absolute_index, "line");
  if (!line_field.has_value() || *line_field <= 0) {
    if (error_message != nullptr) {
      *error_message = "diagnostic line must be a positive integer";
    }
    return false;
  }
  const lua_Integer line = *line_field;

  const std::optional<lua_Integer> column_field =
      lua_interop::ReadOptionalIntegerField(state, absolute_index, "column");
  if (!column_field.has_value() || *column_field <= 0) {
    if (error_message != nullptr) {
      *error_message = "diagnostic column must be a positive integer";
    }
    return false;
  }
  const lua_Integer column = *column_field;

  const lua_Integer end_line =
      lua_interop::ReadOptionalIntegerField(state, absolute_index, "end_line").value_or(line);
  const lua_Integer end_column =
      lua_interop::ReadOptionalIntegerField(state, absolute_index, "end_column")
          .value_or(column + 1);

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
  lua_interop::GetFieldProtected(state, absolute_index, "severity");
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
  // Clamp against a sparse-border table making lua_rawlen overstate the length;
  // matches the DiagnosticsStore per-file cap so nothing beyond it survives anyway.
  constexpr lua_Integer kMaxPluginDiagnostics = 10000;
  const lua_Integer count =
      std::min<lua_Integer>(static_cast<lua_Integer>(lua_rawlen(state, absolute_index)),
                            kMaxPluginDiagnostics);
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
