#include "TestSupport.h"

#include "plugin/PluginHost.h"
#include "workspace/services/SettingsOverlayService.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"
#include "perf/AllocationCounter.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::DefaultSettingValue;
using microide::workspace::FindBuiltinSettingSpec;
using microide::workspace::ParseSettingValue;
using microide::workspace::SerializeSettingValue;
using microide::workspace::SettingsOverlayService;
using microide::workspace::SettingSpec;
using microide::workspace::SettingType;

constexpr std::array<std::string_view, 13> kNewSettingIds = {
    "editor.font_family",
    "editor.font_size",
    "editor.line_endings",
    "editor.format_on_save",
    "editor.autosave",
    "editor.autosave.delay_ms",
    "ui.layout_mode",
    "ui.layout_compact_breakpoint_px",
    "ui.show_status_bar",
    "terminal.shell",
    "terminal.font_size",
    "terminal.font_family",
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

void TestSettingsCatalogProjectTabHideToggle() {
  const SettingSpec* spec = FindBuiltinSettingSpec("chrome.project_tabs.hide_when_single");
  Expect(spec != nullptr, "project-tab hide toggle should be registered");
  Expect(spec->type == SettingType::Bool, "project-tab hide toggle is a boolean setting");
  Expect(spec->scope == microide::workspace::SettingScope::User,
         "project-tab hide toggle is a user-scoped preference");
  const microide::workspace::SettingValue value = DefaultSettingValue(*spec);
  const auto* on = std::get_if<bool>(&value);
  Expect(on != nullptr && *on, "project-tab hide toggle defaults to on");
}

void TestSettingsCatalogLspToggles() {
  // Master + per-feature toggles, all Bool / User-scoped and defaulting on.
  constexpr std::array<std::string_view, 14> kLspToggleIds = {
      "lsp.enabled",
      "lsp.completion.enabled",
      "lsp.hover.enabled",
      "lsp.diagnostics.enabled",
      "lsp.code_actions.enabled",
      "lsp.formatting.enabled",
      "lsp.rename.enabled",
      "lsp.goto_definition.enabled",
      "lsp.find_references.enabled",
      "lsp.navigation.enabled",
      "lsp.workspace_symbol.enabled",
      "lsp.signature_help.enabled",
      "lsp.semantic_tokens.enabled",
      "lsp.document_symbols.enabled",
  };
  for (std::string_view id : kLspToggleIds) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "every LSP toggle should be registered");
    Expect(spec->type == SettingType::Bool, "LSP toggles are boolean settings");
    Expect(spec->scope == microide::workspace::SettingScope::User,
           "LSP toggles are user-scoped preferences");
    Expect(!spec->label.empty() && !spec->description.empty(),
           "LSP toggles should have labels and descriptions");
    const microide::workspace::SettingValue value = DefaultSettingValue(*spec);
    const auto* on = std::get_if<bool>(&value);
    Expect(on != nullptr && *on, "LSP toggles default to on");
  }
  // The master lives at the "LSP" top level; the features nest under "LSP → …".
  Expect(FindBuiltinSettingSpec("lsp.enabled")->group == "LSP",
         "the LSP master switch sits at the LSP category root");
  Expect(FindBuiltinSettingSpec("lsp.hover.enabled")->group.rfind("LSP", 0) == 0,
         "per-feature LSP toggles group under the LSP category");
}

void TestSettingsOverlayDerivesLspCategory() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto& categories = service.Categories();
  Expect(std::find(categories.begin(), categories.end(), "LSP") != categories.end(),
         "the LSP settings group should derive an LSP category");
}

