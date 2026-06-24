#include "plugin/PluginDecorationInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/GutterIconRegistry.h"
#include "editor/PluginDecorationStore.h"
#include "plugin/PluginPathInterop.h"
#include "util/Hex.h"

namespace microide::plugin::decoration_interop {
namespace {

using path_interop::ResolveRuntimePath;

// Cap per-kind entries so a malformed or hostile plugin cannot force an
// unbounded allocation/sort on the shell thread.
constexpr lua_Integer kMaxEntriesPerKind = 100000;

std::optional<SDL_Color> ParseColor(std::string_view text) {
  if (text.size() == 7 && text.front() == '#') {
    const auto rgb = util::DecodeHexColor(text);
    if (!rgb) {
      return std::nullopt;
    }
    return SDL_Color{(*rgb)[0], (*rgb)[1], (*rgb)[2], 255};
  }
  if (text.size() == 9 && text.front() == '#') {
    const auto r = util::ParseHexByte(text[1], text[2]);
    const auto g = util::ParseHexByte(text[3], text[4]);
    const auto b = util::ParseHexByte(text[5], text[6]);
    const auto a = util::ParseHexByte(text[7], text[8]);
    if (!r || !g || !b || !a) {
      return std::nullopt;
    }
    return SDL_Color{*r, *g, *b, *a};
  }
  return std::nullopt;
}

bool ReadBoolField(lua_State* state, int table_index, const char* key) {
  lua_getfield(state, table_index, key);
  const bool value = lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
  return value;
}

// Reads an optional color field. Missing => keeps `out` untouched and returns
// true; a present-but-malformed value is an error.
bool ReadOptionalColorField(lua_State* state, int table_index, const char* key, SDL_Color* out,
                            std::string* error_message) {
  lua_getfield(state, table_index, key);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_isstring(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " must be a hex color string";
    }
    lua_pop(state, 1);
    return false;
  }
  const auto color = ParseColor(lua_tostring(state, -1));
  lua_pop(state, 1);
  if (!color) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " must be #rrggbb or #rrggbbaa";
    }
    return false;
  }
  *out = *color;
  return true;
}

// Reads a required positive (1-based) integer field into a 0-based uint32.
bool ReadOneBasedField(lua_State* state, int table_index, const char* key, std::uint32_t* out,
                       std::string* error_message) {
  lua_getfield(state, table_index, key);
  if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " must be a positive integer";
    }
    lua_pop(state, 1);
    return false;
  }
  *out = static_cast<std::uint32_t>(lua_tointeger(state, -1) - 1);
  lua_pop(state, 1);
  return true;
}

bool ReadStringField(lua_State* state, int table_index, const char* key, std::string* out) {
  lua_getfield(state, table_index, key);
  if (lua_isstring(state, -1)) {
    *out = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  return !out->empty();
}

// Begin iterating an optional array field `key`. Returns false (with the field
// already popped) when absent; otherwise leaves the array table on the stack at
// the returned absolute index and reports its length.
bool BeginArrayField(lua_State* state, int table_index, const char* key, int* array_index,
                     lua_Integer* count, std::string* error_message) {
  lua_getfield(state, table_index, key);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    *count = 0;
    return false;
  }
  if (!lua_istable(state, -1)) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " must be an array";
    }
    lua_pop(state, 1);
    *count = -1;  // signal error to caller
    return false;
  }
  *array_index = lua_absindex(state, -1);
  *count = static_cast<lua_Integer>(lua_rawlen(state, *array_index));
  if (*count > kMaxEntriesPerKind) {
    if (error_message != nullptr) {
      *error_message = std::string(key) + " exceeds the maximum decoration count";
    }
    lua_pop(state, 1);
    *count = -1;
    return false;
  }
  return true;
}

