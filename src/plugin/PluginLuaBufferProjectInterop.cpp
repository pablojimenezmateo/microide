#include "plugin/PluginLuaBufferProjectInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include "plugin/PluginPathInterop.h"

#include <new>
#include <vector>

namespace microide::plugin::lua_buffer_project_interop {

namespace {

constexpr const char* kBufferTextMetatable = "microide.buffer_text";

// The userdata behind a buffer table's text methods. Owns the snapshot; the line
// index is built on the first `line()` / `line_count()` and kept for the rest.
struct BufferText {
  std::shared_ptr<const std::string> text;
  std::vector<std::size_t> line_starts;  // byte offset of each line; empty until indexed

  void IndexLines() {
    if (!line_starts.empty()) {
      return;
    }
    line_starts.push_back(0);
    const std::string& value = *text;
    for (std::size_t i = value.find('\n'); i != std::string::npos; i = value.find('\n', i + 1)) {
      line_starts.push_back(i + 1);
    }
  }
};

BufferText* CheckBufferText(lua_State* state) {
  return static_cast<BufferText*>(lua_touserdata(state, lua_upvalueindex(1)));
}

int BufferTextGc(lua_State* state) {
  static_cast<BufferText*>(lua_touserdata(state, 1))->~BufferText();
  return 0;
}

int BufferTextText(lua_State* state) {
  const BufferText* buffer = CheckBufferText(state);
  lua_pushlstring(state, buffer->text->data(), buffer->text->size());
  return 1;
}

// `buffer:line(n)` and `buffer.line(n)` both work: the line number is the last
// argument either way.
int BufferTextLine(lua_State* state) {
  const lua_Integer line = luaL_checkinteger(state, lua_gettop(state));
  BufferText* buffer = CheckBufferText(state);
  buffer->IndexLines();
  if (line < 1 || static_cast<std::size_t>(line) > buffer->line_starts.size()) {
    lua_pushnil(state);
    return 1;
  }
  const std::string& value = *buffer->text;
  const std::size_t start = buffer->line_starts[static_cast<std::size_t>(line) - 1];
  std::size_t end = static_cast<std::size_t>(line) < buffer->line_starts.size()
                        ? buffer->line_starts[static_cast<std::size_t>(line)] - 1
                        : value.size();
  if (end > start && value[end - 1] == '\r') {
    --end;  // a CRLF document's line ending
  }
  lua_pushlstring(state, value.data() + start, end - start);
  return 1;
}

int BufferTextLineCount(lua_State* state) {
  BufferText* buffer = CheckBufferText(state);
  buffer->IndexLines();
  lua_pushinteger(state, static_cast<lua_Integer>(buffer->line_starts.size()));
  return 1;
}

}  // namespace

void PushProjectTable(lua_State* state, const std::filesystem::path& project_root) {
  lua_createtable(state, 0, 2);
  lua_pushstring(state, project_root.generic_string().c_str());
  lua_setfield(state, -2, "root");
  lua_pushstring(state, path_interop::Basename(project_root).c_str());
  lua_setfield(state, -2, "name");
}

void PushBufferTable(lua_State* state,
                     const std::filesystem::path& path,
                     const std::function<std::optional<std::string>(
                         const std::filesystem::path&)>& relative_path_string) {
  lua_createtable(state, 0, 3);
  const std::filesystem::path normalized_path = path.lexically_normal();
  lua_pushstring(state, normalized_path.generic_string().c_str());
  lua_setfield(state, -2, "path");
  lua_pushstring(state, normalized_path.filename().string().c_str());
  lua_setfield(state, -2, "name");
  if (const std::optional<std::string> relative = relative_path_string(normalized_path);
      relative.has_value()) {
    lua_pushstring(state, relative->c_str());
    lua_setfield(state, -2, "relative_path");
  }
}

// No C++ local with a destructor is live across a call that can raise (the
// project links the C build of Lua; see src/plugin/LuaError.h): the snapshot is
// taken by reference and copied straight into the userdata, which has its __gc
// before anything else can fail.
void PushBufferTextMethods(lua_State* state, const std::shared_ptr<const std::string>& text) {
  if (text == nullptr) {
    return;
  }
  if (luaL_newmetatable(state, kBufferTextMetatable) != 0) {
    lua_pushcfunction(state, &BufferTextGc);
    lua_setfield(state, -2, "__gc");
  }
  void* storage = lua_newuserdatauv(state, sizeof(BufferText), 0);
  new (storage) BufferText{.text = text, .line_starts = {}};
  lua_pushvalue(state, -2);
  lua_setmetatable(state, -2);
  lua_remove(state, -2);  // the metatable
  // table, userdata: each method closes over the userdata.
  const struct {
    const char* name;
    lua_CFunction function;
  } kMethods[] = {
      {"text", &BufferTextText},
      {"line", &BufferTextLine},
      {"line_count", &BufferTextLineCount},
  };
  for (const auto& method : kMethods) {
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, method.function, 1);
    lua_setfield(state, -3, method.name);
  }
  lua_pop(state, 1);
}

}  // namespace microide::plugin::lua_buffer_project_interop

#endif
