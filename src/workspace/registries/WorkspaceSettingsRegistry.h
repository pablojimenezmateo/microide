#pragma once

#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace microide::plugin {
class PluginHost;
}

namespace microide::workspace {

enum class SettingType { Bool, Int, Float, String, Enum };
enum class SettingScope { User, Project };

struct SettingEnumValue {
  std::string_view value;
  std::string_view label;
};

enum class SettingEnumSource { Static, Colorschemes };

struct SettingSpec {
  std::string_view id;
  std::string_view label;
  std::string_view description;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  bool default_bool = false;
  int default_int = 0;
  int min_int = std::numeric_limits<int>::min();
  int max_int = std::numeric_limits<int>::max();
  int int_step = 1;
  float default_float = 0.0f;
  float min_float = std::numeric_limits<float>::lowest();
  float max_float = std::numeric_limits<float>::max();
  std::string_view default_string;
  std::span<const SettingEnumValue> enum_values;
  SettingEnumSource enum_source = SettingEnumSource::Static;
  std::string_view group;
  bool suggests_fonts = false;
};

using SettingValue = std::variant<bool, int, float, std::string>;

struct EditorPreferences;

bool ApplyCanonicalEditorPreference(EditorPreferences& prefs, std::string_view id,
                                    const SettingValue& value);

std::span<const SettingSpec> BuiltinSettingSpecs();
const SettingSpec* FindBuiltinSettingSpec(std::string_view id);

std::optional<SettingValue> ParseSettingValue(const SettingSpec& spec, std::string_view text);
std::string SerializeSettingValue(const SettingValue& value);
void SerializeSettingValueInto(const SettingValue& value, std::string* out);

void CanonicalizeLoadedSettingValue(std::string_view id, std::string& value);

struct SettingInfo {
  std::string id;
  std::string label;
  std::string description;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  SettingValue default_value;
  std::vector<std::string> enum_values;
  std::string plugin_id;
  std::string group;
  bool suggests_fonts = false;
};

std::vector<SettingInfo> AllSettingInfos(const plugin::PluginHost& plugin_host,
                                         std::span<const std::string> colorschemes = {});
std::optional<SettingInfo> FindSettingInfo(std::string_view id,
                                            const plugin::PluginHost& plugin_host,
                                            std::span<const std::string> colorschemes = {});

std::optional<SettingScope> FindSettingScope(std::string_view id,
                                             const plugin::PluginHost& plugin_host);

SettingValue DefaultSettingValue(const SettingSpec& spec);

}
