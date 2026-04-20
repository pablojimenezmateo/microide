#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

enum class SettingType { Bool, Int, Float, String, Enum };
enum class SettingScope { User, Project };

struct SettingEnumValue {
  std::string_view value;
  std::string_view label;
};

// Static descriptor for built-in settings; string_view members point into literals.
struct SettingSpec {
  std::string_view id;
  std::string_view label;
  std::string_view description;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  bool default_bool = false;
  int default_int = 0;
  float default_float = 0.0f;
  std::string_view default_string;
  std::span<const SettingEnumValue> enum_values;
};

using SettingValue = std::variant<bool, int, float, std::string>;

std::span<const SettingSpec> BuiltinSettingSpecs();
const SettingSpec* FindBuiltinSettingSpec(std::string_view id);

// Parses a setting value from its string representation according to type.
// Returns nullopt if the string is invalid for the given type.
std::optional<SettingValue> ParseSettingValue(const SettingSpec& spec, std::string_view text);
std::string SerializeSettingValue(const SettingValue& value);

// Unified view merging built-ins and plugin-contributed settings.
struct SettingInfo {
  std::string id;
  std::string label;
  std::string description;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  SettingValue default_value;
  std::vector<std::string> enum_values;
  std::string plugin_id;  // empty for built-ins
};

std::vector<SettingInfo> AllSettingInfos(const plugin::PluginHost& plugin_host);
std::optional<SettingInfo> FindSettingInfo(std::string_view id,
                                            const plugin::PluginHost& plugin_host);

// Build a default value from a SettingSpec.
SettingValue DefaultSettingValue(const SettingSpec& spec);

}  // namespace microide::workspace
