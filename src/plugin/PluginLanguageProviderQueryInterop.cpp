#include "plugin/PluginLanguageProviderQueryInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <limits>

#include "plugin/PluginLuaInterop.h"

namespace microide::plugin::language_provider_query_interop {
namespace {

// Bounds on plugin-supplied tables: results are read into host memory, so cap
// counts/depth before allocating. Oversize input is truncated, not rejected,
// so a misbehaving provider degrades rather than failing the query.
constexpr lua_Integer kMaxLocations = 4096;
// Shared per-query ceiling across ALL definition/reference providers for one navigation
// (TD-2026-07-17A-048): kMaxLocations bounds a single provider, but many matching
// providers could otherwise append kMaxLocations each with no aggregate bound. Once the
// harvest reaches this, later providers are skipped.
constexpr std::size_t kMaxAggregateLocations = 8192;
constexpr lua_Integer kMaxSignatures = 64;
constexpr lua_Integer kMaxParameters = 256;
constexpr int kMaxSymbolDepth = 32;
constexpr std::size_t kMaxSymbolNodes = 8192;

// Host maximum for a 1-based plugin coordinate. Matches the decoration path's
// over-uint32 rejection and the editor's uint32 document ceiling: a line/column above
// this is not a real navigation target. (TD-2026-07-16-70.)
constexpr std::uint64_t kMaxPluginCoordinate = std::numeric_limits<std::uint32_t>::max();

// Deterministic narrowing of a plugin-supplied Lua integer to int: clamps to
// [INT_MIN, INT_MAX] (never wraps). A bare static_cast of an out-of-int value is
// implementation-defined and can land IN range, selecting the wrong signature-help
// overload/parameter; clamping to INT_MAX instead makes the downstream out-of-range
// fallback fire. Mirrors LspProtocol's JsonIntInRange. (TD-2026-07-16-67.)
constexpr int ClampLuaIntegerToInt(lua_Integer raw) {
  if (raw < static_cast<lua_Integer>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  if (raw > static_cast<lua_Integer>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(raw);
}

// Reads a 1-based coordinate field. Returns false when the field is PRESENT as an
// integer but out of the valid host range (non-positive or above kMaxPluginCoordinate)
// so the caller drops the whole result rather than publish a location the editor later
// clamps to EOF. An absent/nil or non-integer field yields *out = 0 (unspecified) and
// returns true, preserving the previous leniency for omitted coordinates.
bool ReadOneBasedField(lua_State* state, int table_index, const char* field, std::size_t* out) {
  lua_interop::GetFieldProtected(state, table_index, field);
  bool ok = true;
  std::size_t value = 0;
  if (lua_isinteger(state, -1)) {
    const lua_Integer raw = lua_tointeger(state, -1);
    if (raw <= 0 || static_cast<std::uint64_t>(raw) > kMaxPluginCoordinate) {
      ok = false;  // present but out of the host coordinate range
    } else {
      value = static_cast<std::size_t>(raw);
    }
  }
  lua_pop(state, 1);
  *out = value;
  return ok;
}

// Reads a {path|file, line, column} location off the top of the stack. Returns
// false when no usable path is present.
bool ReadLocation(lua_State* state,
                  const std::filesystem::path& current_project_root,
                  const std::function<std::filesystem::path(const std::filesystem::path&,
                                                            const std::filesystem::path&)>&
                      resolve_runtime_path,
                  PluginHost::LocationResult* out) {
  std::string path = lua_interop::ReadStringField(state, -1, "path");
  if (path.empty()) {
    path = lua_interop::ReadStringField(state, -1, "file");
  }
  if (path.empty()) {
    return false;
  }
  out->path = resolve_runtime_path(current_project_root, std::filesystem::path(path));
  // Drop the whole location when a present coordinate is out of the host range, rather
  // than navigate to a clamped EOF position. (TD-2026-07-16-70.)
  if (!ReadOneBasedField(state, -1, "line", &out->line) ||
      !ReadOneBasedField(state, -1, "column", &out->column)) {
    return false;
  }
  return !out->path.empty();
}

void ReadSymbolArray(lua_State* state,
                     int depth,
                     std::size_t* total,
                     std::vector<PluginHost::DocumentSymbolNode>* out) {
  if (depth > kMaxSymbolDepth || *total >= kMaxSymbolNodes) {
    return;
  }
  // Each descent keeps the current element table and its "children" table live on
  // the Lua stack; reserve headroom before recursing. The harvest runs after PCall
  // disarmed the count-hook watchdog and there is no other lua_checkstack in this
  // path, so a deeply nested symbol tree would otherwise overrun the stack top.
  if (!lua_checkstack(state, 4)) {
    return;
  }
  // Bound the harvest with lua_rawlen and read entries with lua_rawgeti, mirroring
  // the sibling completion/code-action harvests: the result table arrives after
  // PCall cleared the count-hook, so the previous unbounded for(;;) + metamethod-
  // invoking lua_geti over an adversarial __index/__len would spin this worker
  // thread forever (and could longjmp past the native frame).
  const int array_index = lua_absindex(state, -1);
  // Clamp the iteration count, not just the accepted-node count: entries that are
  // non-tables or lack a "name" never bump *total, so a sparse-border table with a
  // huge lua_rawlen would otherwise defeat the *total < kMaxSymbolNodes guard and
  // spin this worker thread. Mirrors the sibling completion/code-action harvests.
  const lua_Integer count = std::min<lua_Integer>(
      static_cast<lua_Integer>(lua_rawlen(state, array_index)),
      static_cast<lua_Integer>(kMaxSymbolNodes));
  for (lua_Integer i = 1; i <= count && *total < kMaxSymbolNodes; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      continue;
    }
    PluginHost::DocumentSymbolNode node;
    node.name = lua_interop::ReadStringField(state, -1, "name");
    node.detail = lua_interop::ReadStringField(state, -1, "detail");
    node.kind = lua_interop::ReadStringField(state, -1, "kind");
    const bool coords_valid = ReadOneBasedField(state, -1, "line", &node.line) &&
                              ReadOneBasedField(state, -1, "column", &node.column);
    // Drop a symbol whose present coordinate is out of the host range (would navigate
    // the Outline row to a clamped EOF position). (TD-2026-07-16-70.)
    if (!node.name.empty() && coords_valid) {
      ++(*total);
      lua_interop::GetFieldProtected(state, -1, "children");
      if (lua_istable(state, -1)) {
        ReadSymbolArray(state, depth + 1, total, &node.children);
      }
      lua_pop(state, 1);
      out->push_back(std::move(node));
    }
    lua_pop(state, 1);
  }
}

}  // namespace

std::vector<PluginHost::LocationResult> QueryLocations(
    runtime_types::LanguageQueryKind kind,
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    bool include_declaration,
    const std::filesystem::path& current_project_root,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    const std::function<std::filesystem::path(const std::filesystem::path&,
                                              const std::filesystem::path&)>& resolve_runtime_path,
    std::string* error_message) {
  std::vector<PluginHost::LocationResult> results;
  const bool is_references = kind == runtime_types::LanguageQueryKind::References;
  for (const auto& provider : runtimes) {
    if (provider.kind != kind || provider.language_id != language_id) {
      continue;
    }
    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushPosition(state, line, column);
    int nargs = 2;
    if (is_references) {
      lua_pushboolean(state, include_declaration ? 1 : 0);
      nargs = 3;
    }
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(nargs, 1, &call_error)) {
      // Keep earlier providers' locations and keep scanning later providers instead of
      // making navigation look empty because one provider threw (TD-2026-07-17A-048).
      // Queries run with allow_registration=false, so `runtimes` cannot reallocate across
      // the PCall and `provider` stays valid for the error record.
      lua_interop::AppendProviderFailure(
          error_message, is_references ? "references" : "definition", provider.id, call_error);
      continue;
    }
    if (lua_istable(state, -1)) {
      for (lua_Integer i = 1;
           i <= kMaxLocations && results.size() < kMaxAggregateLocations; ++i) {
        // Raw read: the harvest runs after PCall disarmed the count-hook watchdog,
        // so a metamethod-invoking lua_geti over an adversarial __index/__len could
        // spin this worker thread forever. Mirrors ReadSymbolArray.
        lua_rawgeti(state, -1, i);
        if (lua_isnil(state, -1)) {
          lua_pop(state, 1);
          break;
        }
        if (lua_istable(state, -1)) {
          PluginHost::LocationResult location;
          if (ReadLocation(state, current_project_root, resolve_runtime_path, &location)) {
            results.push_back(std::move(location));
          }
        }
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
    if (results.size() >= kMaxAggregateLocations) {
      break;  // shared per-query ceiling reached; skip remaining providers
    }
  }
  return results;
}

bool QuerySignatureHelp(
    std::string_view language_id,
    const std::filesystem::path& path,
    std::size_t line,
    std::size_t column,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    PluginHost::SignatureHelpResult* result,
    std::string* error_message) {
  if (result == nullptr) {
    return false;
  }
  result->signatures.clear();
  result->active_signature = 0;
  for (const auto& provider : runtimes) {
    if (provider.kind != runtime_types::LanguageQueryKind::SignatureHelp ||
        provider.language_id != language_id) {
      continue;
    }
    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    lua_interop::PushPosition(state, line, column);
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(2, 1, &call_error)) {
      // A broken provider must not mask a healthy later one (TD-2026-07-17A-048/049):
      // record the failure and keep scanning ordered providers.
      lua_interop::AppendProviderFailure(error_message, "signature help", provider.id, call_error);
      continue;
    }
    if (lua_istable(state, -1)) {
      lua_interop::GetFieldProtected(state, -1, "active_signature");
      if (lua_isinteger(state, -1)) {
        result->active_signature = ClampLuaIntegerToInt(lua_tointeger(state, -1));
      }
      lua_pop(state, 1);
      lua_interop::GetFieldProtected(state, -1, "signatures");
      if (lua_istable(state, -1)) {
        for (lua_Integer i = 1; i <= kMaxSignatures; ++i) {
          // Raw read (post-PCall, watchdog disarmed); see QueryLocations above.
          lua_rawgeti(state, -1, i);
          if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            break;
          }
          if (lua_istable(state, -1)) {
            PluginHost::SignatureInfo signature;
            signature.label = lua_interop::ReadStringField(state, -1, "label");
            signature.documentation = lua_interop::ReadStringField(state, -1, "documentation");
            lua_interop::GetFieldProtected(state, -1, "active_parameter");
            if (lua_isinteger(state, -1)) {
              signature.active_parameter = ClampLuaIntegerToInt(lua_tointeger(state, -1));
            }
            lua_pop(state, 1);
            lua_interop::GetFieldProtected(state, -1, "parameters");
            if (lua_istable(state, -1)) {
              for (lua_Integer p = 1; p <= kMaxParameters; ++p) {
                // Raw read (post-PCall, watchdog disarmed); see QueryLocations above.
                lua_rawgeti(state, -1, p);
                if (lua_isnil(state, -1)) {
                  lua_pop(state, 1);
                  break;
                }
                PluginHost::SignatureParameter parameter;
                if (lua_isstring(state, -1)) {
                  parameter.label = lua_tostring(state, -1);
                } else if (lua_istable(state, -1)) {
                  parameter.label = lua_interop::ReadStringField(state, -1, "label");
                  parameter.documentation =
                      lua_interop::ReadStringField(state, -1, "documentation");
                }
                if (!parameter.label.empty()) {
                  signature.parameters.push_back(std::move(parameter));
                }
                lua_pop(state, 1);
              }
            }
            lua_pop(state, 1);
            if (!signature.label.empty()) {
              result->signatures.push_back(std::move(signature));
            }
          }
          lua_pop(state, 1);
        }
      }
      lua_pop(state, 1);
    }
    lua_pop(state, 1);
    if (!result->signatures.empty()) {
      // First provider with a usable signature wins, matching the LSP single-server model.
      break;
    }
  }
  return !result->signatures.empty();
}

std::vector<PluginHost::DocumentSymbolNode> QueryDocumentSymbols(
    std::string_view language_id,
    const std::filesystem::path& path,
    const std::vector<runtime_types::LanguageQueryRuntime>& runtimes,
    const std::function<const runtime_types::PluginInstance*(lua_State*)>& find_plugin_by_state,
    const std::function<void(lua_State*, const std::filesystem::path&)>& push_buffer_context,
    std::string* error_message) {
  std::vector<PluginHost::DocumentSymbolNode> results;
  for (const auto& provider : runtimes) {
    if (provider.kind != runtime_types::LanguageQueryKind::DocumentSymbol ||
        provider.language_id != language_id) {
      continue;
    }
    lua_State* state = provider.state;
    const lua_interop::StackResetGuard stack_guard(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, provider.provide_ref);
    push_buffer_context(state, path);
    const runtime_types::PluginInstance* plugin = find_plugin_by_state(state);
    std::string call_error;
    if (plugin == nullptr || !plugin->runtime || !plugin->runtime->PCall(1, 1, &call_error)) {
      // Keep scanning ordered providers so one broken provider can't mask a healthy
      // later one (TD-2026-07-17A-048/049).
      lua_interop::AppendProviderFailure(error_message, "document symbol", provider.id,
                                         call_error);
      continue;
    }
    if (lua_istable(state, -1)) {
      std::size_t total = 0;
      ReadSymbolArray(state, 0, &total, &results);
    }
    lua_pop(state, 1);
    if (!results.empty()) {
      break;
    }
  }
  return results;
}

}  // namespace microide::plugin::language_provider_query_interop

#endif
