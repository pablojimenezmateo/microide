#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/WorkspaceFormatterRegistry.h"
#include "workspace/WorkspaceSaveParticipants.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceTaskRegistry.h"
#include "workspace/WorkspaceToolRegistry.h"

#include <cassert>

namespace microide::tests {

using namespace util;
using namespace workspace;

static void TestJsonParseBasic() {
  const auto val = ParseJson(R"({"key": "value", "num": 42})");
  assert(val.has_value());
  assert(val->IsObject());
  assert((*val)["key"].AsString() == "value");
  assert((*val)["num"].AsInt() == 42);
}

static void TestJsonParseArray() {
  const auto val = ParseJson(R"([1, 2, 3])");
  assert(val.has_value());
  assert(val->IsArray());
  assert((*val)[0].AsInt() == 1);
  assert((*val)[2].AsInt() == 3);
}

static void TestJsonParseNestedString() {
  const auto val = ParseJson(R"({"nested": {"inner": "value"}})");
  assert(val.has_value());
  const auto& nested = (*val)["nested"];
  assert(nested["inner"].AsString() == "value");
}

static void TestJsonSerializeBasic() {
  JsonObject obj;
  obj["name"] = JsonValue("Alice");
  obj["age"] = JsonValue(static_cast<std::int64_t>(30));
  const auto json = SerializeJson(JsonValue(obj));
  assert(json.find("\"name\":\"Alice\"") != std::string::npos);
  assert(json.find("\"age\":30") != std::string::npos);
}

static void TestJsonParseInvalid() {
  const auto val = ParseJson(R"(invalid json)");
  assert(!val.has_value());
}

static void TestJsonParseEmpty() {
  const auto val = ParseJson("");
  assert(!val.has_value());
}

static void TestJsonRoundTrip() {
  const std::string original = R"({"a":1,"b":"test","c":[1,2,3]})";
  const auto val = ParseJson(original);
  assert(val.has_value());
  const auto serialized = SerializeJson(*val);
  const auto reparsed = ParseJson(serialized);
  assert(reparsed.has_value());
  assert((*reparsed)["a"].AsInt() == 1);
  assert((*reparsed)["b"].AsString() == "test");
  assert((*reparsed)["c"][0].AsInt() == 1);
}

static void TestFormatterRegistry() {
  FormatterRegistry reg;
  FormatterSpec spec{
      .id = "clang-format",
      .language_id = "cpp",
      .label = "Clang Format",
      .command = {"clang-format"},
      .plugin_id = "cpp-tools",
  };
  reg.Register(spec);
  assert(reg.Specs().size() == 1);
  const auto* found = reg.FindFormatter("cpp");
  assert(found != nullptr);
  assert(found->label == "Clang Format");
}

static void TestFormatterRegistryNotFound() {
  FormatterRegistry reg;
  const auto* found = reg.FindFormatter("rust");
  assert(found == nullptr);
}

static void TestSaveParticipantRegistry() {
  SaveParticipantRegistry reg;
  SaveParticipantSpec spec{
      .id = "trim-whitespace",
      .plugin_id = "editor-utils",
  };
  reg.Register(spec);
  assert(reg.Specs().size() == 1);
}

static void TestCompletionRegistry() {
  CompletionRegistry reg;
  CompletionProviderSpec spec{
      .id = "lsp-completion",
      .plugin_id = "lsp-bridge",
      .language_id = "rust",
      .trigger_characters = ".",
  };
  reg.Register(spec);
  assert(reg.Specs().size() == 1);
  const auto* found = reg.FindProvider("rust");
  assert(found != nullptr);
  assert(found->trigger_characters == ".");
}

static void TestCodeActionRegistry() {
  CodeActionRegistry reg;
  CodeActionProviderSpec spec{
      .id = "lsp-actions",
      .plugin_id = "lsp-bridge",
      .language_id = "rust",
  };
  reg.Register(spec);
  const auto* found = reg.FindProvider("rust");
  assert(found != nullptr);
}

static void TestTaskRegistry() {
  TaskRegistry reg;
  TaskSpec spec{
      .id = "build",
      .plugin_id = "cargo",
      .label = "Build Project",
      .group = "build",
      .command = {"cargo", "build"},
      .cwd = ".",
      .run_in_shell = false,
  };
  reg.Register(spec);
  const auto* found = reg.FindTask("build");
  assert(found != nullptr);
  assert(found->label == "Build Project");
  assert(found->command.size() == 2);
}

static void TestTaskRegistryNotFound() {
  TaskRegistry reg;
  const auto* found = reg.FindTask("nonexistent");
  assert(found == nullptr);
}

static void TestToolRegistry() {
  ToolRegistry reg;
  ToolSpec spec{
      .id = "rust-analyzer",
      .plugin_id = "rust-tools",
      .label = "Rust Analyzer",
      .platform = "linux",
      .download_url = "https://example.com/rust-analyzer",
      .sha256 = "abcdef",
      .install_dir = ".cache/tools",
  };
  reg.Register(spec);
  const auto* found = reg.FindTool("rust-analyzer", "linux");
  assert(found != nullptr);
  assert(found->label == "Rust Analyzer");
}

static void TestToolRegistryMultiplePlatforms() {
  ToolRegistry reg;
  ToolSpec linux_spec{
      .id = "tool",
      .plugin_id = "plugin",
      .label = "Tool",
      .platform = "linux",
      .download_url = "https://example.com/linux",
      .sha256 = "abc",
      .install_dir = "",
  };
  ToolSpec macos_spec{
      .id = "tool",
      .plugin_id = "plugin",
      .label = "Tool",
      .platform = "macos",
      .download_url = "https://example.com/macos",
      .sha256 = "def",
      .install_dir = "",
  };
  reg.Register(linux_spec);
  reg.Register(macos_spec);
  assert(reg.Specs().size() == 2);
  assert(reg.FindTool("tool", "linux") != nullptr);
  assert(reg.FindTool("tool", "macos") != nullptr);
  assert(reg.FindTool("tool", "windows") == nullptr);
}

void RegisterPhase3Tests(std::vector<TestCase>& tests) {
  tests.emplace_back("Phase3.JsonParseBasic", &TestJsonParseBasic);
  tests.emplace_back("Phase3.JsonParseArray", &TestJsonParseArray);
  tests.emplace_back("Phase3.JsonParseNestedString", &TestJsonParseNestedString);
  tests.emplace_back("Phase3.JsonSerializeBasic", &TestJsonSerializeBasic);
  tests.emplace_back("Phase3.JsonParseInvalid", &TestJsonParseInvalid);
  tests.emplace_back("Phase3.JsonParseEmpty", &TestJsonParseEmpty);
  tests.emplace_back("Phase3.JsonRoundTrip", &TestJsonRoundTrip);
  tests.emplace_back("Phase3.FormatterRegistry", &TestFormatterRegistry);
  tests.emplace_back("Phase3.FormatterRegistryNotFound", &TestFormatterRegistryNotFound);
  tests.emplace_back("Phase3.SaveParticipantRegistry", &TestSaveParticipantRegistry);
  tests.emplace_back("Phase3.CompletionRegistry", &TestCompletionRegistry);
  tests.emplace_back("Phase3.CodeActionRegistry", &TestCodeActionRegistry);
  tests.emplace_back("Phase3.TaskRegistry", &TestTaskRegistry);
  tests.emplace_back("Phase3.TaskRegistryNotFound", &TestTaskRegistryNotFound);
  tests.emplace_back("Phase3.ToolRegistry", &TestToolRegistry);
  tests.emplace_back("Phase3.ToolRegistryMultiplePlatforms", &TestToolRegistryMultiplePlatforms);
}

}  // namespace microide::tests