void TestSettingsCatalogSaveDefaultsAndDedup() {
  // The stale duplicate keys were removed in favor of the wired editor.save.* pair.
  Expect(FindBuiltinSettingSpec("editor.trim_trailing_whitespace") == nullptr,
         "stale editor.trim_trailing_whitespace spec should be gone");
  Expect(FindBuiltinSettingSpec("editor.insert_final_newline") == nullptr,
         "stale editor.insert_final_newline spec should be gone");
  // The canonical save specs default to true so the shown default matches the
  // runtime fallback used by the save pipeline (WorkspaceShellEditor.cpp).
  for (std::string_view id :
       {"editor.save.trim_trailing_whitespace", "editor.save.ensure_final_newline"}) {
    const SettingSpec* spec = FindBuiltinSettingSpec(id);
    Expect(spec != nullptr, "canonical save setting should exist");
    const microide::workspace::SettingValue value = DefaultSettingValue(*spec);
    const auto* on = std::get_if<bool>(&value);
    Expect(on != nullptr && *on, "canonical save setting should default to true");
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

void TestSettingsIntValuesClampAtStoreTime() {
  const SettingSpec* spec = FindBuiltinSettingSpec("editor.font_size");
  Expect(spec != nullptr, "font_size spec should exist");
  // The store-time clamp keeps the stored/displayed value in sync with the value
  // the editor actually applies (font size range is 8..32).
  const auto high = ParseSettingValue(*spec, "999");
  Expect(high.has_value() && std::get<int>(*high) == 32,
         "an out-of-range-high int must be clamped to max at store time, not stored verbatim");
  const auto low = ParseSettingValue(*spec, "1");
  Expect(low.has_value() && std::get<int>(*low) == 8,
         "an out-of-range-low int must be clamped to min at store time");
  const auto ok = ParseSettingValue(*spec, "16");
  Expect(ok.has_value() && std::get<int>(*ok) == 16, "an in-range int should pass through");

  // A setting without an explicit range keeps the unbounded sentinels: no clamp.
  const SettingSpec* unranged = FindBuiltinSettingSpec("editor.soft_tabs");
  if (unranged != nullptr && unranged->type == SettingType::Int) {
    const auto big = ParseSettingValue(*unranged, "1000000");
    Expect(big.has_value() && std::get<int>(*big) == 1000000,
           "an int setting with no declared range must not be clamped");
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

// TD-2026-08-06-159: a rebuild reuses its rows instead of freeing and reallocating
// eight strings per row. RebuildSettingsRows runs on every keystroke in the
// Settings search box, so what a repeated rebuild costs is a per-keystroke cost.
//
// Two claims are tested together because the fix is only correct if both hold: the
// second rebuild must be nearly free, AND a narrowing rebuild must not leave stale
// rows behind (reusing storage is exactly how that goes wrong).
void TestSettingsOverlayRebuildReusesRowStorage() {
  std::vector<microide::workspace::SettingInfo> settings;
  for (int i = 0; i < 200; ++i) {
    microide::workspace::SettingInfo info;
    info.id = "perf.plugin.setting_" + std::to_string(i);
    info.label = (i % 2 == 0 ? "Alpha number " : "Beta number ") + std::to_string(i);
    info.description = "Controls behavior variant " + std::to_string(i);
    info.type = SettingType::String;
    info.scope = microide::workspace::SettingScope::Project;
    info.default_value = std::string("default");
    info.plugin_id = "perf.plugin";
    info.group = "Plugins";
    settings.push_back(std::move(info));
  }
  std::vector<std::pair<std::string, std::string>> user;
  for (int i = 0; i < 200; i += 2) {
    user.emplace_back("perf.plugin.setting_" + std::to_string(i), "user-override");
  }

  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(settings, user, {});
  Expect(service.SettingsRows().size() == 200, "the unfiltered rebuild shows every row");

#if MICROIDE_PERF_HARNESS_BUILD
  namespace perf = microide::tests::perf;
  const auto before = perf::Allocations::Snapshot();
  service.RebuildSettingsRows(settings, user, {});
  const auto delta = perf::Allocations::DeltaSince(before);
  // Six string allocations per row plus one hash node per override is ~1,300 here;
  // reusing the rows leaves only the two sorted layer indices, which keep their
  // capacity, and the per-category row-index vectors.
  Expect(delta.allocations < 100,
         "a repeated rebuild of the same surface must reuse its row storage (took " +
             std::to_string(delta.allocations) + " allocations)");
#endif

  // Narrowing: the reused storage must not keep rows the query no longer matches.
  service.SetQuery("Alpha");
  service.RebuildSettingsRows(settings, user, {});
  Expect(service.SettingsRows().size() == 100,
         "a narrowing query drops the rows it filters out, reused storage or not");
  for (const auto& row : service.SettingsRows()) {
    Expect(row.label.find("Alpha") != std::string::npos,
           "no stale row survives into a narrowed rebuild");
  }

  // Widening back must restore every row with correct content, not the values a
  // reused row happened to hold.
  service.SetQuery("");
  service.RebuildSettingsRows(settings, user, {});
  Expect(service.SettingsRows().size() == 200, "clearing the query restores every row");
  Expect(service.SettingsRows()[0].id == "perf.plugin.setting_0" &&
             service.SettingsRows()[0].value == "user-override" &&
             service.SettingsRows()[0].resettable,
         "an overridden row reads its override after storage reuse");
  Expect(service.SettingsRows()[1].id == "perf.plugin.setting_1" &&
             service.SettingsRows()[1].value == "default" && !service.SettingsRows()[1].resettable,
         "a row with no override does not inherit the previous occupant's value");
}

void TestSettingsOverlayScopeSelectableRows() {
  // editor.line_endings is a built-in project-scoped setting, so its overlay row
  // supports the per-row "This Project / Default" scope chip.
  const auto row_for = [](const std::vector<std::pair<std::string, std::string>>& user,
                          const std::vector<std::pair<std::string, std::string>>& project) {
    SettingsOverlayService service;
    service.OpenSettings();
    service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                                user, project);
    const auto it = std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                                 [](const auto& row) { return row.id == "editor.line_endings"; });
    Expect(it != service.SettingsRows().end(), "line_endings row should be present");
    return *it;
  };

  // A user-level default with no project override reads as "Default" and shows the
  // default value; the chip is available and no project override is recorded.
  const auto defaulted = row_for({{"editor.line_endings", "crlf"}}, {});
  Expect(defaulted.scope_selectable, "project-scoped built-in should be scope-selectable");
  Expect(defaulted.has_user_default && !defaulted.project_override,
         "user default present, project override absent");
  Expect(defaulted.value == "crlf", "effective value should surface the user-level default");
  Expect(defaulted.scope_label == "Default", "no project override reads as Default");
  Expect(defaulted.resettable, "an active user default is resettable");

  // A per-project override wins over the user default and reads as "Project".
  const auto overridden =
      row_for({{"editor.line_endings", "crlf"}}, {{"editor.line_endings", "lf"}});
  Expect(overridden.project_override && overridden.has_user_default,
         "both layers hold a value");
  Expect(overridden.value == "lf", "project override should win over the user default");
  Expect(overridden.scope_label == "Project", "a project override reads as Project");

  // A user-scoped setting is never scope-selectable.
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto status_it =
      std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                   [](const auto& row) { return row.id == "ui.show_status_bar"; });
  Expect(status_it != service.SettingsRows().end(), "status-bar row present");
  Expect(!status_it->scope_selectable, "user-scoped settings are not scope-selectable");
}

void TestSettingsOverlayStringRowsAreTextEditable() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto it = std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                               [](const auto& row) { return row.id == "editor.font_family"; });
  Expect(it != service.SettingsRows().end(), "font_family row should be present");
  Expect(it->control_kind == microide::workspace::SettingsControlKind::TextEdit,
         "String settings should use the inline text-edit control");

  // The value-edit session round-trips text and clears on cancel.
  Expect(!service.EditingValue(), "no edit active initially");
  service.BeginValueEdit("editor.font_family", "JetBrains Mono");
  Expect(service.EditingValue() && service.EditingRowId() == "editor.font_family",
         "BeginValueEdit activates editing for the row");
  Expect(service.ValueEditText() == "JetBrains Mono", "editor seeds the initial text");
  service.ValueEditor().SetText("Fira Code");
  Expect(service.ValueEditText() == "Fira Code", "typing updates the edit text");
  service.CancelValueEdit();
  Expect(!service.EditingValue(), "CancelValueEdit ends the session");

  // Closing the overlay must not strand an active edit.
  service.BeginValueEdit("editor.font_family", "x");
  service.Close();
  Expect(!service.EditingValue(), "closing the overlay cancels any active edit");
}

