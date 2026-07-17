#include "plugin/PluginPresentationRegistrationParsers.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <algorithm>
#include <optional>
#include <string>

#include "plugin/PluginLuaInterop.h"
#include "render/ThemeFile.h"
#include "util/StringUtil.h"

namespace microide::plugin::presentation_interop {
namespace {

using lua_interop::ReadOptionalStringField;

// Hard caps so a malformed/hostile manifest cannot make the host allocate
// unboundedly. Themes rarely exceed a few dozen groups; icon themes a few
// hundred rules.
constexpr std::size_t kMaxThemeStyles = 512;
constexpr std::size_t kMaxIconRules = 1024;

std::optional<std::string> ReadHostNamespacedId(lua_State* state,
                                                std::string_view plugin_id,
                                                std::string* error_message,
                                                std::string_view kind) {
  auto id_opt = ReadOptionalStringField(state, 1, "id");
  if (!id_opt || id_opt->empty()) {
    if (error_message != nullptr) {
      *error_message = std::string(kind) + " requires id";
    }
    return std::nullopt;
  }
  return std::string(plugin_id) + "." + *id_opt;
}

// Parse a `"fg"` or `"fg,bg"` colour string into a contributed style.
void ApplyColorString(std::string_view value, PluginHost::ContributedThemeStyle* style) {
  const std::size_t comma = value.find(',');
  const std::string_view fg = comma == std::string_view::npos ? value : value.substr(0, comma);
  style->foreground = render::ParseThemeColor(fg);
  if (comma != std::string_view::npos) {
    style->background = render::ParseThemeColor(value.substr(comma + 1));
  }
}

// Parse a `{fg=, bg=, reverse=}` style table. The table is expected at stack
// top (-1).
void ApplyColorTable(lua_State* state, PluginHost::ContributedThemeStyle* style) {
  if (auto fg = ReadOptionalStringField(state, -1, "fg")) {
    style->foreground = render::ParseThemeColor(*fg);
  } else if (auto foreground = ReadOptionalStringField(state, -1, "foreground")) {
    style->foreground = render::ParseThemeColor(*foreground);
  }
  if (auto bg = ReadOptionalStringField(state, -1, "bg")) {
    style->background = render::ParseThemeColor(*bg);
  } else if (auto background = ReadOptionalStringField(state, -1, "background")) {
    style->background = render::ParseThemeColor(*background);
  }
  lua_interop::GetFieldProtected(state, -1, "reverse");
  style->reverse = lua_toboolean(state, -1) != 0;
  lua_pop(state, 1);
}

}  // namespace

bool RegisterTheme(lua_State* state,
                   std::string_view plugin_id,
                   std::vector<PluginHost::ContributedTheme>* themes,
                   std::string* error_message) {
  if (themes == nullptr) {
    return false;
  }
  // Non-raising type check: luaL_checktype would longjmp over the caller's live
  // std::string error_message (RegisterTableContribution), violating the
  // "no longjmp over live C++ locals" invariant.
  if (lua_type(state, 1) != LUA_TTABLE) {
    if (error_message != nullptr) {
      *error_message = "theme registration expects a table argument";
    }
    return false;
  }
  auto full_id = ReadHostNamespacedId(state, plugin_id, error_message, "theme");
  if (!full_id) {
    return false;
  }
  PluginHost::ContributedTheme theme;
  theme.id = std::move(*full_id);
  theme.plugin_id = std::string(plugin_id);
  theme.label = ReadOptionalStringField(state, 1, "label").value_or(theme.id);

  // Accept either `colors` or `styles` for the group map.
  lua_interop::GetFieldProtected(state, 1, "colors");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    lua_interop::GetFieldProtected(state, 1, "styles");
  }
  if (lua_istable(state, -1)) {
    const int colors_index = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, colors_index) != 0) {
      // key at -2, value at -1.
      // TD-2026-07-17-079: stop draining the table once the retained-style cap is
      // reached. A hostile/accidental theme with a huge or sparse `colors`/`styles`
      // map would otherwise spend O(table) setup CPU even though only kMaxThemeStyles
      // entries are ever kept. Pop the pending value AND key to rebalance the stack
      // before breaking (the post-loop lua_pop expects only the table remaining).
      if (theme.styles.size() >= kMaxThemeStyles) {
        lua_pop(state, 2);
        break;
      }
      if (lua_type(state, -2) == LUA_TSTRING) {
        PluginHost::ContributedThemeStyle style;
        style.group = util::ToLowerAscii(lua_tostring(state, -2));
        if (lua_isstring(state, -1)) {
          ApplyColorString(lua_tostring(state, -1), &style);
        } else if (lua_istable(state, -1)) {
          ApplyColorTable(state, &style);
        }
        if (!style.group.empty() &&
            (style.foreground.has_value() || style.background.has_value() || style.reverse)) {
          theme.styles.push_back(std::move(style));
        }
      }
      lua_pop(state, 1);  // pop value, keep key for next iteration
    }
  }
  lua_pop(state, 1);  // pop colors/styles table (or the non-table value)

  if (theme.styles.empty()) {
    if (error_message != nullptr) {
      *error_message = "theme '" + theme.id + "' declares no colours";
    }
    return false;
  }
  const auto existing = std::find_if(
      themes->begin(), themes->end(),
      [&](const PluginHost::ContributedTheme& e) { return e.id == theme.id; });
  if (existing != themes->end()) {
    *existing = std::move(theme);
  } else {
    themes->push_back(std::move(theme));
  }
  return true;
}

