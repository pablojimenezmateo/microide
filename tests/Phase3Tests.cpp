#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/WorkspaceFormatterRegistry.h"
#include "workspace/WorkspaceSaveParticipants.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceToolRegistry.h"

namespace microide::tests {

using namespace util;
using namespace workspace;

static void TestJsonParseBasic() {
  const auto val = ParseJson(R"({"key": "value", "num": 42})");
  Expect(val.has_value(), "object should parse");
  Expect(val->IsObject(), "parsed value should be an object");
  Expect((*val)["key"].AsString() == "value", "key field should round-trip as string");
  Expect((*val)["num"].AsInt() == 42, "num field should round-trip as int");
}

static void TestJsonParseArray() {
  const auto val = ParseJson(R"([1, 2, 3])");
  Expect(val.has_value(), "array should parse");
  Expect(val->IsArray(), "parsed value should be an array");
  Expect((*val)[0].AsInt() == 1, "array element 0 should be 1");
  Expect((*val)[2].AsInt() == 3, "array element 2 should be 3");
}

static void TestJsonParseNestedString() {
  const auto val = ParseJson(R"({"nested": {"inner": "value"}})");
  Expect(val.has_value(), "nested object should parse");
  Expect((*val)["nested"]["inner"].AsString() == "value",
         "nested string should be reachable through chained access");
}

static void TestJsonSerializeBasic() {
  JsonObject obj;
  obj["name"] = JsonValue("Alice");
  obj["age"] = JsonValue(static_cast<std::int64_t>(30));
  const auto json = SerializeJson(JsonValue(obj));
  Expect(json.find("\"name\":\"Alice\"") != std::string::npos,
         "serialized JSON should contain the name field");
  Expect(json.find("\"age\":30") != std::string::npos,
         "serialized JSON should contain the integer age field");
}

static void TestJsonParseInvalid() {
  const auto val = ParseJson(R"(invalid json)");
  Expect(!val.has_value(), "invalid JSON should not parse");
}

static void TestJsonParseEmpty() {
  const auto val = ParseJson("");
  Expect(!val.has_value(), "empty input should not parse");
}

static void TestJsonRoundTrip() {
  const std::string original = R"({"a":1,"b":"test","c":[1,2,3]})";
  const auto val = ParseJson(original);
  Expect(val.has_value(), "round-trip source should parse");
  const auto serialized = SerializeJson(*val);
  const auto reparsed = ParseJson(serialized);
  Expect(reparsed.has_value(), "round-trip serialized form should parse again");
  Expect((*reparsed)["a"].AsInt() == 1, "round-trip integer field a should survive");
  Expect((*reparsed)["b"].AsString() == "test", "round-trip string field b should survive");
  Expect((*reparsed)["c"][0].AsInt() == 1, "round-trip nested array element should survive");
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
  Expect(reg.Specs().size() == 1, "formatter registry should retain registered spec");
  Expect(reg.FindFormatter("cpp") != nullptr,
         "formatter registry should resolve cpp formatter");
  Expect(reg.FindFormatter("cpp")->label == "Clang Format",
         "resolved formatter label should match registration");
}

static void TestFormatterRegistryNotFound() {
  FormatterRegistry reg;
  Expect(reg.FindFormatter("rust") == nullptr,
         "unregistered language should resolve to nullptr");
}

static void TestSaveParticipantRegistry() {
  SaveParticipantRegistry reg;
  SaveParticipantSpec spec{
      .id = "trim-whitespace",
      .plugin_id = "editor-utils",
  };
  reg.Register(spec);
  Expect(reg.Specs().size() == 1, "save-participant registry should retain registered spec");
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
  Expect(reg.Specs().size() == 1, "completion registry should retain registered spec");
  Expect(reg.FindProvider("rust") != nullptr,
         "completion registry should resolve rust provider");
  Expect(reg.FindProvider("rust")->trigger_characters == ".",
         "resolved completion provider should preserve trigger characters");
}

static void TestCodeActionRegistry() {
  CodeActionRegistry reg;
  CodeActionProviderSpec spec{
      .id = "lsp-actions",
      .plugin_id = "lsp-bridge",
      .language_id = "rust",
  };
  reg.Register(spec);
  Expect(reg.FindProvider("rust") != nullptr,
         "code-action registry should resolve rust provider");
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
  Expect(reg.FindTool("rust-analyzer", "linux") != nullptr,
         "tool registry should resolve linux rust-analyzer");
  Expect(reg.FindTool("rust-analyzer", "linux")->label == "Rust Analyzer",
         "resolved tool label should match registration");
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
  Expect(reg.Specs().size() == 2, "tool registry should retain both platform specs");
  Expect(reg.FindTool("tool", "linux") != nullptr,
         "tool registry should resolve linux variant");
  Expect(reg.FindTool("tool", "macos") != nullptr,
         "tool registry should resolve macos variant");
  Expect(reg.FindTool("tool", "windows") == nullptr,
         "tool registry should not resolve unregistered windows variant");
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
  tests.emplace_back("Phase3.ToolRegistry", &TestToolRegistry);
  tests.emplace_back("Phase3.ToolRegistryMultiplePlatforms", &TestToolRegistryMultiplePlatforms);
}

}  // namespace microide::tests