void TestSettingsOverlayFontPickerFiltersAndSelects() {
  SettingsOverlayService service;
  service.OpenSettings();
  const std::vector<std::string> families = {"DejaVu Sans Mono", "Fira Code", "JetBrains Mono",
                                             "Noto Sans Mono"};
  service.BeginFontValueEdit("editor.font_family", families);
  Expect(service.EditingValue() && service.EditingFonts(),
         "BeginFontValueEdit activates a font picker edit");
  Expect(service.ValueEditText().empty(), "the font picker opens with an empty search box");

  // Empty query lists every family; a trailing row is the "Choose file…" entry.
  Expect(service.FilteredFontFamilies().size() == families.size(),
         "an empty query lists all families");
  Expect(service.PickerRowCount() == static_cast<int>(families.size()) + 1,
         "row count includes the Choose file… entry");
  Expect(service.PickerChooseFileIndex() == static_cast<int>(families.size()),
         "Choose file… is the last dropdown row");
  Expect(service.PickerHighlight() == -1,
         "highlight starts unset so a stray Enter never commits a font");

  // Filtering is case-insensitive substring.
  service.ValueEditor().SetText("mono");
  Expect(service.FilteredFontFamilies().size() == 3,
         "'mono' matches the three *Mono families");
  // The result is memoized by query text: reverting the query returns the full
  // list again (cache keyed on the query, not stale from the previous filter).
  service.ValueEditor().SetText("");
  Expect(service.FilteredFontFamilies().size() == families.size(),
         "reverting to an empty query re-lists every family");
  // Re-opening with a different family set invalidates the memoized filter.
  service.BeginFontValueEdit("editor.font_family", {"Only One Mono"});
  Expect(service.FilteredFontFamilies().size() == 1,
         "a new family set replaces the cached filtered list");

  // Highlight navigation clamps to [-1, ChooseFile].
  service.SetPickerHighlight(0);
  Expect(service.PickerHighlight() == 0, "highlight can land on the first family");
  service.MovePickerHighlight(100);
  Expect(service.PickerHighlight() == service.PickerChooseFileIndex(),
         "highlight clamps up to the Choose file… entry");
  service.MovePickerHighlight(-100);
  Expect(service.PickerHighlight() == -1, "highlight clamps down to the search-only state");

  service.CancelValueEdit();
  Expect(!service.EditingFonts() && !service.EditingValue(),
         "cancel clears the font picker session");
}

