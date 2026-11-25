#include "TestSupport.h"

#include "workspace/persistence/SettingsStore.h"

#include <string>

namespace microide::tests {
namespace {

using microide::workspace::SettingsLayer;
using microide::workspace::SettingsStore;

std::string ResolveOr(const SettingsStore& store, std::string_view id, std::string_view fallback) {
  const std::string* value = store.Resolve(id);
  return value != nullptr ? *value : std::string(fallback);
}

void TestUserLayerWinsOverProject() {
  SettingsLayer user;
  SettingsLayer project;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.BindActiveProject(&project);

  store.SetProject("editor.theme", "dark");
  store.SetUser("editor.theme", "light");
  Expect(ResolveOr(store, "editor.theme", "?") == "dark",
         "project layer should win over the user-level default");

  store.SetProject("project.only", "p");
  Expect(ResolveOr(store, "project.only", "?") == "p", "project-only key should resolve");

  Expect(store.Resolve("missing.key") == nullptr, "unknown key resolves to nullptr");
}

void TestUserLevelValueStillResolvesForAProjectScopedSetting() {
  SettingsLayer user;
  SettingsLayer project;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.BindActiveProject(&project);

  store.SetUser("terminal.shell", "/usr/bin/fish");
  Expect(ResolveOr(store, "terminal.shell", "?") == "/usr/bin/fish",
         "a user-config terminal.shell keeps resolving after the setting became "
         "project-scoped");

  store.SetProject("terminal.shell", "ssh build-host");
  Expect(ResolveOr(store, "terminal.shell", "?") == "ssh build-host",
         "a project value still takes precedence over the user one");
}

void TestResetRestoresUnderlyingLayer() {
  SettingsLayer user;
  SettingsLayer project;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.BindActiveProject(&project);

  store.SetUser("debug.enabled", "false");
  store.SetProject("debug.enabled", "true");
  Expect(ResolveOr(store, "debug.enabled", "?") == "true", "project override active");

  store.ResetProject("debug.enabled");
  Expect(ResolveOr(store, "debug.enabled", "?") == "false",
         "resetting the project override surfaces the user-level default");

  store.ResetUser("debug.enabled");
  Expect(store.Resolve("debug.enabled") == nullptr, "resetting both layers unsets the key");
}

void TestRebindActiveProjectAfterMove() {
  SettingsLayer user;
  SettingsStore store;
  store.BindUserLayer(&user);

  SettingsLayer project_a;
  store.BindActiveProject(&project_a);
  store.SetProject("project.scoped", "a");
  Expect(ResolveOr(store, "project.scoped", "?") == "a", "first project value resolves");

  SettingsLayer project_b = std::move(project_a);
  project_a = SettingsLayer{};
  store.BindActiveProject(&project_b);
  Expect(ResolveOr(store, "project.scoped", "?") == "a",
         "re-bound project value resolves after the move");

  microide::workspace::settings_layer::Upsert(project_a, "project.scoped", "stale");
  Expect(ResolveOr(store, "project.scoped", "?") == "a",
         "writes to the detached vector do not leak into the store");

  store.SetUser("user.scoped", "u");
  SettingsLayer project_empty;
  store.BindActiveProject(&project_empty);
  Expect(store.Resolve("project.scoped") == nullptr, "switching projects drops old project keys");
  Expect(ResolveOr(store, "user.scoped", "?") == "u", "user layer survives a project switch");
}

void TestReindexAfterInPlaceReload() {
  SettingsLayer user;
  SettingsLayer project;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.BindActiveProject(&project);

  user.clear();
  user.emplace_back("ui.scale", "1.5");
  user.emplace_back("debug.enabled", "true");
  store.Reindex();
  Expect(ResolveOr(store, "ui.scale", "?") == "1.5", "reindex picks up in-place reload");
  Expect(ResolveOr(store, "debug.enabled", "?") == "true", "reindex picks up second key");
}

void TestRejectsInvalidSettingIds() {
  Expect(SettingsStore::IsValidSettingId("editor.tab_size"), "a normal dotted id is valid");
  Expect(SettingsStore::IsValidSettingId("plugin_id.setting-1"), "dashes/underscores are valid");
  Expect(!SettingsStore::IsValidSettingId(""), "an empty id is invalid");
  Expect(!SettingsStore::IsValidSettingId("has space"), "an id with a space is invalid");
  Expect(!SettingsStore::IsValidSettingId("has\nnewline"), "an id with a newline is invalid");
  Expect(!SettingsStore::IsValidSettingId(std::string("tab\t")), "an id with a tab is invalid");

  SettingsLayer user;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.SetUser("bad id", "x");
  Expect(store.Resolve("bad id") == nullptr, "an invalid id must not enter the store");
  Expect(user.empty(), "an invalid id must not reach the persisted layer");
  store.SetUser("good.id", "y");
  Expect(ResolveOr(store, "good.id", "?") == "y", "a valid id still applies");
}

void TestRevisionOnlyBumpsOnEffectiveChange() {
  SettingsLayer user;
  SettingsLayer project;
  SettingsStore store;
  store.BindUserLayer(&user);
  store.BindActiveProject(&project);
  store.SetUser("a", "1");
  const std::uint64_t r1 = store.Revision();
  Expect(r1 > 0, "the first effective setting bumps the revision");

  store.BindActiveProject(&project);
  Expect(store.Revision() == r1, "rebinding to an identical resolution does not bump the revision");

  store.SetUser("a", "1");
  Expect(store.Revision() == r1, "a no-op set does not bump the revision");

  store.SetUser("a", "2");
  Expect(store.Revision() > r1, "an effective value change bumps the revision");
}

}

void RegisterSettingsStoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SettingsStore/RevisionOnlyBumpsOnEffectiveChange",
          TestRevisionOnlyBumpsOnEffectiveChange);
  AddTest(tests, "SettingsStore/RejectsInvalidSettingIds", TestRejectsInvalidSettingIds);
  AddTest(tests, "SettingsStore/UserLayerWinsOverProject", TestUserLayerWinsOverProject);
  AddTest(tests, "SettingsStore/UserLevelValueStillResolvesForAProjectScopedSetting",
          TestUserLevelValueStillResolvesForAProjectScopedSetting);
  AddTest(tests, "SettingsStore/ResetRestoresUnderlyingLayer", TestResetRestoresUnderlyingLayer);
  AddTest(tests, "SettingsStore/RebindActiveProjectAfterMove", TestRebindActiveProjectAfterMove);
  AddTest(tests, "SettingsStore/ReindexAfterInPlaceReload", TestReindexAfterInPlaceReload);
}

}