bool ReadTextStyles(lua_State* state, int table_index, editor::PluginDecorationData* data,
                    std::string* error_message) {
  int array_index = 0;
  lua_Integer count = 0;
  if (!BeginArrayField(state, table_index, "text_styles", &array_index, &count, error_message)) {
    return count >= 0;
  }
  data->text_styles.reserve(static_cast<std::size_t>(count));
  bool ok = true;
  for (lua_Integer i = 1; i <= count && ok; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) *error_message = "text_styles entries must be tables";
      ok = false;
    } else {
      const int entry = lua_absindex(state, -1);
      editor::TextStyleDecoration ts;
      const bool whole = ReadBoolField(state, entry, "whole_line");
      if (whole) {
        ts.flags |= editor::kDecorationWholeLine;
      }
      if (!ReadOneBasedField(state, entry, "line", &ts.line, error_message)) {
        ok = false;
      } else if (!whole &&
                 (!ReadOneBasedField(state, entry, "start_col", &ts.start_column, error_message) ||
                  !ReadOneBasedField(state, entry, "end_col", &ts.end_column, error_message))) {
        ok = false;
      } else if (!ReadOptionalColorField(state, entry, "fg", &ts.foreground, error_message) ||
                 !ReadOptionalColorField(state, entry, "bg", &ts.background, error_message) ||
                 !ReadOptionalColorField(state, entry, "line_color", &ts.line_color,
                                         error_message)) {
        ok = false;
      } else {
        if (ReadBoolField(state, entry, "underline")) ts.flags |= editor::kDecorationUnderline;
        if (ReadBoolField(state, entry, "strike")) ts.flags |= editor::kDecorationStrikethrough;
        if (ReadBoolField(state, entry, "bold")) ts.flags |= editor::kDecorationBold;
        if (ReadBoolField(state, entry, "italic")) ts.flags |= editor::kDecorationItalic;
        data->text_styles.push_back(std::move(ts));
      }
    }
    lua_pop(state, 1);  // entry
  }
  lua_pop(state, 1);  // array
  return ok;
}

