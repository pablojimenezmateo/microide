#include "TestSupport.h"

#include "persistence/PersistedRecordDump.h"
#include "persistence/PersistedRecordWriter.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

void TestPersistedRecordDumpPrintsHeaderRecordsAndDecodedSummary() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path path = temp_dir.path() / "state" / "user.config";

  microide::workspace::PersistedUserConfigState user{
      .ui_scale = 1.75f,
      .settings = {{"theme", "sunrise"}},
      .disabled_keybinding_ids = {"terminal.focus"},
      .disabled_plugin_ids = {},
  };

  std::vector<std::byte> body;
  Expect(microide::workspace::EncodeUserConfigRecord(user, &body),
         "encode user config should succeed");
  Expect(microide::persistence::PersistedRecordWriter::WriteFile(path, body, 1u),
         "write persisted user config should succeed");

  std::string output;
  std::string error;
  Expect(microide::persistence::DumpPersistedRecordFile(path, &output, &error),
         "dump-state should succeed for a valid persisted file");
  Expect(error.empty(), "dump-state error should be empty on success");
  Expect(output.find("capability_flags: 1") != std::string::npos,
         "dump-state should print capability flags");
  Expect(output.find("record[0].tag:") != std::string::npos,
         "dump-state should list decoded record tags");
  Expect(output.find("decoded.user_config.ui_scale: 1.75") != std::string::npos,
         "dump-state should print decoded summary values");
  Expect(output.find("decoded.user_config.settings_count: 1") != std::string::npos,
         "dump-state should print decoded settings counts");
}

void TestPersistedRecordDumpReportsNotFoundError() {
  TemporaryDirectory temp_dir;
  std::string output;
  std::string error;
  const bool ok = microide::persistence::DumpPersistedRecordFile(
      temp_dir.path() / "missing.state", &output, &error);
  Expect(!ok, "dump-state should fail on missing files");
  Expect(error == "not_found", "dump-state should return a stable not_found error");
}

}  // namespace

void RegisterPersistedRecordDumpTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PersistedRecordDump/PrintsHeaderRecordsAndDecodedSummary",
          TestPersistedRecordDumpPrintsHeaderRecordsAndDecodedSummary);
  AddTest(tests, "PersistedRecordDump/ReportsNotFoundError",
          TestPersistedRecordDumpReportsNotFoundError);
}

}  // namespace microide::tests
