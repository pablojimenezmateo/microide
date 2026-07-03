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
  // Optional grouping path used by the Settings overlay to organize related
  // toggles into subsections. Format: "Group → Subsection" (Unicode arrow).
  // Empty when the setting belongs to no specific group (top-level list).
  std::string_view group;
  // When true, the Settings overlay renders this String row as a font picker: a
  // filterable list of installed families plus a "Choose file…" entry, instead of
  // a bare text field. Only meaningful for SettingType::String.
  bool suggests_fonts = false;
};

using SettingValue = std::variant<bool, int, float, std::string>;

struct EditorPreferences;

// Applies a parsed canonical editor-preference setting (tab size, indent width,
// font size, soft tabs, wrap mode) to `prefs`, clamping numeric values to their
// supported ranges. Returns true when `id` is one of these canonical editor
// preferences (whether or not the value's type matched), and false for ids this
// helper does not own (e.g. `editor.colorscheme`, which callers apply with
// context-specific side effects). Shared by the persistence load path and the
// settings-overlay setter so the id->preference mapping lives in one place.
bool ApplyCanonicalEditorPreference(EditorPreferences& prefs, std::string_view id,
                                    const SettingValue& value);

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
  std::string group;       // see SettingSpec::group
  bool suggests_fonts = false;  // see SettingSpec::suggests_fonts
};

std::vector<SettingInfo> AllSettingInfos(const plugin::PluginHost& plugin_host);
std::optional<SettingInfo> FindSettingInfo(std::string_view id,
                                            const plugin::PluginHost& plugin_host);

// Build a default value from a SettingSpec.
SettingValue DefaultSettingValue(const SettingSpec& spec);

}  // namespace microide::workspace
