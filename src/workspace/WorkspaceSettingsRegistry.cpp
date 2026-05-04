#include "workspace/WorkspaceSettingsRegistry.h"

#include <algorithm>
#include <array>
#include <charconv>

#include "plugin/PluginHost.h"
#include "util/Parse.h"

namespace microide::workspace {

namespace {

const SettingEnumValue kColorschemeValues[] = {
    {"default", "Default"},
    {"dark", "Dark"},
    {"light", "Light"},
};

const SettingEnumValue kEditorWrapValues[] = {
    {"off", "Off"},
    {"word", "Word"},
};

}  // namespace

std::span<const SettingSpec> BuiltinSettingSpecs() {
  static const auto kSpecs = std::to_array<SettingSpec>({
      SettingSpec{
          .id = "editor.tab_size",
          .label = "Tab Size",
          .description = "Number of spaces per tab stop.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 4,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
      },
      SettingSpec{
          .id = "editor.indent_width",
          .label = "Indent Width",
          .description = "Number of spaces used for each indent level.",
          .type = SettingType::Int,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 4,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
      },
      SettingSpec{
          .id = "editor.soft_tabs",
          .label = "Soft Tabs",
          .description = "Insert spaces instead of tabs.",
          .type = SettingType::Bool,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = {},
          .enum_values = {},
      },
      SettingSpec{
          .id = "editor.wrap",
          .label = "Word Wrap",
          .description = "Wrap long lines to the viewport width.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "off",
          .enum_values = kEditorWrapValues,
      },
      SettingSpec{
          .id = "editor.colorscheme",
          .label = "Color Scheme",
          .description = "Active color scheme name.",
          .type = SettingType::Enum,
          .scope = SettingScope::Project,
          .default_bool = false,
          .default_int = 0,
          .default_float = 0.0f,
          .default_string = "default",
          .enum_values = kColorschemeValues,
      },
      SettingSpec{
          .id = "ui.scale",
          .label = "UI Scale",
          .description = "Interface zoom factor.",
          .type = SettingType::Float,
          .scope = SettingScope::User,
          .default_bool = false,
          .default_int = 0,
          .default_float = 1.0f,
          .default_string = {},
          .enum_values = {},
      },
  });
  return kSpecs;
}

const SettingSpec* FindBuiltinSettingSpec(std::string_view id) {
  const auto specs = BuiltinSettingSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(),
                               [id](const SettingSpec& s) { return s.id == id; });
  return it == specs.end() ? nullptr : &(*it);
}

SettingValue DefaultSettingValue(const SettingSpec& spec) {
  switch (spec.type) {
    case SettingType::Bool:
      return spec.default_bool;
    case SettingType::Int:
      return spec.default_int;
    case SettingType::Float:
      return spec.default_float;
    case SettingType::String:
    case SettingType::Enum:
      return std::string(spec.default_string);
  }
  return std::string{};
}

std::optional<SettingValue> ParseSettingValue(const SettingSpec& spec, std::string_view text) {
  switch (spec.type) {
    case SettingType::Bool:
      if (text == "true" || text == "1" || text == "on" || text == "yes") {
        return true;
      }
      if (text == "false" || text == "0" || text == "off" || text == "no") {
        return false;
      }
      return std::nullopt;

    case SettingType::Int: {
      int value = 0;
      const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    }

    case SettingType::Float: {
      return util::ParseFloat(text);
    }

    case SettingType::String:
      return std::string(text);

    case SettingType::Enum:
      for (const SettingEnumValue& ev : spec.enum_values) {
        if (ev.value == text) {
          return std::string(text);
        }
      }
      return std::nullopt;
  }
  return std::nullopt;
}

std::string SerializeSettingValue(const SettingValue& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
          return std::to_string(v);
        } else if constexpr (std::is_same_v<T, float>) {
          return std::to_string(v);
        } else {
          return v;
        }
      },
      value);
}

std::vector<SettingInfo> AllSettingInfos(const plugin::PluginHost& plugin_host) {
  std::vector<SettingInfo> infos;

  for (const SettingSpec& spec : BuiltinSettingSpecs()) {
    SettingInfo info;
    info.id = std::string(spec.id);
    info.label = std::string(spec.label);
    info.description = std::string(spec.description);
    info.type = spec.type;
    info.scope = spec.scope;
    info.default_value = DefaultSettingValue(spec);
    for (const SettingEnumValue& ev : spec.enum_values) {
      info.enum_values.emplace_back(ev.value);
    }
    infos.push_back(std::move(info));
  }

  for (const auto& contrib : plugin_host.ContributedSettings()) {
    SettingInfo info;
    info.id = contrib.id;
    info.label = contrib.label;
    info.description = contrib.description;
    info.plugin_id = contrib.plugin_id;

    if (contrib.type == "bool") {
      info.type = SettingType::Bool;
      info.default_value = contrib.default_value == "true";
    } else if (contrib.type == "int") {
      info.type = SettingType::Int;
      int v = 0;
      std::from_chars(contrib.default_value.data(),
                      contrib.default_value.data() + contrib.default_value.size(), v);
      info.default_value = v;
    } else if (contrib.type == "float") {
      info.type = SettingType::Float;
      info.default_value = util::ParseFloat(contrib.default_value).value_or(0.0f);
    } else if (contrib.type == "enum") {
      info.type = SettingType::Enum;
      info.default_value = contrib.default_value;
      info.enum_values = contrib.enum_values;
    } else {
      info.type = SettingType::String;
      info.default_value = contrib.default_value;
    }

    if (contrib.scope == "user") {
      info.scope = SettingScope::User;
    } else {
      info.scope = SettingScope::Project;
    }

    infos.push_back(std::move(info));
  }

  return infos;
}

std::optional<SettingInfo> FindSettingInfo(std::string_view id,
                                            const plugin::PluginHost& plugin_host) {
  const auto infos = AllSettingInfos(plugin_host);
  const auto it = std::find_if(infos.begin(), infos.end(),
                               [id](const SettingInfo& info) { return info.id == id; });
  if (it == infos.end()) {
    return std::nullopt;
  }
  return *it;
}

}  // namespace microide::workspace
