#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/SettingsOverlayService.h"
#include "workspace/WorkspaceAiProvider.h"
#include "workspace/WorkspaceSettingsRegistry.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DefaultSettingValue;
using microide::workspace::FindBuiltinSettingSpec;
using microide::workspace::ParseSettingValue;
using microide::workspace::SerializeSettingValue;
using microide::workspace::AiProviderRegistry;
using microide::workspace::AiProviderSpec;
using microide::workspace::SettingsOverlayService;
using microide::workspace::SettingSpec;
using microide::workspace::SettingType;

constexpr std::array<std::string_view, 16> kNewSettingIds = {
    "editor.font_family",
    "editor.font_size",
    "editor.line_endings",
    "editor.trim_trailing_whitespace",
    "editor.insert_final_newline",
    "editor.format_on_save",
    "editor.autosave",
    "editor.hover_delay_ms",
    "ui.layout_mode",
    "ui.layout_compact_breakpoint_px",
    "ui.scrollbar_size",
    "ui.resize_handle_size",
    "ui.show_status_bar",
    "terminal.shell",
    "terminal.font_size",
    "diagnostics.min_severity",
};

std::vector<std::string_view> ValidSamplesFor(const SettingSpec& spec) {
  switch (spec.type) {
    case SettingType::Bool:
      return {"true", "false", "1", "0"};
    case SettingType::Int:
      if (spec.id == "ui.layout_compact_breakpoint_px") {
        return {"600", "720", "2000"};
      }
      if (spec.id == "editor.hover_delay_ms") {
        return {"0", "350", "2000"};
      }
      return {"8", "13", "32"};
    case SettingType::Float:
      return {"0.75", "1.0", "2.0"};
    case SettingType::String:
      return {"", "JetBrains Mono", "/bin/bash"};
    case SettingType::Enum: {
      std::vector<std::string_view> samples;
      samples.reserve(spec.enum_values.size());
      for (const auto& value : spec.enum_values) {
        samples.push_back(value.value);
      }
      return samples;
    }
  }
  return {};
}

void TestSettingsCatalogIncludesPolishKeys() {
  for (std::string_view id : kNewSettingIds) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "settings catalog should include every responsive-polish key");
    Expect(!spec->label.empty(), "new setting specs should have user-facing labels");
    Expect(!spec->description.empty(), "new setting specs should have descriptions");
  }
}

void TestSettingsCatalogDefaultsRoundTrip() {
  for (std::string_view id : kNewSettingIds) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "default round-trip should find the setting spec");
    const std::string serialized = SerializeSettingValue(DefaultSettingValue(*spec));
    const auto parsed = ParseSettingValue(*spec, serialized);
    Expect(parsed.has_value(), "serialized default setting value should parse again");
    Expect(SerializeSettingValue(*parsed) == serialized,
           "parsed default setting value should serialize stably");
  }
}

void TestSettingsCatalogEdgeValuesRoundTrip() {
  for (std::string_view id : kNewSettingIds) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "edge-value round-trip should find the setting spec");
    for (std::string_view sample : ValidSamplesFor(*spec)) {
      const auto parsed = ParseSettingValue(*spec, sample);
      Expect(parsed.has_value(), "documented setting sample should parse");
      const auto reparsed = ParseSettingValue(*spec, SerializeSettingValue(*parsed));
      Expect(reparsed.has_value(), "serialized documented setting sample should parse again");
    }
  }
}

void TestSettingsCatalogRejectsInvalidEnums() {
  for (std::string_view id : kNewSettingIds) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "invalid-enum fixture should find the setting spec");
    if (spec->type != SettingType::Enum) {
      continue;
    }
    Expect(!ParseSettingValue(*spec, "not-a-declared-value").has_value(),
           "enum settings should reject values outside enum_values");
  }
}

void TestSettingsOverlayFiltersAndPreservesScopes() {
  SettingsOverlayService service;
  service.SetQuery("layout");
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {{"ui.layout_mode", "compact"}},
                              {{"editor.wrap", "word"}});

  Expect(!service.SettingsRows().empty(), "settings overlay should expose filtered settings rows");
  const auto it = std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                               [](const auto& row) { return row.id == "ui.layout_mode"; });
  Expect(it != service.SettingsRows().end(),
         "settings overlay filter should match setting ids and labels");
  Expect(it->value == "compact" && it->resettable,
         "settings overlay rows should surface stored values and reset affordance state");
  Expect(it->detail.find("User") != std::string::npos,
         "settings overlay rows should preserve scope labels");
}

void TestAiProviderRegistryMetadataValidation() {
  AiProviderRegistry registry;
  AiProviderSpec empty_name;
  empty_name.id = "empty-name";
  registry.Register(empty_name);
  Expect(registry.Specs().empty(),
         "AI provider registry should reject providers without display names");

  AiProviderSpec openai;
  openai.id = "openai";
  openai.label = "OpenAI";
  openai.display_name = "OpenAI";
  openai.type = "cloud";
  openai.api_key_name = "openai.api_key";
  openai.models = {"gpt-test"};
  openai.default_model = "gpt-test";
  registry.Register(openai);
  Expect(registry.Specs().size() == 1,
         "AI provider registry should accept providers with display metadata");
  Expect(registry.Specs().front().requires_api_key &&
             registry.Specs().front().auth_method == "api_key",
         "AI provider registry should derive auth metadata from API-key providers");

  SettingsOverlayService service;
  service.OpenAiProviderPicker();
  service.RebuildProviderRows(registry.Specs(), "openai");
  Expect(service.ProviderRows().size() == 1 && service.ProviderRows().front().active,
         "AI provider picker should expose active provider rows");
  Expect(service.ProviderRows().front().model == "gpt-test",
         "AI provider picker should expose default model metadata");
}

}  // namespace

void RegisterWorkspaceSettingsRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceSettingsRegistry/IncludesPolishKeys",
          TestSettingsCatalogIncludesPolishKeys);
  AddTest(tests, "WorkspaceSettingsRegistry/DefaultsRoundTrip",
          TestSettingsCatalogDefaultsRoundTrip);
  AddTest(tests, "WorkspaceSettingsRegistry/EdgeValuesRoundTrip",
          TestSettingsCatalogEdgeValuesRoundTrip);
  AddTest(tests, "WorkspaceSettingsRegistry/RejectsInvalidEnums",
          TestSettingsCatalogRejectsInvalidEnums);
  AddTest(tests, "WorkspaceSettingsOverlay/FiltersAndPreservesScopes",
          TestSettingsOverlayFiltersAndPreservesScopes);
  AddTest(tests, "WorkspaceAiProviderPicker/MetadataValidation",
          TestAiProviderRegistryMetadataValidation);
}

}  // namespace microide::tests
