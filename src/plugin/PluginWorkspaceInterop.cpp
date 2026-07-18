#include "plugin/PluginWorkspaceInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <system_error>

#include "plugin/PluginPathInterop.h"
#include "util/TextFileIO.h"

namespace microide::plugin::workspace_interop {
namespace {

using path_interop::ContainPath;
using path_interop::ResolveRuntimePath;

// Builds the set of roots a plugin may reach for a given access level: always the project
// root, plus the plugin's data directory when the level grants data access. Empty roots are
// preserved in the array and skipped by ContainPath.
std::array<std::filesystem::path, 2> AllowedRoots(const PluginFsContext& fs, FsAccess level) {
  std::array<std::filesystem::path, 2> roots;
  roots[0] = fs.project_root;
  if (level == FsAccess::kProjectAndData) {
    roots[1] = fs.data_dir;
  }
  return roots;
}

// Resolves `raw_path` against the project root and contains it within the roots permitted by
// `level`. Returns nullopt (and marks `denied`) when the level is kNone or the path escapes.
std::optional<std::filesystem::path> ResolveContained(const PluginFsContext& fs,
                                                      FsAccess level,
                                                      const char* raw_path,
                                                      bool* denied) {
  if (level == FsAccess::kNone) {
    if (denied != nullptr) {
      *denied = true;
    }
    return std::nullopt;
  }
  const std::filesystem::path resolved =
      ResolveRuntimePath(fs.project_root, std::filesystem::path(raw_path));
  const std::array<std::filesystem::path, 2> roots = AllowedRoots(fs, level);
  std::optional<std::filesystem::path> contained = ContainPath(std::span(roots), resolved);
  if (!contained.has_value() && denied != nullptr) {
    *denied = true;
  }
  return contained;
}

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& current_project_root,
                     const std::filesystem::path& path) {
  lua_createtable(state, 0, 3);
  const std::filesystem::path normalized_path = path.lexically_normal();
  lua_pushstring(state, normalized_path.generic_string().c_str());
  lua_setfield(state, -2, "path");
  lua_pushstring(state, normalized_path.filename().string().c_str());
  lua_setfield(state, -2, "name");

  if (!current_project_root.empty()) {
    const std::filesystem::path relative =
        normalized_path.lexically_relative(current_project_root.lexically_normal());
    const bool starts_with_parent =
        relative.begin() != relative.end() &&
        *relative.begin() == std::filesystem::path("..");
    if (!relative.empty() && !starts_with_parent) {
      lua_pushstring(state, relative.generic_string().c_str());
      lua_setfield(state, -2, "relative_path");
    }
  }
}

}  // namespace

int LuaWorkspaceProjectRoot(lua_State* state, const std::filesystem::path& current_project_root) {
  if (current_project_root.empty()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushstring(state, current_project_root.generic_string().c_str());
  return 1;
}

int LuaWorkspaceOpenFile(lua_State* state,
                         const std::filesystem::path& current_project_root,
                         const PluginHost::Callbacks& callbacks) {
  const char* raw_path = luaL_checkstring(state, 1);
  // Read all longjmp-capable arguments before constructing the std::filesystem::path
  // local: luaL_optinteger raises on a present-but-non-numeric argument, and that
  // error is a C longjmp that would skip `path`'s destructor.
  const lua_Integer line = luaL_optinteger(state, 2, 0);
  const lua_Integer column = luaL_optinteger(state, 3, 0);
  if (!callbacks.open_file) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  if (path.empty()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, callbacks.open_file(PluginHost::OpenFileRequest{
                              .path = path,
                              .line = line > 0 ? static_cast<std::size_t>(line) : 0,
                              .column = column > 0 ? static_cast<std::size_t>(column) : 0,
                          })
                              ? 1
                              : 0);
  return 1;
}

int LuaWorkspaceActiveBuffer(lua_State* state,
                             const std::filesystem::path& current_project_root,
                             const PluginHost::Callbacks& callbacks) {
  if (!callbacks.active_buffer) {
    lua_pushnil(state);
    return 1;
  }
  const std::optional<PluginHost::ActiveBuffer> active_buffer = callbacks.active_buffer();
  if (!active_buffer.has_value() || active_buffer->path.empty()) {
    lua_pushnil(state);
    return 1;
  }
  PushBufferTable(state, current_project_root, active_buffer->path);
  lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->line));
  lua_setfield(state, -2, "line");
  lua_pushinteger(state, static_cast<lua_Integer>(active_buffer->column));
  lua_setfield(state, -2, "column");
  return 1;
}

// TD-2026-07-17A-039: explicit byte ceilings for the plugin filesystem API. A plugin with
// project/data filesystem capability could otherwise duplicate a very large file in host
// memory (read) or push a very large atomic write through the host helper (write) before
// Lua's own allocator budget is visible. 16 MiB is far beyond any real plugin snippet
// workload; over-budget calls fail soft (nil/false), matching the existing
// denied/unreadable contract, rather than raising (avoids the no-longjmp-over-C++-locals
// dance for a size guard).
constexpr std::uintmax_t kMaxPluginFileReadBytes = 16ull * 1024 * 1024;
constexpr std::size_t kMaxPluginFileWriteBytes = 16ull * 1024 * 1024;

int LuaFilesReadText(lua_State* state, const PluginFsContext& fs, bool* denied) {
  const char* raw_path = luaL_checkstring(state, 1);
  const std::optional<std::filesystem::path> path =
      ResolveContained(fs, fs.caps.fs_read, raw_path, denied);
  if (!path.has_value()) {
    lua_pushnil(state);
    return 1;
  }
  // Refuse before the whole-file allocation when the file is over the plugin read budget.
  std::error_code size_ec;
  const std::uintmax_t file_size = std::filesystem::file_size(*path, size_ec);
  if (!size_ec && file_size > kMaxPluginFileReadBytes) {
    lua_pushnil(state);
    return 1;
  }
  const std::optional<std::string> text = util::ReadTextFile(*path);
  if (!text.has_value()) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushlstring(state, text->c_str(), text->size());
  return 1;
}

int LuaFilesWriteText(lua_State* state, const PluginFsContext& fs, bool* denied) {
  const char* raw_path = luaL_checkstring(state, 1);
  size_t text_length = 0;
  const char* text = luaL_checklstring(state, 2, &text_length);
  // Reject an over-budget write before resolving containment / copying into the host.
  if (text_length > kMaxPluginFileWriteBytes) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::optional<std::filesystem::path> path =
      ResolveContained(fs, fs.caps.fs_write, raw_path, denied);
  if (!path.has_value()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(
      state, util::WriteTextFileAtomically(*path, std::string_view(text, text_length)) ? 1 : 0);
  return 1;
}

int LuaFilesExists(lua_State* state, const PluginFsContext& fs, bool* denied) {
  const char* raw_path = luaL_checkstring(state, 1);
  const std::optional<std::filesystem::path> path =
      ResolveContained(fs, fs.caps.fs_read, raw_path, denied);
  if (!path.has_value()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  std::error_code error;
  lua_pushboolean(state, std::filesystem::exists(*path, error) && !error ? 1 : 0);
  return 1;
}

}  // namespace microide::plugin::workspace_interop

#endif