void TestSettingsOverlayFontPickerScrollWindow() {
  SettingsOverlayService service;
  service.OpenSettings();
  // More families than fit in one window so the dropdown must scroll.
  const int total = SettingsOverlayService::kPickerVisibleFamilies + 6;
  std::vector<std::string> families;
  for (int i = 0; i < total; ++i) {
    families.push_back("Family " + std::string(1, static_cast<char>('A' + i)));
  }
  service.BeginFontValueEdit("editor.font_family", families);
  Expect(service.PickerScroll() == 0, "picker opens scrolled to the top");

  // Scroll offset clamps to [0, total - visible].
  const int max_top = total - SettingsOverlayService::kPickerVisibleFamilies;
  service.SetPickerScroll(1000);
  Expect(service.PickerScroll() == max_top, "scroll clamps to the last full window");
  service.SetPickerScroll(-5);
  Expect(service.PickerScroll() == 0, "scroll clamps at the top");

  // Keyboard highlight keeps itself inside the visible window.
  service.SetPickerHighlight(0);
  Expect(service.PickerScroll() == 0, "highlighting the first family keeps scroll at top");
  service.SetPickerHighlight(total - 1);  // last family
  Expect(service.PickerHighlight() == total - 1, "highlight lands on the last family");
  Expect(service.PickerScroll() == max_top,
         "highlighting the last family scrolls it into view");
  Expect(service.PickerHighlight() >= service.PickerScroll() &&
             service.PickerHighlight() <
                 service.PickerScroll() + SettingsOverlayService::kPickerVisibleFamilies,
         "the highlighted family sits within the visible window");

  // Re-filtering (a keystroke resets the highlight) snaps the window back to the top.
  service.ValueEditor().SetText("Family");  // still matches all, but resets state
  service.ResetPickerHighlight();
  Expect(service.PickerScroll() == 0, "re-filtering resets the scroll to the top");
}