bool RegisterFileIconTheme(lua_State* state,
                           std::string_view plugin_id,
                           std::vector<PluginHost::ContributedFileIconTheme>* themes,
                           std::string* error_message) {
  if (themes == nullptr) {
    return false;
  }
  // Non-raising type check: see RegisterTheme for the longjmp-over-locals rationale.
  if (lua_type(state, 1) != LUA_TTABLE) {
    if (error_message != nullptr) {
      *error_message = "file icon theme registration expects a table argument";
    }
    return false;
  }
  auto full_id = ReadHostNamespacedId(state, plugin_id, error_message, "file icon theme");
  if (!full_id) {
    return false;
  }
  PluginHost::ContributedFileIconTheme theme;
  theme.id = std::move(*full_id);
  theme.plugin_id = std::string(plugin_id);
  theme.label = ReadOptionalStringField(state, 1, "label").value_or(theme.id);

  // Accept either `rules` or `icons` for the rule array.
  lua_interop::GetFieldProtected(state, 1, "rules");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    lua_interop::GetFieldProtected(state, 1, "icons");
  }
  if (lua_istable(state, -1)) {
    const int rules_index = lua_gettop(state);
    // Clamp iterations, not just accepted rules: entries that are non-tables or
    // fail the matcher/icon gate never grow theme.rules, so a sparse-border table
    // with a huge lua_rawlen would otherwise defeat the size()<kMaxIconRules guard.
    const std::size_t count =
        std::min<std::size_t>(static_cast<std::size_t>(lua_rawlen(state, rules_index)), kMaxIconRules);
    for (std::size_t i = 1; i <= count && theme.rules.size() < kMaxIconRules; ++i) {
      lua_rawgeti(state, rules_index, static_cast<lua_Integer>(i));
      if (lua_istable(state, -1)) {
        PluginHost::ContributedFileIconRule rule;
        auto icon = ReadOptionalStringField(state, -1, "icon");
        auto name = ReadOptionalStringField(state, -1, "name");
        auto ext = ReadOptionalStringField(state, -1, "ext");
        auto color = ReadOptionalStringField(state, -1, "color");
        if (name && !name->empty()) {
          rule.matcher = util::ToLowerAscii(*name);
          rule.match_filename = true;
        } else if (ext && !ext->empty()) {
          std::string e = util::ToLowerAscii(*ext);
          if (!e.empty() && e.front() == '.') {
            e.erase(e.begin());
          }
          rule.matcher = std::move(e);
          rule.match_filename = false;
        }
        rule.icon = icon.value_or(std::string{});
        rule.color = render::ParseThemeColor(color.value_or(std::string{}))
                         .value_or(SDL_Color{0xcc, 0xcc, 0xcc, 0xff});
        if (!rule.matcher.empty() && !rule.icon.empty()) {
          theme.rules.push_back(std::move(rule));
        }
      }
      lua_pop(state, 1);
    }
  }
  lua_pop(state, 1);  // pop rules/icons table (or the non-table value)

  if (theme.rules.empty()) {
    if (error_message != nullptr) {
      *error_message = "file icon theme '" + theme.id + "' declares no rules";
    }
    return false;
  }
  const auto existing = std::find_if(
      themes->begin(), themes->end(),
      [&](const PluginHost::ContributedFileIconTheme& e) { return e.id == theme.id; });
  if (existing != themes->end()) {
    *existing = std::move(theme);
  } else {
    themes->push_back(std::move(theme));
  }
  return true;
}

}  // namespace microide::plugin::presentation_interop

#endif