bool ReadGutterMarks(lua_State* state, int table_index, editor::PluginDecorationData* data,
                     std::string* error_message) {
  int array_index = 0;
  lua_Integer count = 0;
  if (!BeginArrayField(state, table_index, "gutter_marks", &array_index, &count, error_message)) {
    return count >= 0;
  }
  data->gutter_marks.reserve(static_cast<std::size_t>(count));
  bool ok = true;
  for (lua_Integer i = 1; i <= count && ok; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) *error_message = "gutter_marks entries must be tables";
      ok = false;
    } else {
      const int entry = lua_absindex(state, -1);
      editor::GutterMarkDecoration mark;
      std::string icon;
      if (!ReadOneBasedField(state, entry, "line", &mark.line, error_message)) {
        ok = false;
      } else if (!ReadStringField(state, entry, "icon", &icon)) {
        if (error_message != nullptr) *error_message = "gutter_marks icon must be a string";
        ok = false;
      } else if (const auto shape = editor::GutterIconRegistry::ResolveShape(icon); !shape) {
        if (error_message != nullptr) *error_message = "unknown gutter icon: " + icon;
        ok = false;
      } else {
        mark.shape = *shape;
        mark.color = SDL_Color{255, 255, 255, 255};
        if (!ReadOptionalColorField(state, entry, "color", &mark.color, error_message)) {
          ok = false;
        } else {
          lua_getfield(state, entry, "priority");
          if (lua_isinteger(state, -1)) {
            const lua_Integer p = lua_tointeger(state, -1);
            mark.priority = static_cast<std::uint8_t>(p < 0 ? 0 : (p > 255 ? 255 : p));
          }
          lua_pop(state, 1);
          data->gutter_marks.push_back(std::move(mark));
        }
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return ok;
}

bool ReadInlineText(lua_State* state, int table_index, editor::PluginDecorationData* data,
                    std::string* error_message) {
  int array_index = 0;
  lua_Integer count = 0;
  if (!BeginArrayField(state, table_index, "inline_text", &array_index, &count, error_message)) {
    return count >= 0;
  }
  data->inline_texts.reserve(static_cast<std::size_t>(count));
  bool ok = true;
  for (lua_Integer i = 1; i <= count && ok; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) *error_message = "inline_text entries must be tables";
      ok = false;
    } else {
      const int entry = lua_absindex(state, -1);
      editor::InlineTextDecoration inl;
      if (!ReadOneBasedField(state, entry, "line", &inl.line, error_message)) {
        ok = false;
      } else if (!ReadStringField(state, entry, "text", &inl.text)) {
        if (error_message != nullptr) *error_message = "inline_text text must be a non-empty string";
        ok = false;
      } else if (!ReadOptionalColorField(state, entry, "color", &inl.color, error_message) ||
                 !ReadOptionalColorField(state, entry, "bg", &inl.background, error_message)) {
        ok = false;
      } else {
        // `eol` (default true) anchors at end of line; otherwise `col` (1-based).
        lua_getfield(state, entry, "eol");
        const bool eol = lua_isnil(state, -1) ? true : (lua_toboolean(state, -1) != 0);
        lua_pop(state, 1);
        if (eol) {
          inl.anchor_column = editor::kInlineTextEndOfLine;
        } else if (!ReadOneBasedField(state, entry, "col", &inl.anchor_column, error_message)) {
          ok = false;
        }
        if (ok) {
          data->inline_texts.push_back(std::move(inl));
        }
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return ok;
}

bool ReadCodeLenses(lua_State* state, int table_index, editor::PluginDecorationData* data,
                    std::string* error_message) {
  int array_index = 0;
  lua_Integer count = 0;
  if (!BeginArrayField(state, table_index, "code_lenses", &array_index, &count, error_message)) {
    return count >= 0;
  }
  data->code_lenses.reserve(static_cast<std::size_t>(count));
  bool ok = true;
  for (lua_Integer i = 1; i <= count && ok; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      if (error_message != nullptr) *error_message = "code_lenses entries must be tables";
      ok = false;
    } else {
      const int entry = lua_absindex(state, -1);
      editor::CodeLensDecoration lens;
      if (!ReadOneBasedField(state, entry, "line", &lens.line, error_message)) {
        ok = false;
      } else if (!ReadStringField(state, entry, "text", &lens.text)) {
        if (error_message != nullptr) *error_message = "code_lenses text must be a non-empty string";
        ok = false;
      } else {
        ReadStringField(state, entry, "command", &lens.command);
        data->code_lenses.push_back(std::move(lens));
      }
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);
  return ok;
}

}  // namespace

bool PublishDecorations(lua_State* state,
                        std::string_view plugin_id,
                        const std::filesystem::path& current_project_root,
                        std::string_view raw_path,
                        int table_index,
                        const PluginHost::Callbacks& callbacks,
                        std::string* error_message) {
  if (!callbacks.publish_decorations) {
    if (error_message != nullptr) {
      *error_message = "decorations API unavailable";
    }
    return false;
  }

  const std::filesystem::path path =
      ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "decoration path must not be empty";
    }
    return false;
  }

  const int absolute_index = lua_absindex(state, table_index);
  editor::PluginDecorationData data;
  if (!ReadTextStyles(state, absolute_index, &data, error_message) ||
      !ReadGutterMarks(state, absolute_index, &data, error_message) ||
      !ReadInlineText(state, absolute_index, &data, error_message) ||
      !ReadCodeLenses(state, absolute_index, &data, error_message)) {
    return false;
  }

  callbacks.publish_decorations(std::string(plugin_id), path, std::move(data));
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

bool ClearDecorations(std::string_view plugin_id,
                      const std::optional<std::filesystem::path>& path,
                      const PluginHost::Callbacks& callbacks,
                      std::string* error_message) {
  if (path.has_value()) {
    if (!callbacks.clear_file_decorations) {
      if (error_message != nullptr) {
        *error_message = "decorations API unavailable";
      }
      return false;
    }
    callbacks.clear_file_decorations(std::string(plugin_id), path->lexically_normal());
  } else {
    if (!callbacks.clear_owner_decorations) {
      if (error_message != nullptr) {
        *error_message = "decorations API unavailable";
      }
      return false;
    }
    callbacks.clear_owner_decorations(std::string(plugin_id));
  }
  if (error_message != nullptr) {
    error_message->clear();
  }
  return true;
}

}  // namespace microide::plugin::decoration_interop

#endif