void TestSettingsOverlayGroupsEditorEssentialsToggles() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});

  const auto find_row = [&](std::string_view id) {
    return std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                        [&](const auto& row) { return row.id == id; });
  };

  const auto fold_it = find_row("editor.fold.enabled");
  Expect(fold_it != service.SettingsRows().end(),
         "settings overlay should include the code-folding toggle");
  Expect(fold_it->group == "Editor → Essentials → Block Structure",
         "fold toggle should land in the Block Structure subsection");

  const auto match_it = find_row("editor.brackets.match_highlight.enabled");
  Expect(match_it != service.SettingsRows().end(),
         "settings overlay should include the bracket-match toggle");
  Expect(match_it->group == "Editor → Essentials → Pair And Indent",
         "bracket-match toggle should land in the Pair And Indent subsection");

  const auto trim_it = find_row("editor.save.trim_trailing_whitespace");
  Expect(trim_it != service.SettingsRows().end(),
         "settings overlay should include the trim-trailing-whitespace toggle");
  Expect(trim_it->group == "Editor → Essentials → Shaping And Save",
         "trim toggle should land in the Shaping And Save subsection");

  const auto case_it = find_row("editor.search.case_sensitive");
  Expect(case_it != service.SettingsRows().end(),
         "settings overlay should include the case-sensitive search-seed toggle");
  Expect(case_it->group == "Editor → Essentials → Shaping And Save",
         "case-sensitive search-seed toggle should land in the Shaping And Save subsection");

  const auto layout_it = find_row("ui.layout_mode");
  Expect(layout_it != service.SettingsRows().end(),
         "settings overlay should still surface non-essentials toggles");
  Expect(layout_it->group == "Appearance → Layout",
         "the layout-mode toggle now lives in the Appearance section (no ungrouped catch-all)");
}

void TestSettingsCategoryLabelHelper() {
  using microide::workspace::SettingsCategoryLabel;
  Expect(SettingsCategoryLabel("") == "General",
         "an empty group should map to the General category");
  Expect(SettingsCategoryLabel("Editor → Essentials → Block Structure") == "Editor",
         "a grouped setting should map to its top-level segment");
  Expect(SettingsCategoryLabel("UI") == "UI",
         "a single-segment group should map to itself");
}

void TestSettingsOverlayDerivesCategories() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto& categories = service.Categories();
  Expect(!categories.empty(), "settings overlay should derive at least one category");
  // Every builtin is now grouped, so there is no ungrouped "General" catch-all; the
  // first spec (editor.tab_size) makes Editor the leading category.
  Expect(std::find(categories.begin(), categories.end(), "General") == categories.end(),
         "the General catch-all should be gone once every setting is grouped");
  Expect(categories.front() == "Editor",
         "the first grouped setting should make Editor the leading category");
  Expect(std::find(categories.begin(), categories.end(), "Editor") != categories.end(),
         "grouped Editor settings should produce an Editor category");
  Expect(std::find(categories.begin(), categories.end(), "Appearance") != categories.end(),
         "regrouped UI/appearance settings should produce an Appearance category");
  // No duplicate categories.
  for (std::size_t i = 0; i < categories.size(); ++i) {
    for (std::size_t j = i + 1; j < categories.size(); ++j) {
      Expect(categories[i] != categories[j], "category list should not contain duplicates");
    }
  }
}

void TestSettingsOverlayCategorySelectionResolvesRows() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto& categories = service.Categories();
  const auto editor_it = std::find(categories.begin(), categories.end(), "Editor");
  Expect(editor_it != categories.end(), "Editor category should exist for this fixture");
  const int editor_index = static_cast<int>(std::distance(categories.begin(), editor_it));
  service.SetSelectedCategory(editor_index);

  const std::size_t count = service.RowCountInSelectedCategory();
  Expect(count > 0, "Editor category should resolve at least one row");
  for (int i = 0; i < static_cast<int>(count); ++i) {
    const auto* row = service.RowAtVisibleIndex(editor_index, i);
    Expect(row != nullptr, "every in-range visible index should resolve a row");
    Expect(microide::workspace::SettingsCategoryLabel(row->group) == "Editor",
           "rows resolved for the Editor category should all belong to it");
  }
  Expect(service.RowAtVisibleIndex(editor_index, static_cast<int>(count)) == nullptr,
         "an out-of-range visible index should resolve to nullptr");
}

void TestSettingsOverlayFilterReclampsSelection() {
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  service.SetSelectedCategory(static_cast<int>(service.Categories().size()) - 1);

  // A query that matches nothing should empty the category list and reset selection.
  service.QueryEditor().SetText("zzz-no-such-setting-zzz");
  service.SyncQueryFromEditor();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  Expect(service.Categories().empty(), "a non-matching filter should leave no categories");
  Expect(service.SelectedCategory() == 0 && service.SelectedRow() == 0,
         "selection should reset when the filter empties the catalog");
  Expect(service.SelectedSettingRow() == nullptr,
         "no setting should be selected when the catalog is empty");

  // Clearing the filter restores rows and keeps selection in range.
  service.QueryEditor().SetText("");
  service.SyncQueryFromEditor();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  Expect(!service.Categories().empty(), "clearing the filter should restore categories");
  Expect(service.SelectedCategory() >= 0 &&
             service.SelectedCategory() < static_cast<int>(service.Categories().size()),
         "selected category should stay in range after clearing the filter");
}

void TestSettingsOverlayRowControlMetadata() {
  using microide::workspace::SettingsControlKind;
  SettingsOverlayService service;
  service.OpenSettings();
  service.RebuildSettingsRows(microide::workspace::AllSettingInfos(microide::plugin::PluginHost{}),
                              {}, {});
  const auto find_row = [&](std::string_view id) {
    return std::find_if(service.SettingsRows().begin(), service.SettingsRows().end(),
                        [&](const auto& row) { return row.id == id; });
  };

  const auto scale_it = find_row("ui.scale");
  Expect(scale_it != service.SettingsRows().end(), "ui.scale should be present");
  Expect(scale_it->type == SettingType::Float, "ui.scale should be a float setting");
  Expect(scale_it->control_kind == SettingsControlKind::Stepper,
         "float settings should use a stepper control");
  Expect(scale_it->value_display.find('.') == std::string::npos ||
             scale_it->value_display.back() != '0',
         "float value_display should be compacted (no trailing zeros)");

  const auto soft_tabs_it = find_row("editor.soft_tabs");
  Expect(soft_tabs_it != service.SettingsRows().end(), "editor.soft_tabs should be present");
  Expect(soft_tabs_it->control_kind == SettingsControlKind::Checkbox,
         "bool settings should use a checkbox control");
}

// TD-2026-07-17-028: pane cycling now derives its wrap modulus from the
// SettingsPane::Count sentinel instead of a hard-coded 3, so adding/removing a
// pane cannot desync keyboard navigation. Verify forward/backward wrap covers
// exactly the focusable panes.
void TestSettingsOverlayPaneCyclingWrapsAllPanes() {
  using microide::workspace::SettingsPane;
  SettingsOverlayService service;

  service.SetFocusedPane(SettingsPane::Filter);
  service.CycleFocusedPane(1);
  Expect(service.FocusedPane() == SettingsPane::Categories, "forward: Filter -> Categories");
  service.CycleFocusedPane(1);
  Expect(service.FocusedPane() == SettingsPane::Values, "forward: Categories -> Values");
  service.CycleFocusedPane(1);
  Expect(service.FocusedPane() == SettingsPane::Filter, "forward wraps: Values -> Filter");

  // Backward wrap from the first pane lands on the last.
  service.CycleFocusedPane(-1);
  Expect(service.FocusedPane() == SettingsPane::Values, "backward wraps: Filter -> Values");

  // A full-cycle delta returns to the start regardless of pane count.
  const int pane_count = static_cast<int>(SettingsPane::Count);
  service.SetFocusedPane(SettingsPane::Categories);
  service.CycleFocusedPane(pane_count);
  Expect(service.FocusedPane() == SettingsPane::Categories,
         "cycling by exactly the pane count is a no-op");
}

}  // namespace

void RegisterWorkspaceSettingsRegistryTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceSettingsOverlay/PaneCyclingWrapsAllPanes",
          TestSettingsOverlayPaneCyclingWrapsAllPanes);
  AddTest(tests, "WorkspaceSettingsRegistry/IncludesPolishKeys",
          TestSettingsCatalogIncludesPolishKeys);
  AddTest(tests, "WorkspaceSettingsRegistry/SaveDefaultsAndDedup",
          TestSettingsCatalogSaveDefaultsAndDedup);
  AddTest(tests, "WorkspaceSettingsRegistry/ProjectTabHideToggle",
          TestSettingsCatalogProjectTabHideToggle);
  AddTest(tests, "WorkspaceSettingsRegistry/LspToggles", TestSettingsCatalogLspToggles);
  AddTest(tests, "WorkspaceSettingsOverlay/DerivesLspCategory",
          TestSettingsOverlayDerivesLspCategory);
  AddTest(tests, "WorkspaceSettingsRegistry/DefaultsRoundTrip",
          TestSettingsCatalogDefaultsRoundTrip);
  AddTest(tests, "WorkspaceSettingsRegistry/EdgeValuesRoundTrip",
          TestSettingsCatalogEdgeValuesRoundTrip);
  AddTest(tests, "WorkspaceSettingsRegistry/IntValuesClampAtStoreTime",
          TestSettingsIntValuesClampAtStoreTime);
  AddTest(tests, "WorkspaceSettingsRegistry/RejectsInvalidEnums",
          TestSettingsCatalogRejectsInvalidEnums);
  AddTest(tests, "WorkspaceSettingsOverlay/FiltersAndPreservesScopes",
          TestSettingsOverlayFiltersAndPreservesScopes);
  AddTest(tests, "WorkspaceSettingsOverlay/RebuildReusesRowStorage",
          TestSettingsOverlayRebuildReusesRowStorage);
  AddTest(tests, "WorkspaceSettingsOverlay/ScopeSelectableRows",
          TestSettingsOverlayScopeSelectableRows);
  AddTest(tests, "WorkspaceSettingsOverlay/StringRowsAreTextEditable",
          TestSettingsOverlayStringRowsAreTextEditable);
  AddTest(tests, "WorkspaceSettingsOverlay/FontPickerFiltersAndSelects",
          TestSettingsOverlayFontPickerFiltersAndSelects);
  AddTest(tests, "WorkspaceSettingsOverlay/FontPickerScrollWindow",
          TestSettingsOverlayFontPickerScrollWindow);
  AddTest(tests, "WorkspaceSettingsOverlay/GroupsEditorEssentialsToggles",
          TestSettingsOverlayGroupsEditorEssentialsToggles);
  AddTest(tests, "WorkspaceSettingsOverlay/CategoryLabelHelper",
          TestSettingsCategoryLabelHelper);
  AddTest(tests, "WorkspaceSettingsOverlay/DerivesCategories",
          TestSettingsOverlayDerivesCategories);
  AddTest(tests, "WorkspaceSettingsOverlay/CategorySelectionResolvesRows",
          TestSettingsOverlayCategorySelectionResolvesRows);
  AddTest(tests, "WorkspaceSettingsOverlay/FilterReclampsSelection",
          TestSettingsOverlayFilterReclampsSelection);
  AddTest(tests, "WorkspaceSettingsOverlay/RowControlMetadata",
          TestSettingsOverlayRowControlMetadata);
}

}  // namespace microide::tests
